// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use eglantine_ds::{
    Access, Cpu, ExecutionError, LoadError, Machine, MemoryError, RunOutcome, StopReason,
};

const ARM9_BASE: u32 = 0x0200_0000;
const ARM7_BASE: u32 = 0x0200_0100;
const ARM9_RESULT: u32 = 0x0200_0200;
const ARM7_RESULT: u32 = 0x0200_0204;
const ARM7_PRIVATE: u32 = 0x0380_0000;
const ARM9_FILE: usize = 0x200;
const ARM7_FILE: usize = 0x240;

fn write_word(image: &mut [u8], offset: usize, word: u32) {
    image[offset..offset + 4].copy_from_slice(&word.to_le_bytes());
}

fn fixture() -> Vec<u8> {
    let arm9 = [
        0xe59f_100c, // ldr r1, [pc, #12]
        0xe3a0_0029, // mov r0, #41
        0xe581_0000, // str r0, [r1]
        0xe3a0_0007, // mov r0, #7
        0xeaff_fffe, // b .
        ARM9_RESULT,
    ];
    let arm7 = [
        0xe59f_100c, // ldr r1, [pc, #12]
        0xe3a0_0035, // mov r0, #53
        0xe581_0000, // str r0, [r1]
        0xeaff_fffe, // b .
        0,
        ARM7_RESULT,
    ];
    let mut image = vec![0; ARM7_FILE + arm7.len() * 4];
    for (header, file_offset, base, program) in [
        (0x20, ARM9_FILE, ARM9_BASE, arm9),
        (0x30, ARM7_FILE, ARM7_BASE, arm7),
    ] {
        write_word(&mut image, header, file_offset as u32);
        write_word(&mut image, header + 4, base);
        write_word(&mut image, header + 8, base);
        write_word(&mut image, header + 12, 24);
        for (index, word) in program.into_iter().enumerate() {
            write_word(&mut image, file_offset + index * 4, word);
        }
    }
    image
}

#[test]
fn both_cpus_retire_distinct_instructions_and_write_shared_ram() {
    let mut machine = Machine::load(&fixture()).unwrap();
    assert_eq!(machine.pc(Cpu::Arm9), ARM9_BASE);
    assert_eq!(machine.pc(Cpu::Arm7), ARM7_BASE);
    assert_eq!(machine.register(Cpu::Arm7, 16), None);
    assert_eq!(machine.read_word(Cpu::Arm9, ARM9_RESULT), Ok(0));
    assert_eq!(machine.read_word(Cpu::Arm7, ARM7_RESULT), Ok(0));

    let result = machine.run_until(4, |state| state.read_word(Cpu::Arm9, ARM7_RESULT) == Ok(53));
    assert_eq!(
        result,
        Ok(RunOutcome {
            steps: 3,
            reason: StopReason::ConditionMet
        })
    );
    assert_eq!(machine.read_word(Cpu::Arm7, ARM9_RESULT), Ok(41));
    assert_eq!(machine.register(Cpu::Arm9, 0), Some(41));
    assert_eq!(machine.register(Cpu::Arm7, 0), Some(53));
    assert_eq!(machine.pc(Cpu::Arm9), ARM9_BASE + 12);
    assert_eq!(machine.pc(Cpu::Arm7), ARM7_BASE + 12);
}

