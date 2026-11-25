// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <string_view>

#include "common/common_types.h"
#include "common/fs/fs_android.h"

namespace Common::FS::Android {
namespace {

constexpr std::string_view ContentPrefix = "content://";

std::size_t FindLastSeparator(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    const std::size_t enc_upper = path.rfind("%2F");
    const std::size_t enc_lower = path.rfind("%2f");
    std::size_t best = std::string_view::npos;
    for (const auto candidate : {slash, enc_upper, enc_lower}) {
        if (candidate != std::string_view::npos &&
            (best == std::string_view::npos || candidate > best)) {
            best = candidate;
        }
    }
    return best;
}

} // namespace

bool IsContentUri(const std::string& path) {
    return path.starts_with(ContentPrefix);
}

s32 OpenContentUri(const std::string&, OpenMode) {
    return -1;
}

u64 GetSize(const std::string&) {
    return 0;
}

bool IsDirectory(const std::string&) {
    return false;
}

bool Exists(const std::string&) {
    return false;
}

std::string GetParentDirectory(const std::string& filepath) {
    const std::size_t pos = filepath.rfind('/');
    if (pos == std::string::npos || pos <= ContentPrefix.size()) {
        return {};
    }
    return filepath.substr(0, pos);
}

std::string GetFilename(const std::string& filepath) {
    const std::size_t pos = FindLastSeparator(filepath);
    if (pos == std::string::npos) {
        return filepath;
    }
    const std::size_t offset = filepath[pos] == '/' ? 1 : 3;
    return filepath.substr(std::min(pos + offset, filepath.size()));
}

} // namespace Common::FS::Android
