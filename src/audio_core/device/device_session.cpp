// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "audio_core/audio_core.h"
#include "audio_core/audio_manager.h"
#include "audio_core/device/audio_buffer.h"
#include "audio_core/device/device_session.h"
#include "audio_core/sink/sink_stream.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/guest_memory.h"
#include "core/memory.h"

#include "core/hle/kernel/k_process.h"

namespace AudioCore {

using namespace std::literals;
constexpr auto INCREMENT_TIME{5ms};

DeviceSession::DeviceSession(Core::System& system_)
    : system{system_}, thread_event{Core::Timing::CreateEvent(
                           "AudioOutSampleTick",
                           [this](s64 time, std::chrono::nanoseconds) { return ThreadFunc(); })} {}

DeviceSession::~DeviceSession() {
    Finalize();
}

Result DeviceSession::Initialize(std::string_view name_, SampleFormat sample_format_,
                                 u16 channel_count_, size_t session_id_, Kernel::KProcess* handle_,
                                 u64 applet_resource_user_id_, Sink::StreamType type_) {
    if (stream) {
        Finalize();
    }
    name = fmt::format("{}-{}", name_, session_id_);
    type = type_;
    sample_format = sample_format_;
    channel_count = channel_count_;
    session_id = session_id_;
    handle = handle_;
    handle->Open(system.Kernel());
    applet_resource_user_id = applet_resource_user_id_;

    if (type == Sink::StreamType::In) {
        sink = &system.AudioCore().GetInputSink();
    } else {
        sink = &system.AudioCore().GetOutputSink();
    }
    stream = sink->AcquireSinkStream(system, channel_count, name, type);
    initialized = true;
    return ResultSuccess;
}

void DeviceSession::Finalize() {
    if (initialized) {
        Stop();
        sink->CloseStream(stream);
        stream = nullptr;
    }

    if (handle) {
        handle->Close(system.Kernel());
        handle = nullptr;
    }
}

void DeviceSession::Start() {
    if (stream) {
        stream->Start();
        system.CoreTiming().ScheduleLoopingEvent(std::chrono::nanoseconds::zero(), INCREMENT_TIME,
                                                 thread_event);
    }
}

void DeviceSession::Stop() {
    if (stream) {
        stream->Stop();
        system.CoreTiming().UnscheduleEvent(thread_event, {});
    }
}

void DeviceSession::ClearBuffers() {
    if (stream) {
        stream->ClearQueue();
    }
}

void DeviceSession::AppendBuffers(std::span<const AudioBuffer> buffers) {
    if (!stream || !handle || channel_count == 0) {
        return;
    }

    const u64 frame_bytes = static_cast<u64>(channel_count) * sizeof(s16);
    for (const auto& buffer : buffers) {
        if (buffer.size > MaxDeviceBufferSize) {
            continue;
        }
        const u64 frames = buffer.size / frame_bytes;
        const u64 sample_count = frames * channel_count;
        Sink::SinkBuffer new_buffer{
            .frames = frames,
            .frames_played = 0,
            .tag = buffer.tag,
            .consumed = false,
        };

        tmp_samples.resize_destructive(sample_count);
        if (type == Sink::StreamType::In) {
            stream->AppendBuffer(new_buffer, tmp_samples);
        } else {
            Core::Memory::CpuGuestMemory<s16, Core::Memory::GuestMemoryFlags::UnsafeRead> samples(
                handle->GetMemory(), buffer.samples, sample_count);
            stream->AppendBuffer(new_buffer, samples);
        }
    }
}

void DeviceSession::ReleaseBuffer(const AudioBuffer& buffer) const {
    if (type == Sink::StreamType::In && stream && handle && channel_count != 0 &&
        buffer.size <= MaxDeviceBufferSize) {
        const u64 frame_bytes = static_cast<u64>(channel_count) * sizeof(s16);
        const u64 sample_count = (buffer.size / frame_bytes) * channel_count;
        auto samples{stream->ReleaseBuffer(sample_count)};
        const u64 bytes_to_write =
            (std::min<u64>)(sample_count * sizeof(s16), samples.size() * sizeof(s16));
        if (bytes_to_write > 0) {
            handle->GetMemory().WriteBlockUnsafe(buffer.samples, samples.data(), bytes_to_write);
        }
    }
}

bool DeviceSession::IsBufferConsumed(const AudioBuffer& buffer) const {
    return played_sample_count >= buffer.end_timestamp;
}

void DeviceSession::SetVolume(f32 volume) const {
    if (stream) {
        stream->SetSystemVolume(volume);
    }
}

u64 DeviceSession::GetPlayedSampleCount() const {
    return played_sample_count;
}

std::optional<std::chrono::nanoseconds> DeviceSession::ThreadFunc() {
    played_sample_count = stream->GetExpectedPlayedSampleCount();
    if (type == Sink::StreamType::Out) {
        system.AudioCore().GetAudioManager().SetEvent(Event::Type::AudioOutManager, true);
    } else {
        system.AudioCore().GetAudioManager().SetEvent(Event::Type::AudioInManager, true);
    }
    return std::nullopt;
}

void DeviceSession::SetRingSize(u32 ring_size) {
    stream->SetRingSize(ring_size);
}

} // namespace AudioCore
