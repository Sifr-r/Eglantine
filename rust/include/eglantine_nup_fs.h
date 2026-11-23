// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EGL_NUP_DEFAULT_RAW_THRESHOLD (256ULL * 1024ULL * 1024ULL)

typedef struct EglNupZstdOps {
    size_t (*compress)(void* dst,
                       size_t dst_capacity,
                       const void* src,
                       size_t src_size,
                       int compression_level);
    size_t (*decompress)(void* dst, size_t dst_capacity, const void* src, size_t compressed_size);
    size_t (*compress_bound)(size_t src_size);
    unsigned int (*is_error)(size_t code);
} EglNupZstdOps;

typedef struct EglNupVfsOps {
    void* ctx;
    uint64_t size;
    size_t (*read_at)(void* ctx, uint64_t offset, uint8_t* dst, size_t len);
} EglNupVfsOps;

typedef enum EglNupPackResult {
    EGL_NUP_PACK_SUCCESS = 0,
    EGL_NUP_PACK_CANCELLED = 1,
    EGL_NUP_PACK_INVALID_SOURCE = 2,
    EGL_NUP_PACK_IO_ERROR = 3,
} EglNupPackResult;

typedef enum EglNupExtractResult {
    EGL_NUP_EXTRACT_SUCCESS = 0,
    EGL_NUP_EXTRACT_CANCELLED = 1,
    EGL_NUP_EXTRACT_INVALID_SOURCE = 2,
    EGL_NUP_EXTRACT_IO_ERROR = 3,
} EglNupExtractResult;

typedef enum EglNupEditResult {
    EGL_NUP_EDIT_SUCCESS = 0,
    EGL_NUP_EDIT_CANCELLED = 1,
    EGL_NUP_EDIT_INVALID_SOURCE = 2,
    EGL_NUP_EDIT_IO_ERROR = 3,
    EGL_NUP_EDIT_NO_CHANGE = 4,
} EglNupEditResult;

typedef struct EglNupPackProgress {
    uint64_t bytes_done;
    uint64_t bytes_total;
    uint64_t files_done;
    uint64_t files_total;
} EglNupPackProgress;

typedef void (*EglNupProgressCallback)(void* ctx, const EglNupPackProgress* progress);
typedef uint8_t (*EglNupCancelCallback)(void* ctx);
typedef uint8_t (*EglNupVerifyProgressCallback)(void* ctx, size_t done, size_t total);

typedef struct EglNupLocalizedPair {
    const char* lang;
    size_t lang_len;
    const char* value;
    size_t value_len;
} EglNupLocalizedPair;

typedef struct EglNupMetadata {
    const char* title_name;
    size_t title_name_len;
    const uint8_t* icon;
    size_t icon_len;
    const char* display_version;
    size_t display_version_len;
    const char* publisher;
    size_t publisher_len;
    const EglNupLocalizedPair* localized_titles;
    size_t localized_title_count;
} EglNupMetadata;

typedef struct EglNupOwnedString {
    char* data;
    size_t len;
} EglNupOwnedString;

typedef struct EglNupOwnedBytes {
    uint8_t* data;
    size_t len;
} EglNupOwnedBytes;

typedef struct EglNupOwnedLocalizedPair {
    EglNupOwnedString lang;
    EglNupOwnedString value;
} EglNupOwnedLocalizedPair;

typedef struct EglNupOwnedLocalizedBytesPair {
    EglNupOwnedString lang;
    EglNupOwnedBytes data;
} EglNupOwnedLocalizedBytesPair;

typedef struct EglNupOwnedMetadata {
    EglNupOwnedString title_name;
    EglNupOwnedBytes icon;
    EglNupOwnedString display_version;
    EglNupOwnedString publisher;
    EglNupOwnedLocalizedPair* localized_titles;
    size_t localized_title_count;
    EglNupOwnedString version_source;
} EglNupOwnedMetadata;

typedef struct EglNupPackOptions {
    uint8_t delete_sources;
    uint8_t _pad[3];
    int32_t zstd_level;
    uint64_t chunk_bytes;
    uint64_t raw_threshold;
    int32_t nacp_language_index;
    uint32_t _pad2;
    EglNupMetadata metadata;
} EglNupPackOptions;

typedef struct EglNupMetadataEdit {
    uint8_t has_title_name;
    uint8_t has_icon;
    uint8_t has_display_version;
    uint8_t has_publisher;
    uint8_t has_localized_titles;
    uint8_t _pad[3];
    EglNupMetadata values;
} EglNupMetadataEdit;

