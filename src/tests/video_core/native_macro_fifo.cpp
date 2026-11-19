// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/engines/maxwell_3d.h"
#include "video_core/macro_dma_forward.h"

#ifdef ARCHITECTURE_x86_64
#include "video_core/macro_turing_jit.h"
#endif

namespace {
using Tegra::Engines::Maxwell3D;
namespace DmaForward = VideoCommon::DmaForward;

constexpr u32 MacroRegistersStart = 0xe00;
constexpr u32 DmaReadFifoed = 0x158;
constexpr u32 DmaAddressHigh = 0x154;
constexpr u32 DmaAddressLow = 0x155;
constexpr u32 RenderEnableMode = MAXWELL3D_REG_INDEX(render_enable.mode);
constexpr u32 RenderEnableAddress = MAXWELL3D_REG_INDEX(render_enable.address_low);
constexpr GPUVAddr DmaAddress = 0x1'0000'1000;
constexpr std::array<u32, 1> OneWord{1};

std::array<u32, 3> FifoInstruction(u32 method, bool end) {
    std::array<u32, 3> bits{};
    const auto field = [&](u32 offset, u32 count, u32 value) {
        for (u32 bit = 0; bit < count; ++bit) {
            bits[(offset + bit) / 32] |= ((value >> bit) & 1) << ((offset + bit) % 32);
        }
    };
    field(0, 1, end);
    for (u32 lane = 0; lane < 2; ++lane) {
        field(15 + lane * 36, 5, 24);
        field(20 + lane * 36, 5, 24);
        field(25 + lane * 36, 5, 24);
    }
    field(30, 16, method);
    field(82, 3, 5);
    field(85, 4, 3);
    return {bits[2], bits[1], bits[0]};
}

std::array<u32, 6> DmaForwardProgram(u32 target) {
    const auto request = FifoInstruction(DmaReadFifoed, true);
    // END executes one more instruction. That instruction emits the word just read by DMA.
    const auto emit = FifoInstruction(target, false);
    return {request[0], request[1], request[2], emit[0], emit[1], emit[2]};
}

u32 ReadRegister(u32 method) {
    if (method == DmaAddressHigh) {
        return static_cast<u32>(DmaAddress >> 32);
    }
    if (method == DmaAddressLow) {
        return static_cast<u32>(DmaAddress);
    }
    return 0;
}

DmaForward::Plan AnalyzeDma(std::span<const u32> program, std::span<const u32> parameters) {
    return DmaForward::Analyze(
        program, 0, parameters, {}, MacroRegistersStart,
        [](u32 method, bool& unknown) {
            unknown = false;
            return ReadRegister(method);
        },
        [](u32 method) { return method == RenderEnableMode; });
}
} // namespace

TEST_CASE("Native DMA dry run forwards the render-enable value", "[video_core][native_macro][dma]") {
    const auto plan = AnalyzeDma(DmaForwardProgram(RenderEnableMode), OneWord);
    INFO(plan.reason);
    REQUIRE(plan.eligible);
    REQUIRE(plan.num_writes == 2);
    CHECK(plan.dma_write_index == 0);
    CHECK(plan.writes[0].method == DmaReadFifoed);
    CHECK(plan.writes[0].value == 1);
    CHECK_FALSE(plan.writes[0].forwarded);
    CHECK(plan.writes[1].method == RenderEnableMode);
    CHECK(plan.writes[1].forwarded);
    CHECK(plan.snapshot.dma_source == DmaAddress);
    CHECK(plan.snapshot.dma_words == 1);
    CHECK(plan.snapshot.first_forwarded == 1);
}

TEST_CASE("Native DMA dry run rejects a write that cannot wait", "[video_core][native_macro][dma]") {
    const auto plan = AnalyzeDma(DmaForwardProgram(RenderEnableAddress), OneWord);
    INFO(plan.reason);
    CHECK_FALSE(plan.eligible);
    CHECK(plan.num_writes == 1);
}

TEST_CASE("Native DMA dry run rejects an empty parameter FIFO", "[video_core][native_macro][dma]") {
    const auto plan = AnalyzeDma(DmaForwardProgram(RenderEnableMode), {});
    INFO(plan.reason);
    CHECK_FALSE(plan.eligible);
    CHECK(plan.num_writes == 0);
}

