// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
#include <vector>

#include <boost/container/deque.hpp>

#include "common/address_space.h"
#include "common/address_space.inc"
#include "common/alignment.h"
#include "common/assert.h"
#include "common/div_ceil.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/device_memory.h"
#include "core/device_memory_manager.h"
#include "core/memory.h"
#include "video_core/nx2_perf.h"

namespace Core {

namespace {

class MultiAddressContainer {
public:
    MultiAddressContainer() = default;
    ~MultiAddressContainer() = default;

    void GatherValues(u32 start_entry, Common::ScratchBuffer<u32>& buffer) {
        buffer.resize(8);
        const auto add_value = [&buffer](u32 value, size_t index) {
            if (buffer.size() < index + 1)
                buffer.resize(index + 8);
            buffer[index++] = value;
            return index;
        };

        size_t index = 0;
        u32 iter_entry = start_entry;
        Entry* current = &storage[iter_entry - 1];
        index = add_value(current->value, index);
        while (current->next_entry != 0) {
            iter_entry = current->next_entry;
            current = &storage[iter_entry - 1];
            index = add_value(current->value, index);
        }
        buffer.resize(index);
    }

    u32 Register(u32 value) {
        return RegisterImplementation(value);
    }

    void Register(u32 value, u32 start_entry) {
        auto entry_id = RegisterImplementation(value);
        u32 iter_entry = start_entry;
        Entry* current = &storage[iter_entry - 1];
        while (current->next_entry != 0) {
            iter_entry = current->next_entry;
            current = &storage[iter_entry - 1];
        }
        current->next_entry = entry_id;
    }

    std::pair<bool, u32> Unregister(u32 value, u32 start_entry) {
        u32 iter_entry = start_entry;
        Entry* previous{};
        Entry* current = &storage[iter_entry - 1];
        Entry* next{};
        bool more_than_one_remaining = false;
        u32 result_start{start_entry};
        size_t count = 0;
        while (current->value != value) {
            count++;
            previous = current;
            iter_entry = current->next_entry;
            current = &storage[iter_entry - 1];
        }
        // Find next
        u32 next_entry = current->next_entry;
        if (next_entry != 0) {
            next = &storage[next_entry - 1];
            more_than_one_remaining = next->next_entry != 0 || previous != nullptr;
        }
        if (previous) {
            previous->next_entry = next_entry;
        } else {
            result_start = next_entry;
        }
        free_entries.emplace_back(iter_entry);
        return std::make_pair(more_than_one_remaining || count > 1, result_start);
    }

    u32 ReleaseEntry(u32 start_entry) {
        Entry* current = &storage[start_entry - 1];
        free_entries.emplace_back(start_entry);
        return current->value;
    }

    void Clear() {
        storage.clear();
        free_entries.clear();
    }

private:
    u32 RegisterImplementation(u32 value) {
        auto entry_id = GetNewEntry();
        auto& entry = storage[entry_id - 1];
        entry.next_entry = 0;
        entry.value = value;
        return entry_id;
    }
    u32 GetNewEntry() {
        if (!free_entries.empty()) {
            u32 result = free_entries.front();
            free_entries.pop_front();
            return result;
        }
        storage.emplace_back();
        u32 new_entry = static_cast<u32>(storage.size());
        return new_entry;
    }

    struct Entry {
        u32 next_entry{};
        u32 value{};
    };

    std::vector<Entry> storage;
    boost::container::deque<u32> free_entries;
};

struct EmptyAllocator {
    EmptyAllocator([[maybe_unused]] DAddr address) {}
};

} // namespace

template <typename DTraits>
struct DeviceMemoryManagerAllocator {
    static constexpr size_t max_device_virtual_bits = DTraits::device_virtual_bits;
    static constexpr DAddr first_address = 1ULL << Memory::YUZU_PAGEBITS;

    explicit DeviceMemoryManagerAllocator(
        size_t device_virtual_bits_ = max_device_virtual_bits)
        : device_virtual_bits{device_virtual_bits_}
        , max_device_area{DAddr{1} << device_virtual_bits_} {
        main_allocator.emplace(first_address, max_device_area);
    }

    void Reset(size_t device_virtual_bits_) {
        std::scoped_lock lk(allocator_mutex);
        device_virtual_bits = device_virtual_bits_;
        max_device_area = DAddr{1} << device_virtual_bits_;
        main_allocator.emplace(first_address, max_device_area);
        multi_dev_address.Clear();
    }

    mutable std::mutex allocator_mutex;
    size_t device_virtual_bits{max_device_virtual_bits};
    DAddr max_device_area{DAddr{1} << max_device_virtual_bits};
    std::optional<Common::FlatAllocator<DAddr, 0, max_device_virtual_bits + 1>> main_allocator;
    MultiAddressContainer multi_dev_address;

    /// Returns true when vaddr -> vaddr+size is fully contained in the buffer
    template <bool pin_area>
    [[nodiscard]] bool IsInBounds(VAddr addr, u64 size) const noexcept {
        std::scoped_lock lk(allocator_mutex);
        return addr <= max_device_area && size <= max_device_area - addr;
    }

    DAddr Allocate(size_t size) {
        std::scoped_lock lk(allocator_mutex);
        if (size == 0 || size > max_device_area - first_address) {
            return 0;
        }
        return main_allocator->Allocate(size);
    }

    [[nodiscard]] bool TryAllocateFixed(DAddr b_address, size_t b_size) {
        std::scoped_lock lk(allocator_mutex);
        if (b_size == 0 || b_address < first_address || b_address > max_device_area ||
            b_size > max_device_area - b_address) {
            return false;
        }
        main_allocator->AllocateFixed(b_address, b_size);
        return true;
    }

