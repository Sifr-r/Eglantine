// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Vulkan {

enum class DrawSkipDecision : u8 {
    Wait,
    Skip,
    SkipStaleTarget,
};

struct DrawSkipInputs {
    bool use_asynchronous_shaders{};
    bool is_mesh{};
    bool native_turing{};
    bool native_async{true};
    bool native_redraw{true};
    bool is_switch2{};
    bool zeta_enable{};
    bool depth_test_enable{};
    u32 active_color_target_count{};
    u32 draw_element_count{};
    bool stale_target_skip{};
    bool color_target_redrawn_every_frame{};
    bool color_target_written_before_this_frame{};
    bool color_target_has_prior_frame_output{};
};

/// Single source of truth for whether a draw may be skipped while its graphics pipeline is
/// translated, built, deferred by build budget, or replayed from disk.
[[nodiscard]] constexpr DrawSkipDecision EvaluateDrawSkip(const DrawSkipInputs& inputs) noexcept {
    if (!inputs.use_asynchronous_shaders || inputs.is_mesh) {
        return DrawSkipDecision::Wait;
    }
    if (inputs.native_turing) {
        const bool target_redrawn = inputs.native_redraw
                                        ? inputs.color_target_redrawn_every_frame
                                        : inputs.color_target_written_before_this_frame;
        if (!inputs.native_async || !inputs.zeta_enable || !inputs.depth_test_enable ||
            inputs.active_color_target_count == 0 || !target_redrawn) {
            return DrawSkipDecision::Wait;
        }
        return DrawSkipDecision::Skip;
    }
    if (inputs.zeta_enable) {
        return DrawSkipDecision::Skip;
    }
    const bool is_small_draw = inputs.stale_target_skip
                                   ? (inputs.draw_element_count > 0 && inputs.draw_element_count <= 6)
                                   : (inputs.draw_element_count <= 6);
    if (is_small_draw) {
        return (inputs.stale_target_skip && inputs.color_target_has_prior_frame_output)
                   ? DrawSkipDecision::SkipStaleTarget
                   : DrawSkipDecision::Wait;
    }
    if (inputs.is_switch2) {
        return DrawSkipDecision::Wait;
    }
    return DrawSkipDecision::Skip;
}

} // namespace Vulkan
