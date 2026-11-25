// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::fs::{self, File};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::AtomicBool;

use crate::format::{
    is_cancelled, put_u16_le, put_u32_le, put_u64_le, read_u16_le, read_u32_le, read_u64_le,
    sha256, ReadAt, DIR_RECORD_SIZE, EXTENT_ALIGNMENT, FILE_RECORD_SIZE, HEADER_MAGIC,
    PACKAGE_RECORD_SIZE, PROGRESS_STEP, READ_BUFFER_SIZE, SUPERBLOCK_SIZE, TLV_HEADER_SIZE,
    TOC_HEADER_SIZE,
};
use crate::reader::{FileReader, NupContainer, PackageInfo, PackageKind};
use crate::writer::PackProgress;

#[repr(u16)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MetaType {
    Npdm = 0x0001,
    Nacp = 0x0002,
    Icon = 0x0003,
    DisplayVersion = 0x0004,
    TitleName = 0x0005,
    Publisher = 0x0006,
    SourceDump = 0x0008,
    LocalizedTitles = 0x0009,
    LocalizedIcons = 0x000A,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Metadata {
    pub title_name: String,
    pub icon: Vec<u8>,
    pub display_version: String,
    pub publisher: String,
    pub localized_titles: Vec<(String, String)>,
    pub version_source: String,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct MetadataEdit {
    pub title_name: Option<String>,
    pub icon: Option<Vec<u8>>,
    pub display_version: Option<String>,
    pub publisher: Option<String>,
    pub localized_titles: Option<Vec<(String, String)>>,
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EditResult {
    Success = 0,
    Cancelled = 1,
    InvalidSource = 2,
    IoError = 3,
    NoChange = 4,
}

pub const LANGUAGE_NAMES: [&str; 18] = [
    "AmericanEnglish",
    "BritishEnglish",
    "Japanese",
    "French",
    "German",
    "LatinAmericanSpanish",
    "Spanish",
    "Italian",
    "Dutch",
    "CanadianFrench",
    "Portuguese",
    "Russian",
    "Korean",
    "TraditionalChinese",
    "SimplifiedChinese",
    "BrazilianPortuguese",
    "Polish",
    "Thai",
];

pub fn parse_tlvs(block: &[u8]) -> Result<Vec<(u16, Vec<u8>)>, String> {
    let mut out = Vec::new();
    let mut pos = 0usize;
    while pos + TLV_HEADER_SIZE <= block.len() {
        let tlv_type = read_u16_le(block, pos);
        let size = read_u32_le(block, pos + 4) as usize;
        pos += TLV_HEADER_SIZE;
        if size > block.len() - pos {
            return Err("malformed meta block".to_string());
        }
        out.push((tlv_type, block[pos..pos + size].to_vec()));
        pos += size;
    }
    Ok(out)
}

pub fn serialize_tlvs(mut records: Vec<(u16, Vec<u8>)>) -> Vec<u8> {
    records.sort_by_key(|r| r.0);
    let mut out = Vec::new();
    for (tlv_type, payload) in records {
        let at = out.len();
        out.resize(at + 8 + payload.len(), 0);
        put_u16_le(&mut out, at, tlv_type);
        put_u32_le(&mut out, at + 4, payload.len() as u32);
        out[at + 8..at + 8 + payload.len()].copy_from_slice(&payload);
    }
    out
}

pub fn set_tlv(tlvs: &mut Vec<(u16, Vec<u8>)>, tlv_type: u16, payload: Vec<u8>) {
    tlvs.retain(|(t, _)| *t != tlv_type);
    tlvs.push((tlv_type, payload));
}

pub fn set_or_remove_tlv(tlvs: &mut Vec<(u16, Vec<u8>)>, tlv_type: u16, value: Option<&[u8]>) {
    let Some(bytes) = value else {
        return;
    };
    tlvs.retain(|(t, _)| *t != tlv_type);
    if !bytes.is_empty() {
        tlvs.push((tlv_type, bytes.to_vec()));
    }
}

pub fn serialize_localized_titles(titles: &[(String, String)]) -> Vec<u8> {
    let mut out = Vec::new();
    for (lang, name) in titles {
        if lang.is_empty() || lang.len() > 0xFF || name.is_empty() {
            continue;
        }
        out.push(lang.len() as u8);
        out.extend_from_slice(lang.as_bytes());
        out.extend_from_slice(&(name.len() as u32).to_le_bytes());
        out.extend_from_slice(name.as_bytes());
    }
    out
}

pub fn parse_package_meta<R: ReadAt + ?Sized>(
    reader: &R,
    package: &mut PackageInfo,
) -> Result<(), String> {
    if package.meta_offset == 0 {
        return Ok(());
    }
    let block = reader
        .read_exact_vec(package.meta_offset, package.meta_size as usize)
        .map_err(|e| e.to_string())?;
    let mut pos = 0usize;
    while pos + TLV_HEADER_SIZE <= block.len() {
        let tlv_type = read_u16_le(&block, pos);
        let size = read_u32_le(&block, pos + 4) as usize;
        pos += TLV_HEADER_SIZE;
        if size > block.len() - pos {
            return Err("truncated TLV payload".to_string());
        }
        let payload = &block[pos..pos + size];
        pos += size;
        match tlv_type {
            x if x == MetaType::Npdm as u16 => package.npdm = payload.to_vec(),
            x if x == MetaType::Icon as u16 => package.icon = payload.to_vec(),
            x if x == MetaType::TitleName as u16 => {
                package.title_name = String::from_utf8_lossy(payload).into_owned();
            }
            x if x == MetaType::Publisher as u16 => {
                package.publisher = String::from_utf8_lossy(payload).into_owned();
            }
            x if x == MetaType::SourceDump as u16 => {
                package.source_dump = String::from_utf8_lossy(payload).into_owned();
            }
            x if x == MetaType::DisplayVersion as u16 => {
                package.display_version = String::from_utf8_lossy(payload).into_owned();
            }
            x if x == MetaType::LocalizedTitles as u16
                || x == MetaType::LocalizedIcons as u16 =>
            {
                let is_title = tlv_type == MetaType::LocalizedTitles as u16;
                let mut rpos = 0usize;
                while rpos < payload.len() {
                    let lang_len = payload[rpos] as usize;
                    rpos += 1;
                    if rpos + lang_len + 4 > payload.len() {
                        return Err("truncated localized record".to_string());
                    }
                    let lang = String::from_utf8_lossy(&payload[rpos..rpos + lang_len]).into_owned();
                    rpos += lang_len;
                    let data_len = read_u32_le(payload, rpos) as usize;
                    rpos += 4;
                    if data_len > payload.len() - rpos {
                        return Err("truncated localized data".to_string());
                    }
                    let val = &payload[rpos..rpos + data_len];
                    rpos += data_len;
                    if is_title {
                        package
                            .localized_titles
                            .push((lang, String::from_utf8_lossy(val).into_owned()));
                    } else {
                        package.localized_icons.push((lang, val.to_vec()));
                    }
                }
            }
            _ => {}
        }
    }
    Ok(())
}

// Pure-Rust RFC 1951 raw DEFLATE decoder for compressed NACP titles.
struct BitReader<'a> {
    data: &'a [u8],
    byte_pos: usize,
    bit_buf: u32,
    bit_cnt: u8,
}

impl<'a> BitReader<'a> {
    fn new(data: &'a [u8]) -> Self {
        Self {
            data,
            byte_pos: 0,
            bit_buf: 0,
            bit_cnt: 0,
        }
    }

    fn ensure(&mut self, n: u8) -> Result<(), String> {
        while self.bit_cnt < n {
            if self.byte_pos >= self.data.len() {
                return Err("unexpected EOF in deflate stream".to_string());
            }
            self.bit_buf |= (self.data[self.byte_pos] as u32) << self.bit_cnt;
            self.byte_pos += 1;
            self.bit_cnt += 8;
        }
        Ok(())
    }

    fn read_bits(&mut self, n: u8) -> Result<u32, String> {
        if n == 0 {
            return Ok(0);
        }
        self.ensure(n)?;
        let val = self.bit_buf & ((1u32 << n) - 1);
        self.bit_buf >>= n;
        self.bit_cnt -= n;
        Ok(val)
    }

    fn align_byte(&mut self) {
        let drop = self.bit_cnt % 8;
        self.bit_buf >>= drop;
        self.bit_cnt -= drop;
    }

    fn read_aligned_u16(&mut self) -> Result<u16, String> {
        self.align_byte();
        let lo = self.read_bits(8)? as u16;
        let hi = self.read_bits(8)? as u16;
        Ok(lo | (hi << 8))
    }

    fn read_aligned_bytes(&mut self, len: usize, out: &mut Vec<u8>) -> Result<(), String> {
        self.align_byte();
        let mut remaining = len;
        while remaining > 0 && self.bit_cnt >= 8 {
            out.push((self.bit_buf & 0xFF) as u8);
            self.bit_buf >>= 8;
            self.bit_cnt -= 8;
            remaining -= 1;
        }
        if remaining > 0 {
            if self.byte_pos + remaining > self.data.len() {
                return Err("truncated stored deflate block".to_string());
            }
            out.extend_from_slice(&self.data[self.byte_pos..self.byte_pos + remaining]);
            self.byte_pos += remaining;
        }
        Ok(())
    }
}

struct HuffmanTree {
    counts: [u16; 16],
    symbols: Vec<u16>,
}

impl HuffmanTree {
    fn from_lengths(lengths: &[u8]) -> Result<Self, String> {
        let mut counts = [0u16; 16];
        for &l in lengths {
            if l as usize > 15 {
                return Err("huffman length > 15".to_string());
            }
            if l > 0 {
                counts[l as usize] += 1;
            }
        }
        let mut offsets = [0u16; 16];
        let mut total = 0u16;
        for bits in 1..=15 {
            offsets[bits] = total;
            total += counts[bits];
        }
        let mut symbols = vec![0u16; total as usize];
        for (sym, &l) in lengths.iter().enumerate() {
            if l > 0 {
                let idx = offsets[l as usize] as usize;
                offsets[l as usize] += 1;
                symbols[idx] = sym as u16;
            }
        }
        Ok(Self { counts, symbols })
    }

    fn decode(&self, reader: &mut BitReader<'_>) -> Result<u16, String> {
        let mut code = 0i32;
        let mut first = 0i32;
        let mut index = 0i32;
        for len in 1..=15 {
            code |= reader.read_bits(1)? as i32;
            let count = self.counts[len] as i32;
            if code - count < first {
                return Ok(self.symbols[(index + (code - first)) as usize]);
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        Err("invalid huffman code".to_string())
    }
}

pub fn inflate_raw(compressed: &[u8], max_out: usize) -> Result<Vec<u8>, String> {
    const LEN_BASE: [u16; 29] = [
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
        131, 163, 195, 227, 258,
    ];
    const LEN_EXTRA: [u8; 29] = [
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
    ];
    const DIST_BASE: [u16; 30] = [
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
        2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
    ];
    const DIST_EXTRA: [u8; 30] = [
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12,
        13, 13,
    ];
    const CL_ORDER: [usize; 19] = [
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
    ];

    let mut reader = BitReader::new(compressed);
    let mut out = Vec::new();

    loop {
        let bfinal = reader.read_bits(1)?;
        let btype = reader.read_bits(2)?;
        match btype {
            0 => {
                let len = reader.read_aligned_u16()? as usize;
                let nlen = reader.read_aligned_u16()?;
                if (len as u16) != !nlen {
                    return Err("stored block length mismatch".to_string());
                }
                if out.len() + len > max_out {
                    return Err("deflate output exceeds limit".to_string());
                }
                reader.read_aligned_bytes(len, &mut out)?;
            }
            1 | 2 => {
                let (lit_tree, dist_tree) = if btype == 1 {
                    let mut ll = [0u8; 288];
                    ll[0..=143].fill(8);
                    ll[144..=255].fill(9);
                    ll[256..=279].fill(7);
                    ll[280..=287].fill(8);
                    let dl = [5u8; 32];
                    (
                        HuffmanTree::from_lengths(&ll)?,
                        HuffmanTree::from_lengths(&dl)?,
                    )
                } else {
                    let hlit = reader.read_bits(5)? as usize + 257;
                    let hdist = reader.read_bits(5)? as usize + 1;
                    let hclen = reader.read_bits(4)? as usize + 4;
                    let mut cl_lens = [0u8; 19];
                    for i in 0..hclen {
                        cl_lens[CL_ORDER[i]] = reader.read_bits(3)? as u8;
                    }
                    let cl_tree = HuffmanTree::from_lengths(&cl_lens)?;
                    let total = hlit + hdist;
                    let mut lens = Vec::with_capacity(total);
                    while lens.len() < total {
                        let sym = cl_tree.decode(&mut reader)?;
                        match sym {
                            0..=15 => lens.push(sym as u8),
                            16 => {
                                let prev = *lens
                                    .last()
                                    .ok_or_else(|| "invalid repeat code 16".to_string())?;
                                let repeat = reader.read_bits(2)? as usize + 3;
                                for _ in 0..repeat {
                                    lens.push(prev);
                                }
                            }
                            17 => {
                                let repeat = reader.read_bits(3)? as usize + 3;
                                lens.resize(lens.len() + repeat, 0);
                            }
                            18 => {
                                let repeat = reader.read_bits(7)? as usize + 11;
                                lens.resize(lens.len() + repeat, 0);
                            }
                            _ => return Err("invalid code-length symbol".to_string()),
                        }
                    }
                    if lens.len() != total {
                        return Err("code-length table overflow".to_string());
                    }
                    (
                        HuffmanTree::from_lengths(&lens[..hlit])?,
                        HuffmanTree::from_lengths(&lens[hlit..])?,
                    )
                };

                loop {
                    let sym = lit_tree.decode(&mut reader)?;
                    if sym < 256 {
                        if out.len() >= max_out {
                            return Err("deflate output exceeds limit".to_string());
                        }
                        out.push(sym as u8);
                    } else if sym == 256 {
                        break;
                    } else {
                        let len_idx = (sym as usize).saturating_sub(257);
                        if len_idx >= LEN_BASE.len() {
                            return Err("invalid length symbol".to_string());
                        }
                        let length = LEN_BASE[len_idx] as usize
                            + reader.read_bits(LEN_EXTRA[len_idx])? as usize;
                        let dist_sym = dist_tree.decode(&mut reader)? as usize;
                        if dist_sym >= DIST_BASE.len() {
                            return Err("invalid distance symbol".to_string());
                        }
                        let dist = DIST_BASE[dist_sym] as usize
                            + reader.read_bits(DIST_EXTRA[dist_sym])? as usize;
                        if dist == 0 || dist > out.len() {
                            return Err("invalid deflate back-reference".to_string());
                        }
                        if out.len() + length > max_out {
                            return Err("deflate output exceeds limit".to_string());
                        }
                        for _ in 0..length {
                            let b = out[out.len() - dist];
                            out.push(b);
                        }
                    }
                }
            }
            _ => return Err("reserved deflate block type".to_string()),
        }
        if bfinal != 0 {
            break;
        }
    }
    Ok(out)
}

#[derive(Clone, Debug, Default)]
pub struct ParsedNacp {
    pub raw_bytes: Vec<u8>,
    pub application_name: String,
    pub developer_name: String,
    pub version_string: String,
}

fn c_str_from_fixed(bytes: &[u8]) -> String {
    let len = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
    String::from_utf8_lossy(&bytes[..len]).into_owned()
}

fn settings_language_to_nacp_slot(settings_lang: i32) -> usize {
    match settings_lang {
        0 => 2,   // Japanese
        1 => 0,   // EnglishAmerican
        2 => 3,   // French
        3 => 4,   // German
        4 => 7,   // Italian
        5 => 6,   // Spanish
        6 => 14,  // Chinese -> SimplifiedChinese
        7 => 12,  // Korean
        8 => 8,   // Dutch
        9 => 10,  // Portuguese
        10 => 11, // Russian
        11 => 13, // Taiwanese -> TraditionalChinese
        12 => 1,  // EnglishBritish
        13 => 9,  // FrenchCanadian
        14 => 5,  // SpanishLatin
        15 => 14, // ChineseSimplified
        16 => 13, // ChineseTraditional
        17 => 15, // PortugueseBrazilian
        18 => 16, // Polish
        19 => 17, // Thai
        _ => 0,   // AmericanEnglish
    }
}

pub fn parse_nacp_bytes(bytes: &[u8], nacp_language_index: i32) -> ParsedNacp {
    let mut raw = vec![0u8; 0x4000];
    let copy_len = bytes.len().min(0x4000);
    raw[..copy_len].copy_from_slice(&bytes[..copy_len]);

    let version_string = c_str_from_fixed(&raw[0x3060..0x3070]);
    let titles_data_format = raw[0x3215];

    let mut entries: Vec<(String, String)> = Vec::new();
    if titles_data_format == 1 {
        let compressed_size = read_u16_le(&raw, 0) as usize;
        let end = (2 + compressed_size).min(0x3000);
        if let Ok(decompressed) = inflate_raw(&raw[2..end], 0x300 * 32) {
            for chunk in decompressed.as_chunks::<0x300>().0 {
                entries.push((
                    c_str_from_fixed(&chunk[0..0x200]),
                    c_str_from_fixed(&chunk[0x200..0x300]),
                ));
            }
        }
    } else {
        for chunk in raw[0..0x3000].as_chunks::<0x300>().0 {
            entries.push((
                c_str_from_fixed(&chunk[0..0x200]),
                c_str_from_fixed(&chunk[0x200..0x300]),
            ));
        }
    }

    let slot = settings_language_to_nacp_slot(nacp_language_index);
    let selected = if slot < entries.len() && !entries[slot].0.is_empty() {
        Some(&entries[slot])
    } else {
        entries
            .iter()
            .find(|(app, _)| !app.is_empty())
            .or_else(|| entries.first())
    };
    let (application_name, developer_name) = selected.cloned().unwrap_or_default();

    ParsedNacp {
        raw_bytes: raw,
        application_name,
        developer_name,
        version_string,
    }
}

pub fn find_directory_ci(parent: &Path, name: &str) -> Option<PathBuf> {
    if !parent.is_dir() {
        return None;
    }
    let wanted = name.to_ascii_lowercase();
    let entries = fs::read_dir(parent).ok()?;
    for entry in entries.flatten() {
        if entry.path().is_dir() {
            if let Some(fname) = entry.file_name().to_str() {
                if fname.to_ascii_lowercase() == wanted {
                    return Some(entry.path());
                }
            }
        }
    }
    None
}

pub fn find_sidecar_dirs(exefs_dir: &Path) -> Vec<PathBuf> {
    let mut dirs = vec![exefs_dir.to_path_buf()];
    let mut cur = exefs_dir.to_path_buf();
    while dirs.len() < 4 {
        let Some(parent) = cur.parent() else {
            break;
        };
        let Some(name) = parent.file_name().and_then(|n| n.to_str()) else {
            break;
        };
        let parent_buf = parent.to_path_buf();
        dirs.push(parent_buf.clone());
        let lower = name.to_ascii_lowercase();
        let storage_level = lower == "base"
            || lower == "update"
            || lower == "updates"
            || (!name.is_empty() && name.bytes().all(|b| b.is_ascii_hexdigit()));
        if !storage_level {
            break;
        }
        cur = parent_buf;
    }
    dirs
}

fn newest_product_tag(dir: &Path, prefix: &str) -> String {
    let mut best = String::new();
    let Ok(entries) = fs::read_dir(dir) else {
        return best;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if !path.is_file() {
            continue;
        }
        let Some(file_name) = entry.file_name().to_str().map(str::to_owned) else {
            continue;
        };
        let Some(rest) = file_name.as_bytes().strip_prefix(prefix.as_bytes()) else {
            continue;
        };
        if rest.len() < 4 || rest[3] != b'.' || !rest[..3].iter().all(u8::is_ascii_digit) {
            continue;
        }
        let digits = std::str::from_utf8(&rest[..3]).expect("validated ASCII digits");
        if digits > best.as_str() {
            best = digits.to_owned();
        }
    }
    best
}

pub fn romfs_product_version(romfs_dir: &Path) -> String {
    let mut best = newest_product_tag(&romfs_dir.join("System").join("Resource"), "ResourceSizeTable.Product.");
    let game_data = newest_product_tag(&romfs_dir.join("GameData"), "GameDataList.Product.");
    if game_data > best {
        best = game_data;
    }
    if best.len() < 3 {
        return String::new();
    }
    let b = best.as_bytes();
    format!("{}.{}.{}", b[0] as char, b[1] as char, b[2] as char)
}

#[derive(Clone, Debug, Default)]
pub struct PackageLoaderInfo {
    pub nacp: Option<ParsedNacp>,
    pub icon: Vec<u8>,
    pub display_version: String,
}

pub fn probe_package_dir(package_dir: &Path, nacp_language_index: i32) -> Option<PackageLoaderInfo> {
    let exefs = find_directory_ci(package_dir, "exefs")?;
    if !exefs.join("main").is_file() {
        return None;
    }

    let sidecar_dirs = find_sidecar_dirs(&exefs);
    let mut icon = Vec::new();
    'outer_lang: for sidecar in &sidecar_dirs {
        for lang in LANGUAGE_NAMES {
            let candidate = sidecar.join(format!("icon_{lang}.dat"));
            if candidate.is_file() {
                if let Ok(bytes) = fs::read(&candidate) {
                    icon = bytes;
                    break 'outer_lang;
                }
            }
        }
    }
    if icon.is_empty() {
        'outer_img: for sidecar in &sidecar_dirs {
            if let Ok(entries) = fs::read_dir(sidecar) {
                for entry in entries.flatten() {
                    let p = entry.path();
                    if p.is_file() {
                        if let Some(ext) = p.extension().and_then(|e| e.to_str()) {
                            if matches!(ext, "png" | "jpg" | "bmp" | "jpeg") {
                                if let Ok(bytes) = fs::read(&p) {
                                    icon = bytes;
                                    break 'outer_img;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    let mut nacp_path: Option<PathBuf> = None;
    'outer_nacp: for sidecar in &sidecar_dirs {
        let control = sidecar.join("control.nacp");
        if control.is_file() {
            nacp_path = Some(control);
            break;
        }
        if let Ok(entries) = fs::read_dir(sidecar) {
            for entry in entries.flatten() {
                let p = entry.path();
                if p.is_file() && p.extension().and_then(|e| e.to_str()) == Some("nacp") {
                    nacp_path = Some(p);
                    break 'outer_nacp;
                }
            }
        }
    }

    let nacp = nacp_path
        .and_then(|p| fs::read(p).ok())
        .map(|bytes| parse_nacp_bytes(&bytes, nacp_language_index));

    let romfs_version = find_directory_ci(package_dir, "romfs")
        .map(|r| romfs_product_version(&r))
        .unwrap_or_default();

    let display_version = if let Some(ref n) = nacp {
        if !n.version_string.is_empty() {
            n.version_string.clone()
        } else {
            romfs_version
        }
    } else {
        romfs_version
    };

    Some(PackageLoaderInfo {
        nacp,
        icon,
        display_version,
    })
}

pub fn probe_dump_metadata(dump_root: &Path, nacp_language_index: i32) -> Metadata {
    let base = find_directory_ci(dump_root, "base");
    let updates = find_directory_ci(dump_root, "updates");

    let mut update_dirs: Vec<(u64, PathBuf)> = Vec::new();
    if let Some(ref updates_dir) = updates {
        if let Ok(entries) = fs::read_dir(updates_dir) {
            for entry in entries.flatten() {
                let path = entry.path();
                if path.is_dir() {
                    if let Some(version) = entry
                        .file_name()
                        .to_str()
                        .and_then(|s| s.parse::<u64>().ok())
                    {
                        update_dirs.push((version, path));
                    }
                }
            }
        }
    }
    update_dirs.sort_by(|a, b| b.0.cmp(&a.0).then_with(|| b.1.cmp(&a.1)));

    let base_dir = if let Some(ref b) = base {
        Some(b.clone())
    } else if updates.is_none() {
        Some(dump_root.to_path_buf())
    } else {
        None
    };

    fn probe_dir(
        result: &mut Metadata,
        dir: &Path,
        label: String,
        names: bool,
        nacp_language_index: i32,
    ) {
        let Some(info) = probe_package_dir(dir, nacp_language_index) else {
            return;
        };
        if result.display_version.is_empty() && !info.display_version.is_empty() {
            result.display_version = info.display_version;
            result.version_source = label;
        }
        if names {
            if result.title_name.is_empty() || result.publisher.is_empty() {
                if let Some(ref nacp) = info.nacp {
                    if result.title_name.is_empty() {
                        result.title_name = nacp.application_name.clone();
                    }
                    if result.publisher.is_empty() {
                        result.publisher = nacp.developer_name.clone();
                    }
                }
            }
            if result.icon.is_empty() && !info.icon.is_empty() {
                result.icon = info.icon;
            }
        }
    }

    let mut result = Metadata::default();
    for (version, dir) in &update_dirs {
        probe_dir(
            &mut result,
            dir,
            format!("update {version}"),
            false,
            nacp_language_index,
        );
    }
    if let Some(ref dir) = base_dir {
        probe_dir(&mut result, dir, "base".to_string(), true, nacp_language_index);
    }
    for (version, dir) in &update_dirs {
        if !result.title_name.is_empty()
            && !result.icon.is_empty()
            && !result.publisher.is_empty()
        {
            break;
        }
        probe_dir(
            &mut result,
            dir,
            format!("update {version}"),
            true,
            nacp_language_index,
        );
    }
    result
}

pub fn newest_update_index(packages: &[PackageInfo]) -> Option<usize> {
    let mut best: Option<usize> = None;
    for (i, pkg) in packages.iter().enumerate() {
        if pkg.kind != PackageKind::Update {
            continue;
        }
        if best.is_none_or(|b| pkg.version > packages[b].version) {
            best = Some(i);
        }
    }
    best
}

pub fn nup_edit_target_index(packages: &[PackageInfo]) -> usize {
    packages
        .iter()
        .position(|p| p.kind == PackageKind::Base)
        .unwrap_or(0)
}

pub fn probe_nup_metadata(archive: &Path) -> Metadata {
    let mut out = Metadata::default();
    let Ok(reader) = FileReader::open(archive) else {
        return out;
    };
    let Ok(nup) = NupContainer::parse(&reader) else {
        return out;
    };
    if nup.packages.is_empty() {
        return out;
    }
    let target = nup_edit_target_index(&nup.packages);
    let pkg = &nup.packages[target];
    out.title_name = pkg.title_name.clone();
    out.icon = pkg.icon.clone();
    out.publisher = pkg.publisher.clone();
    out.localized_titles = pkg.localized_titles.clone();
    if let Some(update) = newest_update_index(&nup.packages) {
        if !nup.packages[update].display_version.is_empty() {
            out.display_version = nup.packages[update].display_version.clone();
            out.version_source = "NUP".to_string();
            return out;
        }
    }
    out.display_version = pkg.display_version.clone();
    out.version_source = "NUP".to_string();
    out
}

pub fn edit_nup_metadata(
    archive: &Path,
    edit: &MetadataEdit,
    cancel: Option<&AtomicBool>,
    progress_cb: Option<&mut dyn FnMut(&PackProgress)>,
) -> Result<EditResult, (EditResult, String)> {
    edit_nup_metadata_with_cancel_poll(archive, edit, cancel, None, progress_cb)
}

pub(crate) fn edit_nup_metadata_with_cancel_poll(
    archive: &Path,
    edit: &MetadataEdit,
    cancel: Option<&AtomicBool>,
    cancel_poll: Option<&dyn Fn() -> bool>,
    mut progress_cb: Option<&mut dyn FnMut(&PackProgress)>,
) -> Result<EditResult, (EditResult, String)> {
    if edit.title_name.is_none()
        && edit.icon.is_none()
        && edit.display_version.is_none()
        && edit.publisher.is_none()
        && edit.localized_titles.is_none()
    {
        return Ok(EditResult::NoChange);
    }
    if is_cancelled(cancel, cancel_poll) {
        return Err((EditResult::Cancelled, "cancelled".to_string()));
    }
    if !archive.is_file() {
        return Err((
            EditResult::InvalidSource,
            format!("not a file: {}", archive.display()),
        ));
    }

    let (
        target,
        update,
        package_total,
        cur_title,
        cur_version,
        cur_update_version,
        cur_publisher,
        cur_icon,
        cur_localized,
    ) = {
        let reader = FileReader::open(archive).map_err(|e| {
            (
                EditResult::IoError,
                format!("cannot open {}: {e}", archive.display()),
            )
        })?;
        let nup = NupContainer::parse(&reader).map_err(|_| {
            (
                EditResult::InvalidSource,
                format!("not a valid NUP file: {}", archive.display()),
            )
        })?;
        if nup.packages.is_empty() {
            return Err((
                EditResult::InvalidSource,
                format!("not a valid NUP file: {}", archive.display()),
            ));
        }
        let target = nup_edit_target_index(&nup.packages);
        let mut update = newest_update_index(&nup.packages);
        if update == Some(target) {
            update = None;
        }
        let cur = &nup.packages[target];
        let cur_update_version = update
            .map(|u| nup.packages[u].display_version.clone())
            .unwrap_or_else(|| cur.display_version.clone());
        (
            target,
            update,
            nup.packages.len(),
            cur.title_name.clone(),
            cur.display_version.clone(),
            cur_update_version,
            cur.publisher.clone(),
            cur.icon.clone(),
            cur.localized_titles.clone(),
        )
    };

    let same_str = |wanted: &Option<String>, have: &str| wanted.as_deref().is_none_or(|w| w == have);
    let icon_same = edit.icon.as_deref().is_none_or(|w| w == cur_icon);
    let loc_same = edit
        .localized_titles
        .as_deref()
        .is_none_or(|w| w == cur_localized);
    if same_str(&edit.title_name, &cur_title)
        && icon_same
        && loc_same
        && same_str(&edit.display_version, &cur_version)
        && same_str(&edit.display_version, &cur_update_version)
        && same_str(&edit.publisher, &cur_publisher)
    {
        return Ok(EditResult::NoChange);
    }

    let mut infile = File::open(archive).map_err(|e| {
        (
            EditResult::IoError,
            format!("cannot open {}: {e}", archive.display()),
        )
    })?;
    let file_size = infile
        .metadata()
        .map_err(|e| (EditResult::IoError, e.to_string()))?
        .len();
    if file_size < SUPERBLOCK_SIZE as u64 {
        return Err((EditResult::InvalidSource, "file too small".to_string()));
    }
    let mut super_buf = vec![0u8; SUPERBLOCK_SIZE];
    infile
        .read_exact(&mut super_buf)
        .map_err(|_| (EditResult::IoError, "cannot read superblock".to_string()))?;
    if super_buf[..4] != HEADER_MAGIC {
        return Err((EditResult::InvalidSource, "not a NUP file".to_string()));
    }

    let toc_offset = read_u64_le(&super_buf, 0x08);
    let toc_size = read_u64_le(&super_buf, 0x10);
    let blob_offset = read_u64_le(&super_buf, 0x38);
    let super_file_size = read_u64_le(&super_buf, 0x40);
    let package_count = read_u32_le(&super_buf, 0x48) as usize;

    if toc_offset != SUPERBLOCK_SIZE as u64
        || toc_size == 0
        || !blob_offset.is_multiple_of(EXTENT_ALIGNMENT)
        || super_file_size != file_size
        || package_count != package_total
        || file_size > i64::MAX as u64
        || blob_offset > file_size
        || toc_offset > blob_offset
        || toc_size > blob_offset - toc_offset
    {
        return Err((
            EditResult::InvalidSource,
            "NUP superblock mismatch".to_string(),
        ));
    }

    let toc_len = usize::try_from(toc_size).map_err(|_| {
        (
            EditResult::InvalidSource,
            "NUP superblock mismatch".to_string(),
        )
    })?;
    infile
        .seek(SeekFrom::Start(toc_offset))
        .map_err(|_| (EditResult::IoError, "cannot seek to TOC".to_string()))?;
    let mut toc = vec![0u8; toc_len];
    infile
        .read_exact(&mut toc)
        .map_err(|_| (EditResult::IoError, "cannot read TOC".to_string()))?;

    let mut raws: Vec<(u64, u32)> = Vec::with_capacity(package_count);
    let mut meta_ranges: Vec<(u64, u64)> = Vec::new();
    let mut pos = TOC_HEADER_SIZE;
    for _ in 0..package_count {
        if pos > toc.len() || PACKAGE_RECORD_SIZE > toc.len() - pos {
            return Err((EditResult::InvalidSource, "TOC too small".to_string()));
        }
        let meta_offset = read_u64_le(&toc, pos + 0x20);
        let meta_size = read_u32_le(&toc, pos + 0x28);
        if meta_size > 0 {
            if meta_offset < toc_offset + toc_size
                || meta_offset > file_size
                || (meta_size as u64) > file_size - meta_offset
            {
                return Err((EditResult::InvalidSource, "bad meta extent".to_string()));
            }
            meta_ranges.push((meta_offset, meta_offset + meta_size as u64));
        }
        raws.push((meta_offset, meta_size));
        pos += PACKAGE_RECORD_SIZE;
    }

    meta_ranges.sort_unstable();
    let mut merged_ranges: Vec<(u64, u64)> = Vec::with_capacity(meta_ranges.len());
    for r in meta_ranges {
        if let Some(last) = merged_ranges.last_mut() {
            if r.0 <= last.1 {
                last.1 = last.1.max(r.1);
                continue;
            }
        }
        merged_ranges.push(r);
    }

    let dir_count = read_u64_le(&toc, 0x08);
    let file_count = read_u64_le(&toc, 0x10);
    if dir_count > ((toc.len() - pos) / DIR_RECORD_SIZE) as u64 {
        return Err((
            EditResult::InvalidSource,
            "bad directory table size".to_string(),
        ));
    }
    pos += (dir_count as usize) * DIR_RECORD_SIZE;
    if file_count > ((toc.len() - pos) / FILE_RECORD_SIZE) as u64 {
        return Err((EditResult::InvalidSource, "bad file table size".to_string()));
    }

    let mut data_end = blob_offset.max(merged_ranges.first().map_or(file_size, |r| r.0));
    for _ in 0..file_count {
        let extent = read_u64_le(&toc, pos + 0x10);
        let size = read_u64_le(&toc, pos + 0x18);
        pos += FILE_RECORD_SIZE;
        if size == 0 {
            continue;
        }
        if extent < blob_offset || extent > file_size || size > file_size - extent {
            return Err((EditResult::InvalidSource, "bad file extent".to_string()));
        }
        let idx = merged_ranges.partition_point(|r| r.1 <= extent);
        if idx < merged_ranges.len() && merged_ranges[idx].0 < extent + size {
            return Err((
                EditResult::InvalidSource,
                "file extent overlaps metadata".to_string(),
            ));
        }
        data_end = data_end.max(extent + size);
    }

    let mut new_metas: Vec<Vec<u8>> = Vec::with_capacity(raws.len());
    for (i, &(meta_off, meta_sz)) in raws.iter().enumerate() {
        let mut block = Vec::new();
        if meta_sz > 0 {
            infile
                .seek(SeekFrom::Start(meta_off))
                .map_err(|_| (EditResult::IoError, "cannot seek to meta block".to_string()))?;
            block.resize(meta_sz as usize, 0);
            infile
                .read_exact(&mut block)
                .map_err(|_| (EditResult::IoError, "cannot read meta block".to_string()))?;
        }
        if i == target || (edit.display_version.is_some() && update == Some(i)) {
            let mut tlvs = parse_tlvs(&block).map_err(|e| (EditResult::InvalidSource, e))?;
            if i == target {
                if let Some(ref title) = edit.title_name {
                    set_or_remove_tlv(&mut tlvs, MetaType::TitleName as u16, Some(title.as_bytes()));
                }
                if let Some(ref publisher) = edit.publisher {
                    set_or_remove_tlv(
                        &mut tlvs,
                        MetaType::Publisher as u16,
                        Some(publisher.as_bytes()),
                    );
                }
                if let Some(ref icon) = edit.icon {
                    set_or_remove_tlv(&mut tlvs, MetaType::Icon as u16, Some(icon));
                }
                if let Some(ref loc) = edit.localized_titles {
                    let bytes = serialize_localized_titles(loc);
                    set_or_remove_tlv(&mut tlvs, MetaType::LocalizedTitles as u16, Some(&bytes));
                }
            }
            if let Some(ref ver) = edit.display_version {
                set_or_remove_tlv(
                    &mut tlvs,
                    MetaType::DisplayVersion as u16,
                    Some(ver.as_bytes()),
                );
            }
            block = serialize_tlvs(tlvs);
        }
        new_metas.push(block);
    }

    let mut cursor = data_end;
    let mut meta_addrs: Vec<(u64, u32)> = Vec::with_capacity(raws.len());
    for block in &new_metas {
        if block.is_empty() {
            meta_addrs.push((0, 0));
        } else {
            let len_u32 = u32::try_from(block.len()).map_err(|_| {
                (
                    EditResult::IoError,
                    "package metadata is too large".to_string(),
                )
            })?;
            if (block.len() as u64) > (i64::MAX as u64) - cursor {
                return Err((
                    EditResult::IoError,
                    "edited archive is too large".to_string(),
                ));
            }
            meta_addrs.push((cursor, len_u32));
            cursor += block.len() as u64;
        }
    }
    let new_file_size = cursor;

    let mut eprogress = PackProgress {
        bytes_done: 0,
        bytes_total: new_file_size,
        files_done: 0,
        files_total: 1,
    };
    let mut last_write_report = 0u64;
    if let Some(cb) = progress_cb.as_deref_mut() {
        cb(&eprogress);
    }

    let mut new_toc = toc;
    let mut tpos = TOC_HEADER_SIZE;
    for &(off, sz) in &meta_addrs {
        put_u64_le(&mut new_toc, tpos + 0x20, off);
        put_u32_le(&mut new_toc, tpos + 0x28, sz);
        tpos += PACKAGE_RECORD_SIZE;
    }
    let toc_hash = sha256(&new_toc);

    let mut new_super = super_buf;
    new_super[0x18..0x38].copy_from_slice(&toc_hash);
    put_u64_le(&mut new_super, 0x40, new_file_size);

    let mut part_os = archive.as_os_str().to_os_string();
    part_os.push(".part");
    let part = PathBuf::from(part_os);
    if part.exists() {
        return Err((
            EditResult::IoError,
            format!("temporary file already exists: {}", part.display()),
        ));
    }

    let write_result = (|| -> Result<(), (EditResult, String)> {
        let mut outfile = File::create(&part).map_err(|e| {
            (
                EditResult::IoError,
                format!("cannot create {}: {e}", part.display()),
            )
        })?;
        let mut write_all = |data: &[u8]| -> Result<(), (EditResult, String)> {
            outfile.write_all(data).map_err(|_| {
                (
                    EditResult::IoError,
                    "write failed, the disk may be full".to_string(),
                )
            })?;
            eprogress.bytes_done += data.len() as u64;
            if eprogress.bytes_done - last_write_report >= PROGRESS_STEP {
                last_write_report = eprogress.bytes_done;
                if let Some(cb) = progress_cb.as_deref_mut() {
                    cb(&eprogress);
                }
            }
            Ok(())
        };

        write_all(&new_super)?;
        write_all(&new_toc)?;

        let toc_end = SUPERBLOCK_SIZE as u64 + new_toc.len() as u64;
        if toc_end > blob_offset {
            return Err((
                EditResult::IoError,
                "rewritten TOC overlaps the blob region".to_string(),
            ));
        }
        let mut gap = blob_offset - toc_end;
        let zeros = [0u8; 4096];
        while gap > 0 {
            let chunk = (gap as usize).min(zeros.len());
            write_all(&zeros[..chunk])?;
            gap -= chunk as u64;
        }

        let mut buffer = vec![0u8; READ_BUFFER_SIZE];
        let mut offset = blob_offset;
        let mut meta_idx = 0usize;
        infile
            .seek(SeekFrom::Start(blob_offset))
            .map_err(|_| (EditResult::IoError, "cannot seek to blob".to_string()))?;

        while offset < data_end {
            if is_cancelled(cancel, cancel_poll) {
                return Err((EditResult::Cancelled, "cancelled".to_string()));
            }
            let chunk = (data_end - offset).min(buffer.len() as u64) as usize;
            let got = infile
                .read(&mut buffer[..chunk])
                .map_err(|e| (EditResult::IoError, e.to_string()))?;
            if got == 0 {
                return Err((EditResult::IoError, "short read of blob".to_string()));
            }
            let end = offset + got as u64;
            while meta_idx < merged_ranges.len() && merged_ranges[meta_idx].0 < end {
                let r = merged_ranges[meta_idx];
                if r.1 > offset {
                    let start = (r.0.max(offset) - offset) as usize;
                    let stop = (r.1.min(end) - offset) as usize;
                    buffer[start..stop].fill(0);
                }
                if r.1 > end {
                    break;
                }
                meta_idx += 1;
            }
            write_all(&buffer[..got])?;
            offset = end;
        }

        for block in &new_metas {
            if is_cancelled(cancel, cancel_poll) {
                return Err((EditResult::Cancelled, "cancelled".to_string()));
            }
            if !block.is_empty() {
                write_all(block)?;
            }
        }
        eprogress.files_done = 1;
        if let Some(cb) = progress_cb.as_deref_mut() {
            cb(&eprogress);
        }
        outfile
            .set_len(new_file_size)
            .and_then(|_| outfile.sync_all())
            .map_err(|_| {
                (
                    EditResult::IoError,
                    format!("cannot finish {}", part.display()),
                )
            })?;
        Ok(())
    })();

    if let Err(err) = write_result {
        let _ = fs::remove_file(&part);
        return Err(err);
    }
    drop(infile);

    // Validate rewritten archive before replacing the original.
    {
        let ok = FileReader::open(&part)
            .ok()
            .and_then(|r| NupContainer::parse(&r).ok())
            .is_some();
        if !ok {
            let _ = fs::remove_file(&part);
            return Err((
                EditResult::IoError,
                "edited file failed validation".to_string(),
            ));
        }
    }

    if let Err(e) = fs::rename(&part, archive) {
        let _ = fs::remove_file(&part);
        return Err((
            EditResult::IoError,
            format!("cannot replace archive: {e}"),
        ));
    }

    Ok(EditResult::Success)
}