    void AllocateFixed(DAddr b_address, size_t b_size) {
        static_cast<void>(TryAllocateFixed(b_address, b_size));
    }

    void Free(DAddr b_address, size_t b_size) {
        std::scoped_lock lk(allocator_mutex);
        if (b_size == 0 || b_address < first_address || b_address > max_device_area ||
            b_size > max_device_area - b_address) {
            return;
        }
        main_allocator->Free(b_address, b_size);
    }
};

template <typename Traits>
DeviceMemoryManager<Traits>::DeviceMemoryManager(const DeviceMemory& device_memory_,
                                                 size_t device_virtual_bits_)
    : device_virtual_bits{std::clamp(device_virtual_bits_, min_device_virtual_bits,
                                     max_device_virtual_bits)}
    , device_as_size{DAddr{1} << device_virtual_bits.load(std::memory_order_relaxed)}
    , physical_base{uintptr_t(device_memory_.buffer.BackingBasePointer())}
    , device_memory_ptr{&device_memory_}
    , device_inter{nullptr}
    , compressed_device_addr(1ULL << ((Settings::values.memory_layout_mode.GetValue() == Settings::MemoryLayout::Memory_4Gb ? physical_min_bits : physical_max_bits) - Memory::YUZU_PAGEBITS))
    , tracked_entries(device_as_size.load(std::memory_order_relaxed) >> Memory::YUZU_PAGEBITS)
    , num_counter_entries{(device_as_size.load(std::memory_order_relaxed) >> page_bits) / subentries}
{
    ASSERT(device_virtual_bits_ >= min_device_virtual_bits &&
           device_virtual_bits_ <= max_device_virtual_bits);
    impl = std::make_unique<DeviceMemoryManagerAllocator<Traits>>(
        device_virtual_bits.load(std::memory_order_relaxed));
    cached_pages = std::make_unique<CounterEntry[]>(num_counter_entries);
}

template <typename Traits>
DeviceMemoryManager<Traits>::~DeviceMemoryManager() = default;

template <typename Traits>
void DeviceMemoryManager<Traits>::SetDeviceAddressBits(size_t device_virtual_bits_) {
    ASSERT(device_virtual_bits_ >= min_device_virtual_bits &&
           device_virtual_bits_ <= max_device_virtual_bits);
    const size_t clamped =
        std::clamp(device_virtual_bits_, min_device_virtual_bits, max_device_virtual_bits);
    // Lock counter_guard across the full device VA range before mapping_guard so counter readers
    // and cached_pages reallocation cannot race.
    Common::ScopedRangeLock counter_lk(counter_guard, 0, DAddr{1} << max_device_virtual_bits);
    std::scoped_lock lk(mapping_guard);
    const size_t current_bits = device_virtual_bits.load(std::memory_order_acquire);
    const size_t phys_bits =
        Settings::values.memory_layout_mode.GetValue() == Settings::MemoryLayout::Memory_4Gb
            ? physical_min_bits
            : physical_max_bits;
    const size_t expected_phys_pages = size_t{1} << (phys_bits - Memory::YUZU_PAGEBITS);
    if (clamped == current_bits && compressed_device_addr.size() == expected_phys_pages) {
        return;
    }
    const DAddr old_as_size = device_as_size.load(std::memory_order_acquire);
    const DAddr new_as_size = DAddr{1} << clamped;
    if (new_as_size < old_as_size) {
        device_virtual_bits.store(clamped, std::memory_order_release);
        device_as_size.store(new_as_size, std::memory_order_release);
    }
    num_counter_entries = (new_as_size >> page_bits) / subentries;
    impl->Reset(clamped);
    tracked_entries.ResizeAndClear(new_as_size >> Memory::YUZU_PAGEBITS);
    compressed_device_addr.ResizeAndClear(expected_phys_pages);
    cached_pages = std::make_unique<CounterEntry[]>(num_counter_entries);
    if (new_as_size >= old_as_size) {
        device_virtual_bits.store(clamped, std::memory_order_release);
        device_as_size.store(new_as_size, std::memory_order_release);
    }
    translation_generation.fetch_add(1, std::memory_order_release);
}

template <typename Traits>
bool DeviceMemoryManager<Traits>::IsInBounds(DAddr addr, u64 size) const noexcept {
    const DAddr area = device_as_size.load(std::memory_order_acquire);
    return addr <= area && size <= area - addr;
}

template <typename Traits>
u8 DeviceMemoryManager<Traits>::GetPageCachedCount(DAddr addr) const noexcept {
    Common::ScopedRangeLock lk(counter_guard, addr, 1);
    if (addr >= device_as_size.load(std::memory_order_acquire)) {
        return 0;
    }
    const size_t page = addr >> page_bits;
    const size_t entry_index = page >> subentries_shift;
    if (entry_index >= num_counter_entries) {
        return 0;
    }
    return cached_pages[entry_index].Count(page).load(std::memory_order_acquire);
}

template <typename Traits>
void DeviceMemoryManager<Traits>::HintCold(DAddr address, size_t size) {
    struct Run {
        u64 physical_offset;
        VAddr virtual_address; ///< 0: no CPU mapping known
        u64 size;
    };
    if (Common::ColdHintModeInEffect() == Settings::ColdHintMode::Off) {
        return;
    }
    if (size == 0 || !IsInBounds(address, size)) {
        return;
    }
    std::vector<Run> runs;
    const size_t page_end =
        (std::min)(Common::DivCeil(address + size, Memory::YUZU_PAGESIZE), tracked_entries.size());
    for (size_t page = address >> Memory::YUZU_PAGEBITS; page < page_end; ++page) {
        const u32 compressed = tracked_entries[page].compressed_physical_ptr;
        if (compressed == 0) {
            continue;
        }
        const u64 physical = u64{compressed - 1} << Memory::YUZU_PAGEBITS;
        const VAddr vaddr = ExtractCPUBacking(page).second;
        if (!runs.empty()) {
            Run& last = runs.back();
            const bool contiguous_view =
                last.virtual_address == 0 ? vaddr == 0 : last.virtual_address + last.size == vaddr;
            if (last.physical_offset + last.size == physical && contiguous_view) {
                last.size += Memory::YUZU_PAGESIZE;
                continue;
            }
        }
        runs.push_back({physical, vaddr, Memory::YUZU_PAGESIZE});
    }
    if (runs.empty()) {
        return;
    }
    if (!cold_hint_worker) {
        cold_hint_worker = std::make_unique<Common::ThreadWorker>(1, "DeviceMemory:ColdHint");
    }
    cold_hint_worker->QueueWork([this, runs = std::move(runs)] {
        // The worker's totals, logged at most every 10 s (each uploaded range is a call).
        static u64 marked{};
        static u64 paged{};
        static std::chrono::steady_clock::duration busy{};
        static auto logged = std::chrono::steady_clock::now();
        const auto begin = std::chrono::steady_clock::now();
        for (const Run& run : runs) {
            const auto action = device_memory_ptr->buffer.HintCold(
                run.physical_offset, run.size,
                run.virtual_address != 0 ? std::optional<size_t>{run.virtual_address}
                                         : std::nullopt);
            (action == Common::ColdHintAction::PagedOut ? paged : marked) +=
                action == Common::ColdHintAction::None ? 0 : run.size;
        }
        const auto now = std::chrono::steady_clock::now();
        busy += now - begin;
        if (now - logged >= std::chrono::seconds{10} && (marked != 0 || paged != 0)) {
            LOG_INFO(HW_Memory,
                     "Cold hints for uploaded memory: {} MiB marked cold, {} MiB paged out, "
                     "{:.1f} ms",
                     marked >> 20, paged >> 20,
                     std::chrono::duration<double, std::milli>(busy).count());
            marked = paged = 0;
            busy = {};
            logged = now;
        }
    });
}

template <typename Traits>
void DeviceMemoryManager<Traits>::BindInterface(DeviceInterface* device_inter_) {
    device_inter = device_inter_;
}

template <typename Traits>
DAddr DeviceMemoryManager<Traits>::Allocate(size_t size) {
    return impl->Allocate(size);
}

template <typename Traits>
void DeviceMemoryManager<Traits>::AllocateFixed(DAddr start, size_t size) {
    impl->AllocateFixed(start, size);
}

template <typename Traits>
bool DeviceMemoryManager<Traits>::TryAllocateFixed(DAddr start, size_t size) {
    return impl->TryAllocateFixed(start, size);
}

template <typename Traits>
void DeviceMemoryManager<Traits>::Free(DAddr start, size_t size) {
    impl->Free(start, size);
}

template <typename Traits>
void DeviceMemoryManager<Traits>::Map(DAddr address, VAddr virtual_address, size_t size, Asid asid,
                                      bool track) {
    if (size == 0 || !IsInBounds(address, size)) {
        LOG_ERROR(HW_Memory, "Out of bounds DeviceMemoryManager::Map @ {:#x}+{:#x}", address, size);
        return;
    }
    std::scoped_lock lk(mapping_guard);
    if (asid.id >= registered_processes.size() || registered_processes[asid.id] == nullptr) {
        return;
    }
    Core::Memory::Memory* process_memory = registered_processes[asid.id];
    const size_t start_page_d = address >> Memory::YUZU_PAGEBITS;
    const size_t end_page_d = Common::DivCeil(address + size, Memory::YUZU_PAGESIZE);
    const size_t num_pages = end_page_d - start_page_d;
    if (end_page_d > tracked_entries.size()) {
        LOG_ERROR(HW_Memory, "Out of bounds DeviceMemoryManager::Map pages @ {:#x}+{:#x}", address,
                  size);
        return;
    }

    tracked_entries.CommitRegion(start_page_d, end_page_d);
    CutContinuityAt(start_page_d);
    for (size_t i = 0; i < num_pages; i++) {
        const VAddr new_vaddress = virtual_address + i * Memory::YUZU_PAGESIZE;
        auto* ptr = process_memory->GetPointerSilent(Common::ProcessAddress(new_vaddress));
        // One page per run unless tracked below: a run left from an earlier mapping of these
        // device pages would make WalkBlock copy the host memory that followed that mapping.
        tracked_entries.GetUnchecked(start_page_d + i).continuity_tracker = 0;
        if (ptr == nullptr) {
            tracked_entries.GetUnchecked(start_page_d + i).compressed_physical_ptr = 0;
            tracked_entries.GetUnchecked(start_page_d + i).cpu_backing_address = 0;
            continue;
        }
        auto phys_addr = static_cast<u32>(GetRawPhysicalAddr(ptr) >> Memory::YUZU_PAGEBITS) + 1U;
        tracked_entries.GetUnchecked(start_page_d + i).compressed_physical_ptr = phys_addr;
        InsertCPUBacking(start_page_d + i, new_vaddress, asid);
        const u32 base_dev = compressed_device_addr[phys_addr - 1U];
        const u32 new_dev = static_cast<u32>(start_page_d + i);
        if (base_dev == 0) [[likely]] {
            compressed_device_addr.GetAndFault(phys_addr - 1U) = new_dev;
            continue;
        }
        u32 start_id = base_dev & MULTI_MASK;
        if ((base_dev >> MULTI_FLAG_BITS) == 0) {
            start_id = impl->multi_dev_address.Register(base_dev);
            compressed_device_addr.GetAndFault(phys_addr - 1U) = MULTI_FLAG | start_id;
        }
        impl->multi_dev_address.Register(new_dev, start_id);
    }
    translation_generation.fetch_add(1, std::memory_order_release);
    if (track) {
        TrackContinuityImpl(address, virtual_address, size, asid);
    }
}

template <typename Traits>
void DeviceMemoryManager<Traits>::Unmap(DAddr address, size_t size) {
    if (size == 0 || !IsInBounds(address, size)) {
        return;
    }
    if (device_inter != nullptr) {
        device_inter->InvalidateRegion(address, size);
    }
    std::scoped_lock lk(mapping_guard);
    if (!IsInBounds(address, size)) {
        return;
    }
    const size_t start_page_d = address >> Memory::YUZU_PAGEBITS;
    const size_t end_page_d = Common::DivCeil(address + size, Memory::YUZU_PAGESIZE);
    const size_t num_pages = end_page_d - start_page_d;
    if (end_page_d > tracked_entries.size()) {
        return;
    }

    tracked_entries.CommitRegion(start_page_d, end_page_d); // should already be committed, but just in case
    CutContinuityAt(start_page_d);
    for (size_t i = 0; i < num_pages; i++) {
        auto& entry = tracked_entries.GetUnchecked(start_page_d + i);
        auto phys_addr = entry.compressed_physical_ptr;
        entry.compressed_physical_ptr = 0;
        entry.cpu_backing_address = 0;
        entry.continuity_tracker = 0;
        if (phys_addr != 0) [[likely]] {
            u32& base_dev = compressed_device_addr.GetAndFault(phys_addr - 1U);
            if ((base_dev >> MULTI_FLAG_BITS) == 0) [[likely]] {
                base_dev = 0;
                continue;
            }
            const auto [more_entries, new_start] = impl->multi_dev_address.Unregister(
                static_cast<u32>(start_page_d + i), base_dev & MULTI_MASK);
            if (!more_entries) {
                base_dev = impl->multi_dev_address.ReleaseEntry(new_start);
                continue;
            }
            base_dev = new_start | MULTI_FLAG;
        }
    }
    translation_generation.fetch_add(1, std::memory_order_release);
}

template <typename Traits>
void DeviceMemoryManager<Traits>::CutContinuityAt(size_t page) {
    if (page > tracked_entries.size()) {
        return;
    }
    // Runs counted from the pages before `page` end there: the pages from `page` on are being
    // mapped again or unmapped. A run is counted down page by page (TrackContinuityImpl), so the
    // first page whose run already ends before `page` ends the walk.
    for (size_t index = page; index-- > 0;) {
        const size_t last_count = page - 1 - index;
        if (tracked_entries[index].continuity_tracker <= last_count) {
            break;
        }
        tracked_entries.GetUnchecked(index).continuity_tracker = static_cast<u32>(last_count);
    }
}

template <typename Traits>
void DeviceMemoryManager<Traits>::TrackContinuityImpl(DAddr address, VAddr virtual_address,
                                                      size_t size, Asid asid) {
    if (size == 0 || !IsInBounds(address, size) || asid.id >= registered_processes.size() ||
        registered_processes[asid.id] == nullptr) {
        return;
    }
    Core::Memory::Memory* process_memory = registered_processes[asid.id];
    const size_t start_page_d = address >> Memory::YUZU_PAGEBITS;
    const size_t end_page_d = Common::DivCeil(address + size, Memory::YUZU_PAGESIZE);
    const size_t num_pages = end_page_d - start_page_d;
    if (end_page_d > tracked_entries.size()) {
        return;
    }
    uintptr_t last_ptr = 0;
    size_t page_count = 1;

    tracked_entries.CommitRegion(start_page_d, end_page_d);
    for (size_t i = num_pages; i > 0; i--) {
        size_t index = i - 1;
        const VAddr new_vaddress = virtual_address + index * Memory::YUZU_PAGESIZE;
        const uintptr_t new_ptr = reinterpret_cast<uintptr_t>(
            process_memory->GetPointerSilent(Common::ProcessAddress(new_vaddress)));
        if (new_ptr != 0 && new_ptr + page_size == last_ptr) {
            page_count++;
        } else {
            page_count = 1;
        }
        last_ptr = new_ptr;
        tracked_entries.GetUnchecked(start_page_d + index).continuity_tracker = static_cast<u32>(page_count) - 1;
    }
}
template <typename Traits>
u8* DeviceMemoryManager<Traits>::GetSpan(const DAddr src_addr, const std::size_t size) {
    const DAddr area = device_as_size.load(std::memory_order_acquire);
    const size_t page_index = src_addr >> page_bits;
    if (src_addr >= area || !IsInBounds(src_addr, size) || page_index >= tracked_entries.size()) {
        return nullptr;
    }
    const size_t subbits = src_addr & page_mask;
    if ((static_cast<size_t>(tracked_entries[page_index].continuity_tracker + 1) << page_bits) >=
        size + subbits) {
        return GetPointer<u8>(src_addr);
    }
    return nullptr;
}

template <typename Traits>
const u8* DeviceMemoryManager<Traits>::GetSpan(const DAddr src_addr, const std::size_t size) const {
    const DAddr area = device_as_size.load(std::memory_order_acquire);
    const size_t page_index = src_addr >> page_bits;
    if (src_addr >= area || !IsInBounds(src_addr, size) || page_index >= tracked_entries.size()) {
        return nullptr;
    }
    const size_t subbits = src_addr & page_mask;
    if ((static_cast<size_t>(tracked_entries[page_index].continuity_tracker + 1) << page_bits) >=
        size + subbits) {
        return GetPointer<u8>(src_addr);
    }
    return nullptr;
}

template <typename Traits>
void DeviceMemoryManager<Traits>::InnerGatherDeviceAddresses(Common::ScratchBuffer<u32>& buffer,
                                                             PAddr address) {
    const size_t phys_addr = address >> page_bits;
    if (phys_addr >= compressed_device_addr.size()) {
        buffer.resize(0);
        return;
    }
    std::scoped_lock lk(mapping_guard);
    if (phys_addr >= compressed_device_addr.size()) {
        buffer.resize(0);
        return;
    }
    u32 backing = compressed_device_addr[phys_addr];
    if ((backing >> MULTI_FLAG_BITS) != 0) {
        impl->multi_dev_address.GatherValues(backing & MULTI_MASK, buffer);
        return;
    }
    buffer.resize(1);
    buffer[0] = backing;
}

template <typename Traits>
template <typename T>
T* DeviceMemoryManager<Traits>::GetPointer(DAddr address) {
    const size_t index = address >> Memory::YUZU_PAGEBITS;
    if (address >= device_as_size.load(std::memory_order_acquire) ||
        index >= tracked_entries.size()) {
        return nullptr;
    }
    const size_t offset = address & Memory::YUZU_PAGEMASK;
    auto phys_addr = tracked_entries[index].compressed_physical_ptr;
    if (phys_addr == 0)
        return nullptr;
    return GetPointerFromRaw<T>((PAddr(phys_addr - 1) << Memory::YUZU_PAGEBITS) + offset);
}

template <typename Traits>
template <typename T>
const T* DeviceMemoryManager<Traits>::GetPointer(DAddr address) const {
    const size_t index = address >> Memory::YUZU_PAGEBITS;
    if (address >= device_as_size.load(std::memory_order_acquire) ||
        index >= tracked_entries.size()) {
        return nullptr;
    }
    const size_t offset = address & Memory::YUZU_PAGEMASK;
    auto phys_addr = tracked_entries[index].compressed_physical_ptr;
    if (phys_addr == 0)
        return nullptr;
    return GetPointerFromRaw<T>((PAddr(phys_addr - 1) << Memory::YUZU_PAGEBITS) + offset);
}

template <typename Traits>
template <typename T>
void DeviceMemoryManager<Traits>::Write(DAddr address, T value) {
    if constexpr (sizeof(T) == 0) {
        return;
    }
    if (!IsInBounds(address, sizeof(T))) {
        return;
    }
    const size_t page_offset = address & page_mask;
    if (page_offset + sizeof(T) <= page_size) {
        if (T* ptr = GetPointer<T>(address); ptr != nullptr) {
            std::memcpy(ptr, &value, sizeof(T));
        }
        return;
    }
    const auto* src = reinterpret_cast<const u8*>(&value);
    size_t remaining = sizeof(T);
    DAddr cur_addr = address;
    while (remaining > 0) {
        const size_t offset = cur_addr & page_mask;
        const size_t chunk = (std::min)(page_size - offset, remaining);
        if (u8* ptr = GetPointer<u8>(cur_addr); ptr != nullptr) {
            std::memcpy(ptr, src, chunk);
        }
        src += chunk;
        cur_addr += chunk;
        remaining -= chunk;
    }
}

template <typename Traits>
template <typename T>
T DeviceMemoryManager<Traits>::Read(DAddr address) const {
    T result{};
    if constexpr (sizeof(T) == 0) {
        return result;
    }
    if (!IsInBounds(address, sizeof(T))) {
        return result;
    }
    const size_t page_offset = address & page_mask;
    if (page_offset + sizeof(T) <= page_size) {
        if (const T* ptr = GetPointer<T>(address); ptr != nullptr) {
            std::memcpy(&result, ptr, sizeof(T));
        }
        return result;
    }
    auto* dst = reinterpret_cast<u8*>(&result);
    size_t remaining = sizeof(T);
    DAddr cur_addr = address;
    while (remaining > 0) {
        const size_t offset = cur_addr & page_mask;
        const size_t chunk = (std::min)(page_size - offset, remaining);
        if (const u8* ptr = GetPointer<u8>(cur_addr); ptr != nullptr) {
            std::memcpy(dst, ptr, chunk);
        }
        dst += chunk;
        cur_addr += chunk;
        remaining -= chunk;
    }
    return result;
}

template <typename Traits>
void DeviceMemoryManager<Traits>::WalkBlock(DAddr addr, std::size_t size, auto on_unmapped, auto on_memory, auto increment) {
    if (size == 0) {
        return;
    }
    if (!IsInBounds(addr, size)) {
        on_unmapped(size, addr);
        increment(size);
        return;
    }
    std::size_t remaining_size = size;
    std::size_t page_index = addr >> Memory::YUZU_PAGEBITS;
    std::size_t page_offset = addr & Memory::YUZU_PAGEMASK;
    while (remaining_size) {
        if (page_index >= tracked_entries.size()) {
            on_unmapped(remaining_size, u64((page_index << Memory::YUZU_PAGEBITS) + page_offset));
            increment(remaining_size);
            break;
        }
        const size_t next_pages = std::size_t(tracked_entries[page_index].continuity_tracker + 1);
        const std::size_t copy_amount = (std::min)((next_pages << Memory::YUZU_PAGEBITS) - page_offset, remaining_size);
        const auto current_vaddr = u64((page_index << Memory::YUZU_PAGEBITS) + page_offset);
        SCOPE_EXIT{
            page_index += next_pages;
            page_offset = 0;
            increment(copy_amount);
            remaining_size -= copy_amount;
        };

        auto phys_addr = tracked_entries[page_index].compressed_physical_ptr;
        if (phys_addr == 0) {
            on_unmapped(copy_amount, current_vaddr);
            continue;
        }
        auto* mem_ptr = GetPointerFromRaw<u8>((PAddr(phys_addr - 1) << Memory::YUZU_PAGEBITS) + page_offset);
        on_memory(copy_amount, mem_ptr);
    }
}

template <typename Traits>
void DeviceMemoryManager<Traits>::ReadBlock(DAddr address, void* dest_pointer, size_t size) {
    if (size == 0) {
        return;
    }
    if (device_inter != nullptr) {
        device_inter->FlushRegion(address, size);
    }
    const std::size_t page_offset = address & Memory::YUZU_PAGEMASK;
    if (IsInBounds(address, size) && size <= Memory::YUZU_PAGESIZE - page_offset) {
        const DAddr guest_page = address & ~static_cast<DAddr>(Memory::YUZU_PAGEMASK);
        TranslationCache& cache = Translations();
        for (const TranslationEntry& entry : cache.slots) {
            if (entry.guest_page == guest_page && entry.host_ptr != nullptr) {
                std::memcpy(dest_pointer, entry.host_ptr + page_offset, size);
                return;
            }
        }

        const std::size_t page_index = address >> Memory::YUZU_PAGEBITS;
        if (page_index < tracked_entries.size()) {
            const auto phys_addr = tracked_entries[page_index].compressed_physical_ptr;
            if (phys_addr != 0) {
                auto* const mem_ptr = GetPointerFromRaw<u8>((PAddr(phys_addr - 1) << Memory::YUZU_PAGEBITS));
                cache.slots[cache.cursor] = TranslationEntry{.guest_page = guest_page, .host_ptr = mem_ptr};
                cache.cursor = (cache.cursor + 1) & 3U;
                std::memcpy(dest_pointer, mem_ptr + page_offset, size);
                return;
            }
        }
    }
    WalkBlock(
        address, size,
        [&](size_t copy_amount, DAddr current_vaddr) {
            static std::atomic<u32> reported{};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 16) {
                LOG_ERROR(
                    HW_Memory,
                    "Unmapped Device ReadBlock @ {:#016x} (start address = {:#016x}, size = {})",
                    current_vaddr, address, size);
            }
            std::memset(dest_pointer, 0, copy_amount);
        },
        [&](size_t copy_amount, const u8* const src_ptr) {
            std::memcpy(dest_pointer, src_ptr, copy_amount);
        },
        [&](const std::size_t copy_amount) {
            dest_pointer = static_cast<u8*>(dest_pointer) + copy_amount;
        });
}

