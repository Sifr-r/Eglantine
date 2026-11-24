// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon::ShaderWarm {

/// Precache file records: compute entries in the disk pipeline cache format, with no recorded
/// reads (the emulator guesses them, see GuessMissing).

/// Precache files use the Vulkan pipeline cache version: the emulator loads one only when it
/// matches, so the two must move together (vk_pipeline_cache.cpp checks it).
constexpr u32 WarmCacheVersion = 36;

/// What the builder finds in the game files, recorded in the precache's count file. A file built by
/// an older builder is rebuilt at boot (RefreshWarmFiles). 2: Mario Kart World's .esetb effects.
/// 3: .ptcl particle effects (Fire Emblem). 4: SM86 blobs with discovery-hash keys.
constexpr u32 WarmBuilderVersion = 4;

using ComputeKey = std::array<u8, 24>;

/// A compute program captured for precache serialization (SM86 blob + discovery hash).
struct LoweredCode {
    std::vector<u8> code;
    /// Discovery hash (ComputeNativeDiscoveryHash) when captured as an SM86 blob, 0 otherwise.
    u64 native_hash{};
    /// Constant buffer the program's bound textures read, ~0U when it names none.
    u32 texture_buffer{~0U};
};

/// Captures one extracted compute program as a runtime ComputeEnvironment does and packs its SM86
/// blob and discovery hash without invoking LowerToMaxwell. Returns error text on invalid input.
std::optional<std::string> LowerProgram(std::span<const u8> program, LoweredCode& out);

/// The pipeline cache key hash of `code` (ComputeNativeDiscoveryHash for SM86 blobs,
/// NativeShaderHash for legacy lowered words).
u64 StageHash(std::span<const u8> code);

struct ComputeControl {
    std::array<u32, 3> workgroup{};
    u32 shared_memory{};
    u32 local_memory{};
};

/// Workgroup (control bytes 88, 92, 96), shared memory (byte 100 rounded up to 256) and local
/// memory (byte 104). Empty when the control is too short.
std::optional<ComputeControl> ReadComputeControl(std::span<const u8> control);

/// One compute entry (env count, env, code, compute trailer, key); `key` gets the 24-byte key.
std::vector<u8> ComputeEntry(const LoweredCode& lowered, const ComputeControl& control,
                             ComputeKey& key);

} // namespace VideoCommon::ShaderWarm
