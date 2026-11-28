// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "audio_core/common/common.h"
#include "audio_core/out/audio_out_system.h"
#include "audio_core/sink/sink.h"
#include "audio_core/sink/sink_stream.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/service/audio/errors.h"

namespace {

using namespace std::chrono_literals;

struct ScopedAudioSettings {
    bool muted = Settings::values.audio_muted.GetValue();
    u8 volume = Settings::values.volume.GetValue();

    ScopedAudioSettings() {
        Settings::values.audio_muted.SetValue(false);
        Settings::values.volume.SetValue(100);
    }

    ~ScopedAudioSettings() {
        Settings::values.audio_muted.SetValue(muted);
        Settings::values.volume.SetValue(volume);
    }
};

class TestSilentSinkStream final : public AudioCore::Sink::SinkStream {
public:
    explicit TestSilentSinkStream(Core::System& system_, AudioCore::Sink::StreamType type_,
                                  u32 device_channels_ = 2, u32 system_channels_ = 2)
        : SinkStream{system_, type_} {
        device_channels = device_channels_;
        system_channels = system_channels_;
    }

    ~TestSilentSinkStream() override {
        if (stop_on_destroy) {
            Finalize();
        }
    }

    void Finalize() override {
        SignalPause();
        StopSilentDevice();
    }

    void Start(bool = false) override {
        paused = false;
    }

    void Stop() override {
        SignalPause();
    }

    void SetStopOnDestroy(bool value) {
        stop_on_destroy = value;
    }

    using SinkStream::SignalPause;
    using SinkStream::StartSilentDevice;
    using SinkStream::StopSilentDevice;

private:
    bool stop_on_destroy{true};
};

template <typename Predicate>
bool WaitFor(Predicate condition, std::chrono::milliseconds limit = 2000ms) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    return condition();
}

void AppendTestBuffer(AudioCore::Sink::SinkStream& stream, u64 tag, u32 channels, s16 fill_value) {
    AudioCore::Sink::SinkBuffer buffer{
        .frames = AudioCore::TargetSampleCount,
        .frames_played = 0,
        .tag = tag,
        .consumed = false,
    };
    std::vector<s16> samples(static_cast<std::size_t>(AudioCore::TargetSampleCount) * channels,
                             fill_value);
    stream.AppendBuffer(buffer, samples);
}

} // Anonymous namespace

TEST_CASE("SinkStream silent device drains queued output buffers while unpaused",
          "[audio_core][audio_recovery]") {
    const ScopedAudioSettings audio_settings;
    auto system = std::make_unique<Core::System>();
    auto stream =
        std::make_unique<TestSilentSinkStream>(*system, AudioCore::Sink::StreamType::Out, 2, 2);

    stream->SetRingSize(4);
    stream->StartSilentDevice();

    AppendTestBuffer(*stream, 1, 2, 1000);
    AppendTestBuffer(*stream, 2, 2, 2000);
    REQUIRE(stream->GetQueueSize() == 2U);

    // Paused stream must retain queued buffers until Start() unpauses consumption.
    std::this_thread::sleep_for(30ms);
    CHECK(stream->IsPaused());
    CHECK(stream->GetQueueSize() == 2U);

    stream->Start();
    CHECK_FALSE(stream->IsPaused());
    REQUIRE(WaitFor([&] { return stream->GetQueueSize() == 0U; }));
    REQUIRE(WaitFor([&] {
        return stream->GetExpectedPlayedSampleCount() > u64{AudioCore::TargetSampleCount} * 5U;
    }));

    SECTION("downmixes 6-channel input and drains") {
        auto surround_stream = std::make_unique<TestSilentSinkStream>(
            *system, AudioCore::Sink::StreamType::Render, 2, 6);
        surround_stream->SetRingSize(4);
        surround_stream->StartSilentDevice();
        AppendTestBuffer(*surround_stream, 10, 6, 512);
        AppendTestBuffer(*surround_stream, 11, 6, 512);
        surround_stream->Start();
        REQUIRE(WaitFor([&] { return surround_stream->GetQueueSize() == 0U; }));
    }

    SECTION("upmixes mono input and drains") {
        auto mono_stream =
            std::make_unique<TestSilentSinkStream>(*system, AudioCore::Sink::StreamType::Out, 2, 1);
        mono_stream->SetRingSize(4);
        mono_stream->StartSilentDevice();
        AppendTestBuffer(*mono_stream, 20, 1, 256);
        mono_stream->Start();
        REQUIRE(WaitFor([&] { return mono_stream->GetQueueSize() == 0U; }));
    }
}

