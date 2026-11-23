// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/alignment.h"
#include "common/common_types.h"
#include "common/range_sets.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/platform_profile.h"
#include "video_core/buffer_cache/buffer_cache_base.h"
#include "video_core/buffer_cache/memory_tracker_base.h"
#include "video_core/buffer_cache/read_trap_tracker.h"

namespace {

using Range = std::pair<DAddr, u64>;

constexpr u64 PAGE = Core::DEVICE_PAGESIZE;
constexpr DAddr BASE = 0x4000000ULL;

class FakeDeviceTracker {
public:
    void UpdatePagesCachedCount(DAddr, size_t, s32) {}

    void UpdatePagesCachedBatch(std::span<const std::pair<DAddr, size_t>>, s32) {}

    void ArmReadTraps(DAddr addr, size_t size) {
        if (!can_trap_cpu_reads || size == 0) {
            return;
        }
        ++arm_calls;
        const DAddr page_begin = Common::AlignDown(addr, PAGE) >> Core::DEVICE_PAGEBITS;
        const DAddr page_end = Common::AlignUp(addr + size, PAGE) >> Core::DEVICE_PAGEBITS;
        for (DAddr page = page_begin; page < page_end; ++page) {
            armed_pages.insert(page);
        }
    }

    [[nodiscard]] bool ConsumeReadTrap(DAddr addr, u64 size) {
        if (!can_trap_cpu_reads || size == 0) {
            return false;
        }
        const DAddr page_begin = Common::AlignDown(addr, PAGE) >> Core::DEVICE_PAGEBITS;
        const DAddr page_end = Common::AlignUp(addr + size, PAGE) >> Core::DEVICE_PAGEBITS;
        bool triggered = false;
        for (DAddr page = page_begin; page < page_end; ++page) {
            if (armed_pages.contains(page)) {
                triggered = true;
                break;
            }
        }
        if (!triggered) {
            return false;
        }
        for (DAddr page = page_begin; page < page_end; ++page) {
            armed_pages.erase(page);
        }
        return true;
    }

    [[nodiscard]] bool IsPageTrapArmed(DAddr addr) const {
        return armed_pages.contains(addr >> Core::DEVICE_PAGEBITS);
    }

    [[nodiscard]] bool CanTrapCpuReads() const noexcept {
        return can_trap_cpu_reads;
    }

    void SetCanTrapCpuReads(bool enabled) noexcept {
        can_trap_cpu_reads = enabled;
        if (!enabled) {
            armed_pages.clear();
        }
    }

    [[nodiscard]] size_t ArmCalls() const noexcept {
        return arm_calls;
    }

    [[nodiscard]] size_t ArmedPageCount() const noexcept {
        return armed_pages.size();
    }

private:
    bool can_trap_cpu_reads{true};
    size_t arm_calls{};
    std::set<DAddr> armed_pages;
};

using TestMemoryTracker = VideoCommon::MemoryTrackerBase<FakeDeviceTracker>;

struct FenceResult {
    std::vector<Range> downloaded_ranges;
};

struct CpuReadResult {
    bool trap_fired{};
    bool flushed_synchronously{};
    bool preemptive{true};
};

class SimulatedBufferCache {
public:
    explicit SimulatedBufferCache(Core::PlatformProfile profile_, bool has_gpu_memory_,
                                  bool uses_native_tiling_, bool can_trap_cpu_reads_,
                                  std::optional<bool> env_override_ = std::nullopt)
        : profile{profile_}, has_gpu_memory{has_gpu_memory_},
          uses_native_tiling{uses_native_tiling_}, env_override{env_override_} {
        device_tracker.SetCanTrapCpuReads(can_trap_cpu_reads_);
    }

    [[nodiscard]] VideoCommon::BufferDownloadPolicyInputs PolicyInputs() const noexcept {
        return VideoCommon::BufferDownloadPolicyInputs{
            .profile = profile,
            .has_gpu_memory = has_gpu_memory,
            .uses_native_tiling = uses_native_tiling,
            .can_trap_cpu_reads = device_tracker.CanTrapCpuReads(),
            .env_override = env_override,
        };
    }

    [[nodiscard]] bool AsyncBufferDownloads() const noexcept {
        return VideoCommon::EvaluateAsyncBufferDownloads(PolicyInputs());
    }

    [[nodiscard]] bool FlushesOnCpuRead() const noexcept {
        return VideoCommon::EvaluateFlushesOnCpuRead(PolicyInputs());
    }

    [[nodiscard]] bool DownloadsAtFence(DAddr device_addr, u64 size) noexcept {
        return VideoCommon::EvaluateDownloadsAtFence(PolicyInputs(), memory_tracker, device_addr,
                                                     size);
    }

    [[nodiscard]] bool HasGpuWritesNotDownloadedAtFence(DAddr device_addr, u64 size) {
        return VideoCommon::EvaluateHasGpuWritesNotDownloadedAtFence(PolicyInputs(), memory_tracker,
                                                                     device_addr, size);
    }

    void PrepareHostMappedRange(DAddr device_addr, u64 size) {
        memory_tracker.UnmarkRegionAsCpuModified(device_addr, size);
    }

