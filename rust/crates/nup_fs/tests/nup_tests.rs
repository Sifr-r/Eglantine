// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![deny(warnings)]

extern crate eglantine_nup_fs;

use std::ffi::{c_char, c_int, c_uint, c_void};
use std::fs;
use std::mem::MaybeUninit;
use std::path::{Path, PathBuf};
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

use eglantine_nup_fs::ffi::{
    egl_nup_container_free, egl_nup_container_parse, egl_nup_default_archive_path,
    egl_nup_default_extract_path, egl_nup_dump_root_from_main, egl_nup_edit_metadata,
    egl_nup_extract, egl_nup_free_bytes, egl_nup_free_metadata, egl_nup_free_string,
    egl_nup_has_resumable_journal, egl_nup_is_loose_dump, egl_nup_is_nup_magic, egl_nup_pack_dump,
    egl_nup_probe_metadata, egl_nup_probe_nup_metadata, egl_nup_read_extent,
    egl_nup_verify_integrity, EglNupEditResult, EglNupExtractResult, EglNupFileNode,
    EglNupMetadata, EglNupMetadataEdit, EglNupOwnedBytes, EglNupOwnedMetadata, EglNupOwnedString,
    EglNupPackOptions, EglNupPackResult, EglNupPackageInfo, EglNupParsedContainer, EglNupVfsOps,
    EglNupZstdOps,
};
use eglantine_nup_fs::format::{from_hex, put_u16_le, put_u32_le, put_u64_le, to_hex};
use eglantine_nup_fs::journal::plan_from_json;
use eglantine_nup_fs::metadata::parse_nacp_bytes;
use eglantine_nup_fs::{
    default_archive_path, default_extract_path, dump_root_from_main, edit_nup_metadata,
    extract_nup, has_resumable_journal, is_loose_dump, pack_dump, probe_dump_metadata,
    probe_nup_metadata, read_extent, sha256, verify_integrity, EditResult, MetadataEdit,
    NupContainer, PackOptions, PackProgress, PackResult, PackageKind, SliceReader, ZstdCodec,
    DEFAULT_RAW_THRESHOLD, HEADER_MAGIC, SUPERBLOCK_SIZE, TOC_HEADER_SIZE, TOC_MAGIC,
};

static TEMP_SEQ: AtomicU64 = AtomicU64::new(0);

struct TempDir(PathBuf);

