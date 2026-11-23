// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <random>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/alignment.h"
#include "common/common_types.h"
#include "common/container/unordered_map.h"
#include "common/slot_vector.h"
#include "video_core/buffer_cache/memory_tracker_base.h"
#include "video_core/buffer_cache/usage_tracker.h"
#include "video_core/nx2_perf.h"
#include "video_core/renderer_vulkan/fragment_barrier_state.h"
#include "video_core/renderer_vulkan/vk_descriptor_buffer.h"

namespace {

// UsageTracker::Track before the full-page run (Nx2Perf off).
class OldUsageTracker {
    static constexpr size_t BUFFER_BYTES_PER_BITSHIFT = 6;
    static constexpr size_t BUFFER_PAGE_SHIFT = 6 + BUFFER_BYTES_PER_BITSHIFT;
    static constexpr size_t BUFFER_PAGE_BYTES = 1 << BUFFER_PAGE_SHIFT;

public:
    explicit OldUsageTracker(size_t size) : pages((size >> BUFFER_PAGE_SHIFT) + 1, 0ULL) {}

    void Reset() noexcept {
        std::ranges::fill(pages, 0ULL);
        recent.fill({~u64{0}, 0});
    }

    void Track(u64 offset, u64 size) noexcept {
        if (size == 0) {
            return;
        }
        for (const auto& [recent_offset, recent_size] : recent) {
            if (recent_offset == offset && recent_size == size) {
                return;
            }
        }
        recent[recent_cursor++ % recent.size()] = {offset, size};
        const size_t page = offset >> BUFFER_PAGE_SHIFT;
        const size_t page_end = (offset + size) >> BUFFER_PAGE_SHIFT;
        if (page_end < page || page_end >= pages.size()) {
            return;
        }
        TrackPage(page, offset, size);
        if (page == page_end) {
            return;
        }
        for (size_t i = page + 1; i < page_end; i++) {
            pages[i] = ~u64{0};
        }
        const size_t offset_end = offset + size;
        const size_t offset_end_page_aligned = Common::AlignDown(offset_end, BUFFER_PAGE_BYTES);
        if (offset_end > offset_end_page_aligned) {
            TrackPage(page_end, offset_end_page_aligned, offset_end - offset_end_page_aligned);
        }
    }

    std::vector<u64> pages;

private:
    void TrackPage(u64 page, u64 offset, u64 size) noexcept {
        if (size == 0) {
            return;
        }
        const size_t offset_in_page = offset % BUFFER_PAGE_BYTES;
        const size_t end_in_page = std::min<size_t>(BUFFER_PAGE_BYTES, offset_in_page + size);
        const size_t first_bit = offset_in_page >> BUFFER_BYTES_PER_BITSHIFT;
        const size_t end_bit =
            (end_in_page + (size_t{1} << BUFFER_BYTES_PER_BITSHIFT) - 1) >> BUFFER_BYTES_PER_BITSHIFT;
        const size_t num_bits = end_bit - first_bit;
        if (num_bits == 0) {
            return;
        }
        const u64 mask = num_bits >= 64 ? ~u64{0} : ((u64{1} << num_bits) - 1);
        pages[page] |= mask << first_bit;
    }

    std::array<std::pair<u64, u64>, 4> recent{{{~u64{0}, 0}, {~u64{0}, 0}, {~u64{0}, 0}, {~u64{0}, 0}}};
    size_t recent_cursor{};
};

class FakeRasterizer {
public:
    void UpdatePagesCachedCount(DAddr addr, size_t size, s32 delta) {
        calls.emplace_back(addr, size, delta);
    }

    void UpdatePagesCachedBatch(std::span<const std::pair<DAddr, size_t>> ranges, s32 delta) {
        for (const auto& [addr, size] : ranges) {
            calls.emplace_back(addr, size, delta);
        }
    }

    std::vector<std::tuple<DAddr, u64, int>> calls;
};

using Tracker = VideoCommon::MemoryTrackerBase<FakeRasterizer>;
using Ranges = std::vector<std::pair<u64, u64>>;

constexpr u64 PAGE = 4096;
constexpr u64 TRACKER_WORD = PAGE * 64;
constexpr u64 MANAGER = 4ULL << 20;
constexpr DAddr BASE = 16 * MANAGER;

Ranges Upload(Tracker& tracker, DAddr addr, u64 size) {
    Ranges ranges;
    tracker.ForEachUploadRange(addr, size, [&](u64 range_addr, u64 range_size) {
        ranges.emplace_back(range_addr, range_size);
    });
    return ranges;
}

Ranges DirtyUpload(Tracker& tracker, DAddr addr, u64 size) {
    Ranges ranges;
    tracker.ForEachDirtyUploadRange(addr, size, [&](u64 range_addr, u64 range_size) {
        ranges.emplace_back(range_addr, range_size);
    });
    return ranges;
}

} // Anonymous namespace

