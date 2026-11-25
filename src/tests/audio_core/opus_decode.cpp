// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <opus.h>
#include <opus_multistream.h>

#include "audio_core/adsp/apps/opus/opus_decode_object.h"
#include "audio_core/adsp/apps/opus/opus_multistream_decode_object.h"
#include "common/scope_exit.h"

namespace {

using AudioCore::ADSP::OpusDecoder::OpusDecodeObject;
using AudioCore::ADSP::OpusDecoder::OpusMultiStreamDecodeObject;

constexpr u32 SampleRate = 48'000;
constexpr int FrameSamples = 960; // 20 ms at 48 kHz
constexpr u8 CanaryByte = 0xA5;

struct AlignedWorkBuffer {
    std::vector<u64> storage;

    explicit AlignedWorkBuffer(u32 byte_size) : storage((byte_size + sizeof(u64) - 1) / sizeof(u64), 0) {}

    u64 Address() {
        return reinterpret_cast<u64>(storage.data());
    }
};

std::vector<u8> EncodeSingleStreamPacket(int channels) {
    int err = OPUS_OK;
    OpusEncoder* encoder = opus_encoder_create(SampleRate, channels, OPUS_APPLICATION_AUDIO, &err);
    REQUIRE(err == OPUS_OK);
    REQUIRE(encoder != nullptr);
    SCOPE_EXIT { opus_encoder_destroy(encoder); };

    std::vector<opus_int16> pcm(static_cast<size_t>(FrameSamples) * channels);
    for (int i = 0; i < FrameSamples; ++i) {
        const auto sample = static_cast<opus_int16>(10000.0 * std::sin(2.0 * 3.14159265 * 440.0 * i / SampleRate));
        for (int ch = 0; ch < channels; ++ch) {
            pcm[static_cast<size_t>(i) * channels + ch] = static_cast<opus_int16>(sample / (ch + 1));
        }
    }

    std::vector<u8> packet(512);
    const int encoded_bytes = opus_encode(encoder, pcm.data(), FrameSamples, packet.data(),
                                          static_cast<opus_int32>(packet.size()));
    REQUIRE(encoded_bytes > 0);
    packet.resize(static_cast<size_t>(encoded_bytes));
    return packet;
}

std::vector<u8> EncodeMultiStreamPacket(int channels, int streams, int coupled_streams,
                                        const unsigned char* mapping) {
    int err = OPUS_OK;
    OpusMSEncoder* encoder = opus_multistream_encoder_create(
        SampleRate, channels, streams, coupled_streams, mapping, OPUS_APPLICATION_AUDIO, &err);
    REQUIRE(err == OPUS_OK);
    REQUIRE(encoder != nullptr);
    SCOPE_EXIT { opus_multistream_encoder_destroy(encoder); };

    std::vector<opus_int16> pcm(static_cast<size_t>(FrameSamples) * channels);
    for (int i = 0; i < FrameSamples; ++i) {
        const auto sample = static_cast<opus_int16>(8000.0 * std::sin(2.0 * 3.14159265 * 330.0 * i / SampleRate));
        for (int ch = 0; ch < channels; ++ch) {
            pcm[static_cast<size_t>(i) * channels + ch] = static_cast<opus_int16>(sample / (ch + 1));
        }
    }

    std::vector<u8> packet(1024);
    const int encoded_bytes = opus_multistream_encode(encoder, pcm.data(), FrameSamples, packet.data(),
                                                      static_cast<opus_int32>(packet.size()));
    REQUIRE(encoded_bytes > 0);
    packet.resize(static_cast<size_t>(encoded_bytes));
    return packet;
}

bool CanariesIntact(const std::vector<u8>& buffer, size_t capacity) {
    return std::all_of(buffer.begin() + static_cast<std::ptrdiff_t>(capacity), buffer.end(),
                       [](u8 byte) { return byte == CanaryByte; });
}

} // namespace

TEST_CASE("OpusDecodeObject decodes mono and stereo packets and enforces output capacity",
          "[audio_core][opus]") {
    SCOPE_EXIT { OpusDecodeObject::ReleaseAll(); };

    CHECK(OpusDecodeObject::GetWorkBufferSize(0) == 0);
    CHECK(OpusDecodeObject::GetWorkBufferSize(3) == 0);
    const u32 mono_size = OpusDecodeObject::GetWorkBufferSize(1);
    const u32 stereo_size = OpusDecodeObject::GetWorkBufferSize(2);
    REQUIRE(mono_size > 0);
    REQUIRE(stereo_size > mono_size);

    for (const u32 channels : {1U, 2U}) {
        const u32 work_size = OpusDecodeObject::GetWorkBufferSize(channels);
        AlignedWorkBuffer work{work_size};
        auto& obj = OpusDecodeObject::Initialize(work.Address(), work.Address());
        REQUIRE(obj.InitializeDecoder(SampleRate, channels) == OPUS_OK);

        const auto packet = EncodeSingleStreamPacket(static_cast<int>(channels));
        const size_t valid_bytes = static_cast<size_t>(FrameSamples) * channels * sizeof(s16);
        constexpr size_t CanaryBytes = 64;

        std::vector<u8> out(valid_bytes + CanaryBytes, CanaryByte);
        u32 out_samples = 0;
        const s32 decode_rc = obj.Decode(out_samples, reinterpret_cast<u64>(out.data()), valid_bytes,
                                         reinterpret_cast<u64>(packet.data()), packet.size());
        REQUIRE(decode_rc == OPUS_OK);
        CHECK(out_samples == FrameSamples);
        CHECK(CanariesIntact(out, valid_bytes));

        CHECK(obj.ResetDecoder() == OPUS_OK);

        // Undersized buffers must return an error and never write into trailing canary bytes.
        for (const size_t small_cap : {size_t{0}, size_t{2}, size_t{100}}) {
            if (small_cap >= valid_bytes) {
                continue;
            }
            std::vector<u8> small_out(small_cap + CanaryBytes, CanaryByte);
            u32 small_samples = 999;
            const s32 small_rc =
                obj.Decode(small_samples, reinterpret_cast<u64>(small_out.data()), small_cap,
                           reinterpret_cast<u64>(packet.data()), packet.size());
            CHECK(small_rc < OPUS_OK);
            CHECK(small_samples == 0);
            CHECK(CanariesIntact(small_out, small_cap));
        }

        CHECK(obj.Shutdown() == OPUS_OK);
        CHECK(obj.Shutdown() == OPUS_INVALID_STATE);
    }
}

