// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use std::collections::HashSet;
use std::ptr;

use eglantine_ampere_rs::ffi::EglAmpereInstruction;
use eglantine_ampere_rs::Instruction;
use eglantine_ampere_spirv::coop_matrix::{
    CAPABILITY_COOPERATIVE_MATRIX_KHR, OP_COOPERATIVE_MATRIX_MUL_ADD_KHR,
    OP_TYPE_COOPERATIVE_MATRIX_KHR,
};
use eglantine_ampere_spirv::ffi::{
    egl_ampere_spirv_compile, egl_ampere_spirv_compile_batch, egl_ampere_spirv_free,
    EglSpirvBindings, EglSpirvCbufEntry, EglSpirvCompileInput, EglSpirvCompileOutput,
    EglSpirvCoopLayout, EglSpirvProfile,
};
use eglantine_ampere_spirv::{
    compile_batch, compile_shader, Bindings, CoopConfig, LowerInput, OwnedCompileInput, Profile,
    SnapshotEnv, Stage, SPIRV_1_6_VERSION, SPIRV_MAGIC,
};

fn sm86(w0: u32, w1: u32, w2: u32, w3: u32) -> Instruction {
    Instruction {
        words: [w0, w1, w2, w3],
    }
}

/// Encodes SM86 instruction with predicate always-true (`7 << 12`), opcode, and dst register.
fn enc_op_dst(opcode: u32, dst: u32, w1: u32, w2: u32) -> Instruction {
    sm86((opcode & 0xfff) | (7 << 12) | ((dst & 0xff) << 16), w1, w2, 0)
}

fn s2r(dst: u32, special: u32) -> Instruction {
    enc_op_dst(0x919, dst, 0, special << 8)
}

fn mov_imm(dst: u32, imm: u32) -> Instruction {
    enc_op_dst(0x802, dst, imm, 0)
}

fn mov_cbuf(dst: u32, bank: u32, byte_off: u32) -> Instruction {
    let w1 = (bank << 22) | (byte_off << 6);
    enc_op_dst(0xa02, dst, w1, 0)
}

fn uldc_u64(dst_ur: u32, bank: u32, byte_off: u32) -> Instruction {
    let w1 = (bank << 22) | (byte_off << 6);
    // width=5 (U64) at bits 73..76 -> (5 << 9) in w2
    enc_op_dst(0xab9, dst_ur, w1, 5 << 9)
}

fn shf_l_imm(dst: u32, a: u32, shift: u32) -> Instruction {
    // SHF.L form=4 imm: opcode=0x819, bits(73,2)=3 (U32), bits(64,8)=255 (Rc=RZ)
    let w0 = 0x819 | (7 << 12) | (dst << 16) | (a << 24);
    let w2 = 255 | (3 << 9);
    sm86(w0, shift, w2, 0)
}

fn iadd3_reg(dst: u32, a: u32, b: u32, c: u32) -> Instruction {
    // opcode=0x210, form=1 (all regs), carry outs P7/P7 (bits 81..86 = 0x3f -> 0x07e0_0000 in w2)
    let w0 = 0x210 | (7 << 12) | (dst << 16) | (a << 24);
    let w1 = b;
    let w2 = c | (0x3f << 17) | (15 << 23);
    sm86(w0, w1, w2, 0)
}

fn isetp_lt_u32(dst_p: u32, a: u32, b: u32) -> Instruction {
    // opcode=0x20c, cmp=Lt(1) bits(76,3), comb=And(0), accum=PT(7) bits(87,4)
    let w0 = 0x20c | (7 << 12) | (7 << 16) | (a << 24);
    let w1 = b;
    let w2 = (1 << 12) | (dst_p << 17) | (7 << 20) | (7 << 23);
    sm86(w0, w1, w2, 0)
}

fn fadd_reg(dst: u32, a: u32, b: u32) -> Instruction {
    let w0 = 0x221 | (7 << 12) | (dst << 16) | (a << 24);
    sm86(w0, b, 0, 0)
}

fn ldg_ur(dst: u32, base_reg: u32, ur: u32, imm_off: i32) -> Instruction {
    // 0x981: size=4 (B32 -> 4<<9=0x800), descriptor bit off -> 0x081e_8900
    let w0 = 0x981 | (7 << 12) | (dst << 16) | (base_reg << 24);
    let w1 = ur | (((imm_off as u32) & 0xff_ffff) << 8);
    let w2 = 0x081e_8100 | (4 << 9);
    sm86(w0, w1, w2, 0)
}

fn stg_ur(src_reg: u32, base_reg: u32, ur: u32, imm_off: i32) -> Instruction {
    // 0x986: ur at bits(64,6), extended bit72=1, size=4 at bits(73,3) -> 0x0810_a100 | 0x800 | 0x100
    let w0 = 0x986 | (7 << 12) | (base_reg << 24);
    let w1 = src_reg | (((imm_off as u32) & 0xff_ffff) << 8);
    let w2 = 0x0810_a100 | ur | (4 << 9);
    sm86(w0, w1, w2, 0)
}

fn sts_b32(src_reg: u32, base_reg: u32, imm_off: i32) -> Instruction {
    let w0 = 0x388 | (7 << 12) | (base_reg << 24);
    let w1 = src_reg | (((imm_off as u32) & 0xff_ffff) << 8);
    let w2 = 4 << 9;
    sm86(w0, w1, w2, 0)
}

fn lds_b32(dst: u32, base_reg: u32, imm_off: i32) -> Instruction {
    let w0 = 0x984 | (7 << 12) | (dst << 16) | (base_reg << 24);
    let w1 = ((imm_off as u32) & 0xff_ffff) << 8;
    let w2 = 4 << 9;
    sm86(w0, w1, w2, 0)
}

fn bar_sync() -> Instruction {
    sm86(0xb1d | (7 << 12), 0, 0, 0)
}

fn hmma_16816(dst: u32, ra: u32, rb: u32, rc: u32) -> Instruction {
    let w0 = 0x23c | (7 << 12) | (dst << 16) | (ra << 24);
    let w1 = rb;
    let w2 = rc | 0x800;
    sm86(w0, w1, w2, 0)
}

fn bra_rel(pred: u32, neg: bool, rel_insns: i32) -> Instruction {
    let g = pred | if neg { 8 } else { 0 };
    let rel_bytes = (rel_insns as i64) * 16;
    let raw = rel_bytes as u64;
    let w0 = 0x947 | (g << 12);
    let w1 = (raw as u32) & !3u32;
    let w2 = ((raw >> 32) as u32 & 0x3_ffff) | (7 << 23);
    sm86(w0, w1, w2, 0)
}

fn exit_insn(pred: u32, neg: bool) -> Instruction {
    let g = pred | if neg { 8 } else { 0 };
    sm86(0x94d | (g << 12), 0, 0, 0)
}

struct ParsedOp<'a> {
    opcode: u16,
    operands: &'a [u32],
}

fn parse_spirv_module(words: &[u32]) -> Vec<ParsedOp<'_>> {
    assert!(words.len() >= 5, "SPIR-V binary shorter than 5-word header");
    assert_eq!(words[0], SPIRV_MAGIC);
    assert_eq!(words[1], SPIRV_1_6_VERSION);
    assert!(words[3] > 1, "SPIR-V ID bound must be > 1");
    assert_eq!(words[4], 0, "SPIR-V schema must be 0");

    let bound = words[3];
    let mut ops = Vec::new();
    let mut idx = 5usize;
    let mut seen_defs = HashSet::new();

    while idx < words.len() {
        let header = words[idx];
        let count = (header >> 16) as usize;
        let opcode = (header & 0xffff) as u16;
        assert!(count >= 1, "SPIR-V instruction with word_count 0 at word {idx}");
        assert!(
            idx + count <= words.len(),
            "truncated SPIR-V instruction at word {idx}"
        );
        let operands = &words[idx + 1..idx + count];

        // Track result IDs for instructions whose operand layout puts result_id after result_type
        // or directly at operand 0.
        match opcode {
            11 | 19..=33 | 41..=44 | 54 | 59 | 61 | 65 | 80..=83 | 109..=205 | 229..=245
            | 333..=348 | OP_COOPERATIVE_MATRIX_MUL_ADD_KHR => {
                // OpExtInstImport / OpType* / OpVariable / OpFunction / OpLabel define result_id at operands[0] (except OpFunction/OpVariable/OpConstant* have [ty, id]).
                let res = if matches!(opcode, 11 | 19..=33) {
                    operands[0]
                } else {
                    operands[1]
                };
                assert!(res > 0 && res < bound, "result id {res} >= bound {bound}");
                assert!(seen_defs.insert(res), "duplicate SPIR-V result id {res} (op {opcode})");
            }
            OP_TYPE_COOPERATIVE_MATRIX_KHR => {
                let res = operands[0];
                assert!(res > 0 && res < bound);
                assert!(seen_defs.insert(res), "duplicate coop matrix type id {res}");
            }
            248 => {
                let lbl = operands[0];
                assert!(lbl > 0 && lbl < bound);
                assert!(seen_defs.insert(lbl), "duplicate label id {lbl}");
            }
            _ => {}
        }

        ops.push(ParsedOp { opcode, operands });
        idx += count;
    }
    assert_eq!(idx, words.len());
    ops
}

