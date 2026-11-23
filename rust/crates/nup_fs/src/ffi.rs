// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use std::ffi::{c_char, c_int, c_uint, c_void};
use std::io;
use std::mem::align_of;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::PathBuf;
use std::ptr::{self, NonNull};
use std::sync::atomic::{AtomicBool, Ordering};

use crate::format::{ReadAt, ZstdCodec};
use crate::journal::has_resumable_journal;
use crate::metadata::{
    edit_nup_metadata, probe_dump_metadata, probe_nup_metadata, EditResult, Metadata, MetadataEdit,
};
use crate::reader::{extract_nup, read_extent, verify_integrity, ExtractResult, NupContainer};
use crate::writer::{
    default_archive_path, default_extract_path, dump_root_from_main, is_loose_dump, pack_dump,
    PackOptions, PackProgress, PackResult,
};

pub type EglNupProgressCallback =
    Option<unsafe extern "C" fn(ctx: *mut c_void, progress: *const EglNupPackProgress)>;
pub type EglNupCancelCallback = Option<unsafe extern "C" fn(ctx: *mut c_void) -> u8>;
pub type EglNupVerifyProgressCallback =
    Option<unsafe extern "C" fn(ctx: *mut c_void, done: usize, total: usize) -> u8>;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EglNupZstdOps {
    pub compress: Option<
        unsafe extern "C" fn(
            dst: *mut c_void,
            dst_capacity: usize,
            src: *const c_void,
            src_size: usize,
            compression_level: c_int,
        ) -> usize,
    >,
    pub decompress: Option<
        unsafe extern "C" fn(
            dst: *mut c_void,
            dst_capacity: usize,
            src: *const c_void,
            compressed_size: usize,
        ) -> usize,
    >,
    pub compress_bound: Option<unsafe extern "C" fn(src_size: usize) -> usize>,
    pub is_error: Option<unsafe extern "C" fn(code: usize) -> c_uint>,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EglNupVfsOps {
    pub ctx: *mut c_void,
    pub size: u64,
    pub read_at: Option<
        unsafe extern "C" fn(ctx: *mut c_void, offset: u64, dst: *mut u8, len: usize) -> usize,
    >,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EglNupPackResult {
    Success = 0,
    Cancelled = 1,
    InvalidSource = 2,
    IoError = 3,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EglNupExtractResult {
    Success = 0,
    Cancelled = 1,
    InvalidSource = 2,
    IoError = 3,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EglNupEditResult {
    Success = 0,
    Cancelled = 1,
    InvalidSource = 2,
    IoError = 3,
    NoChange = 4,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct EglNupPackProgress {
    pub bytes_done: u64,
    pub bytes_total: u64,
    pub files_done: u64,
    pub files_total: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EglNupLocalizedPair {
    pub lang: *const c_char,
    pub lang_len: usize,
    pub value: *const c_char,
    pub value_len: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EglNupMetadata {
    pub title_name: *const c_char,
    pub title_name_len: usize,
    pub icon: *const u8,
    pub icon_len: usize,
    pub display_version: *const c_char,
    pub display_version_len: usize,
    pub publisher: *const c_char,
    pub publisher_len: usize,
    pub localized_titles: *const EglNupLocalizedPair,
    pub localized_title_count: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EglNupOwnedString {
    pub data: *mut c_char,
    pub len: usize,
}

impl Default for EglNupOwnedString {
    fn default() -> Self {
        Self {
            data: ptr::null_mut(),
            len: 0,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EglNupOwnedBytes {
    pub data: *mut u8,
    pub len: usize,
}

impl Default for EglNupOwnedBytes {
    fn default() -> Self {
        Self {
            data: ptr::null_mut(),
            len: 0,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EglNupOwnedLocalizedPair {
    pub lang: EglNupOwnedString,
    pub value: EglNupOwnedString,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EglNupOwnedLocalizedBytesPair {
    pub lang: EglNupOwnedString,
    pub data: EglNupOwnedBytes,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EglNupOwnedMetadata {
    pub title_name: EglNupOwnedString,
    pub icon: EglNupOwnedBytes,
    pub display_version: EglNupOwnedString,
    pub publisher: EglNupOwnedString,
    pub localized_titles: *mut EglNupOwnedLocalizedPair,
    pub localized_title_count: usize,
    pub version_source: EglNupOwnedString,
}

impl Default for EglNupOwnedMetadata {
    fn default() -> Self {
        Self {
            title_name: EglNupOwnedString::default(),
            icon: EglNupOwnedBytes::default(),
            display_version: EglNupOwnedString::default(),
            publisher: EglNupOwnedString::default(),
            localized_titles: ptr::null_mut(),
            localized_title_count: 0,
            version_source: EglNupOwnedString::default(),
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EglNupPackOptions {
    pub delete_sources: u8,
    pub _pad: [u8; 3],
    pub zstd_level: i32,
    pub chunk_bytes: u64,
    pub raw_threshold: u64,
    pub nacp_language_index: i32,
    pub _pad2: u32,
    pub metadata: EglNupMetadata,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EglNupMetadataEdit {
    pub has_title_name: u8,
    pub has_icon: u8,
    pub has_display_version: u8,
    pub has_publisher: u8,
    pub has_localized_titles: u8,
    pub _pad: [u8; 3],
    pub values: EglNupMetadata,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EglNupPackageInfo {
    pub title_id: u64,
    pub version: u32,
    pub kind: u8,
    pub standalone_update: u8,
    pub _pad: [u8; 2],
    pub root_dir: u32,
    pub exefs_dir: u32,
    pub romfs_dir: u32,
    pub meta_offset: u64,
    pub meta_size: u32,
    pub _pad2: u32,
    pub npdm: EglNupOwnedBytes,
    pub title_name: EglNupOwnedString,
    pub publisher: EglNupOwnedString,
    pub source_dump: EglNupOwnedString,
    pub localized_titles: *mut EglNupOwnedLocalizedPair,
    pub localized_title_count: usize,
    pub icon: EglNupOwnedBytes,
    pub localized_icons: *mut EglNupOwnedLocalizedBytesPair,
    pub localized_icon_count: usize,
    pub display_version: EglNupOwnedString,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EglNupDirNode {
    pub name: EglNupOwnedString,
    pub first_child: u32,
    pub child_count: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EglNupFileNode {
    pub name: EglNupOwnedString,
    pub size: u64,
    pub sha256: [u8; 32],
    pub extent_offset: u64,
    pub extent_size: u64,
    pub codec: u8,
    pub _pad: [u8; 7],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct EglNupParsedContainer {
    pub toc_offset: u64,
    pub toc_size: u64,
    pub blob_offset: u64,
    pub file_size: u64,
    pub packages: *mut EglNupPackageInfo,
    pub package_count: usize,
    pub dirs: *mut EglNupDirNode,
    pub dir_count: usize,
    pub files: *mut EglNupFileNode,
    pub file_count: usize,
    pub children: *mut u32,
    pub child_count: usize,
}

impl Default for EglNupParsedContainer {
    fn default() -> Self {
        Self {
            toc_offset: 0,
            toc_size: 0,
            blob_offset: 0,
            file_size: 0,
            packages: ptr::null_mut(),
            package_count: 0,
            dirs: ptr::null_mut(),
            dir_count: 0,
            files: ptr::null_mut(),
            file_count: 0,
            children: ptr::null_mut(),
            child_count: 0,
        }
    }
}

#[inline]
fn ptr_valid<T>(p: *const T) -> bool {
    !p.is_null() && (p as usize).is_multiple_of(align_of::<T>())
}

#[inline]
fn mut_ptr_valid<T>(p: *mut T) -> bool {
    !p.is_null() && (p as usize).is_multiple_of(align_of::<T>())
}

unsafe fn slice_from_raw_checked<'a, T>(ptr: *const T, len: usize) -> Result<&'a [T], ()> {
    if len == 0 {
        return Ok(&[]);
    }
    if !ptr_valid(ptr) {
        return Err(());
    }
    if len.checked_mul(std::mem::size_of::<T>()).is_none() {
        return Err(());
    }
    // SAFETY: Caller guarantees `[ptr, ptr + len)` is valid for reads across `'a`.
    Ok(unsafe { std::slice::from_raw_parts(ptr, len) })
}

unsafe fn str_from_raw_checked<'a>(ptr: *const c_char, len: usize) -> Result<&'a str, ()> {
    // SAFETY: Validated non-null/aligned via `slice_from_raw_checked`.
    let raw = unsafe { slice_from_raw_checked(ptr.cast::<u8>(), len)? };
    std::str::from_utf8(raw).map_err(|_| ())
}

unsafe fn path_from_raw_checked(ptr: *const c_char, len: usize) -> Result<PathBuf, ()> {
    // SAFETY: Validated via `str_from_raw_checked`.
    let s = unsafe { str_from_raw_checked(ptr, len)? };
    if s.is_empty() {
        return Err(());
    }
    Ok(PathBuf::from(s))
}

fn owned_from_vec<T>(v: Vec<T>) -> (*mut T, usize) {
    if v.is_empty() {
        return (ptr::null_mut(), 0);
    }
    let boxed: Box<[T]> = v.into_boxed_slice();
    let len = boxed.len();
    let raw = Box::into_raw(boxed).cast::<T>();
    (raw, len)
}

unsafe fn free_owned_slice<T>(ptr_ref: &mut *mut T, len_ref: &mut usize) {
    let p = *ptr_ref;
    let len = *len_ref;
    *ptr_ref = ptr::null_mut();
    *len_ref = 0;
    if !p.is_null() && len > 0 && (p as usize).is_multiple_of(align_of::<T>()) {
        let slice_ptr = ptr::slice_from_raw_parts_mut(p, len);
        // SAFETY: `p` came from `Box::<[T]>::into_raw` with length `len`.
        let _ = unsafe { Box::from_raw(slice_ptr) };
    }
}

fn alloc_owned_bytes(bytes: Vec<u8>) -> EglNupOwnedBytes {
    let (data, len) = owned_from_vec(bytes);
    EglNupOwnedBytes { data, len }
}

fn alloc_owned_string(s: String) -> EglNupOwnedString {
    let (data, len) = owned_from_vec(s.into_bytes());
    EglNupOwnedString {
        data: data.cast::<c_char>(),
        len,
    }
}

fn free_owned_string_inner(s: &mut EglNupOwnedString) {
    let mut u8_ptr = s.data.cast::<u8>();
    let mut len = s.len;
    s.data = ptr::null_mut();
    s.len = 0;
    // SAFETY: `s.data` was allocated via `owned_from_vec::<u8>`.
    unsafe { free_owned_slice(&mut u8_ptr, &mut len) };
}

fn free_owned_bytes_inner(b: &mut EglNupOwnedBytes) {
    // SAFETY: `b.data` was allocated via `owned_from_vec::<u8>`.
    unsafe { free_owned_slice(&mut b.data, &mut b.len) };
}

fn alloc_localized_pairs(pairs: Vec<(String, String)>) -> (*mut EglNupOwnedLocalizedPair, usize) {
    let owned: Vec<EglNupOwnedLocalizedPair> = pairs
        .into_iter()
        .map(|(k, v)| EglNupOwnedLocalizedPair {
            lang: alloc_owned_string(k),
            value: alloc_owned_string(v),
        })
        .collect();
    owned_from_vec(owned)
}

fn free_localized_pairs(ptr_ref: &mut *mut EglNupOwnedLocalizedPair, count_ref: &mut usize) {
    let p = *ptr_ref;
    let count = *count_ref;
    *ptr_ref = ptr::null_mut();
    *count_ref = 0;
    if !p.is_null() && count > 0 && (p as usize).is_multiple_of(align_of::<EglNupOwnedLocalizedPair>())
    {
        // SAFETY: Slice allocated via `owned_from_vec::<EglNupOwnedLocalizedPair>`.
        let slice = unsafe { std::slice::from_raw_parts_mut(p, count) };
        for pair in slice {
            free_owned_string_inner(&mut pair.lang);
            free_owned_string_inner(&mut pair.value);
        }
        let mut p_mut = p;
        let mut c_mut = count;
        // SAFETY: Now inner allocations are freed; free outer slice.
        unsafe { free_owned_slice(&mut p_mut, &mut c_mut) };
    }
}

fn free_localized_bytes_pairs(
    ptr_ref: &mut *mut EglNupOwnedLocalizedBytesPair,
    count_ref: &mut usize,
) {
    let p = *ptr_ref;
    let count = *count_ref;
    *ptr_ref = ptr::null_mut();
    *count_ref = 0;
    if !p.is_null()
        && count > 0
        && (p as usize).is_multiple_of(align_of::<EglNupOwnedLocalizedBytesPair>())
    {
        // SAFETY: Slice allocated via `owned_from_vec::<EglNupOwnedLocalizedBytesPair>`.
        let slice = unsafe { std::slice::from_raw_parts_mut(p, count) };
        for pair in slice {
            free_owned_string_inner(&mut pair.lang);
            free_owned_bytes_inner(&mut pair.data);
        }
        let mut p_mut = p;
        let mut c_mut = count;
        // SAFETY: Now inner allocations are freed; free outer slice.
        unsafe { free_owned_slice(&mut p_mut, &mut c_mut) };
    }
}

fn write_error_string(out_error: *mut EglNupOwnedString, msg: String) {
    if mut_ptr_valid(out_error) {
        // SAFETY: `out_error` validated non-null and aligned.
        unsafe {
            free_owned_string_inner(&mut *out_error);
            *out_error = alloc_owned_string(msg);
        }
    }
}

fn clear_error_string(out_error: *mut EglNupOwnedString) {
    if mut_ptr_valid(out_error) {
        // SAFETY: `out_error` validated non-null and aligned.
        unsafe {
            free_owned_string_inner(&mut *out_error);
        }
    }
}

struct FfiZstdCodec {
    compress_fn: unsafe extern "C" fn(*mut c_void, usize, *const c_void, usize, c_int) -> usize,
    decompress_fn: unsafe extern "C" fn(*mut c_void, usize, *const c_void, usize) -> usize,
    compress_bound_fn: unsafe extern "C" fn(usize) -> usize,
    is_error_fn: unsafe extern "C" fn(usize) -> c_uint,
}

impl FfiZstdCodec {
    unsafe fn from_raw(ptr: *const EglNupZstdOps) -> Result<Self, ()> {
        if !ptr_valid(ptr) {
            return Err(());
        }
        // SAFETY: Validated non-null and aligned `EglNupZstdOps`.
        let ops = unsafe { *ptr };
        let (Some(compress_fn), Some(decompress_fn), Some(compress_bound_fn), Some(is_error_fn)) =
            (ops.compress, ops.decompress, ops.compress_bound, ops.is_error)
        else {
            return Err(());
        };
        Ok(Self {
            compress_fn,
            decompress_fn,
            compress_bound_fn,
            is_error_fn,
        })
    }
}

impl ZstdCodec for FfiZstdCodec {
    fn compress_bound(&self, src_size: usize) -> usize {
        // SAFETY: Function pointer validated non-null on construction.
        unsafe { (self.compress_bound_fn)(src_size) }
    }

    fn compress(&self, src: &[u8], level: i32) -> Result<Vec<u8>, String> {
        let bound = self.compress_bound(src.len());
        // SAFETY: Function pointer validated non-null on construction.
        if unsafe { (self.is_error_fn)(bound) } != 0 {
            return Err("ZSTD_compressBound failed".to_string());
        }
        let alloc_len = bound.max(1);
        let mut dst = Vec::new();
        dst.try_reserve_exact(alloc_len)
            .map_err(|_| "zstd compress buffer allocation failed".to_string())?;
        dst.resize(alloc_len, 0);

        let src_ptr = if src.is_empty() {
            NonNull::<u8>::dangling().as_ptr().cast::<c_void>()
        } else {
            src.as_ptr().cast::<c_void>()
        };
        // SAFETY: Both `dst` and `src_ptr` point to valid buffers for the stated capacities.
        let written = unsafe {
            (self.compress_fn)(
                dst.as_mut_ptr().cast::<c_void>(),
                dst.len(),
                src_ptr,
                src.len(),
                level as c_int,
            )
        };
        // SAFETY: `is_error_fn` inspects return code only.
        if unsafe { (self.is_error_fn)(written) } != 0 || written > dst.len() {
            return Err("ZSTD_compress failed".to_string());
        }
        dst.truncate(written);
        Ok(dst)
    }

    fn decompress(&self, compressed: &[u8], uncompressed_size: usize) -> Result<Vec<u8>, String> {
        if uncompressed_size == 0 {
            return Ok(Vec::new());
        }
        let mut dst = Vec::new();
        dst.try_reserve_exact(uncompressed_size)
            .map_err(|_| "zstd decompress buffer allocation failed".to_string())?;
        dst.resize(uncompressed_size, 0);

        let src_ptr = if compressed.is_empty() {
            NonNull::<u8>::dangling().as_ptr().cast::<c_void>()
        } else {
            compressed.as_ptr().cast::<c_void>()
        };
        // SAFETY: `dst` has `uncompressed_size` bytes; `src_ptr` has `compressed.len()` bytes.
        let out_len = unsafe {
            (self.decompress_fn)(
                dst.as_mut_ptr().cast::<c_void>(),
                dst.len(),
                src_ptr,
                compressed.len(),
            )
        };
        // SAFETY: `is_error_fn` inspects return code only.
        if unsafe { (self.is_error_fn)(out_len) } != 0 || out_len != uncompressed_size {
            return Err("ZSTD_decompress failed or size mismatch".to_string());
        }
        Ok(dst)
    }
}

struct FfiVfsReader {
    ctx: *mut c_void,
    size: u64,
    read_at_fn: unsafe extern "C" fn(*mut c_void, u64, *mut u8, usize) -> usize,
}

impl FfiVfsReader {
    unsafe fn from_raw(ptr: *const EglNupVfsOps) -> Result<Self, ()> {
        if !ptr_valid(ptr) {
            return Err(());
        }
        // SAFETY: Validated non-null and aligned `EglNupVfsOps`.
        let ops = unsafe { *ptr };
        let Some(read_at_fn) = ops.read_at else {
            return Err(());
        };
        Ok(Self {
            ctx: ops.ctx,
            size: ops.size,
            read_at_fn,
        })
    }
}

impl ReadAt for FfiVfsReader {
    fn len(&self) -> u64 {
        self.size
    }

    fn read_at(&self, offset: u64, dst: &mut [u8]) -> io::Result<usize> {
        if dst.is_empty() {
            return Ok(0);
        }
        // SAFETY: `dst` points to `dst.len()` writable bytes.
        let n = unsafe { (self.read_at_fn)(self.ctx, offset, dst.as_mut_ptr(), dst.len()) };
        Ok(n.min(dst.len()))
    }
}

unsafe fn parse_metadata_from_raw(raw: &EglNupMetadata) -> Result<Metadata, ()> {
    // SAFETY: Each field validated via `str_from_raw_checked` / `slice_from_raw_checked`.
    let title_name = unsafe { str_from_raw_checked(raw.title_name, raw.title_name_len)? }.to_owned();
    let icon = unsafe { slice_from_raw_checked(raw.icon, raw.icon_len)? }.to_vec();
    let display_version =
        unsafe { str_from_raw_checked(raw.display_version, raw.display_version_len)? }.to_owned();
    let publisher = unsafe { str_from_raw_checked(raw.publisher, raw.publisher_len)? }.to_owned();
    let pairs_slice =
        unsafe { slice_from_raw_checked(raw.localized_titles, raw.localized_title_count)? };
    let mut localized_titles = Vec::with_capacity(pairs_slice.len());
    for pair in pairs_slice {
        let lang = unsafe { str_from_raw_checked(pair.lang, pair.lang_len)? }.to_owned();
        let val = unsafe { str_from_raw_checked(pair.value, pair.value_len)? }.to_owned();
        localized_titles.push((lang, val));
    }
    Ok(Metadata {
        title_name,
        icon,
        display_version,
        publisher,
        localized_titles,
        version_source: String::new(),
    })
}

fn alloc_owned_metadata(meta: Metadata) -> EglNupOwnedMetadata {
    let (localized_titles, localized_title_count) = alloc_localized_pairs(meta.localized_titles);
    EglNupOwnedMetadata {
        title_name: alloc_owned_string(meta.title_name),
        icon: alloc_owned_bytes(meta.icon),
        display_version: alloc_owned_string(meta.display_version),
        publisher: alloc_owned_string(meta.publisher),
        localized_titles,
        localized_title_count,
        version_source: alloc_owned_string(meta.version_source),
    }
}

fn free_owned_metadata_inner(meta: &mut EglNupOwnedMetadata) {
    free_owned_string_inner(&mut meta.title_name);
    free_owned_bytes_inner(&mut meta.icon);
    free_owned_string_inner(&mut meta.display_version);
    free_owned_string_inner(&mut meta.publisher);
    free_localized_pairs(&mut meta.localized_titles, &mut meta.localized_title_count);
    free_owned_string_inner(&mut meta.version_source);
}

#[no_mangle]
pub unsafe extern "C" fn egl_nup_is_nup_magic(
    bytes: *const u8,
    len: usize,
    total_size: u64,
) -> u8 {
    catch_unwind(AssertUnwindSafe(|| {
        let Ok(slice) = slice_from_raw_checked(bytes, len) else {
            return 0;
        };
        u8::from(NupContainer::is_nup_magic(slice, total_size))
    }))
    .unwrap_or(0)
}

#[no_mangle]
pub unsafe extern "C" fn egl_nup_container_parse(
    vfs: *const EglNupVfsOps,
    out_container: *mut EglNupParsedContainer,
) -> i32 {
    if !mut_ptr_valid(out_container) {
        return -1;
    }
    let res = catch_unwind(AssertUnwindSafe(|| {
        egl_nup_container_free(out_container);
        let Ok(reader) = FfiVfsReader::from_raw(vfs) else {
            return -1;
        };
        let Ok(parsed) = NupContainer::parse(&reader) else {
            return -1;
        };

        let c_packages: Vec<EglNupPackageInfo> = parsed
            .packages
            .into_iter()
            .map(|p| {
                let (localized_titles, localized_title_count) =
                    alloc_localized_pairs(p.localized_titles);
                let loc_icons_owned: Vec<EglNupOwnedLocalizedBytesPair> = p
                    .localized_icons
                    .into_iter()
                    .map(|(lang, data)| EglNupOwnedLocalizedBytesPair {
                        lang: alloc_owned_string(lang),
                        data: alloc_owned_bytes(data),
                    })
                    .collect();
                let (localized_icons, localized_icon_count) = owned_from_vec(loc_icons_owned);
                EglNupPackageInfo {
                    title_id: p.title_id,
                    version: p.version,
                    kind: p.kind as u8,
                    standalone_update: u8::from(p.standalone_update),
                    _pad: [0; 2],
                    root_dir: p.root_dir,
                    exefs_dir: p.exefs_dir,
                    romfs_dir: p.romfs_dir,
                    meta_offset: p.meta_offset,
                    meta_size: p.meta_size,
                    _pad2: 0,
                    npdm: alloc_owned_bytes(p.npdm),
                    title_name: alloc_owned_string(p.title_name),
                    publisher: alloc_owned_string(p.publisher),
                    source_dump: alloc_owned_string(p.source_dump),
                    localized_titles,
                    localized_title_count,
                    icon: alloc_owned_bytes(p.icon),
                    localized_icons,
                    localized_icon_count,
                    display_version: alloc_owned_string(p.display_version),
                }
            })
            .collect();

        let c_dirs: Vec<EglNupDirNode> = parsed
            .dirs
            .into_iter()
            .map(|d| EglNupDirNode {
                name: alloc_owned_string(d.name),
                first_child: d.first_child,
                child_count: d.child_count,
            })
            .collect();

        let c_files: Vec<EglNupFileNode> = parsed
            .files
            .into_iter()
            .map(|f| EglNupFileNode {
                name: alloc_owned_string(f.name),
                size: f.size,
                sha256: f.sha256,
                extent_offset: f.extent_offset,
                extent_size: f.extent_size,
                codec: f.codec,
                _pad: [0; 7],
            })
            .collect();

        let (packages, package_count) = owned_from_vec(c_packages);
        let (dirs, dir_count) = owned_from_vec(c_dirs);
        let (files, file_count) = owned_from_vec(c_files);
        let (children, child_count) = owned_from_vec(parsed.children);

        *out_container = EglNupParsedContainer {
            toc_offset: parsed.toc_offset,
            toc_size: parsed.toc_size,
            blob_offset: parsed.blob_offset,
            file_size: parsed.file_size,
            packages,
            package_count,
            dirs,
            dir_count,
            files,
            file_count,
            children,
            child_count,
        };
        0
    }));
    match res {
        Ok(code) => code,
        Err(_) => {
            // SAFETY: Validated non-null and aligned above.
            unsafe {
                egl_nup_container_free(out_container);
            }
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn egl_nup_container_free(container: *mut EglNupParsedContainer) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if !mut_ptr_valid(container) {
            return;
        }
        // SAFETY: `container` validated non-null and aligned.
        let c = unsafe { &mut *container };

        if !c.packages.is_null()
            && c.package_count > 0
            && (c.packages as usize).is_multiple_of(align_of::<EglNupPackageInfo>())
        {
            // SAFETY: Slice allocated via `owned_from_vec::<EglNupPackageInfo>`.
            let pkgs = unsafe { std::slice::from_raw_parts_mut(c.packages, c.package_count) };
            for p in pkgs {
                free_owned_bytes_inner(&mut p.npdm);
                free_owned_string_inner(&mut p.title_name);
                free_owned_string_inner(&mut p.publisher);
                free_owned_string_inner(&mut p.source_dump);
                free_localized_pairs(&mut p.localized_titles, &mut p.localized_title_count);
                free_owned_bytes_inner(&mut p.icon);
                free_localized_bytes_pairs(&mut p.localized_icons, &mut p.localized_icon_count);
                free_owned_string_inner(&mut p.display_version);
            }
        }
        // SAFETY: Inner allocations freed above.
        unsafe { free_owned_slice(&mut c.packages, &mut c.package_count) };

        if !c.dirs.is_null()
            && c.dir_count > 0
            && (c.dirs as usize).is_multiple_of(align_of::<EglNupDirNode>())
        {
            // SAFETY: Slice allocated via `owned_from_vec::<EglNupDirNode>`.
            let dirs = unsafe { std::slice::from_raw_parts_mut(c.dirs, c.dir_count) };
            for d in dirs {
                free_owned_string_inner(&mut d.name);
            }
        }
        // SAFETY: Inner strings freed above.
        unsafe { free_owned_slice(&mut c.dirs, &mut c.dir_count) };

        if !c.files.is_null()
            && c.file_count > 0
            && (c.files as usize).is_multiple_of(align_of::<EglNupFileNode>())
        {
            // SAFETY: Slice allocated via `owned_from_vec::<EglNupFileNode>`.
            let files = unsafe { std::slice::from_raw_parts_mut(c.files, c.file_count) };
            for f in files {
                free_owned_string_inner(&mut f.name);
            }
        }
        // SAFETY: Inner strings freed above.
        unsafe { free_owned_slice(&mut c.files, &mut c.file_count) };
        // SAFETY: Children vec allocated via `owned_from_vec::<u32>`.
        unsafe { free_owned_slice(&mut c.children, &mut c.child_count) };

        *c = EglNupParsedContainer::default();
    }));
}

#[no_mangle]
pub unsafe extern "C" fn egl_nup_read_extent(
    vfs: *const EglNupVfsOps,
    zstd: *const EglNupZstdOps,
    extent_offset: u64,
    extent_size: u64,
    uncompressed_size: u64,
    codec: u8,
    out_bytes: *mut EglNupOwnedBytes,
) -> i32 {
    if !mut_ptr_valid(out_bytes) {
        return -1;
    }
    catch_unwind(AssertUnwindSafe(|| {
        free_owned_bytes_inner(&mut *out_bytes);
        let Ok(reader) = FfiVfsReader::from_raw(vfs) else {
            return -1;
        };
        let Ok(codec_ops) = FfiZstdCodec::from_raw(zstd) else {
            return -1;
        };
        match read_extent(
            &reader,
            &codec_ops,
            extent_offset,
            extent_size,
            uncompressed_size,
            codec,
        ) {
            Ok(bytes) => {
                *out_bytes = alloc_owned_bytes(bytes);
                0
            }
            Err(_) => -1,
        }
    }))
    .unwrap_or(-1)
}

#[no_mangle]
pub unsafe extern "C" fn egl_nup_verify_integrity(
    vfs: *const EglNupVfsOps,
    zstd: *const EglNupZstdOps,
    progress_cb: EglNupVerifyProgressCallback,
    progress_ctx: *mut c_void,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        let Ok(reader) = FfiVfsReader::from_raw(vfs) else {
            return -1;
        };
        let Ok(codec_ops) = FfiZstdCodec::from_raw(zstd) else {
            return -1;
        };
        let mut cb_closure = |done: usize, total: usize| -> bool {
            if let Some(cb) = progress_cb {
                cb(progress_ctx, done, total) != 0
            } else {
                true
            }
        };
        let opt_cb: Option<&mut dyn FnMut(usize, usize) -> bool> =
            progress_cb.map(|_| &mut cb_closure as &mut dyn FnMut(usize, usize) -> bool);
        if verify_integrity(&reader, &codec_ops, opt_cb).is_ok() {
            0
        } else {
            -1
        }
    }))
    .unwrap_or(-1)
}

#[no_mangle]
pub unsafe extern "C" fn egl_nup_is_loose_dump(dir_utf8: *const c_char, dir_len: usize) -> u8 {
    catch_unwind(AssertUnwindSafe(|| {
        let Ok(path) = path_from_raw_checked(dir_utf8, dir_len) else {
            return 0;
        };
        u8::from(is_loose_dump(&path))
    }))
    .unwrap_or(0)
}

#[no_mangle]
pub unsafe extern "C" fn egl_nup_dump_root_from_main(
    main_utf8: *const c_char,
    main_len: usize,
    out_root: *mut EglNupOwnedString,
) -> u8 {
    if !mut_ptr_valid(out_root) {
        return 0;
    }
    catch_unwind(AssertUnwindSafe(|| {
        free_owned_string_inner(&mut *out_root);
        let Ok(main_path) = path_from_raw_checked(main_utf8, main_len) else {
            return 0;
        };
        let Some(root) = dump_root_from_main(&main_path) else {
            return 0;
        };
        let Some(root_str) = root.to_str() else {
            return 0;
        };
        *out_root = alloc_owned_string(root_str.to_owned());
        1
    }))
    .unwrap_or(0)
}

#[no_mangle]
pub unsafe extern "C" fn egl_nup_probe_metadata(
    dump_root_utf8: *const c_char,
    dump_root_len: usize,
    nacp_language_index: i32,
    out_meta: *mut EglNupOwnedMetadata,
) -> i32 {
    if !mut_ptr_valid(out_meta) {
        return -1;
    }
    catch_unwind(AssertUnwindSafe(|| {
        free_owned_metadata_inner(&mut *out_meta);
        let Ok(root) = path_from_raw_checked(dump_root_utf8, dump_root_len) else {
            return -1;
        };
        let meta = probe_dump_metadata(&root, nacp_language_index);
        *out_meta = alloc_owned_metadata(meta);
        0
    }))
    .unwrap_or(-1)
}

#[no_mangle]
pub unsafe extern "C" fn egl_nup_probe_nup_metadata(
    archive_utf8: *const c_char,
    archive_len: usize,
    out_meta: *mut EglNupOwnedMetadata,
) -> i32 {
    if !mut_ptr_valid(out_meta) {
        return -1;
    }
    catch_unwind(AssertUnwindSafe(|| {
        free_owned_metadata_inner(&mut *out_meta);
        let Ok(archive) = path_from_raw_checked(archive_utf8, archive_len) else {
            return -1;
        };
        let meta = probe_nup_metadata(&archive);
        *out_meta = alloc_owned_metadata(meta);
        0
    }))
    .unwrap_or(-1)
}

#[no_mangle]
pub unsafe extern "C" fn egl_nup_default_archive_path(
    dump_root_utf8: *const c_char,
    dump_root_len: usize,
    out_path: *mut EglNupOwnedString,
) -> i32 {
    if !mut_ptr_valid(out_path) {
        return -1;
    }
    catch_unwind(AssertUnwindSafe(|| {
        free_owned_string_inner(&mut *out_path);
        let Ok(root) = path_from_raw_checked(dump_root_utf8, dump_root_len) else {
            return -1;
        };
        let p = default_archive_path(&root);
        let Some(s) = p.to_str() else {
            return -1;
        };
        *out_path = alloc_owned_string(s.to_owned());
        0
    }))
    .unwrap_or(-1)
}

#[no_mangle]
pub unsafe extern "C" fn egl_nup_default_extract_path(
    archive_utf8: *const c_char,
    archive_len: usize,
    out_path: *mut EglNupOwnedString,
) -> i32 {
    if !mut_ptr_valid(out_path) {
        return -1;
    }
    catch_unwind(AssertUnwindSafe(|| {
        free_owned_string_inner(&mut *out_path);
        let Ok(archive) = path_from_raw_checked(archive_utf8, archive_len) else {
            return -1;
        };
        let p = default_extract_path(&archive);
        let Some(s) = p.to_str() else {
            return -1;
        };
        *out_path = alloc_owned_string(s.to_owned());
        0
    }))
    .unwrap_or(-1)
}

#[no_mangle]
pub unsafe extern "C" fn egl_nup_has_resumable_journal(
    archive_utf8: *const c_char,
    archive_len: usize,
) -> u8 {
    catch_unwind(AssertUnwindSafe(|| {
        let Ok(archive) = path_from_raw_checked(archive_utf8, archive_len) else {
            return 0;
        };
        u8::from(has_resumable_journal(&archive))
    }))
    .unwrap_or(0)
}

#[no_mangle]
pub unsafe extern "C" fn egl_nup_pack_dump(
    dump_root_utf8: *const c_char,
    dump_root_len: usize,
    archive_utf8: *const c_char,
    archive_len: usize,
    options: *const EglNupPackOptions,
    zstd: *const EglNupZstdOps,
    progress_cb: EglNupProgressCallback,
    progress_ctx: *mut c_void,
    cancel_cb: EglNupCancelCallback,
    cancel_ctx: *mut c_void,
    out_error: *mut EglNupOwnedString,
) -> EglNupPackResult {
    catch_unwind(AssertUnwindSafe(|| {
        clear_error_string(out_error);
        let Ok(dump_root) = path_from_raw_checked(dump_root_utf8, dump_root_len) else {
            write_error_string(out_error, "invalid dump root path".to_string());
            return EglNupPackResult::InvalidSource;
        };
        let Ok(archive) = path_from_raw_checked(archive_utf8, archive_len) else {
            write_error_string(out_error, "invalid archive path".to_string());
            return EglNupPackResult::InvalidSource;
        };
        let Ok(codec_ops) = FfiZstdCodec::from_raw(zstd) else {
            write_error_string(out_error, "invalid zstd callback table".to_string());
            return EglNupPackResult::InvalidSource;
        };

        let opts = if options.is_null() {
            PackOptions::default()
        } else if !ptr_valid(options) {
            write_error_string(out_error, "unaligned options pointer".to_string());
            return EglNupPackResult::InvalidSource;
        } else {
            let raw = &*options;
            let Ok(meta) = parse_metadata_from_raw(&raw.metadata) else {
                write_error_string(out_error, "invalid metadata pointers".to_string());
                return EglNupPackResult::InvalidSource;
            };
            PackOptions {
                delete_sources: raw.delete_sources != 0,
                zstd_level: raw.zstd_level,
                chunk_bytes: raw.chunk_bytes,
                raw_threshold: raw.raw_threshold,
                nacp_language_index: raw.nacp_language_index,
                metadata: meta,
            }
        };

        let cancel_flag = AtomicBool::new(false);
        if let Some(ccb) = cancel_cb {
            if ccb(cancel_ctx) != 0 {
                cancel_flag.store(true, Ordering::Relaxed);
            }
        }
        let mut bridge = |p: &PackProgress| {
            if let Some(pcb) = progress_cb {
                let cp = EglNupPackProgress {
                    bytes_done: p.bytes_done,
                    bytes_total: p.bytes_total,
                    files_done: p.files_done,
                    files_total: p.files_total,
                };
                pcb(progress_ctx, &cp);
            }
            if let Some(ccb) = cancel_cb {
                if ccb(cancel_ctx) != 0 {
                    cancel_flag.store(true, Ordering::Relaxed);
                }
            }
        };
        let cb_opt: Option<&mut dyn FnMut(&PackProgress)> =
            if progress_cb.is_some() || cancel_cb.is_some() {
                Some(&mut bridge as &mut dyn FnMut(&PackProgress))
            } else {
                None
            };

        match pack_dump(
            &dump_root,
            &archive,
            &opts,
            &codec_ops,
            Some(&cancel_flag),
            cb_opt,
        ) {
            Ok(()) => EglNupPackResult::Success,
            Err((code, msg)) => {
                write_error_string(out_error, msg);
                match code {
                    PackResult::Success => EglNupPackResult::Success,
                    PackResult::Cancelled => EglNupPackResult::Cancelled,
                    PackResult::InvalidSource => EglNupPackResult::InvalidSource,
                    PackResult::IoError => EglNupPackResult::IoError,
                }
            }
        }
    }))
    .unwrap_or_else(|_| {
        write_error_string(out_error, "internal panic in Rust NUP packer".to_string());
        EglNupPackResult::IoError
    })
}

#[no_mangle]
pub unsafe extern "C" fn egl_nup_extract(
    archive_utf8: *const c_char,
    archive_len: usize,
    out_dir_utf8: *const c_char,
    out_dir_len: usize,
    zstd: *const EglNupZstdOps,
    progress_cb: EglNupProgressCallback,
    progress_ctx: *mut c_void,
    cancel_cb: EglNupCancelCallback,
    cancel_ctx: *mut c_void,
    out_error: *mut EglNupOwnedString,
) -> EglNupExtractResult {
    catch_unwind(AssertUnwindSafe(|| {
        clear_error_string(out_error);
        let Ok(archive) = path_from_raw_checked(archive_utf8, archive_len) else {
            write_error_string(out_error, "invalid archive path".to_string());
            return EglNupExtractResult::InvalidSource;
        };
        let Ok(out_dir) = path_from_raw_checked(out_dir_utf8, out_dir_len) else {
            write_error_string(out_error, "invalid output directory path".to_string());
            return EglNupExtractResult::InvalidSource;
        };
        let Ok(codec_ops) = FfiZstdCodec::from_raw(zstd) else {
            write_error_string(out_error, "invalid zstd callback table".to_string());
            return EglNupExtractResult::InvalidSource;
        };

        let cancel_flag = AtomicBool::new(false);
        if let Some(ccb) = cancel_cb {
            if ccb(cancel_ctx) != 0 {
                cancel_flag.store(true, Ordering::Relaxed);
            }
        }
        let mut bridge = |p: &PackProgress| {
            if let Some(pcb) = progress_cb {
                let cp = EglNupPackProgress {
                    bytes_done: p.bytes_done,
                    bytes_total: p.bytes_total,
                    files_done: p.files_done,
                    files_total: p.files_total,
                };
                pcb(progress_ctx, &cp);
            }
            if let Some(ccb) = cancel_cb {
                if ccb(cancel_ctx) != 0 {
                    cancel_flag.store(true, Ordering::Relaxed);
                }
            }
        };
        let cb_opt: Option<&mut dyn FnMut(&PackProgress)> =
            if progress_cb.is_some() || cancel_cb.is_some() {
                Some(&mut bridge as &mut dyn FnMut(&PackProgress))
            } else {
                None
            };

        match extract_nup(&archive, &out_dir, &codec_ops, cb_opt, Some(&cancel_flag)) {
            Ok(()) => EglNupExtractResult::Success,
            Err((code, msg)) => {
                write_error_string(out_error, msg);
                match code {
                    ExtractResult::Success => EglNupExtractResult::Success,
                    ExtractResult::Cancelled => EglNupExtractResult::Cancelled,
                    ExtractResult::InvalidSource => EglNupExtractResult::InvalidSource,
                    ExtractResult::IoError => EglNupExtractResult::IoError,
                }
            }
        }
    }))
    .unwrap_or_else(|_| {
        write_error_string(out_error, "internal panic in Rust NUP extractor".to_string());
        EglNupExtractResult::IoError
    })
}

#[no_mangle]
pub unsafe extern "C" fn egl_nup_edit_metadata(
    archive_utf8: *const c_char,
    archive_len: usize,
    edit: *const EglNupMetadataEdit,
    progress_cb: EglNupProgressCallback,
    progress_ctx: *mut c_void,
    cancel_cb: EglNupCancelCallback,
    cancel_ctx: *mut c_void,
    out_error: *mut EglNupOwnedString,
) -> EglNupEditResult {
    catch_unwind(AssertUnwindSafe(|| {
        clear_error_string(out_error);
        let Ok(archive) = path_from_raw_checked(archive_utf8, archive_len) else {
            write_error_string(out_error, "invalid archive path".to_string());
            return EglNupEditResult::InvalidSource;
        };
        if !ptr_valid(edit) {
            write_error_string(out_error, "invalid metadata edit pointer".to_string());
            return EglNupEditResult::InvalidSource;
        }
        let raw_edit = &*edit;
        let Ok(vals) = parse_metadata_from_raw(&raw_edit.values) else {
            write_error_string(out_error, "invalid metadata field pointers".to_string());
            return EglNupEditResult::InvalidSource;
        };
        let rust_edit = MetadataEdit {
            title_name: (raw_edit.has_title_name != 0).then_some(vals.title_name),
            icon: (raw_edit.has_icon != 0).then_some(vals.icon),
            display_version: (raw_edit.has_display_version != 0).then_some(vals.display_version),
            publisher: (raw_edit.has_publisher != 0).then_some(vals.publisher),
            localized_titles: (raw_edit.has_localized_titles != 0).then_some(vals.localized_titles),
        };

        let cancel_flag = AtomicBool::new(false);
        if let Some(ccb) = cancel_cb {
            if ccb(cancel_ctx) != 0 {
                cancel_flag.store(true, Ordering::Relaxed);
            }
        }
        let mut bridge = |p: &PackProgress| {
            if let Some(pcb) = progress_cb {
                let cp = EglNupPackProgress {
                    bytes_done: p.bytes_done,
                    bytes_total: p.bytes_total,
                    files_done: p.files_done,
                    files_total: p.files_total,
                };
                pcb(progress_ctx, &cp);
            }
            if let Some(ccb) = cancel_cb {
                if ccb(cancel_ctx) != 0 {
                    cancel_flag.store(true, Ordering::Relaxed);
                }
            }
        };
        let cb_opt: Option<&mut dyn FnMut(&PackProgress)> =
            if progress_cb.is_some() || cancel_cb.is_some() {
                Some(&mut bridge as &mut dyn FnMut(&PackProgress))
            } else {
                None
            };

        match edit_nup_metadata(&archive, &rust_edit, Some(&cancel_flag), cb_opt) {
            Ok(code) => match code {
                EditResult::Success => EglNupEditResult::Success,
                EditResult::NoChange => EglNupEditResult::NoChange,
                EditResult::Cancelled => EglNupEditResult::Cancelled,
                EditResult::InvalidSource => EglNupEditResult::InvalidSource,
                EditResult::IoError => EglNupEditResult::IoError,
            },
            Err((code, msg)) => {
                write_error_string(out_error, msg);
                match code {
                    EditResult::Success => EglNupEditResult::Success,
                    EditResult::NoChange => EglNupEditResult::NoChange,
                    EditResult::Cancelled => EglNupEditResult::Cancelled,
                    EditResult::InvalidSource => EglNupEditResult::InvalidSource,
                    EditResult::IoError => EglNupEditResult::IoError,
                }
            }
        }
    }))
    .unwrap_or_else(|_| {
        write_error_string(out_error, "internal panic in Rust NUP metadata editor".to_string());
        EglNupEditResult::IoError
    })
}

#[no_mangle]
pub unsafe extern "C" fn egl_nup_free_bytes(bytes: *mut EglNupOwnedBytes) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if mut_ptr_valid(bytes) {
            // SAFETY: `bytes` validated non-null and aligned.
            free_owned_bytes_inner(unsafe { &mut *bytes });
        }
    }));
}

#[no_mangle]
pub unsafe extern "C" fn egl_nup_free_string(str_val: *mut EglNupOwnedString) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if mut_ptr_valid(str_val) {
            // SAFETY: `str_val` validated non-null and aligned.
            free_owned_string_inner(unsafe { &mut *str_val });
        }
    }));
}

#[no_mangle]
pub unsafe extern "C" fn egl_nup_free_metadata(meta: *mut EglNupOwnedMetadata) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if mut_ptr_valid(meta) {
            // SAFETY: `meta` validated non-null and aligned.
            free_owned_metadata_inner(unsafe { &mut *meta });
        }
    }));
}