TEST_CASE("SinkStream silent device produces zeroed input frames for StreamType::In",
          "[audio_core][audio_recovery]") {
    const ScopedAudioSettings audio_settings;
    auto system = std::make_unique<Core::System>();
    auto stream =
        std::make_unique<TestSilentSinkStream>(*system, AudioCore::Sink::StreamType::In, 2, 2);

    stream->StartSilentDevice();
    stream->Start();

    REQUIRE(WaitFor([&] {
        return stream->GetExpectedPlayedSampleCount() > u64{AudioCore::TargetSampleCount} * 5U;
    }));

    const std::size_t expected_samples = static_cast<std::size_t>(AudioCore::TargetSampleCount) * 2U;
    const auto samples = stream->ReleaseBuffer(expected_samples);
    REQUIRE(samples.size() == expected_samples);
    CHECK(std::all_of(samples.begin(), samples.end(), [](s16 sample) { return sample == 0; }));

    stream->Stop();
    CHECK(stream->IsPaused());
    stream->StopSilentDevice();
}

TEST_CASE("SinkStream SignalPause unblocks WaitFreeSpace", "[audio_core][audio_recovery]") {
    const ScopedAudioSettings audio_settings;
    auto system = std::make_unique<Core::System>();
    auto stream =
        std::make_unique<TestSilentSinkStream>(*system, AudioCore::Sink::StreamType::Out, 2, 2);

    stream->SetRingSize(2);
    stream->Start();

    // Exceed max_queue_size + 3 so WaitFreeSpace enters the indefinite wait.
    for (u64 tag = 1; tag <= 6; ++tag) {
        AppendTestBuffer(*stream, tag, 2, 128);
    }
    REQUIRE(stream->GetQueueSize() == 6U);

    SECTION("SignalPause wakes waiter") {
        std::stop_source stop_source;
        std::atomic<bool> returned{false};
        std::jthread waiter([&] {
            stream->WaitFreeSpace(stop_source.get_token());
            returned = true;
        });

        std::this_thread::sleep_for(25ms);
        CHECK_FALSE(returned.load());

        stream->SignalPause();
        CHECK(stream->IsPaused());
        REQUIRE(WaitFor([&] { return returned.load(); }));
    }

    SECTION("stop_token cancels waiter") {
        std::stop_source stop_source;
        std::atomic<bool> returned{false};
        std::jthread waiter([&] {
            stream->WaitFreeSpace(stop_source.get_token());
            returned = true;
        });

        std::this_thread::sleep_for(25ms);
        CHECK_FALSE(returned.load());

        stop_source.request_stop();
        REQUIRE(WaitFor([&] { return returned.load(); }));
    }
}

TEST_CASE("SinkStream silent device tolerates repeated start, stop, and destruction while active",
          "[audio_core][audio_recovery]") {
    const ScopedAudioSettings audio_settings;
    auto system = std::make_unique<Core::System>();

    SECTION("repeated StartSilentDevice and StopSilentDevice calls are safe") {
        auto stream =
            std::make_unique<TestSilentSinkStream>(*system, AudioCore::Sink::StreamType::Out, 2, 2);
        stream->StopSilentDevice();
        stream->StopSilentDevice();

        stream->StartSilentDevice();
        stream->StartSilentDevice();
        stream->Start();
        AppendTestBuffer(*stream, 1, 2, 64);
        REQUIRE(WaitFor([&] { return stream->GetQueueSize() == 0U; }));

        stream->SignalPause();
        stream->StopSilentDevice();
        stream->StopSilentDevice();

        // Can cleanly restart after stopping.
        stream->StartSilentDevice();
        stream->Start();
        AppendTestBuffer(*stream, 2, 2, 64);
        REQUIRE(WaitFor([&] { return stream->GetQueueSize() == 0U; }));

        stream->Finalize();
        stream->Finalize();
        CHECK(stream->IsPaused());
    }

    SECTION("destroying unpaused stream without explicit StopSilentDevice joins cleanly") {
        auto stream =
            std::make_unique<TestSilentSinkStream>(*system, AudioCore::Sink::StreamType::Out, 2, 2);
        stream->SetStopOnDestroy(false);
        stream->SetRingSize(4);
        stream->StartSilentDevice();
        stream->Start();
        AppendTestBuffer(*stream, 1, 2, 32);
        REQUIRE(WaitFor([&] { return stream->GetQueueSize() == 0U; }));
        stream.reset();
    }
}

