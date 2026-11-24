// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#ifdef EGLANTINE_ENABLE_RUST_AMPERE

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "eglantine_ampere.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ampere/program_override.h"

namespace {

using Shader::Ampere::AppendNativeAddressMap;
using Shader::Ampere::DecodeProgramOverride;
using Shader::Ampere::DecodeProgramOverrideCpp;
using Shader::Ampere::DecodeProgramOverrideRust;
using Shader::Ampere::EncodeProgramOverride;
using Shader::Ampere::EncodeProgramOverrideCpp;
using Shader::Ampere::EncodeProgramOverrideRust;
using Shader::Ampere::HasNativeAddressMap;
using Shader::Ampere::HasNativeAddressMapCpp;
using Shader::Ampere::HasNativeAddressMapRust;
using Shader::Ampere::Instruction;
using Shader::Ampere::LoweredProgram;
using Shader::Ampere::LoweringOptions;
using Shader::Ampere::LowerToMaxwell;
using Shader::Ampere::LowerToMaxwellCpp;
using Shader::Ampere::LowerToMaxwellRust;
using Shader::Ampere::NativeProgramAddress;
using Shader::Ampere::NativeProgramAddressCpp;
using Shader::Ampere::NativeProgramAddressRust;
using Shader::Ampere::NativeProgramKey;
using Shader::Ampere::NativeProgramKeyCpp;
using Shader::Ampere::NativeProgramKeyRust;
using Shader::Ampere::ProgramCaptureState;
using Shader::Ampere::ProgramOverrideSourceSize;
using Shader::Ampere::ProgramOverrideSourceSizeCpp;
using Shader::Ampere::ProgramOverrideSourceSizeRust;

constexpr Instruction kExit{{0x0000794dU, 0U, 0x03800000U, 0U}};
constexpr Instruction kNop{{0x00007918U, 0U, 0U, 0U}};

static_assert(sizeof(EglAmpereCaptureState) == 96);
static_assert(alignof(EglAmpereCaptureState) == 4);
static_assert(offsetof(EglAmpereCaptureState, exit_setup) == 0);
static_assert(offsetof(EglAmpereCaptureState, exit_handler_end) == 80);
static_assert(offsetof(EglAmpereCaptureState, last_call_target) == 84);
static_assert(offsetof(EglAmpereCaptureState, last_forward_target) == 88);
static_assert(offsetof(EglAmpereCaptureState, main_exited) == 92);
static_assert(offsetof(EglAmpereCaptureState, computed_branch) == 93);
static_assert(offsetof(EglAmpereCaptureState, _pad) == 94);

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const char* value) : env_name(name) {
        if (const char* existing = std::getenv(name)) {
            had_previous = true;
            previous_value = existing;
        }
        Apply(value);
    }

    ~ScopedEnvVar() {
        Apply(had_previous ? previous_value.c_str() : nullptr);
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

private:
    void Apply(const char* value) const {
#ifdef _WIN32
        _putenv_s(env_name.c_str(), value != nullptr ? value : "");
        SetEnvironmentVariableA(env_name.c_str(), value);
#else
        if (value != nullptr) {
            setenv(env_name.c_str(), value, 1);
        } else {
            unsetenv(env_name.c_str());
        }
#endif
    }

    std::string env_name;
    std::string previous_value;
    bool had_previous{};
};

void CheckLoweredParity(std::span<const Instruction> code, LoweringOptions options = {}) {
    const LoweredProgram cpp_prog = LowerToMaxwellCpp(code, options);
    const LoweredProgram rust_prog = LowerToMaxwellRust(code, options);
    const auto mismatch = Shader::Ampere::detail::FindLoweredProgramMismatch(cpp_prog, rust_prog);
    INFO(mismatch.value_or(std::string{}));
    REQUIRE_FALSE(mismatch.has_value());
}

} // namespace

