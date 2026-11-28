// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace Emulation::Gba {

struct FifoChannel {
    std::deque<std::int8_t> queue;
    std::int8_t latched = 0;

    void PushWord(std::uint32_t word);
    [[nodiscard]] bool PopSample();
    void Clear();
};

class Apu {
public:
    std::uint16_t soundcnt_l = 0;
    std::uint16_t soundcnt_h = 0;
    std::uint16_t soundcnt_x = 0;
    std::uint16_t soundbias = 0x0200;

    std::uint16_t ch1_sweep = 0;
    std::uint16_t ch1_cnt_l = 0;
    std::uint16_t ch1_cnt_h = 0;

    std::uint16_t ch2_cnt_l = 0;
    std::uint16_t ch2_cnt_h = 0;

    std::uint16_t ch3_cnt_l = 0;
    std::uint16_t ch3_cnt_h = 0;
    std::uint16_t ch3_cnt_x = 0;
    std::array<std::uint8_t, 32> wave_ram{};

    std::uint16_t ch4_cnt_l = 0;
    std::uint16_t ch4_cnt_h = 0;

    FifoChannel fifo_a{};
    FifoChannel fifo_b{};
    std::vector<std::int16_t> samples;

    Apu();
    void Reset();
    void BeginFrame();

    [[nodiscard]] std::uint16_t ReadIoU16(std::uint32_t offset) const;
    void WriteIoU16(std::uint32_t offset, std::uint16_t val);

    [[nodiscard]] std::pair<bool, bool> OnTimerOverflow(std::uint8_t timer_idx);
    void Tick(std::uint32_t cycles);

    struct PsgSnapshot {
        std::uint32_t ch1_phase = 0;
        std::uint32_t ch1_timer = 0;
        bool ch1_enabled = false;
        std::uint8_t ch1_vol = 0;
        std::uint32_t ch2_phase = 0;
        std::uint32_t ch2_timer = 0;
        bool ch2_enabled = false;
        std::uint8_t ch2_vol = 0;
        std::uint32_t ch3_phase = 0;
        std::uint32_t ch3_timer = 0;
        bool ch3_enabled = false;
        std::uint16_t ch4_lfsr = 0x7FFF;
        std::uint32_t ch4_timer = 0;
        bool ch4_enabled = false;
        std::uint8_t ch4_vol = 0;
        std::uint64_t sample_acc = 0;
    };

    [[nodiscard]] PsgSnapshot CapturePsg() const {
        return {
            .ch1_phase = ch1_phase_,
            .ch1_timer = ch1_timer_,
            .ch1_enabled = ch1_enabled_,
            .ch1_vol = ch1_vol_,
            .ch2_phase = ch2_phase_,
            .ch2_timer = ch2_timer_,
            .ch2_enabled = ch2_enabled_,
            .ch2_vol = ch2_vol_,
            .ch3_phase = ch3_phase_,
            .ch3_timer = ch3_timer_,
            .ch3_enabled = ch3_enabled_,
            .ch4_lfsr = ch4_lfsr_,
            .ch4_timer = ch4_timer_,
            .ch4_enabled = ch4_enabled_,
            .ch4_vol = ch4_vol_,
            .sample_acc = sample_acc_,
        };
    }

    void RestorePsg(const PsgSnapshot& s) {
        ch1_phase_ = s.ch1_phase;
        ch1_timer_ = s.ch1_timer;
        ch1_enabled_ = s.ch1_enabled;
        ch1_vol_ = s.ch1_vol;
        ch2_phase_ = s.ch2_phase;
        ch2_timer_ = s.ch2_timer;
        ch2_enabled_ = s.ch2_enabled;
        ch2_vol_ = s.ch2_vol;
        ch3_phase_ = s.ch3_phase;
        ch3_timer_ = s.ch3_timer;
        ch3_enabled_ = s.ch3_enabled;
        ch4_lfsr_ = s.ch4_lfsr;
        ch4_timer_ = s.ch4_timer;
        ch4_enabled_ = s.ch4_enabled;
        ch4_vol_ = s.ch4_vol;
        sample_acc_ = s.sample_acc;
    }

private:
    void StepPsg(std::uint32_t cycles);
    [[nodiscard]] std::pair<std::int16_t, std::int16_t> MixSample() const;

    std::uint32_t ch1_phase_ = 0;
    std::uint32_t ch1_timer_ = 0;
    bool ch1_enabled_ = false;
    std::uint8_t ch1_vol_ = 0;

    std::uint32_t ch2_phase_ = 0;
    std::uint32_t ch2_timer_ = 0;
    bool ch2_enabled_ = false;
    std::uint8_t ch2_vol_ = 0;

    std::uint32_t ch3_phase_ = 0;
    std::uint32_t ch3_timer_ = 0;
    bool ch3_enabled_ = false;

    std::uint16_t ch4_lfsr_ = 0x7FFF;
    std::uint32_t ch4_timer_ = 0;
    bool ch4_enabled_ = false;
    std::uint8_t ch4_vol_ = 0;

    std::uint64_t sample_acc_ = 0;
};

} // namespace Emulation::Gba
