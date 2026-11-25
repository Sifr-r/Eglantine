// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![deny(warnings)]

use eglantine_ampere_rs::ffi::{
    egl_ampere_capture_init, egl_ampere_capture_observe, egl_ampere_decode_override,
    egl_ampere_encode_override, egl_ampere_free_bytes, egl_ampere_free_lowered,
    egl_ampere_has_native_address_map, egl_ampere_lower_to_maxwell,
    egl_ampere_native_program_address, egl_ampere_native_program_key,
    egl_ampere_override_source_size, EglAmpereBytes, EglAmpereCaptureResult,
    EglAmpereCaptureState, EglAmpereLoweredProgram, EglAmpereLoweringOptions,
};
use eglantine_ampere_rs::{
    append_native_address_map, collect_register_operands, decode_program_override,
    encode_program_override, has_native_address_map, lower_to_maxwell, native_program_address,
    native_program_key, prepare_register_pressure, program_override_source_size, CaptureResult,
    Instruction, LoweringOptions, ProgramCaptureState, TensorModeOverride,
    NATIVE_ADDRESS_MAP_MAGIC,
};
use std::ptr;

const EXIT: Instruction = Instruction::new([0x0000_794d, 0, 0x0380_0000, 0]);
const NOP: Instruction = Instruction::new([0x0000_7918, 0, 0, 0]);

fn hmma_sample() -> Instruction {
    // HMMA.1688 (opcode 0x23c) with predicate 7 (unconditional)
    Instruction::new([0x0004_723c, 0x0000_0008, 0x0000_0000, 0])
}

#[test]
fn capture_state_sequences() {
    // Simple EXIT at index 0
    let mut cap = ProgramCaptureState::new();
    assert_eq!(cap.observe(&EXIT, 0), CaptureResult::Complete);

    // Forward BRA over EXIT does not exit because index < last_forward_target
    let mut cap_fwd = ProgramCaptureState::new();
    let fwd_bra = Instruction::new([0x0000_7947, 16, 0, 0]); // +16 bytes -> insn 2
    assert_eq!(cap_fwd.observe(&fwd_bra, 0), CaptureResult::Continue);
    assert_eq!(cap_fwd.observe(&EXIT, 1), CaptureResult::Continue);
    assert!(!cap_fwd.main_exited);
    assert_eq!(cap_fwd.observe(&EXIT, 2), CaptureResult::Complete);

    // CALL -> EXIT -> RET sequence sets main_exited after EXIT >= last_forward_target
    let mut cap_call = ProgramCaptureState::new();
    let call_to_2 = Instruction::new([0x0000_7944, 16, 0, 0]); // target = (0 + 16 + 16) / 16 = 2
    let ret = Instruction::new([0x0000_7950, 0xffff_ffc0, 0x03c3_ffff, 0]);
    assert_eq!(cap_call.observe(&call_to_2, 0), CaptureResult::Continue);
    assert_eq!(cap_call.last_call_target, 2);
    assert_eq!(cap_call.observe(&EXIT, 1), CaptureResult::Continue);
    assert!(cap_call.main_exited);
    assert_eq!(cap_call.observe(&ret, 2), CaptureResult::Complete);

    // Computed branch (BRX 0x949) completes on closing unconditional self-BRA
    let mut cap_comp = ProgramCaptureState::new();
    let brx = Instruction::new([0x0000_7949, 0, 0, 0]);
    let self_bra = Instruction::new([0x0000_7947, (-16i32) as u32, 0x0003_ffff, 0]);
    assert_eq!(cap_comp.observe(&brx, 0), CaptureResult::Continue);
    assert!(cap_comp.computed_branch);
    assert_eq!(cap_comp.observe(&EXIT, 1), CaptureResult::Continue);
    assert_eq!(cap_comp.observe(&self_bra, 2), CaptureResult::Complete);

    // Negative and misaligned targets return InvalidTarget
    let mut cap_bad = ProgramCaptureState::new();
    let neg_bra = Instruction::new([0x0000_7947, (-32i32) as u32, 0x0003_ffff, 0]);
    assert_eq!(cap_bad.observe(&neg_bra, 0), CaptureResult::InvalidTarget);
    let misaligned_jcal = Instruction::new([0x0000_7945, 4, 0, 0]);
    assert_eq!(
        cap_bad.observe(&misaligned_jcal, 0),
        CaptureResult::InvalidTarget
    );
    let oob_jcal = Instruction::new([
        0x0000_7945,
        ProgramCaptureState::MAXIMUM_INSTRUCTIONS * 16,
        0,
        0,
    ]);
    assert_eq!(cap_bad.observe(&oob_jcal, 0), CaptureResult::InvalidTarget);
}

