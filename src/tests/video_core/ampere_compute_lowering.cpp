// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <functional>
#include <span>
#include <string_view>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "common/page_table.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "tests/core/system_test_context.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/macro.h"
#include "video_core/memory_manager.h"
#include "video_core/query_cache/types.h"
#include "video_core/rasterizer_interface.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
using Shader::Ampere::LowerToMaxwellWith;

constexpr Instruction Exit{{0x0000794d, 0, 0x03800000, 0}};

constexpr Instruction UniformLea(u32 guard, u32 carry) {
    // ULEA UR12, UP(carry), UR11, UR19, 0x3. Predicate 7 discards the carry.
    return {{0x291 | (guard << 12) | (12U << 16) | (11U << 24), 19,
             63 | (3U << 11) | (carry << 17) | (1U << 27), 0}};
}

TEST_CASE("Ampere guarded ULEA restores the guest predicate", "[video_core][ampere]") {
    constexpr u32 NegatedUP2 = 8 | 2;
    const auto lowered = LowerToMaxwell(std::array{UniformLea(NegatedUP2, 7), Exit});
    INFO(lowered.error);
    REQUIRE(lowered);
    const auto& words = lowered.instructions;
    const auto lea = std::ranges::find_if(words, [](u64 word) { return (word >> 48) == 0x5c18; });
    REQUIRE(lea != words.end());
    CHECK((*lea & 255) == 228); // UR12
    CHECK(((*lea >> 8) & 255) == 227); // UR11
    CHECK(((*lea >> 20) & 255) == 235); // UR19
    CHECK(((*lea >> 16) & 15) == (8 | 4)); // UP2 is temporarily loaded into P4
    CHECK(((*lea >> 39) & 31) == 3);

    const auto loads_predicates_from = [](u64 word, u32 reg) {
        return (word >> 48) == 0x38f0 && ((word >> 8) & 255) == reg &&
               ((word >> 20) & 255) == (1 << 4);
    };
    CHECK(std::any_of(words.begin(), lea,
                      [&](u64 word) { return loads_predicates_from(word, 239); }));
    CHECK(std::any_of(lea + 1, words.end(),
                      [&](u64 word) { return loads_predicates_from(word, 253); }));
}

TEST_CASE("Ampere ULEA writes its carry to the selected uniform predicate",
          "[video_core][ampere]") {
    const auto lowered = LowerToMaxwell(std::array{UniformLea(7, 1), Exit});
    INFO(lowered.error);
    REQUIRE(lowered);
    const auto& words = lowered.instructions;
    const auto compare = std::ranges::find_if(words, [](u64 word) { return (word >> 52) == 0x5b6; });
    REQUIRE(compare != words.end());
    CHECK(((*compare >> 3) & 7) == 5); // P5 is the temporary for UP1
    CHECK(std::any_of(compare + 1, words.end(), [](u64 word) {
        return (word >> 48) == 0x38e8 && (word & 255) == 239 &&
               ((word >> 20) & 255) == (1 << 5);
    }));
}

TEST_CASE("Ampere surface reductions preserve every U32 atomic operation",
          "[video_core][ampere]") {
    for (u32 op = 0; op < 8; ++op) {
        if (op == 3 || op == 4) continue;
        for (const u32 dimension : {1U, 3U}) {
            for (const u32 guard : {1U, 7U, 9U}) {
                const Instruction instruction{{0x0400059fU | (guard << 12),
                    0x00008400U | (dimension << 29), 0x0010e000U | (op << 23), 0}};
                const auto lowered = LowerToMaxwell(std::array{instruction, Exit});
                INFO(lowered.error);
                REQUIRE(lowered);
                REQUIRE(lowered.required_texture_buffer == 0);
                CHECK(lowered.instructions[0] == (0xea000000000004ffULL | (u64{guard} << 16) |
                    (u64{op} << 29) | (u64{dimension} << 33) | (0x84ULL << 36)));
                auto invalid = instruction;
                invalid.words[2] |= 2U << 9;
                CHECK_FALSE(LowerToMaxwell(std::array{invalid, Exit}));
            }
        }
    }
}

