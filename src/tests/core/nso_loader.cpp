// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_funcs.h"
#include "common/lz4_compression.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/loader/nso.h"
#include "tests/core/system_test_context.h"

namespace {

constexpr VAddr TestLoadBase = 0x7100000000ULL;

std::vector<u8> BuildSyntheticNso(bool compress_segments) {
    const std::vector<u8> text_data(0x400, 0x1F);
    const std::vector<u8> rodata_data(0x200, 0x2A);
    const std::vector<u8> data_bytes(0x300, 0x3C);

    const std::array<std::vector<u8>, 3> raw_segments{text_data, rodata_data, data_bytes};
    std::array<std::vector<u8>, 3> stored_segments;

    Loader::NSOHeader header{};
    header.magic = Common::MakeMagic('N', 'S', 'O', '0');

    const std::array<u32, 3> locations{0x0000, 0x1000, 0x2000};
    u32 file_offset = sizeof(Loader::NSOHeader);

    for (size_t i = 0; i < 3; ++i) {
        header.segments[i].offset = file_offset;
        header.segments[i].location = locations[i];
        header.segments[i].size = static_cast<u32>(raw_segments[i].size());
        if (compress_segments) {
            header.flags |= (1U << i);
            stored_segments[i] =
                Common::Compression::CompressDataLZ4(raw_segments[i].data(), raw_segments[i].size());
            REQUIRE(!stored_segments[i].empty());
            header.segments_compressed_size[i] = static_cast<u32>(stored_segments[i].size());
        } else {
            stored_segments[i] = raw_segments[i];
        }
        file_offset += static_cast<u32>(stored_segments[i].size());
    }
    header.segments[2].bss_size = 0x1200; // data_end (0x2300) + bss (0x1200) = 0x3500 -> aligns to 0x4000

    std::vector<u8> image(file_offset);
    std::memcpy(image.data(), &header, sizeof(header));
    for (size_t i = 0; i < 3; ++i) {
        std::memcpy(image.data() + header.segments[i].offset, stored_segments[i].data(),
                    stored_segments[i].size());
    }
    return image;
}

FileSys::VirtualFile MakeNsoFile(std::vector<u8> bytes, std::string name = "main") {
    return std::make_shared<FileSys::VectorVfsFile>(std::move(bytes), std::move(name));
}

} // namespace

TEST_CASE("AppLoader_NSO::IdentifyType recognizes NSO0 magic and rejects truncated or foreign files",
          "[core][nso-loader]") {
    using Loader::AppLoader_NSO;
    using Loader::FileType;

    CHECK(AppLoader_NSO::IdentifyType(MakeNsoFile({})) == FileType::Error);
    CHECK(AppLoader_NSO::IdentifyType(MakeNsoFile({'N', 'S', 'O'})) == FileType::Error);
    CHECK(AppLoader_NSO::IdentifyType(MakeNsoFile({'N', 'R', 'O', '0'})) == FileType::Error);
    CHECK(AppLoader_NSO::IdentifyType(MakeNsoFile({'N', 'S', 'O', '0'})) == FileType::NSO);
}

