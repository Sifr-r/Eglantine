// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include "emulation/classic/battery_save.h"

namespace Emulation::Classic {
namespace {

constexpr std::size_t MaxBatteryBytes = 1024 * 1024;
std::atomic_uint64_t save_sequence{};

std::FILE* OpenExclusive(const std::filesystem::path& path) {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"wbx");
#else
    const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return nullptr;
    }
    auto* stream = fdopen(descriptor, "wb");
    if (!stream) {
        const int reason = errno;
        close(descriptor);
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        errno = reason;
    }
    return stream;
#endif
}

bool FlushFile(std::FILE* stream) {
    if (std::fflush(stream) != 0) {
        return false;
    }
#ifdef _WIN32
    return _commit(_fileno(stream)) == 0;
#else
    return fsync(fileno(stream)) == 0;
#endif
}

} // namespace

bool ReadBatterySave(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes,
                     std::string& error) {
    bytes.clear();
    error.clear();
    std::error_code status;
    const auto size = std::filesystem::file_size(path, status);
    if (status == std::errc::no_such_file_or_directory) {
        return true;
    }
    if (status || size > MaxBatteryBytes) {
        error = "The battery save could not be read or exceeds 1 MiB";
        return false;
    }
    std::ifstream stream{path, std::ios::binary};
    bytes.resize(static_cast<std::size_t>(size));
    if (!stream || (size != 0 && !stream.read(reinterpret_cast<char*>(bytes.data()),
                                             static_cast<std::streamsize>(bytes.size())))
        || stream.peek() != std::char_traits<char>::eof() || stream.bad()) {
        bytes.clear();
        error = "The battery save changed or could not be read";
        return false;
    }
    return true;
}

bool WriteAtomicFile(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                     std::size_t max_bytes, std::string& error) {
    error.clear();
    if (bytes.empty()) {
        return true;
    }
    if (bytes.size() > max_bytes || path.parent_path().empty()) {
        error = "The file path or size is invalid";
        return false;
    }
    std::error_code status;
    std::filesystem::create_directories(path.parent_path(), status);
    if (status) {
        error = "The destination folder could not be created: " + status.message();
        return false;
    }
#ifdef _WIN32
    const auto process = _getpid();
#else
    const auto process = getpid();
#endif
    std::filesystem::path temporary;
    std::FILE* stream{};
    for (int attempt = 0; attempt < 16; ++attempt) {
        temporary = path;
        temporary += ".tmp." + std::to_string(process) + "." + std::to_string(save_sequence++);
        stream = OpenExclusive(temporary);
        if (stream || errno != EEXIST) {
            break;
        }
    }
    if (!stream) {
        error = "A temporary file could not be created";
        return false;
    }
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), stream) == bytes.size();
    const bool synced = written && FlushFile(stream);
    const bool closed = std::fclose(stream) == 0;
    if (synced && closed) {
        std::filesystem::rename(temporary, path, status);
        if (!status) {
            return true;
        }
        error = "The destination file could not be replaced: " + status.message();
    } else {
        error = "The temporary file could not be written";
    }
    std::filesystem::remove(temporary, status);
    return false;
}

bool WriteBatterySave(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                      std::string& error) {
    return WriteAtomicFile(path, bytes, MaxBatteryBytes, error);
}

} // namespace Emulation::Classic
