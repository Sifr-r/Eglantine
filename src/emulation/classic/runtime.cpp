// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "emulation/classic/runtime.h"
#include "emulation/classic/cartridge.h"
#include "emulation/gba/emulator.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <utility>

#include <mgba/flags.h>

#include <mgba-util/vfs.h>
#include <mgba/core/blip_buf.h>
#include <mgba/core/core.h>
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gb/mbc.h>

namespace Emulation::Classic {
namespace {

constexpr std::size_t MaxRomBytes = 64 * 1024 * 1024;
constexpr std::size_t MaxSaveBytes = 256 * 1024;
constexpr unsigned MaxWidth = 256;
constexpr unsigned MaxHeight = 224;
constexpr int AudioBufferFrames = 2048;
constexpr std::uint32_t KeyMask = 0x3FF;

static_assert(sizeof(color_t) == sizeof(std::uint32_t));

using OwnedFile = std::unique_ptr<VFile, bool (*)(VFile*)>;

OwnedFile OwnFile(VFile* file) {
    if (!file) {
        return {nullptr, nullptr};
    }
    // Keep backing files alive across the core's close callbacks, including a failed ROM load.
    const auto close = std::exchange(file->close, +[](VFile*) { return true; });
    return {file, close};
}

} // namespace

struct Runtime::Impl {
    std::vector<std::uint8_t> rom;
    OwnedFile rom_file{nullptr, nullptr};
    OwnedFile save_file{nullptr, nullptr};
    mCore* core{};
    std::unique_ptr<Emulation::Gba::GbaEmulator> gba;
    bool core_initialized{};
    unsigned width{};
    unsigned height{};
    double frame_rate{};
    std::array<color_t, MaxWidth * MaxHeight> frame_buffer{};
    std::vector<std::uint32_t> video;
    std::vector<std::int16_t> audio;

    ~Impl() {
        CloseCore();
    }

    void CloseCore() {
        gba.reset();
        auto* instance = std::exchange(core, nullptr);
        if (!instance) {
            return;
        }
        mCoreConfigDeinit(&instance->config);
        if (core_initialized) {
            instance->deinit(instance);
        } else {
            std::free(instance);
        }
    }

    bool UpdateDimensions() {
        unsigned next_width{}, next_height{};
        if (gba) {
            next_width = static_cast<unsigned>(gba->Width());
            next_height = static_cast<unsigned>(gba->Height());
        } else {
            core->desiredVideoDimensions(core, &next_width, &next_height);
        }
        if (!next_width || !next_height || next_width > MaxWidth || next_height > MaxHeight) {
            return false;
        }
        width = next_width;
        height = next_height;
        if (!gba) {
            video.resize(static_cast<std::size_t>(width) * height, 0xFF000000);
        }
        return true;
    }

