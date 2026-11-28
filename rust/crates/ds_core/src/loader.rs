// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use crate::cpu::Cpu;
use crate::error::LoadError;
use crate::memory::{Mapping, WORD_SIZE};

const PAYLOAD_FIELDS_END: usize = 0x40;
const ARM9_FIELDS_START: usize = 0x20;
const ARM7_FIELDS_START: usize = 0x30;

pub(crate) struct Segment<'a> {
    pub(crate) entry: u32,
    pub(crate) mapping: Mapping,
    pub(crate) bytes: &'a [u8],
}

fn word(bytes: &[u8], offset: usize) -> u32 {
    let mut word = [0; WORD_SIZE];
    word.copy_from_slice(&bytes[offset..offset + WORD_SIZE]);
    u32::from_le_bytes(word)
}

fn segment(image: &[u8], cpu: Cpu, fields: usize) -> Result<Segment<'_>, LoadError> {
    let offset = word(image, fields);
    let entry = word(image, fields + 4);
    let address = word(image, fields + 8);
    let size = word(image, fields + 12);

    let file_error = LoadError::InvalidPayloadFileRange { cpu, offset, size };
    if (offset as usize) < PAYLOAD_FIELDS_END || size == 0 {
        return Err(file_error);
    }
    let end = offset.checked_add(size).ok_or(file_error)?;
    let bytes = image.get(offset as usize..end as usize).ok_or(file_error)?;

    let mapping = Mapping::locate(cpu, address, bytes.len())
        .ok_or(LoadError::InvalidPayloadAddress { cpu, address, size })?;
    let entry_end = entry
        .checked_sub(address)
        .and_then(|offset| offset.checked_add(WORD_SIZE as u32));
    if entry & 3 != 0 || !matches!(entry_end, Some(end) if end <= size) {
        return Err(LoadError::InvalidEntryPoint {
            cpu,
            entry,
            address,
            size,
        });
    }

    Ok(Segment {
        entry,
        mapping,
        bytes,
    })
}

pub(crate) fn load(image: &[u8]) -> Result<(Segment<'_>, Segment<'_>), LoadError> {
    if image.len() < PAYLOAD_FIELDS_END {
        return Err(LoadError::TruncatedHeader {
            actual: image.len(),
        });
    }

    let arm9 = segment(image, Cpu::Arm9, ARM9_FIELDS_START)?;
    let arm7 = segment(image, Cpu::Arm7, ARM7_FIELDS_START)?;
    if arm9.mapping.overlaps(&arm7.mapping) {
        return Err(LoadError::OverlappingPayloads);
    }
    Ok((arm9, arm7))
}
