// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

enum class TemporalUpscalerBackend : u32 {
    None,
    StandIn,
    Fsr3,
    XeSS,
    Dlss,
};

struct TemporalUpscaleImage {
    VkImageView view{VK_NULL_HANDLE};
    VkImage image{VK_NULL_HANDLE};
    VkFormat format{VK_FORMAT_UNDEFINED};
    VkImageSubresourceRange range{};     ///< The view's level and layer, with the view's aspect.
    VkImageAspectFlags barrier_aspect{}; ///< Every aspect of the image's format.
};

struct TemporalUpscaleInputs {
    TemporalUpscaleImage color;          ///< Jittered linear colour at the render size.
    TemporalUpscaleImage depth;          ///< Device depth at the render size.
    TemporalUpscaleImage motion_vectors; ///< Motion to the previous frame at the render size.
    VkExtent2D render_size{};
    VkExtent2D upscale_size{};
    /// This frame's sample offset in render pixels, as the game hands its own DLSS.
    std::array<f32, 2> jitter{};
    /// Turns a motion vector into render pixels (the render size for UV offsets).
    std::array<f32, 2> motion_vector_scale{1.0f, 1.0f};
    f32 frame_time_ms{1000.0f / 30.0f};
    bool reset{};
    /// Reversed depth (1 near, 0 far), as Fire Emblem draws it.
    bool depth_inverted{true};
    /// Linear colour that goes above 1; false for tonemapped colour.
    bool hdr{true};
    bool sharpen{};
    /// 0 to 1 for RCAS-based backends (2 stops at 0, none at 1).
    f32 sharpness{1.0f};
};

/// Unified interface implemented by host temporal upscalers (DLSS, XeSS, FSR 3.1).
class ITemporalUpscaler {
public:
    virtual ~ITemporalUpscaler() = default;

    [[nodiscard]] virtual TemporalUpscalerBackend Backend() const noexcept = 0;

    /// Records one evaluation into OutputImage() (RGBA16F at the upscale size, GENERAL layout).
    /// Returns false when the backend cannot run for these inputs so the caller can fall back.
    [[nodiscard]] virtual bool Upscale(const TemporalUpscaleInputs& inputs) = 0;

    [[nodiscard]] virtual VkImage OutputImage() const noexcept = 0;
    [[nodiscard]] virtual VkImageView OutputView() const noexcept = 0;
    [[nodiscard]] virtual VkSampler PointSampler() const noexcept = 0;
};

} // namespace Vulkan
