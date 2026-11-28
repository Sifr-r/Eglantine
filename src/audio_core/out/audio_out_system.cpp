// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>
#include <mutex>

#include "audio_core/audio_event.h"
#include "audio_core/audio_manager.h"
#include "audio_core/out/audio_out_system.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_event.h"

// See texture_cache/util.h
template<typename T, size_t N>
#if BOOST_VERSION >= 108100 || __GNUC__ > 12
[[nodiscard]] boost::container::static_vector<T, N> FixStaticVectorADL(const boost::container::static_vector<T, N>& v) {
    return v;
}
#else
[[nodiscard]] std::vector<T> FixStaticVectorADL(const boost::container::static_vector<T, N>& v) {
    std::vector<T> u;
    for (auto const& e : v)
        u.push_back(e);
    return u;
}
#endif

namespace AudioCore::AudioOut {

namespace {

class RealDeviceSession final : public DeviceSessionInterface {
public:
    explicit RealDeviceSession(Core::System& system_)
        : system{system_}, device_session{std::make_unique<DeviceSession>(system_)} {}

    Result Initialize(std::string_view name, SampleFormat sample_format, u16 channel_count,
                      size_t session_id, Kernel::KProcess* handle, u64 applet_resource_user_id,
                      Sink::StreamType type) override {
        device_session = std::make_unique<DeviceSession>(system);
        return device_session->Initialize(name, sample_format, channel_count, session_id, handle,
                                          applet_resource_user_id, type);
    }

    void Finalize() override {
        device_session.reset();
    }

    void AppendBuffers(std::span<const AudioBuffer> buffers) override {
        if (device_session) {
            device_session->AppendBuffers(buffers);
        }
    }

    void ReleaseBuffer(const AudioBuffer& buffer) const override {
        if (device_session) {
            device_session->ReleaseBuffer(buffer);
        }
    }

    bool IsBufferConsumed(const AudioBuffer& buffer) const override {
        return device_session && device_session->IsBufferConsumed(buffer);
    }

    void Start() override {
        if (device_session) {
            device_session->Start();
        }
    }

    void Stop() override {
        if (device_session) {
            device_session->Stop();
        }
    }

    void ClearBuffers() override {
        if (device_session) {
            device_session->ClearBuffers();
        }
    }

    void SetVolume(f32 volume) const override {
        if (device_session) {
            device_session->SetVolume(volume);
        }
    }

    u64 GetPlayedSampleCount() const override {
        return device_session ? device_session->GetPlayedSampleCount() : 0;
    }

