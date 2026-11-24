// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/assert.h"
#include "video_core/control/channel_state_cache.inc"

namespace VideoCommon {

ChannelInfo::ChannelInfo(const Tegra::Control::ChannelStateBridge& bridge_)
    : bridge{bridge_}
    , maxwell3d{[&]() -> Tegra::Engines::Maxwell3D& {
        ASSERT(bridge.HasMaxwell3D());
        return *bridge.Maxwell3DPtr();
    }()}
    , kepler_compute{[&]() -> Tegra::Engines::KeplerCompute& {
        ASSERT(bridge.HasKeplerCompute());
        return *bridge.KeplerComputePtr();
    }()}
    , gpu_memory{[&]() -> Tegra::MemoryManager& {
        ASSERT(bridge.HasGpuMemory());
        return *bridge.GpuMemoryPtr();
    }()}
    , program_id{bridge.ProgramId()} {}

ChannelInfo::ChannelInfo(Tegra::Control::ChannelState& state_)
    : ChannelInfo{Tegra::Control::ChannelStateBridge{state_}} {}

template class VideoCommon::ChannelSetupCaches<VideoCommon::ChannelInfo>;

} // namespace VideoCommon
