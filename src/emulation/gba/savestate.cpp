// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "emulation/gba/savestate.h"
#include "emulation/gba/bus.h"
#include "emulation/gba/cpu.h"
#include "emulation/gba/types.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace Emulation::Gba {
namespace {

constexpr std::array<std::uint8_t, 8> Magic = {'E', 'G', 'L', 'G', 'B', 'A', '0', '1'};
constexpr std::uint32_t Version = 2;
constexpr std::size_t MaxStateBytes = 4 * 1024 * 1024;

std::uint32_t Crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::uint8_t b : bytes) {
        crc ^= b;
        for (int i = 0; i < 8; ++i) {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

class Writer {
public:
    std::vector<std::uint8_t> buf;

    Writer() {
        buf.reserve(512 * 1024);
    }

    void U8(std::uint8_t v) {
        buf.push_back(v);
    }

    void Bool(bool v) {
        buf.push_back(v ? 1u : 0u);
    }

    void U16(std::uint16_t v) {
        buf.push_back(static_cast<std::uint8_t>(v & 0xFFu));
        buf.push_back(static_cast<std::uint8_t>(v >> 8));
    }

    void I16(std::int16_t v) {
        U16(static_cast<std::uint16_t>(v));
    }

    void U32(std::uint32_t v) {
        buf.push_back(static_cast<std::uint8_t>(v & 0xFFu));
        buf.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
        buf.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
        buf.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
    }

    void I32(std::int32_t v) {
        U32(static_cast<std::uint32_t>(v));
    }

    void U64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            U8(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu));
        }
    }

    void Bytes(std::span<const std::uint8_t> b) {
        U32(static_cast<std::uint32_t>(b.size()));
        buf.insert(buf.end(), b.begin(), b.end());
    }
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}

    bool Take(std::size_t n, std::span<const std::uint8_t>& out, std::string& error) {
        if (n > data_.size() - pos_) {
            error = "Invalid GBA save state: Unexpected end of save state";
            return false;
        }
        out = data_.subspan(pos_, n);
        pos_ += n;
        return true;
    }

    bool U8(std::uint8_t& out, std::string& error) {
        std::span<const std::uint8_t> s;
        if (!Take(1, s, error)) {
            return false;
        }
        out = s[0];
        return true;
    }

    bool Bool(bool& out, std::string& error) {
        std::uint8_t v = 0;
        if (!U8(v, error)) {
            return false;
        }
        out = (v != 0);
        return true;
    }

    bool U16(std::uint16_t& out, std::string& error) {
        std::span<const std::uint8_t> s;
        if (!Take(2, s, error)) {
            return false;
        }
        out = static_cast<std::uint16_t>(s[0]) | (static_cast<std::uint16_t>(s[1]) << 8);
        return true;
    }

    bool I16(std::int16_t& out, std::string& error) {
        std::uint16_t u = 0;
        if (!U16(u, error)) {
            return false;
        }
        out = static_cast<std::int16_t>(u);
        return true;
    }

    bool U32(std::uint32_t& out, std::string& error) {
        std::span<const std::uint8_t> s;
        if (!Take(4, s, error)) {
            return false;
        }
        out = static_cast<std::uint32_t>(s[0]) | (static_cast<std::uint32_t>(s[1]) << 8) |
              (static_cast<std::uint32_t>(s[2]) << 16) | (static_cast<std::uint32_t>(s[3]) << 24);
        return true;
    }

    bool I32(std::int32_t& out, std::string& error) {
        std::uint32_t u = 0;
        if (!U32(u, error)) {
            return false;
        }
        out = static_cast<std::int32_t>(u);
        return true;
    }

    bool U64(std::uint64_t& out, std::string& error) {
        std::span<const std::uint8_t> s;
        if (!Take(8, s, error)) {
            return false;
        }
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(s[static_cast<std::size_t>(i)]) << (i * 8);
        }
        out = v;
        return true;
    }

    bool Bytes(std::span<const std::uint8_t>& out, std::string& error) {
        std::uint32_t len = 0;
        if (!U32(len, error)) {
            return false;
        }
        if (len > MaxStateBytes) {
            error = "Invalid GBA save state: Oversized save state chunk";
            return false;
        }
        return Take(len, out, error);
    }

    bool CopyInto(std::span<std::uint8_t> dst, std::string& error) {
        std::span<const std::uint8_t> src;
        if (!Bytes(src, error)) {
            return false;
        }
        if (src.size() != dst.size()) {
            error = "Invalid GBA save state: Mismatched memory region size";
            return false;
        }
        std::copy(src.begin(), src.end(), dst.begin());
        return true;
    }

