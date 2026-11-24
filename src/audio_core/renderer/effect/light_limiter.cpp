// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "audio_core/renderer/effect/light_limiter.h"

namespace AudioCore::Renderer {

namespace {
template <typename InParams>
void UpdateLightLimiter(LightLimiterInfo::ParameterVersion1* params, const InParams& in_params,
                        BehaviorInfo::ErrorInfo& error_info, const PoolMapper& pool_mapper,
                        s32& mix_id, s32& process_order, bool& enabled,
                        EffectInfoBase::UsageState& usage_state, bool& buffer_unmapped,
                        AddressInfo& workbuffer) {
    const auto in_specific{
        reinterpret_cast<const LightLimiterInfo::ParameterVersion1*>(in_params.specific.data())};

    std::memcpy(params, in_specific, sizeof(LightLimiterInfo::ParameterVersion1));
    if (!IsChannelCountValid(params->channel_count)) {
        params->channel_count =
            IsChannelCountValid(params->channel_count_max) ? params->channel_count_max : 0;
    }

    mix_id = in_params.mix_id;
    process_order = in_params.process_order;
    enabled = in_params.enabled && IsChannelCountValid(params->channel_count);

    if (buffer_unmapped || in_params.is_new) {
        usage_state = EffectInfoBase::UsageState::New;
        params->state = EffectInfoBase::ParameterState::Initialized;
        buffer_unmapped = !pool_mapper.TryAttachBuffer(error_info, workbuffer, in_params.workbuffer,
                                                       in_params.workbuffer_size);
    } else {
        error_info.error_code = ResultSuccess;
        error_info.address = CpuAddr(0);
    }
}
} // namespace

void LightLimiterInfo::Update(BehaviorInfo::ErrorInfo& error_info,
                              const InParameterVersion1& in_params, const PoolMapper& pool_mapper) {
    UpdateLightLimiter(reinterpret_cast<ParameterVersion1*>(parameter.data()), in_params, error_info,
                       pool_mapper, mix_id, process_order, enabled, usage_state, buffer_unmapped,
                       workbuffers[0]);
}

void LightLimiterInfo::Update(BehaviorInfo::ErrorInfo& error_info,
                              const InParameterVersion2& in_params, const PoolMapper& pool_mapper) {
    UpdateLightLimiter(reinterpret_cast<ParameterVersion1*>(parameter.data()), in_params, error_info,
                       pool_mapper, mix_id, process_order, enabled, usage_state, buffer_unmapped,
                       workbuffers[0]);
}

void LightLimiterInfo::UpdateForCommandGeneration() {
    if (enabled) {
        usage_state = UsageState::Enabled;
    } else {
        usage_state = UsageState::Disabled;
    }

    auto params{reinterpret_cast<ParameterVersion1*>(parameter.data())};
    params->state = ParameterState::Updated;
    params->statistics_reset_required = false;
}

void LightLimiterInfo::InitializeResultState(EffectResultState& result_state) {
    auto result_state_{reinterpret_cast<StatisticsInternal*>(result_state.state.data())};

    result_state_->channel_max_sample.fill(0);
    result_state_->channel_compression_gain_min.fill(1.0f);
}

void LightLimiterInfo::UpdateResultState(EffectResultState& cpu_state,
                                         EffectResultState& dsp_state) {
    auto cpu_statistics{reinterpret_cast<StatisticsInternal*>(cpu_state.state.data())};
    auto dsp_statistics{reinterpret_cast<StatisticsInternal*>(dsp_state.state.data())};

    *cpu_statistics = *dsp_statistics;
}

CpuAddr LightLimiterInfo::GetWorkbuffer(s32 index) {
    return GetSingleBuffer(index);
}

} // namespace AudioCore::Renderer
