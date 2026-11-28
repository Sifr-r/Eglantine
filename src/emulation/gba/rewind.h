// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <vector>

namespace Emulation::Gba {

class RewindBuffer {
public:
    void Clear();
    [[nodiscard]] std::size_t Size() const {
        return snapshots_.size();
    }
    [[nodiscard]] bool Empty() const {
        return snapshots_.empty();
    }

    void MaybePush(std::span<const std::uint8_t> raw_state);
    [[nodiscard]] bool Pop(std::vector<std::uint8_t>& out, std::string& error);

private:
    std::deque<std::vector<std::uint8_t>> snapshots_;
    std::size_t total_bytes_ = 0;
    std::uint64_t frame_counter_ = 0;
};

} // namespace Emulation::Gba