template <typename Traits>
void DeviceMemoryManager<Traits>::WriteBlock(DAddr address, const void* src_pointer, size_t size) {
    if (size == 0) {
        return;
    }
    WalkBlock(
        address, size,
        [&](size_t copy_amount, DAddr current_vaddr) {
            static std::atomic<u32> reported{};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 16) {
                LOG_ERROR(
                    HW_Memory,
                    "Unmapped Device WriteBlock @ {:#016x} (start address = {:#016x}, size = {})",
                    current_vaddr, address, size);
            }
        },
        [&](size_t copy_amount, u8* const dst_ptr) {
            std::memcpy(dst_ptr, src_pointer, copy_amount);
        },
        [&](const std::size_t copy_amount) {
            src_pointer = static_cast<const u8*>(src_pointer) + copy_amount;
        });
    if (device_inter != nullptr) {
        device_inter->InvalidateRegion(address, size);
    }
}

template <typename Traits>
void DeviceMemoryManager<Traits>::ReadBlockUnsafe(DAddr address, void* dest_pointer, size_t size) {
    if (size == 0) {
        return;
    }
    const std::size_t page_offset = address & Memory::YUZU_PAGEMASK;
    if (IsInBounds(address, size) && size <= Memory::YUZU_PAGESIZE - page_offset) {
        const DAddr guest_page = address & ~static_cast<DAddr>(Memory::YUZU_PAGEMASK);
        TranslationCache& cache = Translations();
        for (const TranslationEntry& entry : cache.slots) {
            if (entry.guest_page == guest_page && entry.host_ptr != nullptr) {
                std::memcpy(dest_pointer, entry.host_ptr + page_offset, size);
                return;
            }
        }

        const std::size_t page_index = address >> Memory::YUZU_PAGEBITS;
        if (page_index < tracked_entries.size()) {
            const auto phys_addr = tracked_entries[page_index].compressed_physical_ptr;
            if (phys_addr != 0) {
                auto* const mem_ptr = GetPointerFromRaw<u8>((PAddr(phys_addr - 1) << Memory::YUZU_PAGEBITS));
                cache.slots[cache.cursor] = TranslationEntry{.guest_page = guest_page, .host_ptr = mem_ptr};
                cache.cursor = (cache.cursor + 1) & 3U;
                std::memcpy(dest_pointer, mem_ptr + page_offset, size);
                return;
            }
        }
    }
    WalkBlock(
        address, size,
        [&](size_t copy_amount, DAddr current_vaddr) {
            static std::atomic<u32> reported{};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 16) {
                LOG_ERROR(
                    HW_Memory,
                    "Unmapped Device ReadBlock @ {:#016x} (start address = {:#016x}, size = {})",
                    current_vaddr, address, size);
            }
            std::memset(dest_pointer, 0, copy_amount);
        },
        [&](size_t copy_amount, const u8* const src_ptr) {
            std::memcpy(dest_pointer, src_ptr, copy_amount);
        },
        [&](const std::size_t copy_amount) {
            dest_pointer = static_cast<u8*>(dest_pointer) + copy_amount;
        });
}