TEST_CASE("Nx2Perf draw-binds: deferred barrier survives unrelated draws", "[video_core]") {
    Vulkan::FragmentBarrierState barrier;
    const auto prepare_draw = [&](bool stores, bool samples_attachment) {
        const bool end_pass = barrier.Pending() && (stores || samples_attachment);
        if (end_pass) {
            barrier.Reset();
        }
        return end_pass;
    };
    REQUIRE_FALSE(prepare_draw(false, true));
    barrier.Defer(true);
    REQUIRE_FALSE(prepare_draw(false, false));
    REQUIRE_FALSE(prepare_draw(false, false));
    REQUIRE(prepare_draw(false, true));
    REQUIRE_FALSE(prepare_draw(false, true));
    barrier.Defer(true);
    REQUIRE(prepare_draw(true, false));
    barrier.Defer(true);
    barrier.Reset();
    REQUIRE_FALSE(prepare_draw(true, true));
    barrier.Defer(false);
    REQUIRE_FALSE(prepare_draw(true, true));
}

TEST_CASE("Nx2Perf draw-binds: usage survives slot relocation and reset", "[video_core]") {
    constexpr u64 size = 16 * MANAGER;
    Common::SlotVector<VideoCommon::UsageTracker> slots;
    const auto id = slots.insert(size);
    slots[id].Track(0, size);
    const std::vector<u64> before{slots[id].Pages().begin(), slots[id].Pages().end()};
    for (size_t i = 0; i < 130; ++i) {
        const auto other = slots.insert(PAGE);
        slots[other].Track(64, 64);
    }
    REQUIRE(std::ranges::equal(slots[id].Pages(), before));
    slots[id].Reset();
    REQUIRE(std::ranges::all_of(slots[id].Pages(), [](u64 word) { return word == 0; }));
    OldUsageTracker old{size};
    for (u64 i = 0; i < 8; ++i) {
        slots[id].Track(i * PAGE, 2 * TRACKER_WORD + 64);
        old.Track(i * PAGE, 2 * TRACKER_WORD + 64);
    }
    REQUIRE(std::ranges::equal(slots[id].Pages(), old.pages));
    auto copied = slots[id];
    VideoCommon::UsageTracker moved{PAGE};
    moved = std::move(copied);
    moved.Track(4 * TRACKER_WORD, TRACKER_WORD);
    old.Track(4 * TRACKER_WORD, TRACKER_WORD);
    REQUIRE(std::ranges::equal(moved.Pages(), old.pages));
    slots.erase(id);
    const auto recycled = slots.insert(size);
    REQUIRE(recycled == id);
    REQUIRE(std::ranges::all_of(slots[recycled].Pages(), [](u64 word) { return word == 0; }));
    slots[recycled].Track(PAGE, 3 * TRACKER_WORD);
    old.Reset();
    old.Track(PAGE, 3 * TRACKER_WORD);
    REQUIRE(std::ranges::equal(slots[recycled].Pages(), old.pages));
}

TEST_CASE("Nx2Perf draw-binds: clean words survive manager pool growth", "[video_core]") {
    FakeRasterizer full_rasterizer;
    FakeRasterizer dirty_rasterizer;
    auto full = std::make_unique<Tracker>(full_rasterizer);
    auto dirty = std::make_unique<Tracker>(dirty_rasterizer);
    for (u64 i = 0; i < 65; ++i) {
        const auto address = BASE + i * MANAGER;
        REQUIRE(Upload(*full, address, MANAGER) == DirtyUpload(*dirty, address, MANAGER));
        full->CachedCpuWrite(address + 15 * TRACKER_WORD + PAGE, 16);
        dirty->CachedCpuWrite(address + 15 * TRACKER_WORD + PAGE, 16);
    }
    full->FlushCachedWrites();
    dirty->FlushCachedWrites();
    REQUIRE(dirty->CleanWordsHold(BASE, 65 * MANAGER));
    REQUIRE(Upload(*full, BASE + 1, 65 * MANAGER - 2) ==
            DirtyUpload(*dirty, BASE + 1, 65 * MANAGER - 2));
    REQUIRE(full_rasterizer.calls == dirty_rasterizer.calls);
    REQUIRE(dirty->CleanWordsHold(BASE, 65 * MANAGER));
    REQUIRE(DirtyUpload(*dirty, BASE, 65 * MANAGER).empty());
}