fn assert_valid_spirv_1_6(words: &[u32], expected_wg: [u32; 3]) -> Vec<ParsedOp<'_>> {
    let ops = parse_spirv_module(words);

    // Check canonical SPIR-V 1.6 section order.
    fn section_rank(op: u16) -> u8 {
        match op {
            17 => 0,                  // OpCapability
            10 => 1,                  // OpExtension
            11 => 2,                  // OpExtInstImport
            14 => 3,                  // OpMemoryModel
            15 => 4,                  // OpEntryPoint
            16 => 5,                  // OpExecutionMode
            5 => 6,                   // OpName
            71 | 72 => 7,             // OpDecorate / OpMemberDecorate
            19..=33 | 41..=44 | 59 | OP_TYPE_COOPERATIVE_MATRIX_KHR => 8,
            _ => 9,
        }
    }

    let mut cur_rank = 0u8;
    let mut in_function = false;
    let mut has_memory_model = false;
    let mut has_entry_point = false;
    let mut has_local_size = false;
    let mut has_vulkan_cap = false;
    let mut entry_iface = HashSet::new();
    let mut global_vars = Vec::new();
    let mut const_u32_map = std::collections::HashMap::new();

    for op in &ops {
        if op.opcode == 54 {
            in_function = true;
        }
        if !in_function {
            let r = section_rank(op.opcode);
            assert!(
                r >= cur_rank,
                "out-of-order SPIR-V opcode {} (rank {} < {})",
                op.opcode,
                r,
                cur_rank
            );
            cur_rank = r;
        }
        match op.opcode {
            17 if op.operands == [5345] => has_vulkan_cap = true,
            43 if op.operands.len() == 3 => {
                // OpConstant u32 [ty, id, val]
                const_u32_map.insert(op.operands[1], op.operands[2]);
            }
            59 if !in_function && op.operands[2] != 7 => {
                // Global OpVariable outside Function storage class
                global_vars.push(op.operands[1]);
            }
            10 => {
                // Decode extension string and assert Vulkan memory model extension is NOT emitted in 1.6.
                let bytes: Vec<u8> = op
                    .operands
                    .iter()
                    .flat_map(|w| w.to_le_bytes())
                    .take_while(|&b| b != 0)
                    .collect();
                let ext_name = std::str::from_utf8(&bytes).expect("valid UTF-8 OpExtension");
                assert_ne!(ext_name, "SPV_KHR_vulkan_memory_model");
            }
            14 => {
                assert_eq!(op.operands, [0, 3], "expected AddressingModelLogical + MemoryModelVulkan");
                has_memory_model = true;
            }
            15 => {
                assert_eq!(op.operands[0], 5, "expected ExecutionModel GLCompute");
                // Entry point operands: [exec_model, fn_id, name_words..., interface_vars...]
                let mut w_idx = 2usize;
                while w_idx < op.operands.len() {
                    let bytes = op.operands[w_idx].to_le_bytes();
                    w_idx += 1;
                    if bytes.contains(&0) {
                        break;
                    }
                }
                for &iface in &op.operands[w_idx..] {
                    entry_iface.insert(iface);
                }
                has_entry_point = true;
            }
            16 if op.operands.len() >= 5 && op.operands[1] == 17 => {
                assert_eq!(&op.operands[2..5], &expected_wg);
                has_local_size = true;
            }
            _ => {}
        }
    }

    assert!(has_vulkan_cap, "missing VulkanMemoryModel capability");
    assert!(has_memory_model, "missing OpMemoryModel");
    assert!(has_entry_point, "missing OpEntryPoint");
    assert!(has_local_size, "missing OpExecutionMode LocalSize");
    for gvar in global_vars {
        assert!(
            entry_iface.contains(&gvar),
            "SPIR-V >=1.4 requires global variable {gvar} in OpEntryPoint interface"
        );
    }
    let _ = const_u32_map;
    ops
}

#[test]
fn compute_kernel_ssbo_shared_cbuf_exit() {
    // Reads TID.X, computes offset, loads cbuf + SSBO[0], writes shared, bar.sync, stores SSBO[1].
    let code = vec![
        uldc_u64(4, 0, 0x160),           // UR4 = c[0][0x160] (SSBO in)
        uldc_u64(6, 0, 0x168),           // UR6 = c[0][0x168] (SSBO out)
        s2r(2, 33),                      // R2 = TID.X
        mov_cbuf(3, 24, 0x20),           // R3 = c[24->17][0x20]
        shf_l_imm(4, 2, 2),              // R4 = R2 << 2
        ldg_ur(5, 4, 4, 0),              // R5 = SSBO_in[R4]
        sts_b32(5, 4, 0),                // shared[R4] = R5
        bar_sync(),                      // BAR.SYNC
        lds_b32(7, 4, 0),                // R7 = shared[R4]
        fadd_reg(8, 7, 3),               // R8 = R7 + R3
        stg_ur(8, 4, 6, 0),              // SSBO_out[R4] = R8
        exit_insn(7, false),             // EXIT
    ];

    let mut snap = SnapshotEnv::default();
    snap.storage_ptrs.insert(SnapshotEnv::make_key(0, 0x160), true);
    snap.storage_ptrs.insert(SnapshotEnv::make_key(0, 0x168), true);

    let input = LowerInput {
        stage: Stage::Compute,
        program_key: 0x1122_3344_5566_7788,
        code: &code,
        workgroup_size: [64, 1, 1],
        local_memory_size: 0,
        shared_memory_size: 256,
        profile: Profile {
            unified_descriptor_binding: true,
            ..Profile::spirv_1_6_default()
        },
        bindings: Bindings::default(),
        snapshot: snap,
    };

    let out = compile_shader(&input);
    assert!(out.fallback.is_none(), "unexpected fallback: {:?}", out.fallback);
    let ops = assert_valid_spirv_1_6(&out.spirv_words, [64, 1, 1]);
    assert!(ops.iter().any(|o| o.opcode == 224), "expected OpControlBarrier");
    assert!(out.info.uses_local_invocation_id);
    assert!(out.info.uses_global_memory);
    assert!(out.info.stores_global_memory);
    // Bank 0 and blit bank 24 remapped to 17 must both be reflected.
    assert_ne!(out.info.constant_buffer_mask & (1 << 0), 0);
    assert_ne!(out.info.constant_buffer_mask & (1 << 17), 0);
    assert_eq!(out.info.storage_buffer_descriptors.len(), 2);
    assert!(!out.info.storage_buffer_descriptors[0].is_written);
    assert!(out.info.storage_buffer_descriptors[1].is_written);
    assert_eq!(out.updated_bindings.unified, 4);
}