template <typename Traits>
void DeviceMemoryManager<Traits>::WriteBlockUnsafe(DAddr address, const void* src_pointer,
                                                   size_t size) {
    if (size == 0) {
        return;
    }
    WalkBlock(
        address, size,
        [&](size_t copy_amount, DAddr current_vaddr) {
            static std::atomic<u32> reported{};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 16) {
                LOG_ERROR(
                    HW_Memory,
                    "Unmapped Device WriteBlock @ {:#016x} (start address = {:#016x}, size = {})",
                    current_vaddr, address, size);
            }
        },
        [&](size_t copy_amount, u8* const dst_ptr) {
            std::memcpy(dst_ptr, src_pointer, copy_amount);
        },
        [&](const std::size_t copy_amount) {
            src_pointer = static_cast<const u8*>(src_pointer) + copy_amount;
        });
}

template <typename Traits>
Asid DeviceMemoryManager<Traits>::RegisterProcess(Memory::Memory* memory_device_inter) {
    std::scoped_lock lk(mapping_guard);
    size_t new_id{};
    if (!id_pool.empty()) {
        new_id = id_pool.front();
        id_pool.pop_front();
        registered_processes[new_id] = memory_device_inter;
    } else {
        registered_processes.emplace_back(memory_device_inter);
        new_id = registered_processes.size() - 1U;
    }
    UpdateHasFastmemArenaLocked();
    return Asid{new_id};
}