    void GpuWrite(DAddr device_addr, u64 size) {
        memory_tracker.MarkRegionAsGpuModified(device_addr, size);
        gpu_modified_ranges.Add(device_addr, size);
        VideoCommon::DispatchGpuWriteDownloadPolicy(
            PolicyInputs(), small_download_limit, preflush_aging, memory_tracker, device_addr, size,
            [this](DAddr addr, u64 range_size) {
                uncommitted_gpu_modified_ranges.Add(addr, range_size);
            },
            [this](DAddr addr, u64 range_size) { ArmReadTraps(addr, range_size); });
    }

    FenceResult Fence() {
        FenceResult result;
        uncommitted_gpu_modified_ranges.ForEach([&](DAddr interval_lower, DAddr interval_upper) {
            const u64 interval_size = interval_upper - interval_lower;
            memory_tracker.ForEachDownloadRange(
                interval_lower, interval_size, false, [&](u64 start, u64 range_size) {
                    gpu_modified_ranges.ForEachInRange(
                        start, range_size, [&](DAddr lower, DAddr upper) {
                            result.downloaded_ranges.emplace_back(lower, upper - lower);
                        });
                });
        });
        for (const auto& [addr, range_size] : result.downloaded_ranges) {
            gpu_modified_ranges.Subtract(addr, range_size);
        }
        uncommitted_gpu_modified_ranges.Clear();
        return result;
    }

    CpuReadResult CpuRead(DAddr addr, u64 size) {
        if (!FlushesOnCpuRead()) {
            return CpuReadResult{
                .trap_fired = false,
                .flushed_synchronously = false,
                .preemptive = true,
            };
        }
        if (!device_tracker.ConsumeReadTrap(addr, size)) {
            return CpuReadResult{
                .trap_fired = false,
                .flushed_synchronously = false,
                .preemptive = true,
            };
        }
        const DAddr aligned_start = Common::AlignDown(addr, PAGE);
        const DAddr aligned_end = Common::AlignUp(addr + size, PAGE);
        const u64 aligned_size = aligned_end - aligned_start;

        if (memory_tracker.IsRegionPreflushable(addr, size) &&
            !HasGpuWritesNotDownloadedAtFence(aligned_start, aligned_size)) {
            read_trapped_ranges.Subtract(aligned_start, aligned_size);
            return CpuReadResult{
                .trap_fired = true,
                .flushed_synchronously = false,
                .preemptive = true,
            };
        }

        const bool gpu_dirty = gpu_modified_ranges.Intersects(aligned_start, aligned_size);
        read_trapped_ranges.Subtract(aligned_start, aligned_size);
        if (!gpu_dirty) {
            return CpuReadResult{
                .trap_fired = true,
                .flushed_synchronously = false,
                .preemptive = true,
            };
        }

        memory_tracker.MarkRegionAsPreflushable(aligned_start, aligned_size);
        memory_tracker.ForEachDownloadRangeAndClear(
            aligned_start, aligned_size, [&](u64 start, u64 range_size) {
                gpu_modified_ranges.Subtract(start, range_size);
            });
        return CpuReadResult{
            .trap_fired = true,
            .flushed_synchronously = true,
            .preemptive = false,
        };
    }

    [[nodiscard]] bool HasPendingFenceDownloads() const {
        return !uncommitted_gpu_modified_ranges.Empty();
    }

    [[nodiscard]] bool IsGpuModifiedInGuestView(DAddr addr, u64 size) const {
        return gpu_modified_ranges.Intersects(addr, size);
    }

    FakeDeviceTracker device_tracker;
    TestMemoryTracker memory_tracker{device_tracker};
    VideoCommon::ReadTrapTracker read_trapped_ranges;
    Common::RangeSet<DAddr> gpu_modified_ranges;
    Common::RangeSet<DAddr> uncommitted_gpu_modified_ranges;
    Core::PlatformProfile profile{};
    bool has_gpu_memory{true};
    bool uses_native_tiling{false};
    std::optional<bool> env_override{};
    u64 small_download_limit{0};
    bool preflush_aging{false};

private:
    void ArmReadTraps(DAddr addr, u64 size) {
        const DAddr begin = Common::AlignDown(addr, PAGE);
        const DAddr end = Common::AlignUp(addr + size, PAGE);
        read_trapped_ranges.Arm(begin, end - begin, [this](DAddr part, u64 part_size) {
            device_tracker.ArmReadTraps(part, part_size);
        });
    }
};

struct FakeRuntimeWithProfile {
    Core::PlatformProfile profile;

    [[nodiscard]] const Core::PlatformProfile& Platform() const noexcept {
        return profile;
    }
};

struct FakeRuntimeWithoutProfile {};

struct FakeMemoryManagerWithProfile {
    Core::PlatformProfile profile;

    [[nodiscard]] const Core::PlatformProfile& Platform() const noexcept {
        return profile;
    }
};

struct FakeMemoryManagerWithoutProfile {};

} // namespace

