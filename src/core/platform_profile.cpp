// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string_view>

#include "common/cpu_features.h"
#include "common/logging.h"
#include "core/file_sys/program_metadata.h"
#include "core/loader/loader.h"
#include "core/platform_profile.h"

namespace Core {

PlatformProfile MakePlatformProfile(PlatformResolution resolution) noexcept {
    switch (resolution) {
    case PlatformResolution::ConfirmedSwitch2:
    case PlatformResolution::OverrideSwitch2:
        return PlatformProfile{
            .kind = PlatformKind::Switch2,
            .resolution = resolution,
            .guest_cntfrq = Common::WallClock::Switch2CNTFRQ,
            .cpu_cores = 6,
            .device_address_bits = 36,
            .prefer_async_buffer_downloads = false,
            .force_12gb_memory_layout = true,
            .reset_dram_on_shutdown = true,
            .allow_contradictory_nx2_gpu_query = false,
        };
    case PlatformResolution::ConfirmedSwitch1:
    case PlatformResolution::OverrideSwitch1:
        return PlatformProfile{
            .kind = PlatformKind::Switch1,
            .resolution = resolution,
            .guest_cntfrq = Common::WallClock::CNTFRQ,
            .cpu_cores = 4,
            .device_address_bits = 34,
            .prefer_async_buffer_downloads = true,
            .force_12gb_memory_layout = false,
            .reset_dram_on_shutdown = false,
            .allow_contradictory_nx2_gpu_query = false,
        };
    case PlatformResolution::DefaultSwitch1:
    default:
        return PlatformProfile{
            .kind = PlatformKind::Switch1,
            .resolution = PlatformResolution::DefaultSwitch1,
            .guest_cntfrq = Common::WallClock::CNTFRQ,
            .cpu_cores = 4,
            .device_address_bits = 34,
            .prefer_async_buffer_downloads = true,
            .force_12gb_memory_layout = false,
            .reset_dram_on_shutdown = false,
            .allow_contradictory_nx2_gpu_query = true,
        };
    }
}

std::optional<PlatformResolution> ParsePlatformOverride(const char* value) noexcept {
    if (value == nullptr) {
        return std::nullopt;
    }
    const std::string_view text{value};
    if (text == "1") {
        return PlatformResolution::OverrideSwitch1;
    }
    if (text == "2") {
        return PlatformResolution::OverrideSwitch2;
    }
    return std::nullopt;
}

PlatformProfile ResolvePlatformProfile(System& system, Loader::AppLoader* loader,
                                       std::optional<PlatformResolution> env_override) {
    if (env_override.has_value()) {
        const auto profile = MakePlatformProfile(*env_override);
        LOG_INFO(Core, "Platform override active: {}", PlatformResolutionName(profile.resolution));
        return profile;
    }

    if (loader == nullptr) {
        LOG_WARNING(Core, "No loader available for platform probe; using DefaultSwitch1");
        return MakePlatformProfile(PlatformResolution::DefaultSwitch1);
    }

    FileSys::ProgramMetadata metadata;
    const auto status = loader->ReadProgramMetadata(system, metadata);
    if (status == Loader::ResultStatus::Success) {
        return MakePlatformProfile(metadata.IsSwitch2() ? PlatformResolution::ConfirmedSwitch2
                                                        : PlatformResolution::ConfirmedSwitch1);
    }

    if (status == Loader::ResultStatus::ErrorMissingNPDM ||
        status == Loader::ResultStatus::ErrorNotImplemented) {
        LOG_INFO(Core, "Program has no main.npdm metadata ({}); using DefaultSwitch1",
                 Loader::GetResultStatusString(status));
        return MakePlatformProfile(PlatformResolution::DefaultSwitch1);
    }

    LOG_WARNING(Core, "Failed to read program metadata for platform probe ({}); using DefaultSwitch1",
                Loader::GetResultStatusString(status));
    return MakePlatformProfile(PlatformResolution::DefaultSwitch1);
}

std::string_view PlatformResolutionName(PlatformResolution resolution) noexcept {
    switch (resolution) {
    case PlatformResolution::DefaultSwitch1:
        return "DefaultSwitch1";
    case PlatformResolution::ConfirmedSwitch1:
        return "ConfirmedSwitch1";
    case PlatformResolution::ConfirmedSwitch2:
        return "ConfirmedSwitch2";
    case PlatformResolution::OverrideSwitch1:
        return "OverrideSwitch1";
    case PlatformResolution::OverrideSwitch2:
        return "OverrideSwitch2";
    }
    return "Unknown";
}

} // namespace Core