namespace {

class MockAudioOutSession final : public AudioCore::AudioOut::DeviceSessionInterface {
public:
    Result Initialize(std::string_view, AudioCore::SampleFormat, u16, size_t, Kernel::KProcess*,
                      u64, AudioCore::Sink::StreamType) override {
        ++initialize_count;
        played_sample_count = 0;
        submitted_buffers.clear();
        return ResultSuccess;
    }

    void Finalize() override {
        ++finalize_count;
    }

    void AppendBuffers(std::span<const AudioCore::AudioBuffer> buffers) override {
        for (const auto& buffer : buffers) {
            submitted_buffers.push_back(buffer);
        }
    }

    void ReleaseBuffer(const AudioCore::AudioBuffer&) const override {}

    bool IsBufferConsumed(const AudioCore::AudioBuffer& buffer) const override {
        return played_sample_count >= buffer.end_timestamp;
    }

    void Start() override {
        ++start_count;
        started = true;
    }

    void Stop() override {
        ++stop_count;
        started = false;
    }

    void ClearBuffers() override {
        ++clear_count;
        submitted_buffers.clear();
    }

    void SetVolume(f32 volume_) const override {
        last_volume = volume_;
    }

    u64 GetPlayedSampleCount() const override {
        return played_sample_count;
    }

    void SetRingSize(u32 ring_size_) override {
        last_ring_size = ring_size_;
    }

    u64 played_sample_count{};
    u32 initialize_count{};
    u32 finalize_count{};
    u32 start_count{};
    u32 stop_count{};
    u32 clear_count{};
    u32 last_ring_size{};
    mutable f32 last_volume{1.0f};
    bool started{};
    std::vector<AudioCore::AudioBuffer> submitted_buffers{};
};

struct AudioOutTestFixture {
    std::unique_ptr<Core::System> core_system;
    MockAudioOutSession* session{};
    u32 signal_count{};
    std::unique_ptr<AudioCore::AudioOut::System> audio_out;

    explicit AudioOutTestFixture(bool initialize = true)
        : core_system{std::make_unique<Core::System>()} {
        auto mock = std::make_unique<MockAudioOutSession>();
        session = mock.get();
        audio_out = std::make_unique<AudioCore::AudioOut::System>(
            *core_system, nullptr, 0, std::move(mock), [this] { ++signal_count; });
        if (initialize) {
            const AudioCore::AudioOut::AudioOutParameter params{
                .sample_rate = static_cast<s32>(AudioCore::TargetSampleRate),
                .channel_count = 2,
                .reserved = 0,
            };
            REQUIRE(audio_out->Initialize("DeviceOut", params, nullptr, 0) == ResultSuccess);
        }
    }
};

AudioCore::AudioOut::AudioOutBuffer MakeAudioOutBuffer(VAddr samples, u64 capacity, u64 size,
                                                       u64 offset = 0) {
    return AudioCore::AudioOut::AudioOutBuffer{
        .next = nullptr,
        .samples = samples,
        .capacity = capacity,
        .size = size,
        .offset = offset,
    };
}

} // Anonymous namespace