TEST_CASE("Buffer download policy evaluates profile defaults, traps, tiling, and env overrides",
          "[video_core][buffer-download-policy]") {
    using VideoCommon::BufferDownloadPolicyInputs;
    using VideoCommon::EvaluateAsyncBufferDownloads;
    using VideoCommon::EvaluateFlushesOnCpuRead;
    using VideoCommon::ParseBufferDownloadsOverride;
    using VideoCommon::ProfilePrefersAsyncBufferDownloads;

    const auto default_sw1 = Core::MakePlatformProfile(Core::PlatformResolution::DefaultSwitch1);
    const auto confirmed_sw1 =
        Core::MakePlatformProfile(Core::PlatformResolution::ConfirmedSwitch1);
    const auto override_sw1 = Core::MakePlatformProfile(Core::PlatformResolution::OverrideSwitch1);
    const auto confirmed_sw2 =
        Core::MakePlatformProfile(Core::PlatformResolution::ConfirmedSwitch2);
    const auto override_sw2 = Core::MakePlatformProfile(Core::PlatformResolution::OverrideSwitch2);

    SECTION("ParseBufferDownloadsOverride handles 0, 1, null, empty, and invalid values") {
        CHECK_FALSE(ParseBufferDownloadsOverride(nullptr).has_value());
        CHECK_FALSE(ParseBufferDownloadsOverride("").has_value());
        CHECK_FALSE(ParseBufferDownloadsOverride("2").has_value());
        CHECK_FALSE(ParseBufferDownloadsOverride("true").has_value());
        CHECK_FALSE(ParseBufferDownloadsOverride("false").has_value());
        CHECK_FALSE(ParseBufferDownloadsOverride("yes").has_value());
        CHECK_FALSE(ParseBufferDownloadsOverride(" 1").has_value());
        CHECK_FALSE(ParseBufferDownloadsOverride("10").has_value());
        CHECK(ParseBufferDownloadsOverride("0") == std::optional<bool>{false});
        CHECK(ParseBufferDownloadsOverride("1") == std::optional<bool>{true});
    }

    SECTION("PlatformProfile defaults Switch 1 async downloads on and Switch 2 off") {
        CHECK(ProfilePrefersAsyncBufferDownloads(default_sw1));
        CHECK(ProfilePrefersAsyncBufferDownloads(confirmed_sw1));
        CHECK(ProfilePrefersAsyncBufferDownloads(override_sw1));
        CHECK_FALSE(ProfilePrefersAsyncBufferDownloads(confirmed_sw2));
        CHECK_FALSE(ProfilePrefersAsyncBufferDownloads(override_sw2));

        if constexpr (requires { default_sw1.prefer_async_buffer_downloads; }) {
            CHECK(default_sw1.prefer_async_buffer_downloads);
            CHECK(confirmed_sw1.prefer_async_buffer_downloads);
            CHECK(override_sw1.prefer_async_buffer_downloads);
            CHECK_FALSE(confirmed_sw2.prefer_async_buffer_downloads);
            CHECK_FALSE(override_sw2.prefer_async_buffer_downloads);

            auto custom = confirmed_sw2;
            custom.prefer_async_buffer_downloads = true;
            CHECK(ProfilePrefersAsyncBufferDownloads(custom));
        }

        for (const auto& sw1 : {default_sw1, confirmed_sw1, override_sw1}) {
            CHECK(EvaluateAsyncBufferDownloads(BufferDownloadPolicyInputs{
                .profile = sw1,
                .has_gpu_memory = true,
                .uses_native_tiling = false,
                .can_trap_cpu_reads = true,
            }));
            CHECK(EvaluateAsyncBufferDownloads(BufferDownloadPolicyInputs{
                .profile = sw1,
                .has_gpu_memory = true,
                .uses_native_tiling = true,
                .can_trap_cpu_reads = true,
            }));
            CHECK_FALSE(EvaluateFlushesOnCpuRead(BufferDownloadPolicyInputs{
                .profile = sw1,
                .has_gpu_memory = true,
                .uses_native_tiling = true,
                .can_trap_cpu_reads = true,
            }));
        }

        for (const auto& sw2 : {confirmed_sw2, override_sw2}) {
            CHECK_FALSE(EvaluateAsyncBufferDownloads(BufferDownloadPolicyInputs{
                .profile = sw2,
                .has_gpu_memory = true,
                .uses_native_tiling = true,
                .can_trap_cpu_reads = true,
            }));
            CHECK(EvaluateFlushesOnCpuRead(BufferDownloadPolicyInputs{
                .profile = sw2,
                .has_gpu_memory = true,
                .uses_native_tiling = true,
                .can_trap_cpu_reads = true,
            }));
        }
    }

    SECTION("Unavailable host read traps force async fence downloads to stay enabled") {
        const BufferDownloadPolicyInputs no_traps{
            .profile = confirmed_sw2,
            .has_gpu_memory = true,
            .uses_native_tiling = true,
            .can_trap_cpu_reads = false,
        };
        CHECK(EvaluateAsyncBufferDownloads(no_traps));
        CHECK_FALSE(EvaluateFlushesOnCpuRead(no_traps));
    }

    SECTION("Absent gpu_memory or disabled native tiling forces async fence downloads") {
        const BufferDownloadPolicyInputs null_gpu_mem{
            .profile = confirmed_sw2,
            .has_gpu_memory = false,
            .uses_native_tiling = false,
            .can_trap_cpu_reads = true,
        };
        CHECK(EvaluateAsyncBufferDownloads(null_gpu_mem));
        CHECK_FALSE(EvaluateFlushesOnCpuRead(null_gpu_mem));

        const BufferDownloadPolicyInputs no_tiling{
            .profile = confirmed_sw2,
            .has_gpu_memory = true,
            .uses_native_tiling = false,
            .can_trap_cpu_reads = true,
        };
        CHECK(EvaluateAsyncBufferDownloads(no_tiling));
        CHECK_FALSE(EvaluateFlushesOnCpuRead(no_tiling));
    }

    SECTION("EDEN_NX2_BUFFER_DOWNLOADS=0|1 override takes precedence over profile and runtime") {
        CHECK(EvaluateAsyncBufferDownloads(BufferDownloadPolicyInputs{
            .profile = confirmed_sw2,
            .has_gpu_memory = true,
            .uses_native_tiling = true,
            .can_trap_cpu_reads = true,
            .env_override = true,
        }));
        CHECK_FALSE(EvaluateFlushesOnCpuRead(BufferDownloadPolicyInputs{
            .profile = confirmed_sw2,
            .has_gpu_memory = true,
            .uses_native_tiling = true,
            .can_trap_cpu_reads = true,
            .env_override = true,
        }));

        CHECK_FALSE(EvaluateAsyncBufferDownloads(BufferDownloadPolicyInputs{
            .profile = confirmed_sw1,
            .has_gpu_memory = false,
            .uses_native_tiling = false,
            .can_trap_cpu_reads = false,
            .env_override = false,
        }));
        CHECK(EvaluateFlushesOnCpuRead(BufferDownloadPolicyInputs{
            .profile = confirmed_sw1,
            .has_gpu_memory = false,
            .uses_native_tiling = false,
            .can_trap_cpu_reads = false,
            .env_override = false,
        }));
    }
}

