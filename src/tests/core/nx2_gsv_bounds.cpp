// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_gsv.h"

TEST_CASE("NX2 image pool counts stay inside the pool's byte budget", "[nx2_gsv]") {
    // A pool of 4 MiB images: the count that fills the budget is the budget over the image size.
    constexpr u64 image_size = 0x400000;
    constexpr u32 limit = static_cast<u32>(Service::Nx2MaxImagePoolBytes / image_size);
    REQUIRE(limit < Service::Nx2MaxImageCount);
    CHECK(Service::Nx2ImagePoolCountLimit(image_size) == limit);

    // A count that fits is accepted, one past it is not.
    CHECK(Service::Nx2ImagePoolCountValid(limit, image_size));
    CHECK_FALSE(Service::Nx2ImagePoolCountValid(limit + 1, image_size));

    // A guest that asks for 0xffffffff images is rejected instead of allocating.
    CHECK_FALSE(Service::Nx2ImagePoolCountValid(std::numeric_limits<u32>::max(), image_size));
    CHECK_FALSE(Service::Nx2ImagePoolCountValid(std::numeric_limits<u32>::max(), 0x10000000));
}

TEST_CASE("NX2 image pool counts are capped whatever the image size", "[nx2_gsv]") {
    // Tiny images: the byte budget would allow more images than the handle space does.
    CHECK(Service::Nx2ImagePoolCountLimit(1) == Service::Nx2MaxImageCount);
    CHECK(Service::Nx2ImagePoolCountLimit(0) == Service::Nx2MaxImageCount);
    CHECK(Service::Nx2ImagePoolCountValid(Service::Nx2MaxImageCount, 1));
    CHECK_FALSE(Service::Nx2ImagePoolCountValid(Service::Nx2MaxImageCount + 1, 1));

    // Huge images: a single one already fills the budget.
    CHECK(Service::Nx2ImagePoolCountLimit(Service::Nx2MaxImagePoolBytes) == 1);
    CHECK(Service::Nx2ImagePoolCountValid(1, Service::Nx2MaxImagePoolBytes));
    CHECK_FALSE(Service::Nx2ImagePoolCountValid(2, Service::Nx2MaxImagePoolBytes));
}

TEST_CASE("NX2 GPU memory ranges must not run backwards", "[nx2_gsv]") {
    constexpr u64 space_start = 0;
    constexpr u64 space_end = 0x8000000000ULL; // a 39-bit address space

    CHECK(Service::Nx2GpuMemoryRangeValid(0x8000000, 0x8000000 + 0xfff, space_start, space_end));
    CHECK(Service::Nx2GpuMemoryRangeValid(0x8000000, 0x8000000, space_start, space_end));

    // last < start: the size (last - start + 1) would underflow.
    CHECK_FALSE(Service::Nx2GpuMemoryRangeValid(0x8001000, 0x8000000, space_start, space_end));
    CHECK_FALSE(Service::Nx2GpuMemoryRangeValid(1, 0, space_start, space_end));
    CHECK_FALSE(Service::Nx2GpuMemoryRangeValid(std::numeric_limits<u64>::max(), 0, space_start,
                                                 space_end));
}

TEST_CASE("NX2 GPU memory ranges must lie inside the address space", "[nx2_gsv]") {
    constexpr u64 space_start = 0x100000;
    constexpr u64 space_end = 0x100000 + 0x10000000;

    CHECK(Service::Nx2GpuMemoryRangeValid(space_start, space_end - 1, space_start, space_end));
    // The last byte of the space is inside it, the first one past it is not.
    CHECK_FALSE(Service::Nx2GpuMemoryRangeValid(space_end, space_end + 0xfff, space_start,
                                                space_end));
    CHECK_FALSE(Service::Nx2GpuMemoryRangeValid(space_start - 0x1000, space_start, space_start,
                                                space_end));
    // A range that starts inside and ends outside is not accepted either.
    CHECK_FALSE(Service::Nx2GpuMemoryRangeValid(space_end - 0x1000, space_end + 0x1000,
                                                space_start, space_end));
}

TEST_CASE("NX2 GPU-only regions stay within the GPU aperture", "[nx2_gsv]") {
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(0x8000000, 0x8000fff) == ResultSuccess);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(0x8100000000, 0x8100000000 + (300ULL << 20) - 1) ==
          ResultSuccess);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(0xbffffff000, 0xbfffffffff) == ResultSuccess);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(0x8001000, 0x8000fff) ==
          Kernel::ResultInvalidAddress);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(0x7fff000, 0x7ffffff) ==
          Kernel::ResultInvalidAddress);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(0xbffffff000, 0xc000000fff) ==
          Kernel::ResultInvalidAddress);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(0x1000, 0xfffffffffffff00f) ==
          Kernel::ResultInvalidAddress);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(0x1000, 0xffffffffffffefff) ==
          Kernel::ResultInvalidAddress);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(0x1000, 0xfffffffeffffffff) ==
          Kernel::ResultInvalidAddress);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(0x8000001, 0x8001000) ==
          Kernel::ResultInvalidAddress);
}

TEST_CASE("NX2 GPU-only regions have a bounded page-aligned size", "[nx2_gsv]") {
    constexpr u64 first = 0x8100000000;
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(first, first + 0x16ad80000 - 1) ==
          ResultSuccess);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(first, first + (8ULL << 30) - 1) ==
          ResultSuccess);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(first, first + (8ULL << 30) + 0xfff) ==
          Kernel::ResultInvalidSize);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(first, first) == Kernel::ResultInvalidSize);
    CHECK(Service::ValidateNx2GpuOnlyRegionRange(first, first + 0x1000) ==
          Kernel::ResultInvalidSize);
}