TEST_CASE("AudioOut preserves unplayed buffers across stop and restart without re-append",
          "[audio_core][audio_out]") {
    AudioOutTestFixture fixture;
    constexpr u64 frame_bytes = 2 * sizeof(s16);
    constexpr u64 long_frames = 48000;
    constexpr u64 short_frames = 12000;

    SECTION("buffers appended before start play in order after start") {
        REQUIRE(fixture.audio_out->AppendBuffer(
                    MakeAudioOutBuffer(0x10000, long_frames * frame_bytes, long_frames * frame_bytes),
                    0x1111) == ResultSuccess);
        REQUIRE(fixture.audio_out->AppendBuffer(
                    MakeAudioOutBuffer(0x20000, short_frames * frame_bytes,
                                       short_frames * frame_bytes),
                    0x2222) == ResultSuccess);

        CHECK(fixture.audio_out->GetState() == AudioCore::AudioOut::State::Stopped);
        CHECK(fixture.audio_out->GetBufferCount() == 2U);
        CHECK(fixture.audio_out->ContainsAudioBuffer(0x1111));
        CHECK(fixture.audio_out->ContainsAudioBuffer(0x2222));
        CHECK(fixture.session->submitted_buffers.empty());

        REQUIRE(fixture.audio_out->Start() == ResultSuccess);
        CHECK(fixture.audio_out->GetState() == AudioCore::AudioOut::State::Started);
        REQUIRE(fixture.session->submitted_buffers.size() == 2U);
        CHECK(fixture.session->submitted_buffers[0].tag == 0x1111);
        CHECK(fixture.session->submitted_buffers[0].start_timestamp == 0U);
        CHECK(fixture.session->submitted_buffers[0].end_timestamp == long_frames);
        CHECK(fixture.session->submitted_buffers[1].tag == 0x2222);
        CHECK(fixture.session->submitted_buffers[1].start_timestamp == long_frames);
        CHECK(fixture.session->submitted_buffers[1].end_timestamp == long_frames + short_frames);

        fixture.session->played_sample_count = long_frames;
        fixture.audio_out->ReleaseBuffers();
        CHECK(fixture.signal_count == 1U);

        std::array<u64, 4> released{};
        REQUIRE(fixture.audio_out->GetReleasedBuffers(released) == 1U);
        CHECK(released[0] == 0x1111);
        CHECK(fixture.audio_out->GetBufferCount() == 1U);

        fixture.session->played_sample_count = long_frames + short_frames;
        fixture.audio_out->ReleaseBuffers();
        REQUIRE(fixture.audio_out->GetReleasedBuffers(released) == 1U);
        CHECK(released[0] == 0x2222);
        CHECK(fixture.audio_out->GetBufferCount() == 0U);
    }

    SECTION("stop before completion retains unplayed buffers and restart plays them without re-append") {
        REQUIRE(fixture.audio_out->AppendBuffer(
                    MakeAudioOutBuffer(0x10000, long_frames * frame_bytes, long_frames * frame_bytes),
                    0xAAAA) == ResultSuccess);
        REQUIRE(fixture.audio_out->AppendBuffer(
                    MakeAudioOutBuffer(0x20000, short_frames * frame_bytes,
                                       short_frames * frame_bytes),
                    0xBBBB) == ResultSuccess);

        REQUIRE(fixture.audio_out->Start() == ResultSuccess);
        REQUIRE(fixture.session->submitted_buffers.size() == 2U);

        // Advance only partway through the first buffer, then stop.
        fixture.session->played_sample_count = 2400;
        REQUIRE(fixture.audio_out->Stop() == ResultSuccess);

        CHECK(fixture.audio_out->GetState() == AudioCore::AudioOut::State::Stopped);
        CHECK(fixture.session->stop_count == 1U);
        CHECK(fixture.session->clear_count == 1U);
        CHECK(fixture.signal_count == 0U);
        CHECK(fixture.audio_out->GetBufferCount() == 2U);
        CHECK(fixture.audio_out->ContainsAudioBuffer(0xAAAA));
        CHECK(fixture.audio_out->ContainsAudioBuffer(0xBBBB));

        std::array<u64, 4> released{};
        CHECK(fixture.audio_out->GetReleasedBuffers(released) == 0U);

        // Restart without appending again: unplayed buffers must be re-submitted in order.
        REQUIRE(fixture.audio_out->Start() == ResultSuccess);
        CHECK(fixture.audio_out->GetState() == AudioCore::AudioOut::State::Started);
        REQUIRE(fixture.session->submitted_buffers.size() == 2U);
        CHECK(fixture.session->submitted_buffers[0].tag == 0xAAAA);
        CHECK(fixture.session->submitted_buffers[0].start_timestamp == 0U);
        CHECK(fixture.session->submitted_buffers[0].end_timestamp == long_frames);
        CHECK(fixture.session->submitted_buffers[1].tag == 0xBBBB);
        CHECK(fixture.session->submitted_buffers[1].start_timestamp == long_frames);
        CHECK(fixture.session->submitted_buffers[1].end_timestamp == long_frames + short_frames);

        fixture.session->played_sample_count = long_frames + short_frames;
        fixture.audio_out->ReleaseBuffers();
        CHECK(fixture.signal_count == 1U);
        REQUIRE(fixture.audio_out->GetReleasedBuffers(released) == 2U);
        CHECK(released[0] == 0xAAAA);
        CHECK(released[1] == 0xBBBB);
        CHECK(fixture.audio_out->GetBufferCount() == 0U);
    }

    SECTION("stop releases already-consumed buffers while preserving unplayed buffers with rebased timestamps") {
        REQUIRE(fixture.audio_out->AppendBuffer(
                    MakeAudioOutBuffer(0x10000, short_frames * frame_bytes,
                                       short_frames * frame_bytes),
                    0x1001) == ResultSuccess);
        REQUIRE(fixture.audio_out->AppendBuffer(
                    MakeAudioOutBuffer(0x20000, long_frames * frame_bytes, long_frames * frame_bytes),
                    0x1002) == ResultSuccess);

        REQUIRE(fixture.audio_out->Start() == ResultSuccess);
        // First buffer finished, second buffer still unplayed when Stop() is called.
        fixture.session->played_sample_count = short_frames + 1000;
        REQUIRE(fixture.audio_out->Stop() == ResultSuccess);

        CHECK(fixture.signal_count == 1U);
        std::array<u64, 4> released{};
        REQUIRE(fixture.audio_out->GetReleasedBuffers(released) == 1U);
        CHECK(released[0] == 0x1001);
        CHECK_FALSE(fixture.audio_out->ContainsAudioBuffer(0x1001));
        CHECK(fixture.audio_out->ContainsAudioBuffer(0x1002));
        CHECK(fixture.audio_out->GetBufferCount() == 1U);

        // Restart: second buffer timestamps must rebase to [0, long_frames].
        REQUIRE(fixture.audio_out->Start() == ResultSuccess);
        REQUIRE(fixture.session->submitted_buffers.size() == 1U);
        CHECK(fixture.session->submitted_buffers[0].tag == 0x1002);
        CHECK(fixture.session->submitted_buffers[0].start_timestamp == 0U);
        CHECK(fixture.session->submitted_buffers[0].end_timestamp == long_frames);

        fixture.session->played_sample_count = long_frames;
        fixture.audio_out->ReleaseBuffers();
        REQUIRE(fixture.audio_out->GetReleasedBuffers(released) == 1U);
        CHECK(released[0] == 0x1002);
    }

    SECTION("stop and restart preserve FIFO order across more than BufferAppendLimit buffers") {
        for (u64 i = 0; i < 6; ++i) {
            REQUIRE(fixture.audio_out->AppendBuffer(
                        MakeAudioOutBuffer(0x10000 + i * 0x1000, short_frames * frame_bytes,
                                           short_frames * frame_bytes),
                        0x3000 + i) == ResultSuccess);
        }

        REQUIRE(fixture.audio_out->Start() == ResultSuccess);
        REQUIRE(fixture.session->submitted_buffers.size() == 4U);
        REQUIRE(fixture.audio_out->Stop() == ResultSuccess);
        CHECK(fixture.audio_out->GetBufferCount() == 6U);

        REQUIRE(fixture.audio_out->Start() == ResultSuccess);
        REQUIRE(fixture.session->submitted_buffers.size() == 4U);
        for (u64 i = 0; i < 4; ++i) {
            CHECK(fixture.session->submitted_buffers[i].tag == 0x3000 + i);
        }

        // Complete first 4 buffers, register remaining 2, complete them too.
        fixture.session->played_sample_count = 4 * short_frames;
        fixture.audio_out->ReleaseBuffers();
        fixture.audio_out->RegisterBuffers();
        REQUIRE(fixture.session->submitted_buffers.size() == 6U);
        CHECK(fixture.session->submitted_buffers[4].tag == 0x3004);
        CHECK(fixture.session->submitted_buffers[5].tag == 0x3005);

        fixture.session->played_sample_count = 6 * short_frames;
        fixture.audio_out->ReleaseBuffers();

        std::array<u64, 8> released{};
        REQUIRE(fixture.audio_out->GetReleasedBuffers(released) == 6U);
        for (u64 i = 0; i < 6; ++i) {
            CHECK(released[i] == 0x3000 + i);
        }
    }
}