#[test]
fn native_address_map_roundtrip_and_bounds() {
    let mut code = vec![0x1111_2222_3333_4444u64; 16];
    assert!(!has_native_address_map(&code));
    let offsets = vec![0u32, 1u32, 2u32];
    append_native_address_map(&mut code, &offsets);
    assert!(has_native_address_map(&code));
    assert_eq!(*code.last().unwrap(), NATIVE_ADDRESS_MAP_MAGIC);

    assert_eq!(native_program_address(&code, 0), Some(0x50));
    assert_eq!(native_program_address(&code, 16), Some(0x58));
    assert_eq!(native_program_address(&code, 32), Some(0x68));
    assert_eq!(native_program_address(&code, 4), None); // misaligned
    assert_eq!(native_program_address(&code, 48), None); // OOB

    // Bad magic and truncated buffers
    let mut bad_magic = code.clone();
    *bad_magic.last_mut().unwrap() ^= 1;
    assert!(!has_native_address_map(&bad_magic));
    assert_eq!(native_program_address(&bad_magic, 0), None);
    assert!(!has_native_address_map(&[NATIVE_ADDRESS_MAP_MAGIC]));
}

#[test]
fn program_override_codec_roundtrip_and_errors() {
    let source = [
        Instruction::new([0x0001_7306, 0x0000_0001, 0x0010_1400, 0x020e_a200]),
        EXIT,
    ];
    let key = native_program_key(&source);
    assert_ne!(key, 0);

    let lowered = lower_to_maxwell(&source, LoweringOptions::default());
    assert!(lowered.is_ok(), "unexpected error: {}", lowered.error);

    let encoded = encode_program_override(&source, &lowered);
    assert!(!encoded.is_empty());
    assert_eq!(program_override_source_size(&encoded), 32);

    let decoded = decode_program_override(&encoded, &source);
    assert!(decoded.is_ok(), "decode error: {}", decoded.error);
    assert_eq!(decoded.instructions, lowered.instructions);
    assert_eq!(
        decoded.source_instruction_offsets,
        lowered.source_instruction_offsets
    );
    assert_eq!(decoded.source_size, lowered.source_size);
    assert_eq!(
        decoded.required_texture_buffer,
        lowered.required_texture_buffer
    );

    // Error program encodes to empty vector
    let mut err_prog = lowered.clone();
    err_prog.error = "bad".to_string();
    assert!(encode_program_override(&source, &err_prog).is_empty());

    // Hash mismatch when source differs
    let other_source = [NOP, EXIT];
    let mismatch = decode_program_override(&encoded, &other_source);
    assert!(!mismatch.is_ok());

    // Truncated / malformed override payload fails cleanly
    assert_eq!(program_override_source_size(&encoded[..12]), 0);
    assert!(!decode_program_override(&encoded[..encoded.len() - 4], &source).is_ok());
}

#[test]
fn register_pressure_handles_high_registers() {
    // High registers R240, R242, R244 in FADD (0x221): dst@16=R240, srcA@24=R242, srcB@32=R244
    let high_fadd = Instruction::new([
        0x221 | (7 << 12) | (240 << 16) | (242 << 24),
        244,
        0,
        0,
    ]);
    let mut cols = Vec::new();
    let ok = collect_register_operands(&high_fadd, |pos, count| cols.push((pos, count)));
    assert!(ok);
    assert_eq!(cols, vec![(16, 1), (24, 1), (32, 1)]);

    let pressure = prepare_register_pressure(&high_fadd, 216);
    assert!(pressure.valid);
    assert!(!pressure.prefix.is_empty());
    assert!(!pressure.suffix.is_empty());
}

