// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use eglantine_ctr::{
    Access, ExecutionError, LoadError, Machine, MemoryError, MemoryErrorKind, RunOutcome, Segment,
    StopReason,
};

const CODE: u32 = 0x0010_0000;
const RODATA: u32 = 0x0010_1000;
const DATA: u32 = 0x0010_2000;

fn fixture(code: &[u32], rodata: &[u8], data: &[u8], bss: u32) -> Vec<u8> {
    let mut image = b"3DSX".to_vec();
    image.extend(32_u16.to_le_bytes());
    image.extend(8_u16.to_le_bytes());
    for word in [
        0,
        0,
        u32::try_from(code.len() * 4).unwrap(),
        u32::try_from(rodata.len()).unwrap(),
        u32::try_from(data.len()).unwrap() + bss,
        bss,
    ] {
        image.extend(word.to_le_bytes());
    }
    image.extend([0; 24]);
    for word in code {
        image.extend(word.to_le_bytes());
    }
    image.extend(rodata);
    image.extend(data);
    image
}

#[test]
fn executes_arm11_from_code_base_and_stores_into_bss() {
    let image = fixture(
        &[
            0xe59f_0004, // ldr r0, [pc, #4]
            0xe3a0_105a, // mov r1, #0x5a
            0xe580_1004, // str r1, [r0, #4]
            DATA,
        ],
        &[1, 2, 3, 4],
        &[5, 6, 7, 8],
        8,
    );
    let mut machine = Machine::load(&image).unwrap();
    assert_eq!(machine.pc(), CODE);
    assert_eq!(machine.register(15), Some(CODE));
    assert_eq!(machine.register(16), None);
    assert_eq!(machine.read_word(RODATA), Ok(0x0403_0201));
    assert_eq!(machine.read_word(DATA), Ok(0x0807_0605));
    assert_eq!(machine.read_word(DATA + 4), Ok(0));
    assert_eq!(machine.read_word(DATA + 8), Ok(0));

    machine.step().unwrap();
    assert_eq!(machine.pc(), CODE + 4);
    assert_eq!(machine.register(0), Some(DATA));
    assert_eq!(
        machine.run_until(8, |state| state.read_word(DATA + 4) == Ok(0x5a)),
        Ok(RunOutcome {
            steps: 2,
            reason: StopReason::ConditionMet,
        })
    );
    assert_eq!(machine.pc(), CODE + 12);
    assert_eq!(machine.register(1), Some(0x5a));
    assert_eq!(machine.read_word(DATA + 8), Ok(0));
}

#[test]
fn applies_rotated_immediates_and_subtracted_offsets() {
    let image = fixture(
        &[
            0xea00_0000, // b CODE + 8
            DATA + 4,
            0xe51f_000c, // ldr r0, [pc, #-12]
            0xe3a0_1480, // mov r1, #0x80000000
            0xe500_1004, // str r1, [r0, #-4]
        ],
        &[0; 4],
        &[],
        4,
    );
    let mut machine = Machine::load(&image).unwrap();
    assert_eq!(
        machine.run_until(4, |state| state.read_word(DATA) == Ok(0x8000_0000)),
        Ok(RunOutcome {
            steps: 4,
            reason: StopReason::ConditionMet,
        })
    );
    assert_eq!(machine.pc(), CODE + 20);
}

#[test]
fn rejects_guest_stores_to_code_and_read_only_data() {
    for address in [CODE, RODATA] {
        let image = fixture(&[0xe59f_0000, 0xe580_0000, address], &[1, 2, 3, 4], &[], 0);
        let mut machine = Machine::load(&image).unwrap();
        let original = machine.read_word(address).unwrap();
        machine.step().unwrap();
        let fault = Err(ExecutionError::Memory {
            pc: CODE + 4,
            source: MemoryError {
                address,
                access: Access::Write,
                kind: MemoryErrorKind::PermissionDenied,
            },
        });
        assert_eq!(machine.step(), fault);
        assert_eq!(machine.step(), fault);
        assert_eq!(machine.pc(), CODE + 4);
        assert_eq!(machine.read_word(address), Ok(original));
    }
}

#[test]
fn rejects_fetches_from_non_executable_segments() {
    for (instruction, address) in [(0xea00_03fe, RODATA), (0xea00_07fe, DATA)] {
        let image = fixture(&[instruction], &[0; 4], &[0; 4], 0);
        let mut machine = Machine::load(&image).unwrap();
        machine.step().unwrap();
        assert_eq!(machine.pc(), address);
        assert_eq!(
            machine.step(),
            Err(ExecutionError::Memory {
                pc: address,
                source: MemoryError {
                    address,
                    access: Access::Fetch,
                    kind: MemoryErrorKind::PermissionDenied,
                },
            })
        );
    }
}

#[test]
fn restricts_word_access_to_aligned_mapped_addresses() {
    let image = fixture(&[0xe59f_0000], &[0; 4], &[], 0);
    let mut machine = Machine::load(&image).unwrap();
    for (address, kind) in [
        (CODE + 1, MemoryErrorKind::Unaligned),
        (CODE + 4, MemoryErrorKind::Unmapped),
        (CODE - 4, MemoryErrorKind::Unmapped),
        (u32::MAX - 3, MemoryErrorKind::Unmapped),
    ] {
        assert_eq!(
            machine.read_word(address),
            Err(MemoryError {
                address,
                access: Access::Read,
                kind,
            })
        );
    }
    assert_eq!(
        machine.step(),
        Err(ExecutionError::Memory {
            pc: CODE,
            source: MemoryError {
                address: CODE + 8,
                access: Access::Read,
                kind: MemoryErrorKind::Unmapped,
            },
        })
    );
    assert_eq!(machine.register(0), Some(0));
    assert_eq!(machine.pc(), CODE);
}