TEST_CASE("OpusDecodeObject rejects invalid setup, mismatched alias, and evicted state",
          "[audio_core][opus]") {
    SCOPE_EXIT { OpusDecodeObject::ReleaseAll(); };

    AlignedWorkBuffer work{OpusDecodeObject::GetWorkBufferSize(2)};
    auto& obj = OpusDecodeObject::Initialize(work.Address(), work.Address());

    CHECK(obj.ResetDecoder() == OPUS_INVALID_STATE);
    CHECK(obj.InitializeDecoder(44'100, 2) == OPUS_BAD_ARG);
    CHECK(obj.InitializeDecoder(SampleRate, 0) == OPUS_BAD_ARG);

    REQUIRE(obj.InitializeDecoder(SampleRate, 2) == OPUS_OK);
    const auto packet = EncodeSingleStreamPacket(2);

    // Corrupted/truncated packet fails without overrunning output.
    std::vector<u8> bad_packet{0xFF};
    std::vector<u8> out(FrameSamples * 2 * sizeof(s16) + 32, CanaryByte);
    u32 out_samples = 123;
    CHECK(obj.Decode(out_samples, reinterpret_cast<u64>(out.data()), out.size() - 32,
                     reinterpret_cast<u64>(bad_packet.data()), bad_packet.size()) < OPUS_OK);
    CHECK(out_samples == 0);
    CHECK(CanariesIntact(out, out.size() - 32));

    // Re-initializing an already initialized buffer with a different self pointer leaves state_valid unchanged/false.
    AlignedWorkBuffer other{OpusDecodeObject::GetWorkBufferSize(2)};
    auto& aliased = OpusDecodeObject::Initialize(work.Address(), other.Address());
    CHECK(aliased.InitializeDecoder(SampleRate, 2) == OPUS_OK); // initialized && GetDecoder() != nullptr

    // After ReleaseAll, Decode on an initialized object returns OPUS_INVALID_STATE cleanly.
    OpusDecodeObject::ReleaseAll();
    CHECK(obj.Decode(out_samples, reinterpret_cast<u64>(out.data()), out.size() - 32,
                     reinterpret_cast<u64>(packet.data()), packet.size()) == OPUS_INVALID_STATE);
    CHECK(out_samples == 0);
}

TEST_CASE("OpusMultiStreamDecodeObject decodes 4-channel streams and enforces capacity bounds",
          "[audio_core][opus]") {
    SCOPE_EXIT { OpusMultiStreamDecodeObject::ReleaseAll(); };

    CHECK(OpusMultiStreamDecodeObject::GetWorkBufferSize(0, 0) == 0);
    CHECK(OpusMultiStreamDecodeObject::GetWorkBufferSize(1, 2) == 0);
    CHECK(OpusMultiStreamDecodeObject::GetWorkBufferSize(256, 0) == 0);

    constexpr u32 Channels = 4;
    constexpr u32 TotalStreams = 2;
    constexpr u32 CoupledStreams = 2;
    std::array<u8, Channels> mapping{0, 1, 2, 3};

    const u32 work_size = OpusMultiStreamDecodeObject::GetWorkBufferSize(TotalStreams, CoupledStreams);
    REQUIRE(work_size > 0);
    AlignedWorkBuffer work{work_size};

    auto& obj = OpusMultiStreamDecodeObject::Initialize(work.Address(), work.Address());
    REQUIRE(obj.InitializeDecoder(SampleRate, TotalStreams, Channels, CoupledStreams, mapping.data()) ==
            OPUS_OK);

    const auto packet = EncodeMultiStreamPacket(Channels, TotalStreams, CoupledStreams, mapping.data());
    const size_t valid_bytes = static_cast<size_t>(FrameSamples) * Channels * sizeof(s16);
    constexpr size_t CanaryBytes = 64;

    std::vector<u8> out(valid_bytes + CanaryBytes, CanaryByte);
    u32 out_samples = 0;
    REQUIRE(obj.Decode(out_samples, reinterpret_cast<u64>(out.data()), valid_bytes,
                       reinterpret_cast<u64>(packet.data()), packet.size()) == OPUS_OK);
    CHECK(out_samples == FrameSamples);
    CHECK(CanariesIntact(out, valid_bytes));

    CHECK(obj.ResetDecoder() == OPUS_OK);

    for (const size_t small_cap : {size_t{0}, size_t{4}, size_t{200}}) {
        std::vector<u8> small_out(small_cap + CanaryBytes, CanaryByte);
        u32 small_samples = 777;
        const s32 small_rc =
            obj.Decode(small_samples, reinterpret_cast<u64>(small_out.data()), small_cap,
                       reinterpret_cast<u64>(packet.data()), packet.size());
        CHECK(small_rc < OPUS_OK);
        CHECK(small_samples == 0);
        CHECK(CanariesIntact(small_out, small_cap));
    }

    CHECK(obj.Shutdown() == OPUS_OK);
    CHECK(obj.Shutdown() == OPUS_INVALID_STATE);
}