    void FlushClock() const {
        if (!core) {
            return;
        }
        // mGBA normally writes cartridge clocks during its delayed dirty-save check.
        if (core->platform(core) == mPLATFORM_GB) {
            auto* gb = static_cast<GB*>(core->board);
            switch (gb->memory.mbcType) {
            case GB_MBC3_RTC:
                GBMBCRTCWrite(gb);
                break;
            case GB_HuC3:
                GBMBCHuC3Write(gb);
                break;
            case GB_TAMA5:
                GBMBCTAMA5Write(gb);
                break;
            default:
                break;
            }
        }
    }
};

Runtime::Runtime() : impl(std::make_unique<Impl>()) {}
Runtime::~Runtime() = default;

std::unique_ptr<Runtime> Runtime::Open(std::vector<std::uint8_t> rom,
                                       std::span<const std::uint8_t> save, std::string& error,
                                       std::span<const std::uint8_t> bios) {
    if (rom.empty() || rom.size() > MaxRomBytes) {
        error = "The cartridge image is empty or exceeds the size limit";
        return {};
    }
    if (save.size() > MaxSaveBytes) {
        error = "The cartridge battery save exceeds the size limit";
        return {};
    }

    std::unique_ptr<Runtime> runtime{new Runtime};
    auto& state = *runtime->impl;

    const auto is_gba = IdentifyClassicCartridge(
        "gba", rom.size(), [&rom](std::span<std::uint8_t> header) {
            const auto n = (std::min)(header.size(), rom.size());
            std::copy_n(rom.begin(), n, header.begin());
            return n;
        }) == ClassicCartridge::GameBoyAdvance;

    if (is_gba) {
        state.gba = Emulation::Gba::GbaEmulator::Create(std::move(rom), save, bios, error);
        if (!state.gba) {
            if (error.empty()) {
                error = "Could not initialize the GBA core";
            }
            return {};
        }
        if (!state.UpdateDimensions()) {
            error = "The GBA core reported invalid video dimensions";
            return {};
        }
        state.frame_rate = state.gba->FrameRate();
        error.clear();
        return runtime;
    }

    state.rom = std::move(rom);
    state.rom_file = OwnFile(VFileFromMemory(state.rom.data(), state.rom.size()));
    if (!state.rom_file) {
        error = "Could not open the cartridge image in memory";
        return {};
    }

    auto* core = state.core = mCoreFindVF(state.rom_file.get());
    if (!core) {
        error = "The cartridge image is not recognized by mGBA";
        return {};
    }
    mCoreInitConfig(core, nullptr);
    state.core_initialized = core->init(core);
    if (!state.core_initialized) {
        error = "Could not initialize the mGBA core";
        return {};
    }

    mCoreOptions options{};
    options.skipBios = true;
    options.volume = 0x100;
    options.audioBuffers = AudioBufferFrames;
    options.sampleRate = AudioRate;
    mCoreConfigLoadDefaults(&core->config, &options);
    mCoreLoadForeignConfig(core, &core->config);
    core->setVideoBuffer(core, state.frame_buffer.data(), MaxWidth);
    if (state.rom_file->seek(state.rom_file.get(), 0, SEEK_SET) < 0 ||
        !core->loadROM(core, state.rom_file.get())) {
        error = "mGBA could not load the cartridge image";
        return {};
    }

    state.save_file = OwnFile(VFileMemChunk(save.data(), save.size()));
    if (!state.save_file || !core->loadSave(core, state.save_file.get())) {
        error = "mGBA could not load the cartridge battery save";
        return {};
    }

    core->reset(core);
    const auto frequency = core->frequency(core);
    const auto frame_cycles = core->frameCycles(core);
    if (!state.UpdateDimensions() || frequency <= 0 || frame_cycles <= 0) {
        error = "mGBA reported invalid video timing";
        return {};
    }
    state.frame_rate = static_cast<double>(frequency) / frame_cycles;
    for (int channel = 0; channel < 2; ++channel) {
        auto* audio = core->getAudioChannel(core, channel);
        if (!audio) {
            error = "mGBA did not provide both audio channels";
            return {};
        }
        blip_set_rates(audio, frequency, AudioRate);
    }
    state.audio.reserve(2 * AudioBufferFrames);
    error.clear();
    return runtime;
}

void Runtime::RunFrame(std::uint32_t keys) {
    auto& state = *impl;
    if (state.gba) {
        state.gba->RunFrame(keys & KeyMask);
        return;
    }
    auto* core = state.core;
    if (!core) {
        return;
    }
    core->setKeys(core, keys & KeyMask);
    core->runFrame(core);

    if (state.UpdateDimensions()) {
        for (unsigned row = 0; row < state.height; ++row) {
            const auto* source = state.frame_buffer.data() + row * MaxWidth;
            auto* destination = state.video.data() + row * state.width;
            std::transform(source, source + state.width, destination,
                           [](color_t pixel) { return pixel | 0xFF000000; });
        }
    }

    auto* left = core->getAudioChannel(core, 0);
    auto* right = core->getAudioChannel(core, 1);
    const int samples = (std::min)(blip_samples_avail(left), blip_samples_avail(right));
    state.audio.resize(samples > 0 ? 2 * static_cast<std::size_t>(samples) : 0);
    if (!state.audio.empty()) {
        blip_read_samples(left, state.audio.data(), samples, true);
        blip_read_samples(right, state.audio.data() + 1, samples, true);
    }
}

void Runtime::Reset() {
    auto& state = *impl;
    if (state.gba) {
        state.gba->Reset();
    } else if (state.core) {
        state.core->reset(state.core);
    }
}

bool Runtime::SaveState(std::vector<std::uint8_t>& out, std::string& error) const {
    const auto& state = *impl;
    if (!state.gba) {
        error = "Save states are only supported on the GBA runtime";
        return false;
    }
    return state.gba->SaveState(out, error);
}

bool Runtime::LoadState(std::span<const std::uint8_t> data, std::string& error) {
    auto& state = *impl;
    if (!state.gba) {
        error = "Save states are only supported on the GBA runtime";
        return false;
    }
    return state.gba->LoadState(data, error);
}

bool Runtime::RewindFrame(std::string& error) {
    auto& state = *impl;
    if (!state.gba) {
        error = "Rewind is only supported on the GBA runtime";
        return false;
    }
    return state.gba->RewindFrame(error);
}

void Runtime::SetVideoOptions(bool color_correction, bool interframe_blending) {
    auto& state = *impl;
    if (state.gba) {
        state.gba->SetVideoOptions({color_correction, interframe_blending});
    }
}

bool Runtime::AddCheat(std::string_view code, std::string& error) {
    auto& state = *impl;
    if (!state.gba) {
        error = "Cheats are only supported on the GBA runtime";
        return false;
    }
    return state.gba->AddCheat(code, error);
}

bool Runtime::SetCheats(std::span<const std::string> codes, std::string& error) {
    auto& state = *impl;
    if (!state.gba) {
        error = "Cheats are only supported on the GBA runtime";
        return false;
    }
    return state.gba->ReplaceCheats(codes, error);
}

void Runtime::ClearCheats() {
    auto& state = *impl;
    if (state.gba) {
        state.gba->ClearCheats();
    }
}

bool Runtime::SupportsGbaFeatures() const {
    return impl->gba != nullptr;
}

std::span<const std::uint32_t> Runtime::Video() const {
    if (impl->gba) {
        return impl->gba->Video();
    }
    return impl->video;
}

std::span<const std::int16_t> Runtime::Audio() const {
    if (impl->gba) {
        return impl->gba->Audio();
    }
    return impl->audio;
}

unsigned Runtime::Width() const {
    return impl->width;
}

unsigned Runtime::Height() const {
    return impl->height;
}

double Runtime::FrameRate() const {
    return impl->frame_rate;
}

std::vector<std::uint8_t> Runtime::Save() const {
    if (impl->gba) {
        return impl->gba->SaveData();
    }
    impl->FlushClock();
    auto* file = impl->save_file.get();
    if (!file) {
        return {};
    }
    const auto size = file->size(file);
    if (size <= 0 || static_cast<std::size_t>(size) > MaxSaveBytes ||
        file->seek(file, 0, SEEK_SET) < 0) {
        return {};
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (file->read(file, bytes.data(), bytes.size()) != size) {
        return {};
    }
    return bytes;
}

std::vector<std::uint8_t> Runtime::Finish() {
    if (impl->gba) {
        auto saved = Save();
        impl->CloseCore();
        impl->audio.clear();
        return saved;
    }
    impl->FlushClock();
    impl->CloseCore();
    impl->audio.clear();
    return Save();
}

} // namespace Emulation::Classic
