// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <memory>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Common {
class DynamicLibrary;
}

namespace Vulkan {

class Device;
class MemoryAllocator;
class Scheduler;

/// Intel XeSS 2.x Vulkan upscaler loaded dynamically from libxess.dll / libxess.so at runtime
/// with zero build-time binary dependencies.
class XeSSUpscaler final : public ITemporalUpscaler {
public:
    using Image = TemporalUpscaleImage;
    using Inputs = TemporalUpscaleInputs;

    /// Throws vk::Exception when libxess Vulkan runtime cannot be initialized for this device.
    explicit XeSSUpscaler(const Device& device, Scheduler& scheduler,
                          MemoryAllocator& memory_allocator);
    ~XeSSUpscaler() override;

    [[nodiscard]] static bool IsSupported(const Device& device);

    [[nodiscard]] TemporalUpscalerBackend Backend() const noexcept override {
        return TemporalUpscalerBackend::XeSS;
    }

    bool Upscale(const Inputs& inputs) override;

    [[nodiscard]] VkImage OutputImage() const noexcept override {
        return *output_image;
    }
    [[nodiscard]] VkImageView OutputView() const noexcept override {
        return *output_view;
    }
    [[nodiscard]] VkSampler PointSampler() const noexcept override {
        return *point_sampler;
    }

private:
    struct RuntimeApi;

    void CreateOutput(VkExtent2D upscale_size);
    bool EnsureInitialized(const Inputs& inputs);

    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& memory_allocator;
    std::unique_ptr<Common::DynamicLibrary> library;
    std::unique_ptr<RuntimeApi> api;
    void* context{};
    VkExtent2D render_extent{};
    VkExtent2D upscale_extent{};
    u32 current_quality{};
    bool depth_inverted{true};
    bool hdr{true};
    bool initialized{};
    vk::Image output_image;
    vk::ImageView output_view;
    vk::Sampler point_sampler;
    std::atomic<bool> failed{};
};

} // namespace Vulkan
