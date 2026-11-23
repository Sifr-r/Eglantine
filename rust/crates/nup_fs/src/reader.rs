// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::collections::HashMap;
use std::fs::{self, File};
use std::io::{self, Seek, SeekFrom, Write};
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};

use crate::format::{
    read_u16_le, read_u32_le, read_u64_le, sha256, ReadAt, ZstdCodec, CODEC_RAW, CODEC_ZSTD,
    DIR_RECORD_SIZE, FILE_RECORD_SIZE, HEADER_MAGIC, MAX_DIR_DEPTH, NO_DIR,
    PACKAGE_FLAG_STANDALONE_UPDATE, PACKAGE_RECORD_SIZE, PROGRESS_STEP, READ_BUFFER_SIZE,
    SUPERBLOCK_SIZE, TOC_HEADER_SIZE, TOC_MAGIC,
};
use crate::metadata::parse_package_meta;
use crate::writer::PackProgress;

#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PackageKind {
    Base = 0,
    Update = 1,
    Addon = 2,
}

impl PackageKind {
    pub const fn from_u8(raw: u8) -> Option<Self> {
        match raw {
            0 => Some(Self::Base),
            1 => Some(Self::Update),
            2 => Some(Self::Addon),
            _ => None,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PackageInfo {
    pub title_id: u64,
    pub version: u32,
    pub kind: PackageKind,
    pub standalone_update: bool,
    pub root_dir: u32,
    pub exefs_dir: u32,
    pub romfs_dir: u32,
    pub meta_offset: u64,
    pub meta_size: u32,

    pub npdm: Vec<u8>,
    pub title_name: String,
    pub publisher: String,
    pub source_dump: String,
    pub localized_titles: Vec<(String, String)>,
    pub icon: Vec<u8>,
    pub localized_icons: Vec<(String, Vec<u8>)>,
    pub display_version: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DirNode {
    pub name: String,
    pub first_child: u32,
    pub child_count: u32,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FileNode {
    pub name: String,
    pub size: u64,
    pub sha256: [u8; 32],
    pub extent_offset: u64,
    pub extent_size: u64,
    pub codec: u8,
}

pub struct SliceReader<'a>(pub &'a [u8]);

impl<'a> ReadAt for SliceReader<'a> {
    fn len(&self) -> u64 {
        self.0.len() as u64
    }

    fn read_at(&self, offset: u64, dst: &mut [u8]) -> io::Result<usize> {
        let Ok(off) = usize::try_from(offset) else {
            return Ok(0);
        };
        if off >= self.0.len() {
            return Ok(0);
        }
        let avail = (self.0.len() - off).min(dst.len());
        dst[..avail].copy_from_slice(&self.0[off..off + avail]);
        Ok(avail)
    }
}

pub struct FileReader {
    file: std::sync::Mutex<File>,
    len: u64,
}

impl FileReader {
    pub fn open(path: &Path) -> io::Result<Self> {
        let file = File::open(path)?;
        let len = file.metadata()?.len();
        Ok(Self {
            file: std::sync::Mutex::new(file),
            len,
        })
    }
}

impl ReadAt for FileReader {
    fn len(&self) -> u64 {
        self.len
    }

    fn read_at(&self, offset: u64, dst: &mut [u8]) -> io::Result<usize> {
        let mut f = self
            .file
            .lock()
            .map_err(|_| io::Error::new(io::ErrorKind::Other, "lock poisoned"))?;
        f.seek(SeekFrom::Start(offset))?;
        io::Read::read(&mut *f, dst)
    }
}

#[derive(Clone, Debug)]
pub struct NupContainer {
    pub toc_offset: u64,
    pub toc_size: u64,
    pub blob_offset: u64,
    pub file_size: u64,
    pub packages: Vec<PackageInfo>,
    pub dirs: Vec<DirNode>,
    pub files: Vec<FileNode>,
    pub children: Vec<u32>,
}

impl NupContainer {
    pub fn is_nup_magic(bytes: &[u8], total_size: u64) -> bool {
        total_size >= SUPERBLOCK_SIZE as u64
            && bytes.len() >= 4
            && bytes[..4] == HEADER_MAGIC
    }

    pub fn parse<R: ReadAt + ?Sized>(reader: &R) -> Result<Self, String> {
        let total_size = reader.len();
        if total_size < SUPERBLOCK_SIZE as u64 {
            return Err("file smaller than superblock".to_string());
        }
        let super_buf = reader
            .read_exact_vec(0, SUPERBLOCK_SIZE)
            .map_err(|e| e.to_string())?;
        if super_buf[..4] != HEADER_MAGIC {
            return Err("invalid NUP magic".to_string());
        }
        let header_size = read_u16_le(&super_buf, 0x04) as usize;
        let _header_version = read_u16_le(&super_buf, 0x06);
        let toc_offset = read_u64_le(&super_buf, 0x08);
        let toc_size = read_u64_le(&super_buf, 0x10);
        let mut toc_sha256 = [0u8; 32];
        toc_sha256.copy_from_slice(&super_buf[0x18..0x38]);
        let blob_offset = read_u64_le(&super_buf, 0x38);
        let file_size = read_u64_le(&super_buf, 0x40);
        let super_package_count = read_u32_le(&super_buf, 0x48);
        let flags = read_u32_le(&super_buf, 0x4C);

        if header_size < SUPERBLOCK_SIZE || flags != 0 {
            return Err("invalid superblock header".to_string());
        }
        if file_size != total_size
            || toc_offset < SUPERBLOCK_SIZE as u64
            || blob_offset > file_size
            || toc_offset > blob_offset
            || toc_size > blob_offset - toc_offset
            || blob_offset % 0x10 != 0
        {
            return Err("superblock bounds check failed".to_string());
        }

        let toc_len = usize::try_from(toc_size).map_err(|_| "TOC too large".to_string())?;
        let toc = reader
            .read_exact_vec(toc_offset, toc_len)
            .map_err(|e| e.to_string())?;
        if sha256(&toc) != toc_sha256 {
            return Err("NUP TOC SHA-256 mismatch".to_string());
        }

        if toc.len() < TOC_HEADER_SIZE {
            return Err("TOC smaller than header".to_string());
        }
        let toc_magic = read_u32_le(&toc, 0x00);
        let package_count = read_u32_le(&toc, 0x04);
        let dir_count = read_u64_le(&toc, 0x08);
        let file_count = read_u64_le(&toc, 0x10);
        let pool_offset = read_u64_le(&toc, 0x18);
        let pool_size = read_u64_le(&toc, 0x20);

        if toc_magic != TOC_MAGIC || package_count == 0 || package_count != super_package_count {
            return Err("invalid TOC header".to_string());
        }
        if dir_count > (toc.len() / DIR_RECORD_SIZE) as u64
            || file_count > (toc.len() / FILE_RECORD_SIZE) as u64
        {
            return Err("TOC record counts exceed TOC size".to_string());
        }

        let needed = (TOC_HEADER_SIZE as u64)
            .checked_add((package_count as u64) * (PACKAGE_RECORD_SIZE as u64))
            .and_then(|v| v.checked_add(dir_count * (DIR_RECORD_SIZE as u64)))
            .and_then(|v| v.checked_add(file_count * (FILE_RECORD_SIZE as u64)))
            .ok_or_else(|| "TOC size arithmetic overflow".to_string())?;
        let toc_len_u64 = toc.len() as u64;
        if needed > toc_len_u64
            || pool_offset < needed
            || pool_offset > toc_len_u64
            || pool_size > toc_len_u64 - pool_offset
            || (pool_offset - needed) % 4 != 0
        {
            return Err("TOC pool/children bounds check failed".to_string());
        }

        let children_offset = needed as usize;
        let children_bytes = (pool_offset - needed) as usize;
        let pool_off_usize = pool_offset as usize;
        let pool_size_u64 = pool_size;

        let mut pos = TOC_HEADER_SIZE;
        let mut packages = Vec::with_capacity(package_count as usize);
        for _ in 0..package_count {
            let title_id = read_u64_le(&toc, pos);
            let root_dir = read_u32_le(&toc, pos + 0x08);
            let exefs_dir = read_u32_le(&toc, pos + 0x0C);
            let romfs_dir = read_u32_le(&toc, pos + 0x10);
            let version = read_u32_le(&toc, pos + 0x14);
            let kind_raw = toc[pos + 0x18];
            let pkg_flags = toc[pos + 0x19];
            let raw_meta_offset = read_u64_le(&toc, pos + 0x20);
            let raw_meta_size = read_u32_le(&toc, pos + 0x28);
            pos += PACKAGE_RECORD_SIZE;

            let Some(kind) = PackageKind::from_u8(kind_raw) else {
                return Err("invalid package kind".to_string());
            };
            if (pkg_flags & !PACKAGE_FLAG_STANDALONE_UPDATE) != 0
                || (root_dir as u64) >= dir_count
                || (exefs_dir != NO_DIR && (exefs_dir as u64) >= dir_count)
                || (romfs_dir != NO_DIR && (romfs_dir as u64) >= dir_count)
            {
                return Err("invalid package record".to_string());
            }

            let (meta_offset, meta_size) = if raw_meta_size > 0
                && raw_meta_offset != 0
                && raw_meta_offset <= file_size
                && (raw_meta_size as u64) <= file_size - raw_meta_offset
            {
                (raw_meta_offset, raw_meta_size)
            } else {
                (0, 0)
            };

            packages.push(PackageInfo {
                title_id,
                version,
                kind,
                standalone_update: (pkg_flags & PACKAGE_FLAG_STANDALONE_UPDATE) != 0,
                root_dir,
                exefs_dir,
                romfs_dir,
                meta_offset,
                meta_size,
                npdm: Vec::new(),
                title_name: String::new(),
                publisher: String::new(),
                source_dump: String::new(),
                localized_titles: Vec::new(),
                icon: Vec::new(),
                localized_icons: Vec::new(),
                display_version: String::new(),
            });
        }

        let dir_count_usize = dir_count as usize;
        let mut dirs = Vec::with_capacity(dir_count_usize);
        for _ in 0..dir_count_usize {
            let name_offset = read_u32_le(&toc, pos) as u64;
            let name_length = read_u32_le(&toc, pos + 0x04) as u64;
            let first_child = read_u32_le(&toc, pos + 0x08);
            let child_count = read_u32_le(&toc, pos + 0x0C);
            pos += DIR_RECORD_SIZE;

            if (first_child as u64) + (child_count as u64) > (children_bytes / 4) as u64
                || name_offset + name_length > pool_size_u64
            {
                return Err("invalid dir record".to_string());
            }
            let start = pool_off_usize + name_offset as usize;
            let end = start + name_length as usize;
            let name = String::from_utf8_lossy(&toc[start..end]).into_owned();
            dirs.push(DirNode {
                name,
                first_child,
                child_count,
            });
        }

        let file_count_usize = file_count as usize;
        let mut files = Vec::with_capacity(file_count_usize);
        for _ in 0..file_count_usize {
            let name_offset = read_u32_le(&toc, pos) as u64;
            let name_length = read_u32_le(&toc, pos + 0x04) as u64;
            let size = read_u64_le(&toc, pos + 0x08);
            let extent_offset = read_u64_le(&toc, pos + 0x10);
            let extent_size = read_u64_le(&toc, pos + 0x18);
            let mut file_sha = [0u8; 32];
            file_sha.copy_from_slice(&toc[pos + 0x20..pos + 0x40]);
            let codec = toc[pos + 0x40];
            let codec_flags = toc[pos + 0x41];
            pos += FILE_RECORD_SIZE;

            if codec > CODEC_ZSTD
                || codec_flags != 0
                || (size == 0 && (extent_offset != 0 || extent_size != 0))
                || (size != 0
                    && (extent_size == 0
                        || extent_offset < blob_offset
                        || extent_offset % 0x10 != 0
                        || extent_offset > file_size
                        || extent_size > file_size - extent_offset))
                || name_offset + name_length > pool_size_u64
            {
                return Err("invalid file record".to_string());
            }
            let start = pool_off_usize + name_offset as usize;
            let end = start + name_length as usize;
            let name = String::from_utf8_lossy(&toc[start..end]).into_owned();
            files.push(FileNode {
                name,
                size,
                sha256: file_sha,
                extent_offset,
                extent_size,
                codec,
            });
        }

        let child_count = children_bytes / 4;
        let mut children = Vec::with_capacity(child_count);
        let total_nodes = dir_count + file_count;
        for chunk in toc[children_offset..children_offset + children_bytes].chunks_exact(4) {
            let child = u32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]);
            if (child as u64) >= total_nodes {
                return Err("child index out of range".to_string());
            }
            children.push(child);
        }

        let mut container = Self {
            toc_offset,
            toc_size,
            blob_offset,
            file_size,
            packages,
            dirs,
            files,
            children,
        };

        container.validate_dir_tree()?;
        for pkg in &mut container.packages {
            parse_package_meta(reader, pkg)?;
        }
        Ok(container)
    }

    fn validate_dir_tree(&self) -> Result<(), String> {
        #[derive(Clone, Copy, PartialEq, Eq)]
        enum State {
            Unvisited,
            InProgress,
            Done,
        }
        #[derive(Clone, Copy)]
        struct Frame {
            dir: usize,
            next: u32,
        }
        let mut state = vec![State::Unvisited; self.dirs.len()];
        let mut height = vec![0u32; self.dirs.len()];
        let mut stack: Vec<Frame> = Vec::new();

        for start in 0..self.dirs.len() {
            if state[start] != State::Unvisited {
                continue;
            }
            state[start] = State::InProgress;
            stack.push(Frame { dir: start, next: 0 });
            while let Some(frame) = stack.last().copied() {
                let node = &self.dirs[frame.dir];
                if frame.next == node.child_count {
                    state[frame.dir] = State::Done;
                    stack.pop();
                    if let Some(parent) = stack.last() {
                        height[parent.dir] = height[parent.dir].max(height[frame.dir] + 1);
                    }
                    continue;
                }
                if let Some(top) = stack.last_mut() {
                    top.next += 1;
                }
                let child = self.children[(node.first_child as usize) + (frame.next as usize)] as usize;
                if child >= self.dirs.len() {
                    continue;
                }
                if state[child] == State::InProgress {
                    return Err("NUP directory tree contains a cycle".to_string());
                }
                if state[child] == State::Done {
                    height[frame.dir] = height[frame.dir].max(height[child] + 1);
                } else {
                    state[child] = State::InProgress;
                    stack.push(Frame { dir: child, next: 0 });
                }
                if stack.len() + (height[child] as usize) > MAX_DIR_DEPTH {
                    return Err("NUP directory tree is nested too deeply".to_string());
                }
            }
        }
        Ok(())
    }

    pub fn find_package(&self, kind: PackageKind, title_id: Option<u64>) -> Option<usize> {
        self.packages.iter().position(|p| {
            p.kind == kind && title_id.map_or(true, |tid| p.title_id == tid)
        })
    }

    pub fn find_base_package(&self) -> Option<usize> {
        self.find_package(PackageKind::Base, None)
    }

    pub fn find_best_update_package(&self) -> Option<usize> {
        let mut best: Option<usize> = None;
        for (i, pkg) in self.packages.iter().enumerate() {
            if pkg.kind != PackageKind::Update {
                continue;
            }
            if best.map_or(true, |b| pkg.version > self.packages[b].version) {
                best = Some(i);
            }
        }
        best
    }
}

pub fn read_extent<R: ReadAt + ?Sized>(
    reader: &R,
    zstd: &dyn ZstdCodec,
    extent_offset: u64,
    extent_size: u64,
    uncompressed_size: u64,
    codec: u8,
) -> Result<Vec<u8>, String> {
    if uncompressed_size == 0 {
        return Ok(Vec::new());
    }
    let total = reader.len();
    if extent_size > total || extent_offset > total - extent_size {
        return Err("extent out of range".to_string());
    }
    let ext_len = usize::try_from(extent_size).map_err(|_| "extent too large".to_string())?;
    let raw = reader
        .read_exact_vec(extent_offset, ext_len)
        .map_err(|e| e.to_string())?;
    match codec {
        CODEC_RAW => {
            if raw.len() as u64 != uncompressed_size {
                return Err("raw extent size mismatch".to_string());
            }
            Ok(raw)
        }
        CODEC_ZSTD => {
            let uncomp_len =
                usize::try_from(uncompressed_size).map_err(|_| "output too large".to_string())?;
            zstd.decompress(&raw, uncomp_len)
        }
        _ => Err("unsupported codec".to_string()),
    }
}

pub fn verify_integrity<R: ReadAt + ?Sized>(
    reader: &R,
    zstd: &dyn ZstdCodec,
    mut progress: Option<&mut dyn FnMut(usize, usize) -> bool>,
) -> Result<(), String> {
    let container = NupContainer::parse(reader)?;
    let total = container.files.len();
    for (i, node) in container.files.iter().enumerate() {
        let content = read_extent(
            reader,
            zstd,
            node.extent_offset,
            node.extent_size,
            node.size,
            node.codec,
        )?;
        if content.len() as u64 != node.size {
            return Err(format!("short read for {}", node.name));
        }
        if sha256(&content) != node.sha256 {
            return Err(format!("SHA-256 mismatch for {}", node.name));
        }
        if let Some(cb) = progress.as_deref_mut() {
            if !cb(i + 1, total) {
                return Err("integrity check cancelled".to_string());
            }
        }
    }
    Ok(())
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ExtractResult {
    Success = 0,
    Cancelled = 1,
    InvalidSource = 2,
    IoError = 3,
}

fn checked_name(name: &str) -> Result<(), (ExtractResult, String)> {
    let invalid = name.is_empty()
        || name == "."
        || name == ".."
        || name.contains('/')
        || name.contains('\\')
        || name.contains(':')
        || name.contains('\0');
    if invalid {
        return Err((
            ExtractResult::InvalidSource,
            format!("invalid file name in archive: {name}"),
        ));
    }
    let mut comps = Path::new(name).components();
    match (comps.next(), comps.next()) {
        (Some(Component::Normal(_)), None) => Ok(()),
        _ => Err((
            ExtractResult::InvalidSource,
            format!("invalid file name in archive: {name}"),
        )),
    }
}

fn count_dir(container: &NupContainer, dir_index: usize, files: &mut u64, bytes: &mut u64) {
    let node = &container.dirs[dir_index];
    let first = node.first_child as usize;
    let end = first + node.child_count as usize;
    for slot in first..end {
        let child = container.children[slot] as usize;
        if child >= container.dirs.len() {
            let f = &container.files[child - container.dirs.len()];
            *files += 1;
            *bytes += f.size;
        } else {
            count_dir(container, child, files, bytes);
        }
    }
}

fn write_dir(
    container: &NupContainer,
    reader: &dyn ReadAt,
    zstd: &dyn ZstdCodec,
    dir_index: usize,
    dest: &Path,
    progress: &mut PackProgress,
    progress_cb: &mut Option<&mut dyn FnMut(&PackProgress)>,
    cancel: Option<&AtomicBool>,
    last_report: &mut u64,
) -> Result<(), (ExtractResult, String)> {
    fs::create_dir_all(dest).map_err(|e| {
        (
            ExtractResult::IoError,
            format!("cannot create {}: {e}", dest.display()),
        )
    })?;

    let node = &container.dirs[dir_index];
    let first = node.first_child as usize;
    let end = first + node.child_count as usize;

    // Subdirectories first, then files (matches WriteDir in nup_writer.cpp).
    for slot in first..end {
        if cancel.is_some_and(|c| c.load(Ordering::Relaxed)) {
            return Err((ExtractResult::Cancelled, "cancelled".to_string()));
        }
        let child = container.children[slot] as usize;
        if child < container.dirs.len() {
            let sub = &container.dirs[child];
            checked_name(&sub.name)?;
            write_dir(
                container,
                reader,
                zstd,
                child,
                &dest.join(&sub.name),
                progress,
                progress_cb,
                cancel,
                last_report,
            )?;
        }
    }

    for slot in first..end {
        if cancel.is_some_and(|c| c.load(Ordering::Relaxed)) {
            return Err((ExtractResult::Cancelled, "cancelled".to_string()));
        }
        let child = container.children[slot] as usize;
        if child >= container.dirs.len() {
            let fnode = &container.files[child - container.dirs.len()];
            checked_name(&fnode.name)?;
            let out_path = dest.join(&fnode.name);
            let mut out = File::create(&out_path).map_err(|e| {
                (
                    ExtractResult::IoError,
                    format!("cannot write {}: {e}", out_path.display()),
                )
            })?;

            if fnode.size > 0 {
                if fnode.codec == CODEC_RAW {
                    let mut remaining = fnode.size;
                    let mut off = 0u64;
                    let mut buf = vec![0u8; READ_BUFFER_SIZE.min(remaining as usize)];
                    while remaining > 0 {
                        if cancel.is_some_and(|c| c.load(Ordering::Relaxed)) {
                            return Err((ExtractResult::Cancelled, "cancelled".to_string()));
                        }
                        let chunk = (remaining as usize).min(buf.len());
                        let got = reader
                            .read_at(fnode.extent_offset + off, &mut buf[..chunk])
                            .map_err(|e| (ExtractResult::IoError, e.to_string()))?;
                        if got == 0 {
                            return Err((
                                ExtractResult::IoError,
                                format!("short read of {}", fnode.name),
                            ));
                        }
                        out.write_all(&buf[..got]).map_err(|_| {
                            (
                                ExtractResult::IoError,
                                "write failed, the disk may be full".to_string(),
                            )
                        })?;
                        off += got as u64;
                        remaining -= got as u64;
                        progress.bytes_done += got as u64;
                        if progress.bytes_done - *last_report >= PROGRESS_STEP {
                            *last_report = progress.bytes_done;
                            if let Some(cb) = progress_cb.as_deref_mut() {
                                cb(progress);
                            }
                        }
                    }
                } else {
                    let data = read_extent(
                        reader,
                        zstd,
                        fnode.extent_offset,
                        fnode.extent_size,
                        fnode.size,
                        fnode.codec,
                    )
                    .map_err(|e| (ExtractResult::IoError, e))?;
                    out.write_all(&data).map_err(|_| {
                        (
                            ExtractResult::IoError,
                            "write failed, the disk may be full".to_string(),
                        )
                    })?;
                    progress.bytes_done += data.len() as u64;
                    if progress.bytes_done - *last_report >= PROGRESS_STEP {
                        *last_report = progress.bytes_done;
                        if let Some(cb) = progress_cb.as_deref_mut() {
                            cb(progress);
                        }
                    }
                }
            }

            progress.files_done += 1;
            if let Some(cb) = progress_cb.as_deref_mut() {
                cb(progress);
            }
        }
    }

    Ok(())
}

pub fn extract_nup(
    archive: &Path,
    out_dir: &Path,
    zstd: &dyn ZstdCodec,
    mut progress_cb: Option<&mut dyn FnMut(&PackProgress)>,
    cancel: Option<&AtomicBool>,
) -> Result<(), (ExtractResult, String)> {
    let mut created = false;
    let mut claimed = false;

    let cleanup = |created_flag: bool, claimed_flag: bool| {
        if !claimed_flag {
            return;
        }
        if created_flag {
            let _ = fs::remove_dir_all(out_dir);
        } else if let Ok(entries) = fs::read_dir(out_dir) {
            for entry in entries.flatten() {
                let path = entry.path();
                if path.is_dir() {
                    let _ = fs::remove_dir_all(path);
                } else {
                    let _ = fs::remove_file(path);
                }
            }
        }
    };

    let res = (|| -> Result<(), (ExtractResult, String)> {
        let meta = fs::metadata(archive);
        if !meta.as_ref().is_ok_and(|m| m.is_file()) {
            return Err((
                ExtractResult::InvalidSource,
                format!("not a file: {}", archive.display()),
            ));
        }

        if let Ok(out_meta) = fs::metadata(out_dir) {
            if !out_meta.is_dir() {
                return Err((
                    ExtractResult::IoError,
                    format!("output exists and is not a directory: {}", out_dir.display()),
                ));
            }
            let mut entries = fs::read_dir(out_dir).map_err(|e| {
                (
                    ExtractResult::IoError,
                    format!("cannot list {}: {e}", out_dir.display()),
                )
            })?;
            if entries.next().is_some() {
                return Err((
                    ExtractResult::IoError,
                    format!("output directory already exists: {}", out_dir.display()),
                ));
            }
            claimed = true;
        } else {
            fs::create_dir_all(out_dir).map_err(|e| {
                (
                    ExtractResult::IoError,
                    format!("cannot create {}: {e}", out_dir.display()),
                )
            })?;
            created = true;
            claimed = true;
        }

        let reader = FileReader::open(archive).map_err(|e| {
            (
                ExtractResult::IoError,
                format!("cannot open {}: {e}", archive.display()),
            )
        })?;
        let container = NupContainer::parse(&reader).map_err(|_| {
            (
                ExtractResult::InvalidSource,
                format!("not a valid NUP file: {}", archive.display()),
            )
        })?;
        if container.packages.is_empty() {
            return Err((
                ExtractResult::InvalidSource,
                "NUP has no packages".to_string(),
            ));
        }

        let bare =
            container.packages.len() == 1 && container.packages[0].kind == PackageKind::Base;
        let mut progress = PackProgress::default();
        let mut targets: Vec<(usize, PathBuf)> = Vec::with_capacity(container.packages.len());
        let mut addon_index: HashMap<u64, usize> = HashMap::new();

        for (i, pkg) in container.packages.iter().enumerate() {
            let dest = if bare {
                out_dir.to_path_buf()
            } else {
                match pkg.kind {
                    PackageKind::Base => out_dir.join("base"),
                    PackageKind::Update => out_dir.join("updates").join(pkg.version.to_string()),
                    PackageKind::Addon => {
                        let idx = addon_index.entry(pkg.title_id).or_insert(0);
                        let current = *idx;
                        *idx += 1;
                        out_dir
                            .join("dlc")
                            .join(format!("{:016X}", pkg.title_id))
                            .join(current.to_string())
                    }
                }
            };
            targets.push((i, dest));
            count_dir(
                &container,
                pkg.root_dir as usize,
                &mut progress.files_total,
                &mut progress.bytes_total,
            );
        }

        if let Some(cb) = progress_cb.as_deref_mut() {
            cb(&progress);
        }

        let mut last_report = 0u64;
        for (idx, dest) in targets {
            write_dir(
                &container,
                &reader,
                zstd,
                container.packages[idx].root_dir as usize,
                &dest,
                &mut progress,
                &mut progress_cb,
                cancel,
                &mut last_report,
            )?;
        }

        Ok(())
    })();

    if res.is_err() {
        cleanup(created, claimed);
    }
    res
}
