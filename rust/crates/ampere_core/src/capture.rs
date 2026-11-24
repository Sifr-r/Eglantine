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
    pub exit_setup: [Instruction; 5],
    pub exit_handler_end: u32,
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
            exit_setup: [Instruction::new([0; 4]); 5],
            exit_handler_end: 0,
            last_call_target: 0,
            last_forward_target: 0,
            main_exited: false,
            computed_branch: false,
        }
    }

    #[inline]
    fn is_geometry_exit_setup(&self) -> bool {
        self.exit_setup
            .iter()
            .all(|setup| (setup.words[3] & 0xc000_01ffu32) == 0)
            && self.exit_setup[0].words[0] == 0x0000_734e
            && self.exit_setup[0].words[1] == 0
            && self.exit_setup[0].words[2] == 0
            && self.exit_setup[1].words[0] == 0x0003_7802
            && self.exit_setup[1].words[2] == 0xf00
            && self.exit_setup[2].words[0] == 0x0000_7210
            && self.exit_setup[2].words[1] == 3
            && self.exit_setup[2].words[2] == 0x07f1_e0ff
            && self.exit_setup[3].words[1] == 0xff
            && ((self.exit_setup[3].words[0] == 0x0101_7210
                && self.exit_setup[3].words[2] == 0x007f_e4ff)
                || (self.exit_setup[3].words[0] == 0xff01_7224
                    && self.exit_setup[3].words[2] == 0x000e_0401))
            && self.exit_setup[4].words[0] == 0x0000_7357
            && self.exit_setup[4].words[1] == 0
            && self.exit_setup[4].words[2] == 0
    }

    pub fn observe(&mut self, instruction: &Instruction, index: u32) -> CaptureResult {
        let opcode = instruction.bits(0, 12);
        let setup_len = self.exit_setup.len() as u32;
        if index < setup_len {
            self.exit_setup[index as usize] = *instruction;
            if index + 1 == setup_len && self.is_geometry_exit_setup() {
                let target = self.exit_setup[1].words[1];
                if target < setup_len * 16
                    || (target % 16) != 0
                    || target >= Self::MAXIMUM_INSTRUCTIONS * 16
                {
                    return CaptureResult::InvalidTarget;
                }
                self.last_forward_target = self.last_forward_target.max(target / 16);
                self.exit_handler_end = target / 16 + 2;
            }
        }
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
        if opcode == 0x94d && instruction.bits(12, 4) == 7 && index == self.exit_handler_end {
            return CaptureResult::Complete;
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

#[cfg(test)]
mod tests {
    use super::*;

    const EXIT_HANDLER_PROGRAM: [Instruction; 10] = [
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

    fn bananza_exit_handler_program() -> [Instruction; 0x740 / 16] {
        let mut code = [Instruction::new([0x0000_7918, 0, 0, 0x000f_c000]); 0x740 / 16];
        code[..5].copy_from_slice(&EXIT_HANDLER_PROGRAM[..5]);
        code[1].words[1] = 0x710;
        code[2].words[3] = 0x000f_ca00;
        code[3] = Instruction::new([0xff01_7224, 0xff, 0x000e_0401, 0x000f_c800]);
        code[0x6e0 / 16] = EXIT_HANDLER_PROGRAM[6];
        code[0x6f0 / 16] = Instruction::new([0xff00_7322, 0x0003_a0ff, 0, 0x0001_e400]);
        code[0x700 / 16] = EXIT_HANDLER_PROGRAM[6];
        code[0x710 / 16..0x710 / 16 + 3].copy_from_slice(&EXIT_HANDLER_PROGRAM[7..10]);
        code
    }

    #[test]
    fn static_geometry_exit_handler_capture() {
        let mut capture = ProgramCaptureState::new();
        for (index, insn) in EXIT_HANDLER_PROGRAM.iter().enumerate() {
            let expected = if index + 1 == EXIT_HANDLER_PROGRAM.len() {
                CaptureResult::Complete
            } else {
                CaptureResult::Continue
            };
            assert_eq!(capture.observe(insn, index as u32), expected);
        }
        assert_eq!(capture.exit_handler_end, 9);
    }

    #[test]
    fn imad_bananza_exit_handler_capture() {
        let code = bananza_exit_handler_program();
        let mut capture = ProgramCaptureState::new();
        for (index, insn) in code.iter().enumerate() {
            let expected = if index + 1 == code.len() {
                CaptureResult::Complete
            } else {
                CaptureResult::Continue
            };
            assert_eq!(capture.observe(insn, index as u32), expected);
        }
        assert_eq!(capture.exit_handler_end, 0x730 / 16);
    }

    #[test]
    fn exit_setup_exact_carry_and_form_validation() {
        for word in 0..3 {
            for bit in 0..32 {
                let mut code = bananza_exit_handler_program();
                code[3].words[word] ^= 1u32 << bit;
                let mut capture = ProgramCaptureState::new();
                for (index, insn) in code.iter().take(0x6e0 / 16 + 1).enumerate() {
                    let expected = if index == 0x6e0 / 16 {
                        CaptureResult::Complete
                    } else {
                        CaptureResult::Continue
                    };
                    assert_eq!(capture.observe(insn, index as u32), expected);
                }
                assert_eq!(capture.exit_handler_end, 0);
            }
        }

        for setup in 0..5 {
            let mut code = EXIT_HANDLER_PROGRAM;
            code[setup].words[0] ^= 1u32 << 12;
            let mut capture = ProgramCaptureState::new();
            for (index, insn) in code.iter().take(7).enumerate() {
                let expected = if index == 6 {
                    CaptureResult::Complete
                } else {
                    CaptureResult::Continue
                };
                assert_eq!(capture.observe(insn, index as u32), expected);
            }
            assert_eq!(capture.exit_handler_end, 0);
        }
    }

    #[test]
    fn exit_setup_bounds_destination_and_modes() {
        for target in [
            0u32,
            0x40u32,
            0x71u32,
            0x711u32,
            ProgramCaptureState::MAXIMUM_INSTRUCTIONS * 16,
        ] {
            let mut code = EXIT_HANDLER_PROGRAM;
            code[1].words[1] = target;
            let mut capture = ProgramCaptureState::new();
            for (index, insn) in code.iter().take(4).enumerate() {
                assert_eq!(capture.observe(insn, index as u32), CaptureResult::Continue);
            }
            assert_eq!(capture.observe(&code[4], 4), CaptureResult::InvalidTarget);
        }

        for setup in 0..5 {
            for bit in [0u32, 1, 2, 3, 4, 5, 6, 7, 8, 30, 31] {
                let mut code = bananza_exit_handler_program();
                code[setup].words[3] ^= 1u32 << bit;
                let mut capture = ProgramCaptureState::new();
                for (index, insn) in code.iter().take(0x6e0 / 16 + 1).enumerate() {
                    let expected = if index == 0x6e0 / 16 {
                        CaptureResult::Complete
                    } else {
                        CaptureResult::Continue
                    };
                    assert_eq!(capture.observe(insn, index as u32), expected);
                }
            }
            for bit in 9..30 {
                let mut code = bananza_exit_handler_program();
                code[setup].words[3] ^= 1u32 << bit;
                let mut capture = ProgramCaptureState::new();
                for (index, insn) in code.iter().enumerate() {
                    let expected = if index + 1 == code.len() {
                        CaptureResult::Complete
                    } else {
                        CaptureResult::Continue
                    };
                    assert_eq!(capture.observe(insn, index as u32), expected);
                }
            }
        }
    }

    #[test]
    fn geometry_exit_capture_ends_at_handler_behind_body_call_and_brx() {
        let mut code = [Instruction::new([0x0000_7918, 0, 0, 0x000f_c000]); 19];
        code[..5].copy_from_slice(&EXIT_HANDLER_PROGRAM[..5]);
        code[1].words[1] = 0xe0;
        code[5] = Instruction::new([0x0000_7944, 0x10, 0x03c0_0000, 0x000f_ea00]);
        code[6] = EXIT_HANDLER_PROGRAM[6];
        code[7] = Instruction::new([0x0800_7950, 0xffff_ff80, 0x03c3_ffff, 0x000f_ea00]);
        code[8] = EXIT_HANDLER_PROGRAM[6];
        code[14..17].copy_from_slice(&EXIT_HANDLER_PROGRAM[7..10]);
        code[17] = Instruction::new([0x0000_7947, 0xffff_fff0, 0x0383_ffff, 0x000f_c000]);
        code[18] = Instruction::new([0x0000_734e, 0, 0, 0x000f_e400]);

        let mut capture = ProgramCaptureState::new();
        for (index, insn) in code.iter().take(17).enumerate() {
            let expected = if index == 16 {
                CaptureResult::Complete
            } else {
                CaptureResult::Continue
            };
            assert_eq!(capture.observe(insn, index as u32), expected);
        }

        // Handler EXIT still terminates capture even if BRX or a forward target appeared in body.
        let mut brx_code = code;
        brx_code[9] = Instruction::new([0x0000_7949, 0, 0, 0]);
        brx_code[10] = Instruction::new([0x0000_7947, 0x90, 0x0380_0000, 0]);
        let mut brx_capture = ProgramCaptureState::new();
        for (index, insn) in brx_code.iter().take(17).enumerate() {
            let expected = if index == 16 {
                CaptureResult::Complete
            } else {
                CaptureResult::Continue
            };
            assert_eq!(brx_capture.observe(insn, index as u32), expected);
        }
        assert!(brx_capture.computed_branch);
        assert_eq!(brx_capture.exit_handler_end, 16);
    }
}