TEST_CASE("Ampere LowerToMaxwell C++ and Rust FFI parity", "[video_core][ampere]") {
    SECTION("Valid programs across lowering options and edge cases") {
        // Minimal EXIT
        CheckLoweredParity(std::array{kExit});

        // CALL + main EXIT + RET relocation
        CheckLoweredParity(std::array<Instruction, 4>{{
            {{0x00097802U, 0x20U, 0x00000f00U, 0U}},
            {{0x00007944U, 0x10U, 0x03c00000U, 0U}},
            kExit,
            {{0x09007950U, 0xffffffc0U, 0x03c3ffffU, 0U}},
        }});

        // Register pressure shadow copies (R240..R244)
        CheckLoweredParity(std::array<Instruction, 2>{{
            {{0x221U | (7U << 12) | (240U << 16) | (242U << 24), 244U, 0U, 0U}},
            kExit,
        }});

        // UR28+ retry loop (high_uniform_registers)
        CheckLoweredParity(std::array<Instruction, 2>{{
            {{0x291U | (7U << 12) | (30U << 16) | (11U << 24), 19U,
              63U | (3U << 11) | (7U << 17) | (1U << 27), 0U}},
            kExit,
        }});

        // Geometry + native_code_address
        CheckLoweredParity(
            std::array<Instruction, 5>{{
                {{0x0004734eU, 0U, 0U, 0U}},
                {{0x00017923U, 0U, 0U, 0U}},
                {{0x00027324U, 0xffU, 0x4000U, 0U}},
                {{0x02037924U, 0U, 0x8000U, 0U}},
                kExit,
            }},
            {.geometry = true, .native_code_address = 0x00000012fffffff0ULL});

        // HMMA tensor disabled, inline, and compact modes
        const std::array<Instruction, 2> hmma{{
            {{0x0004723cU, 0x00000008U, 0U, 0U}},
            kExit,
        }};
        CheckLoweredParity(hmma, {.tensor = false});
        CheckLoweredParity(hmma, {.tensor = true, .compact_tensor = false});
        CheckLoweredParity(hmma, {.tensor = true, .compact_tensor = true});

        // ULDC, I2F, LEA + STG
        CheckLoweredParity(std::array<Instruction, 3>{{
            {{0x00047ab9U, 0x01800800U, 0x00000400U, 0x000fe200U}},
            {{0x00017306U, 0x00000001U, 0x00101400U, 0x020ea200U}},
            kExit,
        }});
        CheckLoweredParity(std::array<Instruction, 3>{{
            {{0x08087211U, 0x00000007U, 0x078ff8ffU, 0x000fc800U}},
            {{0x986U | (7U << 12) | (254U << 24), 0U, 0x0810a900U, 0U}},
            kExit,
        }});
    }

    SECTION("Rejected programs preserve identical error and error_offset") {
        CheckLoweredParity({});
        CheckLoweredParity(std::array{kNop});
        // Out-of-range forward branch
        CheckLoweredParity(std::array<Instruction, 2>{{
            {{0x00007947U, 0x40U, 0x00000000U, 0U}},
            kExit,
        }});
        // Callee overwrites link register
        CheckLoweredParity(std::array<Instruction, 4>{{
            {{0x00097802U, 0x20U, 0x00000f00U, 0U}},
            {{0x00007944U, 0x10U, 0x03c00000U, 0U}},
            kExit,
            {{0x00097802U, 0x01U, 0x00000f00U, 0U}},
        }});
        // Unsupported opcode
        CheckLoweredParity(std::array<Instruction, 2>{{
            {{0x00007fffU, 0U, 0U, 0U}},
            kExit,
        }});
    }

    SECTION("Environment dispatch toggles and EDEN_NX2_LOWER_EXTENDED parity") {
        const std::array<Instruction, 2> uldc{{
            {{0x00047ab9U, 0x01800800U, 0x00000400U, 0x000fe200U}},
            kExit,
        }};
        {
            ScopedEnvVar ext_off("EDEN_NX2_LOWER_EXTENDED", "0");
            CheckLoweredParity(uldc);
        }
        {
            ScopedEnvVar ext_on("EDEN_NX2_LOWER_EXTENDED", "1");
            CheckLoweredParity(uldc);
        }

        const std::array<Instruction, 2> i2f{{
            {{0x00017306U, 0x00000001U, 0x00101400U, 0x020ea200U}},
            kExit,
        }};
        const LoweredProgram expected = LowerToMaxwellCpp(i2f);

        {
            ScopedEnvVar rust_on("EDEN_NX2_RUST_AMPERE", "1");
            ScopedEnvVar verify_off("EDEN_NX2_RUST_AMPERE_VERIFY", nullptr);
            const LoweredProgram dispatched_rust = LowerToMaxwell(i2f);
            REQUIRE_FALSE(
                Shader::Ampere::detail::FindLoweredProgramMismatch(expected, dispatched_rust)
                    .has_value());
        }
        {
            ScopedEnvVar rust_off("EDEN_NX2_RUST_AMPERE", nullptr);
            ScopedEnvVar verify_on("EDEN_NX2_RUST_AMPERE_VERIFY", "1");
            const LoweredProgram dispatched_verify = LowerToMaxwell(i2f);
            REQUIRE_FALSE(
                Shader::Ampere::detail::FindLoweredProgramMismatch(expected, dispatched_verify)
                    .has_value());
        }
    }
}