TEST_CASE("Native DMA recomputation emits the same deferred write as direct execution",
          "[video_core][native_macro][dma]") {
    const auto program = DmaForwardProgram(RenderEnableMode);
    const auto plan = AnalyzeDma(program, OneWord);
    INFO(plan.reason);
    REQUIRE(plan.eligible);

    Tegra::TuringMacroInterpreter interpreter;
    for (const u32 word : {0U, 1U, 4U}) {
        CAPTURE(word);
        std::vector<u32> fifo{OneWord.front()};
        std::vector<std::pair<u32, u32>> direct;
        const auto result = interpreter.Execute(program, 0, fifo, ReadRegister,
                                                [&](u32 method, u32 value) {
            direct.emplace_back(method, value);
            if (method == DmaReadFifoed) {
                fifo.push_back(word);
            }
            return true;
        });
        INFO(result.error);
        REQUIRE(result.success);
        REQUIRE(direct.size() == 2);
        CHECK(direct.back() == std::pair{RenderEnableMode, word});

        std::vector<std::pair<u32, u32>> recomputed;
        REQUIRE(DmaForward::Recompute(plan.snapshot, std::array{word}, MacroRegistersStart,
                                      [&](u32 method, u32 value) {
            recomputed.emplace_back(method, value);
        }));
        REQUIRE(recomputed.size() == 1);
        CHECK(recomputed.front() == direct.back());
    }
}

#ifdef ARCHITECTURE_x86_64
TEST_CASE("Owned native macro FIFOs preserve loads across DMA appends and repeated calls",
          "[video_core][native_macro]") {
    std::vector<u32> code;
    for (const auto instruction : {FifoInstruction(0x158, false), FifoInstruction(0x200, true),
                                   FifoInstruction(0x201, false)}) {
        code.insert(code.end(), instruction.begin(), instruction.end());
    }
    Tegra::TuringMacroJit jit;
    jit.Prepare(code, 1);
    std::array<u32, 0xc00> ram{};
    std::vector<u32> parameters;
    std::vector<u32> reference;
    for (u32 invocation = 0; invocation < 1000; ++invocation) {
        const u32 count = 2 + invocation % 127;
        parameters.push_back(count);
        reference.assign(parameters.begin(), parameters.end());
        const auto run = [&](std::vector<u32>& input) {
            std::vector<std::pair<u32, u32>> writes;
            const auto result = jit.Execute(
                0, input, ram, [](u32) { return 0U; },
                [&](u32 method, u32 value) {
                    writes.emplace_back(method, value);
                    if (method == 0x158) {
                        for (u32 word = 0; word < value; ++word) {
                            input.push_back(invocation * 256 + word);
                        }
                    }
                    return true;
                });
            CHECK(result.success);
            CHECK(result.consumed == 3);
            return writes;
        };
        const auto expected = run(reference);
        const auto actual = run(parameters);
        REQUIRE(actual.size() == 3);
        CHECK(actual == expected);
        CHECK(parameters == reference);
        CHECK(actual[1] == std::pair<u32, u32>{0x200, invocation * 256});
        CHECK(actual[2] == std::pair<u32, u32>{0x201, invocation * 256 + 1});
        parameters.clear();
    }
}

TEST_CASE("Batched native macro emits and borrowed parameters match the plain path",
          "[video_core][native_macro]") {
    std::vector<u32> code;
    for (const auto instruction : {FifoInstruction(0x158, false), FifoInstruction(0x200, true),
                                   FifoInstruction(0x201, false)}) {
        code.insert(code.end(), instruction.begin(), instruction.end());
    }
    Tegra::TuringMacroJit plain;
    Tegra::TuringMacroJit batched;
    plain.Prepare(code, 1);
    batched.Prepare(code, 1, true);
    std::array<u32, 0xc00> ram{};
    for (u32 invocation = 0; invocation < 200; ++invocation) {
        const std::array<u32, 1> words{2 + invocation % 127};
        const auto run = [&](Tegra::TuringMacroJit& jit, bool batch, bool borrow) {
            std::vector<u32> fifo;
            if (!borrow) {
                fifo.assign(words.begin(), words.end());
            }
            std::vector<std::pair<u32, u32>> writes;
            const auto result = jit.Execute(
                0, fifo, borrow ? std::span<const u32>{words} : std::span<const u32>{}, ram,
                [](u32) { return 0U; },
                [&](u32 method, u32 value) {
                    writes.emplace_back(method, value);
                    if (method == 0x158) {
                        if (fifo.empty()) {
                            fifo.assign(words.begin(), words.end());
                        }
                        for (u32 word = 0; word < value; ++word) {
                            fifo.push_back(invocation * 256 + word);
                        }
                    }
                    return true;
                },
                {.batch_emits = batch, .keep_state = batch});
            CHECK(result.success);
            CHECK(result.consumed == 3);
            return writes;
        };
        const auto expected = run(plain, false, false);
        REQUIRE(expected.size() == 3);
        CHECK(run(batched, true, false) == expected);
        CHECK(run(batched, true, true) == expected);
        CHECK(run(plain, false, true) == expected);
    }
}
#endif
