// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use std::ops::Range;

use crate::cpu::Cpu;
use crate::error::MemoryError;

const MAIN_BASE: u32 = 0x0200_0000;
const MAIN_SIZE: usize = 4 * 1024 * 1024;
const ARM7_PRIVATE_BASE: u32 = 0x0380_0000;
const ARM7_PRIVATE_SIZE: usize = 64 * 1024;

pub(crate) const WORD_SIZE: usize = size_of::<u32>();

#[derive(Clone, Copy, Eq, PartialEq)]
enum Region {
    Main,
    Arm7Private,
}

pub(crate) struct Mapping {
    region: Region,
    range: Range<usize>,
}

impl Mapping {
    pub(crate) fn locate(cpu: Cpu, address: u32, size: usize) -> Option<Self> {
        if let Some(range) = region_range(address, size, MAIN_BASE, MAIN_SIZE) {
            return Some(Self {
                region: Region::Main,
                range,
            });
        }
        if cpu == Cpu::Arm7 {
            let range = region_range(address, size, ARM7_PRIVATE_BASE, ARM7_PRIVATE_SIZE)?;
            return Some(Self {
                region: Region::Arm7Private,
                range,
            });
        }
        None
    }

    pub(crate) fn overlaps(&self, other: &Self) -> bool {
        self.region == other.region
            && self.range.start < other.range.end
            && other.range.start < self.range.end
    }

    fn word(cpu: Cpu, address: u32) -> Result<Self, MemoryError> {
        if address & 3 != 0 {
            return Err(MemoryError::UnalignedWord { address });
        }
        Self::locate(cpu, address, WORD_SIZE).ok_or(MemoryError::UnmappedWord { address })
    }
}

fn region_range(address: u32, size: usize, base: u32, capacity: usize) -> Option<Range<usize>> {
    let start = address.checked_sub(base)? as usize;
    let end = start.checked_add(size)?;
    (end <= capacity).then_some(start..end)
}

pub(crate) struct Memory {
    main: Box<[u8]>,
    arm7_private: Box<[u8]>,
}

impl Memory {
    pub(crate) fn new() -> Self {
        Self {
            main: vec![0; MAIN_SIZE].into_boxed_slice(),
            arm7_private: vec![0; ARM7_PRIVATE_SIZE].into_boxed_slice(),
        }
    }

    pub(crate) fn load(&mut self, mapping: Mapping, bytes: &[u8]) {
        let region = match mapping.region {
            Region::Main => &mut self.main,
            Region::Arm7Private => &mut self.arm7_private,
        };
        region[mapping.range].copy_from_slice(bytes);
    }

    pub(crate) fn read_word(&self, cpu: Cpu, address: u32) -> Result<u32, MemoryError> {
        let mapping = Mapping::word(cpu, address)?;
        let region = match mapping.region {
            Region::Main => &self.main,
            Region::Arm7Private => &self.arm7_private,
        };
        let mut word = [0; WORD_SIZE];
        word.copy_from_slice(&region[mapping.range]);
        Ok(u32::from_le_bytes(word))
    }

    pub(crate) fn write_word(
        &mut self,
        cpu: Cpu,
        address: u32,
        word: u32,
    ) -> Result<(), MemoryError> {
        self.load(Mapping::word(cpu, address)?, &word.to_le_bytes());
        Ok(())
    }
}
