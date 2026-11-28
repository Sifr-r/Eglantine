// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "emulation/gba/apu.h"
#include "emulation/gba/types.h"

#include <algorithm>

namespace Emulation::Gba {
namespace {

constexpr std::array<std::array<std::int16_t, 8>, 4> DutyTable = {{
    {{-1, -1, -1, -1, -1, -1, -1, 1}},
    {{1, -1, -1, -1, -1, -1, -1, 1}},
    {{1, -1, -1, -1, -1, 1, 1, 1}},
    {{-1, 1, 1, 1, 1, 1, 1, -1}},
}};

} // namespace

void FifoChannel::PushWord(std::uint32_t word) {
    for (int shift : {0, 8, 16, 24}) {
        if (queue.size() < 32) {
            queue.push_back(static_cast<std::int8_t>((word >> shift) & 0xFFu));
        }
    }
}

bool FifoChannel::PopSample() {
    if (!queue.empty()) {
        latched = queue.front();
        queue.pop_front();
    }
    return queue.size() <= 16;
}

void FifoChannel::Clear() {
    queue.clear();
    latched = 0;
}

Apu::Apu() {
    samples.reserve(2048);
}

void Apu::Reset() {
    *this = Apu{};
}

void Apu::BeginFrame() {
    samples.clear();
}

std::uint16_t Apu::ReadIoU16(std::uint32_t offset) const {
    switch (offset) {
    case 0x60:
        return ch1_sweep;
    case 0x62:
        return ch1_cnt_l;
    case 0x64:
        return ch1_cnt_h;
    case 0x68:
        return ch2_cnt_l;
    case 0x6C:
        return ch2_cnt_h;
    case 0x70:
        return ch3_cnt_l;
    case 0x72:
        return ch3_cnt_h;
    case 0x74:
        return ch3_cnt_x;
    case 0x78:
        return ch4_cnt_l;
    case 0x7C:
        return ch4_cnt_h;
    case 0x80:
        return soundcnt_l;
    case 0x82:
        return soundcnt_h;
    case 0x84: {
        const std::uint16_t status = static_cast<std::uint16_t>(
            (ch1_enabled_ ? 1u : 0u) | ((ch2_enabled_ ? 1u : 0u) << 1) |
            ((ch3_enabled_ ? 1u : 0u) << 2) | ((ch4_enabled_ ? 1u : 0u) << 3));
        return static_cast<std::uint16_t>((soundcnt_x & 0x80u) | status);
    }
    case 0x88:
        return soundbias;
    default:
        if (offset >= 0x90u && offset <= 0x9Eu) {
            const std::size_t idx =
                static_cast<std::size_t>(offset - 0x90u) + ((ch3_cnt_l & 0x40u) != 0 ? 0u : 16u);
            return static_cast<std::uint16_t>(wave_ram[idx]) |
                   (static_cast<std::uint16_t>(wave_ram[idx + 1]) << 8);
        }
        return 0;
    }
}

void Apu::WriteIoU16(std::uint32_t offset, std::uint16_t val) {
    if (offset == 0x84u) {
        const bool was_on = (soundcnt_x & 0x80u) != 0;
        const bool now_on = (val & 0x80u) != 0;
        soundcnt_x = val & 0x80u;
        if (was_on && !now_on) {
            ch1_enabled_ = false;
            ch2_enabled_ = false;
            ch3_enabled_ = false;
            ch4_enabled_ = false;
            soundcnt_l = 0;
        }
        return;
    }
    if (offset == 0x88u) {
        soundbias = val;
        return;
    }
    if (offset >= 0x90u && offset <= 0x9Eu) {
        const std::size_t idx =
            static_cast<std::size_t>(offset - 0x90u) + ((ch3_cnt_l & 0x40u) != 0 ? 0u : 16u);
        wave_ram[idx] = static_cast<std::uint8_t>(val & 0xFFu);
        wave_ram[idx + 1] = static_cast<std::uint8_t>(val >> 8);
        return;
    }
    if ((soundcnt_x & 0x80u) == 0 && offset < 0x82u) {
        return;
    }

    switch (offset) {
    case 0x60:
        ch1_sweep = val;
        break;
    case 0x62:
        ch1_cnt_l = val;
        ch1_vol_ = static_cast<std::uint8_t>((val >> 12) & 0xFu);
        break;
    case 0x64:
        ch1_cnt_h = val;
        if ((val & 0x8000u) != 0) {
            ch1_enabled_ = (ch1_cnt_l & 0xF800u) != 0;
            ch1_vol_ = static_cast<std::uint8_t>((ch1_cnt_l >> 12) & 0xFu);
        }
        break;
    case 0x68:
        ch2_cnt_l = val;
        ch2_vol_ = static_cast<std::uint8_t>((val >> 12) & 0xFu);
        break;
    case 0x6C:
        ch2_cnt_h = val;
        if ((val & 0x8000u) != 0) {
            ch2_enabled_ = (ch2_cnt_l & 0xF800u) != 0;
            ch2_vol_ = static_cast<std::uint8_t>((ch2_cnt_l >> 12) & 0xFu);
        }
        break;
    case 0x70:
        ch3_cnt_l = val;
        break;
    case 0x72:
        ch3_cnt_h = val;
        break;
    case 0x74:
        ch3_cnt_x = val;
        if ((val & 0x8000u) != 0) {
            ch3_enabled_ = (ch3_cnt_l & 0x80u) != 0;
        }
        break;
    case 0x78:
        ch4_cnt_l = val;
        ch4_vol_ = static_cast<std::uint8_t>((val >> 12) & 0xFu);
        break;
    case 0x7C:
        ch4_cnt_h = val;
        if ((val & 0x8000u) != 0) {
            ch4_enabled_ = (ch4_cnt_l & 0xF800u) != 0;
            ch4_vol_ = static_cast<std::uint8_t>((ch4_cnt_l >> 12) & 0xFu);
            ch4_lfsr_ = 0x7FFF;
        }
        break;
    case 0x80:
        soundcnt_l = val;
        break;
    case 0x82:
        soundcnt_h = val;
        if ((val & (1u << 11)) != 0) {
            fifo_a.Clear();
        }
        if ((val & (1u << 15)) != 0) {
            fifo_b.Clear();
        }
        break;
    default:
        break;
    }
}

std::pair<bool, bool> Apu::OnTimerOverflow(std::uint8_t timer_idx) {
    bool need_dma1 = false;
    bool need_dma2 = false;
    const auto fifo_a_timer = static_cast<std::uint8_t>((soundcnt_h >> 10) & 1u);
    const auto fifo_b_timer = static_cast<std::uint8_t>((soundcnt_h >> 14) & 1u);
    if (timer_idx == fifo_a_timer && fifo_a.PopSample()) {
        need_dma1 = true;
    }
    if (timer_idx == fifo_b_timer && fifo_b.PopSample()) {
        need_dma2 = true;
    }
    return {need_dma1, need_dma2};
}

void Apu::Tick(std::uint32_t cycles) {
    StepPsg(cycles);
    sample_acc_ += static_cast<std::uint64_t>(cycles) * AudioRateHz;
    while (sample_acc_ >= CpuClockHz) {
        sample_acc_ -= CpuClockHz;
        const auto [l, r] = MixSample();
        samples.push_back(l);
        samples.push_back(r);
    }
}

void Apu::StepPsg(std::uint32_t cycles) {
    if ((soundcnt_x & 0x80u) == 0) {
        return;
    }
    if (ch1_enabled_) {
        const std::uint32_t period = (std::max<std::uint32_t>)(2048u - (ch1_cnt_h & 0x7FFu), 1u) * 16u;
        ch1_timer_ += cycles;
        if (ch1_timer_ >= period) {
            ch1_phase_ = (ch1_phase_ + ch1_timer_ / period) & 7u;
            ch1_timer_ %= period;
        }
    }
    if (ch2_enabled_) {
        const std::uint32_t period = (std::max<std::uint32_t>)(2048u - (ch2_cnt_h & 0x7FFu), 1u) * 16u;
        ch2_timer_ += cycles;
        if (ch2_timer_ >= period) {
            ch2_phase_ = (ch2_phase_ + ch2_timer_ / period) & 7u;
            ch2_timer_ %= period;
        }
    }
    if (ch3_enabled_ && (ch3_cnt_l & 0x80u) != 0) {
        const std::uint32_t period = (std::max<std::uint32_t>)(2048u - (ch3_cnt_x & 0x7FFu), 1u) * 8u;
        ch3_timer_ += cycles;
        if (ch3_timer_ >= period) {
            ch3_phase_ = (ch3_phase_ + ch3_timer_ / period) & 31u;
            ch3_timer_ %= period;
        }
    }
    if (ch4_enabled_) {
        const std::uint32_t n = ch4_cnt_h & 0x7u;
        const std::uint32_t div = (n == 0) ? 8u : (n * 16u);
        const std::uint32_t shift = (std::min<std::uint32_t>)((ch4_cnt_h >> 4) & 0xFu, 13u);
        const std::uint32_t period = div << shift;
        ch4_timer_ += cycles;
        while (ch4_timer_ >= period) {
            ch4_timer_ -= period;
            const std::uint16_t xorbit = static_cast<std::uint16_t>((ch4_lfsr_ & 1u) ^ ((ch4_lfsr_ >> 1) & 1u));
            ch4_lfsr_ = static_cast<std::uint16_t>((ch4_lfsr_ >> 1) | (xorbit << 14));
            if ((ch4_cnt_h & (1u << 3)) != 0) {
                ch4_lfsr_ = static_cast<std::uint16_t>((ch4_lfsr_ & ~(1u << 6)) | (xorbit << 6));
            }
        }
    }
}

std::pair<std::int16_t, std::int16_t> Apu::MixSample() const {
    if ((soundcnt_x & 0x80u) == 0) {
        return {0, 0};
    }

    std::int32_t psg_l = 0;
    std::int32_t psg_r = 0;

    std::int16_t s1 = 0;
    if (ch1_enabled_) {
        const std::size_t duty = (ch1_cnt_l >> 6) & 3u;
        s1 = static_cast<std::int16_t>(DutyTable[duty][ch1_phase_] * static_cast<std::int16_t>(ch1_vol_));
    }
    std::int16_t s2 = 0;
    if (ch2_enabled_) {
        const std::size_t duty = (ch2_cnt_l >> 6) & 3u;
        s2 = static_cast<std::int16_t>(DutyTable[duty][ch2_phase_] * static_cast<std::int16_t>(ch2_vol_));
    }
    std::int16_t s3 = 0;
    if (ch3_enabled_ && (ch3_cnt_l & 0x80u) != 0) {
        const std::size_t bank_off = ((ch3_cnt_l & 0x40u) != 0) ? 16u : 0u;
        const std::uint8_t byte = wave_ram[bank_off + (ch3_phase_ / 2u)];
        const std::uint8_t nibble = ((ch3_phase_ & 1u) == 0) ? (byte >> 4) : (byte & 0xFu);
        s3 = static_cast<std::int16_t>((static_cast<std::int16_t>(nibble) - 8) * 2);
    }
    std::int16_t s4 = 0;
    if (ch4_enabled_) {
        const std::int16_t sign = ((ch4_lfsr_ & 1u) == 0) ? 1 : -1;
        s4 = static_cast<std::int16_t>(sign * static_cast<std::int16_t>(ch4_vol_));
    }

    const std::array<std::int32_t, 4> chans = {s1, s2, s3, s4};
    for (std::size_t i = 0; i < 4; ++i) {
        if ((soundcnt_l & (1u << (12u + i))) != 0) {
            psg_l += chans[i];
        }
        if ((soundcnt_l & (1u << (8u + i))) != 0) {
            psg_r += chans[i];
        }
    }

    const std::int32_t vol_l = static_cast<std::int32_t>((soundcnt_l >> 4) & 7u) + 1;
    const std::int32_t vol_r = static_cast<std::int32_t>(soundcnt_l & 7u) + 1;
    psg_l = (psg_l * vol_l) >> 1;
    psg_r = (psg_r * vol_r) >> 1;

    std::int32_t psg_shift = 2;
    switch (soundcnt_h & 3u) {
    case 0:
        psg_shift = 2;
        break;
    case 1:
        psg_shift = 1;
        break;
    case 2:
        psg_shift = 0;
        break;
    default:
        psg_shift = 2;
        break;
    }
    psg_l >>= psg_shift;
    psg_r >>= psg_shift;

    std::int32_t total_l = psg_l;
    std::int32_t total_r = psg_r;

    const std::int32_t a_shift = ((soundcnt_h & (1u << 2)) != 0) ? 2 : 1;
    const std::int32_t b_shift = ((soundcnt_h & (1u << 3)) != 0) ? 2 : 1;
    const std::int32_t dma_a = static_cast<std::int32_t>(fifo_a.latched) * a_shift;
    const std::int32_t dma_b = static_cast<std::int32_t>(fifo_b.latched) * b_shift;

    if ((soundcnt_h & (1u << 9)) != 0) {
        total_l += dma_a;
    }
    if ((soundcnt_h & (1u << 8)) != 0) {
        total_r += dma_a;
    }
    if ((soundcnt_h & (1u << 13)) != 0) {
        total_l += dma_b;
    }
    if ((soundcnt_h & (1u << 12)) != 0) {
        total_r += dma_b;
    }

    const auto out_l = static_cast<std::int16_t>(std::clamp(total_l * 48, -32768, 32767));
    const auto out_r = static_cast<std::int16_t>(std::clamp(total_r * 48, -32768, 32767));
    return {out_l, out_r};
}

} // namespace Emulation::Gba