#[test]
fn cooperative_matrix_parameterized_no_patching_hack() {
    // SM86 kernel loading A/B/C registers, running HMMA.16816, storing result D0 to SSBO.
    let code = vec![
        uldc_u64(4, 0, 0x160),
        s2r(2, 33),
        shf_l_imm(2, 2, 2),
        mov_imm(8, 0x3c00_3c00),  // A0 (1.0h, 1.0h)
        mov_imm(9, 0x3c00_3c00),  // A1
        mov_imm(10, 0x3c00_3c00), // A2
        mov_imm(11, 0x3c00_3c00), // A3
        mov_imm(12, 0x3c00_3c00), // B0
        mov_imm(13, 0x3c00_3c00), // B1
        mov_imm(14, 0),           // C0
        mov_imm(15, 0),           // C1
        hmma_16816(16, 8, 12, 14),
        stg_ur(16, 2, 4, 0),
        exit_insn(7, false),
    ];

    let mut profile_nv = Profile::spirv_1_6_default();
    profile_nv.coop_layout = CoopConfig {
        supported: true,
        use_fp16_accum: true,
        tile_m: 16,
        tile_n: 8,
        tile_k: 16,
        subgroup_size: 32,
        a_word_map: [0, 2, 1, 3],
        b_word_map: [0, 1],
        c_word_map: [0, 1],
        d_word_map: [0, 1],
    };

    let out_nv = compile_shader(&LowerInput {
        stage: Stage::Compute,
        program_key: 0xdead_beef,
        code: &code,
        workgroup_size: [32, 1, 1],
        local_memory_size: 0,
        shared_memory_size: 0,
        profile: profile_nv,
        bindings: Bindings::default(),
        snapshot: SnapshotEnv::default(),
    });
    assert!(out_nv.fallback.is_none(), "unexpected fallback: {:?}", out_nv.fallback);
    assert!(out_nv.info.uses_cooperative_matrix);
    assert!(out_nv.info.requires_full_subgroups);
    let ops_nv = assert_valid_spirv_1_6(&out_nv.spirv_words, [32, 1, 1]);

    assert!(
        ops_nv
            .iter()
            .any(|o| o.opcode == 17 && o.operands == [CAPABILITY_COOPERATIVE_MATRIX_KHR]),
        "missing CooperativeMatrixKHR capability"
    );
    let coop_types_nv: Vec<_> = ops_nv
        .iter()
        .filter(|o| o.opcode == OP_TYPE_COOPERATIVE_MATRIX_KHR)
        .collect();
    assert_eq!(coop_types_nv.len(), 3, "expected A, B, Accumulator coop types");
    assert!(
        ops_nv
            .iter()
            .any(|o| o.opcode == OP_COOPERATIVE_MATRIX_MUL_ADD_KHR),
        "missing native OpCooperativeMatrixMulAddKHR"
    );
    assert!(
        !ops_nv.iter().any(|o| o.opcode == 169),
        "OpSelect patching hack must NOT be emitted"
    );

    // Verify custom tile_m/n/k and custom a_word_map change the generated SPIR-V parameterization.
    let mut profile_custom = Profile::spirv_1_6_default();
    profile_custom.coop_layout = CoopConfig {
        supported: true,
        use_fp16_accum: true,
        tile_m: 16,
        tile_n: 16,
        tile_k: 16,
        subgroup_size: 32,
        a_word_map: [3, 1, 2, 0],
        b_word_map: [1, 0],
        c_word_map: [1, 0],
        d_word_map: [1, 0],
    };

    let out_custom = compile_shader(&LowerInput {
        stage: Stage::Compute,
        program_key: 0xdead_beef,
        code: &code,
        workgroup_size: [64, 1, 1],
        local_memory_size: 0,
        shared_memory_size: 0,
        profile: profile_custom,
        bindings: Bindings::default(),
        snapshot: SnapshotEnv::default(),
    });
    let coop_types_custom: Vec<_> = assert_valid_spirv_1_6(&out_custom.spirv_words, [64, 1, 1])
        .into_iter()
        .filter(|o| o.opcode == OP_TYPE_COOPERATIVE_MATRIX_KHR)
        .map(|o| (o.operands[3], o.operands[4]))
        .collect();
    assert_eq!(coop_types_custom.len(), 3);
    assert_ne!(
        out_nv.spirv_words, out_custom.spirv_words,
        "coop matrix layout parameterization must alter emitted SPIR-V"
    );
}

#[test]
fn structured_selection_if_else_and_predicated_exit() {
    let code = vec![
        uldc_u64(4, 0, 0x160),
        s2r(2, 33),                  // R2 = TID.X
        mov_imm(3, 16),
        isetp_lt_u32(0, 2, 3),       // P0 = TID.X < 16
        exit_insn(0, true),          // @!P0 EXIT
        isetp_lt_u32(1, 2, 255),     // P1 = TID.X < 0
        bra_rel(1, false, 2),        // @P1 BRA taken
        mov_imm(5, 111),             // fallthrough else-branch
        bra_rel(7, false, 1),        // BRA merge
        mov_imm(5, 222),             // taken branch
        shf_l_imm(6, 2, 2),
        stg_ur(5, 6, 4, 0),
        exit_insn(7, false),
    ];

    let out = compile_shader(&LowerInput {
        stage: Stage::Compute,
        program_key: 0x4242,
        code: &code,
        workgroup_size: [32, 1, 1],
        local_memory_size: 0,
        shared_memory_size: 0,
        profile: Profile::spirv_1_6_default(),
        bindings: Bindings::default(),
        snapshot: SnapshotEnv::default(),
    });
    assert!(out.fallback.is_none(), "unexpected fallback: {:?}", out.fallback);
    let ops = assert_valid_spirv_1_6(&out.spirv_words, [32, 1, 1]);
    assert!(ops.iter().any(|o| o.opcode == 247), "expected OpSelectionMerge");
    assert!(ops.iter().any(|o| o.opcode == 245), "expected OpPhi across branches");
}

#[test]
fn structured_if_loop_mkw_and_dead_padding() {
    // 1. Structured loop + forward conditional branch + dead garbage after EXIT.
    let loop_code = vec![
        uldc_u64(4, 0, 0x160),
        mov_imm(2, 0),            // i = 0
        mov_imm(3, 4),            // limit = 4
        mov_imm(5, 0),            // acc = 0
        // idx 4: loop header
        iadd3_reg(5, 5, 2, 255),  // acc += i
        iadd3_reg(2, 2, 255, 1),  // placeholder below replaced by imm add? Use R2 += 1 via mov+add:
        iadd3_reg(2, 2, 5, 255),  // advance
        isetp_lt_u32(0, 2, 3),    // P0 = i < 4
        bra_rel(0, false, -5),    // back to idx 4 while P0
        s2r(6, 33),
        shf_l_imm(6, 6, 2),
        stg_ur(5, 6, 4, 0),
        exit_insn(7, false),
        sm86(0x0000_0b60, 0, 0, 0), // dead TEX opcode after EXIT: must be ignored!
    ];

    let out = compile_shader(&LowerInput {
        stage: Stage::Compute,
        program_key: 1,
        code: &loop_code,
        workgroup_size: [32, 1, 1],
        local_memory_size: 0,
        shared_memory_size: 0,
        profile: Profile::spirv_1_6_default(),
        bindings: Bindings::default(),
        snapshot: SnapshotEnv::default(),
    });
    assert!(out.fallback.is_none(), "unexpected loop fallback: {:?}", out.fallback);
    let ops = assert_valid_spirv_1_6(&out.spirv_words, [32, 1, 1]);
    assert!(ops.iter().any(|o| o.opcode == 246), "expected OpLoopMerge");
    assert!(ops.iter().any(|o| o.opcode == 245), "expected OpPhi for loop variable");

    // 2. MKW 0x5a4f3726590efb17 0x3b0-byte shader compiles cleanly.
    let mut mkw_code = vec![enc_op_dst(0x918, 0, 0, 0); 59]; // 59 * 16 == 0x3b0
    mkw_code[0] = uldc_u64(4, 0, 0x160);
    mkw_code[1] = mov_imm(8, 0);
    mkw_code[2] = mov_imm(9, 0);
    mkw_code[3] = mov_imm(4, 0);
    mkw_code[57] = stg_ur(4, 255, 4, 0);
    mkw_code[58] = exit_insn(7, false);

    let mkw_out = compile_shader(&LowerInput {
        stage: Stage::Compute,
        program_key: 0x5a4f_3726_590e_fb17,
        code: &mkw_code,
        workgroup_size: [32, 1, 1],
        local_memory_size: 0,
        shared_memory_size: 0,
        profile: Profile::spirv_1_6_default(),
        bindings: Bindings::default(),
        snapshot: SnapshotEnv::default(),
    });
    assert!(mkw_out.fallback.is_none());
    let _ = assert_valid_spirv_1_6(&mkw_out.spirv_words, [32, 1, 1]);
}