TEST_CASE("Ampere ProgramCaptureState C++ and Rust FFI parity", "[video_core][ampere]") {
    const std::vector<std::vector<Instruction>> sequences{
        {kExit},
        {kNop, kExit},
        // Forward branch over first EXIT
        {Instruction{{0x00007947U, 16U, 0U, 0U}}, kExit, kExit},
        // CALL + main EXIT + RET
        {Instruction{{0x00007944U, 16U, 0U, 0U}}, kExit,
         Instruction{{0x00007950U, 0xffffffc0U, 0x03c3ffffU, 0U}}},
        // Computed branch (BRX) + self-branch BRA
        {Instruction{{0x00007949U, 0U, 0U, 0U}}, kExit,
         Instruction{{0x00007947U, static_cast<u32>(-16), 0x0003ffffU, 0U}}},
        // Invalid negative branch target
        {Instruction{{0x00007947U, static_cast<u32>(-32), 0x0003ffffU, 0U}}},
        // Invalid unaligned JMP target
        {Instruction{{0x00007945U, 4U, 0U, 0U}}},
        // Static IADD3.X geometry exit-envelope program
        {
            Instruction{{0x0000734eU, 0U, 0U, 0x000fe400U}},
            Instruction{{0x00037802U, 0x70U, 0x00000f00U, 0x000fc800U}},
            Instruction{{0x00007210U, 3U, 0x07f1e0ffU, 0x000fc800U}},
            Instruction{{0x01017210U, 0xffU, 0x007fe4ffU, 0x000fc800U}},
            Instruction{{0x00007357U, 0U, 0U, 0x0001e400U}},
            Instruction{{0x0000894dU, 0U, 0x03800000U, 0x021fea00U}},
            Instruction{{0x0000794dU, 0U, 0x03800000U, 0x003fea00U}},
            Instruction{{0x00ff7324U, 0xffU, 0U, 0x0001e200U}},
            Instruction{{0x00007957U, 0U, 0U, 0x000fe200U}},
            Instruction{{0x0000794dU, 0U, 0x03800000U, 0x001fea00U}},
        },
        // IMAD geometry exit-envelope program
        {
            Instruction{{0x0000734eU, 0U, 0U, 0x000fe400U}},
            Instruction{{0x00037802U, 0x70U, 0x00000f00U, 0x000fc800U}},
            Instruction{{0x00007210U, 3U, 0x07f1e0ffU, 0x000fca00U}},
            Instruction{{0xff017224U, 0xffU, 0x000e0401U, 0x000fc800U}},
            Instruction{{0x00007357U, 0U, 0U, 0x0001e400U}},
            kExit,
            kNop,
            Instruction{{0x00ff7324U, 0xffU, 0U, 0x0001e200U}},
            Instruction{{0x00007957U, 0U, 0U, 0x000fe200U}},
            kExit,
        },
        // Geometry exit-envelope with body CALL + BRX + forward target past handler
        {
            Instruction{{0x0000734eU, 0U, 0U, 0x000fe400U}},
            Instruction{{0x00037802U, 0x90U, 0x00000f00U, 0x000fc800U}},
            Instruction{{0x00007210U, 3U, 0x07f1e0ffU, 0x000fc800U}},
            Instruction{{0x01017210U, 0xffU, 0x007fe4ffU, 0x000fc800U}},
            Instruction{{0x00007357U, 0U, 0U, 0x0001e400U}},
            Instruction{{0x00007944U, 0x10U, 0x03c00000U, 0x000fea00U}},
            Instruction{{0x00007949U, 0U, 0U, 0U}},
            Instruction{{0x08007950U, 0xffffff80U, 0x03c3ffffU, 0x000fea00U}},
            Instruction{{0x00007947U, 0x90U, 0x03800000U, 0U}},
            Instruction{{0x00ff7324U, 0xffU, 0U, 0x0001e200U}},
            Instruction{{0x00007957U, 0U, 0U, 0x000fe200U}},
            kExit,
        },
        // Invalid geometry exit handler destination
        {
            Instruction{{0x0000734eU, 0U, 0U, 0x000fe400U}},
            Instruction{{0x00037802U, 0x40U, 0x00000f00U, 0x000fc800U}},
            Instruction{{0x00007210U, 3U, 0x07f1e0ffU, 0x000fc800U}},
            Instruction{{0x01017210U, 0xffU, 0x007fe4ffU, 0x000fc800U}},
            Instruction{{0x00007357U, 0U, 0U, 0x0001e400U}},
        },
        // Mismatched geometry exit setup form stops at first main EXIT
        {
            Instruction{{0x0000634eU, 0U, 0U, 0x000fe400U}},
            Instruction{{0x00037802U, 0x70U, 0x00000f00U, 0x000fc800U}},
            Instruction{{0x00007210U, 3U, 0x07f1e0ffU, 0x000fc800U}},
            Instruction{{0x01017210U, 0xffU, 0x007fe4ffU, 0x000fc800U}},
            Instruction{{0x00007357U, 0U, 0U, 0x0001e400U}},
            kExit,
        },
    };

    for (const auto& seq : sequences) {
        ProgramCaptureState cpp_cap;
        ProgramCaptureState rust_cap;
        for (u32 idx = 0; idx < static_cast<u32>(seq.size()); ++idx) {
            const auto cpp_res = cpp_cap.ObserveCpp(seq[idx], idx);
            const auto rust_res = rust_cap.ObserveRust(seq[idx], idx);
            REQUIRE(cpp_res == rust_res);
            if (cpp_res != ProgramCaptureState::Result::Continue) {
                break;
            }
        }
    }

    for (const char* mode : {"rust", "verify"}) {
        ScopedEnvVar rust_env("EDEN_NX2_RUST_AMPERE", std::string_view{mode} == "rust" ? "1" : nullptr);
        ScopedEnvVar verify_env("EDEN_NX2_RUST_AMPERE_VERIFY",
                                std::string_view{mode} == "verify" ? "1" : nullptr);
        for (const auto& seq : sequences) {
            ProgramCaptureState cap;
            ProgramCaptureState ref;
            for (u32 idx = 0; idx < static_cast<u32>(seq.size()); ++idx) {
                const auto res = cap.Observe(seq[idx], idx);
                const auto ref_res = ref.ObserveCpp(seq[idx], idx);
                REQUIRE(res == ref_res);
                if (res != ProgramCaptureState::Result::Continue) {
                    break;
                }
            }
        }
    }
}