TEST_CASE("Nx2Perf draw-binds: UsageTracker run matches the old Track", "[video_core]") {
    REQUIRE(VideoCommon::Nx2Perf::Enabled());
    constexpr u64 buffer_size = 24ULL << 20;
    std::mt19937_64 rng{0x0400486024DEC000ULL};
    VideoCommon::UsageTracker tracker{buffer_size};
    OldUsageTracker old{buffer_size};
    const auto random_range = [&]() -> std::pair<u64, u64> {
        static constexpr u64 sizes[] = {16, 64, 100, 256, 4096, 6000, 65536, 1ULL << 20, 8ULL << 20};
        const u64 size = sizes[rng() % std::size(sizes)] + (rng() % 2 == 0 ? 0 : rng() % 8192);
        const u64 offset = rng() % 2 == 0 ? (rng() % (buffer_size / 4096)) * 4096 : rng() % buffer_size;
        return {offset, size};
    };
    std::vector<std::pair<u64, u64>> history;
    for (int step = 0; step < 200000; ++step) {
        const u32 choice = static_cast<u32>(rng() % 1000);
        if (choice == 0) {
            tracker.Reset();
            old.Reset();
            history.clear();
            continue;
        }
        std::pair<u64, u64> range;
        if (choice < 600 && !history.empty()) {
            range = history[rng() % history.size()];
        } else {
            range = random_range();
            history.push_back(range);
        }
        tracker.Track(range.first, range.second);
        old.Track(range.first, range.second);
        if (step % 4096 == 0) {
            REQUIRE(std::ranges::equal(tracker.Pages(), old.pages));
        }
    }
    REQUIRE(std::ranges::equal(tracker.Pages(), old.pages));
}

TEST_CASE("Nx2Perf draw-binds: clean words", "[video_core]") {
    FakeRasterizer rasterizer;
    auto tracker = std::make_unique<Tracker>(rasterizer);
    // A new manager is CPU-modified everywhere.
    REQUIRE(DirtyUpload(*tracker, BASE + 100, 3 * TRACKER_WORD) ==
            Ranges{{BASE, 3 * TRACKER_WORD + PAGE}});
    REQUIRE(DirtyUpload(*tracker, BASE, MANAGER) ==
            Ranges{{BASE + 3 * TRACKER_WORD + PAGE, MANAGER - 3 * TRACKER_WORD - PAGE}});
    REQUIRE(tracker->CleanWordsHold(BASE, MANAGER));
    const size_t calls = rasterizer.calls.size();
    REQUIRE(DirtyUpload(*tracker, BASE, MANAGER).empty());
    REQUIRE(rasterizer.calls.size() == calls);

    tracker->MarkRegionAsCpuModified(BASE + 5 * TRACKER_WORD + 3 * PAGE, 16);
    REQUIRE(tracker->CleanWordsHold(BASE, MANAGER));
    REQUIRE(DirtyUpload(*tracker, BASE + 5 * TRACKER_WORD, PAGE).empty());
    REQUIRE(DirtyUpload(*tracker, BASE, MANAGER) == Ranges{{BASE + 5 * TRACKER_WORD + 3 * PAGE, PAGE}});

    // A cached write is Untracked before it is flushed.
    tracker->CachedCpuWrite(BASE + 9 * TRACKER_WORD, 4);
    REQUIRE(tracker->CleanWordsHold(BASE, MANAGER));
    tracker->FlushCachedWrites();
    REQUIRE(tracker->CleanWordsHold(BASE, MANAGER));
    REQUIRE(DirtyUpload(*tracker, BASE + 9 * TRACKER_WORD, TRACKER_WORD) ==
            Ranges{{BASE + 9 * TRACKER_WORD, PAGE}});

    // Unmarking makes a word clean again, as the upload would.
    tracker->MarkRegionAsCpuModified(BASE + 2 * TRACKER_WORD, 2 * PAGE);
    tracker->UnmarkRegionAsCpuModified(BASE + 2 * TRACKER_WORD, PAGE);
    REQUIRE(tracker->CleanWordsHold(BASE, MANAGER));
    tracker->UnmarkRegionAsCpuModified(BASE + 2 * TRACKER_WORD + PAGE, PAGE);
    REQUIRE(tracker->CleanWordsHold(BASE, MANAGER));
    REQUIRE(DirtyUpload(*tracker, BASE, MANAGER).empty());

    // GPU writes leave words clean; a range reaching a missing manager creates it.
    tracker->MarkRegionAsGpuModified(BASE, MANAGER);
    REQUIRE(DirtyUpload(*tracker, BASE + MANAGER - TRACKER_WORD, 2 * TRACKER_WORD) ==
            Ranges{{BASE + MANAGER, TRACKER_WORD}});
    REQUIRE(tracker->CleanWordsHold(BASE, 2 * MANAGER));
}