TEST_CASE("AudioOut FlushAudioOutBuffers requires stopped state and releases queued buffers cleanly",
          "[audio_core][audio_out]") {
    AudioOutTestFixture fixture;
    constexpr u64 frame_bytes = 2 * sizeof(s16);
    constexpr u64 frames = 4800;

    // Stopped with no queued buffers: returns true without signalling event.
    CHECK(fixture.audio_out->FlushAudioOutBuffers());
    CHECK(fixture.signal_count == 0U);

    REQUIRE(fixture.audio_out->AppendBuffer(
                MakeAudioOutBuffer(0x10000, frames * frame_bytes, frames * frame_bytes), 0xA1) ==
            ResultSuccess);
    REQUIRE(fixture.audio_out->AppendBuffer(
                MakeAudioOutBuffer(0x20000, frames * frame_bytes, frames * frame_bytes), 0xA2) ==
            ResultSuccess);
    REQUIRE(fixture.audio_out->Start() == ResultSuccess);

    // Started: FlushAudioOutBuffers must fail and leave buffers untouched.
    CHECK_FALSE(fixture.audio_out->FlushAudioOutBuffers());
    CHECK(fixture.signal_count == 0U);
    CHECK(fixture.audio_out->GetBufferCount() == 2U);
    CHECK(fixture.audio_out->ContainsAudioBuffer(0xA1));
    CHECK(fixture.audio_out->ContainsAudioBuffer(0xA2));

    // Stop then flush: must succeed, signal event, and expose all queued tags for retrieval.
    REQUIRE(fixture.audio_out->Stop() == ResultSuccess);
    CHECK(fixture.signal_count == 0U);
    CHECK(fixture.audio_out->FlushAudioOutBuffers());
    CHECK(fixture.signal_count == 1U);
    CHECK(fixture.audio_out->GetBufferCount() == 0U);

    std::array<u64, 4> released{};
    REQUIRE(fixture.audio_out->GetReleasedBuffers(released) == 2U);
    CHECK(released[0] == 0xA1);
    CHECK(released[1] == 0xA2);
    CHECK_FALSE(fixture.audio_out->ContainsAudioBuffer(0xA1));
    CHECK_FALSE(fixture.audio_out->ContainsAudioBuffer(0xA2));

    // Appending, starting, and releasing after a flush must keep ring indices consistent.
    REQUIRE(fixture.audio_out->AppendBuffer(
                MakeAudioOutBuffer(0x30000, frames * frame_bytes, frames * frame_bytes), 0xA3) ==
            ResultSuccess);
    REQUIRE(fixture.audio_out->Start() == ResultSuccess);
    REQUIRE(fixture.session->submitted_buffers.size() == 1U);
    CHECK(fixture.session->submitted_buffers[0].tag == 0xA3);
    CHECK(fixture.session->submitted_buffers[0].start_timestamp == 0U);
    CHECK(fixture.session->submitted_buffers[0].end_timestamp == frames);

    fixture.session->played_sample_count = frames;
    fixture.audio_out->ReleaseBuffers();
    REQUIRE(fixture.audio_out->GetReleasedBuffers(released) == 1U);
    CHECK(released[0] == 0xA3);
}