#[test]
fn sm86_lowering_core_parity_cases() {
    // EXIT and closing self-BRA
    let exit_only = lower_to_maxwell(&[EXIT], LoweringOptions::default());
    assert!(exit_only.is_ok(), "{}", exit_only.error);
    assert!(!exit_only.instructions.is_empty());

    // CALL + EXIT + RET subroutine
    let call_ret = [
        Instruction::new([0x0000_7802, 0x20, 0x0000_0f00, 0]),
        Instruction::new([0x0000_7944, 0x10, 0x03c0_0000, 0]),
        EXIT,
        Instruction::new([0x0000_7950, 0xffff_ffc0, 0x03c3_ffff, 0]),
    ];
    let lowered_ret = lower_to_maxwell(&call_ret, LoweringOptions::default());
    assert!(lowered_ret.is_ok(), "{}", lowered_ret.error);
    assert_eq!(lowered_ret.instructions.last().copied(), Some(0xe320_0000_0007_000f));

    // I2F cases matching ampere_i2f.cpp expectations
    let i2f_1 = [
        Instruction::new([0x0001_7306, 0x0000_0001, 0x0010_1400, 0x020e_a200]),
        EXIT,
    ];
    let l_i2f_1 = lower_to_maxwell(&i2f_1, LoweringOptions::default());
    assert!(l_i2f_1.is_ok(), "{}", l_i2f_1.error);
    assert!(l_i2f_1.instructions.contains(&0x5cb8_0000_0017_2601));

    let i2f_2 = [
        Instruction::new([0x0003_7306, 0x0000_0003, 0x0010_1400, 0x000e_2400]),
        EXIT,
    ];
    let l_i2f_2 = lower_to_maxwell(&i2f_2, LoweringOptions::default());
    assert!(l_i2f_2.is_ok(), "{}", l_i2f_2.error);
    assert!(l_i2f_2.instructions.contains(&0x5cb8_0000_0037_2603));

    let i2f_3 = [
        Instruction::new([0x0001_7306, 0x0000_0001, 0x0010_1000, 0]),
        EXIT,
    ];
    let l_i2f_3 = lower_to_maxwell(&i2f_3, LoweringOptions::default());
    assert!(l_i2f_3.is_ok(), "{}", l_i2f_3.error);
    assert!(l_i2f_3.instructions.contains(&0x5cb8_0000_0017_0601));

    // F2I valid & unsupported
    let f2i_ok = [
        Instruction::new([0x0006_7305, 0x0000_0009, 0x0021_b100, 0]),
        EXIT,
    ];
    let l_f2i_ok = lower_to_maxwell(&f2i_ok, LoweringOptions::default());
    assert!(l_f2i_ok.is_ok(), "{}", l_f2i_ok.error);

    let f2i_bad = [
        Instruction::new([0x0006_7305, 0x0000_0009, 0x0020_b001, 0]),
        EXIT,
    ];
    assert!(!lower_to_maxwell(&f2i_bad, LoweringOptions::default()).is_ok());

    // LDC valid & ULDC unsupported
    let uldc_ok = [
        Instruction::new([0x0004_7ab9, 0x0180_0800, 0x0000_0400, 0x000f_e200]),
        EXIT,
    ];
    let l_uldc = lower_to_maxwell(&uldc_ok, LoweringOptions::default());
    assert!(l_uldc.is_ok(), "{}", l_uldc.error);
    assert!(l_uldc.instructions.contains(&0xef92_0060_0207_ffdc));

    let ldc_bad = [Instruction::new([0x0215_7b82, 0, 5 << 9, 0]), EXIT];
    assert!(!lower_to_maxwell(&ldc_bad, LoweringOptions::default()).is_ok());
    let ldc_bad_width = [Instruction::new([0x0214_7b82, 0, 6 << 9, 0]), EXIT];
    assert!(!lower_to_maxwell(&ldc_bad_width, LoweringOptions::default()).is_ok());

    // SURED valid & R253-R255 overflow error
    let sured_ok = [
        Instruction::new([0x0000_759f, 0xa000_2203, 0x0010_e000, 0x0011_e600]),
        EXIT,
    ];
    assert!(lower_to_maxwell(&sured_ok, LoweringOptions::default()).is_ok());

    let sured_bad = [
        Instruction::new([0xfd00_759f, 0xa000_2203, 0x0010_e000, 0x0011_e600]),
        EXIT,
    ];
    assert!(!lower_to_maxwell(&sured_bad, LoweringOptions::default()).is_ok());

    // VOTE valid & unsupported
    let vote_ok = [Instruction::new([0x0000_7886, 0, 0x038e_0100, 0]), EXIT];
    assert!(lower_to_maxwell(&vote_ok, LoweringOptions::default()).is_ok());
    let vote_bad = [Instruction::new([0x001c_7886, 0, 0x038e_0100, 0]), EXIT];
    assert!(!lower_to_maxwell(&vote_bad, LoweringOptions::default()).is_ok());

    // Geometry: ISBERD, OUT.EMIT, OUT.CUT, LEPC
    let geom = [
        Instruction::new([0x0004_734e, 0, 0, 0]),
        Instruction::new([0x0001_7923, 0, 0, 0]),
        Instruction::new([0x0002_7324, 0xff, 0x4000, 0]),
        Instruction::new([0x0203_7924, 0, 0x8000, 0]),
        EXIT,
    ];
    let l_geom = lower_to_maxwell(
        &geom,
        LoweringOptions {
            geometry: true,
            native_code_address: 0x0000_0012_ffff_fff0,
            ..Default::default()
        },
    );
    assert!(l_geom.is_ok(), "{}", l_geom.error);

    // LEA and STG
    let lea_stg = [
        Instruction::new([0x0808_7211, 0x0000_0007, 0x078f_f8ff, 0x000f_c800]),
        Instruction::new([
            0x986 | (0x7 << 12) | (254 << 24),
            0,
            0x0810_a900,
            0,
        ]),
        EXIT,
    ];
    assert!(lower_to_maxwell(&lea_stg, LoweringOptions::default()).is_ok());
}