TEST_CASE("ResolveBufferCachePlatformProfile prefers gpu_memory/runtime and falls back cleanly",
          "[video_core][buffer-download-policy]") {
    using VideoCommon::ResolveBufferCachePlatformProfile;

    const bool prev_switch2 = Settings::CurrentProgramIsSwitch2();
    SCOPE_EXIT {
        Settings::SetCurrentProgramIsSwitch2(prev_switch2);
    };

    const auto sw1 = Core::MakePlatformProfile(Core::PlatformResolution::ConfirmedSwitch1);
    const auto sw2 = Core::MakePlatformProfile(Core::PlatformResolution::ConfirmedSwitch2);

    Settings::SetCurrentProgramIsSwitch2(false);
    const FakeMemoryManagerWithProfile gpu_mem_sw2{sw2};
    const FakeRuntimeWithProfile runtime_sw1{sw1};
    CHECK(ResolveBufferCachePlatformProfile(runtime_sw1, &gpu_mem_sw2).IsSwitch2());

    const FakeMemoryManagerWithProfile* null_mem_with_profile = nullptr;
    CHECK_FALSE(ResolveBufferCachePlatformProfile(runtime_sw1, null_mem_with_profile).IsSwitch2());

    const FakeRuntimeWithoutProfile bare_runtime;
    const FakeMemoryManagerWithoutProfile bare_mem;
    CHECK_FALSE(ResolveBufferCachePlatformProfile(bare_runtime, &bare_mem).IsSwitch2());

    Settings::SetCurrentProgramIsSwitch2(true);
    CHECK(ResolveBufferCachePlatformProfile(bare_runtime, &bare_mem).IsSwitch2());
    CHECK(ResolveBufferCachePlatformProfile(bare_runtime,
                                            static_cast<const FakeMemoryManagerWithoutProfile*>(nullptr))
              .IsSwitch2());
}

