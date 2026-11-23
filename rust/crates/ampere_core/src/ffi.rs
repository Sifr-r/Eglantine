// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use std::ffi::c_char;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;

use crate::address_map::{has_native_address_map, native_program_address};
use crate::capture::{CaptureResult, ProgramCaptureState};
use crate::lower::lower_to_maxwell;
use crate::override_codec::{
    decode_program_override, encode_program_override, native_program_key,
    program_override_source_size,
};
use crate::types::{Instruction, LoweredProgram, LoweringOptions};

pub type EglAmpereInstruction = Instruction;

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EglAmpereCaptureResult {
    Continue = 0,
    Complete = 1,
    InvalidTarget = 2,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct EglAmpereCaptureState {
    pub exit_setup: [EglAmpereInstruction; 5],
    pub exit_handler_end: u32,
    pub last_call_target: u32,
    pub last_forward_target: u32,
    pub main_exited: u8,
    pub computed_branch: u8,
    pub _pad: [u8; 2],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct EglAmpereLoweringOptions {
    pub geometry: u8,
    pub tensor: u8,
    pub compact_tensor: u8,
    pub lower_extended_override: i8,
    pub _pad: [u8; 4],
    pub native_code_address: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct EglAmpereLoweredProgram {
    pub instructions: *mut u64,
    pub instruction_count: usize,
    pub source_instruction_offsets: *mut u32,
    pub source_instruction_offset_count: usize,
    pub source_size: u32,
    pub required_texture_buffer: u32,
    pub high_uniform_registers: u32,
    pub error_offset: u32,
    pub error: *mut c_char,
    pub error_len: usize,
}

impl Default for EglAmpereLoweredProgram {
    fn default() -> Self {
        Self {
            instructions: ptr::null_mut(),
            instruction_count: 0,
            source_instruction_offsets: ptr::null_mut(),
            source_instruction_offset_count: 0,
            source_size: 0,
            required_texture_buffer: 0,
            high_uniform_registers: 0,
            error_offset: 0,
            error: ptr::null_mut(),
            error_len: 0,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct EglAmpereBytes {
    pub data: *mut u8,
    pub len: usize,
}

impl Default for EglAmpereBytes {
    fn default() -> Self {
        Self {
            data: ptr::null_mut(),
            len: 0,
        }
    }
}

unsafe fn slice_from_raw<'a, T>(ptr: *const T, count: usize) -> Option<&'a [T]> {
    if count == 0 {
        return Some(&[]);
    }
    if ptr.is_null() || !ptr.is_aligned() {
        return None;
    }
    let elem_size = std::mem::size_of::<T>();
    if elem_size > 0 && count > (isize::MAX as usize) / elem_size {
        return None;
    }
    Some(std::slice::from_raw_parts(ptr, count))
}

fn vec_into_raw_parts<T>(vec: Vec<T>) -> (*mut T, usize) {
    if vec.is_empty() {
        (ptr::null_mut(), 0)
    } else {
        let boxed = vec.into_boxed_slice();
        let len = boxed.len();
        (Box::into_raw(boxed) as *mut T, len)
    }
}

unsafe fn free_raw_slice<T>(ptr: *mut T, len: usize) {
    if !ptr.is_null() && len > 0 {
        drop(Box::from_raw(ptr::slice_from_raw_parts_mut(ptr, len)));
    }
}

unsafe fn write_lowered(out_program: *mut EglAmpereLoweredProgram, lowered: LoweredProgram) {
    let (instructions, instruction_count) = vec_into_raw_parts(lowered.instructions);
    let (source_instruction_offsets, source_instruction_offset_count) =
        vec_into_raw_parts(lowered.source_instruction_offsets);
    let error_vec: Vec<c_char> = lowered
        .error
        .into_bytes()
        .into_iter()
        .map(|b| b as c_char)
        .collect();
    let (error, error_len) = vec_into_raw_parts(error_vec);
    ptr::write(
        out_program,
        EglAmpereLoweredProgram {
            instructions,
            instruction_count,
            source_instruction_offsets,
            source_instruction_offset_count,
            source_size: lowered.source_size,
            required_texture_buffer: lowered.required_texture_buffer,
            high_uniform_registers: lowered.high_uniform_registers,
            error_offset: lowered.error_offset,
            error,
            error_len,
        },
    );
}

unsafe fn write_bytes(out_bytes: *mut EglAmpereBytes, bytes: Vec<u8>) {
    let (data, len) = vec_into_raw_parts(bytes);
    ptr::write(out_bytes, EglAmpereBytes { data, len });
}

#[no_mangle]
pub extern "C" fn egl_ampere_capture_init(state: *mut EglAmpereCaptureState) {
    if state.is_null() || !state.is_aligned() {
        return;
    }
    unsafe {
        ptr::write(state, EglAmpereCaptureState::default());
    }
}

#[no_mangle]
pub extern "C" fn egl_ampere_capture_observe(
    state: *mut EglAmpereCaptureState,
    insn: *const EglAmpereInstruction,
    index: u32,
) -> EglAmpereCaptureResult {
    if state.is_null() || !state.is_aligned() || insn.is_null() || !insn.is_aligned() {
        return EglAmpereCaptureResult::InvalidTarget;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let state_ref = unsafe { &mut *state };
        let insn_ref = unsafe { &*insn };
        let mut capture = ProgramCaptureState {
            exit_setup: state_ref.exit_setup,
            exit_handler_end: state_ref.exit_handler_end,
            last_call_target: state_ref.last_call_target,
            last_forward_target: state_ref.last_forward_target,
            main_exited: state_ref.main_exited != 0,
            computed_branch: state_ref.computed_branch != 0,
        };
        let observed = capture.observe(insn_ref, index);
        state_ref.exit_setup = capture.exit_setup;
        state_ref.exit_handler_end = capture.exit_handler_end;
        state_ref.last_call_target = capture.last_call_target;
        state_ref.last_forward_target = capture.last_forward_target;
        state_ref.main_exited = u8::from(capture.main_exited);
        state_ref.computed_branch = u8::from(capture.computed_branch);
        state_ref._pad = [0; 2];
        match observed {
            CaptureResult::Continue => EglAmpereCaptureResult::Continue,
            CaptureResult::Complete => EglAmpereCaptureResult::Complete,
            CaptureResult::InvalidTarget => EglAmpereCaptureResult::InvalidTarget,
        }
    }));
    result.unwrap_or(EglAmpereCaptureResult::InvalidTarget)
}

#[no_mangle]
pub extern "C" fn egl_ampere_native_program_key(
    source: *const EglAmpereInstruction,
    count: usize,
) -> u64 {
    let result = catch_unwind(AssertUnwindSafe(|| {
        let Some(slice) = (unsafe { slice_from_raw(source, count) }) else {
            return 0;
        };
        native_program_key(slice)
    }));
    result.unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn egl_ampere_has_native_address_map(code: *const u64, count: usize) -> u8 {
    let result = catch_unwind(AssertUnwindSafe(|| {
        let Some(slice) = (unsafe { slice_from_raw(code, count) }) else {
            return 0;
        };
        u8::from(has_native_address_map(slice))
    }));
    result.unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn egl_ampere_native_program_address(
    code: *const u64,
    count: usize,
    address: u32,
    out_target: *mut u32,
) -> u8 {
    if out_target.is_null() || !out_target.is_aligned() {
        return 0;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let Some(slice) = (unsafe { slice_from_raw(code, count) }) else {
            return 0;
        };
        match native_program_address(slice, address) {
            Some(target) => {
                unsafe {
                    ptr::write(out_target, target);
                }
                1
            }
            None => 0,
        }
    }));
    result.unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn egl_ampere_override_source_size(bytes: *const u8, len: usize) -> u32 {
    let result = catch_unwind(AssertUnwindSafe(|| {
        let Some(slice) = (unsafe { slice_from_raw(bytes, len) }) else {
            return 0;
        };
        program_override_source_size(slice)
    }));
    result.unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn egl_ampere_encode_override(
    source: *const EglAmpereInstruction,
    source_count: usize,
    program: *const EglAmpereLoweredProgram,
    out_bytes: *mut EglAmpereBytes,
) -> i32 {
    if out_bytes.is_null() || !out_bytes.is_aligned() {
        return -1;
    }
    if program.is_null() || !program.is_aligned() {
        return -1;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let Some(source_slice) = (unsafe { slice_from_raw(source, source_count) }) else {
            return -1;
        };
        let prog = unsafe { &*program };
        let Some(instructions) = (unsafe { slice_from_raw(prog.instructions, prog.instruction_count) }) else {
            return -1;
        };
        let Some(offsets) = (unsafe {
            slice_from_raw(
                prog.source_instruction_offsets,
                prog.source_instruction_offset_count,
            )
        }) else {
            return -1;
        };
        if prog.error_len > 0 && prog.error.is_null() {
            return -1;
        }
        if prog.error_len != 0 {
            unsafe {
                write_bytes(out_bytes, Vec::new());
            }
            return 0;
        }
        let lowered = LoweredProgram {
            instructions: instructions.to_vec(),
            source_instruction_offsets: offsets.to_vec(),
            source_size: prog.source_size,
            required_texture_buffer: prog.required_texture_buffer,
            high_uniform_registers: prog.high_uniform_registers,
            error_offset: prog.error_offset,
            error: String::new(),
        };
        let encoded = encode_program_override(source_slice, &lowered);
        unsafe {
            write_bytes(out_bytes, encoded);
        }
        0
    }));
    result.unwrap_or(-1)
}

#[no_mangle]
pub extern "C" fn egl_ampere_decode_override(
    bytes: *const u8,
    len: usize,
    source: *const EglAmpereInstruction,
    source_count: usize,
    out_program: *mut EglAmpereLoweredProgram,
) -> i32 {
    if out_program.is_null() || !out_program.is_aligned() {
        return -1;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let Some(byte_slice) = (unsafe { slice_from_raw(bytes, len) }) else {
            return -1;
        };
        let Some(source_slice) = (unsafe { slice_from_raw(source, source_count) }) else {
            return -1;
        };
        let decoded = decode_program_override(byte_slice, source_slice);
        unsafe {
            write_lowered(out_program, decoded);
        }
        0
    }));
    result.unwrap_or(-1)
}

#[no_mangle]
pub extern "C" fn egl_ampere_lower_to_maxwell(
    code: *const EglAmpereInstruction,
    count: usize,
    options: *const EglAmpereLoweringOptions,
    out_program: *mut EglAmpereLoweredProgram,
) -> i32 {
    if out_program.is_null() || !out_program.is_aligned() {
        return -1;
    }
    if !options.is_null() && !options.is_aligned() {
        return -1;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let Some(code_slice) = (unsafe { slice_from_raw(code, count) }) else {
            return -1;
        };
        let rust_options = if options.is_null() {
            LoweringOptions::default()
        } else {
            let raw = unsafe { &*options };
            let lower_extended_override = if raw.lower_extended_override < 0 {
                None
            } else if raw.lower_extended_override == 0 {
                Some(false)
            } else {
                Some(true)
            };
            LoweringOptions {
                geometry: raw.geometry != 0,
                tensor: raw.tensor != 0,
                compact_tensor: raw.compact_tensor != 0,
                native_code_address: raw.native_code_address,
                lower_extended_override,
                tensor_mode_override: None,
            }
        };
        let lowered = lower_to_maxwell(code_slice, rust_options);
        unsafe {
            write_lowered(out_program, lowered);
        }
        0
    }));
    result.unwrap_or(-1)
}

