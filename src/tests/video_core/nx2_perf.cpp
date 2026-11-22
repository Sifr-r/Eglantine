// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/nx2_perf.h"

namespace {

using namespace VideoCommon::Nx2Perf;

constexpr size_t Index(Opt opt) {
    return static_cast<size_t>(opt);
}

} // namespace

TEST_CASE("Nx2Perf default mask keeps every switch on", "[nx2_perf]") {
    REQUIRE(ParseOffMask(false, "", "") == u64{0});
}

TEST_CASE("Nx2Perf bank_once_per_frame is on by default and off under SAFE", "[nx2_perf]") {
    REQUIRE(opt_names[Index(Opt::BankOncePerFrame)] == "bank_once_per_frame");
    REQUIRE(!IsOff(ParseOffMask(false, "", ""), Index(Opt::BankOncePerFrame)));
    REQUIRE(IsOff(ParseOffMask(true, "", ""), Index(Opt::BankOncePerFrame)));
    REQUIRE(ParseOffMask(false, "", "bank_once_per_frame") == Bit(Opt::BankOncePerFrame));
}

TEST_CASE("Nx2Perf OFF=bind_memo flips only its bit", "[nx2_perf]") {
    const auto mask = ParseOffMask(false, "", "bind_memo");
    REQUIRE(mask == Bit(Opt::BindMemo));
    REQUIRE(!IsOff(mask, Index(Opt::IndexRebind)));
}

TEST_CASE("Nx2Perf SAFE is the default-off set", "[nx2_perf]") {
    const auto mask = ParseOffMask(true, "", "");
    REQUIRE(mask == DefaultOffMask());
    for (size_t i = Index(Opt::Bounds); i <= Index(Opt::IndexRebind); ++i) {
        REQUIRE(!IsOff(mask, i));
    }
    for (size_t i = Index(Opt::DownloadBatch); i <= Index(Opt::BindMemo); ++i) {
        REQUIRE(IsOff(mask, i));
    }
}

TEST_CASE("Nx2Perf lists of names", "[nx2_perf]") {
    REQUIRE(ParseOffMask(false, "", "bounds,draw_preamble") ==
            (Bit(Opt::Bounds) | Bit(Opt::DrawPreamble)));
    REQUIRE(ParseOffMask(true, "front_run,download_batch", "") ==
            (DefaultOffMask() & ~(Bit(Opt::FrontRun) | Bit(Opt::DownloadBatch))));
}

TEST_CASE("compute_rebuild_async: on by default", "[nx2_perf]") {
    REQUIRE(opt_names[Index(Opt::ComputeRebuildAsync)] == "compute_rebuild_async");
    REQUIRE(!IsOff(DefaultOffMask(), Index(Opt::ComputeRebuildAsync)));
    REQUIRE(!IsOff(ParseOffMask(false, "", ""), Index(Opt::ComputeRebuildAsync)));
    REQUIRE(ParseOffMask(false, "", "compute_rebuild_async") == Bit(Opt::ComputeRebuildAsync));
}

TEST_CASE("compute_preload_variants: on by default", "[nx2_perf]") {
    REQUIRE(opt_names[Index(Opt::ComputePreloadVariants)] == "compute_preload_variants");
    REQUIRE(!IsOff(DefaultOffMask(), Index(Opt::ComputePreloadVariants)));
    REQUIRE(!IsOff(ParseOffMask(false, "", ""), Index(Opt::ComputePreloadVariants)));
    REQUIRE(ParseOffMask(false, "", "compute_preload_variants") ==
            Bit(Opt::ComputePreloadVariants));
}

TEST_CASE("draw-skipping and replay opts are on by default and off under SAFE", "[nx2_perf]") {
    const u64 default_mask = ParseOffMask(false, "", "");
    const u64 safe_mask = ParseOffMask(true, "", "");
    REQUIRE(safe_mask == DefaultOffMask());

    struct NamedOpt {
        Opt opt;
        const char* name;
    };
    constexpr std::array<NamedOpt, 5> draw_skip_opts{{
        {Opt::ShaderBuildBudget, "shader_build_budget"},
        {Opt::StaleTargetSkip, "stale_target_skip"},
        {Opt::GeometryConsistency, "geometry_consistency"},
        {Opt::BuildOrderPriority, "build_order_priority"},
        {Opt::ReplayDiskRecordings, "replay_disk_recordings"},
    }};

    for (const auto& [opt, name] : draw_skip_opts) {
        REQUIRE(opt_names[Index(opt)] == name);
        REQUIRE_FALSE(IsOff(default_mask, Index(opt)));
        REQUIRE(IsOff(DefaultOffMask(), Index(opt)));
        REQUIRE(IsOff(safe_mask, Index(opt)));
        REQUIRE_FALSE(IsOff(ParseOffMask(true, name, ""), Index(opt)));
        REQUIRE(ParseOffMask(false, "", name) == Bit(opt));
    }
}
