// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>

#include "audio_core/opus/decoder.h"
#include "audio_core/opus/hardware_opus.h"
#include "audio_core/opus/parameters.h"
#include "common/alignment.h"
#include "common/scope_exit.h"
#include "common/swap.h"
#include "core/core.h"

namespace AudioCore::OpusDecoder {
using namespace Service::Audio;
namespace {
constexpr u64 MaxOpusWorkBufferSize = 16ULL * 1024 * 1024;

bool IsValidSampleRate(u32 sample_rate) {
    return sample_rate == 8'000 || sample_rate == 12'000 || sample_rate == 16'000 ||
           sample_rate == 24'000 || sample_rate == 48'000;
}

OpusPacketHeader ReverseHeader(OpusPacketHeader header) {
    OpusPacketHeader out;
    out.size = Common::swap32(header.size);
    out.final_range = Common::swap32(header.final_range);
    return out;
}
} // namespace

OpusDecoder::OpusDecoder(Core::System& system_, HardwareOpus& hardware_opus_)
    : system{system_}, hardware_opus{hardware_opus_} {}

OpusDecoder::~OpusDecoder() {
    if (decode_object_initialized) {
        if (is_multistream) {
            hardware_opus.ShutdownMultiStreamDecodeObject(shared_buffer.data(),
                                                          shared_buffer.size());
        } else {
            hardware_opus.ShutdownDecodeObject(shared_buffer.data(), shared_buffer.size());
        }
        hardware_opus.UnregisterDecoder(this);
    }
}

Result OpusDecoder::Initialize(const OpusParametersEx& params, Kernel::KTransferMemory* transfer_memory, u64 transfer_memory_size) {
    R_UNLESS(!decode_object_initialized, ResultLibOpusInvalidState);
    R_UNLESS(params.channel_count == 1 || params.channel_count == 2,
             ResultInvalidOpusChannelCount);
    R_UNLESS(IsValidSampleRate(params.sample_rate), ResultInvalidOpusSampleRate);
    R_UNLESS(transfer_memory != nullptr, ResultInvalidHandle);
    R_UNLESS(transfer_memory_size <= transfer_memory->GetSize(), ResultBufferTooSmall);

    const u64 frame_size{params.use_large_frame_size ? 5760ull : 1920ull};
    const u64 computed_buffer_size =
        Common::AlignUp((frame_size * params.channel_count) / (48'000ull / params.sample_rate),
                        16ull);
    constexpr size_t in_data_size{0x600u};
    const u64 required_workbuffer_size = hardware_opus.GetWorkBufferSize(params.channel_count);
    R_UNLESS(required_workbuffer_size > 0, ResultLibOpusInvalidState);
    R_UNLESS(transfer_memory_size >=
                     required_workbuffer_size + in_data_size + computed_buffer_size &&
                 transfer_memory_size <= MaxOpusWorkBufferSize,
             ResultBufferTooSmall);

    R_TRY(hardware_opus.RegisterDecoder(this));
    SCOPE_EXIT {
        if (!decode_object_initialized) {
            hardware_opus.UnregisterDecoder(this);
        }
    };

    shared_buffer.resize(transfer_memory_size);
    shared_memory_mapped = true;

    buffer_size = computed_buffer_size;
    out_data = {shared_buffer.data() + shared_buffer.size() - buffer_size, buffer_size};
    in_data = {out_data.data() - in_data_size, in_data_size};

    ON_RESULT_FAILURE {
        if (shared_memory_mapped) {
            shared_memory_mapped = false;
            ASSERT(R_SUCCEEDED(hardware_opus.UnmapMemory(shared_buffer.data(), shared_buffer.size())));
        }
    };

    R_TRY(hardware_opus.InitializeDecodeObject(params.sample_rate, params.channel_count,
                                               shared_buffer.data(), shared_buffer.size()));

    sample_rate = params.sample_rate;
    channel_count = params.channel_count;
    use_large_frame_size = params.use_large_frame_size;
    is_multistream = false;
    decode_object_initialized = true;
    R_SUCCEED();
}

Result OpusDecoder::Initialize(const OpusMultiStreamParametersEx& params, Kernel::KTransferMemory* transfer_memory, u64 transfer_memory_size) {
    R_UNLESS(!decode_object_initialized, ResultLibOpusInvalidState);
    R_UNLESS(params.channel_count > 0 && params.channel_count <= OpusStreamCountMax,
             ResultInvalidOpusChannelCount);
    R_UNLESS(IsValidSampleRate(params.sample_rate), ResultInvalidOpusSampleRate);
    R_UNLESS(params.total_stream_count > 0 && params.total_stream_count <= OpusStreamCountMax &&
                 params.stereo_stream_count <= params.total_stream_count &&
                 static_cast<u64>(params.total_stream_count) + params.stereo_stream_count <=
                     params.channel_count,
             ResultInvalidOpusSampleRate);
    R_UNLESS(transfer_memory != nullptr, ResultInvalidHandle);
    R_UNLESS(transfer_memory_size <= transfer_memory->GetSize(), ResultBufferTooSmall);

    const u64 frame_size{params.use_large_frame_size ? 5760ull : 1920ull};
    const u64 computed_buffer_size =
        Common::AlignUp((frame_size * params.channel_count) / (48'000ull / params.sample_rate),
                        16ull);
    const size_t in_data_size{Common::AlignUp(1500ull * params.total_stream_count, 64ull)};
    const u64 required_workbuffer_size = hardware_opus.GetWorkBufferSizeForMultiStream(
        params.total_stream_count, params.stereo_stream_count);
    R_UNLESS(required_workbuffer_size > 0, ResultLibOpusInvalidState);
    R_UNLESS(transfer_memory_size >=
                     required_workbuffer_size + in_data_size + computed_buffer_size &&
                 transfer_memory_size <= MaxOpusWorkBufferSize,
             ResultBufferTooSmall);

    R_TRY(hardware_opus.RegisterDecoder(this));
    SCOPE_EXIT {
        if (!decode_object_initialized) {
            hardware_opus.UnregisterDecoder(this);
        }
    };

    shared_buffer.resize(transfer_memory_size, 0);
    shared_memory_mapped = true;

    buffer_size = computed_buffer_size;
    out_data = {shared_buffer.data() + shared_buffer.size() - buffer_size, buffer_size};
    in_data = {out_data.data() - in_data_size, in_data_size};

    ON_RESULT_FAILURE {
        if (shared_memory_mapped) {
            shared_memory_mapped = false;
            ASSERT(R_SUCCEEDED(hardware_opus.UnmapMemory(shared_buffer.data(), shared_buffer.size())));
        }
    };

    R_TRY(hardware_opus.InitializeMultiStreamDecodeObject(
        params.sample_rate, params.channel_count, params.total_stream_count,
        params.stereo_stream_count, params.mappings.data(), shared_buffer.data(),
        shared_buffer.size()));

    sample_rate = params.sample_rate;
    channel_count = params.channel_count;
    total_stream_count = params.total_stream_count;
    stereo_stream_count = params.stereo_stream_count;
    use_large_frame_size = params.use_large_frame_size;
    is_multistream = true;
    decode_object_initialized = true;
    R_SUCCEED();
}

Result OpusDecoder::DecodeInterleaved(u32* out_data_size, u64* out_time_taken,
                                      u32* out_sample_count, std::span<const u8> input_data,
                                      std::span<u8> output_data, bool reset) {
    R_UNLESS(decode_object_initialized, ResultLibOpusInvalidState);
    u32 out_samples;
    u64 time_taken{};

    R_UNLESS(input_data.size_bytes() > sizeof(OpusPacketHeader), ResultInputDataTooSmall);

    OpusPacketHeader header;
    std::memcpy(&header, input_data.data(), sizeof(header));
    header = ReverseHeader(header);

    R_UNLESS(in_data.size_bytes() >= header.size &&
                 header.size + sizeof(OpusPacketHeader) <= input_data.size_bytes(),
             ResultBufferTooSmall);

    if (!shared_memory_mapped) {
        R_TRY(hardware_opus.MapMemory(shared_buffer.data(), shared_buffer.size()));
        shared_memory_mapped = true;
    }

    std::memcpy(in_data.data(), input_data.data() + sizeof(OpusPacketHeader), header.size);

    const size_t output_capacity = std::min(out_data.size_bytes(), output_data.size_bytes());
    R_UNLESS(channel_count > 0, ResultLibOpusInvalidState);
    const size_t bytes_per_frame = static_cast<size_t>(channel_count) * sizeof(s16);
    R_UNLESS(output_capacity >= bytes_per_frame, ResultBufferTooSmall);
    R_TRY(hardware_opus.DecodeInterleaved(out_samples, out_data.data(), output_capacity,
                                          channel_count, in_data.data(), header.size,
                                          shared_buffer.data(), time_taken, reset));

    R_UNLESS(out_samples <= output_capacity / bytes_per_frame, ResultBufferTooSmall);
    std::memcpy(output_data.data(), out_data.data(), out_samples * bytes_per_frame);

    *out_data_size = header.size + sizeof(OpusPacketHeader);
    *out_sample_count = out_samples;
    if (out_time_taken) {
        *out_time_taken = time_taken / 1000;
    }
    R_SUCCEED();
}

Result OpusDecoder::SetContext([[maybe_unused]] std::span<const u8> context) {
    R_UNLESS(decode_object_initialized, ResultLibOpusInvalidState);
    R_SUCCEED_IF(shared_memory_mapped);
    shared_memory_mapped = true;
    R_RETURN(hardware_opus.MapMemory(shared_buffer.data(), shared_buffer.size()));
}

Result OpusDecoder::DecodeInterleavedForMultiStream(u32* out_data_size, u64* out_time_taken,
                                                    u32* out_sample_count,
                                                    std::span<const u8> input_data,
                                                    std::span<u8> output_data, bool reset) {
    R_UNLESS(decode_object_initialized, ResultLibOpusInvalidState);
    u32 out_samples;
    u64 time_taken{};

    R_UNLESS(input_data.size_bytes() > sizeof(OpusPacketHeader), ResultInputDataTooSmall);

    OpusPacketHeader header;
    std::memcpy(&header, input_data.data(), sizeof(header));
    header = ReverseHeader(header);

    LOG_TRACE(Service_Audio, "header size {:#x} input data size {:#x} in_data size {:#x}",
              header.size, input_data.size_bytes(), in_data.size_bytes());

    R_UNLESS(in_data.size_bytes() >= header.size &&
                 header.size + sizeof(OpusPacketHeader) <= input_data.size_bytes(),
             ResultBufferTooSmall);

    if (!shared_memory_mapped) {
        R_TRY(hardware_opus.MapMemory(shared_buffer.data(), shared_buffer.size()));
        shared_memory_mapped = true;
    }

    std::memcpy(in_data.data(), input_data.data() + sizeof(OpusPacketHeader), header.size);

    const size_t output_capacity = std::min(out_data.size_bytes(), output_data.size_bytes());
    R_UNLESS(channel_count > 0, ResultLibOpusInvalidState);
    const size_t bytes_per_frame = static_cast<size_t>(channel_count) * sizeof(s16);
    R_UNLESS(output_capacity >= bytes_per_frame, ResultBufferTooSmall);
    R_TRY(hardware_opus.DecodeInterleavedForMultiStream(
        out_samples, out_data.data(), output_capacity, channel_count, in_data.data(),
        header.size, shared_buffer.data(), time_taken, reset));

    R_UNLESS(out_samples <= output_capacity / bytes_per_frame, ResultBufferTooSmall);
    std::memcpy(output_data.data(), out_data.data(), out_samples * bytes_per_frame);

    *out_data_size = header.size + sizeof(OpusPacketHeader);
    *out_sample_count = out_samples;
    if (out_time_taken) {
        *out_time_taken = time_taken / 1000;
    }
    R_SUCCEED();
}

} // namespace AudioCore::OpusDecoder
