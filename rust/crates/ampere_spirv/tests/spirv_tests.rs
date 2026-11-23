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