TEST_CASE("Half per-tap gathers prove offsets across an unrelated LEA.HI",
          "[video_core][ampere][gather]") {
    const Instruction lea{{0x08087211, 0x00000007, 0x078ff8ff, 0x000fc800}};
    const Instruction gather{{0x0a0c7b63, 0x30003000, 0x001eaf0d, 0x000fc200}};
    auto code = std::array{
        Instruction{{0x00007802, 0xff00ff00, 0xf00, 0}},
        Instruction{{0x00017802, 0x01000001, 0xf00, 0}},
        lea, gather, Exit,
    };
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(lowered);
    code[2].words[0] = 0x08017211;
    CHECK(LowerToMaxwell(code).error == "native per-tap gather nonconstant offsets");
}

TEST_CASE("Ampere DEPBAR.LE lowers to a no-op without async copies", "[video_core][ampere]") {
    const Instruction depbar{{0x791a, 0x8001, 0, 0}};
    const auto lowered = LowerToMaxwell(std::array{depbar, Exit});
    INFO(lowered.error);
    REQUIRE(lowered);
    REQUIRE(lowered.instructions.size() == 1);
    CHECK(lowered.instructions[0] == 0xe30000000007000fULL);

    // Paired with LDGSTS (0xfae), DEPBAR.LE on SB0 emits MEMBAR.CTA.
    const Instruction ldgsts{{0xfaeU | (7U << 12) | (6U << 16) | (2U << 24), 0, 0x0f901c08U, 0}};
    const auto with_copy = LowerToMaxwell(std::array{ldgsts, depbar, Exit});
    INFO(with_copy.error);
    REQUIRE(with_copy);
    CHECK(with_copy.instructions[with_copy.source_instruction_offsets[1]] == 0xef98000000070000ULL);
    CHECK(with_copy.source_instruction_offsets[2] == with_copy.source_instruction_offsets[1] + 1);

    Instruction invalid = depbar;
    invalid.words[2] = 1;
    CHECK(LowerToMaxwell(std::array{invalid, Exit}).error == "native dependency barrier mode");
}

TEST_CASE("Ampere guarded ULEA rejects carry-out and immediate ULEA lowers cleanly",
          "[video_core][ampere]") {
    // Guarded ULEA (pred != 7) with carry-out (dst_pred != 7) must be rejected.
    const auto rejected = LowerToMaxwell(std::array{UniformLea(1, 2), Exit});
    CHECK_FALSE(rejected);
    CHECK(rejected.instructions.empty());

    // Immediate ULEA (0x891): ULEA UR8, UR10, 0x100, 0x2.
    const Instruction imm_ulea{
        {0x891U | (7U << 12) | (8U << 16) | (10U << 24), 0x100U,
         63U | (2U << 11) | (7U << 17) | (1U << 27), 0}};
    const auto lowered = LowerToMaxwell(std::array{imm_ulea, Exit});
    INFO(lowered.error);
    REQUIRE(lowered);
    const auto iscadd = std::ranges::find_if(lowered.instructions,
                                             [](u64 word) { return (word >> 48) == 0x5c18; });
    REQUIRE(iscadd != lowered.instructions.end());
    CHECK((*iscadd & 255) == 224);       // UR8
    CHECK(((*iscadd >> 8) & 255) == 226); // UR10
    CHECK(((*iscadd >> 20) & 255) == 251);
    CHECK(((*iscadd >> 39) & 31) == 2);
}