TEST_CASE("AudioOut AppendBuffer honors valid nonzero offset and rejects invalid spans",
          "[audio_core][audio_out]") {
    constexpr u64 frame_bytes = 2 * sizeof(s16);

    SECTION("uninitialized system returns ResultOperationFailed") {
        AudioOutTestFixture uninit_fixture{false};
        const auto result = uninit_fixture.audio_out->AppendBuffer(
            MakeAudioOutBuffer(0x10000, 0x1000, 0x400, 0), 0x1);
        CHECK(result == Service::Audio::ResultOperationFailed);
    }

    AudioOutTestFixture fixture;

    SECTION("valid nonzero offset within capacity queues base + offset") {
        REQUIRE(fixture.audio_out->AppendBuffer(
                    MakeAudioOutBuffer(0x100000, 0x2000, 0x800, 0x400), 0x501) == ResultSuccess);
        // Exact boundary: offset + size == capacity.
        REQUIRE(fixture.audio_out->AppendBuffer(
                    MakeAudioOutBuffer(0x200000, 0x1000, 0x600, 0xA00), 0x502) == ResultSuccess);
        // Zero capacity with nonzero offset still honors samples + offset.
        REQUIRE(fixture.audio_out->AppendBuffer(
                    MakeAudioOutBuffer(0x300000, 0, 0x400, 0x100), 0x503) == ResultSuccess);

        REQUIRE(fixture.audio_out->Start() == ResultSuccess);
        REQUIRE(fixture.session->submitted_buffers.size() == 3U);
        CHECK(fixture.session->submitted_buffers[0].samples == 0x100400U);
        CHECK(fixture.session->submitted_buffers[0].size == 0x800U);
        CHECK(fixture.session->submitted_buffers[1].samples == 0x200A00U);
        CHECK(fixture.session->submitted_buffers[1].size == 0x600U);
        CHECK(fixture.session->submitted_buffers[2].samples == 0x300100U);
        CHECK(fixture.session->submitted_buffers[2].size == 0x400U);
    }

    SECTION("invalid spans return ResultInvalidArgument instead of ResultBufferCountReached") {
        // offset + size > capacity
        CHECK(fixture.audio_out->AppendBuffer(
                  MakeAudioOutBuffer(0x100000, 0x1000, 0x800, 0x804), 0x601) ==
              Service::Audio::ResultInvalidArgument);
        // offset > capacity with size == 0
        CHECK(fixture.audio_out->AppendBuffer(
                  MakeAudioOutBuffer(0x100000, 0x1000, 0, 0x1004), 0x602) ==
              Service::Audio::ResultInvalidArgument);
        // offset + size overflow
        CHECK(fixture.audio_out->AppendBuffer(
                  MakeAudioOutBuffer(0x100000, 0x1000, 0x100,
                                     (std::numeric_limits<u64>::max)() - 0x40),
                  0x603) == Service::Audio::ResultInvalidArgument);
        // samples + offset overflow
        CHECK(fixture.audio_out->AppendBuffer(
                  MakeAudioOutBuffer((std::numeric_limits<VAddr>::max)() - 0x40, 0, 0x100, 0x100),
                  0x604) == Service::Audio::ResultInvalidArgument);
        // (samples + offset) + size overflow
        CHECK(fixture.audio_out->AppendBuffer(
                  MakeAudioOutBuffer((std::numeric_limits<VAddr>::max)() - 0x80, 0, 0x100, 0x40),
                  0x605) == Service::Audio::ResultInvalidArgument);
        // Unaligned size (not a multiple of channel_count * sizeof(s16))
        CHECK(fixture.audio_out->AppendBuffer(
                  MakeAudioOutBuffer(0x100000, 0x1000, frame_bytes + 2, 0), 0x606) ==
              Service::Audio::ResultInvalidArgument);
        // Size exceeds MaxDeviceBufferSize
        CHECK(fixture.audio_out->AppendBuffer(
                  MakeAudioOutBuffer(0x100000, 0, AudioCore::MaxDeviceBufferSize + frame_bytes, 0),
                  0x607) == Service::Audio::ResultInvalidArgument);

        CHECK(fixture.audio_out->GetBufferCount() == 0U);
    }

    SECTION("exceeding BufferCount returns ResultBufferCountReached") {
        for (u64 i = 0; i < AudioCore::BufferCount; ++i) {
            REQUIRE(fixture.audio_out->AppendBuffer(
                        MakeAudioOutBuffer(0x100000 + i * 0x1000, 0x1000, 0x400, 0), 0x700 + i) ==
                    ResultSuccess);
        }
        CHECK(fixture.audio_out->AppendBuffer(MakeAudioOutBuffer(0x500000, 0x1000, 0x400, 0),
                                              0x7FF) == Service::Audio::ResultBufferCountReached);
    }
}

TEST_CASE("AudioOut Finalize stops active session and clears queued buffers idempotently",
          "[audio_core][audio_out]") {
    AudioOutTestFixture fixture;
    REQUIRE(fixture.audio_out->AppendBuffer(MakeAudioOutBuffer(0x10000, 0x1000, 0x400, 0), 0x901) ==
            ResultSuccess);
    REQUIRE(fixture.audio_out->Start() == ResultSuccess);

    fixture.audio_out->Finalize();
    CHECK(fixture.audio_out->GetState() == AudioCore::AudioOut::State::Stopped);
    CHECK(fixture.audio_out->GetBufferCount() == 0U);
    CHECK(fixture.session->stop_count == 1U);
    CHECK(fixture.session->clear_count == 1U);
    CHECK(fixture.session->finalize_count == 1U);
    CHECK(fixture.signal_count == 0U);

    // Second Finalize() must be a no-op.
    fixture.audio_out->Finalize();
    CHECK(fixture.session->finalize_count == 1U);
}
