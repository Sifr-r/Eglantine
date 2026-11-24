// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![deny(warnings)]

pub mod ffi;
pub mod format;
pub mod journal;
pub mod metadata;
pub mod reader;
pub mod writer;

pub use format::{
    align_up, sha256, ReadAt, Sha256, ZstdCodec, CODEC_RAW, CODEC_ZSTD, DEFAULT_RAW_THRESHOLD,
    EXTENT_ALIGNMENT, FILE_RECORD_SIZE, HEADER_MAGIC, NO_DIR, PACKAGE_FLAG_STANDALONE_UPDATE,
    PACKAGE_RECORD_SIZE, SUPERBLOCK_SIZE, TLV_HEADER_SIZE, TOC_HEADER_SIZE, TOC_MAGIC,
    TOC_MAGIC_BYTES, DIR_RECORD_SIZE,
};
pub use journal::{has_resumable_journal, JournalExtent, JournalLine};
pub use metadata::{
    edit_nup_metadata, parse_tlvs, probe_dump_metadata, probe_nup_metadata, serialize_tlvs,
    EditResult, Metadata, MetadataEdit, MetaType,
};
pub use reader::{
    extract_nup, read_extent, verify_integrity, DirNode, ExtractResult, FileNode, NupContainer,
    PackageInfo, PackageKind, SliceReader,
};
pub use writer::{
    default_archive_path, default_extract_path, dump_root_from_main, is_loose_dump, pack_dump,
    PackOptions, PackProgress, PackResult, Plan, PlanEntry, PlanPackage,
};