#[test]
fn tensor_modes_lower_extended_and_high_uniform_retry() {
    let prog = [hmma_sample(), EXIT];

    // Tensor disabled -> error
    let dis = lower_to_maxwell(
        &prog,
        LoweringOptions {
            tensor: false,
            tensor_mode_override: Some(TensorModeOverride::Disabled),
            ..Default::default()
        },
    );
    assert!(!dis.is_ok());

    // Tensor Inline, Compact, Stub modes
    let inline = lower_to_maxwell(
        &prog,
        LoweringOptions {
            tensor: true,
            tensor_mode_override: Some(TensorModeOverride::Inline),
            lower_extended_override: Some(true),
            ..Default::default()
        },
    );
    assert!(inline.is_ok(), "inline err: {}", inline.error);

    let compact = lower_to_maxwell(
        &prog,
        LoweringOptions {
            tensor: true,
            compact_tensor: true,
            tensor_mode_override: Some(TensorModeOverride::Compact),
            lower_extended_override: Some(true),
            ..Default::default()
        },
    );
    assert!(compact.is_ok(), "compact err: {}", compact.error);

    let stub = lower_to_maxwell(
        &prog,
        LoweringOptions {
            tensor: true,
            tensor_mode_override: Some(TensorModeOverride::Stub),
            lower_extended_override: Some(true),
            ..Default::default()
        },
    );
    assert!(stub.is_ok(), "stub err: {}", stub.error);

    // lower_extended_override=false rejects narrow ULDC (0xab9 with width < 4)
    let uldc_narrow = [
        Instruction::new([0x0004_7ab9, 0x0180_0800, 0x0000_0400, 0x000f_e200]),
        EXIT,
    ];
    let ext_on = lower_to_maxwell(
        &uldc_narrow,
        LoweringOptions {
            lower_extended_override: Some(true),
            ..Default::default()
        },
    );
    assert!(ext_on.is_ok(), "{}", ext_on.error);
    let ext_off = lower_to_maxwell(
        &uldc_narrow,
        LoweringOptions {
            lower_extended_override: Some(false),
            ..Default::default()
        },
    );
    assert!(!ext_off.is_ok());

    // UR30 destination triggers high_uniform_registers retry loop:
    // pass 1 (high_uniforms = 0) fails reporting high_uniform_registers = 4,
    // and lower_to_maxwell retries with 4 mirrors and succeeds.
    let ulea_ur30 = Instruction::new([
        0x291 | (7 << 12) | (30 << 16) | (11 << 24),
        19,
        63 | (3 << 11) | (7 << 17) | (1 << 27),
        0,
    ]);
    let pass1 = eglantine_ampere_rs::lower_to_maxwell_with(
        &[ulea_ur30, EXIT],
        0,
        LoweringOptions::default(),
    );
    assert!(!pass1.is_ok());
    assert_eq!(pass1.high_uniform_registers, 4);
    let high_ur = lower_to_maxwell(&[ulea_ur30, EXIT], LoweringOptions::default());
    assert!(high_ur.is_ok(), "{}", high_ur.error);
}

