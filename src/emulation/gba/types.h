// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

namespace Emulation::Gba {

inline constexpr std::uint32_t CpuClockHz = 16'777'216;
inline constexpr std::size_t ScreenWidth = 240;
inline constexpr std::size_t ScreenHeight = 160;
inline constexpr std::uint32_t CyclesPerScanline = 1232;
inline constexpr std::uint32_t HdrawCycles = 960;
inline constexpr std::uint32_t HblankCycles = 272;
inline constexpr std::uint16_t VisibleScanlines = 160;
inline constexpr std::uint16_t ScanlinesPerFrame = 228;
inline constexpr std::uint32_t CyclesPerFrame =
    CyclesPerScanline * static_cast<std::uint32_t>(ScanlinesPerFrame);
inline constexpr double FrameRateHz =
    static_cast<double>(CpuClockHz) / static_cast<double>(CyclesPerFrame);
inline constexpr std::uint32_t AudioRateHz = 48'000;

inline constexpr std::size_t MaxRomBytes = 32 * 1024 * 1024;
inline constexpr std::size_t MaxSaveBytes = 256 * 1024;
inline constexpr std::size_t BiosBytes = 16 * 1024;

inline constexpr std::uint32_t KeyA = 1u << 0;
inline constexpr std::uint32_t KeyB = 1u << 1;
inline constexpr std::uint32_t KeySelect = 1u << 2;
inline constexpr std::uint32_t KeyStart = 1u << 3;
inline constexpr std::uint32_t KeyRight = 1u << 4;
inline constexpr std::uint32_t KeyLeft = 1u << 5;
inline constexpr std::uint32_t KeyUp = 1u << 6;
inline constexpr std::uint32_t KeyDown = 1u << 7;
inline constexpr std::uint32_t KeyR = 1u << 8;
inline constexpr std::uint32_t KeyL = 1u << 9;
inline constexpr std::uint32_t KeyMask = 0x3FFu;

inline constexpr std::uint16_t IrqVBlank = 1u << 0;
inline constexpr std::uint16_t IrqHBlank = 1u << 1;
inline constexpr std::uint16_t IrqVCount = 1u << 2;
inline constexpr std::uint16_t IrqTimer0 = 1u << 3;
inline constexpr std::uint16_t IrqTimer1 = 1u << 4;
inline constexpr std::uint16_t IrqTimer2 = 1u << 5;
inline constexpr std::uint16_t IrqTimer3 = 1u << 6;
inline constexpr std::uint16_t IrqSerial = 1u << 7;
inline constexpr std::uint16_t IrqDma0 = 1u << 8;
inline constexpr std::uint16_t IrqDma1 = 1u << 9;
inline constexpr std::uint16_t IrqDma2 = 1u << 10;
inline constexpr std::uint16_t IrqDma3 = 1u << 11;
inline constexpr std::uint16_t IrqKeypad = 1u << 12;
inline constexpr std::uint16_t IrqGamePak = 1u << 13;

struct VideoOptions {
    bool color_correction = false;
    bool interframe_blending = false;

    bool operator==(const VideoOptions&) const = default;
};

enum class SaveType : std::uint8_t {
    None = 0,
    Sram = 1,
    Eeprom4K = 2,
    Eeprom64K = 3,
    Flash64K = 4,
    Flash128K = 5,
};

[[nodiscard]] inline bool SaveTypeFromU8(std::uint8_t v, SaveType& out) {
    if (v <= static_cast<std::uint8_t>(SaveType::Flash128K)) {
        out = static_cast<SaveType>(v);
        return true;
    }
    return false;
}

} // namespace Emulation::Gba
