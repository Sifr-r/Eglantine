// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "audio_core/renderer/effect/biquad_filter.h"

namespace AudioCore::Renderer {

void BiquadFilterInfo::Update(BehaviorInfo::ErrorInfo& error_info,
                              const InParameterVersion1& in_params, const PoolMapper& pool_mapper) {
    auto in_specific{reinterpret_cast<const ParameterVersion1*>(in_params.specific.data())};
    auto params{reinterpret_cast<ParameterVersion1*>(parameter.data())};

    std::memcpy(params, in_specific, sizeof(ParameterVersion1));
    mix_id = in_params.mix_id;
    process_order = in_params.process_order;
    enabled = in_params.enabled;
    parameter_version2 = false;

    error_info.error_code = ResultSuccess;
    error_info.address = CpuAddr(0);
}

void BiquadFilterInfo::Update(BehaviorInfo::ErrorInfo& error_info,
                              const InParameterVersion2& in_params, const PoolMapper& pool_mapper) {
    auto in_specific{reinterpret_cast<const ParameterVersion2*>(in_params.specific.data())};
    auto params{reinterpret_cast<ParameterVersion2*>(parameter.data())};

    std::memcpy(params, in_specific, sizeof(ParameterVersion2));
    mix_id = in_params.mix_id;
    process_order = in_params.process_order;
    enabled = in_params.enabled;
    parameter_version2 = true;

    error_info.error_code = ResultSuccess;
    error_info.address = CpuAddr(0);
}

void BiquadFilterInfo::UpdateForCommandGeneration() {
    usage_state = enabled ? UsageState::Enabled : UsageState::Disabled;

    if (parameter_version2) {
        auto* params = reinterpret_cast<ParameterVersion2*>(parameter.data());
        params->state = ParameterState::Updated;
    } else {
        auto* params = reinterpret_cast<ParameterVersion1*>(parameter.data());
        params->state = ParameterState::Updated;
    }
}

void BiquadFilterInfo::InitializeResultState(EffectResultState& result_state) {}

void BiquadFilterInfo::UpdateResultState(EffectResultState& cpu_state,
                                         EffectResultState& dsp_state) {}

} // namespace AudioCore::Renderer