impl TempDir {
    fn new(prefix: &str) -> Self {
        let id = TEMP_SEQ.fetch_add(1, Ordering::Relaxed);
        let dir = std::env::temp_dir().join(format!(
            "eglantine_nup_fs_{prefix}_{}_{}",
            std::process::id(),
            id
        ));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).expect("create temp dir");
        Self(dir)
    }

    fn path(&self) -> &Path {
        &self.0
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

// Pure-Rust deterministic framed test codec (`ZSTD_MOCK` framing for standalone tests).
struct MockZstd;

impl ZstdCodec for MockZstd {
    fn compress_bound(&self, src_size: usize) -> usize {
        src_size + 12
    }

    fn compress(&self, src: &[u8], level: i32) -> Result<Vec<u8>, String> {
        let mut out = Vec::with_capacity(8 + src.len());
        out.extend_from_slice(b"ZMOK");
        out.extend_from_slice(&(src.len() as u32).to_le_bytes());
        out.push(level as u8);
        // Simple XOR/RLE-friendly tag if payload is highly repetitive so size < src.len().
        if src.len() >= 16 && src.iter().all(|&b| b == src[0]) {
            out.push(1);
            out.push(src[0]);
            return Ok(out);
        }
        out.push(0);
        out.extend_from_slice(src);
        Ok(out)
    }

    fn decompress(&self, compressed: &[u8], uncompressed_size: usize) -> Result<Vec<u8>, String> {
        if compressed.len() < 10 || &compressed[..4] != b"ZMOK" {
            return Err("bad mock header".to_string());
        }
        let orig_len =
            u32::from_le_bytes([compressed[4], compressed[5], compressed[6], compressed[7]])
                as usize;
        if orig_len != uncompressed_size {
            return Err("mock size mismatch".to_string());
        }
        let mode = compressed[9];
        if mode == 1 {
            if compressed.len() != 11 {
                return Err("bad mock rle".to_string());
            }
            return Ok(vec![compressed[10]; orig_len]);
        }
        if mode == 0 {
            if compressed.len() != 10 + orig_len {
                return Err("bad mock literal".to_string());
            }
            return Ok(compressed[10..].to_vec());
        }
        Err("unknown mock mode".to_string())
    }
}

unsafe extern "C" fn mock_c_compress(
    dst: *mut c_void,
    dst_capacity: usize,
    src: *const c_void,
    src_size: usize,
    level: c_int,
) -> usize {
    let input = if src_size == 0 {
        &[][..]
    } else {
        std::slice::from_raw_parts(src.cast::<u8>(), src_size)
    };
    let Ok(encoded) = MockZstd.compress(input, level) else {
        return usize::MAX;
    };
    if encoded.len() > dst_capacity {
        return usize::MAX;
    }
    ptr::copy_nonoverlapping(encoded.as_ptr(), dst.cast::<u8>(), encoded.len());
    encoded.len()
}

unsafe extern "C" fn mock_c_decompress(
    dst: *mut c_void,
    dst_capacity: usize,
    src: *const c_void,
    compressed_size: usize,
) -> usize {
    let input = if compressed_size == 0 {
        &[][..]
    } else {
        std::slice::from_raw_parts(src.cast::<u8>(), compressed_size)
    };
    let Ok(decoded) = MockZstd.decompress(input, dst_capacity) else {
        return usize::MAX;
    };
    ptr::copy_nonoverlapping(decoded.as_ptr(), dst.cast::<u8>(), decoded.len());
    decoded.len()
}

unsafe extern "C" fn mock_c_compress_bound(src_size: usize) -> usize {
    MockZstd.compress_bound(src_size)
}

unsafe extern "C" fn mock_c_is_error(code: usize) -> c_uint {
    u32::from(code == usize::MAX)
}

fn mock_c_zstd_ops() -> EglNupZstdOps {
    EglNupZstdOps {
        compress: Some(mock_c_compress),
        decompress: Some(mock_c_decompress),
        compress_bound: Some(mock_c_compress_bound),
        is_error: Some(mock_c_is_error),
    }
}

unsafe extern "C" fn vec_vfs_read_at(
    ctx: *mut c_void,
    offset: u64,
    dst: *mut u8,
    len: usize,
) -> usize {
    let buf = &*ctx.cast::<Vec<u8>>();
    let Ok(off) = usize::try_from(offset) else {
        return 0;
    };
    if off >= buf.len() || len == 0 {
        return 0;
    }
    let avail = (buf.len() - off).min(len);
    ptr::copy_nonoverlapping(buf[off..].as_ptr(), dst, avail);
    avail
}

fn make_npdm(title_id: u64) -> Vec<u8> {
    let mut npdm = vec![0u8; 0xA0];
    npdm[0..4].copy_from_slice(b"META");
    put_u32_le(&mut npdm, 0x70, 0x80);
    put_u32_le(&mut npdm, 0x74, 0x20);
    npdm[0x80..0x84].copy_from_slice(b"ACI0");
    put_u64_le(&mut npdm, 0x90, title_id);
    npdm
}

fn make_nacp(name: &str, publisher: &str, version: &str) -> Vec<u8> {
    let mut nacp = vec![0u8; 0x4000];
    let nbytes = name.as_bytes();
    nacp[..nbytes.len().min(0x1FF)].copy_from_slice(&nbytes[..nbytes.len().min(0x1FF)]);
    let pbytes = publisher.as_bytes();
    nacp[0x200..0x200 + pbytes.len().min(0xFF)]
        .copy_from_slice(&pbytes[..pbytes.len().min(0xFF)]);
    let vbytes = version.as_bytes();
    nacp[0x3060..0x3060 + vbytes.len().min(0xF)]
        .copy_from_slice(&vbytes[..vbytes.len().min(0xF)]);
    nacp
}

fn write_file_bytes(path: &Path, data: &[u8]) {
    if let Some(p) = path.parent() {
        fs::create_dir_all(p).expect("mkdir");
    }
    fs::write(path, data).expect("write");
}

fn build_sample_multi_dump(root: &Path) {
    write_file_bytes(&root.join("base/exefs/main"), b"MAIN_EXE_BASE");
    write_file_bytes(
        &root.join("base/exefs/main.npdm"),
        &make_npdm(0x0100_1234_5678_0000),
    );
    write_file_bytes(
        &root.join("base/control.nacp"),
        &make_nacp("Pokémon™ Pokopia", "Studio Example", "1.0.0"),
    );
    write_file_bytes(&root.join("base/icon_AmericanEnglish.dat"), b"FAKE_JPEG_ICON");
    write_file_bytes(&root.join("base/romfs/empty.bin"), b"");
    write_file_bytes(&root.join("base/romfs/dup_a.bin"), &vec![0xAAu8; 256]);
    write_file_bytes(&root.join("base/romfs/dup_b.bin"), &vec![0xAAu8; 256]);
    write_file_bytes(&root.join("base/romfs/sub/unique.bin"), b"UNIQUE_DATA");

    write_file_bytes(&root.join("updates/65536/exefs/main"), b"MAIN_EXE_UPD");
    write_file_bytes(
        &root.join("updates/65536/exefs/main.npdm"),
        &make_npdm(0x0100_1234_5678_0800),
    );
    write_file_bytes(
        &root.join("updates/65536/control.nacp"),
        &make_nacp("Pokémon™ Pokopia", "Studio Example", "1.0.1"),
    );

    write_file_bytes(
        &root.join("dlc/0100123456781001/0/romfs/dlc_item.bin"),
        b"DLC_PAYLOAD",
    );
}

#[test]
fn sha256_nist_vectors_and_hex() {
    assert_eq!(
        to_hex(&sha256(b"")),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
    );
    assert_eq!(
        to_hex(&sha256(b"abc")),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    );
    let long = b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    assert_eq!(
        to_hex(&sha256(long)),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
    );
    let raw = [0x01u8, 0xAB, 0xFE];
    assert_eq!(from_hex(&to_hex(&raw)).unwrap(), raw);
    assert!(from_hex("abc").is_err());
    assert!(from_hex("zz").is_err());
}

#[test]
fn nacp_uncompressed_and_deflate_stored_titles() {
    let raw = make_nacp("Title One", "Dev One", "2.3.4");
    let parsed = parse_nacp_bytes(&raw, 1);
    assert_eq!(parsed.application_name, "Title One");
    assert_eq!(parsed.developer_name, "Dev One");
    assert_eq!(parsed.version_string, "2.3.4");

    // Format 1: single DEFLATE stored block (BFINAL=1, BTYPE=00) holding 0x300 entry.
    let mut entry = vec![0u8; 0x300];
    entry[..10].copy_from_slice(b"DeflateApp");
    entry[0x200..0x20A].copy_from_slice(b"DeflateDev");
    let mut deflate = Vec::new();
    deflate.push(0x01); // BFINAL=1, BTYPE=00
    let len = entry.len() as u16;
    deflate.extend_from_slice(&len.to_le_bytes());
    deflate.extend_from_slice(&(!len).to_le_bytes());
    deflate.extend_from_slice(&entry);

    let mut comp_nacp = vec![0u8; 0x4000];
    put_u16_le(&mut comp_nacp, 0, deflate.len() as u16);
    comp_nacp[2..2 + deflate.len()].copy_from_slice(&deflate);
    comp_nacp[0x3215] = 1;
    let parsed2 = parse_nacp_bytes(&comp_nacp, 1);
    assert_eq!(parsed2.application_name, "DeflateApp");
    assert_eq!(parsed2.developer_name, "DeflateDev");
}

#[test]
fn pack_parse_verify_extract_dedup_roundtrip() {
    let tmp = TempDir::new("roundtrip");
    let dump = tmp.path().join("Poké Dump");
    build_sample_multi_dump(&dump);

    assert!(is_loose_dump(&dump));
    assert_eq!(
        dump_root_from_main(&dump.join("base/exefs/main")),
        Some(dump.clone())
    );
    assert_eq!(
        default_archive_path(&dump),
        tmp.path().join("Poké Dump.nup")
    );
    let archive = default_archive_path(&dump);
    assert_eq!(default_extract_path(&archive), tmp.path().join("Poké Dump"));

    let probed = probe_dump_metadata(&dump, 1);
    assert_eq!(probed.title_name, "Pokémon™ Pokopia");
    assert_eq!(probed.publisher, "Studio Example");
    assert_eq!(probed.display_version, "1.0.1");
    assert_eq!(probed.version_source, "update 65536");

    let mut last_progress = PackProgress::default();
    let opts = PackOptions::default();
    assert_eq!(opts.raw_threshold, DEFAULT_RAW_THRESHOLD);
    pack_dump(
        &dump,
        &archive,
        &opts,
        &MockZstd,
        None,
        Some(&mut |p| last_progress = *p),
    )
    .expect("pack_dump");
    assert!(archive.is_file());
    assert!(!has_resumable_journal(&archive));
    assert_eq!(last_progress.files_done, last_progress.files_total);
    assert!(last_progress.files_total >= 8);

    let bytes = fs::read(&archive).expect("read archive");
    assert!(NupContainer::is_nup_magic(&bytes, bytes.len() as u64));
    let reader = SliceReader(&bytes);
    let nup = NupContainer::parse(&reader).expect("parse archive");
    assert_eq!(nup.packages.len(), 3);
    assert_eq!(nup.packages[0].kind, PackageKind::Base);
    assert_eq!(nup.packages[1].kind, PackageKind::Update);
    assert_eq!(nup.packages[1].version, 65536);
    assert_eq!(nup.packages[2].kind, PackageKind::Addon);
    assert_eq!(nup.packages[2].title_id, 0x0100_1234_5678_1001);

    let dup_a = nup.files.iter().find(|f| f.name == "dup_a.bin").unwrap();
    let dup_b = nup.files.iter().find(|f| f.name == "dup_b.bin").unwrap();
    assert_eq!(dup_a.extent_offset, dup_b.extent_offset);
    assert_eq!(dup_a.sha256, dup_b.sha256);

    verify_integrity(&reader, &MockZstd, None).expect("verify integrity");

    let nup_meta = probe_nup_metadata(&archive);
    assert_eq!(nup_meta.title_name, "Pokémon™ Pokopia");
    assert_eq!(nup_meta.display_version, "1.0.1");

    let out_dir = tmp.path().join("extracted");
    extract_nup(&archive, &out_dir, &MockZstd, None, None).expect("extract_nup");
    assert_eq!(
        fs::read(out_dir.join("base/romfs/dup_b.bin")).unwrap(),
        vec![0xAAu8; 256]
    );
    assert_eq!(
        fs::read(out_dir.join("base/romfs/empty.bin")).unwrap(),
        Vec::<u8>::new()
    );
    assert_eq!(
        fs::read(out_dir.join("updates/65536/exefs/main")).unwrap(),
        b"MAIN_EXE_UPD"
    );
    assert_eq!(
        fs::read(out_dir.join("dlc/0100123456781001/0/romfs/dlc_item.bin")).unwrap(),
        b"DLC_PAYLOAD"
    );
}

#[test]
fn journal_cancel_resume_and_delete_sources() {
    let tmp = TempDir::new("resume");
    let dump = tmp.path().join("dump");
    build_sample_multi_dump(&dump);
    let archive = tmp.path().join("resumable.nup");

    let cancel = AtomicBool::new(false);
    let opts = PackOptions {
        delete_sources: true,
        chunk_bytes: 1,
        ..PackOptions::default()
    };

    let err = pack_dump(
        &dump,
        &archive,
        &opts,
        &MockZstd,
        Some(&cancel),
        Some(&mut |p| {
            if p.files_done >= 3 {
                cancel.store(true, Ordering::Relaxed);
            }
        }),
    )
    .expect_err("expected cancellation");
    assert_eq!(err.0, PackResult::Cancelled);
    assert!(has_resumable_journal(&archive));

    let journal_text =
        fs::read_to_string(format!("{}.journal", archive.display())).expect("read journal");
    let first_line = journal_text.lines().next().unwrap();
    let parsed_plan = plan_from_json(first_line).expect("plan_from_json");
    assert_eq!(parsed_plan.packages.len(), 3);

    // Resume to completion even though early sources were already deleted.
    pack_dump(&dump, &archive, &opts, &MockZstd, None, None).expect("resume pack_dump");
    assert!(!has_resumable_journal(&archive));
    assert!(!dump.join("base/exefs/main").exists());

    let bytes = fs::read(&archive).unwrap();
    verify_integrity(&SliceReader(&bytes), &MockZstd, None).expect("resumed archive verifies");
}

#[test]
fn edit_nup_metadata_and_no_change() {
    let tmp = TempDir::new("edit_meta");
    let dump = tmp.path().join("game");
    build_sample_multi_dump(&dump);
    let archive = tmp.path().join("game.nup");
    pack_dump(&dump, &archive, &PackOptions::default(), &MockZstd, None, None).unwrap();

    let edit = MetadataEdit {
        title_name: Some("Edited Title".to_string()),
        publisher: Some("Edited Pub".to_string()),
        display_version: Some("9.9.9".to_string()),
        icon: Some(vec![1, 2, 3, 4]),
        localized_titles: Some(vec![("Japanese".to_string(), "タイトル".to_string())]),
    };
    let res = edit_nup_metadata(&archive, &edit, None, None).unwrap();
    assert_eq!(res, EditResult::Success);

    let probed = probe_nup_metadata(&archive);
    assert_eq!(probed.title_name, "Edited Title");
    assert_eq!(probed.publisher, "Edited Pub");
    assert_eq!(probed.display_version, "9.9.9");
    assert_eq!(probed.icon, vec![1, 2, 3, 4]);
    assert_eq!(
        probed.localized_titles,
        vec![("Japanese".to_string(), "タイトル".to_string())]
    );

    let res2 = edit_nup_metadata(&archive, &edit, None, None).unwrap();
    assert_eq!(res2, EditResult::NoChange);

    let bytes = fs::read(&archive).unwrap();
    verify_integrity(&SliceReader(&bytes), &MockZstd, None).expect("edited archive verifies");
}

#[test]
fn malformed_toc_bounds_and_cycle_rejected() {
    assert!(NupContainer::parse(&SliceReader(b"short")).is_err());

    // Build minimal TOC with cycle: dir 0 -> dir 0.
    let mut toc = vec![0u8; TOC_HEADER_SIZE + 0x40 + 0x10 + 4];
    put_u32_le(&mut toc, 0x00, TOC_MAGIC);
    put_u32_le(&mut toc, 0x04, 1); // 1 package
    put_u64_le(&mut toc, 0x08, 1); // 1 dir
    put_u64_le(&mut toc, 0x10, 0); // 0 files
    let toc_len = toc.len() as u64;
    put_u64_le(&mut toc, 0x18, toc_len); // pool offset
    put_u64_le(&mut toc, 0x20, 0); // pool size
    // Dir 0 points to child slice [0] -> dir 0 (cycle!)
    let dir_off = TOC_HEADER_SIZE + 0x40;
    put_u32_le(&mut toc, dir_off + 0x08, 0);
    put_u32_le(&mut toc, dir_off + 0x0C, 1);
    put_u32_le(&mut toc, dir_off + 0x10, 0);

    let toc_sha = sha256(&toc);
    let mut file = vec![0u8; SUPERBLOCK_SIZE + toc.len()];
    file[..4].copy_from_slice(&HEADER_MAGIC);
    put_u16_le(&mut file, 0x04, SUPERBLOCK_SIZE as u16);
    put_u16_le(&mut file, 0x06, 1);
    put_u64_le(&mut file, 0x08, SUPERBLOCK_SIZE as u64);
    put_u64_le(&mut file, 0x10, toc.len() as u64);
    file[0x18..0x38].copy_from_slice(&toc_sha);
    put_u64_le(&mut file, 0x38, (SUPERBLOCK_SIZE + toc.len()) as u64);
    let total_file_len = file.len() as u64;
    put_u64_le(&mut file, 0x40, total_file_len);
    put_u32_le(&mut file, 0x48, 1);
    file[SUPERBLOCK_SIZE..].copy_from_slice(&toc);

    assert!(NupContainer::parse(&SliceReader(&file)).is_err());

    // Verify helper read_extent rejects invalid codec / OOB.
    assert!(read_extent(&SliceReader(&file), &MockZstd, 0, 1000, 1000, 0).is_err());
    assert!(read_extent(&SliceReader(&file), &MockZstd, 0, 4, 4, 99).is_err());
}

#[test]
fn ffi_roundtrip_null_safety_and_idempotent_free() {
    // ABI checks for generated FFI structs.
    assert!(std::mem::size_of::<EglNupPackageInfo>() >= 120);
    assert_eq!(std::mem::size_of::<EglNupFileNode>(), 80);

    unsafe {
        assert_eq!(egl_nup_is_nup_magic(ptr::null(), 0, 128), 0);
        assert_eq!(egl_nup_is_loose_dump(ptr::null(), 0), 0);
        assert_eq!(egl_nup_has_resumable_journal(ptr::null(), 0), 0);
        assert_eq!(egl_nup_container_parse(ptr::null(), ptr::null_mut()), -1);

        let unaligned_ptr = 1usize as *mut EglNupParsedContainer;
        egl_nup_container_free(unaligned_ptr);
        egl_nup_free_bytes(ptr::null_mut());
        egl_nup_free_string(ptr::null_mut());
        egl_nup_free_metadata(ptr::null_mut());
    }

    let tmp = TempDir::new("ffi");
    let dump = tmp.path().join("dump");
    build_sample_multi_dump(&dump);
    let archive = tmp.path().join("ffi.nup");

    let dump_str = dump.to_str().unwrap();
    let arch_str = archive.to_str().unwrap();
    let zstd = mock_c_zstd_ops();

    unsafe {
        assert_eq!(
            egl_nup_is_loose_dump(dump_str.as_ptr().cast::<c_char>(), dump_str.len()),
            1
        );

        let mut err_str = EglNupOwnedString {
            data: ptr::null_mut(),
            len: 0,
        };
        let opts = EglNupPackOptions {
            delete_sources: 0,
            _pad: [0; 3],
            zstd_level: 3,
            chunk_bytes: 64 * 1024 * 1024,
            raw_threshold: DEFAULT_RAW_THRESHOLD,
            nacp_language_index: 1,
            _pad2: 0,
            metadata: EglNupMetadata {
                title_name: ptr::null(),
                title_name_len: 0,
                icon: ptr::null(),
                icon_len: 0,
                display_version: ptr::null(),
                display_version_len: 0,
                publisher: ptr::null(),
                publisher_len: 0,
                localized_titles: ptr::null(),
                localized_title_count: 0,
            },
        };
        let pack_res = egl_nup_pack_dump(
            dump_str.as_ptr().cast::<c_char>(),
            dump_str.len(),
            arch_str.as_ptr().cast::<c_char>(),
            arch_str.len(),
            &opts,
            &zstd,
            None,
            ptr::null_mut(),
            None,
            ptr::null_mut(),
            &mut err_str,
        );
        assert_eq!(pack_res, EglNupPackResult::Success);
        egl_nup_free_string(&mut err_str);

        let mut probed = EglNupOwnedMetadata {
            title_name: EglNupOwnedString {
                data: ptr::null_mut(),
                len: 0,
            },
            icon: EglNupOwnedBytes {
                data: ptr::null_mut(),
                len: 0,
            },
            display_version: EglNupOwnedString {
                data: ptr::null_mut(),
                len: 0,
            },
            publisher: EglNupOwnedString {
                data: ptr::null_mut(),
                len: 0,
            },
            localized_titles: ptr::null_mut(),
            localized_title_count: 0,
            version_source: EglNupOwnedString {
                data: ptr::null_mut(),
                len: 0,
            },
        };
        assert_eq!(
            egl_nup_probe_metadata(
                dump_str.as_ptr().cast::<c_char>(),
                dump_str.len(),
                1,
                &mut probed,
            ),
            0
        );
        egl_nup_free_metadata(&mut probed);
        assert_eq!(
            egl_nup_probe_nup_metadata(
                arch_str.as_ptr().cast::<c_char>(),
                arch_str.len(),
                &mut probed,
            ),
            0
        );
        egl_nup_free_metadata(&mut probed);
        egl_nup_free_metadata(&mut probed); // idempotent free check

        let mut bytes = fs::read(&archive).unwrap();
        let vfs = EglNupVfsOps {
            ctx: (&mut bytes as *mut Vec<u8>).cast::<c_void>(),
            size: bytes.len() as u64,
            read_at: Some(vec_vfs_read_at),
        };
        let mut parsed = std::mem::zeroed::<EglNupParsedContainer>();
        assert_eq!(egl_nup_container_parse(&vfs, &mut parsed), 0);
        assert_eq!(parsed.package_count, 3);
        assert_eq!(egl_nup_verify_integrity(&vfs, &zstd, None, ptr::null_mut()), 0);

        let f0 = &*parsed.files;
        let mut out_bytes = EglNupOwnedBytes {
            data: ptr::null_mut(),
            len: 0,
        };
        assert_eq!(
            egl_nup_read_extent(
                &vfs,
                &zstd,
                f0.extent_offset,
                f0.extent_size,
                f0.size,
                f0.codec,
                &mut out_bytes,
            ),
            0
        );
        assert_eq!(out_bytes.len as u64, f0.size);
        egl_nup_free_bytes(&mut out_bytes);
        egl_nup_free_bytes(&mut out_bytes); // idempotent free check

        egl_nup_container_free(&mut parsed);
        egl_nup_container_free(&mut parsed); // idempotent free check

        let new_title = "FFI Edited Title";
        let edit = EglNupMetadataEdit {
            has_title_name: 1,
            has_icon: 0,
            has_display_version: 0,
            has_publisher: 0,
            has_localized_titles: 0,
            _pad: [0; 3],
            values: EglNupMetadata {
                title_name: new_title.as_ptr().cast::<c_char>(),
                title_name_len: new_title.len(),
                icon: ptr::null(),
                icon_len: 0,
                display_version: ptr::null(),
                display_version_len: 0,
                publisher: ptr::null(),
                publisher_len: 0,
                localized_titles: ptr::null(),
                localized_title_count: 0,
            },
        };
        assert_eq!(
            egl_nup_edit_metadata(
                arch_str.as_ptr().cast::<c_char>(),
                arch_str.len(),
                &edit,
                None,
                ptr::null_mut(),
                None,
                ptr::null_mut(),
                &mut err_str,
            ),
            EglNupEditResult::Success
        );
        egl_nup_free_string(&mut err_str);

        let extract_dir = tmp.path().join("ffi_out");
        let ext_str = extract_dir.to_str().unwrap();
        assert_eq!(
            egl_nup_extract(
                arch_str.as_ptr().cast::<c_char>(),
                arch_str.len(),
                ext_str.as_ptr().cast::<c_char>(),
                ext_str.len(),
                &zstd,
                None,
                ptr::null_mut(),
                None,
                ptr::null_mut(),
                &mut err_str,
            ),
            EglNupExtractResult::Success
        );
        egl_nup_free_string(&mut err_str);
        assert!(extract_dir.join("base/exefs/main").is_file());
    }
}

#[test]
fn non_destructive_pack_skips_and_cleans_journal_on_cancel_and_error() {
    use eglantine_nup_fs::journal::{journal_path_for, part_path_for};

    let tmp = TempDir::new("non_destructive_journal");
    let dump = tmp.path().join("game");
    build_sample_multi_dump(&dump);
    let archive = tmp.path().join("game.nup");

    let opts = PackOptions {
        delete_sources: false,
        ..PackOptions::default()
    };

    // Cancellation during non-destructive pack must remove both .part and .journal.
    let cancel = AtomicBool::new(false);
    let err = pack_dump(
        &dump,
        &archive,
        &opts,
        &MockZstd,
        Some(&cancel),
        Some(&mut |p| {
            if p.files_done >= 2 {
                cancel.store(true, Ordering::Relaxed);
            }
        }),
    )
    .expect_err("expected cancellation");
    assert_eq!(err.0, PackResult::Cancelled);
    assert!(!has_resumable_journal(&archive));
    assert!(!part_path_for(&archive).exists());
    assert!(!journal_path_for(&archive).exists());
    assert!(dump.join("base/exefs/main").is_file());

    // Error mid-write during non-destructive pack must also clean up .part and .journal.
    let err_dump = tmp.path().join("err_game");
    write_file_bytes(&err_dump.join("base/exefs/main"), b"MAIN");
    write_file_bytes(&err_dump.join("base/exefs/main.npdm"), &make_npdm(0x0100_1234_5678_0000));
    write_file_bytes(&err_dump.join("base/romfs/second.bin"), &[0x55u8; 64]);
    let err_archive = tmp.path().join("err_game.nup");
    let seq_opts = PackOptions {
        delete_sources: false,
        raw_threshold: 10,
        ..PackOptions::default()
    };
    let io_err = pack_dump(
        &err_dump,
        &err_archive,
        &seq_opts,
        &MockZstd,
        None,
        Some(&mut |p| {
            if p.files_done == 1 {
                let _ = fs::remove_file(err_dump.join("base/exefs/main"));
                let _ = fs::remove_file(err_dump.join("base/exefs/main.npdm"));
                let _ = fs::remove_file(err_dump.join("base/romfs/second.bin"));
            }
        }),
    )
    .expect_err("expected mid-pack read error");
    assert_eq!(io_err.0, PackResult::IoError);
    assert!(!has_resumable_journal(&err_archive));
    assert!(!part_path_for(&err_archive).exists());
    assert!(!journal_path_for(&err_archive).exists());

    // Adding a file before retrying must pack the updated dump freshly rather than resuming stale state.
    write_file_bytes(&dump.join("base/romfs/added_after_cancel.bin"), b"NEW_FILE");
    pack_dump(&dump, &archive, &opts, &MockZstd, None, None).expect("fresh retry pack_dump");
    assert!(!has_resumable_journal(&archive));
    assert!(dump.join("base/exefs/main").is_file());

    let bytes = fs::read(&archive).unwrap();
    verify_integrity(&SliceReader(&bytes), &MockZstd, None).expect("retried archive verifies");
    let parsed = NupContainer::parse(&SliceReader(&bytes)).unwrap();
    assert!(parsed
        .files
        .iter()
        .any(|f| f.name == "added_after_cancel.bin"));
}

#[test]
fn ffi_cancel_polling_without_progress_cb_and_large_stream_f0416() {
    struct CancelAfterCalls {
        calls: AtomicU64,
        trigger_at: u64,
    }

    unsafe extern "C" fn poll_cancel_after(ctx: *mut c_void) -> u8 {
        let state = &*ctx.cast::<CancelAfterCalls>();
        let n = state.calls.fetch_add(1, Ordering::Relaxed) + 1;
        u8::from(n >= state.trigger_at)
    }

    let tmp = TempDir::new("cancel_poll");
    let big_dump = tmp.path().join("big_dump");
    // 9 MiB guarantees stream_large_file loops over multiple 4 MiB chunks.
    let big_data = vec![0x3Cu8; 9 * 1024 * 1024];
    write_file_bytes(&big_dump.join("base/exefs/main"), &big_data);
    write_file_bytes(
        &big_dump.join("base/exefs/main.npdm"),
        &make_npdm(0x0100_1234_5678_0000),
    );
    fs::create_dir_all(big_dump.join("base/romfs")).unwrap();
    let archive = tmp.path().join("big.nup");
    let dump_str = big_dump.to_str().unwrap();
    let arch_str = archive.to_str().unwrap();
    let zstd = mock_c_zstd_ops();

    let opts = EglNupPackOptions {
        delete_sources: 0,
        _pad: [0; 3],
        zstd_level: 3,
        chunk_bytes: 64 * 1024 * 1024,
        raw_threshold: 64 * 1024,
        nacp_language_index: 1,
        _pad2: 0,
        metadata: EglNupMetadata {
            title_name: ptr::null(),
            title_name_len: 0,
            icon: ptr::null(),
            icon_len: 0,
            display_version: ptr::null(),
            display_version_len: 0,
            publisher: ptr::null(),
            publisher_len: 0,
            localized_titles: ptr::null(),
            localized_title_count: 0,
        },
    };

    let mut cancel_state = CancelAfterCalls {
        calls: AtomicU64::new(0),
        trigger_at: 5,
    };
    let mut err_str = EglNupOwnedString {
        data: ptr::null_mut(),
        len: 0,
    };
    unsafe {
        let res = egl_nup_pack_dump(
            dump_str.as_ptr().cast::<c_char>(),
            dump_str.len(),
            arch_str.as_ptr().cast::<c_char>(),
            arch_str.len(),
            &opts,
            &zstd,
            None,
            ptr::null_mut(),
            Some(poll_cancel_after),
            (&mut cancel_state as *mut CancelAfterCalls).cast::<c_void>(),
            &mut err_str,
        );
        egl_nup_free_string(&mut err_str);
        assert_eq!(res, EglNupPackResult::Cancelled);
        assert_eq!(cancel_state.calls.load(Ordering::Relaxed), 5);
    }

    // Now build a valid 9 MiB NUP and verify extract and edit_metadata poll cancel_cb with progress_cb=None.
    let valid_dump = tmp.path().join("valid_9m");
    write_file_bytes(&valid_dump.join("base/exefs/main"), &vec![0x5Au8; 9 * 1024 * 1024]);
    write_file_bytes(
        &valid_dump.join("base/exefs/main.npdm"),
        &make_npdm(0x0100_1234_5678_0000),
    );
    write_file_bytes(
        &valid_dump.join("base/control.nacp"),
        &make_nacp("Big Game", "Pub", "1.0.0"),
    );
    fs::create_dir_all(valid_dump.join("base/romfs")).unwrap();
    let valid_archive = tmp.path().join("valid.nup");
    let valid_opts = PackOptions {
        raw_threshold: 64 * 1024,
        ..PackOptions::default()
    };
    pack_dump(&valid_dump, &valid_archive, &valid_opts, &MockZstd, None, None).unwrap();

    let out_dir = tmp.path().join("extract_cancelled");
    let valid_arch_str = valid_archive.to_str().unwrap();
    let out_str = out_dir.to_str().unwrap();
    let mut ex_cancel = CancelAfterCalls {
        calls: AtomicU64::new(0),
        trigger_at: 5,
    };
    unsafe {
        let res = egl_nup_extract(
            valid_arch_str.as_ptr().cast::<c_char>(),
            valid_arch_str.len(),
            out_str.as_ptr().cast::<c_char>(),
            out_str.len(),
            &zstd,
            None,
            ptr::null_mut(),
            Some(poll_cancel_after),
            (&mut ex_cancel as *mut CancelAfterCalls).cast::<c_void>(),
            &mut err_str,
        );
        egl_nup_free_string(&mut err_str);
        assert_eq!(res, EglNupExtractResult::Cancelled);
        assert!(ex_cancel.calls.load(Ordering::Relaxed) >= 5);
    }

    let new_title = b"Cancelled Title";
    let raw_edit = EglNupMetadataEdit {
        has_title_name: 1,
        has_icon: 0,
        has_display_version: 0,
        has_publisher: 0,
        has_localized_titles: 0,
        _pad: [0; 3],
        values: EglNupMetadata {
            title_name: new_title.as_ptr().cast::<c_char>(),
            title_name_len: new_title.len(),
            icon: ptr::null(),
            icon_len: 0,
            display_version: ptr::null(),
            display_version_len: 0,
            publisher: ptr::null(),
            publisher_len: 0,
            localized_titles: ptr::null(),
            localized_title_count: 0,
        },
    };
    let mut ed_cancel = CancelAfterCalls {
        calls: AtomicU64::new(0),
        trigger_at: 3,
    };
    unsafe {
        let res = egl_nup_edit_metadata(
            valid_arch_str.as_ptr().cast::<c_char>(),
            valid_arch_str.len(),
            &raw_edit,
            None,
            ptr::null_mut(),
            Some(poll_cancel_after),
            (&mut ed_cancel as *mut CancelAfterCalls).cast::<c_void>(),
            &mut err_str,
        );
        egl_nup_free_string(&mut err_str);
        assert_eq!(res, EglNupEditResult::Cancelled);
        assert!(ed_cancel.calls.load(Ordering::Relaxed) >= 3);
    }
}

#[test]
fn probe_dump_metadata_multibyte_product_tag_f0542() {
    let tmp = TempDir::new("multibyte_tag");
    let dump = tmp.path().join("dump");
    write_file_bytes(&dump.join("base/exefs/main"), b"MAIN");
    write_file_bytes(
        &dump.join("base/exefs/main.npdm"),
        &make_npdm(0x0100_1234_5678_0000),
    );
    write_file_bytes(
        &dump.join("base/romfs/System/Resource/ResourceSizeTable.Product.1\u{00e9}0.rsizetable.byml"),
        b"bad",
    );
    write_file_bytes(
        &dump.join("base/romfs/GameData/GameDataList.Product.\u{3042}00.byml"),
        b"bad",
    );
    let only_invalid = probe_dump_metadata(&dump, 1);
    assert_eq!(only_invalid.display_version, "");

    write_file_bytes(
        &dump.join("base/romfs/System/Resource/ResourceSizeTable.Product.200.rsizetable.byml"),
        b"good",
    );
    let valid = probe_dump_metadata(&dump, 1);
    assert_eq!(valid.display_version, "2.0.0");
}

#[test]
fn reject_shared_dir_dag_rootless_cycle_and_raw_extent_mismatch_f0462_f0426() {
    fn synthetic_blob_offset(dir_count: usize, file_count: usize, child_count: usize) -> u64 {
        let toc_len =
            TOC_HEADER_SIZE + 0x40 + dir_count * 0x18 + file_count * 0x50 + child_count * 4;
        ((SUPERBLOCK_SIZE + toc_len + 15) & !15usize) as u64
    }

    fn build_synthetic_nup(
        dirs: &[(u32, u32)],
        files: &[(u64, u64, u64, u8)],
        children: &[u32],
        blob_size: usize,
    ) -> Vec<u8> {
        let pkg_bytes = 0x40usize;
        let dir_bytes = dirs.len() * 0x18;
        let file_bytes = files.len() * 0x50;
        let child_bytes = children.len() * 4;
        let mut toc = vec![0u8; TOC_HEADER_SIZE + pkg_bytes + dir_bytes + file_bytes + child_bytes];
        let toc_len = toc.len() as u64;
        put_u32_le(&mut toc, 0x00, TOC_MAGIC);
        put_u32_le(&mut toc, 0x04, 1); // 1 package -> root_dir = 0
        put_u64_le(&mut toc, 0x08, dirs.len() as u64);
        put_u64_le(&mut toc, 0x10, files.len() as u64);
        put_u64_le(&mut toc, 0x18, toc_len);
        put_u64_le(&mut toc, 0x20, 0);

        let mut off = TOC_HEADER_SIZE + pkg_bytes;
        for &(first_child, child_count) in dirs {
            put_u32_le(&mut toc, off + 0x08, first_child);
            put_u32_le(&mut toc, off + 0x0C, child_count);
            off += 0x18;
        }
        for &(size, extent_offset, extent_size, codec) in files {
            put_u64_le(&mut toc, off + 0x08, size);
            put_u64_le(&mut toc, off + 0x10, extent_offset);
            put_u64_le(&mut toc, off + 0x18, extent_size);
            toc[off + 0x40] = codec;
            off += 0x50;
        }
        for &c in children {
            put_u32_le(&mut toc, off, c);
            off += 4;
        }

        let blob_off = (SUPERBLOCK_SIZE + toc.len() + 15) & !15usize;
        let total_size = blob_off + blob_size;
        let toc_sha = sha256(&toc);
        let mut file = vec![0u8; total_size];
        file[..4].copy_from_slice(&HEADER_MAGIC);
        put_u16_le(&mut file, 0x04, SUPERBLOCK_SIZE as u16);
        put_u16_le(&mut file, 0x06, 1);
        put_u64_le(&mut file, 0x08, SUPERBLOCK_SIZE as u64);
        put_u64_le(&mut file, 0x10, toc.len() as u64);
        file[0x18..0x38].copy_from_slice(&toc_sha);
        put_u64_le(&mut file, 0x38, blob_off as u64);
        put_u64_le(&mut file, 0x40, total_size as u64);
        put_u32_le(&mut file, 0x48, 1);
        file[SUPERBLOCK_SIZE..SUPERBLOCK_SIZE + toc.len()].copy_from_slice(&toc);
        file
    }

    // F0462 (a): shared directory diamond DAG (dir 0 -> {1, 2}, dir 1 -> {3}, dir 2 -> {3}).
    let dag = build_synthetic_nup(&[(0, 2), (2, 1), (3, 1), (0, 0)], &[], &[1, 2, 3, 3], 0);
    assert!(
        NupContainer::parse(&SliceReader(&dag)).is_err(),
        "shared directory DAG must be rejected"
    );

    // F0462 (b): rootless cycle unreachable from root (dir 0 -> [], dir 1 <-> dir 2).
    let rootless_cycle = build_synthetic_nup(&[(0, 0), (0, 1), (1, 1)], &[], &[2, 1], 0);
    assert!(
        NupContainer::parse(&SliceReader(&rootless_cycle)).is_err(),
        "rootless directory cycle must be rejected"
    );

    // F0426: CODEC_RAW with extent_size != size within blob bounds must be rejected.
    let blob_off = synthetic_blob_offset(1, 1, 1);
    let raw_larger = build_synthetic_nup(&[(0, 1)], &[(32, blob_off, 64, 0)], &[1], 128);
    assert!(
        NupContainer::parse(&SliceReader(&raw_larger)).is_err(),
        "CODEC_RAW with extent_size > size must be rejected"
    );
    let raw_smaller = build_synthetic_nup(&[(0, 1)], &[(64, blob_off, 32, 0)], &[1], 128);
    assert!(
        NupContainer::parse(&SliceReader(&raw_smaller)).is_err(),
        "CODEC_RAW with extent_size < size must be rejected"
    );
    let raw_ok = build_synthetic_nup(&[(0, 1)], &[(32, blob_off, 32, 0)], &[1], 128);
    assert!(NupContainer::parse(&SliceReader(&raw_ok)).is_ok());
}

#[test]
fn ffi_uninitialized_out_params_and_isize_slice_bounds_f0479_f0509() {
    fn poisoned_uninit<T>() -> MaybeUninit<T> {
        let mut slot = MaybeUninit::<T>::uninit();
        // SAFETY: Writing arbitrary byte pattern into uninitialized storage is valid.
        unsafe {
            ptr::write_bytes(slot.as_mut_ptr(), 0xAA, 1);
        }
        slot
    }

    let tmp = TempDir::new("ffi_uninit");
    let dump = tmp.path().join("dump");
    build_sample_multi_dump(&dump);
    let archive = tmp.path().join("ffi_uninit.nup");
    let dump_str = dump.to_str().unwrap();
    let arch_str = archive.to_str().unwrap();
    let main_path = dump.join("base/exefs/main");
    let main_str = main_path.to_str().unwrap();
    let zstd = mock_c_zstd_ops();

    unsafe {
        // F0509: lengths > isize::MAX must be rejected cleanly before from_raw_parts.
        let dummy_byte: u8 = 0;
        assert_eq!(
            egl_nup_is_nup_magic(&dummy_byte, (isize::MAX as usize) + 1, 128),
            0
        );

        // F0479: error paths with poisoned uninitialized out-params must zero-init without freeing garbage.
        let mut bad_container = poisoned_uninit::<EglNupParsedContainer>();
        assert_eq!(
            egl_nup_container_parse(ptr::null(), bad_container.as_mut_ptr()),
            -1
        );
        let mut bad_container = bad_container.assume_init();
        assert!(bad_container.packages.is_null());
        egl_nup_container_free(&mut bad_container);

        let mut bad_bytes = poisoned_uninit::<EglNupOwnedBytes>();
        assert_eq!(
            egl_nup_read_extent(ptr::null(), &zstd, 0, 0, 0, 0, bad_bytes.as_mut_ptr()),
            -1
        );
        let mut bad_bytes = bad_bytes.assume_init();
        assert!(bad_bytes.data.is_null());
        egl_nup_free_bytes(&mut bad_bytes);

        let mut bad_root = poisoned_uninit::<EglNupOwnedString>();
        assert_eq!(
            egl_nup_dump_root_from_main(ptr::null(), 0, bad_root.as_mut_ptr()),
            0
        );
        let mut bad_root = bad_root.assume_init();
        assert!(bad_root.data.is_null());
        egl_nup_free_string(&mut bad_root);

        let mut bad_meta = poisoned_uninit::<EglNupOwnedMetadata>();
        assert_eq!(
            egl_nup_probe_metadata(ptr::null(), 0, 1, bad_meta.as_mut_ptr()),
            -1
        );
        let mut bad_meta = bad_meta.assume_init();
        assert!(bad_meta.title_name.data.is_null());
        egl_nup_free_metadata(&mut bad_meta);

        let mut bad_nup_meta = poisoned_uninit::<EglNupOwnedMetadata>();
        assert_eq!(
            egl_nup_probe_nup_metadata(ptr::null(), 0, bad_nup_meta.as_mut_ptr()),
            -1
        );
        let mut bad_nup_meta = bad_nup_meta.assume_init();
        assert!(bad_nup_meta.title_name.data.is_null());
        egl_nup_free_metadata(&mut bad_nup_meta);

        let mut bad_arc_path = poisoned_uninit::<EglNupOwnedString>();
        assert_eq!(
            egl_nup_default_archive_path(ptr::null(), 0, bad_arc_path.as_mut_ptr()),
            -1
        );
        let mut bad_arc_path = bad_arc_path.assume_init();
        assert!(bad_arc_path.data.is_null());
        egl_nup_free_string(&mut bad_arc_path);

        let mut bad_ext_path = poisoned_uninit::<EglNupOwnedString>();
        assert_eq!(
            egl_nup_default_extract_path(ptr::null(), 0, bad_ext_path.as_mut_ptr()),
            -1
        );
        let mut bad_ext_path = bad_ext_path.assume_init();
        assert!(bad_ext_path.data.is_null());
        egl_nup_free_string(&mut bad_ext_path);

        let mut pack_err = poisoned_uninit::<EglNupOwnedString>();
        assert_eq!(
            egl_nup_pack_dump(
                ptr::null(),
                0,
                arch_str.as_ptr().cast::<c_char>(),
                arch_str.len(),
                ptr::null(),
                &zstd,
                None,
                ptr::null_mut(),
                None,
                ptr::null_mut(),
                pack_err.as_mut_ptr(),
            ),
            EglNupPackResult::InvalidSource
        );
        let mut pack_err = pack_err.assume_init();
        assert!(!pack_err.data.is_null());
        egl_nup_free_string(&mut pack_err);

        // Success paths with poisoned uninitialized out-params.
        let mut ok_err = poisoned_uninit::<EglNupOwnedString>();
        assert_eq!(
            egl_nup_pack_dump(
                dump_str.as_ptr().cast::<c_char>(),
                dump_str.len(),
                arch_str.as_ptr().cast::<c_char>(),
                arch_str.len(),
                ptr::null(),
                &zstd,
                None,
                ptr::null_mut(),
                None,
                ptr::null_mut(),
                ok_err.as_mut_ptr(),
            ),
            EglNupPackResult::Success
        );
        let mut ok_err = ok_err.assume_init();
        assert!(ok_err.data.is_null());
        egl_nup_free_string(&mut ok_err);

        let mut ok_root = poisoned_uninit::<EglNupOwnedString>();
        assert_eq!(
            egl_nup_dump_root_from_main(
                main_str.as_ptr().cast::<c_char>(),
                main_str.len(),
                ok_root.as_mut_ptr(),
            ),
            1
        );
        let mut ok_root = ok_root.assume_init();
        assert!(!ok_root.data.is_null());
        egl_nup_free_string(&mut ok_root);

        let mut ok_arc_path = poisoned_uninit::<EglNupOwnedString>();
        assert_eq!(
            egl_nup_default_archive_path(
                dump_str.as_ptr().cast::<c_char>(),
                dump_str.len(),
                ok_arc_path.as_mut_ptr(),
            ),
            0
        );
        let mut ok_arc_path = ok_arc_path.assume_init();
        assert!(!ok_arc_path.data.is_null());
        egl_nup_free_string(&mut ok_arc_path);

        let mut ok_ext_path = poisoned_uninit::<EglNupOwnedString>();
        assert_eq!(
            egl_nup_default_extract_path(
                arch_str.as_ptr().cast::<c_char>(),
                arch_str.len(),
                ok_ext_path.as_mut_ptr(),
            ),
            0
        );
        let mut ok_ext_path = ok_ext_path.assume_init();
        assert!(!ok_ext_path.data.is_null());
        egl_nup_free_string(&mut ok_ext_path);

        let mut ok_meta = poisoned_uninit::<EglNupOwnedMetadata>();
        assert_eq!(
            egl_nup_probe_metadata(
                dump_str.as_ptr().cast::<c_char>(),
                dump_str.len(),
                1,
                ok_meta.as_mut_ptr(),
            ),
            0
        );
        let mut ok_meta = ok_meta.assume_init();
        egl_nup_free_metadata(&mut ok_meta);

        let mut ok_nup_meta = poisoned_uninit::<EglNupOwnedMetadata>();
        assert_eq!(
            egl_nup_probe_nup_metadata(
                arch_str.as_ptr().cast::<c_char>(),
                arch_str.len(),
                ok_nup_meta.as_mut_ptr(),
            ),
            0
        );
        let mut ok_nup_meta = ok_nup_meta.assume_init();
        egl_nup_free_metadata(&mut ok_nup_meta);

        let mut bytes = fs::read(&archive).unwrap();
        let vfs = EglNupVfsOps {
            ctx: (&mut bytes as *mut Vec<u8>).cast::<c_void>(),
            size: bytes.len() as u64,
            read_at: Some(vec_vfs_read_at),
        };
        let mut ok_container = poisoned_uninit::<EglNupParsedContainer>();
        assert_eq!(egl_nup_container_parse(&vfs, ok_container.as_mut_ptr()), 0);
        let mut ok_container = ok_container.assume_init();
        assert!(ok_container.file_count > 0);
        let f0 = &*ok_container.files;
        let mut ok_bytes = poisoned_uninit::<EglNupOwnedBytes>();
        assert_eq!(
            egl_nup_read_extent(
                &vfs,
                &zstd,
                f0.extent_offset,
                f0.extent_size,
                f0.size,
                f0.codec,
                ok_bytes.as_mut_ptr(),
            ),
            0
        );
        let mut ok_bytes = ok_bytes.assume_init();
        assert_eq!(ok_bytes.len as u64, f0.size);
        egl_nup_free_bytes(&mut ok_bytes);
        egl_nup_container_free(&mut ok_container);

        let mut edit_err = poisoned_uninit::<EglNupOwnedString>();
        assert_eq!(
            egl_nup_edit_metadata(
                ptr::null(),
                0,
                ptr::null(),
                None,
                ptr::null_mut(),
                None,
                ptr::null_mut(),
                edit_err.as_mut_ptr(),
            ),
            EglNupEditResult::InvalidSource
        );
        let mut edit_err = edit_err.assume_init();
        assert!(!edit_err.data.is_null());
        egl_nup_free_string(&mut edit_err);

        let mut extract_err = poisoned_uninit::<EglNupOwnedString>();
        assert_eq!(
            egl_nup_extract(
                ptr::null(),
                0,
                ptr::null(),
                0,
                &zstd,
                None,
                ptr::null_mut(),
                None,
                ptr::null_mut(),
                extract_err.as_mut_ptr(),
            ),
            EglNupExtractResult::InvalidSource
        );
        let mut extract_err = extract_err.assume_init();
        assert!(!extract_err.data.is_null());
        egl_nup_free_string(&mut extract_err);
    }
}
