// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "symlink.h"

#ifdef _WIN32
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>
#include <winioctl.h>
#endif

#include <boost/filesystem.hpp>

namespace fs = std::filesystem;

// The sole purpose of this file is to treat symlinks like symlinks on POSIX,
// or treat them as directory junctions on Windows.
// This is because, for some inexplicable reason, Microsoft has locked symbolic
// links behind a "security policy", whereas directory junctions--functionally identical
// for directories, by the way--are not. Why? I don't know.
// And no, they do NOT provide a standard API for this (at least to my knowledge).
// CreateSymbolicLink, even when EXPLICITLY TOLD to create a junction, still fails
// because of their security policy.
// I don't know what kind of drugs the Windows developers have been on since NT started.

// Microsoft still has not implemented any of this in their std::filesystem implemenation,
// which ALSO means that it DOES NOT FOLLOW ANY DIRECTORY JUNCTIONS... AT ALL.
// Nor does any of their command line utilities or APIs. So you're quite literally
// on your own.

namespace Common::FS {

#ifdef _WIN32
namespace {

struct MountPointReparseHeader {
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    USHORT SubstituteNameOffset;
    USHORT SubstituteNameLength;
    USHORT PrintNameOffset;
    USHORT PrintNameLength;
};
static_assert(sizeof(MountPointReparseHeader) == 16);

bool CreateDirectoryJunction(const fs::path& target, const fs::path& link) {
    const DWORD full_len = GetFullPathNameW(target.c_str(), 0, nullptr, nullptr);
    if (full_len == 0) {
        return false;
    }

    std::wstring full_target(full_len, L'\0');
    const DWORD copied = GetFullPathNameW(target.c_str(), full_len, full_target.data(), nullptr);
    if (copied == 0 || copied >= full_len) {
        return false;
    }
    full_target.resize(copied);

    std::wstring substitute_name;
    std::wstring print_name = full_target;
    if (full_target.starts_with(L"\\\\?\\UNC\\")) {
        substitute_name = L"\\??\\UNC\\" + full_target.substr(8);
        print_name = L"\\\\" + full_target.substr(8);
    } else if (full_target.starts_with(L"\\\\?\\")) {
        substitute_name = L"\\??\\" + full_target.substr(4);
        print_name = full_target.substr(4);
    } else if (full_target.starts_with(L"\\\\")) {
        substitute_name = L"\\??\\UNC\\" + full_target.substr(2);
    } else {
        substitute_name = L"\\??\\" + full_target;
    }

    const std::size_t substitute_bytes = substitute_name.size() * sizeof(wchar_t);
    const std::size_t print_bytes = print_name.size() * sizeof(wchar_t);
    const std::size_t total_bytes =
        sizeof(MountPointReparseHeader) + substitute_bytes + sizeof(wchar_t) + print_bytes +
        sizeof(wchar_t);
    if (total_bytes > MAXIMUM_REPARSE_DATA_BUFFER_SIZE) {
        return false;
    }

    if (CreateDirectoryW(link.c_str(), nullptr) == FALSE) {
        return false;
    }

    HANDLE dir_handle = CreateFileW(
        link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (dir_handle == INVALID_HANDLE_VALUE) {
        RemoveDirectoryW(link.c_str());
        return false;
    }

    std::vector<BYTE> buffer(total_bytes, 0);
    MountPointReparseHeader header{};
    header.ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    header.ReparseDataLength =
        static_cast<USHORT>(total_bytes - offsetof(MountPointReparseHeader, SubstituteNameOffset));
    header.Reserved = 0;
    header.SubstituteNameOffset = 0;
    header.SubstituteNameLength = static_cast<USHORT>(substitute_bytes);
    header.PrintNameOffset = static_cast<USHORT>(substitute_bytes + sizeof(wchar_t));
    header.PrintNameLength = static_cast<USHORT>(print_bytes);

    std::memcpy(buffer.data(), &header, sizeof(header));
    std::size_t offset = sizeof(header);
    std::memcpy(buffer.data() + offset, substitute_name.data(), substitute_bytes);
    offset += substitute_bytes + sizeof(wchar_t);
    std::memcpy(buffer.data() + offset, print_name.data(), print_bytes);

    DWORD bytes_returned = 0;
    const BOOL ok = DeviceIoControl(dir_handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
                                    static_cast<DWORD>(buffer.size()), nullptr, 0,
                                    &bytes_returned, nullptr);
    CloseHandle(dir_handle);

    if (ok == FALSE) {
        RemoveDirectoryW(link.c_str());
        return false;
    }

    return true;
}

} // namespace
#endif

bool CreateSymlink(fs::path from, fs::path to)
{
    from.make_preferred();
    to.make_preferred();

    std::error_code ec;
    fs::create_directory_symlink(from, to, ec);
#ifdef _WIN32
    if (ec) {
        return CreateDirectoryJunction(from, to);
    }
#endif

    return !ec;
}

bool IsSymlink(const fs::path &path)
{
    return boost::filesystem::is_symlink(boost::filesystem::path{path});
}

} // namespace Common::FS
