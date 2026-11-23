// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <ranges>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/container/unordered_map.h"
#include "common/thread.h"
#include "video_core/nx2_perf.h"

namespace Tegra {

// Set once a Switch 2 (Ampere, NVN2) driver is talking to the GPU. Its command streams use
// Ampere classes and the Turing+ macro ISA, which Eden's Maxwell engines don't implement.
inline std::atomic<bool> g_nx2_ampere{false};

// Device pages the GPU has written through the buffer cache (shader stores, copies), ever. Title 1
// runs command lists the GPU writes each frame (20-byte entries its culling fills in); read
// without a flush they are stale and the draws behind them (trees) go missing. The bits never
// clear: the dispatch that rewrites a page may still be queued on the split's back when its front
// reads the command list. One bit per 4 KiB of the 36-bit device address space (2 MiB).
inline constexpr u64 Nx2WrittenPageBits = 12;
inline std::array<std::atomic<u64>, (u64{1} << (36 - Nx2WrittenPageBits)) / 64> g_nx2_gpu_written_pages{};

// The same, one bit per 1 MiB: the split front's early MME DMA reads fall back to the drain for
// any read in a region the GPU has written. A queued dispatch can write a page of such a region
// for the first time before its bit is set (Ocarina's indirect draw arguments did).
inline constexpr u64 Nx2WrittenRegionBits = 20;
inline std::array<std::atomic<u64>, (u64{1} << (36 - Nx2WrittenRegionBits)) / 64>
    g_nx2_gpu_written_regions{};

[[nodiscard]] constexpr u64 Nx2SaturatingLastByte(u64 addr, u64 size) noexcept {
    if (size == 0) {
        return addr;
    }
    constexpr u64 MaxAddr = (std::numeric_limits<u64>::max)();
    return size - 1 <= MaxAddr - addr ? addr + (size - 1) : MaxAddr;
}

[[nodiscard]] constexpr u64 Nx2SaturatingEnd(u64 addr, u64 size) noexcept {
    constexpr u64 MaxAddr = (std::numeric_limits<u64>::max)();
    return size <= MaxAddr - addr ? addr + size : MaxAddr;
}

inline void Nx2MarkGpuWritten(u64 device_address, u64 size) {
    if (size == 0) {
        return;
    }
    {
        const u64 last_region =
            (std::min)((device_address + size - 1) >> Nx2WrittenRegionBits,
                       u64{g_nx2_gpu_written_regions.size() * 64 - 1});
        for (u64 region = device_address >> Nx2WrittenRegionBits; region <= last_region;
             ++region) {
            auto& word = g_nx2_gpu_written_regions[region / 64];
            const u64 bit = u64{1} << (region % 64);
            if ((word.load(std::memory_order_relaxed) & bit) == 0) {
                word.fetch_or(bit, std::memory_order_relaxed);
            }
        }
    }
    const u64 last = (std::min)((device_address + size - 1) >> Nx2WrittenPageBits,
                                u64{g_nx2_gpu_written_pages.size() * 64 - 1});
    for (u64 page = device_address >> Nx2WrittenPageBits; page <= last;) {
        const u64 first_bit = page % 64;
        const u64 bits = (std::min)(u64{64} - first_bit, last - page + 1);
        const u64 mask = (bits == 64 ? ~u64{0} : (u64{1} << bits) - 1) << first_bit;
        auto& word = g_nx2_gpu_written_pages[page / 64];
        if ((word.load(std::memory_order_relaxed) & mask) != mask) {
            word.fetch_or(mask, std::memory_order_relaxed);
        }
        page += bits;
    }
}

// GPU-written device pages the front has read command lists from (same layout as above). The
// buffer cache copies these pages into host memory right after the compute or copy work that
// writes them and submits, so the front reads the copy once the GPU is done instead of draining
// the split and flushing the caches (BufferCache::Nx2ReadListCopy).
inline std::array<std::atomic<u64>, (u64{1} << (36 - Nx2WrittenPageBits)) / 64> g_nx2_command_list_pages{};
inline std::atomic<bool> g_nx2_has_command_list_pages{false};

// Shaders in graphics pipeline stages that write storage buffers or command list pages.
// Checked on the split GPU thread's front to identify writer draws and note them with split.NoteWriter().
// Sized to 65,536 bits (1,024 64-bit words, 8 KiB), indexed via a fast multiplicative hash of the
// 32-bit low shader address (which is aligned to 128/256 bytes in native Switch 2 drivers).
inline constexpr size_t Nx2WriterShaderBits = 65536;
inline std::array<std::atomic<u64>, Nx2WriterShaderBits / 64> g_nx2_graphics_writer_shaders{};
inline std::atomic<bool> g_nx2_has_graphics_writer_shaders{false};

inline void Nx2MarkGraphicsWriterShader(u32 addr_low) {
    if (addr_low == 0) {
        return;
    }
    const u32 hash = (addr_low >> 6) * 0x9e3779b9u;
    const u32 bit_index = hash & (Nx2WriterShaderBits - 1);
    auto& word = g_nx2_graphics_writer_shaders[bit_index / 64];
    const u64 bit = u64{1} << (bit_index % 64);
    if ((word.load(std::memory_order_acquire) & bit) == 0) {
        word.fetch_or(bit, std::memory_order_release);
    }
    if (!g_nx2_has_graphics_writer_shaders.load(std::memory_order_acquire)) {
        g_nx2_has_graphics_writer_shaders.store(true, std::memory_order_release);
    }
}

[[nodiscard]] inline bool Nx2IsGraphicsWriterShader(u32 addr_low) {
    if (addr_low == 0 || !g_nx2_has_graphics_writer_shaders.load(std::memory_order_acquire)) {
        return false;
    }
    const u32 hash = (addr_low >> 6) * 0x9e3779b9u;
    const u32 bit_index = hash & (Nx2WriterShaderBits - 1);
    const u64 word = g_nx2_graphics_writer_shaders[bit_index / 64].load(std::memory_order_acquire);
    return (word & (u64{1} << (bit_index % 64))) != 0;
}

inline void Nx2MarkCommandListPage(u64 device_address) {
    const u64 page = device_address >> Nx2WrittenPageBits;
    if (page >= g_nx2_command_list_pages.size() * 64) {
        return;
    }
    auto& word = g_nx2_command_list_pages[page / 64];
    const u64 bit = u64{1} << (page % 64);
    if ((word.load(std::memory_order_relaxed) & bit) == 0) {
        word.fetch_or(bit, std::memory_order_relaxed);
        g_nx2_has_command_list_pages.store(true, std::memory_order_relaxed);
    }
}

[[nodiscard]] inline bool Nx2IsCommandListPage(u64 device_address) {
    const u64 page = device_address >> Nx2WrittenPageBits;
    if (page >= g_nx2_command_list_pages.size() * 64) {
        return false;
    }
    return (g_nx2_command_list_pages[page / 64].load(std::memory_order_relaxed) >> (page % 64)) & 1;
}

// Nx2Perf MacroReadWriterSet: graphics draws the back proved write no guest memory, so the
// split front's early MME DMA reads need not wait for them. A proof holds for the exact program
// addresses a draw binds (Nx2DrawPrograms, built from the registers on both threads) and for one
// program epoch: the epoch moves whenever the shader cache drops a program, and whenever a write
// that the shader cache only applies later (CPU writes into GPU-modified buffers, accumulated
// invalidations) reaches a page holding proven code. A proof from an older epoch proves nothing,
// so an address that later holds other code is never trusted. Epoch 0 is never valid.
inline std::atomic<u64> g_nx2_code_epoch{1};

inline void Nx2BumpCodeEpoch() {
    g_nx2_code_epoch.fetch_add(1, std::memory_order_acq_rel);
}

// Device pages (4 KiB) holding code of a proven program (same layout as g_nx2_gpu_written_pages).
inline std::array<std::atomic<u64>, (u64{1} << (36 - Nx2WrittenPageBits)) / 64> g_nx2_proven_code_pages{};
inline std::atomic<bool> g_nx2_has_proven_code{false};

inline void Nx2MarkProvenCode(u64 device_address, u64 size) {
    if (size == 0) {
        return;
    }
    const u64 last_addr = Nx2SaturatingLastByte(device_address, size);
    const u64 last =
        (std::min)(last_addr >> Nx2WrittenPageBits, u64{g_nx2_proven_code_pages.size() * 64 - 1});
    for (u64 page = device_address >> Nx2WrittenPageBits; page <= last; ++page) {
        auto& word = g_nx2_proven_code_pages[page / 64];
        const u64 bit = u64{1} << (page % 64);
        if ((word.load(std::memory_order_acquire) & bit) == 0) {
            word.fetch_or(bit, std::memory_order_release);
        }
    }
    g_nx2_has_proven_code.store(true, std::memory_order_release);
}

[[nodiscard]] inline bool Nx2IsProvenCode(u64 device_address, u64 size) {
    if (size == 0 || !g_nx2_has_proven_code.load(std::memory_order_acquire)) {
        return false;
    }
    const u64 last_addr = Nx2SaturatingLastByte(device_address, size);
    const u64 last =
        (std::min)(last_addr >> Nx2WrittenPageBits, u64{g_nx2_proven_code_pages.size() * 64 - 1});
    for (u64 page = device_address >> Nx2WrittenPageBits; page <= last; ++page) {
        if ((g_nx2_proven_code_pages[page / 64].load(std::memory_order_acquire) >> (page % 64)) & 1) {
            return true;
        }
    }
    return false;
}

/// A write whose invalidation the shader cache applies later reached [device_address, +size).
inline void Nx2NoteDeferredCodeWrite(u64 device_address, u64 size) {
    if (Nx2IsProvenCode(device_address, size)) {
        Nx2BumpCodeEpoch();
    }
}

/// The programs a graphics draw binds, as Maxwell3D::Nx2DrawPrograms computes them from the
/// registers: per program slot, its GPU address (0 when disabled).
struct Nx2DrawPrograms {
    std::array<u64, 6> addresses{};
    u64 memory_id{};
    u64 map_serial{};
    bool native{};