TEST_CASE("Ampere LEA and LEA.HI with constant base lower cleanly and reject carry-out on LEA.HI",
          "[video_core][ampere]") {
    // LEA.HI R4, R6, c[0x2][0x20], RZ, 4 (bit 80=.HI, bits 75..79=4, bits 81..83=7 no carry).
    Instruction lea_hi{{0x0a11U | (7U << 12) | (4U << 16) | (6U << 24),
                        (0x20U << 6) | (2U << 22), 255U | (4U << 11) | (1U << 16) | (7U << 17), 0}};
    const auto hi_lowered = LowerToMaxwell(std::array{lea_hi, Exit});
    INFO(hi_lowered.error);
    REQUIRE(hi_lowered);
    CHECK(std::ranges::any_of(hi_lowered.instructions, [](u64 word) {
        return (word >> 48) == 0x4c98 && (word & 255) == 251;
    }));
    CHECK(std::ranges::any_of(hi_lowered.instructions, [](u64 word) {
        return (word >> 48) == 0x3828 && (word & 255) == 250 && ((word >> 8) & 255) == 6 &&
               ((word >> 20) & 0x3f) == 28;
    }));
    CHECK(std::ranges::any_of(hi_lowered.instructions, [](u64 word) {
        return (word >> 48) == 0x5c10 && (word & 255) == 4 && ((word >> 8) & 255) == 250 &&
               ((word >> 20) & 255) == 251;
    }));

    // Setting a carry-out predicate (P0) on LEA.HI with constant base is rejected.
    lea_hi.words[2] &= ~(7U << 17);
    CHECK(LowerToMaxwell(std::array{lea_hi, Exit}).error == "native constant LEA carry mode");

    // LEA R4, R6, c[0x2][0x20], 3 -> ISCADD R4, R6, R251, 3.
    const Instruction lea{{0x0a11U | (7U << 12) | (4U << 16) | (6U << 24),
                           (0x20U << 6) | (2U << 22), 255U | (3U << 11) | (7U << 17), 0}};
    const auto lea_lowered = LowerToMaxwell(std::array{lea, Exit});
    INFO(lea_lowered.error);
    REQUIRE(lea_lowered);
    const auto iscadd = std::ranges::find_if(lea_lowered.instructions,
                                             [](u64 word) { return (word >> 48) == 0x5c18; });
    REQUIRE(iscadd != lea_lowered.instructions.end());
    CHECK((*iscadd & 255) == 4);
    CHECK(((*iscadd >> 8) & 255) == 6);
    CHECK(((*iscadd >> 20) & 255) == 251);
    CHECK(((*iscadd >> 39) & 31) == 3);
}

TEST_CASE("Ampere SHF.L.U32.HI immediate clamps without .W and wraps with .W",
          "[video_core][ampere]") {
    Instruction shift{{0x419U | (7U << 12) | (4U << 16) | (255U << 24), 0x1234U, 0x00010600U | 6U, 0}};
    for (const bool wrap : {false, true}) {
        CAPTURE(wrap);
        shift.words[2] = (0x00010600U | 6U) | (u32{wrap} << 11);
        const auto lowered = LowerToMaxwell(std::array{shift, Exit});
        INFO(lowered.error);
        REQUIRE(lowered);
        const auto shl = std::ranges::find_if(lowered.instructions,
                                              [](u64 word) { return (word >> 48) == 0x5c48; });
        REQUIRE(shl != lowered.instructions.end());
        CHECK((*shl & 255) == 4);
        CHECK(((*shl >> 8) & 255) == 250);
        CHECK(((*shl >> 20) & 255) == 6);
        CHECK(((*shl >> 39) & 1) == u64{wrap});
    }
}

TEST_CASE("Ampere guarded UIADD3 and USHF.R.U32.HI swap and restore the uniform predicate",
          "[video_core][ampere]") {
    // @!UP0 UIADD3 UR6, UR14, 0x1c, URZ (no carry outputs).
    const Instruction uiadd3{{0x890U | (8U << 12) | (6U << 16) | (14U << 24), 0x1cU, 0x07ffe03fU, 0}};
    const auto add_lowered = LowerToMaxwell(std::array{uiadd3, Exit});
    INFO(add_lowered.error);
    REQUIRE(add_lowered);
    const auto iadd32i = std::ranges::find_if(add_lowered.instructions,
                                              [](u64 word) { return (word >> 56) == 0x1c; });
    REQUIRE(iadd32i != add_lowered.instructions.end());
    CHECK((*iadd32i & 255) == 222);
    CHECK(((*iadd32i >> 8) & 255) == 230);
    CHECK(((*iadd32i >> 16) & 15) == (8 | 6));
    CHECK(((*iadd32i >> 20) & 0xffffffffU) == 0x1cU);
    CHECK(std::any_of(add_lowered.instructions.begin(), iadd32i, [](u64 word) {
        return (word >> 48) == 0x38f0 && ((word >> 8) & 255) == 239 &&
               ((word >> 20) & 255) == (1 << 6);
    }));
    CHECK(std::any_of(iadd32i + 1, add_lowered.instructions.end(), [](u64 word) {
        return (word >> 48) == 0x38f0 && ((word >> 8) & 255) == 253 &&
               ((word >> 20) & 255) == (1 << 6);
    }));

    // @UP1 USHF.R.U32.HI UR6, URZ, 0x4, UR10 (signed right shift).
    const Instruction ushf{{0x899U | (1U << 12) | (6U << 16) | (63U << 24), 4U, 0x08011400U | 10U, 0}};
    const auto shf_lowered = LowerToMaxwell(std::array{ushf, Exit});
    INFO(shf_lowered.error);
    REQUIRE(shf_lowered);
    const auto shr = std::ranges::find_if(shf_lowered.instructions, [](u64 word) {
        return ((word & ~(u64{1} << 48)) >> 48) == 0x3828;
    });
    REQUIRE(shr != shf_lowered.instructions.end());
    CHECK((*shr & 255) == 222);
    CHECK(((*shr >> 8) & 255) == 226);
    CHECK(((*shr >> 16) & 15) == 5);
    CHECK(((*shr >> 20) & 0x3f) == 4);
    CHECK(((*shr >> 48) & 1) == 1);
    CHECK( std::any_of(shf_lowered.instructions.begin(), shr, [](u64 word) {
        return (word >> 48) == 0x38f0 && ((word >> 8) & 255) == 239 &&
               ((word >> 20) & 255) == (1 << 5);
    }));
    CHECK(std::any_of(shr + 1, shf_lowered.instructions.end(), [](u64 word) {
        return (word >> 48) == 0x38f0 && ((word >> 8) & 255) == 253 &&
               ((word >> 20) & 255) == (1 << 5);
    }));
}