private:
    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
};

void WriteFifo(Writer& w, const FifoChannel& fifo) {
    w.U8(static_cast<std::uint8_t>(fifo.latched));
    const std::size_t len = (std::min<std::size_t>)(fifo.queue.size(), 32u);
    w.U8(static_cast<std::uint8_t>(len));
    for (std::size_t i = 0; i < len; ++i) {
        w.U8(static_cast<std::uint8_t>(fifo.queue[i]));
    }
}

bool ReadFifo(Reader& r, FifoChannel& fifo, std::string& error) {
    std::uint8_t latched_u = 0;
    std::uint8_t len_u = 0;
    if (!r.U8(latched_u, error) || !r.U8(len_u, error)) {
        return false;
    }
    if (len_u > 32u) {
        error = "Invalid GBA save state: Corrupted APU FIFO length";
        return false;
    }
    fifo.queue.clear();
    fifo.latched = static_cast<std::int8_t>(latched_u);
    const std::size_t len = len_u;
    for (std::size_t i = 0; i < len; ++i) {
        std::uint8_t b = 0;
        if (!r.U8(b, error)) {
            return false;
        }
        fifo.queue.push_back(static_cast<std::int8_t>(b));
    }
    return true;
}

} // namespace

bool SaveState(const Cpu& cpu, const Bus& bus, std::vector<std::uint8_t>& out,
               std::string& error) {
    Writer w;

    for (std::uint32_t r : cpu.regs.r) {
        w.U32(r);
    }
    w.U32(cpu.regs.cpsr);
    for (std::uint32_t r : cpu.regs.r_fiq) {
        w.U32(r);
    }
    for (std::uint32_t r : cpu.regs.r_usr_hi) {
        w.U32(r);
    }
    w.U32(cpu.regs.sp_usr);
    w.U32(cpu.regs.lr_usr);
    w.U32(cpu.regs.sp_irq);
    w.U32(cpu.regs.lr_irq);
    w.U32(cpu.regs.sp_svc);
    w.U32(cpu.regs.lr_svc);
    w.U32(cpu.regs.sp_abt);
    w.U32(cpu.regs.lr_abt);
    w.U32(cpu.regs.sp_und);
    w.U32(cpu.regs.lr_und);
    w.U32(cpu.regs.spsr_fiq);
    w.U32(cpu.regs.spsr_irq);
    w.U32(cpu.regs.spsr_svc);
    w.U32(cpu.regs.spsr_abt);
    w.U32(cpu.regs.spsr_und);
    w.Bool(cpu.regs.halted);

    w.U16(bus.ie);
    w.U16(bus.if_flags);
    w.U16(bus.ime);
    w.U16(bus.waitcnt);
    w.U8(bus.postflg);
    w.U16(bus.keyinput);
    w.U16(bus.keycnt);
    w.Bool(bus.halt_requested);
    w.Bool(bus.stop_requested);
    w.U32(bus.open_bus_latch);
    w.U32(bus.bios.latch);
    w.Bool(bus.bios.intr_wait_active);
    w.U16(bus.bios.intr_wait_mask);
    w.Bytes(bus.ewram);
    w.Bytes(bus.iwram);

    w.Bytes(bus.ppu.palette_ram);
    w.Bytes(bus.ppu.vram);
    w.Bytes(bus.ppu.oam);
    w.U16(bus.ppu.dispcnt);
    w.U16(bus.ppu.dispstat);
    w.U16(bus.ppu.vcount);
    for (std::uint16_t v : bus.ppu.bgcnt) {
        w.U16(v);
    }
    for (std::uint16_t v : bus.ppu.bghofs) {
        w.U16(v);
    }
    for (std::uint16_t v : bus.ppu.bgvofs) {
        w.U16(v);
    }
    for (const auto& aff : bus.ppu.bg_affine) {
        w.I16(aff.pa);
        w.I16(aff.pb);
        w.I16(aff.pc);
        w.I16(aff.pd);
        w.I32(aff.ref_x);
        w.I32(aff.ref_y);
        w.I32(aff.internal_x);
        w.I32(aff.internal_y);
    }
    w.U16(bus.ppu.winh[0]);
    w.U16(bus.ppu.winh[1]);
    w.U16(bus.ppu.winv[0]);
    w.U16(bus.ppu.winv[1]);
    w.U16(bus.ppu.winin);
    w.U16(bus.ppu.winout);
    w.U16(bus.ppu.mosaic);
    w.U16(bus.ppu.bldcnt);
    w.U16(bus.ppu.bldalpha);
    w.U16(bus.ppu.bldy);
    w.U32(bus.ppu.cycle_in_line);
    w.Bool(bus.ppu.in_hblank);
    for (std::uint16_t px : bus.ppu.raw_frame) {
        w.U16(px);
    }
    for (std::uint16_t px : bus.ppu.prev_raw_frame) {
        w.U16(px);
    }

    w.U16(bus.apu.soundcnt_l);
    w.U16(bus.apu.soundcnt_h);
    w.U16(bus.apu.soundcnt_x);
    w.U16(bus.apu.soundbias);
    w.U16(bus.apu.ch1_sweep);
    w.U16(bus.apu.ch1_cnt_l);
    w.U16(bus.apu.ch1_cnt_h);
    w.U16(bus.apu.ch2_cnt_l);
    w.U16(bus.apu.ch2_cnt_h);
    w.U16(bus.apu.ch3_cnt_l);
    w.U16(bus.apu.ch3_cnt_h);
    w.U16(bus.apu.ch3_cnt_x);
    w.U16(bus.apu.ch4_cnt_l);
    w.U16(bus.apu.ch4_cnt_h);
    w.Bytes(bus.apu.wave_ram);
    WriteFifo(w, bus.apu.fifo_a);
    WriteFifo(w, bus.apu.fifo_b);
    const auto psg = bus.apu.CapturePsg();
    w.U32(psg.ch1_phase);
    w.U32(psg.ch1_timer);
    w.Bool(psg.ch1_enabled);
    w.U8(psg.ch1_vol);
    w.U32(psg.ch2_phase);
    w.U32(psg.ch2_timer);
    w.Bool(psg.ch2_enabled);
    w.U8(psg.ch2_vol);
    w.U32(psg.ch3_phase);
    w.U32(psg.ch3_timer);
    w.Bool(psg.ch3_enabled);
    w.U16(psg.ch4_lfsr);
    w.U32(psg.ch4_timer);
    w.Bool(psg.ch4_enabled);
    w.U8(psg.ch4_vol);
    w.U64(psg.sample_acc);

    for (const auto& ch : bus.dma.channels) {
        w.U32(ch.sad);
        w.U32(ch.dad);
        w.U16(ch.count);
        w.U16(ch.control);
        w.U32(ch.internal_sad);
        w.U32(ch.internal_dad);
        w.U32(ch.internal_count);
    }

    for (const auto& ch : bus.timers.channels) {
        w.U16(ch.counter);
        w.U16(ch.reload);
        w.U16(ch.control);
        w.U32(ch.subcycles);
    }

    const auto cart = bus.cartridge.StateSnapshot();
    w.Bytes(cart);

    const std::uint32_t crc = Crc32(w.buf);
    const auto payload_len = static_cast<std::uint32_t>(w.buf.size());

    out.clear();
    out.reserve(20u + w.buf.size());
    out.insert(out.end(), Magic.begin(), Magic.end());
    auto append_u32 = [&out](std::uint32_t v) {
        out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
    };
    append_u32(Version);
    append_u32(payload_len);
    append_u32(crc);
    out.insert(out.end(), w.buf.begin(), w.buf.end());
    error.clear();
    return true;
}

