// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use crate::types::{Instruction, LoweredProgram};

#[inline]
fn read_u32_le(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes([
        bytes[offset],
        bytes[offset + 1],
        bytes[offset + 2],
        bytes[offset + 3],
    ])
}

#[inline]
fn read_u64_le(bytes: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes([
        bytes[offset],
        bytes[offset + 1],
        bytes[offset + 2],
        bytes[offset + 3],
        bytes[offset + 4],
        bytes[offset + 5],
        bytes[offset + 6],
        bytes[offset + 7],
    ])
}

pub fn native_program_key(source: &[Instruction]) -> u64 {
    let mut hash: u64 = 14_695_981_039_346_656_037;
    for instruction in source {
        for word in instruction.words {
            for byte in 0..4 {
                hash = (hash ^ (((word >> (byte * 8)) & 255) as u64))
                    .wrapping_mul(1_099_511_628_211);
            }
        }
    }
    hash
}

// Private iteration format: version, lengths, texture bank, exact original
// instructions, then the newly generated canonical instructions. Source bytes
// are compared directly; the filename hash is only an index.
pub fn encode_program_override(source: &[Instruction], program: &LoweredProgram) -> Vec<u8> {
    let source_bytes = std::mem::size_of_val(source);
    if !program.is_ok()
        || (program.source_size as usize) != source_bytes
        || program.source_instruction_offsets.len() != source.len()
    {
        return Vec::new();
    }
    let header: [u32; 6] = [
        0x4e45_4445,
        0x3230_564f,
        source_bytes as u32,
        program.instructions.len() as u32,
        program.required_texture_buffer,
        program.source_instruction_offsets.len() as u32,
    ];
    let total_size = 24
        + source_bytes
        + program.instructions.len() * 8
        + program.source_instruction_offsets.len() * 4;
    let mut bytes = Vec::with_capacity(total_size);
    for word in header {
        bytes.extend_from_slice(&word.to_le_bytes());
    }
    for instruction in source {
        for word in instruction.words {
            bytes.extend_from_slice(&word.to_le_bytes());
        }
    }
    for &instruction in &program.instructions {
        bytes.extend_from_slice(&instruction.to_le_bytes());
    }
    for &offset in &program.source_instruction_offsets {
        bytes.extend_from_slice(&offset.to_le_bytes());
    }
    bytes
}

pub fn program_override_source_size(bytes: &[u8]) -> u32 {
    if bytes.len() < 24 {
        return 0;
    }
    let header: [u32; 6] = [
        read_u32_le(bytes, 0),
        read_u32_le(bytes, 4),
        read_u32_le(bytes, 8),
        read_u32_le(bytes, 12),
        read_u32_le(bytes, 16),
        read_u32_le(bytes, 20),
    ];
    let mapped = header[1] == 0x3230_564f;
    if header[0] != 0x4e45_4445
        || (!mapped && header[1] != 0x3130_564f)
        || (if mapped {
            header[5] != header[2] / 16
        } else {
            header[5] != 0
        })
        || header[2] == 0
        || !header[2].is_multiple_of(16)
        || header[2] > 1_048_576
        || header[3] == 0
        || header[3] > 1_048_576
        || (header[4] != u32::MAX && header[4] >= 18)
    {
        return 0;
    }
    let expected_size =
        24usize + (header[2] as usize) + (header[3] as usize) * 8 + (header[5] as usize) * 4;
    if bytes.len() != expected_size {
        return 0;
    }
    header[2]
}

pub fn decode_program_override(bytes: &[u8], source: &[Instruction]) -> LoweredProgram {
    let mut result = LoweredProgram::default();
    let fail = |mut res: LoweredProgram, message: &str| -> LoweredProgram {
        res.instructions.clear();
        res.source_instruction_offsets.clear();
        res.error = message.to_owned();
        res
    };
    if program_override_source_size(bytes) == 0 {
        return fail(result, "invalid native override header or length");
    }
    let header: [u32; 6] = [
        read_u32_le(bytes, 0),
        read_u32_le(bytes, 4),
        read_u32_le(bytes, 8),
        read_u32_le(bytes, 12),
        read_u32_le(bytes, 16),
        read_u32_le(bytes, 20),
    ];
    let source_bytes = std::mem::size_of_val(source);
    if (header[2] as usize) != source_bytes {
        return fail(result, "native override source length mismatch");
    }
    let src_payload = &bytes[24..24 + source_bytes];
    for (chunk, insn) in src_payload.as_chunks::<16>().0.iter().zip(source.iter()) {
        for (word_bytes, &word) in chunk.as_chunks::<4>().0.iter().zip(insn.words.iter()) {
            if *word_bytes != word.to_le_bytes() {
                return fail(result, "native override source mismatch");
            }
        }
    }
    let insns_start = 24 + source_bytes;
    let insns_len = header[3] as usize;
    let insns_end = insns_start + insns_len * 8;
    result.instructions.reserve_exact(insns_len);
    for chunk in bytes[insns_start..insns_end].as_chunks::<8>().0 {
        result.instructions.push(read_u64_le(chunk, 0));
    }
    if header[5] != 0 {
        let offsets_len = header[5] as usize;
        let offsets_end = insns_end + offsets_len * 4;
        result.source_instruction_offsets.reserve_exact(offsets_len);
        for chunk in bytes[insns_end..offsets_end].as_chunks::<4>().0 {
            result.source_instruction_offsets.push(read_u32_le(chunk, 0));
        }
        let mut previous = 0u32;
        for &offset in &result.source_instruction_offsets {
            if (offset as usize) >= result.instructions.len() || offset < previous {
                return fail(result, "native override address map mismatch");
            }
            previous = offset;
        }
    }
    result.source_size = header[2];
    result.required_texture_buffer = header[4];
    result
}