#[test]
fn ffi_boundary_and_memory_safety() {
    unsafe {
        // Null/misaligned argument checks
        assert_eq!(
            egl_ampere_capture_observe(ptr::null_mut(), &EXIT, 0),
            EglAmpereCaptureResult::InvalidTarget
        );
        assert_eq!(egl_ampere_native_program_key(ptr::null(), 1), 0);
        assert_eq!(egl_ampere_native_program_key(ptr::null(), 0), native_program_key(&[])); // zero-count ok

        let mut cap = EglAmpereCaptureState::default();
        egl_ampere_capture_init(&mut cap);
        assert_eq!(
            egl_ampere_capture_observe(&mut cap, &EXIT, 0),
            EglAmpereCaptureResult::Complete
        );

        let src = [
            Instruction::new([0x0001_7306, 0x0000_0001, 0x0010_1400, 0x020e_a200]),
            EXIT,
        ];
        let rust_direct = lower_to_maxwell(&src, LoweringOptions::default());

        let opts = EglAmpereLoweringOptions {
            geometry: 0,
            tensor: 0,
            compact_tensor: 0,
            lower_extended_override: 1,
            _pad: [0; 4],
            native_code_address: 0,
        };
        let mut out_prog = EglAmpereLoweredProgram::default();
        assert_eq!(
            egl_ampere_lower_to_maxwell(src.as_ptr(), src.len(), &opts, &mut out_prog),
            0
        );
        assert_eq!(out_prog.error_len, 0);
        assert_eq!(out_prog.instruction_count, rust_direct.instructions.len());
        let out_slice = std::slice::from_raw_parts(out_prog.instructions, out_prog.instruction_count);
        assert_eq!(out_slice, rust_direct.instructions.as_slice());

        // Also verify empty non-null error pointer encodes successfully after FFI fix
        out_prog.error = b"".as_ptr() as *mut _;
        let mut encoded = EglAmpereBytes::default();
        assert_eq!(
            egl_ampere_encode_override(src.as_ptr(), src.len(), &out_prog, &mut encoded),
            0
        );
        out_prog.error = ptr::null_mut();
        assert_ne!(encoded.len, 0);
        assert_eq!(
            egl_ampere_override_source_size(encoded.data, encoded.len),
            32
        );

        // Address map via FFI (append native address map after lowering)
        let mut with_map = vec![0u64; 16];
        with_map.splice(0..out_slice.len(), out_slice.iter().copied());
        let off_slice = std::slice::from_raw_parts(
            out_prog.source_instruction_offsets,
            out_prog.source_instruction_offset_count,
        );
        append_native_address_map(&mut with_map, off_slice);
        let mut target = 0u32;
        assert_eq!(
            egl_ampere_has_native_address_map(with_map.as_ptr(), with_map.len()),
            1
        );
        assert_eq!(
            egl_ampere_native_program_address(
                with_map.as_ptr(),
                with_map.len(),
                16,
                &mut target
            ),
            1
        );
        let expected_idx = rust_direct.source_instruction_offsets[1];
        let expected_addr = 0x50u32 + expected_idx * 8 + ((expected_idx + 1) / 3) * 8;
        assert_eq!(target, expected_addr);

        // Decode override via FFI
        let mut decoded = EglAmpereLoweredProgram::default();
        assert_eq!(
            egl_ampere_decode_override(encoded.data, encoded.len, src.as_ptr(), src.len(), &mut decoded),
            0
        );
        assert_eq!(decoded.error_len, 0);
        assert_eq!(decoded.instruction_count, out_prog.instruction_count);

        // Key matches
        assert_eq!(
            egl_ampere_native_program_key(src.as_ptr(), src.len()),
            native_program_key(&src)
        );

        // Idempotent double-free
        egl_ampere_free_lowered(&mut out_prog);
        egl_ampere_free_lowered(&mut out_prog);
        egl_ampere_free_lowered(&mut decoded);
        egl_ampere_free_lowered(&mut decoded);
        egl_ampere_free_bytes(&mut encoded);
        egl_ampere_free_bytes(&mut encoded);
        egl_ampere_free_lowered(ptr::null_mut());
        egl_ampere_free_bytes(ptr::null_mut());
    }
}