bool LoadState(std::span<const std::uint8_t> data, Cpu& cpu, Bus& bus, std::string& error) {
    if (data.size() < 20u || data.size() > MaxStateBytes) {
        error = "Invalid GBA save state: Invalid save state size";
        return false;
    }
    if (!std::equal(Magic.begin(), Magic.end(), data.begin())) {
        error = "Invalid GBA save state: Unrecognized save state header";
        return false;
    }
    auto read_u32_at = [&data](std::size_t off) -> std::uint32_t {
        return static_cast<std::uint32_t>(data[off]) |
               (static_cast<std::uint32_t>(data[off + 1u]) << 8) |
               (static_cast<std::uint32_t>(data[off + 2u]) << 16) |
               (static_cast<std::uint32_t>(data[off + 3u]) << 24);
    };
    const std::uint32_t version = read_u32_at(8);
    if (version != Version) {
        error = "Invalid GBA save state: Unsupported version " + std::to_string(version);
        return false;
    }
    const std::size_t payload_len = read_u32_at(12);
    const std::uint32_t expected_crc = read_u32_at(16);
    if (data.size() != 20u + payload_len) {
        error = "Invalid GBA save state: Save state length mismatch";
        return false;
    }
    const auto payload = data.subspan(20);
    if (Crc32(payload) != expected_crc) {
        error = "Invalid GBA save state: Save state checksum mismatch";
        return false;
    }

    Reader r(payload);
    Cpu next_cpu = cpu;
    for (auto& reg : next_cpu.regs.r) {
        if (!r.U32(reg, error)) {
            return false;
        }
    }
    if (!r.U32(next_cpu.regs.cpsr, error)) {
        return false;
    }
    for (auto& reg : next_cpu.regs.r_fiq) {
        if (!r.U32(reg, error)) {
            return false;
        }
    }
    for (auto& reg : next_cpu.regs.r_usr_hi) {
        if (!r.U32(reg, error)) {
            return false;
        }
    }
    if (!r.U32(next_cpu.regs.sp_usr, error) || !r.U32(next_cpu.regs.lr_usr, error) ||
        !r.U32(next_cpu.regs.sp_irq, error) || !r.U32(next_cpu.regs.lr_irq, error) ||
        !r.U32(next_cpu.regs.sp_svc, error) || !r.U32(next_cpu.regs.lr_svc, error) ||
        !r.U32(next_cpu.regs.sp_abt, error) || !r.U32(next_cpu.regs.lr_abt, error) ||
        !r.U32(next_cpu.regs.sp_und, error) || !r.U32(next_cpu.regs.lr_und, error) ||
        !r.U32(next_cpu.regs.spsr_fiq, error) || !r.U32(next_cpu.regs.spsr_irq, error) ||
        !r.U32(next_cpu.regs.spsr_svc, error) || !r.U32(next_cpu.regs.spsr_abt, error) ||
        !r.U32(next_cpu.regs.spsr_und, error) || !r.Bool(next_cpu.regs.halted, error)) {
        return false;
    }

    Bus next_bus = bus;
    if (!r.U16(next_bus.ie, error) || !r.U16(next_bus.if_flags, error) ||
        !r.U16(next_bus.ime, error) || !r.U16(next_bus.waitcnt, error) ||
        !r.U8(next_bus.postflg, error) || !r.U16(next_bus.keyinput, error) ||
        !r.U16(next_bus.keycnt, error) || !r.Bool(next_bus.halt_requested, error) ||
        !r.Bool(next_bus.stop_requested, error) || !r.U32(next_bus.open_bus_latch, error) ||
        !r.U32(next_bus.bios.latch, error) ||
        !r.Bool(next_bus.bios.intr_wait_active, error) ||
        !r.U16(next_bus.bios.intr_wait_mask, error)) {
        return false;
    }

    next_bus.ewram.resize(EwramSize);
    next_bus.iwram.resize(IwramSize);
    if (!r.CopyInto(next_bus.ewram, error) || !r.CopyInto(next_bus.iwram, error)) {
        return false;
    }

    if (!r.CopyInto(next_bus.ppu.palette_ram, error) || !r.CopyInto(next_bus.ppu.vram, error) ||
        !r.CopyInto(next_bus.ppu.oam, error) || !r.U16(next_bus.ppu.dispcnt, error) ||
        !r.U16(next_bus.ppu.dispstat, error) || !r.U16(next_bus.ppu.vcount, error)) {
        return false;
    }
    for (auto& v : next_bus.ppu.bgcnt) {
        if (!r.U16(v, error)) {
            return false;
        }
    }
    for (auto& v : next_bus.ppu.bghofs) {
        if (!r.U16(v, error)) {
            return false;
        }
    }
    for (auto& v : next_bus.ppu.bgvofs) {
        if (!r.U16(v, error)) {
            return false;
        }
    }
    for (auto& aff : next_bus.ppu.bg_affine) {
        if (!r.I16(aff.pa, error) || !r.I16(aff.pb, error) || !r.I16(aff.pc, error) ||
            !r.I16(aff.pd, error) || !r.I32(aff.ref_x, error) || !r.I32(aff.ref_y, error) ||
            !r.I32(aff.internal_x, error) || !r.I32(aff.internal_y, error)) {
            return false;
        }
    }
    if (!r.U16(next_bus.ppu.winh[0], error) || !r.U16(next_bus.ppu.winh[1], error) ||
        !r.U16(next_bus.ppu.winv[0], error) || !r.U16(next_bus.ppu.winv[1], error) ||
        !r.U16(next_bus.ppu.winin, error) || !r.U16(next_bus.ppu.winout, error) ||
        !r.U16(next_bus.ppu.mosaic, error) || !r.U16(next_bus.ppu.bldcnt, error) ||
        !r.U16(next_bus.ppu.bldalpha, error) || !r.U16(next_bus.ppu.bldy, error) ||
        !r.U32(next_bus.ppu.cycle_in_line, error) || !r.Bool(next_bus.ppu.in_hblank, error)) {
        return false;
    }
    next_bus.ppu.raw_frame.resize(ScreenWidth * ScreenHeight);
    for (auto& px : next_bus.ppu.raw_frame) {
        if (!r.U16(px, error)) {
            return false;
        }
    }
    next_bus.ppu.prev_raw_frame.resize(ScreenWidth * ScreenHeight);
    for (auto& px : next_bus.ppu.prev_raw_frame) {
        if (!r.U16(px, error)) {
            return false;
        }
    }
    next_bus.ppu.RefreshOutputFrame();

    if (!r.U16(next_bus.apu.soundcnt_l, error) || !r.U16(next_bus.apu.soundcnt_h, error) ||
        !r.U16(next_bus.apu.soundcnt_x, error) || !r.U16(next_bus.apu.soundbias, error) ||
        !r.U16(next_bus.apu.ch1_sweep, error) || !r.U16(next_bus.apu.ch1_cnt_l, error) ||
        !r.U16(next_bus.apu.ch1_cnt_h, error) || !r.U16(next_bus.apu.ch2_cnt_l, error) ||
        !r.U16(next_bus.apu.ch2_cnt_h, error) || !r.U16(next_bus.apu.ch3_cnt_l, error) ||
        !r.U16(next_bus.apu.ch3_cnt_h, error) || !r.U16(next_bus.apu.ch3_cnt_x, error) ||
        !r.U16(next_bus.apu.ch4_cnt_l, error) || !r.U16(next_bus.apu.ch4_cnt_h, error) ||
        !r.CopyInto(next_bus.apu.wave_ram, error) ||
        !ReadFifo(r, next_bus.apu.fifo_a, error) || !ReadFifo(r, next_bus.apu.fifo_b, error)) {
        return false;
    }
    Apu::PsgSnapshot psg{};
    if (!r.U32(psg.ch1_phase, error) || !r.U32(psg.ch1_timer, error) ||
        !r.Bool(psg.ch1_enabled, error) || !r.U8(psg.ch1_vol, error) ||
        !r.U32(psg.ch2_phase, error) || !r.U32(psg.ch2_timer, error) ||
        !r.Bool(psg.ch2_enabled, error) || !r.U8(psg.ch2_vol, error) ||
        !r.U32(psg.ch3_phase, error) || !r.U32(psg.ch3_timer, error) ||
        !r.Bool(psg.ch3_enabled, error) || !r.U16(psg.ch4_lfsr, error) ||
        !r.U32(psg.ch4_timer, error) || !r.Bool(psg.ch4_enabled, error) ||
        !r.U8(psg.ch4_vol, error) || !r.U64(psg.sample_acc, error)) {
        return false;
    }
    next_bus.apu.RestorePsg(psg);

    for (auto& ch : next_bus.dma.channels) {
        if (!r.U32(ch.sad, error) || !r.U32(ch.dad, error) || !r.U16(ch.count, error) ||
            !r.U16(ch.control, error) || !r.U32(ch.internal_sad, error) ||
            !r.U32(ch.internal_dad, error) || !r.U32(ch.internal_count, error)) {
            return false;
        }
    }

    for (auto& ch : next_bus.timers.channels) {
        if (!r.U16(ch.counter, error) || !r.U16(ch.reload, error) ||
            !r.U16(ch.control, error) || !r.U32(ch.subcycles, error)) {
            return false;
        }
    }

    std::span<const std::uint8_t> cart_bytes;
    if (!r.Bytes(cart_bytes, error) || !next_bus.cartridge.RestoreSnapshot(cart_bytes, error)) {
        return false;
    }

    cpu = std::move(next_cpu);
    bus = std::move(next_bus);
    error.clear();
    return true;
}

} // namespace Emulation::Gba
