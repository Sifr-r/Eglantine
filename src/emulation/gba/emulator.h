// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "emulation/gba/bus.h"
#include "emulation/gba/cheats.h"
#include "emulation/gba/cpu.h"
#include "emulation/gba/rewind.h"
#include "emulation/gba/types.h"

namespace Emulation::Gba {

class GbaEmulator {
public:
    [[nodiscard]] static std::unique_ptr<GbaEmulator> Create(
        std::vector<std::uint8_t> rom, std::span<const std::uint8_t> save,
        std::span<const std::uint8_t> bios, std::string& error);

    void Reset();
    void RunFrame(std::uint32_t keys);

    [[nodiscard]] std::span<const std::uint32_t> Video() const {
        return bus_.ppu.output_frame;
    }

    [[nodiscard]] std::span<const std::int16_t> Audio() const {
        return bus_.apu.samples;
    }

    [[nodiscard]] std::size_t Width() const {
        return ScreenWidth;
    }

    [[nodiscard]] std::size_t Height() const {
        return ScreenHeight;
    }

    [[nodiscard]] double FrameRate() const {
        return FrameRateHz;
    }

    [[nodiscard]] std::vector<std::uint8_t> SaveData() const {
        return bus_.cartridge.SaveBytes();
    }

    [[nodiscard]] bool SaveState(std::vector<std::uint8_t>& out, std::string& error) const;
    [[nodiscard]] bool LoadState(std::span<const std::uint8_t> data, std::string& error);
    [[nodiscard]] bool RewindFrame(std::string& error);

    void SetVideoOptions(VideoOptions options);
    [[nodiscard]] VideoOptions GetVideoOptions() const {
        return bus_.ppu.video_options;
    }

    [[nodiscard]] bool AddCheat(std::string_view code, std::string& error) {
        return cheats_.AddCode(code, error);
    }

    [[nodiscard]] bool ReplaceCheats(std::span<const std::string> codes, std::string& error) {
        return cheats_.ReplaceCodes(codes, error);
    }

    void ClearCheats() {
        cheats_.Clear();
    }

    [[nodiscard]] std::size_t CheatCount() const {
        return cheats_.Size();
    }

    [[nodiscard]] Bus& GetBus() {
        return bus_;
    }

    [[nodiscard]] const Cpu& GetCpu() const {
        return cpu_;
    }

private:
    GbaEmulator(Cpu cpu, Bus bus);
    void ApplyCheats();

    Cpu cpu_;
    Bus bus_;
    CheatEngine cheats_;
    RewindBuffer rewind_;
    std::uint32_t excess_cycles_ = 0;
};

} // namespace Emulation::Gba