TEST_CASE("Per-tap gather backward scan detects multi-register LDG.E.64 clobbering high half",
          "[video_core][ampere][gather]") {
    // Synthetic per-tap TLD4 (0xb63): Rd=R8, Ra=R10..R11, Rb=R5..R6 (offset_mode=2), 2D (dim=1), mask=0xf.
    constexpr Instruction Gather{{0xb63U | (7U << 12) | (8U << 16) | (10U << 24),
                                  5U | (1U << 29),
                                  255U | (0xfU << 8) | (2U << 12) | (7U << 17), 0}};
    constexpr Instruction MovR5{{0x00057802, 0xff00ff00, 0xf00, 0}};
    constexpr Instruction MovR6{{0x00067802, 0x01000001, 0xf00, 0}};

    // Unrelated LDG.E.64 R12, [R2] (writes R12..R13) leaves R5..R6 intact.
    const Instruction ldg_unrelated{
        {0x381U | (7U << 12) | (12U << 16) | (2U << 24), 0, 0x001e8100U | (5U << 9), 0}};
    const auto valid = LowerToMaxwell(std::array{MovR5, MovR6, ldg_unrelated, Gather, Exit});
    INFO(valid.error);
    REQUIRE(valid);

    // LDG.E.64 R4, [R2] writes R4..R5, clobbering R5 (the first offset register).
    const Instruction ldg_clobber_r5{
        {0x381U | (7U << 12) | (4U << 16) | (2U << 24), 0, 0x001e8100U | (5U << 9), 0}};
    const auto clobbered = LowerToMaxwell(std::array{MovR5, MovR6, ldg_clobber_r5, Gather, Exit});
    CHECK_FALSE(clobbered);
    CHECK(clobbered.error == "native per-tap gather nonconstant offsets");
}

