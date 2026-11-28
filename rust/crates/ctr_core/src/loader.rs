// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use crate::{LoadError, Segment};

pub(crate) const CODE_BASE: u32 = 0x0010_0000;
const PAGE_SIZE: u64 = 4096;
const MAX_MAPPED_SIZE: u64 = 16 * 1024 * 1024;
const HEADER_SIZE: usize = 32;
const RELOCATION_HEADER_SIZE: usize = 8;
const SEGMENTS: [Segment; 3] = [Segment::Code, Segment::Rodata, Segment::Data];

pub(crate) struct LoadSegment<'a> {
    pub kind: Segment,
    pub base: u32,
    pub size: usize,
    pub initialized: &'a [u8],
}

pub(crate) fn parse(image: &[u8]) -> Result<[LoadSegment<'_>; 3], LoadError> {
    let (header, payload) =
        image
            .split_first_chunk::<HEADER_SIZE>()
            .ok_or(LoadError::TruncatedHeader {
                available: image.len(),
            })?;
    let magic = [header[0], header[1], header[2], header[3]];
    if &magic != b"3DSX" {
        return Err(LoadError::InvalidMagic(magic));
    }
    let header_size = u16::from_le_bytes([header[4], header[5]]);
    if usize::from(header_size) != HEADER_SIZE {
        return Err(LoadError::UnsupportedHeaderSize(header_size));
    }
    let relocation_header_size = u16::from_le_bytes([header[6], header[7]]);
    if usize::from(relocation_header_size) != RELOCATION_HEADER_SIZE {
        return Err(LoadError::UnsupportedRelocationHeaderSize(
            relocation_header_size,
        ));
    }
    let version = read_word(header, 8);
    if version != 0 {
        return Err(LoadError::UnsupportedVersion(version));
    }
    let flags = read_word(header, 12);
    if flags != 0 {
        return Err(LoadError::UnsupportedFlags(flags));
    }
    let code_size = read_word(header, 16);
    let rodata_size = read_word(header, 20);
    let data_size = read_word(header, 24);
    let bss_size = read_word(header, 28);
    if code_size < 4 {
        return Err(LoadError::InvalidCodeSize(code_size));
    }
    let initialized_data_size =
        data_size
            .checked_sub(bss_size)
            .ok_or(LoadError::InvalidBssSize {
                data_size,
                bss_size,
            })?;

    let rodata_offset = aligned_size(code_size);
    let data_offset = rodata_offset + aligned_size(rodata_size);
    let mapped_size = data_offset + aligned_size(data_size);
    if mapped_size > MAX_MAPPED_SIZE {
        return Err(LoadError::ImageTooLarge { mapped_size });
    }

    let (relocations, mut payload) = payload
        .split_first_chunk::<{ RELOCATION_HEADER_SIZE * SEGMENTS.len() }>()
        .ok_or(LoadError::TruncatedRelocationHeaders {
            available: payload.len(),
        })?;
    let (headers, _) = relocations.as_chunks::<RELOCATION_HEADER_SIZE>();
    for (segment, header) in SEGMENTS.into_iter().zip(headers) {
        let absolute = read_word(header, 0);
        let relative = read_word(header, 4);
        if absolute != 0 || relative != 0 {
            return Err(LoadError::UnsupportedRelocations {
                segment,
                absolute,
                relative,
            });
        }
    }

    let code = take_segment(&mut payload, Segment::Code, CODE_BASE, code_size, code_size)?;
    let rodata = take_segment(
        &mut payload,
        Segment::Rodata,
        CODE_BASE + rodata_offset as u32,
        rodata_size,
        rodata_size,
    )?;
    let data = take_segment(
        &mut payload,
        Segment::Data,
        CODE_BASE + data_offset as u32,
        data_size,
        initialized_data_size,
    )?;
    if !payload.is_empty() {
        return Err(LoadError::TrailingData {
            bytes: payload.len(),
        });
    }
    Ok([code, rodata, data])
}

fn aligned_size(size: u32) -> u64 {
    u64::from(size).next_multiple_of(PAGE_SIZE)
}

fn read_word(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes([
        bytes[offset],
        bytes[offset + 1],
        bytes[offset + 2],
        bytes[offset + 3],
    ])
}

fn take_segment<'a>(
    payload: &mut &'a [u8],
    kind: Segment,
    base: u32,
    size: u32,
    initialized_size: u32,
) -> Result<LoadSegment<'a>, LoadError> {
    let expected = initialized_size as usize;
    let (initialized, rest) =
        payload
            .split_at_checked(expected)
            .ok_or(LoadError::TruncatedSegment {
                segment: kind,
                expected,
                available: payload.len(),
            })?;
    *payload = rest;
    Ok(LoadSegment {
        kind,
        base,
        size: size as usize,
        initialized,
    })
}