TEST_CASE("Ampere NativeAddressMap C++ and Rust FFI parity", "[video_core][ampere]") {
    const std::array<Instruction, 3> program{{
        {{0x00017306U, 0x00000001U, 0x00101400U, 0x020ea200U}},
        {{0x00027306U, 0x00000002U, 0x00101400U, 0x020ea200U}},
        kExit,
    }};
    const LoweredProgram lowered = LowerToMaxwellCpp(program);
    REQUIRE(static_cast<bool>(lowered));

    // Emulate header (10 words) + scheduling-padded instructions + trailing pad
    std::vector<u64> mapped(10, 0ULL);
    for (const u64 insn : lowered.instructions) {
        if ((mapped.size() * sizeof(u64)) % 32 == 0) {
            mapped.push_back(0ULL);
        }
        mapped.push_back(insn);
    }
    mapped.push_back(0ULL);

    REQUIRE_FALSE(HasNativeAddressMapCpp(mapped));
    REQUIRE_FALSE(HasNativeAddressMapRust(mapped));
    REQUIRE(NativeProgramAddressCpp(mapped, 0) == std::nullopt);
    REQUIRE(NativeProgramAddressRust(mapped, 0) == std::nullopt);

    AppendNativeAddressMap(mapped, lowered.source_instruction_offsets);
    REQUIRE(HasNativeAddressMapCpp(mapped));
    REQUIRE(HasNativeAddressMapRust(mapped));

    for (u32 addr = 0; addr <= static_cast<u32>(program.size() * 16 + 32); addr += 4) {
        REQUIRE(NativeProgramAddressCpp(mapped, addr) == NativeProgramAddressRust(mapped, addr));
    }

    // Corrupt target (invalid 32-byte alignment) and count (out-of-bounds)
    std::vector<u64> corrupt = mapped;
    corrupt[corrupt.size() - 3] = 0x60ULL;
    REQUIRE(NativeProgramAddressCpp(corrupt, static_cast<u32>((program.size() - 1) * 16)) ==
            NativeProgramAddressRust(corrupt, static_cast<u32>((program.size() - 1) * 16)));

    for (const char* mode : {"rust", "verify"}) {
        ScopedEnvVar rust_env("EDEN_NX2_RUST_AMPERE", std::string_view{mode} == "rust" ? "1" : nullptr);
        ScopedEnvVar verify_env("EDEN_NX2_RUST_AMPERE_VERIFY",
                                std::string_view{mode} == "verify" ? "1" : nullptr);
        REQUIRE(HasNativeAddressMap(mapped));
        REQUIRE(NativeProgramAddress(mapped, 0) == NativeProgramAddressCpp(mapped, 0));
        REQUIRE(NativeProgramAddress(mapped, 16) == NativeProgramAddressCpp(mapped, 16));
        REQUIRE(NativeProgramAddress(mapped, 4) == std::nullopt);
    }
}

