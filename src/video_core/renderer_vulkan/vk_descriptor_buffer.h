// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

#include "common/alignment.h"
#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

/// Parses EDEN_NX2_DRAW_CACHE (default on; '0' disables).
[[nodiscard]] bool ParseDrawCacheEnvValue(const char* value) noexcept;

/// True unless EDEN_NX2_DRAW_CACHE=0.
[[nodiscard]] bool IsDrawCacheEnabled() noexcept;

struct DescriptorSliceKey {
    static constexpr size_t NUM_STAGES = 5;

    u64 layout_hash{};
    std::array<u64, NUM_STAGES> stage_hashes{};
    u64 payload_hash{};
    u32 entry_count{};
    VkDeviceSize size{};

    [[nodiscard]] bool operator==(const DescriptorSliceKey&) const noexcept = default;
};

struct DescriptorSliceHit {
    VkDeviceSize offset{};
    u32 chunk{};
    u64 generation{};
};

/// Generation-scoped cache mapping per-stage descriptor signatures to already-written descriptor
/// buffer slices within the active frame / ring generation.
class DescriptorSliceCache final {
    static constexpr size_t NUM_SLOTS = 2048;
    static constexpr size_t SLOT_MASK = NUM_SLOTS - 1;

public:
    explicit DescriptorSliceCache(bool enabled_ = IsDrawCacheEnabled()) noexcept
        : enabled{enabled_} {}

    void SetEnabled(bool enabled_) noexcept {
        enabled = enabled_;
        if (!enabled) {
            Clear();
        }
    }

    [[nodiscard]] bool IsEnabled() const noexcept {
        return enabled;
    }

    [[nodiscard]] std::optional<DescriptorSliceHit> Lookup(const DescriptorSliceKey& key,
                                                           u64 generation) const noexcept;

    void Insert(const DescriptorSliceKey& key, VkDeviceSize offset, u32 chunk,
                u64 generation) noexcept;

    void Clear() noexcept;

private:
    struct Entry {
        DescriptorSliceKey key{};
        VkDeviceSize offset{};
        u32 chunk{};
        u64 generation{};
        bool valid{false};
    };

    static size_t SlotFor(const DescriptorSliceKey& key) noexcept;

    std::vector<Entry> slots{NUM_SLOTS};
    bool enabled{true};
};

/// Tracks command-buffer descriptor buffer & offset binding state across draws so redundant
/// vkCmdBindDescriptorBuffersEXT and vkCmdSetDescriptorBufferOffsetsEXT commands are skipped
/// when the bound chunk, offset, pipeline layout, generation, and command tick match.
class DescriptorBindCache final {
public:
    struct BindDecision {
        bool bind_buffer{false};
        bool set_offsets{false};
    };

    explicit DescriptorBindCache(bool enabled_ = IsDrawCacheEnabled()) noexcept : enabled{enabled_} {}

    void SetEnabled(bool enabled_) noexcept {
        enabled = enabled_;
        if (!enabled) {
            InvalidateAll();
        }
    }

    [[nodiscard]] bool IsEnabled() const noexcept {
        return enabled;
    }

    [[nodiscard]] BindDecision AcquireGraphicsBinding(u32 chunk, VkDeviceSize offset,
                                                      VkPipelineLayout layout, u64 generation,
                                                      u64 tick,
                                                      bool scheduler_chunk_dirty) noexcept;

    void InvalidateOffsets() noexcept {
        offsets_bound = false;
    }

    void InvalidateAll() noexcept {
        buffer_bound = false;
        offsets_bound = false;
        bound_layout = VK_NULL_HANDLE;
    }

private:
    bool enabled{true};
    bool buffer_bound{false};
    bool offsets_bound{false};
    u32 bound_chunk{};
    VkDeviceSize bound_offset{};
    VkPipelineLayout bound_layout{VK_NULL_HANDLE};
    u64 bound_generation{};
    u64 bound_tick{};
};

/// Computes a fast 64-bit hash over `byte_count` descriptor payload bytes.
[[nodiscard]] u64 HashDescriptorBytes(const void* data, size_t byte_count) noexcept;

class DescriptorBufferRing final {
    static constexpr size_t FRAMES_IN_FLIGHT = 8;
    static constexpr VkDeviceSize TILER_FRAME_SIZE = 2 * 1024 * 1024;
    static constexpr VkDeviceSize DESKTOP_FRAME_SIZE = 4 * 1024 * 1024;

public:
    explicit DescriptorBufferRing(const Device& device_, MemoryAllocator& memory_allocator);
    ~DescriptorBufferRing();

    struct Allocation {
        u8* host{};
        VkDeviceSize offset{};
        u32 chunk{};
        u64 generation{};
    };

    [[nodiscard]] u64 CurrentGeneration() const noexcept {
        return generation;
    }

    void TouchFrame(Scheduler& scheduler);

    [[nodiscard]] bool CanAllocate(VkDeviceSize size) const noexcept {
        return Common::AlignUp(size, alignment) <= chunk_capacity;
    }

    void TickFrame();

    [[nodiscard]] Allocation Allocate(Scheduler& scheduler, VkDeviceSize size);

    [[nodiscard]] VkDescriptorBufferBindingInfoEXT BindingInfo(u32 chunk) const noexcept {
        return VkDescriptorBufferBindingInfoEXT{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,
            .pNext = nullptr,
            .address = chunk_addresses[chunk],
            .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                     VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT,
        };
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return !chunks.empty();
    }

    [[nodiscard]] bool IsDrawCacheEnabled() const noexcept {
        return draw_bind_cache.IsEnabled();
    }

    [[nodiscard]] std::optional<DescriptorSliceHit> LookupSlice(
        const DescriptorSliceKey& key) const noexcept {
        return slice_cache.Lookup(key, generation);
    }

    void CacheSlice(const DescriptorSliceKey& key, VkDeviceSize offset, u32 chunk) noexcept {
        slice_cache.Insert(key, offset, chunk, generation);
    }

    [[nodiscard]] DescriptorBindCache::BindDecision AcquireGraphicsBind(
        u32 chunk, VkDeviceSize offset, VkPipelineLayout layout, u64 tick,
        bool scheduler_chunk_dirty) noexcept {
        return draw_bind_cache.AcquireGraphicsBinding(chunk, offset, layout, generation, tick,
                                                        scheduler_chunk_dirty);
    }

    void InvalidateDrawBindCache() noexcept {
        draw_bind_cache.InvalidateAll();
    }

    void InvalidateDrawOffsetCache() noexcept {
        draw_bind_cache.InvalidateOffsets();
    }

private:
    const Device& device;
    std::vector<vk::Buffer> chunks;
    std::vector<VkDeviceAddress> chunk_addresses;
    std::vector<u8*> chunk_hosts;
    VkDeviceSize alignment{1};
    VkDeviceSize chunk_capacity{};
    size_t chunks_per_frame{};
    size_t frame_index{};
    size_t chunk_cursor{};
    VkDeviceSize cursor{};
    u64 generation{1};
    std::array<u64, FRAMES_IN_FLIGHT> frame_ticks{};
    bool frame_reused{};
    DescriptorSliceCache slice_cache;
    DescriptorBindCache draw_bind_cache;
};

} // namespace Vulkan
