// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "emulation/gba/rewind.h"

#include <algorithm>

namespace Emulation::Gba {
namespace {

constexpr std::size_t MaxRewindFrames = 600;
constexpr std::size_t MaxRewindMemoryBytes = 32 * 1024 * 1024;
constexpr std::uint64_t RecordIntervalFrames = 1;

std::vector<std::uint8_t> RleCompress(std::span<const std::uint8_t> input) {
    std::vector<std::uint8_t> out;
    out.reserve(input.size() / 4);
    std::size_t i = 0;
    while (i < input.size()) {
        const std::uint8_t b = input[i];
        std::size_t run = 1;
        while (i + run < input.size() && input[i + run] == b && run < 128) {
            ++run;
        }
        if (run >= 3) {
            out.push_back(static_cast<std::uint8_t>(0x80u | (run - 1u)));
            out.push_back(b);
            i += run;
        } else {
            const std::size_t start = i;
            std::size_t lit = 0;
            while (i < input.size() && lit < 128) {
                const std::uint8_t cur = input[i];
                const bool same3 =
                    (i + 2 < input.size()) && input[i + 1] == cur && input[i + 2] == cur;
                if (same3 && lit > 0) {
                    break;
                }
                ++i;
                ++lit;
                if (same3) {
                    break;
                }
            }
            out.push_back(static_cast<std::uint8_t>(lit - 1u));
            out.insert(out.end(), input.begin() + static_cast<std::ptrdiff_t>(start),
                       input.begin() + static_cast<std::ptrdiff_t>(start + lit));
        }
    }
    return out;
}

bool RleDecompress(std::span<const std::uint8_t> compressed, std::vector<std::uint8_t>& out,
                   std::string& error) {
    out.clear();
    out.reserve(512 * 1024);
    std::size_t i = 0;
    while (i < compressed.size()) {
        const std::uint8_t tag = compressed[i++];
        const std::size_t count = static_cast<std::size_t>(tag & 0x7Fu) + 1u;
        if ((tag & 0x80u) != 0) {
            if (i >= compressed.size()) {
                error = "Invalid GBA save state: Truncated RLE rewind stream";
                return false;
            }
            const std::uint8_t val = compressed[i++];
            out.insert(out.end(), count, val);
        } else {
            if (i + count > compressed.size()) {
                error = "Invalid GBA save state: Truncated RLE literal run";
                return false;
            }
            out.insert(out.end(), compressed.begin() + static_cast<std::ptrdiff_t>(i),
                       compressed.begin() + static_cast<std::ptrdiff_t>(i + count));
            i += count;
        }
    }
    return true;
}

} // namespace

void RewindBuffer::Clear() {
    snapshots_.clear();
    total_bytes_ = 0;
    frame_counter_ = 0;
}

void RewindBuffer::MaybePush(std::span<const std::uint8_t> raw_state) {
    ++frame_counter_;
    if ((frame_counter_ % RecordIntervalFrames) != 0 && !snapshots_.empty()) {
        return;
    }
    auto compressed = RleCompress(raw_state);
    total_bytes_ += compressed.size();
    snapshots_.push_back(std::move(compressed));
    while (snapshots_.size() > MaxRewindFrames || total_bytes_ > MaxRewindMemoryBytes) {
        if (snapshots_.empty()) {
            break;
        }
        total_bytes_ -= (std::min)(total_bytes_, snapshots_.front().size());
        snapshots_.pop_front();
    }
}

bool RewindBuffer::Pop(std::vector<std::uint8_t>& out, std::string& error) {
    if (snapshots_.empty()) {
        error = "No rewind state available";
        return false;
    }
    auto compressed = std::move(snapshots_.back());
    snapshots_.pop_back();
    total_bytes_ -= (std::min)(total_bytes_, compressed.size());
    return RleDecompress(compressed, out, error);
}

} // namespace Emulation::Gba
