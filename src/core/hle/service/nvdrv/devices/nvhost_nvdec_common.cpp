// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <limits>

#include "common/common_types.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/nvdrv/core/container.h"
#include "core/hle/service/nvdrv/core/nvmap.h"
#include "core/hle/service/nvdrv/core/syncpoint_manager.h"
#include "core/hle/service/nvdrv/devices/nvhost_nvdec_common.h"
#include "core/memory.h"
#include "video_core/host1x/host1x.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_base.h"

namespace Service::Nvidia::Devices {

nvhost_nvdec_common::nvhost_nvdec_common(Core::System& system_, NvCore::Container& core_,
                                         NvCore::ChannelType channel_type_, DeviceKind kind_)
    : nvdevice{system_, kind_}, host1x{system_.Host1x()}, core{core_},
      syncpoint_manager{core.GetSyncpointManager()}, nvmap{core.GetNvMapFile()},
      channel_type{channel_type_} {
    auto& syncpts_accumulated = core.Host1xDeviceFile().syncpts_accumulated;
    if (syncpts_accumulated.empty()) {
        channel_syncpoint = syncpoint_manager.AllocateSyncpoint(false);
    } else {
        channel_syncpoint = syncpts_accumulated.front();
        syncpts_accumulated.pop_front();
    }
}

nvhost_nvdec_common::~nvhost_nvdec_common() {
    core.Host1xDeviceFile().syncpts_accumulated.push_back(channel_syncpoint);
}

NvResult nvhost_nvdec_common::SetNVMAPfd(IoctlSetNvmapFD& params) {
    LOG_DEBUG(Service_NVDRV, "called, fd={}", params.nvmap_fd);

    nvmap_fd = params.nvmap_fd;
    return NvResult::Success;
}

NvResult nvhost_nvdec_common::Submit(IoctlSubmit& params, std::span<u8> data, DeviceFD fd) {
    LOG_DEBUG(Service_NVDRV, "called NVDEC Submit, cmd_buffer_count={}", params.cmd_buffer_count);

    const size_t command_count = params.cmd_buffer_count;
    const size_t relocation_count = params.relocation_count;
    const size_t syncpoint_count = params.syncpoint_count;
    const size_t fence_count = params.fence_count;

    size_t offset = 0;
    const auto take_array = [&](size_t count, size_t element_size) {
        if (count > (data.size() - offset) / element_size) {
            return false;
        }
        offset += count * element_size;
        return true;
    };
    if (syncpoint_count > fence_count || !take_array(command_count, sizeof(CommandBuffer)) ||
        !take_array(relocation_count, sizeof(Reloc) + sizeof(u32))) {
        return NvResult::InvalidSize;
    }
    const size_t syncpoints_offset = offset;
    if (!take_array(syncpoint_count, sizeof(SyncptIncr)) ||
        !take_array(fence_count, sizeof(u32))) {
        return NvResult::InvalidSize;
    }
    const size_t fences_offset = syncpoints_offset + syncpoint_count * sizeof(SyncptIncr);

    const auto session_it = sessions.find(fd);
    if (session_it == sessions.end()) {
        return NvResult::InvalidState;
    }
    Kernel::KScopedAutoObject<Kernel::KProcess> process{system.Kernel()};
    if (!core.WithSession(session_it->second, [&](const NvCore::Session& session) {
            process.Reset(session.process);
        }) ||
        process.IsNull()) {
        return NvResult::InvalidState;
    }

    std::vector<SyncptIncr> syncpt_increments(syncpoint_count);
    if (!syncpt_increments.empty()) {
        std::memcpy(syncpt_increments.data(), data.data() + syncpoints_offset,
                    syncpoint_count * sizeof(SyncptIncr));
    }
    for (const auto& increment : syncpt_increments) {
        if (!syncpoint_manager.IsSyncpointAllocated(increment.id)) {
            return NvResult::BadParameter;
        }
    }

    struct CommandRange {
        VAddr address;
        size_t word_count;
    };
    std::vector<CommandRange> command_ranges;
    command_ranges.reserve(command_count);
    for (size_t i = 0; i < command_count; ++i) {
        CommandBuffer cmd_buffer;
        std::memcpy(&cmd_buffer, data.data() + i * sizeof(CommandBuffer), sizeof(CommandBuffer));
        const auto object = nvmap.GetHandle(cmd_buffer.memory_id);
        if (!object) {
            return NvResult::InvalidState;
        }
        if (cmd_buffer.word_count < 0) {
            return NvResult::BadParameter;
        }
        const size_t word_count = static_cast<size_t>(cmd_buffer.word_count);
        std::scoped_lock object_lock{object->mutex};
        if (cmd_buffer.offset > object->size ||
            word_count > (object->size - cmd_buffer.offset) / sizeof(Tegra::ChCommandHeader) ||
            object->address > std::numeric_limits<VAddr>::max() - cmd_buffer.offset) {
            return NvResult::BadParameter;
        }
        const VAddr address = object->address + cmd_buffer.offset;
        if (word_count != 0 &&
            (object->address == 0 || !process->GetMemory().IsValidVirtualAddressRange(
                                         address, word_count * sizeof(Tegra::ChCommandHeader)))) {
            return NvResult::BadParameter;
        }
        command_ranges.push_back({address, word_count});
    }

    for (size_t i = 0; i < syncpt_increments.size(); ++i) {
        const auto& increment = syncpt_increments[i];
        const u32 threshold =
            syncpoint_manager.IncrementSyncpointMaxExt(increment.id, increment.increments);
        std::memcpy(data.data() + fences_offset + i * sizeof(u32), &threshold, sizeof(threshold));
    }

    for (const auto& command : command_ranges) {
        Core::Memory::CpuGuestMemory<Tegra::ChCommandHeader,
                                     Core::Memory::GuestMemoryFlags::SafeRead>
            cmdlist(process->GetMemory(), command.address, command.word_count);
        host1x.PushEntries(fd, std::move(cmdlist));
    }

    return NvResult::Success;
}

NvResult nvhost_nvdec_common::GetSyncpoint(IoctlGetSyncpoint& params) {
    LOG_DEBUG(Service_NVDRV, "called GetSyncpoint, id={}", params.param);
    params.value = channel_syncpoint;
    return NvResult::Success;
}

NvResult nvhost_nvdec_common::GetWaitbase(IoctlGetWaitbase& params) {
    LOG_DEBUG(Service_NVDRV, "called WAITBASE");
    params.value = 0;
    return NvResult::Success;
}

NvResult nvhost_nvdec_common::MapBuffer(IoctlMapBuffer& params, std::span<MapBufferEntry> entries,
                                        DeviceFD fd) {
    const size_t num_entries = (std::min)(params.num_entries, static_cast<u32>(entries.size()));
    for (size_t i = 0; i < num_entries; i++) {
        DAddr pin_address = nvmap.PinHandle(entries[i].map_handle, true);
        if (pin_address == 0) {
            for (size_t pinned = 0; pinned < i; ++pinned) {
                nvmap.UnpinHandle(entries[pinned].map_handle);
                entries[pinned].map_address = 0;
            }
            entries[i].map_address = 0;
            return NvResult::BadValue;
        }
        entries[i].map_address = static_cast<u32>(pin_address);
    }

    return NvResult::Success;
}

NvResult nvhost_nvdec_common::UnmapBuffer(IoctlMapBuffer& params,
                                          std::span<MapBufferEntry> entries) {
    const size_t num_entries = (std::min)(params.num_entries, static_cast<u32>(entries.size()));
    for (size_t i = 0; i < num_entries; i++) {
        nvmap.UnpinHandle(entries[i].map_handle);
        entries[i] = {};
    }

    params = {};
    return NvResult::Success;
}

NvResult nvhost_nvdec_common::SetSubmitTimeout(u32 timeout) {
    LOG_WARNING(Service_NVDRV, "(STUBBED) called");
    return NvResult::Success;
}

NvResult nvhost_nvdec_common::GetClkRate(IoctlGetClkRate& params) {
    LOG_WARNING(Service_NVDRV, "(STUBBED) called");
    params.clk_rate = 614400000;
    params.module_id = 0;
    return NvResult::Success;
}

Kernel::KEvent* nvhost_nvdec_common::QueryEvent(u32 event_id) {
    LOG_CRITICAL(Service_NVDRV, "Unknown HOSTX1 Event {}", event_id);
    return nullptr;
}

} // namespace Service::Nvidia::Devices
