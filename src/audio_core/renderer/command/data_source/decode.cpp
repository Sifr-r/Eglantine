// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "audio_core/renderer/command/data_source/decode.h"
#include "audio_core/renderer/command/resample/resample.h"
#include "common/fixed_point.h"
#include "common/logging.h"
#include "common/scratch_buffer.h"
#include "core/guest_memory.h"
#include "core/memory.h"

namespace AudioCore::Renderer {

constexpr u32 TempBufferSize = 0x3F00;

/// @brief Decode PCM data. Only s16 or f32 is supported.
/// @tparam T         - Type to decode. Only s16 and f32 are supported.
/// @param memory     - Core memory for reading samples.
/// @param out_buffer - Output mix buffer to receive the samples.
/// @param req        - Information for how to decode.
/// @return Number of samples decoded.
template <typename T>
static u32 DecodePcm(Core::Memory::Memory& memory, std::span<s16> out_buffer, const DecodeArg& req) {
    constexpr s32 min{(std::numeric_limits<s16>::min)()};
    constexpr s32 max{(std::numeric_limits<s16>::max)()};

    if (req.buffer == 0 || req.buffer_size == 0) {
        return 0;
    }

    if (req.start_offset >= req.end_offset || req.offset >= req.end_offset - req.start_offset) {
        return 0;
    }

    const u32 samples_to_decode = std::min({req.samples_to_read,
                                          req.end_offset - req.start_offset - req.offset,
                                          static_cast<u32>(out_buffer.size())});
    if (samples_to_decode == 0) {
        return 0;
    }
    const u32 channel_count = static_cast<u32>(req.channel_count);
    const u64 frame_size = channel_count * sizeof(T);
    const u64 start_byte = (u64{req.start_offset} + req.offset) * frame_size;
    const u64 end_byte = start_byte + samples_to_decode * frame_size;
    if (end_byte > req.buffer_size ||
        req.buffer > (std::numeric_limits<CpuAddr>::max)() - end_byte) {
        return 0;
    }
    const VAddr source{req.buffer + start_byte};
    const auto float_to_pcm = [](f32 sample) -> s16 {
        if (std::isnan(sample)) {
            return 0;
        }
        return static_cast<s16>(std::clamp(sample * max, static_cast<f32>(min),
                                           static_cast<f32>(max)));
    };

    switch (req.channel_count) {
    default: {
        const u64 size{u64{channel_count} * samples_to_decode};

        Core::Memory::CpuGuestMemory<T, Core::Memory::GuestMemoryFlags::UnsafeRead> samples(
            memory, source, size);
        if constexpr (std::is_floating_point_v<T>) {
            for (u32 i = 0; i < samples_to_decode; i++) {
                out_buffer[i] = float_to_pcm(samples[i * channel_count + req.target_channel]);
            }
        } else {
            for (u32 i = 0; i < samples_to_decode; i++) {
                out_buffer[i] = samples[i * channel_count + req.target_channel];
            }
        }
    } break;

    case 1:
        if (req.target_channel != 0) {
            LOG_ERROR(Service_Audio, "Invalid target channel, expected 0, got {}",
                      req.target_channel);
            return 0;
        }

        Core::Memory::CpuGuestMemory<T, Core::Memory::GuestMemoryFlags::UnsafeRead> samples(
            memory, source, samples_to_decode);

        if constexpr (std::is_floating_point_v<T>) {
            for (u32 i = 0; i < samples_to_decode; i++) {
                out_buffer[i] = float_to_pcm(samples[i]);
            }
        } else {
            std::memcpy(out_buffer.data(), samples.data(), samples_to_decode * sizeof(s16));
        }
        break;
    }

    return samples_to_decode;
}

/// @brief Decode ADPCM data.
/// @param memory     - Core memory for reading samples.
/// @param out_buffer - Output mix buffer to receive the samples.
/// @param req        - Information for how to decode.
/// @return Number of samples decoded.
static u32 DecodeAdpcm(Core::Memory::Memory& memory, std::span<s16> out_buffer, const DecodeArg& req) {
    constexpr u32 SamplesPerFrame{14};
    constexpr u32 NibblesPerFrame{16};

    if (req.buffer == 0 || req.buffer_size == 0) {
        return 0;
    }

    if (req.end_offset <= req.start_offset || req.offset >= req.end_offset - req.start_offset) {
        return 0;
    }

    const u32 start_pos = req.start_offset + req.offset;
    const u32 samples_to_process = std::min({req.end_offset - start_pos, req.samples_to_read,
                                           static_cast<u32>(out_buffer.size())});
    if (samples_to_process == 0) {
        return 0;
    }

    auto samples_to_read = samples_to_process;
    auto samples_remaining_in_frame = start_pos % SamplesPerFrame;
    u64 position_in_frame = u64{start_pos / SamplesPerFrame} * NibblesPerFrame +
                            samples_remaining_in_frame;
    if (samples_remaining_in_frame) {
        position_in_frame += 2;
    }

    const u32 last_sample = start_pos + samples_to_process - 1;
    const u64 end_byte = u64{last_sample / SamplesPerFrame} * (NibblesPerFrame / 2) +
                         (last_sample % SamplesPerFrame) / 2 + 2;
    if (end_byte > req.buffer_size ||
        req.buffer > (std::numeric_limits<CpuAddr>::max)() - end_byte) {
        return 0;
    }
    const u64 start_byte = position_in_frame / 2;
    Core::Memory::CpuGuestMemory<u8, Core::Memory::GuestMemoryFlags::UnsafeRead> wavebuffer(
        memory, req.buffer + start_byte, end_byte - start_byte);

    auto context = req.adpcm_context;
    auto header = context->header;
    u8 scale = u8(header & 0xfU);
    u8 coeff_index = u8((header >> 4U) & 0x7u);
    s32 coeff0 = req.coefficients[coeff_index * 2 + 0];
    s32 coeff1 = req.coefficients[coeff_index * 2 + 1];

    auto yn0 = context->yn0;
    auto yn1 = context->yn1;
    auto const get_step = [](u32 index) {
        // Emulates the following table
        // 0, 1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2, -1,
        constexpr u64 steps_table = 0x1234567876543210ull;
        constexpr u64 steps_sign = 0b1111111100000000ull;
        auto const r = s32((steps_table >> (index * 4)) & 0xf);
        return ((steps_sign >> index) & 1) == 0 ? -r : r;
    };
    auto const decode_sample = [&](const s32 code) -> s16 {
        const auto xn = code * (1 << scale);
        const s64 prediction = s64{coeff0} * yn0 + s64{coeff1} * yn1;
        const s64 sample = (s64{xn} * 0x800 + 0x400 + prediction) >> 11;
        const auto saturated = std::clamp<s64>(sample, -0x8000, 0x7FFF);
        yn1 = yn0;
        return yn0 = s16(saturated);
    };

    u32 read_index = 0;
    for (u32 write_index = 0; samples_to_read > 0 && write_index < out_buffer.size(); ) {
        // Are we at a new frame?
        if ((position_in_frame % NibblesPerFrame) == 0) {
            header = wavebuffer[read_index++];
            scale = header & 0xFu;
            coeff_index = (header >> 4) & 0x7u;
            coeff0 = req.coefficients[coeff_index * 2 + 0];
            coeff1 = req.coefficients[coeff_index * 2 + 1];
            position_in_frame += 2;

            // Can we consume all of this frame's samples?
            if (samples_to_read >= SamplesPerFrame) {
                // Can grab all samples until the next header
                for (u32 i = 0; i < SamplesPerFrame / 2; i++) {
                    auto code0 = get_step((wavebuffer[read_index + i] >> 4) & 0xF);
                    auto code1 = get_step(wavebuffer[read_index + i] & 0xF);
                    out_buffer[write_index++] = decode_sample(code0);
                    out_buffer[write_index++] = decode_sample(code1);
                }
                read_index += SamplesPerFrame / 2;
                position_in_frame += SamplesPerFrame;
                samples_to_read -= SamplesPerFrame;
                continue;
            }
        }

        // Decode a single sample
        auto code = wavebuffer[read_index];
        if (position_in_frame & 1) {
            code &= 0xF;
            read_index++;
        } else {
            code >>= 4;
        }
        out_buffer[write_index++] = decode_sample(get_step(code));

        position_in_frame++;
        samples_to_read--;
    }

    context->header = header;
    context->yn0 = yn0;
    context->yn1 = yn1;
    return samples_to_process;
}

/// @brief Decode implementation.
/// Decode wavebuffers according to the given args.
///
/// @param memory - Core memory to read data from.
/// @param args   - The wavebuffer data, and information for how to decode it.
void DecodeFromWaveBuffers(Core::Memory::Memory& memory, const DecodeFromWaveBuffersArgs& args) {
    constexpr auto EndWaveBuffer = [](auto& voice_state, auto& wavebuffer, auto& index, auto& played_samples, auto& consumed) -> void {
        voice_state.wave_buffer_valid[index] = false;
        voice_state.loop_count = 0;
        if (wavebuffer.stream_ended) {
            played_samples = 0;
        }
        index = (index + 1) % MaxWaveBuffers;
        consumed++;
    };
    const u32 samples_to_write = args.sample_count;
    if (samples_to_write == 0) {
        return;
    }
    constexpr std::array<u32, 3> history_sizes{4, 8, 4};
    const size_t quality = static_cast<size_t>(args.src_quality);
    if (samples_to_write > args.output.size() || quality >= history_sizes.size() ||
        args.target_sample_rate == 0 || args.channel_count <= 0 ||
        args.channel_count > static_cast<s8>(MaxChannels) || args.channel < 0 ||
        args.channel >= args.channel_count || args.wave_buffers.size() != MaxWaveBuffers) {
        LOG_ERROR(Service_Audio, "Invalid waveform decode parameters");
        return;
    }

    const f32 ratio = static_cast<f32>(args.source_sample_rate) /
                      static_cast<f32>(args.target_sample_rate) * args.pitch;
    if (!std::isfinite(ratio) || ratio <= 0.0f || ratio > TempBufferSize) {
        LOG_ERROR(Service_Audio, "Invalid waveform sample rate ratio {}", ratio);
        return;
    }
    using SamplePosition = Common::FixedPoint<49, 15>;
    const SamplePosition sample_rate_ratio{ratio};
    auto& voice_state = *args.voice_state;
    auto fraction = voice_state.fraction;
    if (sample_rate_ratio == 0 || fraction < 0 || fraction >= 1) {
        LOG_ERROR(Service_Audio, "Invalid waveform sample rate ratio or sample position");
        return;
    }
    const u32 history_size = history_sizes[quality];
    const auto size_required = SamplePosition::from_base(
        fraction.to_raw() + samples_to_write * sample_rate_ratio.to_raw());
    if (size_required >= TempBufferSize - history_size + 1) {
        LOG_ERROR(Service_Audio, "Waveform decode exceeds the temporary sample buffer");
        return;
    }
    const u32 samples_to_read = size_required.to_uint_floor();
    if (args.IsVoicePitchAndSrcSkippedSupported && samples_to_read > args.output.size()) {
        LOG_ERROR(Service_Audio, "Waveform decode exceeds the output sample buffer");
        return;
    }

    std::array<s16, 16> coefficients{};
    if (args.sample_format == SampleFormat::Adpcm &&
        (args.data_size > sizeof(coefficients) ||
         (args.data_size != 0 &&
          !memory.ReadBlockUnsafe(args.data_address, coefficients.data(), args.data_size)))) {
        LOG_ERROR(Service_Audio, "Invalid ADPCM coefficient data");
        return;
    }

    auto wavebuffers_consumed{voice_state.wave_buffers_consumed};
    auto wavebuffer_index{voice_state.wave_buffer_index};
    auto played_sample_count{voice_state.played_sample_count};

    u32 offset{voice_state.offset};

    const auto output_buffer = args.output;
    std::array<s16, TempBufferSize> temp_buffer{};

    u32 temp_buffer_pos{0};

    if (!args.IsVoicePitchAndSrcSkippedSupported) {
        for (u32 i = 0; i < history_size; i++) {
            temp_buffer[i] = voice_state.sample_history[i];
        }
        temp_buffer_pos = history_size;
    }

    u32 samples_read{0};
    while (samples_read < samples_to_read) {
        if (wavebuffer_index >= MaxWaveBuffers) {
            LOG_ERROR(Service_Audio, "Invalid wavebuffer index! {}", wavebuffer_index);
            wavebuffer_index = 0;
            wavebuffers_consumed = MaxWaveBuffers;
            voice_state.wave_buffer_valid.fill(false);
        }

        if (!voice_state.wave_buffer_valid[wavebuffer_index]) {
            break;
        }

        auto& wavebuffer = args.wave_buffers[wavebuffer_index];
        if (offset == 0 && args.sample_format == SampleFormat::Adpcm &&
            wavebuffer.context != 0 && wavebuffer.context_size != 0) {
            auto context = voice_state.adpcm_context;
            if (wavebuffer.context_size > sizeof(context) ||
                !memory.ReadBlockUnsafe(wavebuffer.context, &context, wavebuffer.context_size)) {
                LOG_ERROR(Service_Audio, "Invalid ADPCM wave buffer context");
                EndWaveBuffer(voice_state, wavebuffer, wavebuffer_index, played_sample_count,
                              wavebuffers_consumed);
                continue;
            }
            voice_state.adpcm_context = context;
        }

        auto start_offset{wavebuffer.start_offset};
        auto end_offset{wavebuffer.end_offset};

        if (wavebuffer.loop && voice_state.loop_count > 0 &&
            wavebuffer.loop_start_offset <= wavebuffer.loop_end_offset) {
            start_offset = wavebuffer.loop_start_offset;
            end_offset = wavebuffer.loop_end_offset;
        }

        DecodeArg decode_arg{
            .buffer{wavebuffer.buffer},
            .buffer_size{wavebuffer.buffer_size},
            .start_offset{start_offset},
            .end_offset{end_offset},
            .channel_count{args.channel_count},
            .coefficients{coefficients},
            .adpcm_context{nullptr},
            .target_channel{args.channel},
            .offset{offset},
            .samples_to_read{samples_to_read - samples_read},
        };

        u32 samples_decoded{0};

        switch (args.sample_format) {
        case SampleFormat::PcmInt16:
            samples_decoded = DecodePcm<s16>(
                memory, {temp_buffer.data() + temp_buffer_pos, TempBufferSize - temp_buffer_pos},
                decode_arg);
            break;

        case SampleFormat::PcmFloat:
            samples_decoded = DecodePcm<f32>(
                memory, {temp_buffer.data() + temp_buffer_pos, TempBufferSize - temp_buffer_pos},
                decode_arg);
            break;

        case SampleFormat::Adpcm: {
            decode_arg.adpcm_context = &voice_state.adpcm_context;
            samples_decoded = DecodeAdpcm(
                memory, {temp_buffer.data() + temp_buffer_pos, TempBufferSize - temp_buffer_pos},
                decode_arg);
        } break;

        default:
            LOG_ERROR(Service_Audio, "Invalid sample format to decode {}",
                      static_cast<u32>(args.sample_format));
            samples_decoded = 0;
            break;
        }

        played_sample_count += samples_decoded;
        samples_read += samples_decoded;
        temp_buffer_pos += samples_decoded;
        offset += samples_decoded;

        if (samples_decoded && offset < end_offset - start_offset) {
            continue;
        }

        offset = 0;
        if (wavebuffer.loop) {
            voice_state.loop_count++;
            if (wavebuffer.loop_count >= 0 &&
                (voice_state.loop_count > wavebuffer.loop_count || samples_decoded == 0)) {
                EndWaveBuffer(voice_state, wavebuffer, wavebuffer_index, played_sample_count,
                              wavebuffers_consumed);
            }

            if (samples_decoded == 0) {
                break;
            }

            if (args.IsVoicePlayedSampleCountResetAtLoopPointSupported) {
                played_sample_count = 0;
            }
        } else {
            EndWaveBuffer(voice_state, wavebuffer, wavebuffer_index, played_sample_count,
                          wavebuffers_consumed);
        }
    }

    if (args.IsVoicePitchAndSrcSkippedSupported) {
        for (u32 i = 0; i < samples_read; i++) {
            output_buffer[i] = temp_buffer[i];
        }
    } else {
        std::memset(temp_buffer.data() + temp_buffer_pos, 0,
                    (samples_to_read - samples_read) * sizeof(s16));

        Resample(output_buffer, temp_buffer, sample_rate_ratio, fraction, samples_to_write,
                 args.src_quality);

        std::memcpy(voice_state.sample_history.data(), temp_buffer.data() + samples_to_read,
                    history_size * sizeof(s16));
    }

    voice_state.wave_buffers_consumed = wavebuffers_consumed;
    voice_state.played_sample_count = played_sample_count;
    voice_state.wave_buffer_index = wavebuffer_index;
    voice_state.offset = offset;
    voice_state.fraction = fraction;
}

} // namespace AudioCore::Renderer