TEST_CASE("Ampere NativeProgramKey and Override codec C++ and Rust FFI parity",
          "[video_core][ampere]") {
    const std::array<Instruction, 3> source{{
        {{0x00017306U, 0x00000001U, 0x00101400U, 0x020ea200U}},
        {{0x00047ab9U, 0x01800800U, 0x00000400U, 0x000fe200U}},
        kExit,
    }};

    REQUIRE(NativeProgramKeyCpp({}) == NativeProgramKeyRust({}));
    REQUIRE(NativeProgramKeyCpp(source) == NativeProgramKeyRust(source));

    const LoweredProgram lowered = LowerToMaxwellCpp(source);
    REQUIRE(static_cast<bool>(lowered));

    const std::vector<u8> cpp_encoded = EncodeProgramOverrideCpp(source, lowered);
    const std::vector<u8> rust_encoded = EncodeProgramOverrideRust(source, lowered);
    REQUIRE_FALSE(cpp_encoded.empty());
    REQUIRE(cpp_encoded == rust_encoded);

    // Invalid LoweredProgram encodes to empty in both C++ and Rust
    LoweredProgram invalid;
    invalid.error = "failed";
    REQUIRE(EncodeProgramOverrideCpp(source, invalid).empty());
    REQUIRE(EncodeProgramOverrideRust(source, invalid).empty());

    REQUIRE(ProgramOverrideSourceSizeCpp(cpp_encoded) == sizeof(source));
    REQUIRE(ProgramOverrideSourceSizeRust(cpp_encoded) == sizeof(source));

    const LoweredProgram cpp_decoded = DecodeProgramOverrideCpp(cpp_encoded, source);
    const LoweredProgram rust_decoded = DecodeProgramOverrideRust(cpp_encoded, source);
    REQUIRE(static_cast<bool>(cpp_decoded));
    REQUIRE_FALSE(
        Shader::Ampere::detail::FindLoweredProgramMismatch(cpp_decoded, rust_decoded).has_value());

    // Malformed override buffers must reject identically
    std::vector<std::vector<u8>> malformed{
        {},
        {1, 2, 3, 4},
        cpp_encoded,
    };
    malformed.back().pop_back();
    if (cpp_encoded.size() > 28) {
        auto bad_magic = cpp_encoded;
        bad_magic[0] ^= 1U;
        malformed.push_back(bad_magic);
        auto bad_src = cpp_encoded;
        bad_src[24] ^= 0xffU;
        malformed.push_back(bad_src);
    }

    for (const auto& buf : malformed) {
        REQUIRE(ProgramOverrideSourceSizeCpp(buf) == ProgramOverrideSourceSizeRust(buf));
        const LoweredProgram cpp_err = DecodeProgramOverrideCpp(buf, source);
        const LoweredProgram rust_err = DecodeProgramOverrideRust(buf, source);
        REQUIRE_FALSE(
            Shader::Ampere::detail::FindLoweredProgramMismatch(cpp_err, rust_err).has_value());
    }

    for (const char* mode : {"rust", "verify"}) {
        ScopedEnvVar rust_env("EDEN_NX2_RUST_AMPERE", std::string_view{mode} == "rust" ? "1" : nullptr);
        ScopedEnvVar verify_env("EDEN_NX2_RUST_AMPERE_VERIFY",
                                std::string_view{mode} == "verify" ? "1" : nullptr);
        REQUIRE(NativeProgramKey(source) == NativeProgramKeyCpp(source));
        const auto encoded = EncodeProgramOverride(source, lowered);
        REQUIRE(encoded == cpp_encoded);
        REQUIRE(ProgramOverrideSourceSize(encoded) == sizeof(source));
        const auto decoded = DecodeProgramOverride(encoded, source);
        REQUIRE_FALSE(
            Shader::Ampere::detail::FindLoweredProgramMismatch(cpp_decoded, decoded).has_value());
    }

    // FFI safety: idempotent cleanup & null pointers
    EglAmpereLoweredProgram raw_lowered{};
    egl_ampere_free_lowered(&raw_lowered);
    egl_ampere_free_lowered(&raw_lowered);
    egl_ampere_free_lowered(nullptr);

    EglAmpereBytes raw_bytes{};
    egl_ampere_free_bytes(&raw_bytes);
    egl_ampere_free_bytes(&raw_bytes);
    egl_ampere_free_bytes(nullptr);
}

#endif // EGLANTINE_ENABLE_RUST_AMPERE
