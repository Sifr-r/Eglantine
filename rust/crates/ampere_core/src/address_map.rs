// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

pub const NATIVE_ADDRESS_MAP_MAGIC: u64 = 0x5041_4d4e_4544_454eu64;

#[inline]
pub fn has_native_address_map(code: &[u64]) -> bool {
    code.len() >= 2 && code[code.len() - 1] == NATIVE_ADDRESS_MAP_MAGIC
}

#[inline]
pub fn append_native_address_map(code: &mut Vec<u64>, offsets: &[u32]) {
    if offsets.is_empty() {
        return;
    }
    for &index in offsets {
        let mapped = 0x50u32
            .wrapping_add(index.wrapping_mul(8))
            .wrapping_add((index.wrapping_add(1) / 3).wrapping_mul(8));
        code.push(mapped as u64);
    }
    code.push(offsets.len() as u64);
    code.push(NATIVE_ADDRESS_MAP_MAGIC);
}

#[inline]
pub fn native_program_address(code: &[u64], address: u32) -> Option<u32> {
    if !has_native_address_map(code) || !address.is_multiple_of(16) {
        return None;
    }
    let count = code[code.len() - 2];
    if count == 0
        || count > 65536
        || count + 2 > (code.len() as u64)
        || ((address / 16) as u64) >= count
    {
        return None;
    }
    let start = code.len() - (count as usize) - 2;
    let target = code[start + (address / 16) as usize];
    if target < 0x50
        || target >= (start as u64).saturating_mul(8)
        || !target.is_multiple_of(8)
        || target.is_multiple_of(32)
    {
        return None;
    }
    Some(target as u32)
}