TEST_CASE("AppLoader_NSO::LoadModule computes page-aligned layout and rejects malformed NSOs",
          "[core][nso-loader]") {
    const ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();
    kernel.Initialize();
    SCOPE_EXIT { system.ShutdownMainProcess(); };

    auto* const process = Kernel::KProcess::Create(kernel);
    REQUIRE(process != nullptr);
    Kernel::KProcess::Register(kernel, process);
    SCOPE_EXIT { process->Close(kernel); };

    SECTION("Uncompressed synthetic NSO resolves aligned end address") {
        const auto file = MakeNsoFile(BuildSyntheticNso(false));
        const auto end = Loader::AppLoader_NSO::LoadModule(*process, system, *file, TestLoadBase,
                                                           false, false);
        REQUIRE(end.has_value());
        CHECK(*end == TestLoadBase + 0x4000);
    }

    SECTION("LZ4-compressed synthetic NSO decompresses and resolves aligned end address") {
        const auto file = MakeNsoFile(BuildSyntheticNso(true));
        const auto end = Loader::AppLoader_NSO::LoadModule(*process, system, *file, TestLoadBase,
                                                           false, false);
        REQUIRE(end.has_value());
        CHECK(*end == TestLoadBase + 0x4000);
    }

    SECTION("Truncated header and truncated segment payload are rejected") {
        std::vector<u8> short_header(sizeof(Loader::NSOHeader) - 1, 0);
        CHECK_FALSE(Loader::AppLoader_NSO::LoadModule(*process, system,
                                                      *MakeNsoFile(short_header), TestLoadBase,
                                                      false, false)
                        .has_value());

        auto truncated = BuildSyntheticNso(false);
        truncated.pop_back();
        CHECK_FALSE(Loader::AppLoader_NSO::LoadModule(*process, system, *MakeNsoFile(truncated),
                                                      TestLoadBase, false, false)
                        .has_value());
    }

    SECTION("Out-of-bounds segment layout and integer overflow are rejected") {
        auto bad_layout = BuildSyntheticNso(false);
        Loader::NSOHeader header{};
        std::memcpy(&header, bad_layout.data(), sizeof(header));
        // Text segment extending past data_end.
        header.segments[0].location = 0x3000;
        std::memcpy(bad_layout.data(), &header, sizeof(header));
        CHECK_FALSE(Loader::AppLoader_NSO::LoadModule(*process, system, *MakeNsoFile(bad_layout),
                                                      TestLoadBase, false, false)
                        .has_value());

        auto overflow_bss = BuildSyntheticNso(false);
        std::memcpy(&header, overflow_bss.data(), sizeof(header));
        header.segments[2].bss_size = 0xFFFFF000U;
        std::memcpy(overflow_bss.data(), &header, sizeof(header));
        CHECK_FALSE(Loader::AppLoader_NSO::LoadModule(*process, system, *MakeNsoFile(overflow_bss),
                                                      TestLoadBase, false, false)
                        .has_value());
    }

    SECTION("Zero compressed size and corrupted LZ4 payload are rejected") {
        auto zero_comp = BuildSyntheticNso(true);
        Loader::NSOHeader header{};
        std::memcpy(&header, zero_comp.data(), sizeof(header));
        header.segments_compressed_size[0] = 0;
        std::memcpy(zero_comp.data(), &header, sizeof(header));
        CHECK_FALSE(Loader::AppLoader_NSO::LoadModule(*process, system, *MakeNsoFile(zero_comp),
                                                      TestLoadBase, false, false)
                        .has_value());

        auto bad_lz4 = BuildSyntheticNso(true);
        std::memcpy(&header, bad_lz4.data(), sizeof(header));
        // Corrupt the LZ4 token/stream of the text segment so decompression fails or mismatches size.
        std::fill_n(bad_lz4.begin() + header.segments[0].offset,
                    header.segments_compressed_size[0], u8{0xFF});
        CHECK_FALSE(Loader::AppLoader_NSO::LoadModule(*process, system, *MakeNsoFile(bad_lz4),
                                                      TestLoadBase, false, false)
                        .has_value());
    }
}

TEST_CASE("VectorVfsFile enforces read/write offset and overflow bounds", "[core][nso-loader]") {
    FileSys::VectorVfsFile file{{1, 2, 3, 4}, "sample.bin"};
    std::array<u8, 8> out{};

    CHECK(file.Read(out.data(), 0, 0) == 0);
    CHECK(file.Read(out.data(), 4, 4) == 0);
    CHECK(file.Read(out.data(), 4, 100) == 0);

    CHECK(file.Read(out.data(), 8, 2) == 2);
    CHECK(out[0] == 3);
    CHECK(out[1] == 4);

    const std::array<u8, 2> tail{9, 10};
    CHECK(file.Write(tail.data(), 0, 0) == 0);
    CHECK(file.Write(tail.data(), tail.size(), (std::numeric_limits<size_t>::max)()) == 0);
    CHECK(file.GetSize() == 4);

    CHECK(file.Write(tail.data(), tail.size(), 4) == 2);
    CHECK(file.GetSize() == 6);
    CHECK(file.Read(out.data(), 6, 0) == 6);
    CHECK(out[4] == 9);
    CHECK(out[5] == 10);
}
