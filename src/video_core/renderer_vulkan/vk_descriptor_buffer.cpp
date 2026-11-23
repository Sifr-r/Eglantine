// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

namespace {

[[nodiscard]] u64 HashMix64(u64 x) noexcept {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

} // namespace

bool ParseDrawCacheEnvValue(const char* value) noexcept {
    return value == nullptr || value[0] != '0';
}

bool IsDrawCacheEnabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_DRAW_CACHE");
        return ParseDrawCacheEnvValue(value);
    }();
    return enabled;
}

u64 HashDescriptorBytes(const void* data, size_t byte_count) noexcept {
    if (data == nullptr || byte_count == 0) {
        return 0;
    }
    const auto* bytes = static_cast<const u8*>(data);
    u64 hash = 0xcbf29ce484222325ULL ^ static_cast<u64>(byte_count);
    size_t offset = 0;
    while (offset + sizeof(u64) <= byte_count) {
        u64 word{};
        std::memcpy(&word, bytes + offset, sizeof(word));
        hash = HashMix64(hash ^ word);
        offset += sizeof(u64);
    }
    if (offset < byte_count) {
        u64 tail{};
        std::memcpy(&tail, bytes + offset, byte_count - offset);
        hash = HashMix64(hash ^ tail);
    }
    return hash;
}

size_t DescriptorSliceCache::SlotFor(const DescriptorSliceKey& key) noexcept {
    u64 h = HashMix64(key.layout_hash ^ key.payload_hash);
    h = HashMix64(h ^ (static_cast<u64>(key.entry_count) << 32) ^ static_cast<u64>(key.size));
    for (const u64 stage_hash : key.stage_hashes) {
        h = HashMix64(h ^ stage_hash);
    }
    return static_cast<size_t>(h) & SLOT_MASK;
}

std::optional<DescriptorSliceHit> DescriptorSliceCache::Lookup(const DescriptorSliceKey& key,
                                                               u64 generation) const noexcept {
    if (!enabled) {
        return std::nullopt;
    }
    const Entry& entry = slots[SlotFor(key)];
    if (!entry.valid || entry.generation != generation || entry.key != key) {
        return std::nullopt;
    }
    return DescriptorSliceHit{
        .offset = entry.offset,
        .chunk = entry.chunk,
        .generation = entry.generation,
    };
}

void DescriptorSliceCache::Insert(const DescriptorSliceKey& key, VkDeviceSize offset, u32 chunk,
                                  u64 generation) noexcept {
    if (!enabled) {
        return;
    }
    Entry& entry = slots[SlotFor(key)];
    entry.key = key;
    entry.offset = offset;
    entry.chunk = chunk;
    entry.generation = generation;
    entry.valid = true;
}

void DescriptorSliceCache::Clear() noexcept {
    for (Entry& entry : slots) {
        entry.valid = false;
    }
}

DescriptorBindCache::BindDecision DescriptorBindCache::AcquireGraphicsBinding(
    u32 chunk, VkDeviceSize offset, VkPipelineLayout layout, u64 generation, u64 tick,
    bool scheduler_chunk_dirty) noexcept {
    if (!enabled) {
        buffer_bound = true;
        offsets_bound = true;
        bound_chunk = chunk;
        bound_offset = offset;
        bound_layout = layout;
        bound_generation = generation;
        bound_tick = tick;
        return BindDecision{.bind_buffer = true, .set_offsets = true};
    }

    const bool need_bind_buffer = scheduler_chunk_dirty || !buffer_bound || bound_chunk != chunk ||
                                  bound_generation != generation || bound_tick != tick;
    const bool need_set_offsets =
        need_bind_buffer || !offsets_bound || bound_offset != offset || bound_layout != layout;

    buffer_bound = true;
    offsets_bound = true;
    bound_chunk = chunk;
    bound_offset = offset;
    bound_layout = layout;
    bound_generation = generation;
    bound_tick = tick;

    return BindDecision{
        .bind_buffer = need_bind_buffer,
        .set_offsets = need_set_offsets,
    };
}