TEST_CASE("Half per-tap gathers detect TXD and multi-register offset clobbers",
          "[video_core][ampere][gather]") {
    constexpr Instruction Gather{{0x0a0c7b63, 0x30003000, 0x001eaf0d, 0x000fc200}};
    constexpr Instruction MovR0{{0x00007802, 0xff00ff00, 0xf00, 0}};
    constexpr Instruction MovR1{{0x00017802, 0x01000001, 0xf00, 0}};

    constexpr auto make_txd = [](u32 dest, u32 high, u32 mask, u32 binding = 12,
                                 u32 coordinates = 8, u32 derivatives = 12, u32 dimension = 1) {
        return Instruction{{
            0xb6cU | (7U << 12) | (dest << 16) | (coordinates << 24),
            derivatives | (binding << 22) | (dimension << 29),
            0x001e0000U | (mask << 8) | high,
            0,
        }};
    };
    constexpr auto make_txd_bindless = [](u32 ur_index, u32 dest, u32 high, u32 mask,
                                          u32 coordinates = 8, u32 derivatives = 12,
                                          u32 dimension = 1) {
        return Instruction{{
            0xf6cU | (7U << 12) | (dest << 16) | (coordinates << 24),
            derivatives | (ur_index << 8) | (dimension << 29),
            0x081e0000U | (mask << 8) | high,
            0,
        }};
    };

    const auto txd_clean = LowerToMaxwell(
        std::array{MovR0, MovR1, make_txd(20, 22, 0xf), Gather, Exit});
    INFO(txd_clean.error);
    REQUIRE(txd_clean);

    for (const Instruction& clobbering_txd : {
             make_txd(0, 22, 0xf),
             make_txd(20, 0, 0xf),
             make_txd_bindless(4, 20, 0, 0xf),
         }) {
        const auto rejected = LowerToMaxwell(
            std::array{MovR0, MovR1, clobbering_txd, Gather, Exit});
        CHECK_FALSE(rejected);
        CHECK(rejected.error == "native per-tap gather offset clobber");
    }

    struct MultiRegWriter {
        std::string_view name;
        Instruction (*make)(u32 dest);
    };
    constexpr std::array writers{
        MultiRegWriter{
            "LDG.E.64",
            [](u32 dest) {
                return Instruction{
                    {0x381U | (7U << 12) | (dest << 16) | (8U << 24), 0, 0x001e8100U | (5U << 9), 0}};
            },
        },
        MultiRegWriter{
            "LDG.E.128",
            [](u32 dest) {
                return Instruction{
                    {0x381U | (7U << 12) | (dest << 16) | (8U << 24), 0, 0x001e8100U | (6U << 9), 0}};
            },
        },
        MultiRegWriter{
            "LDC.64",
            [](u32 dest) {
                return Instruction{
                    {0xb82U | (7U << 12) | (dest << 16) | (8U << 24), 0, 5U << 9, 0}};
            },
        },
        MultiRegWriter{
            "LDSM.16.M88.2",
            [](u32 dest) {
                return Instruction{
                    {0x83bU | (7U << 12) | (dest << 16) | (8U << 24), 0, 1U << 8, 0}};
            },
        },
        MultiRegWriter{
            "ALD.64",
            [](u32 dest) {
                return Instruction{
                    {0x321U | (7U << 12) | (dest << 16) | (255U << 24), 255U, 1U << 10, 0}};
            },
        },
        MultiRegWriter{
            "HMMA.16816.F16",
            [](u32 dest) {
                return Instruction{
                    {0x23cU | (7U << 12) | (dest << 16) | (16U << 24), 20U, 0x800U | 255U, 0}};
            },
        },
        MultiRegWriter{
            "LDL.64",
            [](u32 dest) {
                return Instruction{
                    {0x983U | (7U << 12) | (dest << 16) | (255U << 24), 0, 0x00100000U | (5U << 9), 0}};
            },
        },
    };

    for (const auto& writer : writers) {
        CAPTURE(writer.name);
        const auto valid = LowerToMaxwellWith(
            std::array{MovR0, MovR1, writer.make(4), Gather, Exit}, 0, {.tensor = true});
        INFO(valid.error);
        REQUIRE(valid);

        const auto clobbered = LowerToMaxwellWith(
            std::array{MovR1, writer.make(0), MovR0, Gather, Exit}, 0, {.tensor = true});
        CHECK_FALSE(clobbered);
        CHECK(clobbered.error == "native per-tap gather nonconstant offsets");
    }
}

class RecordingComputeRasterizer final : public VideoCore::RasterizerInterface {
public:
    struct RecordedQuery {
        GPUVAddr address;
        VideoCommon::QueryType type;
        VideoCommon::QueryPropertiesFlags flags;
        u32 payload;
        u32 subreport;
    };

    struct RecordedInlineUpload {
        GPUVAddr address;
        size_t size;
        std::vector<u8> bytes;
    };

