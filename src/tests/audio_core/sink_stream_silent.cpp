// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "audio_core/common/common.h"
#include "audio_core/sink/sink.h"
#include "audio_core/sink/sink_stream.h"
#include "common/settings.h"
#include "core/core.h"

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