DescriptorBufferRing::DescriptorBufferRing(const Device& device_,
                                           MemoryAllocator& memory_allocator)
    : device{device_} {
    if (!device.IsExtDescriptorBufferSupported() || !device.IsBufferDeviceAddressSupported()) {
        return;
    }
    const VkPhysicalDeviceDescriptorBufferPropertiesEXT& props{device.DescriptorBufferProperties()};
    alignment = std::max<VkDeviceSize>(props.descriptorBufferOffsetAlignment, 1);

    const VkDeviceSize max_bound{(std::min)({props.maxSamplerDescriptorBufferRange,
                                             props.maxResourceDescriptorBufferRange,
                                             props.samplerDescriptorBufferAddressSpaceSize,
                                             props.resourceDescriptorBufferAddressSpaceSize,
                                             props.descriptorBufferAddressSpaceSize})};
    const VkDeviceSize frame_size{device.IsTiler() ? TILER_FRAME_SIZE : DESKTOP_FRAME_SIZE};
    const VkDeviceSize chunk_size{
        Common::AlignDown((std::min)(frame_size, max_bound), alignment)};
    if (chunk_size <= alignment) {
        LOG_DEBUG(Render_Vulkan, "Descriptor buffer binding limit of {} is unusable, disabling",
                  max_bound);
        return;
    }
    chunk_capacity = chunk_size - alignment;
    chunks_per_frame = static_cast<size_t>(frame_size / chunk_size);

    const VkBufferCreateInfo buffer_ci{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = chunk_size,
        .usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    const size_t total_chunks{chunks_per_frame * FRAMES_IN_FLIGHT};
    chunks.reserve(total_chunks);
    chunk_addresses.reserve(total_chunks);
    chunk_hosts.reserve(total_chunks);
    for (size_t index = 0; index < total_chunks; ++index) {
        vk::Buffer buffer{memory_allocator.CreateBuffer(buffer_ci, MemoryUsage::Upload)};
        if (!buffer.IsHostVisible()) {
            LOG_DEBUG(Render_Vulkan, "Descriptor buffer is not host visible, disabling");
            chunks.clear();
            return;
        }
        if (!buffer.IsHostCoherent()) {
            LOG_DEBUG(Render_Vulkan, "Descriptor buffer is not host coherent, disabling");
            chunks.clear();
            return;
        }
        if (device.HasDebuggingToolAttached()) {
            buffer.SetObjectNameEXT("Descriptor buffer");
        }
        const VkDeviceAddress raw_address{device.GetLogical().GetBufferDeviceAddress(*buffer)};
        const VkDeviceAddress address{Common::AlignUp(raw_address, alignment)};
        chunk_addresses.push_back(address);
        chunk_hosts.push_back(buffer.Mapped().data() + (address - raw_address));
        chunks.push_back(std::move(buffer));
    }
}

DescriptorBufferRing::~DescriptorBufferRing() = default;

void DescriptorBufferRing::TickFrame() {
    if (++frame_index >= FRAMES_IN_FLIGHT) {
        frame_index = 0;
    }
    chunk_cursor = 0;
    cursor = 0;
    ++generation;
    frame_reused = true;
    slice_cache.Clear();
    draw_bind_cache.InvalidateAll();
}

void DescriptorBufferRing::TouchFrame(Scheduler& scheduler) {
    frame_ticks[frame_index] = scheduler.CurrentTick();
}

DescriptorBufferRing::Allocation DescriptorBufferRing::Allocate(Scheduler& scheduler,
                                                                VkDeviceSize size) {
    ASSERT(!chunks.empty());
    if (!CanAllocate(size)) {
        LOG_DEBUG(Render_Vulkan, "Descriptor set of {} bytes exceeds chunk capacity {}", size,
                  chunk_capacity);
        return Allocation{};
    }
    const VkDeviceSize needed{Common::AlignUp(size, alignment)};
    if (frame_reused) {
        frame_reused = false;
        scheduler.Wait(frame_ticks[frame_index]);
    }
    if (cursor + needed > chunk_capacity) {
        if (chunk_cursor + 1 < chunks_per_frame) {
            ++chunk_cursor;
        } else {
            LOG_DEBUG(Render_Vulkan, "Descriptor buffer frame exhausted, stalling on the GPU");
            scheduler.Finish();
            chunk_cursor = 0;
            ++generation;
            slice_cache.Clear();
            draw_bind_cache.InvalidateAll();
        }
        cursor = 0;
    }
    const size_t chunk{frame_index * chunks_per_frame + chunk_cursor};
    const VkDeviceSize offset{cursor};
    cursor += needed;
    frame_ticks[frame_index] = scheduler.CurrentTick();
    return Allocation{
        .host = chunk_hosts[chunk] + offset,
        .offset = offset,
        .chunk = static_cast<u32>(chunk),
        .generation = generation,
    };
}

} // namespace Vulkan