    void Draw(bool, u32) override {}
    void DrawTexture() override {}
    void Clear(u32) override {}
    void DispatchCompute() override {}
    void ResetCounter(VideoCommon::QueryType) override {}
    void Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
               VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) override {
        queries.push_back({gpu_addr, type, flags, payload, subreport});
    }
    void BindGraphicsUniformBuffer(size_t, u32, GPUVAddr, u32) override {}
    void DisableGraphicsUniformBuffer(size_t, u32) override {}
    void SignalFence(std::function<void()>&& func, bool) override {
        ++fences;
        func();
    }
    void SyncOperation(std::function<void()>&&) override {}
    void SignalSyncPoint(u32) override {}
    void SignalReference() override {}
    void ReleaseFences(bool) override {}
    void FlushAll() override {}
    void FlushRegion(DAddr, u64, VideoCommon::CacheType) override {}
    bool MustFlushRegion(DAddr, u64, VideoCommon::CacheType) override {
        return false;
    }
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override {
        return {addr, addr + size, false};
    }
    void InvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void OnCacheInvalidation(PAddr, u64) override {}
    bool OnCPUWrite(PAddr, u64) override {
        return false;
    }
    void InvalidateGPUCache() override {}
    void UnmapMemory(DAddr, u64) override {}
    void ModifyGPUMemory(size_t, GPUVAddr, u64) override {}
    void FlushAndInvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void WaitForIdle() override {}
    void FragmentBarrier() override {}
    void TiledCacheBarrier() override {}
    void FlushCommands() override {}
    void TickFrame() override {}
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override {
        std::terminate();
    }
    void AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                  std::span<const u8> memory) override {
        inline_uploads.push_back(
            {address, copy_size, std::vector<u8>(memory.begin(), memory.end())});
    }

    std::vector<RecordedQuery> queries;
    std::vector<RecordedInlineUpload> inline_uploads;
    u32 fences = 0;
};