template <typename Traits>
void DeviceMemoryManager<Traits>::UnregisterProcess(Asid asid) {
    std::scoped_lock lk(mapping_guard);
    if (asid.id >= registered_processes.size()) {
        return;
    }
    registered_processes[asid.id] = nullptr;
    id_pool.push_front(asid.id);
    UpdateHasFastmemArenaLocked();
}

template <typename Traits>
void DeviceMemoryManager<Traits>::UpdateHasFastmemArenaLocked() {
    const bool any = std::any_of(
        registered_processes.begin(), registered_processes.end(),
        [](const Memory::Memory* inter) { return DeviceMethods::HasFastmemArena(inter); });
    has_fastmem_arena.store(any, std::memory_order_release);
}

template <typename Traits>
bool DeviceMemoryManager<Traits>::CanTrapCpuReads() const {
    static const bool disabled = [] {
        const char* value = std::getenv("EDEN_NX2_READ_TRAPS");
        return value != nullptr && value[0] == '0';
    }();
    return !disabled && has_fastmem_arena.load(std::memory_order_acquire) &&
           !Settings::values.use_reactive_flushing.GetValue() &&
           Settings::values.cpu_backend.GetValue() != Settings::CpuBackend::Nce;
}

template <typename Traits>
void DeviceMemoryManager<Traits>::UpdatePagesCachedCountNoLock(DAddr addr, size_t size, s32 delta) {
    if (size == 0 || !IsInBounds(addr, size)) {
        return;
    }
    u64 uncache_begin = 0;
    u64 cache_begin = 0;
    u64 uncache_bytes = 0;
    u64 cache_bytes = 0;
    const auto MarkRegionCaching = &DeviceMemoryManager<Traits>::DeviceMethods::MarkRegionCaching;

    std::atomic_thread_fence(std::memory_order_acquire);
    // Sem processo registrado nao ha o que marcar, e asid.id 0 indexaria um vetor vazio. Acontece
    // no alvo de testes, que chama UpdatePagesCachedBatch num DeviceMemoryManager recem-criado.
    if (registered_processes.empty()) {
        return;
    }
    const size_t page_end =
        (std::min)(Common::DivCeil(addr + size, Memory::YUZU_PAGESIZE), tracked_entries.size());
    size_t page = addr >> Memory::YUZU_PAGEBITS;
    if (page >= page_end) {
        return;
    }
    auto [asid, base_vaddress] = ExtractCPUBacking(page);
    auto* memory_device_inter =
        asid.id < registered_processes.size() ? registered_processes[asid.id] : nullptr;
    const auto release_pending = [&] {
        if (uncache_bytes > 0) {
            if (memory_device_inter != nullptr) {
                MarkRegionCaching(memory_device_inter, uncache_begin << Memory::YUZU_PAGEBITS,
                                  uncache_bytes, false);
            }
            uncache_bytes = 0;
        }
        if (cache_bytes > 0) {
            if (memory_device_inter != nullptr) {
                MarkRegionCaching(memory_device_inter, cache_begin << Memory::YUZU_PAGEBITS,
                                  cache_bytes, true);
            }
            cache_bytes = 0;
        }
    };
    size_t old_vpage = (base_vaddress >> Memory::YUZU_PAGEBITS) - 1;
    for (; page != page_end; ++page) {
        const size_t entry_index = page >> subentries_shift;
        if (entry_index >= num_counter_entries) {
            break;
        }
        CounterAtomicType& count = cached_pages[entry_index].Count(page);
        auto [asid_2, vpage] = ExtractCPUBacking(page);
        vpage >>= Memory::YUZU_PAGEBITS;

        if (vpage == 0) {
            release_pending();
            continue;
        }

        if (asid.id != asid_2.id) {
            release_pending();
            asid = asid_2;
            memory_device_inter =
                asid_2.id < registered_processes.size() ? registered_processes[asid_2.id] : nullptr;
        }

        if (vpage != old_vpage + 1) {
            release_pending();
        }

        old_vpage = vpage;

        // Adds or subtracts 1, as count is a unsigned 8-bit value
        count.fetch_add(static_cast<CounterType>(delta), std::memory_order_release);

        // Assume delta is either -1 or 1
        if (count.load(std::memory_order::relaxed) == 0) {
            if (uncache_bytes == 0) {
                uncache_begin = vpage;
            }
            uncache_bytes += Memory::YUZU_PAGESIZE;
        } else if (uncache_bytes > 0) {
            if (memory_device_inter != nullptr) {
                MarkRegionCaching(memory_device_inter, uncache_begin << Memory::YUZU_PAGEBITS,
                                  uncache_bytes, false);
            }
            uncache_bytes = 0;
        }
        if (count.load(std::memory_order::relaxed) == 1 && delta > 0) {
            if (cache_bytes == 0) {
                cache_begin = vpage;
            }
            cache_bytes += Memory::YUZU_PAGESIZE;
        } else if (cache_bytes > 0) {
            if (memory_device_inter != nullptr) {
                MarkRegionCaching(memory_device_inter, cache_begin << Memory::YUZU_PAGEBITS,
                                  cache_bytes, true);
            }
            cache_bytes = 0;
        }
    }
    release_pending();
}