TEST_CASE("GPU write -> fence -> CPU read under Switch 1, Switch 2, traps, tiling, and overrides",
          "[video_core][buffer-download-policy]") {
    const auto sw1 = Core::MakePlatformProfile(Core::PlatformResolution::ConfirmedSwitch1);
    const auto sw2 = Core::MakePlatformProfile(Core::PlatformResolution::ConfirmedSwitch2);
    constexpr DAddr buf = BASE;
    constexpr u64 size = PAGE * 2;

    SECTION("Switch 1 downloads GPU writes at fence without arming read traps") {
        SimulatedBufferCache cache{sw1, /*has_gpu_memory=*/true, /*uses_native_tiling=*/false,
                                   /*can_trap_cpu_reads=*/true};
        cache.PrepareHostMappedRange(buf, size);
        CHECK(cache.AsyncBufferDownloads());
        CHECK_FALSE(cache.FlushesOnCpuRead());

        cache.GpuWrite(buf, size);
        CHECK(cache.DownloadsAtFence(buf, size));
        CHECK_FALSE(cache.HasGpuWritesNotDownloadedAtFence(buf, size));
        CHECK(cache.HasPendingFenceDownloads());
        CHECK(cache.device_tracker.ArmedPageCount() == 0);

        const auto fence = cache.Fence();
        CHECK(fence.downloaded_ranges == std::vector<Range>{{buf, size}});
        CHECK_FALSE(cache.IsGpuModifiedInGuestView(buf, size));

        const auto read = cache.CpuRead(buf, size);
        CHECK_FALSE(read.trap_fired);
        CHECK_FALSE(read.flushed_synchronously);
        CHECK(read.preemptive);
    }

    SECTION("Switch 2 with native tiling and read traps skips fence download until first CPU read") {
        SimulatedBufferCache cache{sw2, /*has_gpu_memory=*/true, /*uses_native_tiling=*/true,
                                   /*can_trap_cpu_reads=*/true};
        cache.PrepareHostMappedRange(buf, size);
        CHECK_FALSE(cache.AsyncBufferDownloads());
        CHECK(cache.FlushesOnCpuRead());

        cache.GpuWrite(buf, size);
        CHECK_FALSE(cache.DownloadsAtFence(buf, size));
        CHECK(cache.HasGpuWritesNotDownloadedAtFence(buf, size));
        CHECK_FALSE(cache.HasPendingFenceDownloads());
        CHECK(cache.device_tracker.IsPageTrapArmed(buf));
        CHECK(cache.device_tracker.IsPageTrapArmed(buf + PAGE));

        const auto fence = cache.Fence();
        CHECK(fence.downloaded_ranges.empty());
        CHECK(cache.IsGpuModifiedInGuestView(buf, size));

        const auto read = cache.CpuRead(buf, size);
        CHECK(read.trap_fired);
        CHECK(read.flushed_synchronously);
        CHECK_FALSE(read.preemptive);
        CHECK(cache.memory_tracker.IsRegionPreflushable(buf, size));
        CHECK_FALSE(cache.IsGpuModifiedInGuestView(buf, size));
        CHECK(cache.device_tracker.ArmedPageCount() == 0);
    }

    SECTION("Switch 2 without host read traps keeps fence downloads enabled") {
        SimulatedBufferCache cache{sw2, /*has_gpu_memory=*/true, /*uses_native_tiling=*/true,
                                   /*can_trap_cpu_reads=*/false};
        cache.PrepareHostMappedRange(buf, size);
        CHECK(cache.AsyncBufferDownloads());
        CHECK_FALSE(cache.FlushesOnCpuRead());

        cache.GpuWrite(buf, size);
        CHECK(cache.DownloadsAtFence(buf, size));
        CHECK_FALSE(cache.HasGpuWritesNotDownloadedAtFence(buf, size));
        CHECK(cache.HasPendingFenceDownloads());
        CHECK(cache.device_tracker.ArmedPageCount() == 0);

        const auto fence = cache.Fence();
        CHECK(fence.downloaded_ranges == std::vector<Range>{{buf, size}});
    }

    SECTION("Switch 2 with native tiling disabled keeps fence downloads enabled") {
        SimulatedBufferCache cache{sw2, /*has_gpu_memory=*/true, /*uses_native_tiling=*/false,
                                   /*can_trap_cpu_reads=*/true};
        cache.PrepareHostMappedRange(buf, size);
        CHECK(cache.AsyncBufferDownloads());

        cache.GpuWrite(buf, size);
        CHECK(cache.DownloadsAtFence(buf, size));
        CHECK_FALSE(cache.HasGpuWritesNotDownloadedAtFence(buf, size));
        CHECK(cache.device_tracker.ArmedPageCount() == 0);

        const auto fence = cache.Fence();
        CHECK(fence.downloaded_ranges == std::vector<Range>{{buf, size}});
    }

    SECTION("Switch 2 with null gpu_memory keeps fence downloads enabled") {
        SimulatedBufferCache cache{sw2, /*has_gpu_memory=*/false, /*uses_native_tiling=*/false,
                                   /*can_trap_cpu_reads=*/true};
        cache.PrepareHostMappedRange(buf, size);
        CHECK(cache.AsyncBufferDownloads());
        CHECK_FALSE(cache.FlushesOnCpuRead());

        cache.GpuWrite(buf, size);
        CHECK(cache.DownloadsAtFence(buf, size));
        CHECK_FALSE(cache.HasGpuWritesNotDownloadedAtFence(buf, size));
        CHECK(cache.device_tracker.ArmedPageCount() == 0);
        CHECK(cache.Fence().downloaded_ranges == std::vector<Range>{{buf, size}});
    }

    SECTION("EDEN_NX2_BUFFER_DOWNLOADS=0/1 overrides Switch 1 and Switch 2 defaults in lifecycle") {
        {
            SimulatedBufferCache sw2_forced_on{sw2, /*has_gpu_memory=*/true,
                                               /*uses_native_tiling=*/true,
                                               /*can_trap_cpu_reads=*/true,
                                               /*env_override=*/true};
            sw2_forced_on.PrepareHostMappedRange(buf, size);
            CHECK(sw2_forced_on.AsyncBufferDownloads());
            sw2_forced_on.GpuWrite(buf, size);
            CHECK(sw2_forced_on.DownloadsAtFence(buf, size));
            CHECK_FALSE(sw2_forced_on.HasGpuWritesNotDownloadedAtFence(buf, size));
            CHECK(sw2_forced_on.device_tracker.ArmedPageCount() == 0);
            CHECK(sw2_forced_on.Fence().downloaded_ranges == std::vector<Range>{{buf, size}});
        }

        {
            SimulatedBufferCache sw1_forced_off{sw1, /*has_gpu_memory=*/true,
                                                /*uses_native_tiling=*/false,
                                                /*can_trap_cpu_reads=*/true,
                                                /*env_override=*/false};
            sw1_forced_off.PrepareHostMappedRange(buf, size);
            CHECK_FALSE(sw1_forced_off.AsyncBufferDownloads());
            CHECK(sw1_forced_off.FlushesOnCpuRead());
            sw1_forced_off.GpuWrite(buf, size);
            CHECK_FALSE(sw1_forced_off.DownloadsAtFence(buf, size));
            CHECK(sw1_forced_off.HasGpuWritesNotDownloadedAtFence(buf, size));
            CHECK(sw1_forced_off.Fence().downloaded_ranges.empty());
            const auto read = sw1_forced_off.CpuRead(buf, size);
            CHECK(read.trap_fired);
            CHECK(read.flushed_synchronously);
            CHECK(sw1_forced_off.memory_tracker.IsRegionPreflushable(buf, size));
        }
    }
}