    bool operator==(const Nx2DrawPrograms&) const = default;
};

struct Nx2NonWriterSet {
    struct Hash {
        size_t operator()(const Nx2DrawPrograms& key) const noexcept {
            u64 hash = key.memory_id * 0x9e3779b97f4a7c15ULL ^ key.map_serial ^ (key.native ? 1 : 0);
            for (const u64 address : key.addresses) {
                hash = (hash ^ address) * 0xff51afd7ed558ccdULL;
                hash ^= hash >> 32;
            }
            return static_cast<size_t>(hash);
        }
    };
    std::mutex mutex;
    Common::unordered_map<Nx2DrawPrograms, u64, Hash> proofs;
    std::atomic<u64> version{1}; ///< Moves on every insert (the front caches its lookups)
};

inline Nx2NonWriterSet& Nx2NonWriters() {
    static Nx2NonWriterSet set;
    return set;
}

/// Whether the back proved, in program epoch `epoch`, that a draw binding `programs` writes no
/// guest memory.
[[nodiscard]] inline bool Nx2IsNonWriterDraw(const Nx2DrawPrograms& programs, u64 epoch) {
    auto& set = Nx2NonWriters();
    std::scoped_lock lock{set.mutex};
    const auto it = set.proofs.find(programs);
    return it != set.proofs.end() && it->second == epoch;
}

inline void Nx2InsertNonWriterDraw(const Nx2DrawPrograms& programs, u64 epoch) {
    auto& set = Nx2NonWriters();
    {
        std::scoped_lock lock{set.mutex};
        set.proofs.insert_or_assign(programs, epoch);
    }
    set.version.fetch_add(1, std::memory_order_release);
}

/// A new emulation session starts: no page has been written by its GPU yet.
inline void Nx2ClearGpuWrittenPages() {
    for (auto& word : g_nx2_gpu_written_pages) {
        word.store(0, std::memory_order_relaxed);
    }
    for (auto& word : g_nx2_gpu_written_regions) {
        word.store(0, std::memory_order_relaxed);
    }
    for (auto& word : g_nx2_command_list_pages) {
        word.store(0, std::memory_order_relaxed);
    }
    g_nx2_has_command_list_pages.store(false, std::memory_order_release);
    for (auto& word : g_nx2_proven_code_pages) {
        word.store(0, std::memory_order_relaxed);
    }
    g_nx2_has_proven_code.store(false, std::memory_order_release);
    for (auto& word : g_nx2_graphics_writer_shaders) {
        word.store(0, std::memory_order_relaxed);
    }
    g_nx2_has_graphics_writer_shaders.store(false, std::memory_order_release);
    Nx2BumpCodeEpoch(); // No proof of the last session holds
    {
        auto& set = Nx2NonWriters();
        std::scoped_lock lock{set.mutex};
        set.proofs.clear();
        set.version.fetch_add(1, std::memory_order_release);
    }
}

// Query results written when their host work completes (counter reports, delayed fence
// releases): per 16-byte slot hash, how many result writes the back queued and how many ran.
// Equal counts mean the words in memory are final, so the split's front can read them without
// draining the back (lichii #179, 166c21d96c). A hash collision only makes a read wait longer.
inline constexpr u64 Nx2AsyncReportSlots = u64{1} << 16;

class Nx2AsyncReports {
public:
    static constexpr u64 Slots = Nx2AsyncReportSlots;
    using CounterArray = std::array<std::atomic<u32>, Slots>;

