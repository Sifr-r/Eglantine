// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use crate::types::Instruction;

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CaptureResult {
    Continue = 0,
    Complete = 1,
    InvalidTarget = 2,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ProgramCaptureState {
    pub last_call_target: u32,
    pub last_forward_target: u32,
    pub main_exited: bool,
    pub computed_branch: bool,
}

impl ProgramCaptureState {
    pub const MAXIMUM_INSTRUCTIONS: u32 = 65536;

    #[inline]
    pub const fn new() -> Self {
        Self {
            last_call_target: 0,
            last_forward_target: 0,
            main_exited: false,
            computed_branch: false,
        }
    }

    pub fn observe(&mut self, instruction: &Instruction, index: u32) -> CaptureResult {
        let opcode = instruction.bits(0, 12);
        if opcode == 0x944 || opcode == 0x947 || opcode == 0x945 {
            // BRA keeps its .DIV/.CONV mode in the offset's two low bits.
            let low = instruction.words[1] & if opcode == 0x947 { !3u32 } else { !0u32 };
            let raw = (low as u64) | (((instruction.words[2] & 0x3_ffff) as u64) << 32);
            let relative = if opcode == 0x945 {
                (instruction.words[1] as i32) as i64
            } else {
                ((raw << 14) as i64) >> 14
            };
            let target = (index as i64) * 16 + 16 + relative;
            if target < 0
                || (target % 16) != 0
                || target >= (Self::MAXIMUM_INSTRUCTIONS as i64) * 16
            {
                return CaptureResult::InvalidTarget;
            }
            // CALL.REL.NOINC (bit 86) pushes no return address, but its callee still returns
            // with RET.REL.NODEC through the link register set ahead of the call.
            if opcode == 0x944 {
                self.last_call_target = self.last_call_target.max((target / 16) as u32);
            } else {
                self.last_forward_target = self.last_forward_target.max((target / 16) as u32);
            }
            // The compiler ends every program with an unconditional branch to itself; with a jump
            // table (whose targets live in a constant buffer) that is the only reliable end.
            if opcode == 0x947
                && self.computed_branch
                && instruction.bits(12, 4) == 7
                && target == (index as i64) * 16
            {
                return CaptureResult::Complete;
            }
        }
        if opcode == 0x949 || opcode == 0x958 {
            self.computed_branch = true;
        }
        if self.computed_branch {
            return CaptureResult::Continue;
        }
        if opcode == 0x94d && instruction.bits(12, 4) == 7 && index >= self.last_forward_target {
            if index >= self.last_call_target {
                return CaptureResult::Complete;
            }
            self.main_exited = true;
        }
        if opcode == 0x950
            && instruction.bits(12, 4) == 7
            && self.main_exited
            && index >= self.last_call_target
            && index >= self.last_forward_target
        {
            return CaptureResult::Complete;
        }
        CaptureResult::Continue
    }
}
