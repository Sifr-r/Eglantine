// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Emulation::Classic {

class Runtime {
public:
    static constexpr unsigned AudioRate = 48'000;

    [[nodiscard]] static std::unique_ptr<Runtime> Open(
        std::vector<std::uint8_t> rom, std::span<const std::uint8_t> save, std::string& error,
        std::span<const std::uint8_t> bios = {});
    ~Runtime();

    void RunFrame(std::uint32_t keys);
    void Reset();
    [[nodiscard]] bool SaveState(std::vector<std::uint8_t>& out, std::string& error) const;
    [[nodiscard]] bool LoadState(std::span<const std::uint8_t> data, std::string& error);
    [[nodiscard]] bool RewindFrame(std::string& error);
    void SetVideoOptions(bool color_correction, bool interframe_blending);
    [[nodiscard]] bool AddCheat(std::string_view code, std::string& error);
    [[nodiscard]] bool SetCheats(std::span<const std::string> codes, std::string& error);
    void ClearCheats();
    [[nodiscard]] bool SupportsGbaFeatures() const;

    [[nodiscard]] std::span<const std::uint32_t> Video() const; // Packed 0xFFBBGGRR pixels.
    [[nodiscard]] std::span<const std::int16_t> Audio() const;  // Interleaved stereo at AudioRate.
    [[nodiscard]] unsigned Width() const;
    [[nodiscard]] unsigned Height() const;
    [[nodiscard]] double FrameRate() const;
    [[nodiscard]] std::vector<std::uint8_t> Save() const;
    [[nodiscard]] std::vector<std::uint8_t> Finish(); // Close the core and collect its final save.

private:
    struct Impl;
    Runtime();

    std::unique_ptr<Impl> impl;
};

} // namespace Emulation::Classic