template <typename Traits>
void DeviceMemoryManager<Traits>::UpdatePagesCachedCount(DAddr addr, size_t size, s32 delta) {
    if (size == 0 || !IsInBounds(addr, size)) {
        return;
    }
    Common::ScopedRangeLock lk(counter_guard, addr, size);
    UpdatePagesCachedCountNoLock(addr, size, delta);
}

template <typename Traits>
void DeviceMemoryManager<Traits>::UpdatePagesCachedBatch(std::span<const std::pair<DAddr, size_t>> ranges, s32 delta) {
    if (ranges.empty()) {
        return;
    }
    // Runs already sorted with gaps between them (the buffer tracker's) are what the sort and
    // merge below would produce: use them as they are instead of two heap copies.
    const bool separated =
        VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::SortedPageBatches) &&
        std::ranges::all_of(ranges, [this](const auto& r) {
            return r.second > 0 && IsInBounds(r.first, r.second);
        }) &&
        std::ranges::adjacent_find(ranges, [](const auto& a, const auto& b) {
            return a.first > b.first || a.first + a.second >= b.first;
        }) == ranges.end();
    if (separated && !VideoCommon::Nx2Perf::Verify()) {
        const DAddr lock_begin = ranges.front().first;
        const DAddr lock_end = ranges.back().first + ranges.back().second;
        Common::ScopedRangeLock lk(counter_guard, lock_begin,
                                   static_cast<size_t>(lock_end - lock_begin));
        for (const auto& [addr, size] : ranges) {
            UpdatePagesCachedCountNoLock(addr, size, delta);
        }
        return;
    }
    // Make a local copy and sort by address
    std::vector<std::pair<DAddr, size_t>> tmp;
    tmp.reserve(ranges.size());
    for (const auto& range : ranges) {
        if (range.second > 0 && IsInBounds(range.first, range.second)) {
            tmp.push_back(range);
        }
    }
    if (tmp.empty()) {
        return;
    }
    std::sort(tmp.begin(), tmp.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    // Coalesce adjacent/overlapping ranges
    std::vector<std::pair<DAddr, size_t>> coalesced;
    DAddr cur_addr = tmp[0].first;
    size_t cur_size = tmp[0].second;
    for (size_t i = 1; i < tmp.size(); ++i) {
        DAddr next_addr = tmp[i].first;
        size_t next_size = tmp[i].second;
        if (cur_addr + cur_size >= next_addr) {
            // overlapping or contiguous
            const DAddr end = std::max(cur_addr + cur_size, next_addr + next_size);
            cur_size = end - cur_addr;
        } else {
            coalesced.emplace_back(cur_addr, cur_size);
            cur_addr = next_addr;
            cur_size = next_size;
        }
    }
    coalesced.emplace_back(cur_addr, cur_size);
    if (separated) {
        static std::atomic<u32> logged{};
        if (!std::ranges::equal(ranges, coalesced) &&
            logged.fetch_add(1, std::memory_order_relaxed) < 64) {
            LOG_ERROR(HW_GPU, "Nx2Perf texture-writes: cached page runs from {:#x} merged differently",
                      coalesced.front().first);
        }
    }

    const DAddr lock_begin = coalesced.front().first;
    const DAddr lock_end = coalesced.back().first + coalesced.back().second;
    Common::ScopedRangeLock lk(counter_guard, lock_begin, static_cast<size_t>(lock_end - lock_begin));

    for (const auto& [addr, size] : coalesced) {
        UpdatePagesCachedCountNoLock(addr, size, delta);
    }
}