    [[nodiscard]] static constexpr u64 Slot(u64 device_address) noexcept {
        return (device_address >> 4) % Slots;
    }

    static void IssueSlot(CounterArray& issued_counts, u16 slot) noexcept {
        issued_counts[slot].fetch_add(1, std::memory_order_release);
    }

    static void CompleteSlot(CounterArray& completed_counts, u16 slot) noexcept {
        completed_counts[slot].fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] static bool RangeSettled(const CounterArray& issued_counts,
                                           const CounterArray& completed_counts,
                                           u64 device_address, u64 size) noexcept {
        if (size == 0) {
            return true;
        }
        const u64 first_slot = device_address >> 4;
        const u64 end_address =
            device_address <= (std::numeric_limits<u64>::max)() - (size - 1)
                ? device_address + (size - 1)
                : (std::numeric_limits<u64>::max)();
        const u64 last_slot = end_address >> 4;
        const u64 count = (std::min<u64>)(last_slot - first_slot + 1, Slots);
        for (u64 offset = 0; offset < count; ++offset) {
            const size_t index = static_cast<size_t>((first_slot + offset) % Slots);
            if (completed_counts[index].load(std::memory_order_acquire) !=
                issued_counts[index].load(std::memory_order_acquire)) {
                return false;
            }
        }
        return true;
    }

    static void ClearArrays(CounterArray& issued_counts,
                            CounterArray& completed_counts) noexcept {
        for (auto& count : issued_counts) {
            count.store(0, std::memory_order_relaxed);
        }
        for (auto& count : completed_counts) {
            count.store(0, std::memory_order_relaxed);
        }
    }

    void Issue(u16 slot) noexcept {
        IssueSlot(issued, slot);
    }

    void Complete(u16 slot) noexcept {
        CompleteSlot(completed, slot);
    }

    [[nodiscard]] u32 Issued(u16 slot) const noexcept {
        return issued[slot].load(std::memory_order_acquire);
    }

    [[nodiscard]] u32 Completed(u16 slot) const noexcept {
        return completed[slot].load(std::memory_order_acquire);
    }

    [[nodiscard]] bool Settled(u64 device_address, u64 size) const noexcept {
        return RangeSettled(issued, completed, device_address, size);
    }

