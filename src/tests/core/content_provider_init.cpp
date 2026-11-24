// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "tests/core/system_test_context.h"

TEST_CASE("A fresh System has an empty content provider", "[core][content_provider]") {
    ScopedSystemTestContext environment;
    Core::System system;
    constexpr u64 ApplicationId = 0x0100123456780000;
    CHECK_FALSE(system.GetContentProvider().HasEntry(ApplicationId,
                                                     FileSys::ContentRecordType::Program));
    CHECK_FALSE(system.GetContentProviderUnion().HasEntry(ApplicationId,
                                                          FileSys::ContentRecordType::Program));
}

TEST_CASE("A manual content provider swaps its entries with another", "[core][content_provider]") {
    constexpr u64 ApplicationId = 0x0100123456780000;
    FileSys::ManualContentProvider kept;
    FileSys::ManualContentProvider scanned;
    kept.AddEntryWithVersion(FileSys::TitleType::Update, FileSys::ContentRecordType::Program,
                             ApplicationId, 131072, "2.0.0",
                             std::make_shared<FileSys::VectorVfsFile>(std::vector<u8>{1}, "u.nca"));
    kept.SwapEntries(scanned);
    CHECK_FALSE(kept.HasEntry(ApplicationId, FileSys::ContentRecordType::Program));
    CHECK(kept.ListUpdateVersions(ApplicationId).empty());
    CHECK(scanned.HasEntry(ApplicationId, FileSys::ContentRecordType::Program));
    CHECK(scanned.ListUpdateVersions(ApplicationId).size() == 1);
    kept.SwapEntries(scanned);
    CHECK(kept.HasEntry(ApplicationId, FileSys::ContentRecordType::Program));
    CHECK(kept.ListUpdateVersions(ApplicationId).size() == 1);
    CHECK_FALSE(scanned.HasEntry(ApplicationId, FileSys::ContentRecordType::Program));
    CHECK(scanned.ListUpdateVersions(ApplicationId).empty());
}