    void SetRingSize(u32 ring_size) override {
        if (device_session) {
            device_session->SetRingSize(ring_size);
        }
    }

private:
    Core::System& system;
    std::unique_ptr<DeviceSession> device_session;
};

} // namespace

System::System(Core::System& system_, Kernel::KEvent* event_, size_t session_id_)
    : System{system_, event_, session_id_, std::make_unique<RealDeviceSession>(system_)} {}

System::System(Core::System& system_, Kernel::KEvent* event_, size_t session_id_,
               std::unique_ptr<DeviceSessionInterface> session_,
               std::function<void()> signal_event_callback_)
    : system{system_}, buffer_event{event_},
      signal_event_callback{std::move(signal_event_callback_)}, session_id{session_id_},
      session{std::move(session_)} {}

System::~System() {
    Finalize();
}

void System::SignalBufferEvent() {
    if (signal_event_callback) {
        signal_event_callback();
    }
    if (buffer_event) {
        buffer_event->Signal(system.Kernel());
    }
}

void System::Finalize() {
    if (finalized) {
        return;
    }
    finalized = true;

    if (state == State::Started) {
        session->Stop();
        session->SetVolume(0.0f);
        session->ClearBuffers();
        state = State::Stopped;
    }

    u32 buffers_released{};
    buffers.FlushBuffers(buffers_released);
    session->Finalize();
}

std::string_view System::GetDefaultOutputDeviceName() const {
    return "DeviceOut";
}

Result System::IsConfigValid(std::string_view device_name,
                             const AudioOutParameter& in_params) const {
    if ((device_name.size() > 0) && (device_name != GetDefaultOutputDeviceName())) {
        return Service::Audio::ResultNotFound;
    }

    if (in_params.sample_rate != TargetSampleRate && in_params.sample_rate > 0) {
        return Service::Audio::ResultInvalidSampleRate;
    }

    if (in_params.channel_count == 0 || in_params.channel_count == 2 ||
        in_params.channel_count == 6) {
        return ResultSuccess;
    }

    return Service::Audio::ResultInvalidChannelCount;
}

Result System::Initialize(std::string device_name, const AudioOutParameter& in_params,
                          Kernel::KProcess* handle_, u64 applet_resource_user_id_) {
    auto result = IsConfigValid(device_name, in_params);
    if (result.IsError()) {
        return result;
    }

    handle = handle_;
    applet_resource_user_id = applet_resource_user_id_;
    if (device_name.empty() || device_name[0] == '\0') {
        name = std::string(GetDefaultOutputDeviceName());
    } else {
        name = std::move(device_name);
    }

    sample_rate = TargetSampleRate;
    sample_format = SampleFormat::PcmInt16;
    channel_count = in_params.channel_count <= 2 ? 2 : 6;
    volume = 1.0f;
    return ResultSuccess;
}

void System::StartSession() {
    session->Start();
}

size_t System::GetSessionId() const {
    return session_id;
}

Result System::Start() {
    if (state != State::Stopped) {
        return Service::Audio::ResultOperationFailed;
    }

    session->Initialize(name, sample_format, channel_count, session_id, handle,
                        applet_resource_user_id, Sink::StreamType::Out);
    session->SetVolume(volume);
    session->Start();
    state = State::Started;

    const u64 frame_bytes = static_cast<u64>(channel_count) * sizeof(s16);
    buffers.RebaseTimestamps(frame_bytes);

    boost::container::static_vector<AudioBuffer, BufferCount> buffers_to_flush{};
    buffers.RegisterBuffers(buffers_to_flush);
    session->AppendBuffers(FixStaticVectorADL(buffers_to_flush));
    session->SetRingSize(static_cast<u32>(buffers_to_flush.size()));

    return ResultSuccess;
}

Result System::Stop() {
    if (state == State::Started) {
        session->Stop();
        session->SetVolume(0.0f);
        session->ClearBuffers();
        if (buffers.ReleaseConsumedBuffers(system.CoreTiming(), *session)) {
            SignalBufferEvent();
        }
        buffers.UnregisterBuffers();
        state = State::Stopped;
    }

    return ResultSuccess;
}

Result System::AppendBuffer(const AudioOutBuffer& buffer, u64 tag) {
    if (channel_count == 0) {
        return Service::Audio::ResultOperationFailed;
    }

    const u64 frame_bytes = static_cast<u64>(channel_count) * sizeof(s16);
    if (buffer.size > MaxDeviceBufferSize || buffer.size % frame_bytes != 0) {
        return Service::Audio::ResultInvalidArgument;
    }

    if (buffer.offset > (std::numeric_limits<u64>::max)() - buffer.size) {
        return Service::Audio::ResultInvalidArgument;
    }
    const u64 end_offset = buffer.offset + buffer.size;
    if (buffer.capacity > 0 && end_offset > buffer.capacity) {
        return Service::Audio::ResultInvalidArgument;
    }

    if (buffer.samples > (std::numeric_limits<VAddr>::max)() - buffer.offset) {
        return Service::Audio::ResultInvalidArgument;
    }
    const VAddr sample_addr = buffer.samples + buffer.offset;
    if (sample_addr > (std::numeric_limits<VAddr>::max)() - buffer.size) {
        return Service::Audio::ResultInvalidArgument;
    }

    if (buffers.GetTotalBufferCount() >= BufferCount) {
        return Service::Audio::ResultBufferCountReached;
    }

    const auto timestamp{buffers.GetNextTimestamp()};
    const AudioBuffer new_buffer{
        .start_timestamp = timestamp,
        .end_timestamp = timestamp + buffer.size / frame_bytes,
        .played_timestamp = 0,
        .samples = sample_addr,
        .tag = tag,
        .size = buffer.size,
    };

    buffers.AppendBuffer(new_buffer);
    RegisterBuffers();

    return ResultSuccess;
}

void System::RegisterBuffers() {
    if (state == State::Started) {
        boost::container::static_vector<AudioBuffer, BufferCount> registered_buffers{};
        buffers.RegisterBuffers(registered_buffers);
        session->AppendBuffers(FixStaticVectorADL(registered_buffers));
    }
}

void System::ReleaseBuffers() {
    bool signal{buffers.ReleaseBuffers(system.CoreTiming(), *session, false)};
    if (signal) {
        // Signal if any buffer was released, or if none are registered, we need more.
        SignalBufferEvent();
    }
}

u32 System::GetReleasedBuffers(std::span<u64> tags) {
    return buffers.GetReleasedBuffers(tags);
}

bool System::FlushAudioOutBuffers() {
    if (state != State::Stopped) {
        return false;
    }

    u32 buffers_released{};
    buffers.FlushBuffers(buffers_released);

    if (buffers_released > 0) {
        SignalBufferEvent();
    }
    return true;
}

u16 System::GetChannelCount() const {
    return channel_count;
}

u32 System::GetSampleRate() const {
    return sample_rate;
}

SampleFormat System::GetSampleFormat() const {
    return sample_format;
}

State System::GetState() {
    switch (state) {
    case State::Started:
    case State::Stopped:
        return state;
    default:
        LOG_ERROR(Service_Audio, "AudioOut invalid state!");
        state = State::Stopped;
        break;
    }
    return state;
}

std::string System::GetName() const {
    return name;
}

f32 System::GetVolume() const {
    return volume;
}

void System::SetVolume(const f32 volume_) {
    volume = volume_;
    session->SetVolume(volume_);
}

bool System::ContainsAudioBuffer(const u64 tag) const {
    return buffers.ContainsBuffer(tag);
}

u32 System::GetBufferCount() const {
    return buffers.GetAppendedRegisteredCount();
}

u64 System::GetPlayedSampleCount() const {
    return session->GetPlayedSampleCount();
}

} // namespace AudioCore::AudioOut