TEST_CASE("Repeating GPU write -> fence -> CPU read on Preflushable range under Switch 2",
          "[video_core][buffer-download-policy]") {
    const auto sw2 = Core::MakePlatformProfile(Core::PlatformResolution::ConfirmedSwitch2);
    constexpr DAddr buf = BASE;
    constexpr u64 size = PAGE * 2;

    SECTION("Previously CPU-read range downloads at fence and avoids repeat CPU flush stalls") {
        SimulatedBufferCache cache{sw2, /*has_gpu_memory=*/true, /*uses_native_tiling=*/true,
                                   /*can_trap_cpu_reads=*/true};
        cache.PrepareHostMappedRange(buf, size);

        // Pass 1: GPU write -> fence (no download) -> CPU read (flushes and marks Preflushable).
        cache.GpuWrite(buf, size);
        REQUIRE_FALSE(cache.DownloadsAtFence(buf, size));
        REQUIRE(cache.HasGpuWritesNotDownloadedAtFence(buf, size));
        CHECK(cache.DownloadsAtFence(buf, size) ==
              !cache.HasGpuWritesNotDownloadedAtFence(buf, size));
        REQUIRE(cache.Fence().downloaded_ranges.empty());

        const auto first_read = cache.CpuRead(buf, size);
        REQUIRE(first_read.trap_fired);
        REQUIRE(first_read.flushed_synchronously);
        REQUIRE(cache.memory_tracker.IsRegionPreflushable(buf, size));

        // Pass 2: Repeating GPU write on the now-Preflushable range downloads at the fence.
        const size_t arms_before = cache.device_tracker.ArmCalls();
        cache.GpuWrite(buf, size);
        CHECK(cache.DownloadsAtFence(buf, size));
        CHECK_FALSE(cache.HasGpuWritesNotDownloadedAtFence(buf, size));
        CHECK(cache.DownloadsAtFence(buf, size) ==
              !cache.HasGpuWritesNotDownloadedAtFence(buf, size));
        CHECK(cache.device_tracker.ArmCalls() == arms_before);
        CHECK(cache.device_tracker.ArmedPageCount() == 0);

        const auto second_fence = cache.Fence();
        CHECK(second_fence.downloaded_ranges == std::vector<Range>{{buf, size}});
        CHECK_FALSE(cache.IsGpuModifiedInGuestView(buf, size));

        const auto second_read = cache.CpuRead(buf, size);
        CHECK_FALSE(second_read.trap_fired);
        CHECK_FALSE(second_read.flushed_synchronously);
        CHECK(second_read.preemptive);
    }

    SECTION("PreflushAging re-arms Preflushable pages and skips synchronous flush on read") {
        SimulatedBufferCache cache{sw2, /*has_gpu_memory=*/true, /*uses_native_tiling=*/true,
                                   /*can_trap_cpu_reads=*/true};
        cache.preflush_aging = true;
        cache.PrepareHostMappedRange(buf, size);

        cache.GpuWrite(buf, size);
        REQUIRE(cache.Fence().downloaded_ranges.empty());
        REQUIRE(cache.CpuRead(buf, size).flushed_synchronously);

        cache.GpuWrite(buf, size);
        CHECK(cache.DownloadsAtFence(buf, size));
        CHECK_FALSE(cache.HasGpuWritesNotDownloadedAtFence(buf, size));
        CHECK(cache.DownloadsAtFence(buf, size) ==
              !cache.HasGpuWritesNotDownloadedAtFence(buf, size));
        CHECK(cache.device_tracker.IsPageTrapArmed(buf));
        CHECK(cache.device_tracker.IsPageTrapArmed(buf + PAGE));
        CHECK(cache.Fence().downloaded_ranges == std::vector<Range>{{buf, size}});

        const auto aged_read = cache.CpuRead(buf, size);
        CHECK(aged_read.trap_fired);
        CHECK_FALSE(aged_read.flushed_synchronously);
        CHECK(aged_read.preemptive);
    }

    SECTION("Mixed Preflushable and cold sub-ranges download only Preflushable parts at fence") {
        SimulatedBufferCache cache{sw2, /*has_gpu_memory=*/true, /*uses_native_tiling=*/true,
                                   /*can_trap_cpu_reads=*/true};
        cache.PrepareHostMappedRange(buf, size);

        // Only page 0 was previously CPU-read.
        cache.GpuWrite(buf, PAGE);
        REQUIRE(cache.CpuRead(buf, PAGE).flushed_synchronously);

        // Next GPU write touches both page 0 (Preflushable) and page 1 (cold).
        cache.GpuWrite(buf, size);
        CHECK(cache.DownloadsAtFence(buf, PAGE));
        CHECK_FALSE(cache.HasGpuWritesNotDownloadedAtFence(buf, PAGE));
        CHECK(cache.DownloadsAtFence(buf, PAGE) ==
              !cache.HasGpuWritesNotDownloadedAtFence(buf, PAGE));

        CHECK_FALSE(cache.DownloadsAtFence(buf + PAGE, PAGE));
        CHECK(cache.HasGpuWritesNotDownloadedAtFence(buf + PAGE, PAGE));
        CHECK(cache.DownloadsAtFence(buf + PAGE, PAGE) ==
              !cache.HasGpuWritesNotDownloadedAtFence(buf + PAGE, PAGE));

        // Combined span still reports un-downloaded GPU writes on page 1 so a span read flushes.
        CHECK(cache.HasGpuWritesNotDownloadedAtFence(buf, size));
        CHECK_FALSE(cache.device_tracker.IsPageTrapArmed(buf));
        CHECK(cache.device_tracker.IsPageTrapArmed(buf + PAGE));

        const auto fence = cache.Fence();
        CHECK(fence.downloaded_ranges == std::vector<Range>{{buf, PAGE}});
        CHECK_FALSE(cache.IsGpuModifiedInGuestView(buf, PAGE));
        CHECK(cache.IsGpuModifiedInGuestView(buf + PAGE, PAGE));

        // Reading across the combined span flushes page 1 and promotes it to Preflushable.
        const auto span_read = cache.CpuRead(buf, size);
        CHECK(span_read.trap_fired);
        CHECK(span_read.flushed_synchronously);
        CHECK_FALSE(span_read.preemptive);

        cache.GpuWrite(buf, size);
        CHECK(cache.DownloadsAtFence(buf, size));
        CHECK_FALSE(cache.HasGpuWritesNotDownloadedAtFence(buf, size));
        CHECK(cache.DownloadsAtFence(buf, size) ==
              !cache.HasGpuWritesNotDownloadedAtFence(buf, size));
        CHECK(cache.Fence().downloaded_ranges == std::vector<Range>{{buf, size}});
    }
}