#[no_mangle]
pub extern "C" fn egl_ampere_free_lowered(program: *mut EglAmpereLoweredProgram) {
    if program.is_null() || !program.is_aligned() {
        return;
    }
    unsafe {
        let prog = &mut *program;
        free_raw_slice(prog.instructions, prog.instruction_count);
        free_raw_slice(
            prog.source_instruction_offsets,
            prog.source_instruction_offset_count,
        );
        free_raw_slice(prog.error, prog.error_len);
        *prog = EglAmpereLoweredProgram::default();
    }
}

#[no_mangle]
pub extern "C" fn egl_ampere_free_bytes(bytes: *mut EglAmpereBytes) {
    if bytes.is_null() || !bytes.is_aligned() {
        return;
    }
    unsafe {
        let b = &mut *bytes;
        free_raw_slice(b.data, b.len);
        *b = EglAmpereBytes::default();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::{align_of, offset_of, size_of};

    #[test]
    fn capture_state_ffi_layout_and_roundtrip() {
        assert_eq!(size_of::<EglAmpereCaptureState>(), 96);
        assert_eq!(align_of::<EglAmpereCaptureState>(), 4);
        assert_eq!(offset_of!(EglAmpereCaptureState, exit_setup), 0);
        assert_eq!(offset_of!(EglAmpereCaptureState, exit_handler_end), 80);
        assert_eq!(offset_of!(EglAmpereCaptureState, last_call_target), 84);
        assert_eq!(offset_of!(EglAmpereCaptureState, last_forward_target), 88);
        assert_eq!(offset_of!(EglAmpereCaptureState, main_exited), 92);
        assert_eq!(offset_of!(EglAmpereCaptureState, computed_branch), 93);
        assert_eq!(offset_of!(EglAmpereCaptureState, _pad), 94);

        let mut state = EglAmpereCaptureState::default();
        egl_ampere_capture_init(&mut state);
        let insns = [
            Instruction::new([0x0000_734e, 0, 0, 0x000f_e400]),
            Instruction::new([0x0003_7802, 0x70, 0x0000_0f00, 0x000f_c800]),
            Instruction::new([0x0000_7210, 3, 0x07f1_e0ff, 0x000f_c800]),
            Instruction::new([0x0101_7210, 0xff, 0x007f_e4ff, 0x000f_c800]),
            Instruction::new([0x0000_7357, 0, 0, 0x0001_e400]),
            Instruction::new([0x0000_894d, 0, 0x0380_0000, 0x021f_ea00]),
            Instruction::new([0x0000_794d, 0, 0x0380_0000, 0x003f_ea00]),
            Instruction::new([0x00ff_7324, 0xff, 0, 0x0001_e200]),
            Instruction::new([0x0000_7957, 0, 0, 0x000f_e200]),
            Instruction::new([0x0000_794d, 0, 0x0380_0000, 0x001f_ea00]),
        ];
        for (index, insn) in insns.iter().enumerate() {
            let expected = if index + 1 == insns.len() {
                EglAmpereCaptureResult::Complete
            } else {
                EglAmpereCaptureResult::Continue
            };
            assert_eq!(
                egl_ampere_capture_observe(&mut state, insn, index as u32),
                expected
            );
        }
        assert_eq!(state.exit_handler_end, 9);
        assert_eq!(state.last_forward_target, 7);
        assert_eq!(state.exit_setup, insns[..5]);
    }
}
