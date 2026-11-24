// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/textures/astc.h"

namespace {

using Block = std::array<uint8_t, 16>;

constexpr Block VoidExtent(bool hdr, uint16_t alpha) {
    Block block{0xfc, static_cast<uint8_t>(hdr ? 0xff : 0xfd), 0xff, 0xff,
                0xff, 0xff, 0xff, 0xff};
    block[14] = static_cast<uint8_t>(alpha);
    block[15] = static_cast<uint8_t>(alpha >> 8);
    return block;
}

constexpr Block EndpointBlock(uint8_t mode, bool two_partitions) {
    // A 4x4, 4-level weight grid. Two partitions use partition index 0 and a shared CEM.
    const uint32_t header = 0x42 | (two_partitions ? (1U << 11) | (uint32_t{mode} << 25)
                                                 : uint32_t{mode} << 13);
    Block block{};
    for (size_t i = 0; i < 4; ++i) {
        block[i] = static_cast<uint8_t>(header >> (8 * i));
    }
    return block;
}

} // namespace

TEST_CASE("ASTC opacity uses the alpha endpoint and constant alpha encodings",
          "[video_core][textures][astc]") {
    using Tegra::Texture::ASTC::IsOpaque;

    CHECK(IsOpaque(VoidExtent(false, 0xffff)));
    CHECK_FALSE(IsOpaque(VoidExtent(false, 0xfffe)));
    CHECK(IsOpaque(VoidExtent(true, 0x3c00)));
    CHECK_FALSE(IsOpaque(VoidExtent(true, 0x3800)));
    CHECK(IsOpaque(EndpointBlock(8, false)));
    CHECK_FALSE(IsOpaque(EndpointBlock(12, false)));
    CHECK(IsOpaque(EndpointBlock(8, true)));
    CHECK_FALSE(IsOpaque(EndpointBlock(4, true)));

    constexpr Block Opaque = EndpointBlock(8, false);
    constexpr Block Transparent = VoidExtent(false, 0);
    std::vector<uint8_t> storage(1 + 33 * Opaque.size() + Transparent.size() - 1);
    const auto blocks = std::span{storage}.subspan(1);
    for (size_t i = 0; i < 33; ++i) {
        std::ranges::copy(Opaque, blocks.subspan(i * Opaque.size()).begin());
    }
    std::ranges::copy(std::span{Transparent}.first(Transparent.size() - 1),
                      blocks.last(Transparent.size() - 1).begin());
    CHECK(IsOpaque(blocks));
    std::ranges::copy(Transparent, blocks.subspan(32 * Opaque.size()).begin());
    CHECK_FALSE(IsOpaque(blocks));
}

TEST_CASE("ASTC opacity prescan throughput", "[.astc_bench]") {
    using Tegra::Texture::ASTC::IsIspcEnabled;
    using Tegra::Texture::ASTC::IsOpaque;

    constexpr size_t MiB = 1024 * 1024;
    constexpr std::array OpaqueBlocks{EndpointBlock(8, false), EndpointBlock(8, true),
                                      VoidExtent(false, 0xffff), VoidExtent(true, 0x3c00)};
    constexpr size_t BlockSize = OpaqueBlocks.front().size();

    for (const size_t block_count : {size_t{4096}, size_t{65536}}) {
        std::vector<uint8_t> data(block_count * BlockSize);
        for (size_t i = 0; i < block_count; ++i) {
            std::ranges::copy(OpaqueBlocks[i % OpaqueBlocks.size()], data.begin() + i * BlockSize);
        }
        REQUIRE(IsOpaque(data));

        const size_t iterations = 256 * MiB / data.size();
        size_t opaque_count = 0;
        const auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < iterations; ++i) {
            opaque_count += IsOpaque(data);
        }
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
        const double mib_per_second = static_cast<double>(data.size() * iterations) / MiB /
                                      elapsed.count();
        std::printf("astc_bench backend=%s blocks=%zu seconds=%.6f MiB_s=%.2f\n",
                    IsIspcEnabled() ? "ispc" : "cpp", block_count, elapsed.count(), mib_per_second);
        REQUIRE(opaque_count == iterations);
    }
}