namespace {

class MockIntervalMemoryTracker {
public:
    std::vector<std::pair<u64, u64>> download_ranges;
    std::vector<std::pair<u64, u64>> preflushable_ranges;
    size_t download_queries{};
    size_t preflushable_queries{};

    template <typename Func>
    void ForEachDownloadRange(DAddr, u64, bool, Func&& func) {
        ++download_queries;
        for (const auto& [start, range_size] : download_ranges) {
            func(start, range_size);
        }
    }

    template <typename Func>
    void ForEachPreflushableRange(DAddr, u64, Func&& func) {
        ++preflushable_queries;
        for (const auto& [start, range_size] : preflushable_ranges) {
            func(start, range_size);
        }
    }
};

} // namespace

TEST_CASE("DispatchGpuWriteDownloadPolicy guards size == 0 across policy configurations",
          "[video_core][buffer-download-policy]") {
    using VideoCommon::BufferDownloadPolicyInputs;
    using VideoCommon::DispatchGpuWriteDownloadPolicy;

    const auto sw1 = Core::MakePlatformProfile(Core::PlatformResolution::ConfirmedSwitch1);
    const auto sw2 = Core::MakePlatformProfile(Core::PlatformResolution::ConfirmedSwitch2);

    for (const auto& profile : {sw1, sw2}) {
        for (const u64 small_limit : {u64{0}, u64{4096}}) {
            for (const bool preflush_aging : {false, true}) {
                MockIntervalMemoryTracker tracker;
                const BufferDownloadPolicyInputs inputs{
                    .profile = profile,
                    .has_gpu_memory = true,
                    .uses_native_tiling = true,
                    .can_trap_cpu_reads = true,
                };
                size_t download_calls{};
                size_t trap_calls{};
                DispatchGpuWriteDownloadPolicy(
                    inputs, small_limit, preflush_aging, tracker, BASE, 0,
                    [&](DAddr, u64) { ++download_calls; },
                    [&](DAddr, u64) { ++trap_calls; });
                CHECK(download_calls == 0);
                CHECK(trap_calls == 0);
                CHECK(tracker.preflushable_queries == 0);
            }
        }
    }

    SimulatedBufferCache cache{sw2, /*has_gpu_memory=*/true, /*uses_native_tiling=*/true,
                               /*can_trap_cpu_reads=*/true};
    cache.GpuWrite(BASE, 0);
    CHECK(cache.device_tracker.ArmCalls() == 0);
    CHECK_FALSE(cache.HasPendingFenceDownloads());
}

