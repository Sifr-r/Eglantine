// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>

namespace Common::FS::Android {

enum class OpenMode {
    Read,
    Write,
    ReadWrite,
    WriteAppend,
    WriteTruncate,
    ReadWriteAppend,
    ReadWriteTruncate,
    Never
};

bool IsContentUri(const std::string& path);
int OpenContentUri(const std::string& filepath, OpenMode openmode);
std::uint64_t GetSize(const std::string& filepath);
bool IsDirectory(const std::string& filepath);
bool Exists(const std::string& filepath);
std::string GetParentDirectory(const std::string& filepath);
std::string GetFilename(const std::string& filepath);

} // namespace Common::FS::Android