template <typename Traits>
void DeviceMemoryManager<Traits>::ArmReadTraps(DAddr addr, size_t size) {
    if (!CanTrapCpuReads() || size == 0 || !IsInBounds(addr, size)) {
        return;
    }
    const auto MarkRegionReadTrap = &DeviceMemoryManager<Traits>::DeviceMethods::MarkRegionReadTrap;
    Memory::Memory* run_inter = nullptr;
    u64 run_begin = 0;
    u64 run_pages = 0;
    const auto release_run = [&] {
        if (run_pages > 0 && run_inter != nullptr) {
            MarkRegionReadTrap(run_inter, run_begin << Memory::YUZU_PAGEBITS,
                               run_pages << Memory::YUZU_PAGEBITS);
        }
        run_pages = 0;
    };
    const size_t page_end =
        (std::min)(Common::DivCeil(addr + size, Memory::YUZU_PAGESIZE), tracked_entries.size());
    for (size_t page = addr >> Memory::YUZU_PAGEBITS; page < page_end; ++page) {
        auto [asid, vaddr] = ExtractCPUBacking(page);
        const u64 vpage = vaddr >> Memory::YUZU_PAGEBITS;
        if (vpage == 0 || asid.id >= registered_processes.size()) {
            release_run();
            continue;
        }
        auto* const inter = registered_processes[asid.id];
        if (inter == nullptr) {
            release_run();
            continue;
        }
        if (inter != run_inter || vpage != run_begin + run_pages) {
            release_run();
            run_inter = inter;
            run_begin = vpage;
        }
        ++run_pages;
    }
    release_run();
}

} // namespace Core