    void Clear() noexcept {
        ClearArrays(issued, completed);
    }

private:
    CounterArray issued{};
    CounterArray completed{};
};

inline Nx2AsyncReports::CounterArray g_nx2_async_reports_issued{};
inline Nx2AsyncReports::CounterArray g_nx2_async_reports_completed{};

/// A new emulation session starts: counter reports the previous one abandoned are not pending.
inline void Nx2ClearAsyncReports() {
    Nx2AsyncReports::ClearArrays(g_nx2_async_reports_issued, g_nx2_async_reports_completed);
}

[[nodiscard]] inline u64 Nx2AsyncReportSlot(u64 device_address) {
    return Nx2AsyncReports::Slot(device_address);
}

[[nodiscard]] inline bool Nx2AsyncReportsSettled(u64 device_address, u64 size) {
    return Nx2AsyncReports::RangeSettled(g_nx2_async_reports_issued,
                                         g_nx2_async_reports_completed, device_address, size);
}

[[nodiscard]] inline bool Nx2IsGpuWrittenRegion(u64 device_address) {
    const u64 region = device_address >> Nx2WrittenRegionBits;
    if (region >= g_nx2_gpu_written_regions.size() * 64) {
        return false;
    }
    return (g_nx2_gpu_written_regions[region / 64].load(std::memory_order_relaxed) >>
            (region % 64)) &
           1;
}

[[nodiscard]] inline bool Nx2IsGpuWritten(u64 device_address) {
    const u64 page = device_address >> Nx2WrittenPageBits;
    if (page >= g_nx2_gpu_written_pages.size() * 64) {
        return false;
    }
    return (g_nx2_gpu_written_pages[page / 64].load(std::memory_order_relaxed) >> (page % 64)) & 1;
}

// NVN2 queues release their progress values into the second page of a two-page GPU-accessible
// region and GPU acquires read the same offsets in the first page (the CPU polls the second).
// Something on the console copies one into the other; until that is known, the puller mirrors
// releases that land in a second page. Every queue has such a region, so keep a small table.
inline constexpr size_t Nx2MaxSemaphoreRegions = 32;

class Nx2SemaphoreRegions {
public:
    using Table = std::array<std::atomic<u64>, Nx2MaxSemaphoreRegions>;

    static void AddTo(Table& table, u64 start) {
        for (auto& region : table) {
            if (region.load() == start) {
                return;
            }
        }
        for (auto& region : table) {
            u64 expected = 0;
            if (region.compare_exchange_strong(expected, start)) {
                return;
            }
        }
    }

    static void RemoveFrom(Table& table, u64 start) {
        for (auto& region : table) {
            u64 expected = start;
            region.compare_exchange_strong(expected, 0);
        }
    }

    static void ClearTable(Table& table) {
        for (auto& region : table) {
            region.store(0);
        }
    }

    [[nodiscard]] static u64 MirrorIn(const Table& table, u64 address) {
        for (const auto& region : table) {
            const u64 start = region.load(std::memory_order_relaxed);
            if (start == 0) {
                continue;
            }
            if (address >= start + 0x1000 && address < start + 0x2000) {
                return address - 0x1000;
            }
        }
        return 0;
    }

    void Add(u64 start) {
        AddTo(regions, start);
    }

    void Remove(u64 start) {
        RemoveFrom(regions, start);
    }

    void Clear() {
        ClearTable(regions);
    }