TEST_CASE("KeplerCompute native mode classifies memory-writing methods and dispatches semaphores and inline QMDs",
          "[video_core][ampere][compute]") {
    ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    Core::DeviceMemory physical;
    RecordingComputeRasterizer rasterizer;
    Tegra::MaxwellDeviceMemoryManager device_memory{physical};
    device_memory.BindInterface(&rasterizer);
    Kernel::KProcess process{system.Kernel()};
    SCOPE_EXIT { system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    u8* const backing = physical.GetPointerFromRaw<u8>(0x10000);
    std::memset(backing, 0, 0x1000);
    table.entries.GetAndFault(4).Store(false, Common::PageType::Memory, 1,
                                     reinterpret_cast<uintptr_t>(backing) - 0x4000);
    process.GetMemory().SetCurrentPageTable(process);
    const auto asid = device_memory.RegisterProcess(&process.GetMemory());

    constexpr GPUVAddr MappedAddress = 0x2000'0000;
    constexpr DAddr DeviceAddress = 0x1000'0000;
    constexpr u64 PageSize = 0x1000;
    SCOPE_EXIT {
        device_memory.Unmap(DeviceAddress, PageSize);
        device_memory.UnregisterProcess(asid);
    };
    device_memory.Map(DeviceAddress, 0x4000, PageSize, asid);

    Tegra::MemoryManager memory{system, device_memory, 32, 1ULL << 31, 16};
    memory.BindRasterizer(&rasterizer);
    memory.Map(MappedAddress, DeviceAddress, PageSize, Tegra::PTEKind::PITCH, false);

    Tegra::Engines::KeplerCompute compute{memory};
    compute.BindRasterizer(&rasterizer);

    // Non-native MayWriteMemory classification.
    REQUIRE_FALSE(compute.IsNativeMode());
    REQUIRE_FALSE(compute.FrontRunsMacros());
    for (const u32 method : {0x60U, 0x107U, 0x6c3U}) {
        CAPTURE(method);
        CHECK_FALSE(compute.MayWriteMemory(method));
    }
    for (const u32 method : {0x6cU, 0x6dU, 0xafU, 0xe00U}) {
        CAPTURE(method);
        CHECK(compute.MayWriteMemory(method));
    }

    // Enable native mode and verify native method classification.
    Tegra::MacroEngine macro_engine{true};
    std::array<u32, 0x80> positions{};
    compute.SetNativeMacroMode(&macro_engine, positions);
    REQUIRE(compute.IsNativeMode());
    REQUIRE(compute.FrontRunsMacros());
    for (const u32 method : {0x60U, 0xadU, 0x6c0U, 0x6c1U, 0x6c2U}) {
        CAPTURE(method);
        CHECK_FALSE(compute.MayWriteMemory(method));
    }
    for (const u32 method : {0x45U, 0x46U, 0x47U, 0x48U, 0xaeU, 0xafU, 0xb0U, 0x107U, 0x6c3U,
                             0x6cU, 0x6dU, 0xe00U}) {
        CAPTURE(method);
        CHECK(compute.MayWriteMemory(method));
    }

    // Plain native CallMultiMethod stores the final word of the batch.
    constexpr std::array<u32, 3> multi_batch{0x11111111U, 0x22222222U, 0x33333333U};
    compute.CallMultiMethod(system, 0xc8, multi_batch.data(), static_cast<u32>(multi_batch.size()),
                            0);
    CHECK(compute.regs.reg_array[0xc8] == multi_batch.back());

    // Native report semaphore: one-word release with 41-bit address.
    constexpr GPUVAddr SemaphoreAddress = (GPUVAddr{0x123} << 32) | 0x4567'8900ULL;
    constexpr u32 SemaphorePayload = 0xdeadbeefU;
    compute.CallMethod(system, 0x6c0, static_cast<u32>(SemaphoreAddress >> 32), true);
    compute.CallMethod(system, 0x6c1, static_cast<u32>(SemaphoreAddress), true);
    compute.CallMethod(system, 0x6c2, SemaphorePayload, true);
    compute.CallMethod(system, 0x6c3, 1U << 28, true);
    REQUIRE(rasterizer.queries.size() == 1);
    CHECK(rasterizer.queries[0].address == SemaphoreAddress);
    CHECK(rasterizer.queries[0].type == VideoCommon::QueryType::Payload);
    CHECK(rasterizer.queries[0].flags == VideoCommon::QueryPropertiesFlags::IsAFence);
    CHECK(rasterizer.queries[0].payload == SemaphorePayload);
    CHECK(rasterizer.queries[0].subreport == 0);
    CHECK(rasterizer.fences == 0);

    // Native report semaphore: unsigned ADD reduction updates mapped guest memory via SignalFence.
    memory.Write<u32>(MappedAddress, 0x0000'0010U);
    REQUIRE(memory.Read<u32>(MappedAddress) == 0x0000'0010U);
    compute.CallMethod(system, 0x6c0, static_cast<u32>(MappedAddress >> 32), true);
    compute.CallMethod(system, 0x6c1, static_cast<u32>(MappedAddress), true);
    compute.CallMethod(system, 0x6c2, 0x0000'0025U, true);
    compute.CallMethod(system, 0x6c3, 1U << 3, true);
    CHECK(rasterizer.queries.size() == 1);
    CHECK(rasterizer.fences == 1);
    CHECK(memory.Read<u32>(MappedAddress) == 0x0000'0035U);

    // Native report semaphore: unsupported operation emits neither a query nor a fence.
    compute.CallMethod(system, 0x6c3, 1U, true);
    CHECK(rasterizer.queries.size() == 1);
    CHECK(rasterizer.fences == 1);
    CHECK(memory.Read<u32>(MappedAddress) == 0x0000'0035U);

    // Native inline QMD upload (0x107): mapped address emits 256 bytes matching 0xc8..0x107.
    std::array<u32, 64> expected_qmd{};
    expected_qmd[0] = multi_batch.back();
    for (u32 i = 1; i < expected_qmd.size(); ++i) {
        expected_qmd[i] = 0xa500'0000U | i;
    }
    expected_qmd[4] = 1U << 12; // Skip implicit QMD dispatch on 0x107.
    for (u32 i = 1; i < 63; ++i) {
        compute.CallMethod(system, 0xc8U + i, expected_qmd[i], true);
    }
    compute.CallMethod(system, 0xc6, 0U, true);
    compute.CallMethod(system, 0xc7, static_cast<u32>(MappedAddress >> 8), true);
    compute.CallMethod(system, 0x107, expected_qmd[63], true);
    REQUIRE(rasterizer.inline_uploads.size() == 1);
    CHECK(rasterizer.inline_uploads[0].address == MappedAddress);
    CHECK(rasterizer.inline_uploads[0].size == 256);
    REQUIRE(rasterizer.inline_uploads[0].bytes.size() == 256);
    CHECK(std::memcmp(rasterizer.inline_uploads[0].bytes.data(), expected_qmd.data(), 256) == 0);
    CHECK_FALSE(macro_engine.native_execution_failed);

    // Unmapped inline QMD destination sets native_execution_failed without uploading.
    compute.CallMethod(system, 0xc7, static_cast<u32>(0x3000'0000ULL >> 8), true);
    compute.CallMethod(system, 0x107, expected_qmd[63], true);
    CHECK(rasterizer.inline_uploads.size() == 1);
    CHECK(macro_engine.native_execution_failed);
}
} // namespace