#[test]
fn fallbacks_are_clean_with_zero_errors() {
    let basic = vec![
        uldc_u64(4, 0, 0x160),
        mov_imm(2, 42),
        stg_ur(2, 255, 4, 0),
        exit_insn(7, false),
    ];

    // 1. Old SPIR-V version (< 1.6) falls back cleanly.
    let mut low_profile = Profile::spirv_1_6_default();
    low_profile.supported_spirv = 0x0001_0500;
    let fb_ver = compile_shader(&LowerInput {
        stage: Stage::Compute,
        program_key: 1,
        code: &basic,
        workgroup_size: [32, 1, 1],
        local_memory_size: 0,
        shared_memory_size: 0,
        profile: low_profile,
        bindings: Bindings::default(),
        snapshot: SnapshotEnv::default(),
    });
    assert!(fb_ver.fallback.is_some());
    assert!(fb_ver.spirv_words.is_empty());

    // 2. Unimplemented reachable SM86 opcode sets exact fallback offset & opcode cleanly.
    let unsupported_code = vec![mov_imm(2, 1), sm86(0x0000_7b60, 0, 0, 0), exit_insn(7, false)];
    let fb_op = compile_shader(&LowerInput {
        stage: Stage::Compute,
        program_key: 2,
        code: &unsupported_code,
        workgroup_size: [32, 1, 1],
        local_memory_size: 0,
        shared_memory_size: 0,
        profile: Profile::spirv_1_6_default(),
        bindings: Bindings::default(),
        snapshot: SnapshotEnv::default(),
    });
    let fb = fb_op.fallback.expect("expected fallback on TEX 0xb60");
    assert_eq!(fb.offset, 16);
    assert_eq!(fb.opcode, 0xb60);

    // 3. HMMA with unsupported coop_layout falls back cleanly.
    let hmma_only = vec![hmma_16816(8, 4, 6, 255), exit_insn(7, false)];
    let mut no_coop = Profile::spirv_1_6_default();
    no_coop.coop_layout.supported = false;
    let fb_coop = compile_shader(&LowerInput {
        stage: Stage::Compute,
        program_key: 3,
        code: &hmma_only,
        workgroup_size: [32, 1, 1],
        local_memory_size: 0,
        shared_memory_size: 0,
        profile: no_coop,
        bindings: Bindings::default(),
        snapshot: SnapshotEnv::default(),
    });
    assert!(fb_coop.fallback.is_some());
}

#[test]
fn batch_compilation_and_ffi_safety() {
    let basic = vec![
        uldc_u64(4, 0, 0x160),
        mov_imm(2, 99),
        stg_ur(2, 255, 4, 0),
        exit_insn(7, false),
    ];
    let bad = vec![sm86(0x7fff, 0, 0, 0)];

    // Pure Rust parallel batch via std::thread::scope.
    let mut batch_in = Vec::new();
    for i in 0..8usize {
        batch_in.push(OwnedCompileInput {
            stage: Stage::Compute,
            program_key: i as u64 + 10,
            code: if i % 2 == 0 { basic.clone() } else { bad.clone() },
            workgroup_size: [32, 1, 1],
            local_memory_size: 0,
            shared_memory_size: 0,
            profile: Profile::spirv_1_6_default(),
            bindings: Bindings::default(),
            snapshot: SnapshotEnv::default(),
        });
    }
    let batch_out = compile_batch(&batch_in);
    assert_eq!(batch_out.len(), 8);
    for (i, o) in batch_out.iter().enumerate() {
        if i % 2 == 0 {
            assert!(o.fallback.is_none());
            let _ = assert_valid_spirv_1_6(&o.spirv_words, [32, 1, 1]);
        } else {
            assert!(o.fallback.is_some());
        }
    }

    // C FFI boundary: null check, compile, batch compile, idempotent free.
    let mut out = EglSpirvCompileOutput::default();
    unsafe {
        assert_eq!(egl_ampere_spirv_compile(ptr::null(), &mut out), -1);
        assert_eq!(egl_ampere_spirv_compile(ptr::null(), ptr::null_mut()), -1);
    }

    let c_code: Vec<EglAmpereInstruction> = basic
        .iter()
        .map(|i| EglAmpereInstruction { words: i.words })
        .collect();
    let c_cbuf = [EglSpirvCbufEntry {
        index: 0,
        offset: 0x160,
        value: 0x1000,
    }];
    let c_in = EglSpirvCompileInput {
        stage: 5,
        texture_bound: u32::MAX,
        program_key: 0x9999,
        program_header: [0; 10],
        code: c_code.as_ptr(),
        code_count: c_code.len(),
        workgroup_size: [64, 1, 1],
        local_memory_size: 0,
        shared_memory_size: 0,
        is_task_shader: 0,
        is_mesh_shader: 0,
        native_tensor: 0,
        native_compact_tensor: 0,
        has_hle_engine_state: 0,
        mesh_as_compute: 0,
        _pad0: [0; 2],
        viewport_transform_state: 0,
        mesh_max_vertices: 0,
        mesh_max_primitives: 0,
        mesh_topology: 0,
        mesh_payload_words: 0,
        _pad1: 0,
        cbufs: c_cbuf.as_ptr(),
        cbuf_count: c_cbuf.len(),
        texture_types: ptr::null(),
        texture_type_count: 0,
        texture_formats: ptr::null(),
        texture_format_count: 0,
        storage_ptrs: ptr::null(),
        storage_ptr_count: 0,
        replacements: ptr::null(),
        replacement_count: 0,
        profile: EglSpirvProfile {
            supported_spirv: SPIRV_1_6_VERSION,
            max_subgroup_size: 32,
            supported_subgroup_stages: u32::MAX,
            max_user_clip_distances: 8,
            min_ssbo_alignment: 16,
            support_float16: 1,
            support_vote: 1,
            coop_layout: EglSpirvCoopLayout {
                supported: 1,
                use_fp16_accum: 1,
                _pad: [0; 2],
                tile_m: 16,
                tile_n: 8,
                tile_k: 16,
                subgroup_size: 32,
                a_word_map: [0, 2, 1, 3],
                b_word_map: [0, 1],
                c_word_map: [0, 1],
                d_word_map: [0, 1],
            },
            ..EglSpirvProfile::default()
        },
        bindings: EglSpirvBindings::default(),
        _pad2: 0,
    };

    unsafe {
        assert_eq!(egl_ampere_spirv_compile(&c_in, &mut out), 0);
        assert_eq!(out.fallback_to_maxwell, 0);
        assert!(out.error.is_null() && out.error_len == 0);
        assert!(out.word_count > 5);
        let words = std::slice::from_raw_parts(out.spirv_words, out.word_count);
        let _ = assert_valid_spirv_1_6(words, [64, 1, 1]);
        // Idempotent double-free & null free must be safe.
        egl_ampere_spirv_free(&mut out);
        assert!(out.spirv_words.is_null() && out.word_count == 0);
        egl_ampere_spirv_free(&mut out);
        egl_ampere_spirv_free(ptr::null_mut());

        // C FFI batch compilation.
        let inputs = [c_in, c_in, c_in, c_in];
        let mut outs = [EglSpirvCompileOutput::default(); 4];
        assert_eq!(egl_ampere_spirv_compile_batch(inputs.as_ptr(), 4, outs.as_mut_ptr()), 0);
        for o in &mut outs {
            assert_eq!(o.fallback_to_maxwell, 0);
            assert!(o.word_count > 5);
            egl_ampere_spirv_free(o);
        }
    }
}

mod regression_tests {
    use super::*;
    use eglantine_ampere_spirv::decode_sm86::{
        decode_insn, DecodedOp, MemWidth, ShflMode, SrcOperand,
    };
    use eglantine_ampere_spirv::ssa_ir::SsaOp;
    use eglantine_ampere_spirv::{lower_to_ssa, LowerInput};

    fn enc_shf_imm(dst: u32, a: u32, shift: u32, c: u32, ty: u32, wrap: bool, right: bool, hi: bool) -> Instruction {
        let w0 = 0x819 | (7 << 12) | (dst << 16) | (a << 24);
        let w2 = c
            | (ty << 9)
            | (u32::from(wrap) << 11)
            | (u32::from(right) << 12)
            | (u32::from(hi) << 16);
        sm86(w0, shift, w2, 0)
    }

    fn enc_shf_reg(dst: u32, a: u32, shift_reg: u32, c: u32, ty: u32, wrap: bool, right: bool, hi: bool) -> Instruction {
        let w0 = 0x219 | (7 << 12) | (dst << 16) | (a << 24);
        let w2 = c
            | (ty << 9)
            | (u32::from(wrap) << 11)
            | (u32::from(right) << 12)
            | (u32::from(hi) << 16);
        sm86(w0, shift_reg, w2, 0)
    }

    fn enc_shfl_imm(dst: u32, pred_out: u32, src: u32, lane: u32, mask: u32, mode: u32) -> Instruction {
        let w0 = 0xf89 | (7 << 12) | (dst << 16) | (src << 24);
        let w1 = ((mode & 3) << 26) | ((lane & 0x1f) << 21) | ((mask & 0x1fff) << 8);
        let w2 = (pred_out & 7) << 17;
        sm86(w0, w1, w2, 0)
    }