TEST_CASE("EvaluateHasGpuWritesNotDownloadedAtFence handles interval union and size == 0",
          "[video_core][buffer-download-policy]") {
    using VideoCommon::BufferDownloadPolicyInputs;
    using VideoCommon::EvaluateHasGpuWritesNotDownloadedAtFence;

    const BufferDownloadPolicyInputs sw2_inputs{
        .profile = Core::MakePlatformProfile(Core::PlatformResolution::ConfirmedSwitch2),
        .has_gpu_memory = true,
        .uses_native_tiling = true,
        .can_trap_cpu_reads = true,
    };

    SECTION("size == 0 short-circuits without querying the memory tracker") {
        MockIntervalMemoryTracker tracker;
        tracker.download_ranges.emplace_back(BASE, PAGE);
        CHECK_FALSE(EvaluateHasGpuWritesNotDownloadedAtFence(sw2_inputs, tracker, BASE, 0));
        CHECK(tracker.download_queries == 0);
        CHECK(tracker.preflushable_queries == 0);

        SimulatedBufferCache cache{sw2_inputs.profile, /*has_gpu_memory=*/true,
                                   /*uses_native_tiling=*/true, /*can_trap_cpu_reads=*/true};
        cache.PrepareHostMappedRange(BASE, PAGE);
        cache.GpuWrite(BASE, PAGE);
        CHECK(cache.HasGpuWritesNotDownloadedAtFence(BASE, PAGE));
        CHECK_FALSE(cache.HasGpuWritesNotDownloadedAtFence(BASE, 0));
    }

    SECTION("Unsorted overlapping preflushable intervals covering full download range return false") {
        MockIntervalMemoryTracker tracker;
        tracker.download_ranges.emplace_back(0x1000, 0x3000);
        // Reported out of order and overlapping across [0x2000, 0x2800).
        tracker.preflushable_ranges.emplace_back(0x2000, 0x2000);
        tracker.preflushable_ranges.emplace_back(0x1000, 0x1800);
        CHECK_FALSE(
            EvaluateHasGpuWritesNotDownloadedAtFence(sw2_inputs, tracker, 0x1000, 0x3000));
    }

    SECTION("Overlapping intervals exceeding range length total still detect uncovered tail") {
        MockIntervalMemoryTracker tracker;
        tracker.download_ranges.emplace_back(0x1000, 0x3000);
        // Overlapping intervals sum to 0x3000 == range length, yet leave [0x3000, 0x4000) uncovered.
        tracker.preflushable_ranges.emplace_back(0x1000, 0x2000);
        tracker.preflushable_ranges.emplace_back(0x1400, 0x1000);
        CHECK(EvaluateHasGpuWritesNotDownloadedAtFence(sw2_inputs, tracker, 0x1000, 0x3000));
    }

    SECTION("Overlapping intervals with a middle or leading gap return true") {
        MockIntervalMemoryTracker gap_tracker;
        gap_tracker.download_ranges.emplace_back(0x1000, 0x3000);
        gap_tracker.preflushable_ranges.emplace_back(0x1000, 0x1000);
        gap_tracker.preflushable_ranges.emplace_back(0x1200, 0x0400);
        gap_tracker.preflushable_ranges.emplace_back(0x2800, 0x1800);
        CHECK(EvaluateHasGpuWritesNotDownloadedAtFence(sw2_inputs, gap_tracker, 0x1000, 0x3000));

        MockIntervalMemoryTracker leading_gap_tracker;
        leading_gap_tracker.download_ranges.emplace_back(0x1000, 0x3000);
        leading_gap_tracker.preflushable_ranges.emplace_back(0x1800, 0x2800);
        CHECK(EvaluateHasGpuWritesNotDownloadedAtFence(sw2_inputs, leading_gap_tracker, 0x1000,
                                                       0x3000));
    }

    SECTION("Out-of-bounds extending intervals are clamped cleanly without missing gaps") {
        MockIntervalMemoryTracker clamped_tracker;
        clamped_tracker.download_ranges.emplace_back(0x1000, 0x3000);
        clamped_tracker.preflushable_ranges.emplace_back(0x0000, 0x2000);
        clamped_tracker.preflushable_ranges.emplace_back(0x5000, 0x1000);
        clamped_tracker.preflushable_ranges.emplace_back(0x2000, 0x0000);
        clamped_tracker.preflushable_ranges.emplace_back(0x1800, 0x4000);
        CHECK_FALSE(
            EvaluateHasGpuWritesNotDownloadedAtFence(sw2_inputs, clamped_tracker, 0x1000, 0x3000));
    }
}