#[test]
fn rejects_truncated_headers_and_payloads() {
    let image = fixture(&[0xeaff_fffe], &[0; 4], &[0; 4], 4);
    assert!(matches!(
        Machine::load(&image[..31]),
        Err(LoadError::TruncatedHeader { .. })
    ));
    assert!(matches!(
        Machine::load(&image[..55]),
        Err(LoadError::TruncatedRelocationHeaders { .. })
    ));
    for (len, expected) in [
        (59, Segment::Code),
        (63, Segment::Rodata),
        (67, Segment::Data),
    ] {
        assert!(matches!(
            Machine::load(&image[..len]),
            Err(LoadError::TruncatedSegment { segment, .. }) if segment == expected
        ));
    }
}

#[test]
fn rejects_impossible_sizes_and_oversized_mappings() {
    let image = fixture(&[], &[], &[], 0);
    assert!(matches!(
        Machine::load(&image),
        Err(LoadError::InvalidCodeSize(0))
    ));
    let mut image = fixture(&[0xeaff_fffe], &[], &[], 0);
    image[16..20].copy_from_slice(&3_u32.to_le_bytes());
    assert!(matches!(
        Machine::load(&image),
        Err(LoadError::InvalidCodeSize(3))
    ));
    image[16..20].copy_from_slice(&4_u32.to_le_bytes());
    image[28..32].copy_from_slice(&4_u32.to_le_bytes());
    assert!(matches!(
        Machine::load(&image),
        Err(LoadError::InvalidBssSize { .. })
    ));

    let image = fixture(&[0xeaff_fffe], &[], &[], 16 * 1024 * 1024 - 4);
    assert!(matches!(
        Machine::load(&image),
        Err(LoadError::ImageTooLarge { .. })
    ));
    let image = fixture(&[0xeaff_fffe], &[], &[], u32::MAX);
    assert!(matches!(
        Machine::load(&image),
        Err(LoadError::ImageTooLarge { .. })
    ));
}

#[test]
fn rejects_features_outside_the_initial_3dsx_profile() {
    let image = fixture(&[0xeaff_fffe], &[], &[], 0);
    let mut modified = image.clone();
    modified[0] = b'N';
    assert!(matches!(
        Machine::load(&modified),
        Err(LoadError::InvalidMagic(_))
    ));
    for (offset, value, expected) in [
        (4, 44, LoadError::UnsupportedHeaderSize(44)),
        (6, 12, LoadError::UnsupportedRelocationHeaderSize(12)),
        (8, 1, LoadError::UnsupportedVersion(1)),
        (12, 1, LoadError::UnsupportedFlags(1)),
    ] {
        let mut modified = image.clone();
        modified[offset] = value;
        assert_eq!(Machine::load(&modified).err(), Some(expected));
    }
    for (index, segment) in [Segment::Code, Segment::Rodata, Segment::Data]
        .into_iter()
        .enumerate()
    {
        for kind in 0..2 {
            let mut modified = image.clone();
            modified[32 + index * 8 + kind * 4] = 1;
            assert!(matches!(
                Machine::load(&modified),
                Err(LoadError::UnsupportedRelocations { segment: actual, .. }) if actual == segment
            ));
        }
    }
    let mut trailing = image;
    trailing.push(0);
    assert_eq!(
        Machine::load(&trailing).err(),
        Some(LoadError::TrailingData { bytes: 1 })
    );
}

#[test]
fn reports_the_address_and_opcode_of_an_unsupported_instruction() {
    for opcode in [
        0xe12f_ff1e,
        0xe3b0_0000,
        0x13a0_0001,
        0xe3a0_f000,
        0xe580_f000,
    ] {
        let image = fixture(&[0xe3a0_0001, opcode], &[], &[], 0);
        let mut machine = Machine::load(&image).unwrap();
        assert_eq!(
            machine.run_until(2, |_| false),
            Err(ExecutionError::UnsupportedInstruction {
                pc: CODE + 4,
                opcode,
            })
        );
        assert_eq!(machine.pc(), CODE + 4);
        assert_eq!(machine.register(0), Some(1));
    }
}

#[test]
fn reports_initial_stop_and_bounds_a_loop() {
    let image = fixture(&[0xeaff_fffe], &[], &[], 0); // b .
    let mut machine = Machine::load(&image).unwrap();
    assert_eq!(
        machine.run_until(0, |state| state.pc() == CODE),
        Ok(RunOutcome {
            steps: 0,
            reason: StopReason::ConditionMet,
        })
    );
    for budget in [0, 3] {
        let mut observations = 0;
        assert_eq!(
            machine.run_until(budget, |_| {
                observations += 1;
                false
            }),
            Ok(RunOutcome {
                steps: budget,
                reason: StopReason::BudgetExhausted,
            })
        );
        assert_eq!(observations, budget + 1);
        assert_eq!(machine.pc(), CODE);
    }
}