#[test]
fn arm7_can_boot_and_store_in_private_ram() {
    let mut image = fixture();
    write_word(&mut image, 0x34, ARM7_PRIVATE);
    write_word(&mut image, 0x38, ARM7_PRIVATE);
    write_word(&mut image, ARM7_FILE + 20, ARM7_PRIVATE + 0xfffc);
    let mut machine = Machine::load(&image).unwrap();

    assert_eq!(machine.pc(Cpu::Arm7), ARM7_PRIVATE);
    assert_eq!(machine.read_word(Cpu::Arm7, ARM7_PRIVATE), Ok(0xe59f_100c));
    assert_eq!(
        machine.read_word(Cpu::Arm9, ARM7_PRIVATE),
        Err(MemoryError::UnmappedWord {
            address: ARM7_PRIVATE
        })
    );
    for _ in 0..3 {
        machine.step().unwrap();
    }
    assert_eq!(machine.read_word(Cpu::Arm7, ARM7_PRIVATE + 0xfffc), Ok(53));
    assert_eq!(
        machine.read_word(Cpu::Arm7, ARM7_PRIVATE + 0x1_0000),
        Err(MemoryError::UnmappedWord {
            address: ARM7_PRIVATE + 0x1_0000
        })
    );
}

#[test]
fn rejects_overlapping_main_ram_destinations() {
    let mut image = fixture();
    write_word(&mut image, 0x34, ARM9_BASE + 20);
    write_word(&mut image, 0x38, ARM9_BASE + 20);
    assert!(matches!(
        Machine::load(&image),
        Err(LoadError::OverlappingPayloads)
    ));

    write_word(&mut image, 0x34, ARM9_BASE + 24);
    write_word(&mut image, 0x38, ARM9_BASE + 24);
    assert!(Machine::load(&image).is_ok());
}

#[test]
fn rejects_missing_header_and_invalid_file_ranges() {
    let image = fixture();
    assert!(matches!(
        Machine::load(&image[..0x3f]),
        Err(LoadError::TruncatedHeader { .. })
    ));
    assert!(matches!(
        Machine::load(&image[..image.len() - 1]),
        Err(LoadError::InvalidPayloadFileRange { cpu: Cpu::Arm7, .. })
    ));

    for (offset, size) in [(u32::MAX - 3, 24), (0x3c, 24), (ARM7_FILE as u32, 0)] {
        let mut image = fixture();
        write_word(&mut image, 0x30, offset);
        write_word(&mut image, 0x3c, size);
        assert!(matches!(
            Machine::load(&image),
            Err(LoadError::InvalidPayloadFileRange { cpu: Cpu::Arm7, .. })
        ));
    }
}

#[test]
fn rejects_invalid_guest_payload_ranges() {
    for address in [0x01ff_fffc, 0x023f_fffc, ARM7_PRIVATE, 0xffff_fffc] {
        let mut image = fixture();
        write_word(&mut image, 0x24, address);
        write_word(&mut image, 0x28, address);
        assert!(matches!(
            Machine::load(&image),
            Err(LoadError::InvalidPayloadAddress { cpu: Cpu::Arm9, .. })
        ));
    }
}

#[test]
fn requires_an_aligned_entry_with_a_full_instruction_inside_its_payload() {
    for entry in [ARM7_BASE - 4, ARM7_BASE + 1, ARM7_BASE + 24, ARM9_BASE] {
        let mut image = fixture();
        write_word(&mut image, 0x34, entry);
        assert!(
            matches!(Machine::load(&image), Err(LoadError::InvalidEntryPoint { cpu: Cpu::Arm7, entry: observed, .. }) if observed == entry)
        );
    }

    let mut image = fixture();
    write_word(&mut image, 0x34, ARM7_BASE + 20);
    write_word(&mut image, 0x3c, 23);
    assert!(matches!(
        Machine::load(&image),
        Err(LoadError::InvalidEntryPoint { cpu: Cpu::Arm7, .. })
    ));
}

