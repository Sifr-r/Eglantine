// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Emulation::Gba {

class Cpu;
class Bus;

[[nodiscard]] bool SaveState(const Cpu& cpu, const Bus& bus, std::vector<std::uint8_t>& out,
                             std::string& error);
[[nodiscard]] bool LoadState(std::span<const std::uint8_t> data, Cpu& cpu, Bus& bus,
                             std::string& error);

} // namespace Emulation::Gba
