// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/core.h"
#include "video_core/host1x/host1x.h"
#include "video_core/host1x/nvdec.h"
#include "video_core/host1x/vic.h"

namespace Tegra::Host1x {

Host1x::Host1x(Core::System& system_)
    : system{system_}
    , syncpoint_manager{}
    , memory_manager(system.DeviceMemory(), system_.Platform().device_address_bits)
    , gmmu_manager{system, memory_manager, 32, 0, 12}
    , allocator{1 << 12}
{}

Host1x::~Host1x() = default;

void Host1x::StartDevice(s32 fd, ChannelType type, u32 syncpt) {
    if (type != ChannelType::NvDec && type != ChannelType::VIC) {
        LOG_ERROR(HW_GPU, "Unimplemented host1x device {}", u32(type));
        return;
    }
#ifdef YUZU_LEGACY
    if (type == ChannelType::NvDec) {
        std::call_once(nvdec_first_init, []() {std::this_thread::sleep_for(std::chrono::milliseconds{500});}); // HACK: For Astroneer
    } else {
        std::call_once(vic_first_init, []() {std::this_thread::sleep_for(std::chrono::milliseconds{500});}); // HACK: For Astroneer
    }
#endif
    for (;;) {
        {
            std::scoped_lock lock{devices_mutex};
            if (!devices.contains(fd)) {
                const auto it = devices.try_emplace(fd).first;
                if (type == ChannelType::NvDec) {
                    it->second.emplace<Tegra::Host1x::Nvdec>(*this, fd, syncpt);
                } else {
                    it->second.emplace<Tegra::Host1x::Vic>(*this, fd, syncpt);
                }
                return;
            }
        }
        // Extract under lock so device teardown (joining CDmaPusher thread) happens outside devices_mutex.
        [&] {
            std::scoped_lock lock{devices_mutex};
            return devices.extract(fd);
        }();
    }
}

void Host1x::StopDevice(s32 fd, ChannelType /*type*/) {
    // Extract under lock so device teardown (joining CDmaPusher thread) happens after unlock.
    [&] {
        std::scoped_lock lock{devices_mutex};
        return devices.extract(fd);
    }();
}

} // namespace Tegra::Host1x
