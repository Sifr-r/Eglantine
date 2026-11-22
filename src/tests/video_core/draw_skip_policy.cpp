// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/draw_skip_policy.h"

namespace {

using Vulkan::DrawSkipDecision;
using Vulkan::DrawSkipInputs;
using Vulkan::EvaluateDrawSkip;

DrawSkipInputs MakeNativeRedrawnDepthColor() {
    return DrawSkipInputs{
        .use_asynchronous_shaders = true,
        .is_mesh = false,
        .native_turing = true,
        .native_async = true,
        .native_redraw = true,
        .is_switch2 = true,
        .zeta_enable = true,
        .depth_test_enable = true,
        .active_color_target_count = 1,
        .draw_element_count = 128,
        .stale_target_skip = true,
        .color_target_redrawn_every_frame = true,
        .color_target_written_before_this_frame = true,
        .color_target_has_prior_frame_output = true,
    };
}

} // namespace

TEST_CASE("EvaluateDrawSkip: waits when async shaders are disabled",
          "[draw_skip_policy][video_core]") {
    DrawSkipInputs inputs = MakeNativeRedrawnDepthColor();
    inputs.use_asynchronous_shaders = false;
    CHECK(EvaluateDrawSkip(inputs) == DrawSkipDecision::Wait);

    inputs.native_turing = false;
    inputs.is_switch2 = false;
    CHECK(EvaluateDrawSkip(inputs) == DrawSkipDecision::Wait);
}

TEST_CASE("EvaluateDrawSkip: never skips mesh draws", "[draw_skip_policy][video_core]") {
    DrawSkipInputs inputs = MakeNativeRedrawnDepthColor();
    inputs.is_mesh = true;
    CHECK(EvaluateDrawSkip(inputs) == DrawSkipDecision::Wait);

    inputs.native_turing = false;
    CHECK(EvaluateDrawSkip(inputs) == DrawSkipDecision::Wait);

    inputs.zeta_enable = false;
    CHECK(EvaluateDrawSkip(inputs) == DrawSkipDecision::Wait);
}

TEST_CASE("EvaluateDrawSkip: native Turing depth-tested color draws into redrawn targets",
          "[draw_skip_policy][video_core]") {
    const DrawSkipInputs base = MakeNativeRedrawnDepthColor();
    CHECK(EvaluateDrawSkip(base) == DrawSkipDecision::Skip);

    {
        DrawSkipInputs inputs = base;
        inputs.native_async = false;
        CHECK(EvaluateDrawSkip(inputs) == DrawSkipDecision::Wait);
    }
    {
        DrawSkipInputs inputs = base;
        inputs.zeta_enable = false;
        CHECK(EvaluateDrawSkip(inputs) == DrawSkipDecision::Wait);

        inputs.draw_element_count = 3;
        CHECK(EvaluateDrawSkip(inputs) == DrawSkipDecision::Wait);
    }
    {
        DrawSkipInputs inputs = base;
        inputs.depth_test_enable = false;
        CHECK(EvaluateDrawSkip(inputs) == DrawSkipDecision::Wait);
    }
    {
        DrawSkipInputs inputs = base;
        inputs.active_color_target_count = 0;
        CHECK(EvaluateDrawSkip(inputs) == DrawSkipDecision::Wait);
    }
    {
        DrawSkipInputs inputs = base;
        inputs.native_redraw = true;
        inputs.color_target_redrawn_every_frame = false;
        inputs.color_target_written_before_this_frame = true;
        CHECK(EvaluateDrawSkip(inputs) == DrawSkipDecision::Wait);
    }
    {
        DrawSkipInputs inputs = base;
        inputs.native_redraw = false;
        inputs.color_target_redrawn_every_frame = true;
        inputs.color_target_written_before_this_frame = false;
        CHECK(EvaluateDrawSkip(inputs) == DrawSkipDecision::Wait);

        inputs.color_target_written_before_this_frame = true;
        inputs.color_target_redrawn_every_frame = false;
        CHECK(EvaluateDrawSkip(inputs) == DrawSkipDecision::Skip);
    }
}

TEST_CASE("EvaluateDrawSkip: non-native depth, small, indirect, and Switch 2 font-atlas draws",
          "[draw_skip_policy][video_core]") {
    DrawSkipInputs base{
        .use_asynchronous_shaders = true,
        .is_mesh = false,
        .native_turing = false,
        .is_switch2 = false,
        .zeta_enable = true,
        .depth_test_enable = false,
        .active_color_target_count = 0,
        .draw_element_count = 36,
        .stale_target_skip = true,
        .color_target_has_prior_frame_output = false,
    };
    CHECK(EvaluateDrawSkip(base) == DrawSkipDecision::Skip);
    base.is_switch2 = true;
    CHECK(EvaluateDrawSkip(base) == DrawSkipDecision::Skip);

    // Small draws without depth (<= 6 elements) wait unless stale_target_skip has a recent output.
    base.zeta_enable = false;
    for (const u32 count : {u32{3}, u32{6}}) {
        DrawSkipInputs small = base;
        small.draw_element_count = count;
        small.stale_target_skip = false;
        small.color_target_has_prior_frame_output = true;
        CHECK(EvaluateDrawSkip(small) == DrawSkipDecision::Wait);

        small.stale_target_skip = true;
        small.color_target_has_prior_frame_output = false;
        CHECK(EvaluateDrawSkip(small) == DrawSkipDecision::Wait);

        small.color_target_has_prior_frame_output = true;
        CHECK(EvaluateDrawSkip(small) == DrawSkipDecision::SkipStaleTarget);
    }

    // Indirect draws (0 elements): treated as small when stale_target_skip is off, not small when
    // stale_target_skip is on.
    {
        DrawSkipInputs indirect = base;
        indirect.draw_element_count = 0;
        indirect.stale_target_skip = false;
        CHECK(EvaluateDrawSkip(indirect) == DrawSkipDecision::Wait);

        indirect.stale_target_skip = true;
        indirect.is_switch2 = false;
        CHECK(EvaluateDrawSkip(indirect) == DrawSkipDecision::Skip);

        indirect.is_switch2 = true;
        CHECK(EvaluateDrawSkip(indirect) == DrawSkipDecision::Wait);
    }

    // Larger non-depth draws (e.g., font glyph atlases): Switch 1 skips, Switch 2 waits.
    {
        DrawSkipInputs large = base;
        large.draw_element_count = 12;
        large.stale_target_skip = true;
        large.color_target_has_prior_frame_output = true;

        large.is_switch2 = false;
        CHECK(EvaluateDrawSkip(large) == DrawSkipDecision::Skip);

        large.is_switch2 = true;
        CHECK(EvaluateDrawSkip(large) == DrawSkipDecision::Wait);
    }
}
