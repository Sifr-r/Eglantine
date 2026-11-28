// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Emulation::Classic {

bool ReadBatterySave(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes,
                     std::string& error);
bool WriteBatterySave(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                      std::string& error);
bool WriteAtomicFile(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                     std::size_t max_bytes, std::string& error);

} // namespace Emulation::Classic
