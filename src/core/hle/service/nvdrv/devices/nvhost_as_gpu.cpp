// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-FileCopyrightText: 2021 Skyline Team and Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/nx2_session_state.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/core/heap_mapper.h"
#include "core/hle/service/nvdrv/core/nvmap.h"
#include "core/hle/service/nvdrv/devices/ioctl_serialization.h"
#include "core/hle/service/nvdrv/devices/nvhost_as_gpu.h"
#include "core/hle/service/nvdrv/devices/nvhost_gpu.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "video_core/control/channel_state.h"
#include "video_core/gpu.h"
#include "video_core/host1x/host1x.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"

namespace Service::Nvidia::Devices {

nvhost_as_gpu::nvhost_as_gpu(Core::System& system_, Module& module_, NvCore::Container& core)
    : nvdevice{system_, Kind}, module{module_}, container{core}, nvmap{core.GetNvMapFile()}, vm{},
      gmmu{} {}

nvhost_as_gpu::~nvhost_as_gpu() {
    // Unregister before draining so no new listener callbacks arrive on `this` mid-teardown.
    const auto session = nx2_session_state.lock();
    if (session) {
        session->GpuMemoryRegistry().RemoveListener(this);
    }
    // The callbacks queued for the GPU thread below use this device's address space and the
    // session's heap mapper: let them run before those are gone. The drain is only skipped once
    // the core shutdown began: the GPU thread is stopped then, before the services are closed,
    // so the queued callbacks never run at all. The frontend asks for a shutdown much earlier,
    // with the GPU thread still running: there the drain has to happen.
    if (!system.IsShuttingDown() || system.IsPoweredOn()) {
        DrainGpuCallbacks();
    }
    // Leave shared ranges to the process's other address spaces; like this address space's own
    // ranges, one nobody else maps stays allocated.
    if (session) {
        for (const auto& [key, range] : nx2_device_ranges) {
            if (range.shared) {
                session->GpuMemoryRegistry().ReleaseDeviceRange(range.asid, key.first, key.second);
            }
        }
    }
}

void nvhost_as_gpu::DrainGpuCallbacks() {
    // Tegra::GPU::RunInOrder always executes on the GPU thread (even in non-async GPU mode), and
    // Tegra::GPU does not expose whether that thread is stopped or unstarted. Check the stop token
    // and shutdown state before queueing, then bound the wait to 5 s with stop-token early wake.
    const auto stop_token = system.GetEmulationStopToken();
    const auto is_shutting_down = [&] {
        return system.IsShuttingDown() && !system.IsPoweredOn();
    };
    if (stop_token.stop_requested() || is_shutting_down()) {
        return;
    }

    struct DrainState {
        std::mutex mutex;
        std::condition_variable cv;
        bool done{};
        bool stopped{};
    };
    auto state = std::make_shared<DrainState>();
    // The GPU thread runs the queued callbacks in order, so this one is the last.
    system.GPU().RunInOrder([state] {
        {
            std::scoped_lock lk{state->mutex};
            state->done = true;
        }
        state->cv.notify_all();
    });
    std::stop_callback wake{stop_token, [state] {
                                {
                                    std::scoped_lock lk{state->mutex};
                                    state->stopped = true;
                                }
                                state->cv.notify_all();
                            }};
    // Wait for it, but not forever: a stuck GPU thread must not hang the emulator here.
    std::unique_lock lk{state->mutex};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    const auto aborted = [&] { return state->stopped || is_shutting_down(); };
    if (!state->cv.wait_until(lk, deadline, [&] { return state->done || aborted(); }) &&
        !aborted()) {
        LOG_ERROR(Service_NVDRV,
                  "Switch 2 GPU callbacks did not finish in time, giving up on them");
    }
}

NvResult nvhost_as_gpu::Ioctl1(DeviceFD fd, Ioctl command, std::span<const u8> input,
                               std::span<u8> output) {
    switch (command.group) {
    case 'A':
        switch (command.cmd) {
        case 0x1:
            return WrapFixed(this, &nvhost_as_gpu::BindChannel, input, output);
        case 0x2:
            return WrapFixed(this, &nvhost_as_gpu::AllocateSpace, input, output);
        case 0x3:
            return WrapFixed(this, &nvhost_as_gpu::FreeSpace, input, output);
        case 0x5:
            return WrapFixed(this, &nvhost_as_gpu::UnmapBuffer, input, output);
        case 0x6:
            return WrapFixed(this, &nvhost_as_gpu::MapBufferEx, input, output);
        case 0x8:
            return WrapFixed(this, &nvhost_as_gpu::GetVARegions1, input, output);
        case 0x9: {
            const auto result = WrapFixed(this, &nvhost_as_gpu::AllocAsEx, input, output);
            if (result == NvResult::Success) {
                if (const auto session = system.Nx2Session()) {
                    nx2_session_state = session;
                    session->GpuMemoryRegistry().AddListener(
                        this,
                        [this](u64 gpu_address, u64 cpu_address, u64 size) {
                            Nx2Map(gpu_address, cpu_address, size);
                        },
                        [this](u64 gpu_address, u64 size) { Nx2Unmap(gpu_address, size); },
                        [this](u64 cpu_address, u64 size) { Nx2Release(cpu_address, size); },
                        [this](u64 gpu_address, u64 size, bool create) {
                            Nx2Sparse(gpu_address, size, create);
                        },
                        [this](u64 gpu_address, u64 cpu_address, u64 size) {
                            Nx2BackSparse(gpu_address, cpu_address, size);
                        },
                        [this](u64 gpu_address, u64 cpu_address, u64 size) {
                            Nx2SparseTile(gpu_address, cpu_address, size);
                        });
                }
            }
            return result;
        }
        case 0xb:
            if (const auto session = system.Nx2Session(); session && session->IsDriverActive()) {
                return WrapFixed(this, &nvhost_as_gpu::GetSyncRoMap, input, output);
            }
            break;
        case 0x14:
            return WrapVariable(this, &nvhost_as_gpu::Remap, input, output);
        default:
            break;
        }
        break;
    default:
        break;
    }

    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

NvResult nvhost_as_gpu::Ioctl2(DeviceFD fd, Ioctl command, std::span<const u8> input,
                               std::span<const u8> inline_input, std::span<u8> output) {
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

NvResult nvhost_as_gpu::Ioctl3(DeviceFD fd, Ioctl command, std::span<const u8> input,
                               std::span<u8> output, std::span<u8> inline_output) {
    switch (command.group) {
    case 'A':
        switch (command.cmd) {
        case 0x8:
            return WrapFixedInlOut(this, &nvhost_as_gpu::GetVARegions3, input, output,
                                   inline_output);
        default:
            break;
        }
        break;
    default:
        break;
    }
    UNIMPLEMENTED_MSG("Unimplemented ioctl={:08X}", command.raw);
    return NvResult::NotImplemented;
}

void nvhost_as_gpu::OnOpen(NvCore::SessionId session_id, DeviceFD fd) {
    nx2_session = session_id;
}

void nvhost_as_gpu::Nx2Map(u64 gpu_address, u64 cpu_address, u64 size) {
    std::scoped_lock lock(mutex);
    if (!gmmu) {
        LOG_ERROR(Service_NVDRV, "Switch 2 GPU mapping {:#x}+{:#x}: no address space or session",
                  gpu_address, size);
        return;
    }
    // The same CPU range is often mapped twice (at its own address and in a sparse region), so
    // reuse its SMMU mapping.
    Nx2DeviceRange range{};
    bool resolved = false;
    std::optional<Core::Asid> session_asid;
    if (const auto it = nx2_device_ranges.find({cpu_address, size}); it != nx2_device_ranges.end()) {
        if (!container.WithSession(nx2_session, [](const NvCore::Session&) {})) {
            LOG_ERROR(Service_NVDRV,
                      "Switch 2 GPU mapping {:#x}+{:#x}: no address space or session", gpu_address,
                      size);
            return;
        }
        range = it->second;
        resolved = true;
    } else if (!container.WithSession(nx2_session, [&](NvCore::Session& session) {
                   session_asid = session.asid;
                   if (session.has_preallocated_area &&
                       session.mapper->IsInBounds(cpu_address, size)) {
                       range = {session.mapper->Map(cpu_address, size), true};
                       // The mapper that made the mapping: it goes away with the session, so the
                       // release of the range asks the container for it by id.
                       range.heap_mapper = NvCore::Container::IdOf(session.mapper.get());
                       resolved = true;
                   }
               })) {
        LOG_ERROR(Service_NVDRV, "Switch 2 GPU mapping {:#x}+{:#x}: no address space or session",
                  gpu_address, size);
        return;
    }
    if (!resolved) {
        const auto allocate = [&]() -> u64 {
            auto& smmu = system.Host1x().MemoryManager();
            const DAddr address = smmu.Allocate(Common::AlignUp(size, 0x20000));
            if (address != 0) {
                smmu.Map(address, cpu_address, size, *session_asid, true);
            }
            return address;
        };
        auto session = nx2_session_state.lock();
        if (!session) {
            session = system.Nx2Session();
        }
        if (!session) {
            return;
        }
        // EDEN_NX2_PRIVATE_DEVICE_RANGES=1 (A/B aid): a range per address space.
        static const bool private_ranges = std::getenv("EDEN_NX2_PRIVATE_DEVICE_RANGES") != nullptr;
        if (private_ranges) {
            range = {allocate(), false};
        } else {
            range = {session->GpuMemoryRegistry().AcquireDeviceRange(session_asid->id, cpu_address,
                                                                    size, allocate),
                     false, true, session_asid->id};
        }
        if (range.address == 0) {
            LOG_CRITICAL(Service_NVDRV, "Switch 2 GPU memory {:#x}+{:#x}: out of SMMU space",
                         cpu_address, size);
            return;
        }
    }
    const DAddr device_address = range.address;
    nx2_device_ranges[{cpu_address, size}] = range;
    if (auto session = nx2_session_state.lock()) {
        session->GpuMemoryRegistry().RecordDeviceRange(cpu_address, size, device_address);
    } else if (const auto active_session = system.Nx2Session()) {
        active_session->GpuMemoryRegistry().RecordDeviceRange(cpu_address, size, device_address);
    }
    // On the GPU thread, like unmaps: a GMMU map takes the texture cache lock, and the callers hold
    // nx2:gpu locks that the GPU thread takes under that lock (sparse blocks backed on write).
    // The callback holds a copy of the address space, so it stays alive until it ran.
    auto gmmu_owner = gmmu;
    system.GPU().MapInOrder(
        gpu_address, size, [gmmu = std::move(gmmu_owner), gpu_address, device_address, size] {
            gmmu->Map(gpu_address, device_address, size, Tegra::PTEKind::PITCH, false);
        });
    LOG_DEBUG(Service_NVDRV, "Switch 2 GPU mapping {:#x}+{:#x} -> CPU {:#x}", gpu_address, size,
              cpu_address);
}

void nvhost_as_gpu::Nx2Unmap(u64 gpu_address, u64 size) {
    std::scoped_lock lock(mutex);
    if (!gmmu) {
        return;
    }
    // The GPU thread may still be running command lists that use the range.
    auto gmmu_owner = gmmu;
    system.GPU().RunInOrder(
        [gmmu = std::move(gmmu_owner), gpu_address, size] { gmmu->Unmap(gpu_address, size); });
    LOG_DEBUG(Service_NVDRV, "Switch 2 GPU unmapping {:#x}+{:#x}", gpu_address, size);
}

void nvhost_as_gpu::Nx2Release(u64 cpu_address, u64 size) {
    if (size == 0) {
        return;
    }
    std::scoped_lock lock(mutex);
    struct PendingRelease {
        u64 cpu_address;
        u64 size;
        Nx2DeviceRange range;
    };
    std::vector<PendingRelease> released;
    auto it = nx2_device_ranges.lower_bound({cpu_address, 0});
    while (it != nx2_device_ranges.end() && it->first.first - cpu_address < size) {
        const auto [entry_cpu, entry_size] = it->first;
        const u64 offset = entry_cpu - cpu_address;
        if (entry_size > size - offset) {
            ++it;
            continue;
        }
        const Nx2DeviceRange range = it->second;
        it = nx2_device_ranges.erase(it);
        const auto session = nx2_session_state.lock();
        if (range.shared && session &&
            !session->GpuMemoryRegistry().ReleaseDeviceRange(range.asid, entry_cpu, entry_size)) {
            continue; // another address space of the process still maps it
        }
        released.push_back({entry_cpu, entry_size, range});
    }
    if (released.empty()) {
        return;
    }
    const size_t released_count = released.size();
    // The SMMU belongs to the host1x memory manager, which outlives the device: the callback
    // holds a pointer to it, not a reference to a local.
    auto* const smmu = &system.Host1x().MemoryManager();
    // The container outlives the device too: it holds the session's heap mapper.
    auto* const container_ptr = &container;
    const NvCore::SessionId session_id = nx2_session;
    // Queued after the GPU unmaps of the range, so the GPU thread is done with it.
    system.GPU().RunInOrder([smmu, container_ptr, session_id, released = std::move(released)] {
        for (const auto& entry : released) {
            if (entry.range.heap) {
                // The session may have closed while this waited, and its mapper (which owns the
                // heap mapping) went with it: it unmaps what is left of it when it dies, so
                // there is nothing left to do here. The container checks that the mapper is still
                // the session's one before it is used, so a mapper that went away is never used.
                container_ptr->WithSessionMapper(
                    session_id, entry.range.heap_mapper,
                    [cpu_address = entry.cpu_address, size = entry.size](NvCore::HeapMapper& mapper) {
                        mapper.Unmap(cpu_address, size);
                    });
                continue;
            }
            smmu->Unmap(entry.range.address, entry.size);
            smmu->Free(entry.range.address, Common::AlignUp(entry.size, 0x20000));
        }
    });
    LOG_DEBUG(Service_NVDRV, "Switch 2 released backing {:#x}+{:#x} ({} ranges)", cpu_address, size,
              released_count);
}

void nvhost_as_gpu::Nx2Sparse(u64 gpu_address, u64 size, bool create) {
    std::scoped_lock lock(mutex);
    if (!gmmu) {
        return;
    }
    auto* const smmu = &system.Host1x().MemoryManager();
    const u64 device_size = Common::AlignUp(size, 0x20000);
    if (!create) {
        const auto it = nx2_sparse_ranges.find(gpu_address);
        if (it == nx2_sparse_ranges.end()) {
            return;
        }
        const DAddr device_address = it->second.second;
        nx2_sparse_ranges.erase(it);
        // After the command lists that may still use it.
        auto gmmu_owner = gmmu;
        system.GPU().RunInOrder([gmmu = std::move(gmmu_owner), smmu, gpu_address, size,
                                 device_address, device_size] {
            gmmu->Unmap(gpu_address, size);
            smmu->Unmap(device_address, size);
            smmu->Free(device_address, device_size);
        });
        return;
    }
    const DAddr device_address = smmu->Allocate(device_size);
    if (device_address == 0) {
        LOG_CRITICAL(Service_NVDRV, "Switch 2 sparse region {:#x}+{:#x}: out of SMMU space",
                     gpu_address, size);
        return;
    }
    nx2_sparse_ranges[gpu_address] = {size, device_address};
    // On the GPU thread, as in Nx2Map.
    auto gmmu_owner = gmmu;
    system.GPU().MapInOrder(
        gpu_address, size, [gmmu = std::move(gmmu_owner), gpu_address, device_address, size] {
            gmmu->Map(gpu_address, device_address, size, Tegra::PTEKind::PITCH, false);
        });
    LOG_DEBUG(Service_NVDRV, "Switch 2 sparse region {:#x}+{:#x} -> device {:#x}", gpu_address,
              size, device_address);
}

void nvhost_as_gpu::Nx2BackSparse(u64 gpu_address, u64 cpu_address, u64 size) {
    std::scoped_lock lock(mutex);
    auto it = nx2_sparse_ranges.upper_bound(gpu_address);
    if (it == nx2_sparse_ranges.begin()) {
        return;
    }
    --it;
    const auto& [start, range] = *it;
    const auto& [region_size, device_address] = range;
    if (size > region_size || gpu_address - start > region_size - size) {
        return;
    }
    std::optional<Core::Asid> asid;
    container.WithSession(nx2_session, [&](const NvCore::Session& session) { asid = session.asid; });
    if (!asid) {
        return;
    }
    system.Host1x().MemoryManager().Map(device_address + (gpu_address - start), cpu_address, size,
                                        *asid, true);
}

void nvhost_as_gpu::Nx2SparseTile(u64 gpu_address, u64 cpu_address, u64 size) {
    std::scoped_lock lock(mutex);
    auto it = nx2_sparse_ranges.upper_bound(gpu_address);
    if (it == nx2_sparse_ranges.begin()) {
        return;
    }
    --it;
    const auto& [start, range] = *it;
    const auto& [region_size, device_address] = range;
    if (size > region_size || gpu_address - start > region_size - size) {
        return;
    }
    std::optional<Core::Asid> asid;
    container.WithSession(nx2_session, [&](const NvCore::Session& session) { asid = session.asid; });
    if (!asid) {
        return;
    }
    // In GPU-thread order: command lists submitted before the request still see the old tile.
    // Publish cached writes before changing either translation or its device backing.
    const DAddr device = device_address + (gpu_address - start);
    const auto canonical = [this, cpu_address, size]() -> std::optional<DAddr> {
        if (cpu_address == 0) {
            return std::nullopt;
        }
        auto session = nx2_session_state.lock();
        if (!session) {
            session = system.Nx2Session();
        }
        if (!session) {
            return std::nullopt;
        }
        return session->GpuMemoryRegistry().DeviceAddressOf(cpu_address, size);
    }();
    auto* const smmu = &system.Host1x().MemoryManager();
    auto gmmu_owner = gmmu;
    system.GPU().MapInOrder(
        gpu_address, size, [smmu, memory = std::move(gmmu_owner), device, cpu_address, size,
                           gpu_address, canonical, asid = *asid] {
            if (canonical) {
                memory->Map(gpu_address, *canonical, size, Tegra::PTEKind::PITCH, false);
                smmu->Unmap(device, size);
                return;
            }
            memory->PreserveMapping(gpu_address, size);
            smmu->Unmap(device, size);
            if (cpu_address != 0) {
                smmu->Map(device, cpu_address, size, asid, true);
            }
            memory->Map(gpu_address, device, size, Tegra::PTEKind::PITCH, false);
        });
}

void nvhost_as_gpu::OnClose(DeviceFD fd) {}

// NVGPU_AS_IOCTL_GET_SYNC_RO_MAP: Switch 2 NvRm (with characteristics flag 26) asks for the
// read-only syncpoint shim, a GPU page whose u32 at id * sync_size reads as that syncpoint's
// value; NVN waits on syncpoints through it and asserts without one. Same scheme as
// changes.patch, placed on the last page of the VA range.
NvResult nvhost_as_gpu::GetSyncRoMap(IoctlGetSyncRoMap& params) {
    std::scoped_lock lock(mutex);
    if (!vm.initialised || !gmmu) {
        return NvResult::NotSupported;
    }
    const u64 address = vm.va_range_end - VM::YUZU_PAGESIZE;
    if (!nx2_syncpoint_shim) {
        nx2_syncpoint_shim = true;
        gmmu->SetNativeSyncpointShim(address);
    }
    params.base_gpuva = address;
    params.sync_size = sizeof(u32);
    params.num_syncpoints = Tegra::MemoryManager::NumShimSyncpoints;
    LOG_INFO(Service_NVDRV, "Switch 2 syncpoint shim at {:#x} ({} syncpoints)", address,
             params.num_syncpoints);
    return NvResult::Success;
}

NvResult nvhost_as_gpu::AllocAsEx(IoctlAllocAsEx& params) {
    LOG_DEBUG(Service_NVDRV, "called, flags={:#x} big_page_size={:#x} va_range_start={:#x} va_range_end={:#x} va_range_split={:#x}", params.flags, params.big_page_size, params.va_range_start, params.va_range_end, params.va_range_split);

    std::scoped_lock lock(mutex);

    if (vm.initialised) {
        ASSERT_MSG(false, "Cannot initialise an address space twice!");
        return NvResult::InvalidState;
    }

    if (params.big_page_size) {
        if (!std::has_single_bit(params.big_page_size)) {
            LOG_ERROR(Service_NVDRV, "Non power-of-2 big page size: {:#x}!", params.big_page_size);
            return NvResult::BadValue;
        }

        if ((params.big_page_size & VM::SUPPORTED_BIG_PAGE_SIZES) == 0) {
            LOG_ERROR(Service_NVDRV, "Unsupported big page size: {:#x}!", params.big_page_size);
            return NvResult::BadValue;
        }

        vm.big_page_size = params.big_page_size;
        vm.big_page_size_bits = static_cast<u32>(std::countr_zero(params.big_page_size));

        vm.va_range_start = params.big_page_size << VM::VA_START_SHIFT;
    }

    // If this is unspecified then default values should be used
    if (params.va_range_start) {
        vm.va_range_start = params.va_range_start;
        vm.va_range_split = params.va_range_split;
        vm.va_range_end = params.va_range_end;
        if (vm.va_range_split == 0) {
            // Switch 2 NvRm (flags 5, no big page size) asks for small pages over the whole range
            // and then reserves 4 KiB VA up to 512 GiB. Keep a small big-page region past the end
            // so both allocators stay valid.
            vm.va_range_split = vm.va_range_end;
            vm.va_range_end += 4ULL << 30;
            vm.absolute_big_page_limit = true;
            LOG_WARNING(Service_NVDRV, "No VA split given; small pages up to {:#x}", vm.va_range_split);
        }
    }

    const u64 max_big_page_bits = Common::Log2Ceil(vm.va_range_end);

    const auto start_pages{static_cast<u32>(vm.va_range_start >> VM::PAGE_SIZE_BITS)};
    const auto end_pages{static_cast<u32>(vm.va_range_split >> VM::PAGE_SIZE_BITS)};
    vm.small_page_allocator.emplace(start_pages, end_pages);

    const auto start_big_pages{static_cast<u32>(vm.va_range_split >> vm.big_page_size_bits)};
    // The Switch 1 layout passes the big-page region length as the limit (so the limit stays above
    // the start there); a split past the region length needs the absolute end instead.
    const auto end_big_pages{static_cast<u32>(
        (vm.absolute_big_page_limit ? vm.va_range_end : vm.va_range_end - vm.va_range_split) >>
        vm.big_page_size_bits)};
    vm.big_page_allocator.emplace(start_big_pages, end_big_pages);

    gmmu = std::make_shared<Tegra::MemoryManager>(system, max_big_page_bits, vm.va_range_split, vm.big_page_size_bits, VM::PAGE_SIZE_BITS);
    system.GPU().InitAddressSpace(*gmmu);
    vm.initialised = true;

    return NvResult::Success;
}

NvResult nvhost_as_gpu::AllocateSpace(IoctlAllocSpace& params) {
    LOG_DEBUG(Service_NVDRV, "called, pages={:X}, page_size={:X}, flags={:X}", params.pages,
              params.page_size, params.flags);

    std::scoped_lock lock(mutex);

    if (!vm.initialised) {
        return NvResult::BadValue;
    }

    if (params.page_size != VM::YUZU_PAGESIZE && params.page_size != vm.big_page_size) {
        return NvResult::BadValue;
    }

    if (params.page_size != vm.big_page_size &&
        ((params.flags & MappingFlags::Sparse) != MappingFlags::None)) {
        UNIMPLEMENTED_MSG("Sparse small pages are not implemented!");
        return NvResult::NotImplemented;
    }

    const u32 page_size_bits{params.page_size == VM::YUZU_PAGESIZE ? VM::PAGE_SIZE_BITS
                                                                   : vm.big_page_size_bits};

    auto& allocator{params.page_size == VM::YUZU_PAGESIZE ? *vm.small_page_allocator
                                                          : *vm.big_page_allocator};

    if ((params.flags & MappingFlags::Fixed) != MappingFlags::None) {
        allocator.AllocateFixed(static_cast<u32>(params.offset >> page_size_bits), params.pages);
    } else {
        params.offset = static_cast<u64>(allocator.Allocate(params.pages)) << page_size_bits;
        if (!params.offset) {
            ASSERT_MSG(false, "Failed to allocate free space in the GPU AS!");
            return NvResult::InsufficientMemory;
        }
    }

    u64 size{static_cast<u64>(params.pages) * params.page_size};

    if ((params.flags & MappingFlags::Sparse) != MappingFlags::None) {
        gmmu->MapSparse(params.offset, size);
    }

    allocation_map[params.offset] = {
        .mappings{},
        .size = size,
        .page_size = params.page_size,
        .sparse = (params.flags & MappingFlags::Sparse) != MappingFlags::None,
        .big_pages = params.page_size != VM::YUZU_PAGESIZE,
    };

    return NvResult::Success;
}

bool nvhost_as_gpu::FreeMappingLocked(u64 offset) noexcept {
    if (auto const it = mapping_map.find(offset); it != mapping_map.end()) {
        auto const mapping = it->second;
        if (!mapping.fixed) {
            auto& allocator{mapping.big_page ? *vm.big_page_allocator : *vm.small_page_allocator};
            u32 page_size_bits{mapping.big_page ? vm.big_page_size_bits : VM::PAGE_SIZE_BITS};
            u32 page_size{mapping.big_page ? vm.big_page_size : VM::YUZU_PAGESIZE};
            u64 aligned_size{Common::AlignUp(mapping.size, page_size)};
            allocator.Free(u32(mapping.offset >> page_size_bits), u32(aligned_size >> page_size_bits));
        }
        nvmap.UnpinHandle(mapping.handle);
        // Sparse mappings shouldn't be fully unmapped, just returned to their sparse state
        // Only FreeSpace can unmap them fully
        if (mapping.sparse_alloc) {
            gmmu->MapSparse(offset, mapping.size, mapping.big_page);
        } else {
            gmmu->Unmap(offset, mapping.size);
        }
        mapping_map.erase(offset);
        map_buffer_offsets.erase(offset);
        return true;
    }
    return false;
}

NvResult nvhost_as_gpu::FreeSpace(IoctlFreeSpace& params) {
    LOG_DEBUG(Service_NVDRV, "called, offset={:X}, pages={:X}, page_size={:X}", params.offset, params.pages, params.page_size);
    std::scoped_lock lock(mutex);
    if (!vm.initialised) {
        return NvResult::BadValue;
    }
    if (auto const it = allocation_map.find(params.offset); it != allocation_map.end()) {
        auto const allocation = it->second;
        if (allocation.page_size != params.page_size || allocation.size != (u64(params.pages) * params.page_size))
            return NvResult::BadValue;

        for (const auto mapping_offset : allocation.mappings)
            if (!FreeMappingLocked(mapping_offset))
                return NvResult::BadValue;

        // Unset sparse flag if required
        if (allocation.sparse)
            gmmu->Unmap(params.offset, allocation.size);

        auto& allocator{params.page_size == VM::YUZU_PAGESIZE ? *vm.small_page_allocator : *vm.big_page_allocator};
        u32 page_size_bits{params.page_size == VM::YUZU_PAGESIZE ? VM::PAGE_SIZE_BITS : vm.big_page_size_bits};

        allocator.Free(u32(params.offset >> page_size_bits), u32(allocation.size >> page_size_bits));
        allocation_map.erase(params.offset);
        return NvResult::Success;
    }
    return NvResult::BadValue;
}

NvResult nvhost_as_gpu::Remap(std::span<IoctlRemapEntry> entries) {
    LOG_DEBUG(Service_NVDRV, "called, num_entries={:#x}", entries.size());

    std::scoped_lock lock(mutex);

    if (!vm.initialised) {
        return NvResult::BadValue;
    }

    for (const auto& entry : entries) {
        GPUVAddr virtual_address{static_cast<u64>(entry.as_offset_big_pages)
                                 << vm.big_page_size_bits};
        u64 size{static_cast<u64>(entry.big_pages) << vm.big_page_size_bits};

        auto alloc{allocation_map.upper_bound(virtual_address)};

        if (alloc-- == allocation_map.begin() ||
            size > alloc->second.size ||
            (virtual_address - alloc->first) > alloc->second.size - size) {
            LOG_WARNING(Service_NVDRV, "Cannot remap into an unallocated region!");
            return NvResult::BadValue;
        }

        if (!alloc->second.sparse) {
            LOG_WARNING(Service_NVDRV, "Cannot remap a non-sparse mapping!");
            return NvResult::BadValue;
        }

        const bool use_big_pages = alloc->second.big_pages;
        if (!entry.handle) {
            gmmu->MapSparse(virtual_address, size, use_big_pages);
        } else {
            auto handle{nvmap.GetHandle(entry.handle)};
            if (!handle) {
                return NvResult::BadValue;
            }

            DAddr base = nvmap.PinHandle(entry.handle, false);
            if (base == 0) {
                return NvResult::BadValue;
            }
            DAddr device_address{static_cast<DAddr>(
                base + (static_cast<u64>(entry.handle_offset_big_pages) << vm.big_page_size_bits))};

            gmmu->Map(virtual_address, device_address, size,
                      static_cast<Tegra::PTEKind>(entry.kind), use_big_pages);
        }
    }

    return NvResult::Success;
}

NvResult nvhost_as_gpu::MapBufferEx(IoctlMapBufferEx& params) {
    LOG_DEBUG(Service_NVDRV,
              "called, flags={:X}, nvmap_handle={:X}, buffer_offset={}, mapping_size={}"
              ", offset={:#x}",
              params.flags, params.handle, params.buffer_offset, params.mapping_size,
              params.offset);

    std::scoped_lock lock(mutex);

    if (!vm.initialised) {
        return NvResult::BadValue;
    }

    // Remaps a subregion of an existing mapping to a different PA
    if ((params.flags & MappingFlags::Remap) != MappingFlags::None) {
        if (auto const it = mapping_map.find(params.offset); it != mapping_map.end()) {
            auto const mapping = it->second;
            if (mapping.size < params.mapping_size) {
                LOG_WARNING(Service_NVDRV, "Cannot remap a partially mapped GPU address space region: {:#x}", params.offset);
                return NvResult::BadValue;
            }
            u64 gpu_address = u64(params.offset + params.buffer_offset);
            VAddr device_address{mapping.ptr + params.buffer_offset};
            gmmu->Map(gpu_address, device_address, params.mapping_size, Tegra::PTEKind(params.kind), mapping.big_page);
            return NvResult::Success;
        } else {
            LOG_WARNING(Service_NVDRV, "Cannot remap an unmapped GPU address space region: {:#x}", params.offset);
            return NvResult::BadValue;
        }
    }

    auto handle{nvmap.GetHandle(params.handle)};
    if (!handle) {
        return NvResult::BadValue;
    }

    const DAddr base = nvmap.PinHandle(params.handle, false);
    if (base == 0) {
        return NvResult::BadValue;
    }
    bool pinned = true;
    SCOPE_EXIT {
        if (pinned) {
            nvmap.UnpinHandle(params.handle);
        }
    };
    DAddr device_address = DAddr(base + params.buffer_offset);
    u64 size{params.mapping_size ? params.mapping_size : handle->orig_size};

    bool big_page{[&]() {
        if (Common::IsAligned(handle->align, vm.big_page_size)) {
            return true;
        } else if (Common::IsAligned(handle->align, VM::YUZU_PAGESIZE)) {
            return false;
        } else {
            ASSERT(false);
            return false;
        }
    }()};

    if ((params.flags & MappingFlags::Fixed) != MappingFlags::None) {
        auto alloc{allocation_map.upper_bound(params.offset)};

        if (alloc-- == allocation_map.begin() ||
            size > alloc->second.size ||
            (params.offset - alloc->first) > alloc->second.size - size) {
            LOG_ERROR(Service_NVDRV, "Cannot perform a fixed mapping into an unallocated region!");
            return NvResult::BadValue;
        }

        const bool use_big_pages = alloc->second.big_pages && big_page;
        gmmu->Map(params.offset, device_address, size, static_cast<Tegra::PTEKind>(params.kind), use_big_pages);

        alloc->second.mappings.push_back(params.offset);
        mapping_map.insert_or_assign(params.offset, Mapping(params.handle, device_address, params.offset, size, true, use_big_pages, alloc->second.sparse));
    } else {
        auto& allocator{big_page ? *vm.big_page_allocator : *vm.small_page_allocator};
        u32 page_size{big_page ? vm.big_page_size : VM::YUZU_PAGESIZE};
        u32 page_size_bits{big_page ? vm.big_page_size_bits : VM::PAGE_SIZE_BITS};

        const u64 aligned_size = Common::AlignUp(size, page_size);
        const u64 page_count = aligned_size >> page_size_bits;
        if (aligned_size < size || page_count == 0 ||
            page_count > std::numeric_limits<u32>::max()) {
            return NvResult::BadValue;
        }

        params.offset = u64(allocator.Allocate(static_cast<u32>(page_count))) << page_size_bits;
        if (!params.offset) {
            LOG_ERROR(Service_NVDRV, "Failed to allocate free space in the GPU AS!");
            return NvResult::InsufficientMemory;
        }
        gmmu->Map(params.offset, device_address, aligned_size, Tegra::PTEKind(params.kind), big_page);
        mapping_map.insert_or_assign(params.offset, Mapping(params.handle, device_address, params.offset, size, false, big_page, false));
    }

    map_buffer_offsets.insert(params.offset);
    pinned = false;

    return NvResult::Success;
}

NvResult nvhost_as_gpu::UnmapBuffer(IoctlUnmapBuffer& params) {
    std::scoped_lock lock(mutex);
    if (auto const offset_it = map_buffer_offsets.find(params.offset); offset_it != map_buffer_offsets.end()) {
        LOG_DEBUG(Service_NVDRV, "called, offset={:#x}", params.offset);
        if (!vm.initialised) {
            return NvResult::BadValue;
        }

        auto const it = mapping_map.find(params.offset);
        if (it == mapping_map.end()) {
            map_buffer_offsets.erase(offset_it);
            return NvResult::BadValue;
        }
        auto const mapping = it->second;
        if (!mapping.fixed) {
            auto& allocator{mapping.big_page ? *vm.big_page_allocator : *vm.small_page_allocator};
            u32 page_size_bits{mapping.big_page ? vm.big_page_size_bits : VM::PAGE_SIZE_BITS};
            allocator.Free(u32(mapping.offset >> page_size_bits), u32(mapping.size >> page_size_bits));
        }

        // Sparse mappings shouldn't be fully unmapped, just returned to their sparse state
        // Only FreeSpace can unmap them fully
        if (mapping.sparse_alloc) {
            gmmu->MapSparse(params.offset, mapping.size, mapping.big_page);
        } else {
            gmmu->Unmap(params.offset, mapping.size);
        }

        nvmap.UnpinHandle(mapping.handle);
        mapping_map.erase(params.offset);
        map_buffer_offsets.erase(params.offset);
    }
    return NvResult::Success;
}

NvResult nvhost_as_gpu::BindChannel(IoctlBindChannel& params) {
    LOG_DEBUG(Service_NVDRV, "called, fd={:X}", params.fd);

    auto gpu_channel_device = module.GetDevice<nvhost_gpu>(params.fd);
    if (gpu_channel_device == nullptr || gpu_channel_device->channel_state == nullptr) {
        return NvResult::BadParameter;
    }
    gpu_channel_device->channel_state->memory_manager = gmmu.get();
    return NvResult::Success;
}

void nvhost_as_gpu::GetVARegionsImpl(IoctlGetVaRegions& params) {
    params.buf_size = 2 * sizeof(VaRegion);

    params.regions = std::array<VaRegion, 2>{
        VaRegion{
            .offset = vm.small_page_allocator->GetVAStart() << VM::PAGE_SIZE_BITS,
            .page_size = VM::YUZU_PAGESIZE,
            ._pad0_{},
            .pages = vm.small_page_allocator->GetVALimit() - vm.small_page_allocator->GetVAStart(),
        },
        VaRegion{
            .offset = vm.big_page_allocator->GetVAStart() << vm.big_page_size_bits,
            .page_size = vm.big_page_size,
            ._pad0_{},
            .pages = vm.big_page_allocator->GetVALimit() - vm.big_page_allocator->GetVAStart(),
        },
    };
}

NvResult nvhost_as_gpu::GetVARegions1(IoctlGetVaRegions& params) {
    LOG_DEBUG(Service_NVDRV, "called, buf_addr={:X}, buf_size={:X}", params.buf_addr, params.buf_size);

    std::scoped_lock lock(mutex);

    if (!vm.initialised) {
        return NvResult::BadValue;
    }

    GetVARegionsImpl(params);

    return NvResult::Success;
}

NvResult nvhost_as_gpu::GetVARegions3(IoctlGetVaRegions& params, std::span<VaRegion> regions) {
    LOG_DEBUG(Service_NVDRV, "called, buf_addr={:X}, buf_size={:X}", params.buf_addr,
              params.buf_size);

    std::scoped_lock lock(mutex);

    if (!vm.initialised) {
        return NvResult::BadValue;
    }

    GetVARegionsImpl(params);

    const size_t num_regions = (std::min)(params.regions.size(), regions.size());
    for (size_t i = 0; i < num_regions; i++) {
        regions[i] = params.regions[i];
    }

    return NvResult::Success;
}

Kernel::KEvent* nvhost_as_gpu::QueryEvent(u32 event_id) {
    LOG_CRITICAL(Service_NVDRV, "Unknown AS GPU Event {}", event_id);
    return nullptr;
}

} // namespace Service::Nvidia::Devices