typedef struct EglNupPackageInfo {
    uint64_t title_id;
    uint32_t version;
    uint8_t kind;
    uint8_t standalone_update;
    uint8_t _pad[2];
    uint32_t root_dir;
    uint32_t exefs_dir;
    uint32_t romfs_dir;
    uint64_t meta_offset;
    uint32_t meta_size;
    uint32_t _pad2;
    EglNupOwnedBytes npdm;
    EglNupOwnedString title_name;
    EglNupOwnedString publisher;
    EglNupOwnedString source_dump;
    EglNupOwnedLocalizedPair* localized_titles;
    size_t localized_title_count;
    EglNupOwnedBytes icon;
    EglNupOwnedLocalizedBytesPair* localized_icons;
    size_t localized_icon_count;
    EglNupOwnedString display_version;
} EglNupPackageInfo;

typedef struct EglNupDirNode {
    EglNupOwnedString name;
    uint32_t first_child;
    uint32_t child_count;
} EglNupDirNode;

typedef struct EglNupFileNode {
    EglNupOwnedString name;
    uint64_t size;
    uint8_t sha256[32];
    uint64_t extent_offset;
    uint64_t extent_size;
    uint8_t codec;
    uint8_t _pad[7];
} EglNupFileNode;

typedef struct EglNupParsedContainer {
    uint64_t toc_offset;
    uint64_t toc_size;
    uint64_t blob_offset;
    uint64_t file_size;
    EglNupPackageInfo* packages;
    size_t package_count;
    EglNupDirNode* dirs;
    size_t dir_count;
    EglNupFileNode* files;
    size_t file_count;
    uint32_t* children;
    size_t child_count;
} EglNupParsedContainer;

uint8_t egl_nup_is_nup_magic(const uint8_t* bytes, size_t len, uint64_t total_size);

int32_t egl_nup_container_parse(const EglNupVfsOps* vfs, EglNupParsedContainer* out_container);

void egl_nup_container_free(EglNupParsedContainer* container);

int32_t egl_nup_read_extent(const EglNupVfsOps* vfs,
                            const EglNupZstdOps* zstd,
                            uint64_t extent_offset,
                            uint64_t extent_size,
                            uint64_t uncompressed_size,
                            uint8_t codec,
                            EglNupOwnedBytes* out_bytes);

int32_t egl_nup_verify_integrity(const EglNupVfsOps* vfs,
                                 const EglNupZstdOps* zstd,
                                 EglNupVerifyProgressCallback progress_cb,
                                 void* progress_ctx);

uint8_t egl_nup_is_loose_dump(const char* dir_utf8, size_t dir_len);

uint8_t egl_nup_dump_root_from_main(const char* main_utf8,
                                    size_t main_len,
                                    EglNupOwnedString* out_root);

int32_t egl_nup_probe_metadata(const char* dump_root_utf8,
                               size_t dump_root_len,
                               int32_t nacp_language_index,
                               EglNupOwnedMetadata* out_meta);

int32_t egl_nup_probe_nup_metadata(const char* archive_utf8,
                                   size_t archive_len,
                                   EglNupOwnedMetadata* out_meta);

int32_t egl_nup_default_archive_path(const char* dump_root_utf8,
                                     size_t dump_root_len,
                                     EglNupOwnedString* out_path);

int32_t egl_nup_default_extract_path(const char* archive_utf8,
                                     size_t archive_len,
                                     EglNupOwnedString* out_path);

uint8_t egl_nup_has_resumable_journal(const char* archive_utf8, size_t archive_len);

EglNupPackResult egl_nup_pack_dump(const char* dump_root_utf8,
                                   size_t dump_root_len,
                                   const char* archive_utf8,
                                   size_t archive_len,
                                   const EglNupPackOptions* options,
                                   const EglNupZstdOps* zstd,
                                   EglNupProgressCallback progress_cb,
                                   void* progress_ctx,
                                   EglNupCancelCallback cancel_cb,
                                   void* cancel_ctx,
                                   EglNupOwnedString* out_error);

EglNupExtractResult egl_nup_extract(const char* archive_utf8,
                                    size_t archive_len,
                                    const char* out_dir_utf8,
                                    size_t out_dir_len,
                                    const EglNupZstdOps* zstd,
                                    EglNupProgressCallback progress_cb,
                                    void* progress_ctx,
                                    EglNupCancelCallback cancel_cb,
                                    void* cancel_ctx,
                                    EglNupOwnedString* out_error);

EglNupEditResult egl_nup_edit_metadata(const char* archive_utf8,
                                       size_t archive_len,
                                       const EglNupMetadataEdit* edit,
                                       EglNupProgressCallback progress_cb,
                                       void* progress_ctx,
                                       EglNupCancelCallback cancel_cb,
                                       void* cancel_ctx,
                                       EglNupOwnedString* out_error);

void egl_nup_free_bytes(EglNupOwnedBytes* bytes);
void egl_nup_free_string(EglNupOwnedString* str_val);
void egl_nup_free_metadata(EglNupOwnedMetadata* meta);

#ifdef __cplusplus
} // extern "C"
#endif
