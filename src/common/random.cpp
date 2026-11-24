// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <mutex>
#include <random>
#include "common/random.h"

namespace Common::Random {
namespace {
std::mutex& GetRandomMutex() noexcept {
    static std::mutex mutex;
    return mutex;
}

std::random_device& GetRandomDevice() noexcept {
    static std::random_device device;
    return device;
}
} // namespace

[[nodiscard]] u32 Random32(u32 seed) noexcept {
    if (seed != 0) {
        return std::mt19937{seed}();
    }
    std::scoped_lock lock{GetRandomMutex()};
    return static_cast<u32>(GetRandomDevice()());
}

[[nodiscard]] u64 Random64(u64 seed) noexcept {
    if (seed != 0) {
        return std::mt19937_64{seed}();
    }
    std::scoped_lock lock{GetRandomMutex()};
    auto& device = GetRandomDevice();
    const u64 high = static_cast<u32>(device());
    const u64 low = static_cast<u32>(device());
    return (high << 32) | low;
}

[[nodiscard]] std::mt19937 GetMT19937() noexcept {
    return std::mt19937(Random32(0));
}
} // namespace Common::Random