    fn enc_uisetp_lt_imm(dst_up: u32, a_ur: u32, imm: u32) -> Instruction {
        let w0 = 0x88c | (7 << 12) | (a_ur << 24);
        let w2 = (1 << 12) | ((dst_up & 7) << 17) | (7 << 23);
        sm86(w0, imm, w2, 0)
    }

    fn default_compute_input(code: &[Instruction]) -> LowerInput<'_> {
        LowerInput {
            stage: Stage::Compute,
            program_key: 0x1234_5678,
            code,
            workgroup_size: [32, 1, 1],
            local_memory_size: 0,
            shared_memory_size: 0,
            profile: Profile::spirv_1_6_default(),
            bindings: Bindings::default(),
            snapshot: SnapshotEnv::default(),
        }
    }

    #[test]
    fn decoded_half_u_imm_i2f_f2i_hfma2_and_uldc_indexed_ops() {
        // F0082: HFMA2 (0x431, form=2) has !wide+upper on b and wide+imm on c;
        // HFMA2 (0x831, form=4) has wide+imm on b and !wide+upper on c.
        let w1_lit = 0xbc00_bc00;
        let w2_upper = 0x15 | (2 << 17) | (1 << 19) | (1 << 20) | (7 << 23);
        let hfma_f2 = sm86(0x431 | (7 << 12) | (4 << 16) | (1 << 24), w1_lit, w2_upper, 0);
        match decode_insn(0, &hfma_f2).op {
            DecodedOp::Hfma2 { dst: 4, b, c, relu, .. } => {
                assert_eq!(b.op, SrcOperand::Reg(0x15));
                assert_eq!((b.swizzle, b.abs, b.neg), (2, true, true));
                assert_eq!(c.op, SrcOperand::Imm32(w1_lit));
                assert_eq!((c.swizzle, c.abs, c.neg), (0, false, false));
                assert!(!relu);
            }
            other => panic!("unexpected HFMA2 form=2 decode: {other:?}"),
        }
        let hfma_f4 = sm86(0x831 | (7 << 12) | (4 << 16) | (1 << 24), w1_lit, w2_upper, 0);
        match decode_insn(0, &hfma_f4).op {
            DecodedOp::Hfma2 { dst: 4, b, c, .. } => {
                assert_eq!(b.op, SrcOperand::Imm32(w1_lit));
                assert_eq!((b.swizzle, b.abs, b.neg), (0, false, false));
                assert_eq!(c.op, SrcOperand::Reg(0x15));
                assert_eq!((c.swizzle, c.abs, c.neg), (2, true, true));
            }
            other => panic!("unexpected HFMA2 form=4 decode: {other:?}"),
        }

        // F0339: HFMA2.RELU sets bit 79 even with PT predicate (bits 87..90 == 7).
        let hfma_relu = sm86(
            0x231 | (7 << 12) | (2 << 16) | (1 << 24),
            2,
            3 | (1 << 15) | (7 << 23),
            0,
        );
        assert!(matches!(decode_insn(0, &hfma_relu).op, DecodedOp::Hfma2 { relu: true, .. }));
        let relu_code = [hfma_relu, exit_insn(7, false)];
        assert!(compile_shader(&default_compute_input(&relu_code)).fallback.is_some());

        // F0198: 0x4a4 UIMAD (form == 2) reads b from bits(64, 8), not words[1] & 0xff.
        let uimad = sm86(0x4a4 | (7 << 12) | (2 << 16) | (1 << 24), 0x1234_5609, 6, 0);
        match decode_insn(0, &uimad).op {
            DecodedOp::Uimad { dst_ur: 2, a, b, c, .. } => {
                assert_eq!(a, SrcOperand::UReg(1));
                assert_eq!(b, SrcOperand::UReg(6));
                assert_eq!(c, SrcOperand::Imm32(0x1234_5609));
            }
            other => panic!("unexpected UIMAD decode: {other:?}"),
        }

        // F0260: 0x499 USHF (form == 2) reads shift from bits(64, 8), not words[1] & 0xff.
        let ushf = sm86(0x499 | (7 << 12) | (3 << 16) | (1 << 24), 0xdead_000a, 5 | (3 << 9), 0);
        match decode_insn(0, &ushf).op {
            DecodedOp::Ushf { dst_ur: 3, a, shift, c, .. } => {
                assert_eq!(a, SrcOperand::UReg(1));
                assert_eq!(shift, SrcOperand::UReg(5));
                assert_eq!(c, SrcOperand::Imm32(0xdead_000a));
            }
            other => panic!("unexpected USHF decode: {other:?}"),
        }

        // F0052: I2F destination/source formats and 0x906 immediate modifier suppression.
        let i2f_u8 = sm86(0x306 | (7 << 12) | (1 << 16), 2, 0x1000, 0);
        let i2f_u16 = sm86(0x306 | (7 << 12) | (1 << 16), 2, 0x0010_1000, 0);
        let i2f_s32_f16 = sm86(0x306 | (7 << 12) | (1 << 16), 2, 0x0020_0c00, 0);
        let i2f_imm = sm86(0x906 | (7 << 12) | (1 << 16), 0xc000_0000, 0x0020_1400, 0);
        assert!(matches!(decode_insn(0, &i2f_u8).op, DecodedOp::I2f { dst_f16: false, src_bits: 8, src_signed: false, .. }));
        assert!(matches!(decode_insn(0, &i2f_u16).op, DecodedOp::I2f { dst_f16: false, src_bits: 16, .. }));
        assert!(matches!(decode_insn(0, &i2f_s32_f16).op, DecodedOp::I2f { dst_f16: true, src_bits: 32, src_signed: true, .. }));
        assert!(matches!(
            decode_insn(0, &i2f_imm).op,
            DecodedOp::I2f {
                src: SrcOperand::Imm32(0xc000_0000),
                dst_f16: false,
                src_bits: 32,
                src_signed: true,
                byte_lane: 0,
                abs: false,
                ..
            }
        ));

        // F0258: F2I.S32 (0x0020_f100: bit 72=1, bit 74=0) decodes dst_signed=true & lowers to F32ConvertToS.
        let f2i_s32 = sm86(0x305 | (7 << 12) | (4 << 16), 1, 0x0020_f100, 0);
        assert!(matches!(decode_insn(0, &f2i_s32).op, DecodedOp::F2i { dst_signed: true, dst_bits: 32, .. }));
        let f2i_code = [
            uldc_u64(4, 0, 0x160),
            s2r(1, 33),
            f2i_s32,
            stg_ur(4, 255, 4, 0),
            exit_insn(7, false),
        ];
        let f2i_ssa = lower_to_ssa(&default_compute_input(&f2i_code)).expect("F2I SSA");
        assert!(f2i_ssa.blocks[0].insns.iter().any(|i| matches!(i.op, SsaOp::F32ConvertToS(_))));

        // F0216: Indexed ULDC (0xabb) extracts index_reg and marks indirect cbuf read of 0x10_000 bytes.
        let uldc_idx = sm86(
            0xabb | (7 << 12) | (4 << 16) | (2 << 24),
            6 << 6,
            4 << 9,
            0,
        );
        assert!(matches!(
            decode_insn(0, &uldc_idx).op,
            DecodedOp::Uldc { dst_ur: 4, bank: 0, offset: 6, index_reg: Some(2), width: MemWidth::B32 }
        ));
        let uldc_code = [
            uldc_u64(6, 0, 0x160),
            uldc_idx,
            enc_op_dst(0xc02, 5, 4, 0), // MOV R5, UR4
            stg_ur(5, 255, 6, 0),
            exit_insn(7, false),
        ];
        let uldc_ssa = lower_to_ssa(&default_compute_input(&uldc_code)).expect("ULDC indexed SSA");
        assert!(uldc_ssa.uses_dynamic_cbuf);
        assert!(uldc_ssa.info.uses_cbuf_indirect);
        assert!(uldc_ssa.info.constant_buffer_used_sizes[0] >= 0x10_000);
        assert!(uldc_ssa.blocks[0].insns.iter().any(|i| matches!(i.op, SsaOp::ReadCbufDynamic { bank: 0, .. })));
    }

    #[test]
    fn subword_ldc_uldc_and_imad_wide_hi_64bit_addend() {
        // F0094: Unaligned static LDC.S16 and dynamic ULDC.U8 extract subwords & align cbuf lookup.
        let ldc_s16 = sm86(
            0xb82 | (7 << 12) | (2 << 16) | (255 << 24),
            0x12 << 6,
            3 << 9, // S16
            0,
        );
        let uldc_u8_idx = sm86(
            0xabb | (7 << 12) | (2 << 16) | (1 << 24),
            0x20 << 6,
            0 << 9, // U8
            0,
        );
        let code = [
            uldc_u64(4, 0, 0x160),
            ldc_s16,
            stg_ur(2, 255, 4, 0),
            uldc_u8_idx,
            enc_op_dst(0xc02, 3, 2, 0), // MOV R3, UR2
            stg_ur(3, 255, 4, 4),
            exit_insn(7, false),
        ];
        let mut input = default_compute_input(&code);
        input.snapshot.replacements.insert((0, 0x10), 0x1234_8001);
        let ssa = lower_to_ssa(&input).expect("subword LDC/ULDC");
        assert!(ssa.blocks[0].insns.iter().any(|i| matches!(i.op, SsaOp::ConstU32(0x1234_8001))));
        assert!(ssa.blocks[0].insns.iter().any(|i| matches!(i.op, SsaOp::SBitFieldExtract { count: 16, .. })));
        assert!(ssa.blocks[0].insns.iter().any(|i| matches!(i.op, SsaOp::ReadCbufDynamic { bank: 0, .. })));

        // F0050: IMAD.WIDE / IMAD.HI / UIMAD.WIDE include c:c+1 & neg_c carry into hi word.
        let imad_wide = sm86(0x225 | (7 << 12) | (6 << 16) | (1 << 24), 2, 4 | (1 << 11), 0);
        let imad_hi = sm86(0x227 | (7 << 12) | (8 << 16) | (1 << 24), 2, 4, 0);
        let uimad_wide = sm86(0x2a5 | (7 << 12) | (2 << 16) | (0 << 24), 1, 4, 0);
        let uldc_u64_pair0 = uldc_u64(0, 0, 0x140);
        let uldc_u64_pair4 = uldc_u64(4, 0, 0x148);
        let wide_code = [
            uldc_u64(6, 0, 0x160),
            s2r(1, 33),
            s2r(2, 34),
            s2r(4, 35),
            s2r(5, 33),
            uldc_u64_pair0,
            uldc_u64_pair4,
            imad_wide,
            imad_hi,
            uimad_wide,
            enc_op_dst(0xc02, 9, 3, 0), // MOV R9, UR3
            stg_ur(6, 255, 6, 0),
            stg_ur(7, 255, 6, 4),
            stg_ur(8, 255, 6, 8),
            stg_ur(9, 255, 6, 12),
            exit_insn(7, false),
        ];
        let out = compile_shader(&default_compute_input(&wide_code));
        assert!(out.fallback.is_none(), "unexpected IMAD fallback: {:?}", out.fallback);
        let _ = assert_valid_spirv_1_6(&out.spirv_words, [32, 1, 1]);

        let ssa_wide = lower_to_ssa(&default_compute_input(&wide_code)).expect("wide IMAD SSA");
        let add_count = ssa_wide.blocks[0]
            .insns
            .iter()
            .filter(|i| matches!(i.op, SsaOp::IAdd(_, _)))
            .count();
        assert!(add_count >= 5, "expected 64-bit c:c+1 add & carry across wide/hi IMADs");

        // Odd c register on IMAD.WIDE cleanly triggers fallback instead of miscompiling.
        let bad_c_code = [
            sm86(0x225 | (7 << 12) | (6 << 16) | (1 << 24), 2, 3, 0),
            exit_insn(7, false),
        ];
        assert!(compile_shader(&default_compute_input(&bad_c_code)).fallback.is_some());
    }

    #[test]
    fn selection_predicated_ptr_and_loop_phi_tracking() {
        // F0051: Single-armed If updates R2 ptr offset; merge_states must Phi byte_offset (valid SSA dominance).
        let ldc_r2_b64 = sm86(0xb82 | (7 << 12) | (2 << 16) | (255 << 24), 0x160 << 6, 5 << 9, 0);
        let pred_iadd_r2 = sm86(
            0x210 | (0 << 12) | (2 << 16) | (2 << 24),
            4,
            255 | (0x3f << 17) | (15 << 23),
            0,
        );
        let pred_uisetp = sm86(0x88c | (0 << 12), 10, (1 << 12) | (2 << 17) | (7 << 23), 0);
        let if_code = [
            ldc_r2_b64,
            s2r(0, 33),
            mov_imm(1, 8),
            mov_imm(4, 16),
            isetp_lt_u32(0, 0, 1),
            bra_rel(0, false, 1),
            iadd3_reg(2, 2, 4, 255),
            pred_iadd_r2,
            pred_uisetp,
            sm86(0x386 | (7 << 12) | (2 << 24), 0, 4 << 9, 0), // STG [R2], R0
            exit_insn(7, false),
        ];
        let if_out = compile_shader(&default_compute_input(&if_code));
        assert!(if_out.fallback.is_none(), "If ptr merge failed: {:?}", if_out.fallback);
        let if_ops = assert_valid_spirv_1_6(&if_out.spirv_words, [32, 1, 1]);
        assert!(if_ops.iter().any(|o| o.opcode == 245), "expected OpPhi for pointer offset merge");
        assert!(if_ops.iter().any(|o| o.opcode == 169), "expected OpSelect for predicated ptr/up");

        // F0061: Loop modifies R2 ptr offset, UP1 (UISETP), and P2 (SHFL), requiring header Phis.
        let loop_code = [
            ldc_r2_b64,
            mov_imm(0, 0),
            mov_imm(1, 4),
            mov_imm(4, 16),
            mov_imm(6, 0),
            isetp_lt_u32(2, 1, 0), // init P2 = false (4 < 0 is false)
            // Loop header (idx 6):
            sm86(0x210 | (2 << 12) | (6 << 16) | (6 << 24), 4, 255 | (0x3f << 17) | (15 << 23), 0), // @P2 IADD3 R6, R6, R4
            iadd3_reg(2, 2, 4, 255),
            iadd3_reg(6, 6, 2, 255),
            enc_uisetp_lt_imm(1, 0, 5),
            enc_shfl_imm(7, 2, 0, 1, 0x1f, 1),
            sm86(0x386 | (7 << 12) | (2 << 24), 7, 4 << 9, 0), // STG [R2], R7
            sm86(0x386 | (7 << 12) | (2 << 24), 6 | (4 << 8), 4 << 9, 0), // STG [R2+4], R6
            iadd3_reg(0, 0, 255, 4),
            isetp_lt_u32(0, 0, 1),
            bra_rel(0, false, -10), // target = 15 + 1 - 10 = 6
            exit_insn(7, false),
        ];
        let loop_out = compile_shader(&default_compute_input(&loop_code));
        assert!(loop_out.fallback.is_none(), "loop ptr/up/shfl failed: {:?}", loop_out.fallback);
        let _ = assert_valid_spirv_1_6(&loop_out.spirv_words, [32, 1, 1]);
        let loop_ssa = lower_to_ssa(&default_compute_input(&loop_code)).expect("loop SSA");
        let loop_header_phis: usize = loop_ssa
            .blocks
            .iter()
            .skip(1)
            .map(|b| b.insns.iter().filter(|i| matches!(i.op, SsaOp::Phi(_))).count())
            .sum();
        assert!(loop_header_phis >= 4, "expected Phis for R0, R2/r_ptr, R6, P2");

        // Overwriting a pointer register with a non-pointer constant inside a loop must invalidate
        // provenance across the loop rather than keeping stale PtrOrigin.
        let clobber_loop_code = [
            ldc_r2_b64,
            mov_imm(0, 0),
            mov_imm(1, 2),
            mov_imm(2, 0), // loop header (idx 3): overwrites R2 with non-pointer imm
            isetp_lt_u32(0, 0, 1),
            bra_rel(0, false, -3), // target = 5 + 1 - 3 = 3
            sm86(0x386 | (7 << 12) | (2 << 24), 0, 4 << 9, 0), // STG [R2], R0 -> no provenance!
            exit_insn(7, false),
        ];
        assert!(compile_shader(&default_compute_input(&clobber_loop_code)).fallback.is_some());
    }

    #[test]
    fn ssbo_direct_indirect_offset_min_alignment_and_shf_shfl() {
        // F0015 & F0144: Direct SSBO indirect_offset == u32::MAX + min_ssbo_alignment masking.
        let ssbo_code = [
            uldc_u64(4, 0, 0x160),
            mov_imm(0, 42),
            stg_ur(0, 255, 4, 0),
            exit_insn(7, false),
        ];
        let mut input = default_compute_input(&ssbo_code);
        input.profile.min_ssbo_alignment = 256;
        let ssa_256 = lower_to_ssa(&input).expect("min_ssbo_alignment 256");
        assert_eq!(ssa_256.info.storage_buffer_descriptors[0].indirect_offset, u32::MAX);
        assert!(ssa_256.blocks[0].insns.iter().any(|i| matches!(i.op, SsaOp::ConstU32(255))));

        let mut input_rep = default_compute_input(&ssbo_code);
        input_rep.profile.min_ssbo_alignment = 256;
        input_rep.snapshot.replacements.insert((0, 0x160), 0x1040);
        let ssa_rep = lower_to_ssa(&input_rep).expect("min_ssbo_alignment replacement");
        assert!(ssa_rep.blocks[0].insns.iter().any(|i| matches!(i.op, SsaOp::ConstU32(0x40))));

        // F0121: SHFL clamp/seg_mask, pred_out = in_range, and !support_shuffle_relative fallback.
        let shfl_code = [
            uldc_u64(4, 0, 0x160),
            mov_imm(1, 99),
            enc_shfl_imm(2, 0, 1, 1, 0x0f0f, 1), // SHFL.UP P0, R2, R1, 1, 0x0f0f
            stg_ur(2, 255, 4, 0),
            sm86(0x986 | (0 << 12), 1 | (4 << 8), 0x0810_a100 | 4 | (4 << 9), 0), // @P0 STG [UR4+4], R1
            exit_insn(7, false),
        ];
        let mut shfl_in = default_compute_input(&shfl_code);
        shfl_in.profile.support_shuffle_relative = false;
        let shfl_ssa = lower_to_ssa(&shfl_in).expect("SHFL SSA");
        assert!(shfl_ssa.blocks[0]
            .insns
            .iter()
            .any(|i| matches!(i.op, SsaOp::SubgroupShuffle { mode: ShflMode::Idx, .. })));
        let shfl_out = compile_shader(&shfl_in);
        assert!(shfl_out.fallback.is_none(), "SHFL fallback: {:?}", shfl_out.fallback);
        let shfl_ops = assert_valid_spirv_1_6(&shfl_out.spirv_words, [32, 1, 1]);
        // Must use OpGroupNonUniformShuffle (345) instead of ShuffleUp (346) when !support_shuffle_relative.
        assert!(shfl_ops.iter().any(|o| o.opcode == 345));
        assert!(!shfl_ops.iter().any(|o| o.opcode == 346));
        assert!(shfl_ops.iter().any(|o| o.opcode == 169), "expected OpSelect clamping out-of-range lane");

        let mut unsup_stage = default_compute_input(&shfl_code);
        unsup_stage.profile.supported_subgroup_stages = 1 << (Stage::VertexB as u32);
        assert!(compile_shader(&unsup_stage).fallback.is_some());

        // F0031: SHF never emits Shl/LShr/AShr with shift >= 32 and compiles dynamic/wide shifts cleanly.
        let shf_code = [
            mov_imm(1, 0x1122_3344),
            mov_imm(2, 0x5566_7788),
            s2r(3, 33),
            enc_shf_imm(4, 1, 0, 2, 3, false, true, false),   // shift=0 right lo -> R1
            enc_shf_imm(5, 1, 40, 2, 2, false, true, false),  // shift=40 signed right lo -> c >>_s 8
            enc_shf_imm(6, 1, 64, 2, 3, false, true, true),   // shift=64 unsigned right hi -> 0
            enc_shf_reg(7, 1, 3, 2, 2, false, true, false),   // dynamic signed right lo
            enc_shf_reg(8, 1, 3, 2, 3, false, false, true),   // dynamic left hi
            enc_shf_reg(9, 1, 3, 2, 3, true, true, false),    // wrapped right lo
            exit_insn(7, false),
        ];
        let shf_out = compile_shader(&default_compute_input(&shf_code));
        assert!(shf_out.fallback.is_none(), "SHF failed: {:?}", shf_out.fallback);
        let _ = assert_valid_spirv_1_6(&shf_out.spirv_words, [32, 1, 1]);

        let shf_ssa = lower_to_ssa(&default_compute_input(&shf_code)).expect("SHF SSA");
        let consts: std::collections::HashMap<_, _> = shf_ssa
            .blocks
            .iter()
            .flat_map(|b| &b.insns)
            .filter_map(|i| match i.op {
                SsaOp::ConstU32(v) => Some((i.id, v)),
                _ => None,
            })
            .collect();
        for blk in &shf_ssa.blocks {
            for insn in &blk.insns {
                if let SsaOp::Shl(_, s) | SsaOp::LShr(_, s) | SsaOp::AShr(_, s) = insn.op {
                    if let Some(&v) = consts.get(&s) {
                        assert!(v < 32, "found out-of-range constant shift {v} >= 32");
                    }
                }
            }
        }
    }

    #[test]
    fn coop_config_and_depth8_fallback_f0420() {
        let cfg = Profile::spirv_1_6_default().coop_layout;
        assert!(cfg.validate(16).is_ok());
        assert!(cfg.validate(8).is_err(), "depth 8 must be rejected");

        let mut bad_k = cfg;
        bad_k.tile_k = 8;
        assert!(bad_k.validate(16).is_err());

        let mut bad_a = cfg;
        bad_a.a_word_map = [0, 1, 2, 4];
        assert!(bad_a.validate(16).is_err());

        let mut bad_a_dup = cfg;
        bad_a_dup.a_word_map = [0, 1, 1, 3];
        assert!(bad_a_dup.validate(16).is_err());

        let mut bad_b = cfg;
        bad_b.b_word_map = [0, 2];
        assert!(bad_b.validate(16).is_err());

        let hmma8 = sm86(0x23c | (7 << 12) | (8 << 16) | (4 << 24), 6, 255, 0);
        let code = [
            mov_imm(4, 0),
            mov_imm(5, 0),
            mov_imm(6, 0),
            mov_imm(7, 0),
            hmma8,
            exit_insn(7, false),
        ];
        let out = compile_shader(&default_compute_input(&code));
        assert!(out.fallback.is_some(), "expected fallback for depth=8 HMMA");
        assert!(out.spirv_words.is_empty());
    }

    #[test]
    fn atomg_decode_and_lowering_f0152() {
        use eglantine_ampere_spirv::decode_sm86::AtomicOp;

        let enc_atomg_reg =
            |dst: u32, base: u32, src: u32, off: i32, op_bits: u32, ty_bits: u32, extra: u32| {
                sm86(
                    0x3a8 | (7 << 12) | ((dst & 0xff) << 16) | ((base & 0xff) << 24),
                    (src & 0xff) | (((off as u32) & 0xff_ffff) << 8),
                    0x001e_e100 | ((ty_bits & 7) << 9) | ((op_bits & 0xf) << 23) | extra,
                    0,
                )
            };
        let enc_atomg_ur =
            |dst: u32, ur: u32, src: u32, off: i32, op_bits: u32, ty_bits: u32, extra: u32| {
                sm86(
                    0x9a8 | (7 << 12) | ((dst & 0xff) << 16) | (255 << 24),
                    (src & 0xff) | (((off as u32) & 0xff_ffff) << 8),
                    0x081e_e140 | (ur & 0x3f) | ((ty_bits & 7) << 9) | ((op_bits & 0xf) << 23) | extra,
                    0,
                )
            };

        match decode_insn(0, &enc_atomg_reg(2, 4, 3, 8, 8, 1, 0)).op {
            DecodedOp::Atomg {
                dst: Some(2),
                base_reg: 4,
                uniform_reg: None,
                imm_offset: 8,
                src_a: 3,
                op: AtomicOp::Exch,
                signed: true,
            } => {}
            other => panic!("unexpected ATOMG reg decode: {other:?}"),
        }

        // Unsupported ops (Inc=3, Dec=4, CAS/reserved=9), types (B64=2, F32=3), and extra mode bits must fail cleanly.
        for bad in [
            enc_atomg_reg(2, 4, 3, 0, 3, 0, 0),
            enc_atomg_ur(2, 4, 3, 0, 4, 0, 0),
            enc_atomg_ur(2, 4, 3, 0, 9, 0, 0),
            enc_atomg_ur(2, 4, 3, 0, 0, 2, 0),
            enc_atomg_ur(2, 4, 3, 0, 0, 3, 0),
            enc_atomg_reg(2, 4, 3, 0, 0, 0, 1 << 12),
            enc_atomg_ur(2, 4, 3, 0, 0, 0, 1 << 12),
        ] {
            assert!(
                matches!(decode_insn(0, &bad).op, DecodedOp::Unsupported { .. }),
                "expected Unsupported for {bad:?}"
            );
            let fb = compile_shader(&default_compute_input(&[uldc_u64(4, 0, 0x160), mov_imm(3, 1), bad, exit_insn(7, false)]));
            assert!(fb.fallback.is_some(), "expected Maxwell fallback for unsupported ATOMG");
        }

        let code = [
            uldc_u64(4, 0, 0x160),
            mov_imm(1, 7),
            enc_atomg_ur(2, 4, 1, 0, 8, 0, 0),   // EXCH U32 -> OpAtomicExchange (229)
            enc_atomg_ur(3, 4, 1, 4, 5, 0, 0),   // AND U32 -> OpAtomicAnd (240)
            enc_atomg_ur(4, 4, 1, 8, 6, 0, 0),   // OR U32 -> OpAtomicOr (241)
            enc_atomg_ur(5, 4, 1, 12, 7, 0, 0),  // XOR U32 -> OpAtomicXor (242)
            enc_atomg_ur(6, 4, 1, 16, 1, 0, 0),  // MIN U32 -> OpAtomicUMin (237)
            enc_atomg_ur(7, 4, 1, 20, 2, 0, 0),  // MAX U32 -> OpAtomicUMax (239)
            enc_atomg_ur(8, 4, 1, 24, 1, 1, 0),  // MIN S32 -> OpAtomicSMin (236)
            enc_atomg_ur(9, 4, 1, 28, 2, 1, 0),  // MAX S32 -> OpAtomicSMax (238)
            stg_ur(9, 255, 4, 32),
            exit_insn(7, false),
        ];
        let out = compile_shader(&default_compute_input(&code));
        assert!(out.fallback.is_none(), "ATOMG failed: {:?}", out.fallback);
        let ops = assert_valid_spirv_1_6(&out.spirv_words, [32, 1, 1]);
        for expected_op in [229u16, 236, 237, 238, 239, 240, 241, 242] {
            assert!(
                ops.iter().any(|o| o.opcode == expected_op),
                "missing expected SPIR-V atomic opcode {expected_op}"
            );
        }
    }

    #[test]
    fn uiadd3_up_carry_and_accum_f0431() {
        let enc_uiadd3_reg =
            |dst_ur: u32, a_ur: u32, b_ur: u32, c_ur: u32, x_mode: bool, carry_in: u32, carry_out: u32| {
                let w0 = 0x290 | (7 << 12) | ((dst_ur & 0x3f) << 16) | ((a_ur & 0x3f) << 24);
                let w1 = b_ur & 0x3f;
                let w2 = (c_ur & 0x3f)
                    | (u32::from(x_mode) << 10)
                    | ((carry_out & 7) << 17)
                    | ((carry_in & 0xf) << 23);
                sm86(w0, w1, w2, 0)
            };
        let enc_uiadd3_imm =
            |dst_ur: u32, a_ur: u32, imm: u32, c_ur: u32, x_mode: bool, carry_in: u32, carry_out: u32| {
                let w0 = 0x890 | (7 << 12) | ((dst_ur & 0x3f) << 16) | ((a_ur & 0x3f) << 24);
                let w2 = (c_ur & 0x3f)
                    | (u32::from(x_mode) << 10)
                    | ((carry_out & 7) << 17)
                    | ((carry_in & 0xf) << 23);
                sm86(w0, imm, w2, 0)
            };

        let code = [
            uldc_u64(4, 0, 0x160),                    // UR4/UR5 = SSBO base
            enc_uisetp_lt_imm(0, 63, 1),              // UP0 = (0 < 1) == true, while P0 untouched
            enc_uiadd3_imm(6, 63, 0x7fff_ffff, 63, false, 15, 7),
            enc_uiadd3_imm(7, 63, 1, 63, false, 15, 7),
            enc_uiadd3_reg(8, 6, 6, 7, false, 15, 7), // UR8 = 0xffff_ffff
            enc_uiadd3_reg(9, 63, 63, 8, true, 0, 1), // UP1 = carry(0 + 0 + 0xffff_ffff + UP0)
            enc_uiadd3_reg(6, 4, 63, 9, true, 1, 7),  // consume UR9 & UP1 into SSBO offset
            mov_imm(2, 42),
            stg_ur(2, 255, 6, 0),
            exit_insn(7, false),
        ];

        let input = default_compute_input(&code);
        let ssa = lower_to_ssa(&input).expect("UIADD3 SSA");
        let uisetp_cmp_id = ssa.blocks[0]
            .insns
            .iter()
            .find(|i| matches!(i.op, SsaOp::IntCmp { signed: false, .. }))
            .expect("expected UISETP IntCmp")
            .id;
        let uisetp_up0_id = ssa.blocks[0]
            .insns
            .iter()
            .find(|i| matches!(i.op, SsaOp::BoolAnd(a, _) if a == uisetp_cmp_id))
            .expect("expected UISETP BoolAnd result for UP0")
            .id;

        // UIADD3.X carry_in = UP0 must read `state.up[0]` (`uisetp_up0_id`), not `state.p[0]`.
        let uses_up0_select = ssa.blocks[0].insns.iter().any(|i| {
            matches!(
                i.op,
                SsaOp::Select { cond, .. } if cond == uisetp_up0_id
            )
        });
        assert!(uses_up0_select, "UIADD3 must read UP0 from state.up rather than P0");

        let out = compile_shader(&input);
        assert!(out.fallback.is_none(), "UIADD3 compile failed: {:?}", out.fallback);
        let _ = assert_valid_spirv_1_6(&out.spirv_words, [32, 1, 1]);
    }

    #[test]
    fn f2i_rounding_and_signed_subword_f0402() {
        use eglantine_ampere_spirv::ssa_ir::GlslOp;

        let enc_f2i = |dst: u32, src: u32, signed: bool, size_bits: u32, round_mode: u32| {
            let w0 = 0x305 | (7 << 12) | ((dst & 0xff) << 16);
            let w2 = (u32::from(signed) << 8)
                | ((size_bits & 3) << 11)
                | ((round_mode & 3) << 14);
            sm86(w0, src & 0xff, w2, 0)
        };

        let code = [
            uldc_u64(4, 0, 0x160),
            mov_imm(1, 0x3fc0_0000),       // 1.5f
            enc_f2i(2, 1, true, 2, 0),     // F2I.ROUND.S32
            enc_f2i(3, 1, false, 2, 1),    // F2I.FLOOR.U32
            enc_f2i(4, 1, true, 2, 2),     // F2I.CEIL.S32
            enc_f2i(5, 1, true, 0, 3),     // F2I.TRUNC.S8
            enc_f2i(6, 1, true, 1, 3),     // F2I.TRUNC.S16
            enc_f2i(7, 1, false, 0, 3),    // F2I.TRUNC.U8
            stg_ur(2, 255, 4, 0),
            stg_ur(3, 255, 4, 4),
            stg_ur(4, 255, 4, 8),
            stg_ur(5, 255, 4, 12),
            stg_ur(6, 255, 4, 16),
            stg_ur(7, 255, 4, 20),
            exit_insn(7, false),
        ];

        let input = default_compute_input(&code);
        let ssa = lower_to_ssa(&input).expect("F2I SSA");
        let insns = &ssa.blocks[0].insns;
        assert!(insns.iter().any(|i| matches!(i.op, SsaOp::Glsl1(GlslOp::RoundEven, _))));
        assert!(insns.iter().any(|i| matches!(i.op, SsaOp::Glsl1(GlslOp::Floor, _))));
        assert!(insns.iter().any(|i| matches!(i.op, SsaOp::Glsl1(GlslOp::Ceil, _))));
        assert!(insns
            .iter()
            .any(|i| matches!(i.op, SsaOp::SBitFieldExtract { offset: 0, count: 8, .. })));
        assert!(insns
            .iter()
            .any(|i| matches!(i.op, SsaOp::SBitFieldExtract { offset: 0, count: 16, .. })));

        let out = compile_shader(&input);
        assert!(out.fallback.is_none(), "F2I compile failed: {:?}", out.fallback);
        let ops = assert_valid_spirv_1_6(&out.spirv_words, [32, 1, 1]);
        assert!(
            ops.iter().any(|o| o.opcode == 202),
            "expected OpBitFieldSExtract (202) for signed 8/16-bit F2I"
        );
    }
}
