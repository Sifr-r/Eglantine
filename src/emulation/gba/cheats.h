// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Emulation::Gba {

struct CheatEntry {
    enum class Kind : std::uint8_t {
        Raw16,
        Raw32,
        CodeBreakerWrite8,
        CodeBreakerWrite16,
        CodeBreakerAdd16,
        CodeBreakerIfEqual16,
        GameSharkWrite8,
        GameSharkWrite16,
        GameSharkWrite32,
        GameSharkIfEqual16,
    };

    Kind kind;
    std::uint32_t addr;
    std::uint32_t val;
};

class CheatEngine {
public:
    void Clear() {
        codes_.clear();
    }

    [[nodiscard]] std::size_t Size() const {
        return codes_.size();
    }

    [[nodiscard]] bool Empty() const {
        return codes_.empty();
    }

    [[nodiscard]] bool AddCode(std::string_view raw, std::string& error);
    [[nodiscard]] bool ReplaceCodes(std::span<const std::string> codes, std::string& error);

    [[nodiscard]] const std::vector<CheatEntry>& Codes() const {
        return codes_;
    }

private:
    std::vector<CheatEntry> codes_;
};

} // namespace Emulation::Gba
