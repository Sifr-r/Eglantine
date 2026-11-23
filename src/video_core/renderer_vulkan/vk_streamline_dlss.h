// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVIDIA DLSS (through NVIDIA Streamline) in place of a game's DLSS
// evaluation. NVIDIA's DLLs come from the user's PC; see vulkan_common/streamline.h.

#pragma once

#include <array>
#include <atomic>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class MemoryAllocator;
class Scheduler;

class StreamlineDlssUpscaler final : public ITemporalUpscaler {
public:
    using Image = TemporalUpscaleImage;
    using Inputs = TemporalUpscaleInputs;

    /// Throws vk::Exception when Streamline has no DLSS for this device.
    explicit StreamlineDlssUpscaler(const Device& device, Scheduler& scheduler,
                                    MemoryAllocator& memory_allocator);
    ~StreamlineDlssUpscaler() override;

    [[nodiscard]] TemporalUpscalerBackend Backend() const noexcept override {
        return TemporalUpscalerBackend::Dlss;
    }

    /// Records one evaluation into the output image (RGBA16F at the upscale size, GENERAL
    /// layout). Returns false when DLSS can't run, so the caller can fall back to XeSS / FSR 3.1.
    bool Upscale(const Inputs& inputs) override;

    [[nodiscard]] VkImage OutputImage() const noexcept override {
        return *output_image;
    }
    [[nodiscard]] VkImageView OutputView() const noexcept override {
        return *output_view;
    }
    /// Nearest-neighbour clamp sampler (normalized coordinates), for reading the output.
    [[nodiscard]] VkSampler PointSampler() const noexcept override {
        return *point_sampler;
    }

private:
    void CreateOutput(VkExtent2D upscale_size);

    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& memory_allocator;
    void* set_options{};          ///< slDLSSSetOptions
    void* get_optimal_settings{}; ///< slDLSSGetOptimalSettings
    VkExtent2D render_extent{};
    VkExtent2D upscale_extent{};
    u32 frame_index{};
    u32 current_mode{};
    bool hdr{true}; ///< The colour kind the options were last set for.
    vk::Image output_image;
    vk::ImageView output_view;
    vk::Sampler point_sampler;
    std::atomic<bool> failed{};
};

} // namespace Vulkan
