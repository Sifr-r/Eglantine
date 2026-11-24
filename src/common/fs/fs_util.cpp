// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "common/fs/fs_util.h"
#include <ranges>

namespace Common::FS {

namespace {
std::size_t BufferStringLength(std::span<const u8> buffer) {
    return static_cast<std::size_t>(std::ranges::find(buffer, u8{0}) - buffer.begin());
}
} // namespace

std::u8string ToU8String(std::string_view utf8_string) {
    return std::u8string{utf8_string.begin(), utf8_string.end()};
}

std::u8string BufferToU8String(std::span<const u8> buffer) {
    return std::u8string{buffer.begin(), std::ranges::find(buffer, u8{0})};
}

std::u8string_view BufferToU8StringView(std::span<const u8> buffer) {
    if (buffer.empty()) {
        return {};
    }
    return std::u8string_view{reinterpret_cast<const char8_t*>(buffer.data()),
                            BufferStringLength(buffer)};
}

std::string ToUTF8String(std::u8string_view u8_string) {
    return std::string{u8_string.begin(), u8_string.end()};
}

std::string BufferToUTF8String(std::span<const u8> buffer) {
    return std::string{buffer.begin(), std::ranges::find(buffer, u8{0})};
}

std::string_view BufferToUTF8StringView(std::span<const u8> buffer) {
    if (buffer.empty()) {
        return {};
    }
    return std::string_view{reinterpret_cast<const char*>(buffer.data()),
                            BufferStringLength(buffer)};
}

std::string PathToUTF8String(const std::filesystem::path& path) {
    return ToUTF8String(path.u8string());
}

} // namespace Common::FS
