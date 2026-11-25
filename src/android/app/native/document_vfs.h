// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace Eglantine::AndroidPrototype {

struct DocumentVfs {
    FileSys::VirtualFilesystem filesystem;
    std::string path;
    std::string error;
};

struct DiscoveredGame {
    std::string uri;
    std::string title;
    u64 program_id{};
};

struct FolderScanResult {
    std::vector<DiscoveredGame> games;
    std::string error;
};

[[nodiscard]] DocumentVfs OpenDocumentVfs(std::string_view uri);
[[nodiscard]] std::string QueryDocumentDisplayName(std::string_view uri);
bool TakePersistableReadPermission(std::string_view tree_uri);
[[nodiscard]] FolderScanResult ScanDocumentTree(std::string_view tree_uri);

} // namespace Eglantine::AndroidPrototype