    [[nodiscard]] u64 Mirror(u64 address) const {
        return MirrorIn(regions, address);
    }

private:
    Table regions{};
};

inline Nx2SemaphoreRegions::Table g_nx2_semaphore_regions{}; // starts

inline void Nx2AddSemaphoreRegion(u64 start) {
    Nx2SemaphoreRegions::AddTo(g_nx2_semaphore_regions, start);
}

/// The region's memory was destroyed: stop mirroring into it.
inline void Nx2RemoveSemaphoreRegion(u64 start) {
    Nx2SemaphoreRegions::RemoveFrom(g_nx2_semaphore_regions, start);
}

/// A new emulation session starts: no queue has a region yet.
inline void Nx2ClearSemaphoreRegions() {
    Nx2SemaphoreRegions::ClearTable(g_nx2_semaphore_regions);
}

// Switch 2 sparse regions (nx2:gpu 214, word 7 bit 0x200: title 1's terrain texture arrays and
// layer pools, 170 MB-5.8 GB each, of which the game fills a little) get memory per block when the
// GPU first writes there, not in full. nx2:gpu installs the handler and the span its regions cover;
// the GPU memory manager calls Nx2BackSparseWrite before each write, with the bytes it writes
// (blocks nobody wrote that only get zeros need no memory: they read zero already).
using Nx2SparseBackingHandler = std::function<void(u64 gpu_address, u64 size, const u8* data)>;

// Which blocks of each sparse region have been written (got memory), for the texture cache: an
// image over blocks nobody wrote is all zeros and needs no device memory. Its own lock, taken last:
// the texture cache asks with its lock held, and nx2:gpu updates it holding only its region mutex.
struct Nx2SparseRegionState {
    u64 size;
    u64 block_size;
    std::vector<bool> written;
    /// Once the game maps its own memory into the region (nx2:gpu 216): which tiles it maps.
    /// Only those hold data then (writes elsewhere are dropped, as on the console).
    std::vector<bool> mapped;
};

/// The granularity of the game's sparse tiles (nx2:gpu 216 maps 64 KiB pages).
inline constexpr u64 Nx2SparseTileSize = 64ULL << 10;

struct Nx2SparseBackingStorage {
    std::mutex mutex;
    std::condition_variable cv;
    Nx2SparseBackingHandler handler;
    u32 inflight{0};
};

struct Nx2SparseRegionMap {
    mutable std::mutex mutex;
    std::map<u64, Nx2SparseRegionState> regions;
};

namespace Detail {

inline void SetSparseBacking(Nx2SparseBackingStorage& backing, std::atomic<u64>& low,
                             std::atomic<u64>& high, Nx2SparseBackingHandler handler) {
    std::unique_lock lk{backing.mutex};
    const bool clearing = !handler;
    backing.handler = {};
    if (clearing) {
        low.store(~u64{0}, std::memory_order_release);
        high.store(0, std::memory_order_release);
    }
    // Drain any callback already invoked outside the mutex before installing/tearing down.
    backing.cv.wait(lk, [&backing] { return backing.inflight == 0; });
    backing.handler = std::move(handler);
}

inline void AddSparseSpan(std::atomic<u64>& low_ref, std::atomic<u64>& high_ref, u64 gpu_address,
                          u64 size) {
    if (size == 0) {
        return;
    }
    const u64 end = size <= (std::numeric_limits<u64>::max)() - gpu_address
                        ? gpu_address + size
                        : (std::numeric_limits<u64>::max)();
    u64 low = low_ref.load(std::memory_order_acquire);
    while (gpu_address < low &&
           !low_ref.compare_exchange_weak(low, gpu_address, std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
    }
    u64 high = high_ref.load(std::memory_order_acquire);
    while (end > high &&
           !high_ref.compare_exchange_weak(high, end, std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
    }
}

inline void BackSparseWrite(Nx2SparseBackingStorage& backing, const std::atomic<u64>& low,
                            const std::atomic<u64>& high, u64 gpu_address, u64 size,
                            const void* data) {
    if (size == 0) {
        return;
    }
    const u64 end = size <= (std::numeric_limits<u64>::max)() - gpu_address
                        ? gpu_address + size
                        : (std::numeric_limits<u64>::max)();
    if (end <= low.load(std::memory_order_acquire) ||
        gpu_address >= high.load(std::memory_order_acquire)) {
        return;
    }
    Nx2SparseBackingHandler handler;
    {
        std::scoped_lock lk{backing.mutex};
        if (!backing.handler) {
            return;
        }
        handler = backing.handler;
        ++backing.inflight;
    }
    struct InflightGuard {
        Nx2SparseBackingStorage& storage;
        ~InflightGuard() {
            std::scoped_lock lk{storage.mutex};
            if (--storage.inflight == 0) {
                storage.cv.notify_all();
            }
        }
    } guard{backing};
    handler(gpu_address, size, static_cast<const u8*>(data));
}

inline void SparseRegionCreated(Nx2SparseRegionMap& map, u64 gpu_address, u64 size, u64 block_size) {
    if (block_size == 0) {
        return;
    }
    const u64 num_blocks = size == 0 ? 0 : (size - 1) / block_size + 1;
    std::scoped_lock lk{map.mutex};
    map.regions[gpu_address] = {size, block_size, std::vector<bool>(num_blocks)};
}

inline bool RangeOutsideSpan(const std::atomic<u64>& low_ref, const std::atomic<u64>& high_ref,
                             u64 gpu_address, u64 size) noexcept {
    if (size == 0) {
        return true;
    }
    const u64 high = high_ref.load(std::memory_order_acquire);
    const u64 low = low_ref.load(std::memory_order_acquire);
    if (low >= high || gpu_address >= high) {
        return true;
    }
    constexpr u64 MaxAddr = (std::numeric_limits<u64>::max)();
    return size <= MaxAddr - gpu_address && gpu_address + size <= low;
}

inline void SparseRegionTiled(Nx2SparseRegionMap& map, std::atomic<u64>& low,
                              std::atomic<u64>& high, u64 gpu_address, u64 size, u64 block_size) {
    if (block_size == 0) {
        return;
    }
    AddSparseSpan(low, high, gpu_address, size);
    const u64 num_blocks = size == 0 ? 0 : (size - 1) / block_size + 1;
    std::scoped_lock lk{map.mutex};
    if (!map.regions.contains(gpu_address)) {
        map.regions[gpu_address] = {size, block_size, std::vector<bool>(num_blocks)};
    }
}

inline void SparseRegionDestroyed(Nx2SparseRegionMap& map, u64 gpu_address) {
    std::scoped_lock lk{map.mutex};
    map.regions.erase(gpu_address);
}

inline void ClearSparseRegions(Nx2SparseRegionMap& map, std::atomic<u64>& low,
                               std::atomic<u64>& high) {
    low.store(~u64{0}, std::memory_order_release);
    high.store(0, std::memory_order_release);
    std::scoped_lock lk{map.mutex};
    map.regions.clear();
}

inline void SparseBlockWritten(Nx2SparseRegionMap& map, u64 region_address, u64 block) {
    std::scoped_lock lk{map.mutex};
    if (const auto it = map.regions.find(region_address);
        it != map.regions.end() && block < it->second.written.size()) {
        it->second.written[block] = true;
    }
}

inline bool SparseBlockIsWritten(const Nx2SparseRegionMap& map, u64 region_address, u64 block) {
    std::scoped_lock lk{map.mutex};
    const auto it = map.regions.find(region_address);
    return it == map.regions.end() || block >= it->second.written.size() ||
           it->second.written[block];
}

inline void SparseTilesMapped(Nx2SparseRegionMap& map, u64 region_address, u64 offset, u64 size,
                              bool mapped) {
    std::scoped_lock lk{map.mutex};
    const auto it = map.regions.find(region_address);
    if (it == map.regions.end() || size == 0) {
        return;
    }
    auto& region = it->second;
    if (region.size == 0) {
        return;
    }
    if (region.mapped.empty()) {
        region.mapped.resize((region.size - 1) / Nx2SparseTileSize + 1);
    }
    const u64 first = offset / Nx2SparseTileSize;
    if (first >= region.mapped.size()) {
        return;
    }
    const u64 last_offset = Nx2SaturatingLastByte(offset, size);
    const u64 last = (std::min)(last_offset / Nx2SparseTileSize, u64{region.mapped.size() - 1});
    for (u64 tile = first; tile <= last; ++tile) {
        region.mapped[tile] = mapped;
    }
}

inline std::vector<std::pair<u64, u64>> SparseDataRanges(const Nx2SparseRegionMap& map,
                                                         u64 gpu_address, u64 size) {
    std::vector<std::pair<u64, u64>> ranges;
    if (size == 0) {
        return ranges;
    }
    std::scoped_lock lk{map.mutex};
    auto it = map.regions.upper_bound(gpu_address);
    if (it == map.regions.begin()) {
        return ranges;
    }
    --it;
    const auto& [start, region] = *it;
    const u64 begin = gpu_address - start;
    if (size > region.size || begin > region.size - size) {
        return ranges;
    }
    const bool by_tile = !region.mapped.empty();
    const u64 unit = by_tile ? Nx2SparseTileSize : region.block_size;
    if (unit == 0) {
        return ranges;
    }
    const std::vector<bool>& present = by_tile ? region.mapped : region.written;
    const u64 end = begin + size;
    for (u64 index = begin / unit; index < present.size() && index * unit < end; ++index) {
        if (!present[index]) {
            continue;
        }
        const u64 run_begin = (std::max)(index * unit, begin) - begin;
        const u64 block_end = (index + 1) <= end / unit ? (index + 1) * unit : end;
        const u64 run_end = (std::min)(block_end, end) - begin;
        if (!ranges.empty() && ranges.back().first + ranges.back().second == run_begin) {
            ranges.back().second += run_end - run_begin;
        } else {
            ranges.emplace_back(run_begin, run_end - run_begin);
        }
    }
    return ranges;
}

inline bool InSparseRegion(const Nx2SparseRegionMap& map, const std::atomic<u64>& low,
                           const std::atomic<u64>& high, u64 gpu_address, u64 size) {
    if (RangeOutsideSpan(low, high, gpu_address, size)) {
        return false;
    }
    std::scoped_lock lk{map.mutex};
    auto it = map.regions.upper_bound(gpu_address);
    if (it == map.regions.begin()) {
        return false;
    }
    --it;
    const u64 offset = gpu_address - it->first;
    return size <= it->second.size && offset <= it->second.size - size;
}

inline void SparseMarkWritten(Nx2SparseRegionMap& map, const std::atomic<u64>& low,
                              const std::atomic<u64>& high, u64 gpu_address, u64 size) {
    if (RangeOutsideSpan(low, high, gpu_address, size)) {
        return;
    }
    const u64 req_last = Nx2SaturatingLastByte(gpu_address, size);
    std::scoped_lock lk{map.mutex};
    for (auto& [start, region] : map.regions) {
        if (region.size == 0 || region.block_size == 0 || region.written.empty()) {
            continue;
        }
        const u64 region_last = Nx2SaturatingLastByte(start, region.size);
        if (gpu_address > region_last || req_last < start) {
            continue;
        }
        const u64 first = ((std::max)(gpu_address, start) - start) / region.block_size;
        const u64 last = (std::min)(((std::min)(req_last, region_last) - start) / region.block_size,
                                    u64{region.written.size() - 1});
        for (u64 block = first; block <= last; ++block) {
            region.written[block] = true;
        }
    }
}

inline bool SparseUnwritten(const Nx2SparseRegionMap& map, const std::atomic<u64>& low,
                            const std::atomic<u64>& high, u64 gpu_address, u64 size) {
    if (RangeOutsideSpan(low, high, gpu_address, size)) {
        return false;
    }
    std::scoped_lock lk{map.mutex};
    auto it = map.regions.upper_bound(gpu_address);
    if (it == map.regions.begin()) {
        return false;
    }
    --it;
    const auto& [start, region] = *it;
    const u64 offset = gpu_address - start;
    if (size > region.size || offset > region.size - size || region.block_size == 0 ||
        region.written.empty()) {
        return false;
    }
    const u64 first = offset / region.block_size;
    const u64 last = (std::min)((offset + size - 1) / region.block_size,
                                u64{region.written.size() - 1});
    for (u64 block = first; block <= last; ++block) {
        if (region.written[block]) {
            return false;
        }
    }
    return true;
}

} // namespace Detail

class Nx2SparseState {
public:
    void SetBacking(Nx2SparseBackingHandler handler) {
        Detail::SetSparseBacking(backing, low, high, std::move(handler));
    }

    void AddSpan(u64 gpu_address, u64 size) {
        Detail::AddSparseSpan(low, high, gpu_address, size);
    }

    void BackWrite(u64 gpu_address, u64 size, const void* data) {
        Detail::BackSparseWrite(backing, low, high, gpu_address, size, data);
    }

    void RegionCreated(u64 gpu_address, u64 size, u64 block_size) {
        Detail::SparseRegionCreated(region_map, gpu_address, size, block_size);
    }

    void RegionTiled(u64 gpu_address, u64 size, u64 block_size) {
        Detail::SparseRegionTiled(region_map, low, high, gpu_address, size, block_size);
    }

    void RegionDestroyed(u64 gpu_address) {
        Detail::SparseRegionDestroyed(region_map, gpu_address);
    }

    void Clear() {
        Detail::ClearSparseRegions(region_map, low, high);
        SetBacking({});
    }

    void BlockWritten(u64 region_address, u64 block) {
        Detail::SparseBlockWritten(region_map, region_address, block);
    }

    [[nodiscard]] bool BlockIsWritten(u64 region_address, u64 block) const {
        return Detail::SparseBlockIsWritten(region_map, region_address, block);
    }

    void TilesMapped(u64 region_address, u64 offset, u64 size, bool mapped) {
        Detail::SparseTilesMapped(region_map, region_address, offset, size, mapped);
    }

    [[nodiscard]] std::vector<std::pair<u64, u64>> DataRanges(u64 gpu_address, u64 size) const {
        return Detail::SparseDataRanges(region_map, gpu_address, size);
    }

    [[nodiscard]] bool InRegion(u64 gpu_address, u64 size) const {
        return Detail::InSparseRegion(region_map, low, high, gpu_address, size);
    }

    void MarkWritten(u64 gpu_address, u64 size) {
        Detail::SparseMarkWritten(region_map, low, high, gpu_address, size);
    }

    [[nodiscard]] bool Unwritten(u64 gpu_address, u64 size) const {
        return Detail::SparseUnwritten(region_map, low, high, gpu_address, size);
    }

    [[nodiscard]] bool OverlapsSpan(u64 gpu_address, u64 size) const noexcept {
        return !Detail::RangeOutsideSpan(low, high, gpu_address, size);
    }

    [[nodiscard]] u64 Low() const noexcept {
        return low.load(std::memory_order_acquire);
    }

    [[nodiscard]] u64 High() const noexcept {
        return high.load(std::memory_order_acquire);
    }

private:
    Nx2SparseBackingStorage backing;
    Nx2SparseRegionMap region_map;
    std::atomic<u64> low{~u64{0}};
    std::atomic<u64> high{0};
};

inline Nx2SparseBackingStorage g_nx2_sparse_backing_storage;
inline Nx2SparseRegionMap g_nx2_sparse_region_map;
inline std::atomic<u64> g_nx2_sparse_low{~u64{0}};
inline std::atomic<u64> g_nx2_sparse_high{0};

inline void Nx2SetSparseBacking(Nx2SparseBackingHandler handler) {
    Detail::SetSparseBacking(g_nx2_sparse_backing_storage, g_nx2_sparse_low, g_nx2_sparse_high,
                             std::move(handler));
}

/// Widens the span that writes are checked against (a new lazily backed region).
inline void Nx2AddSparseSpan(u64 gpu_address, u64 size) {
    Detail::AddSparseSpan(g_nx2_sparse_low, g_nx2_sparse_high, gpu_address, size);
}

inline void Nx2BackSparseWrite(u64 gpu_address, u64 size, const void* data) {
    Detail::BackSparseWrite(g_nx2_sparse_backing_storage, g_nx2_sparse_low, g_nx2_sparse_high,
                            gpu_address, size, data);
}

inline void Nx2SparseRegionCreated(u64 gpu_address, u64 size, u64 block_size) {
    Detail::SparseRegionCreated(g_nx2_sparse_region_map, gpu_address, size, block_size);
}

/// An initially GPU-only pool now has explicit tiles. Let the renderer bind only resident tiles,
/// while preserving the written/mapped state of pools that were sparse from creation.
inline void Nx2SparseRegionTiled(u64 gpu_address, u64 size, u64 block_size) {
    Detail::SparseRegionTiled(g_nx2_sparse_region_map, g_nx2_sparse_low, g_nx2_sparse_high,
                              gpu_address, size, block_size);
}

inline void Nx2SparseRegionDestroyed(u64 gpu_address) {
    Detail::SparseRegionDestroyed(g_nx2_sparse_region_map, gpu_address);
}

/// A new emulation session: the previous game's regions are gone (nvdrv's Module clears them).
inline void Nx2ClearSparseRegions() {
    Detail::ClearSparseRegions(g_nx2_sparse_region_map, g_nx2_sparse_low, g_nx2_sparse_high);
}

inline void Nx2SparseBlockWritten(u64 region_address, u64 block) {
    Detail::SparseBlockWritten(g_nx2_sparse_region_map, region_address, block);
}

/// True unless the block of the region at region_address is known to hold nothing (no write from
/// the CPU or the GPU yet).
inline bool Nx2SparseBlockIsWritten(u64 region_address, u64 block) {
    return Detail::SparseBlockIsWritten(g_nx2_sparse_region_map, region_address, block);
}

/// The game maps its own memory over [offset, +size) of the region at region_address (mapped), or
/// unmaps it there.
inline void Nx2SparseTilesMapped(u64 region_address, u64 offset, u64 size, bool mapped) {
    Detail::SparseTilesMapped(g_nx2_sparse_region_map, region_address, offset, size, mapped);
}

/// The parts of [gpu_address, +size), which lies in one sparse region, that can hold data: the
/// tiles the game maps there if it maps any, otherwise the blocks written. As (offset from
/// gpu_address, size) runs.
inline std::vector<std::pair<u64, u64>> Nx2SparseDataRanges(u64 gpu_address, u64 size) {
    return Detail::SparseDataRanges(g_nx2_sparse_region_map, gpu_address, size);
}

/// True when [gpu_address, +size) lies in one sparse region.
inline bool Nx2InSparseRegion(u64 gpu_address, u64 size) {
    return Detail::InSparseRegion(g_nx2_sparse_region_map, g_nx2_sparse_low, g_nx2_sparse_high,
                                  gpu_address, size);
}

/// The GPU renders into [gpu_address, +size): count its blocks as written, so images made there
/// later get memory (their contents live in the host image, not in guest memory).
inline void Nx2SparseMarkWritten(u64 gpu_address, u64 size) {
    Detail::SparseMarkWritten(g_nx2_sparse_region_map, g_nx2_sparse_low, g_nx2_sparse_high,
                              gpu_address, size);
}

/// True when [gpu_address, +size) lies in one sparse region and none of it has been written.
inline bool Nx2SparseUnwritten(u64 gpu_address, u64 size) {
    return Detail::SparseUnwritten(g_nx2_sparse_region_map, g_nx2_sparse_low, g_nx2_sparse_high,
                                   gpu_address, size);
}

/// GPU VA in the first page that mirrors a release at `address`, or 0.
inline u64 Nx2SemaphoreMirror(u64 address) {
    return Nx2SemaphoreRegions::MirrorIn(g_nx2_semaphore_regions, address);
}

/// pending_wake: the GPU thread's 1 ms sleep while a channel waits on an acquire ends at once
/// when a command is queued or a release latches a waiter. The timeout bounds a missed wake.
namespace Nx2PendingWake {
enum Reason : u32 { ByPush = 1, ByLatch = 2 };

inline bool On() {
    static const bool on = VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::PendingWake);
    return on;
}

inline Common::Event wake_event;
inline std::atomic<bool> front_sleeping{};
inline std::atomic<u32> wake_reasons{};
inline std::atomic<u64> push_seq{};
inline std::atomic<u64> latch_seq{};
inline std::atomic<s64> last_latch_ns{}; ///< Steady clock of the last latch (0: consumed)
/// EDEN_NX2_PERF_VERIFY: resumes after a wake with the word unmet and nothing latched.
inline std::atomic<u64> verify_unmet{};
/// GPU thread only: the next pending poll follows a wake (Verify checks its resumes).
inline bool verify_resume{};

inline s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Bumps `seq`, then wakes the front if it sleeps. The front sets front_sleeping before it
/// re-reads the counters (both seq_cst): it sees the bump or this sees the flag.
inline void Wake(std::atomic<u64>& seq, u32 reason) {
    seq.fetch_add(1, std::memory_order_seq_cst);
    if (front_sleeping.load(std::memory_order_seq_cst)) {
        wake_reasons.fetch_or(reason, std::memory_order_relaxed);
        wake_event.Set();
    }
}
} // namespace Nx2PendingWake

/// Channels suspended on a semaphore acquire. A console channel polls and sees every release;
/// Eden writes releases in bursts when fences signal and resumes waiters only between command
/// lists, so an acquire-equal could miss its value. Writes latch the waiters they satisfy.
class Nx2SemaphoreWaiters {
public:
    // Puller GpuSemaphoreOperation values.
    static constexpr u32 AcquireEqual = 0x1;
    static constexpr u32 AcquireGequal = 0x4;
    static constexpr u32 AcquireMask = 0x8;