#[test]
fn arm7_fault_preserves_the_arm9_retirement_and_latches_the_failed_round() {
    let mut image = fixture();
    write_word(&mut image, ARM7_FILE + 8, 0xef00_0000);
    let mut machine = Machine::load(&image).unwrap();

    let error = ExecutionError::UnsupportedInstruction {
        cpu: Cpu::Arm7,
        pc: ARM7_BASE + 8,
        opcode: 0xef00_0000,
    };
    assert_eq!(machine.run_until(4, |_| false), Err(error));
    assert_eq!(machine.pc(Cpu::Arm7), ARM7_BASE + 8);
    assert_eq!(machine.pc(Cpu::Arm9), ARM9_BASE + 12);
    assert_eq!(machine.read_word(Cpu::Arm7, ARM9_RESULT), Ok(41));
    assert_eq!(machine.read_word(Cpu::Arm9, ARM7_RESULT), Ok(0));

    assert_eq!(machine.step(), Err(error));
    assert_eq!(machine.run_until(0, |_| true), Err(error));
    assert_eq!(machine.pc(Cpu::Arm9), ARM9_BASE + 12);
    assert_eq!(machine.register(Cpu::Arm9, 0), Some(41));
}

#[test]
fn reports_address_faults_with_instruction_and_access_context() {
    let mut image = fixture();
    write_word(&mut image, ARM9_FILE + 20, ARM9_RESULT + 1);
    let mut machine = Machine::load(&image).unwrap();
    assert_eq!(
        machine.read_word(Cpu::Arm9, ARM9_RESULT + 1),
        Err(MemoryError::UnalignedWord {
            address: ARM9_RESULT + 1
        })
    );
    assert_eq!(
        machine.run_until(5, |_| false),
        Err(ExecutionError::Memory {
            cpu: Cpu::Arm9,
            pc: ARM9_BASE + 8,
            access: Access::Write,
            source: MemoryError::UnalignedWord {
                address: ARM9_RESULT + 1
            },
        })
    );
    assert_eq!(machine.pc(Cpu::Arm7), ARM7_BASE + 8);
    assert_eq!(machine.read_word(Cpu::Arm9, ARM9_RESULT), Ok(0));
}

#[test]
fn reports_a_fetch_fault_after_a_branch_out_of_main_ram() {
    const LAST_WORD: u32 = 0x023f_fffc;
    let mut image = fixture();
    write_word(&mut image, 0x24, LAST_WORD);
    write_word(&mut image, 0x28, LAST_WORD);
    write_word(&mut image, 0x2c, 4);
    write_word(&mut image, ARM9_FILE, 0xea00_0000); // b . + 8
    let mut machine = Machine::load(&image).unwrap();
    machine.step().unwrap();

    assert_eq!(
        machine.step(),
        Err(ExecutionError::Memory {
            cpu: Cpu::Arm9,
            pc: 0x0240_0004,
            access: Access::Fetch,
            source: MemoryError::UnmappedWord {
                address: 0x0240_0004
            },
        })
    );
    assert_eq!(machine.pc(Cpu::Arm7), ARM7_BASE + 4);
}

#[test]
fn bounded_run_checks_initial_state_and_counts_only_completed_rounds() {
    let mut machine = Machine::load(&fixture()).unwrap();
    assert_eq!(
        machine.run_until(0, |state| state.pc(Cpu::Arm9) == ARM9_BASE),
        Ok(RunOutcome {
            steps: 0,
            reason: StopReason::ConditionMet
        })
    );
    assert_eq!(
        machine.run_until(0, |_| false),
        Ok(RunOutcome {
            steps: 0,
            reason: StopReason::BudgetExhausted
        })
    );

    let mut observations = Vec::new();
    let outcome = machine.run_until(20, |state| {
        observations.push(state.pc(Cpu::Arm7));
        false
    });
    assert_eq!(
        outcome,
        Ok(RunOutcome {
            steps: 20,
            reason: StopReason::BudgetExhausted
        })
    );
    assert_eq!(
        &observations[..4],
        &[ARM7_BASE, ARM7_BASE + 4, ARM7_BASE + 8, ARM7_BASE + 12]
    );
    assert_eq!(observations.len(), 21);
    assert_eq!(machine.pc(Cpu::Arm9), ARM9_BASE + 16);
    assert_eq!(machine.pc(Cpu::Arm7), ARM7_BASE + 12);
}
