// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use crate::loader::LoadSegment;
use crate::{Access, MemoryError, MemoryErrorKind, Segment};

pub(crate) struct Memory {
    regions: [Region; 3],
}

struct Region {
    kind: Segment,
    base: u32,
    bytes: Vec<u8>,
}

impl Memory {
    pub(crate) fn new(segments: [LoadSegment<'_>; 3]) -> Self {
        Self {
            regions: segments.map(|segment| {
                let mut bytes = vec![0; segment.size];
                bytes[..segment.initialized.len()].copy_from_slice(segment.initialized);
                Region {
                    kind: segment.kind,
                    base: segment.base,
                    bytes,
                }
            }),
        }
    }

    pub(crate) fn fetch_word(&self, address: u32) -> Result<u32, MemoryError> {
        self.load_word(address, Access::Fetch)
    }

    pub(crate) fn read_word(&self, address: u32) -> Result<u32, MemoryError> {
        self.load_word(address, Access::Read)
    }

    fn load_word(&self, address: u32, access: Access) -> Result<u32, MemoryError> {
        let (index, offset) = self.resolve_word(address, access)?;
        let bytes = &self.regions[index].bytes[offset..offset + 4];
        Ok(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
    }

    pub(crate) fn write_word(&mut self, address: u32, value: u32) -> Result<(), MemoryError> {
        let (index, offset) = self.resolve_word(address, Access::Write)?;
        self.regions[index].bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
        Ok(())
    }

    fn resolve_word(&self, address: u32, access: Access) -> Result<(usize, usize), MemoryError> {
        let fault = |kind| MemoryError {
            address,
            access,
            kind,
        };
        if address & 3 != 0 {
            return Err(fault(MemoryErrorKind::Unaligned));
        }
        for (index, region) in self.regions.iter().enumerate() {
            let Some(offset) = address
                .checked_sub(region.base)
                .map(|offset| offset as usize)
            else {
                continue;
            };
            if offset >= region.bytes.len() || region.bytes.len() - offset < 4 {
                continue;
            }
            let permitted = match access {
                Access::Read => true,
                Access::Fetch => region.kind == Segment::Code,
                Access::Write => region.kind == Segment::Data,
            };
            return if permitted {
                Ok((index, offset))
            } else {
                Err(fault(MemoryErrorKind::PermissionDenied))
            };
        }
        Err(fault(MemoryErrorKind::Unmapped))
    }
}