TEST_CASE("Nx2Perf draw-binds: dirty-word uploads match full uploads", "[video_core]") {
    FakeRasterizer full_rasterizer;
    FakeRasterizer narrow_rasterizer;
    auto full = std::make_unique<Tracker>(full_rasterizer);
    auto narrow = std::make_unique<Tracker>(narrow_rasterizer);
    std::mt19937_64 rng{0x8e0ULL};
    constexpr u64 span = 6 * MANAGER;
    const auto random_range = [&]() -> std::pair<DAddr, u64> {
        static constexpr u64 sizes[] = {4,           256,
                                        PAGE,        3 * PAGE,
                                        TRACKER_WORD, 3 * TRACKER_WORD + 100,
                                        MANAGER,     2 * MANAGER + 5 * PAGE};
        const u64 size = sizes[rng() % std::size(sizes)];
        const u64 offset = rng() % 2 == 0 ? (rng() % (span / PAGE)) * PAGE : rng() % span;
        return {BASE + offset, (std::min)(size, span - offset)};
    };
    for (int step = 0; step < 40000; ++step) {
        const auto [addr, size] = random_range();
        switch (rng() % 10) {
        case 0:
            full->MarkRegionAsCpuModified(addr, size);
            narrow->MarkRegionAsCpuModified(addr, size);
            break;
        case 1:
            full->UnmarkRegionAsCpuModified(addr, size);
            narrow->UnmarkRegionAsCpuModified(addr, size);
            break;
        case 2:
            full->CachedCpuWrite(addr, size);
            narrow->CachedCpuWrite(addr, size);
            break;
        case 3:
            full->FlushCachedWrites();
            narrow->FlushCachedWrites();
            break;
        case 4:
            full->MarkRegionAsGpuModified(addr, size);
            narrow->MarkRegionAsGpuModified(addr, size);
            break;
        case 5:
            full->UnmarkRegionAsGpuModified(addr, size);
            narrow->UnmarkRegionAsGpuModified(addr, size);
            break;
        default:
            REQUIRE(Upload(*full, addr, size) == DirtyUpload(*narrow, addr, size));
            break;
        }
        REQUIRE(full_rasterizer.calls == narrow_rasterizer.calls);
        REQUIRE(narrow->CleanWordsHold(BASE, span));
        if (step % 1000 == 0) {
            for (u64 page = 0; page < span; page += PAGE) {
                REQUIRE(full->IsRegionCpuModified(BASE + page, PAGE) ==
                        narrow->IsRegionCpuModified(BASE + page, PAGE));
                REQUIRE(full->IsRegionGpuModified(BASE + page, PAGE) ==
                        narrow->IsRegionGpuModified(BASE + page, PAGE));
            }
        }
    }
    REQUIRE(full->ModifiedCpuRegion(BASE, span) == narrow->ModifiedCpuRegion(BASE, span));
}

