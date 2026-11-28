// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "emulation/gba/emulator.h"
#include "emulation/gba/savestate.h"

#include <algorithm>

namespace Emulation::Gba {

GbaEmulator::GbaEmulator(Cpu cpu, Bus bus) : cpu_(std::move(cpu)), bus_(std::move(bus)) {}

std::unique_ptr<GbaEmulator> GbaEmulator::Create(std::vector<std::uint8_t> rom,
                                                 std::span<const std::uint8_t> save,
                                                 std::span<const std::uint8_t> bios_bytes,
                                                 std::string& error) {
    Cartridge cartridge;
    if (!Cartridge::Create(std::move(rom), save, cartridge, error)) {
        return {};
    }
    Bios bios;
    if (!Bios::Create(bios_bytes, bios, error)) {
        return {};
    }
    const bool with_bios = bios.HasRealBios();
    Cpu cpu(with_bios);
    Bus bus(std::move(bios), std::move(cartridge));
    error.clear();
    return std::unique_ptr<GbaEmulator>(new GbaEmulator(std::move(cpu), std::move(bus)));
}

void GbaEmulator::Reset() {
    const bool with_bios = bus_.bios.HasRealBios();
    cpu_.Reset(with_bios);
    bus_.Reset();
    rewind_.Clear();
    excess_cycles_ = 0;
}

void GbaEmulator::RunFrame(std::uint32_t keys) {
    std::vector<std::uint8_t> snapshot;
    std::string ignored;
    if (Emulation::Gba::SaveState(cpu_, bus_, snapshot, ignored)) {
        rewind_.MaybePush(snapshot);
    }
    ApplyCheats();
    bus_.SetKeys(keys & KeyMask);
    bus_.apu.BeginFrame();

    std::uint32_t cycles = excess_cycles_;
    while (cycles < CyclesPerFrame) {
        const std::uint32_t step = (std::max<std::uint32_t>)(cpu_.Step(bus_), 1u);
        cycles += step;
        (void)bus_.Tick(step);
    }
    excess_cycles_ = cycles - CyclesPerFrame;
}

bool GbaEmulator::SaveState(std::vector<std::uint8_t>& out, std::string& error) const {
    return Emulation::Gba::SaveState(cpu_, bus_, out, error);
}

bool GbaEmulator::LoadState(std::span<const std::uint8_t> data, std::string& error) {
    if (!Emulation::Gba::LoadState(data, cpu_, bus_, error)) {
        return false;
    }
    excess_cycles_ = 0;
    return true;
}

bool GbaEmulator::RewindFrame(std::string& error) {
    std::vector<std::uint8_t> raw;
    if (!rewind_.Pop(raw, error)) {
        return false;
    }
    if (!Emulation::Gba::LoadState(raw, cpu_, bus_, error)) {
        return false;
    }
    excess_cycles_ = 0;
    return true;
}

void GbaEmulator::SetVideoOptions(VideoOptions options) {
    bus_.ppu.video_options = options;
    bus_.ppu.RefreshOutputFrame();
}

void GbaEmulator::ApplyCheats() {
    if (cheats_.Empty()) {
        return;
    }
    bool skip_next = false;
    for (const auto& code : cheats_.Codes()) {
        if (skip_next) {
            skip_next = false;
            continue;
        }
        switch (code.kind) {
        case CheatEntry::Kind::Raw16:
        case CheatEntry::Kind::CodeBreakerWrite16:
        case CheatEntry::Kind::GameSharkWrite16:
            bus_.WriteU16(code.addr, static_cast<std::uint16_t>(code.val));
            break;
        case CheatEntry::Kind::Raw32:
        case CheatEntry::Kind::GameSharkWrite32:
            bus_.WriteU32(code.addr, code.val);
            break;
        case CheatEntry::Kind::CodeBreakerWrite8:
        case CheatEntry::Kind::GameSharkWrite8:
            bus_.WriteU8(code.addr, static_cast<std::uint8_t>(code.val));
            break;
        case CheatEntry::Kind::CodeBreakerAdd16: {
            const std::uint16_t cur = bus_.ReadU16(code.addr);
            bus_.WriteU16(code.addr, static_cast<std::uint16_t>(cur + code.val));
            break;
        }
        case CheatEntry::Kind::CodeBreakerIfEqual16:
        case CheatEntry::Kind::GameSharkIfEqual16:
            if (bus_.ReadU16(code.addr) != static_cast<std::uint16_t>(code.val)) {
                skip_next = true;
            }
            break;
        }
    }
}

} // namespace Emulation::Gba