    static Nx2SemaphoreWaiters& Get() {
        static Nx2SemaphoreWaiters waiters;
        return waiters;
    }

    void Add(const void* owner, u64 device_address, u32 value, u32 operation) {
        std::scoped_lock lk{mutex};
        std::erase_if(waiters, [owner](const Waiter& w) { return w.owner == owner; });
        waiters.push_back({owner, device_address, value, operation, false});
        count.store(waiters.size(), std::memory_order_release);
    }

    void Remove(const void* owner) {
        if (count.load(std::memory_order_acquire) == 0) {
            return;
        }
        std::scoped_lock lk{mutex};
        std::erase_if(waiters, [owner](const Waiter& w) { return w.owner == owner; });
        count.store(waiters.size(), std::memory_order_release);
    }

    void Clear() {
        std::scoped_lock lk{mutex};
        waiters.clear();
        count.store(0, std::memory_order_release);
    }

    [[nodiscard]] size_t Size() const noexcept {
        return count.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool Latched(const void* owner) {
        if (count.load(std::memory_order_acquire) == 0) {
            return false;
        }
        std::scoped_lock lk{mutex};
        return std::ranges::any_of(waiters,
                                   [owner](const Waiter& w) { return w.owner == owner && w.latched; });
    }

    /// A GPU write of `value` to the u32 at `device_address`.
    void OnWrite(u64 device_address, u32 value) {
        if (count.load(std::memory_order_acquire) == 0) {
            return;
        }
        bool latched = false;
        {
            std::scoped_lock lk{mutex};
            for (auto& w : waiters) {
                if (w.address == device_address && !w.latched && Matches(w, value)) {
                    w.latched = true;
                    latched = true;
                }
            }
        }
        if (latched && Nx2PendingWake::On()) {
            Nx2PendingWake::last_latch_ns.store(Nx2PendingWake::NowNs(), std::memory_order_relaxed);
            Nx2PendingWake::Wake(Nx2PendingWake::latch_seq, Nx2PendingWake::ByLatch);
        }
    }

private:
    struct Waiter {
        const void* owner;
        u64 address;
        u32 value;
        u32 operation;
        bool latched;
    };

    static bool Matches(const Waiter& w, u32 current) {
        switch (w.operation) {
        case AcquireEqual:
            return current == w.value;
        case AcquireGequal:
            return current >= w.value;
        case AcquireMask:
            return (current & w.value) != 0;
        default:
            return false;
        }
    }

    std::mutex mutex;
    std::vector<Waiter> waiters;
    std::atomic<size_t> count{};
};

} // namespace Tegra