TEST_CASE("Nx2Perf draw-binds: descriptor slice and bind cache skips redundant draws",
          "[nx2_perf][video_core]") {
    using Vulkan::DescriptorBindCache;
    using Vulkan::DescriptorSliceCache;
    using Vulkan::DescriptorSliceKey;
    using Vulkan::HashDescriptorBytes;
    using Vulkan::ParseDrawCacheEnvValue;

    CHECK(ParseDrawCacheEnvValue(nullptr));
    CHECK(ParseDrawCacheEnvValue("1"));
    CHECK_FALSE(ParseDrawCacheEnvValue("0"));

    const auto make_layout = [](uintptr_t v) -> VkPipelineLayout {
        return reinterpret_cast<VkPipelineLayout>(v);
    };

    SECTION("Per-stage descriptor slice cache respects generation and stage hashes") {
        DescriptorSliceCache cache{true};
        DescriptorSliceKey key{
            .layout_hash = 0x1122334455667788ULL,
            .stage_hashes = {10ULL, 20ULL, 0ULL, 0ULL, 30ULL},
            .payload_hash = HashDescriptorBytes("stage-payload-v1", 16),
            .entry_count = 6,
            .size = 256,
        };

        CHECK_FALSE(cache.Lookup(key, 1).has_value());
        cache.Insert(key, 128, 2, 1);

        const auto hit = cache.Lookup(key, 1);
        REQUIRE(hit.has_value());
        CHECK(hit->offset == 128);
        CHECK(hit->chunk == 2);
        CHECK(hit->generation == 1);

        // Changing a single stage's slice hash misses instead of aliasing.
        DescriptorSliceKey changed_stage = key;
        changed_stage.stage_hashes[4] = 31ULL;
        CHECK_FALSE(cache.Lookup(changed_stage, 1).has_value());

        // Frame / ring wrap generation bump invalidates old entries.
        CHECK_FALSE(cache.Lookup(key, 2).has_value());

        // Disabled mode (EDEN_NX2_DRAW_CACHE=0) never caches or hits.
        DescriptorSliceCache disabled{false};
        disabled.Insert(key, 128, 2, 1);
        CHECK_FALSE(disabled.Lookup(key, 1).has_value());
    }

    SECTION("DescriptorBindCache skips redundant binds when chunk, offset, layout, and tick match") {
        DescriptorBindCache binds{true};
        const VkPipelineLayout layout_a = make_layout(0x1000);
        const VkPipelineLayout layout_b = make_layout(0x2000);

        // First draw must bind buffer and set offsets.
        auto d = binds.AcquireGraphicsBinding(0, 64, layout_a, 10, 1, true);
        CHECK(d.bind_buffer);
        CHECK(d.set_offsets);

        // Matching state on the same command buffer skips both calls.
        d = binds.AcquireGraphicsBinding(0, 64, layout_a, 10, 1, false);
        CHECK_FALSE(d.bind_buffer);
        CHECK_FALSE(d.set_offsets);

        // Same chunk and generation, changed offset or layout skips BindDescriptorBuffersEXT only.
        d = binds.AcquireGraphicsBinding(0, 192, layout_a, 10, 1, false);
        CHECK_FALSE(d.bind_buffer);
        CHECK(d.set_offsets);

        d = binds.AcquireGraphicsBinding(0, 192, layout_b, 10, 1, false);
        CHECK_FALSE(d.bind_buffer);
        CHECK(d.set_offsets);

        // Compute / offset invalidation requires re-emitting offsets, not buffer bind when chunk matches.
        binds.InvalidateOffsets();
        d = binds.AcquireGraphicsBinding(0, 192, layout_b, 10, 1, false);
        CHECK_FALSE(d.bind_buffer);
        CHECK(d.set_offsets);

        // Command buffer tick advance, ring generation advance, or scheduler invalidation re-binds both.
        d = binds.AcquireGraphicsBinding(0, 192, layout_b, 10, 2, false);
        CHECK(d.bind_buffer);
        CHECK(d.set_offsets);

        d = binds.AcquireGraphicsBinding(0, 192, layout_b, 11, 2, false);
        CHECK(d.bind_buffer);
        CHECK(d.set_offsets);

        d = binds.AcquireGraphicsBinding(0, 192, layout_b, 11, 2, true);
        CHECK(d.bind_buffer);
        CHECK(d.set_offsets);

        binds.InvalidateAll();
        d = binds.AcquireGraphicsBinding(0, 192, layout_b, 11, 2, false);
        CHECK(d.bind_buffer);
        CHECK(d.set_offsets);

        // Disabled mode (EDEN_NX2_DRAW_CACHE=0) always emits both commands.
        DescriptorBindCache disabled{false};
        d = disabled.AcquireGraphicsBinding(0, 64, layout_a, 10, 1, false);
        CHECK(d.bind_buffer);
        CHECK(d.set_offsets);
        d = disabled.AcquireGraphicsBinding(0, 64, layout_a, 10, 1, false);
        CHECK(d.bind_buffer);
        CHECK(d.set_offsets);
    }
}
