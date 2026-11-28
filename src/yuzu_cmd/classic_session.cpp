// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_cmd/classic_session.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <openssl/evp.h>

#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/fs_util.h"
#include "common/hex_util.h"
#include "common/logging.h"
#include "emulation/classic/battery_save.h"
#include "emulation/classic/cartridge.h"
#include "emulation/classic/runtime.h"
#include "yuzu_cmd/emu_window/embedded_window.h"
#include "yuzu_cmd/gpui_session.h"

namespace DesktopClassic {
namespace {

namespace Classic = Emulation::Classic;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using enum Gpui::SessionState;

constexpr int AudioRate = Classic::Runtime::AudioRate;
constexpr int MaxQueuedAudio = AudioRate * 2 * static_cast<int>(sizeof(std::int16_t)) / 10;
constexpr std::size_t MaxCartridgeSize = 64 * 1024 * 1024;
constexpr std::size_t MaxStateFileSize = 4 * 1024 * 1024;
constexpr unsigned MaxFrameDimension = 1024;
constexpr auto BatterySaveInterval = 3s;
constexpr std::uint8_t MaxSaveSlots = 9;
constexpr int FastForwardMultiplier = 4;

bool WriteAtomicState(const std::filesystem::path& path, std::span<const std::uint8_t> bytes,
                      std::string& error) {
    if (bytes.empty() || bytes.size() > MaxStateFileSize) {
        error = "Save state exceeds size limit";
        return false;
    }
    return Classic::WriteAtomicFile(path, bytes, MaxStateFileSize, error);
}

bool ReadStateFile(const std::filesystem::path& path, std::vector<std::uint8_t>& out,
                   std::string& error) {
    Common::FS::IOFile file{path, Common::FS::FileAccessMode::Read};
    if (!file.IsOpen()) {
        error = "Save state slot is empty";
        return false;
    }
    const auto size = file.GetSize();
    if (size == 0 || size > MaxStateFileSize) {
        error = "Save state file has an invalid size";
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    if (file.ReadSpan<std::uint8_t>(out) != out.size()) {
        error = "Could not read save state file";
        return false;
    }
    return true;
}

using Window = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>;
using Renderer = std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)>;
using Texture = std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)>;
using AudioStream = std::unique_ptr<SDL_AudioStream, decltype(&SDL_DestroyAudioStream)>;
using Gamepad = std::unique_ptr<SDL_Gamepad, decltype(&SDL_CloseGamepad)>;

struct ButtonMapping {
    SDL_Scancode key;
    SDL_GamepadButton button;
};

// Runtime bit order is A, B, Select, Start, Right, Left, Up, Down, R, L.
constexpr std::array ButtonMappings{
    ButtonMapping{SDL_SCANCODE_X, SDL_GAMEPAD_BUTTON_EAST},
    ButtonMapping{SDL_SCANCODE_Z, SDL_GAMEPAD_BUTTON_SOUTH},
    ButtonMapping{SDL_SCANCODE_BACKSPACE, SDL_GAMEPAD_BUTTON_BACK},
    ButtonMapping{SDL_SCANCODE_RETURN, SDL_GAMEPAD_BUTTON_START},
    ButtonMapping{SDL_SCANCODE_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    ButtonMapping{SDL_SCANCODE_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    ButtonMapping{SDL_SCANCODE_UP, SDL_GAMEPAD_BUTTON_DPAD_UP},
    ButtonMapping{SDL_SCANCODE_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    ButtonMapping{SDL_SCANCODE_S, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    ButtonMapping{SDL_SCANCODE_A, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
};

std::string SdlError(std::string_view context) {
    return std::string{context} + ": " + SDL_GetError();
}

std::string_view SystemName(Classic::ClassicCartridge cartridge) {
    switch (cartridge) {
    case Classic::ClassicCartridge::GameBoy:
        return "game-boy";
    case Classic::ClassicCartridge::GameBoyColor:
        return "game-boy-color";
    case Classic::ClassicCartridge::GameBoyAdvance:
        return "game-boy-advance";
    }
    return {};
}

std::string Digest(std::span<const std::uint8_t> bytes) {
    std::array<std::uint8_t, 32> digest{};
    unsigned int size{};
    if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_sha256(), nullptr) != 1 ||
        size != digest.size()) {
        return {};
    }
    return Common::HexToString(digest, false);
}

} // namespace

struct Session::State {
    std::unique_ptr<Classic::Runtime> runtime;
    Window window{nullptr, SDL_DestroyWindow};
    Renderer renderer{nullptr, SDL_DestroyRenderer};
    Texture texture{nullptr, SDL_DestroyTexture};
    AudioStream audio{nullptr, SDL_DestroyAudioStream};
    std::vector<std::pair<SDL_JoystickID, Gamepad>> gamepads;
    std::filesystem::path save_path;
    std::filesystem::path state_dir;
    std::string rom_hash;
    std::vector<std::uint8_t> persisted_save;
    std::vector<std::uint8_t> final_save;
    Gpui::Session* gpui{};
    std::optional<GpuiEmbed::ParentConfig> embedded_parent;
    SDL_Scancode home_key{};
    SDL_GamepadButton home_button{};
    unsigned texture_width{};
    unsigned texture_height{};
    Clock::duration frame_period{};
    Clock::time_point next_frame{};
    Clock::time_point next_save{};
    std::uint8_t active_slot{1};
    bool color_correction{};
    bool interframe_blending{};
    bool fast_forward{};
    bool key_home{};
    bool home_announced{};
    bool paused{};
    bool stopping{};
    bool save_error_reported{};
    bool fatal_error_reported{};

    ~State();
    bool InitSdl(const LaunchOptions& options, std::string& error);
    bool CreateTexture();
    void AddGamepad(SDL_JoystickID id);
    [[nodiscard]] bool HasFocus() const;
    [[nodiscard]] std::uint32_t Keys() const;
    [[nodiscard]] std::filesystem::path SlotPath(std::uint8_t slot) const;
    bool SaveSlot(std::uint8_t slot, std::string& error) const;
    bool LoadSlot(std::uint8_t slot, std::string& error);
    bool ApplyCheats(const std::vector<std::string>& codes, std::string& error);
    void UpdateHome();
    void ReportError(std::string_view error);
    bool PersistSave(const std::vector<std::uint8_t>& current_save);
    bool FinishRuntime();
    bool Render();
    bool QueueAudio();
    void HandleRequests();
    void RequestStop();
};

Session::State::~State() {
    if (embedded_parent && window) {
        GpuiEmbed::Detach(window.get());
    }
}

bool Session::State::InitSdl(const LaunchOptions& options, std::string& error) {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        error = SdlError("Could not initialize classic gameplay");
        return false;
    }
    if (gpui) {
        embedded_parent = GpuiEmbed::ReadParentFromEnv();
    }
    const auto title = Common::FS::PathToUTF8String(options.cartridge.stem());
    const std::string window_title = title.empty() ? "Eglantine" : "Eglantine — " + title;
    const int width = embedded_parent ? std::max(1, embedded_parent->viewport.width)
                                      : static_cast<int>(runtime->Width() * 3);
    const int height = embedded_parent ? std::max(1, embedded_parent->viewport.height)
                                       : static_cast<int>(runtime->Height() * 3);
    window.reset(SDL_CreateWindow(window_title.c_str(), width, height,
                                  GpuiEmbed::CreateFlags(embedded_parent.has_value())));
    if (!window) {
        error = SdlError("Could not create the classic game window");
        return false;
    }
    if (embedded_parent) {
        if (!GpuiEmbed::Attach(window.get(), *embedded_parent)) {
            LOG_WARNING(Frontend,
                        "Failed to attach classic embedded window; falling back to standalone window");
            embedded_parent.reset();
            SDL_ShowWindow(window.get());
        }
    }
    if (!embedded_parent && options.fullscreen && !SDL_SetWindowFullscreen(window.get(), true)) {
        error = SdlError("Could not create the classic game window");
        return false;
    }
    renderer.reset(SDL_CreateRenderer(window.get(), nullptr));
    if (!renderer || !CreateTexture()) {
        error = SdlError("Could not create the classic game renderer");
        return false;
    }

    const SDL_AudioSpec spec{.format = SDL_AUDIO_S16, .channels = 2, .freq = AudioRate};
    audio.reset(SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr));
    if (!audio || !SDL_SetAudioStreamGain(audio.get(), options.volume) ||
        !SDL_ResumeAudioStreamDevice(audio.get())) {
        error = SdlError("Could not open classic gameplay audio");
        return false;
    }

    int count{};
    using GamepadIds = std::unique_ptr<SDL_JoystickID, decltype(&SDL_free)>;
    const GamepadIds ids{SDL_GetGamepads(&count), SDL_free};
    if (ids) {
        for (int index = 0; index < count; ++index) {
            AddGamepad(ids.get()[index]);
        }
    }
    return true;
}

bool Session::State::CreateTexture() {
    const auto width = runtime->Width();
    const auto height = runtime->Height();
    if (width == 0 || height == 0 || width > MaxFrameDimension || height > MaxFrameDimension) {
        return SDL_SetError("The classic runtime returned an invalid frame size");
    }
    if (!SDL_SetRenderLogicalPresentation(renderer.get(), static_cast<int>(width),
                                          static_cast<int>(height),
                                          SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
        return false;
    }
    texture.reset(SDL_CreateTexture(renderer.get(), SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                    static_cast<int>(width), static_cast<int>(height)));
    if (!texture || !SDL_SetTextureScaleMode(texture.get(), SDL_SCALEMODE_NEAREST)) {
        return false;
    }
    texture_width = width;
    texture_height = height;
    return true;
}

void Session::State::AddGamepad(SDL_JoystickID id) {
    if (std::ranges::any_of(gamepads, [id](const auto& pad) { return pad.first == id; })) {
        return;
    }
    if (Gamepad pad{SDL_OpenGamepad(id), SDL_CloseGamepad}; pad) {
        gamepads.emplace_back(id, std::move(pad));
    } else {
        LOG_WARNING(Frontend, "Could not open classic gamepad {}: {}", id, SDL_GetError());
    }
}

bool Session::State::HasFocus() const {
    if (embedded_parent) {
        return GpuiEmbed::HasFocus(window.get());
    }
    return (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

std::uint32_t Session::State::Keys() const {
    if (!HasFocus()) {
        return 0;
    }
    const bool* keys = SDL_GetKeyboardState(nullptr);
    std::uint32_t result{};
    for (std::size_t index = 0; index < ButtonMappings.size(); ++index) {
        const auto& [key, button] = ButtonMappings[index];
        if ((!gpui || key != home_key) && keys[key]) {
            result |= std::uint32_t{1} << index;
        }
        if ((!gpui || button != home_button) &&
            std::ranges::any_of(gamepads, [button](const auto& pad) {
                return SDL_GetGamepadButton(pad.second.get(), button);
            })) {
            result |= std::uint32_t{1} << index;
        }
    }
    return result;
}

void Session::State::UpdateHome() {
    if (!gpui) {
        return;
    }
    const bool held = key_home || std::ranges::any_of(gamepads, [this](const auto& pad) {
        return SDL_GetGamepadButton(pad.second.get(), home_button);
    });
    if (held && !home_announced && !paused && !stopping && HasFocus()) {
        home_announced = true;
        gpui->SendEvent("home_requested");
    } else if (!held && std::exchange(home_announced, false)) {
        gpui->SendEvent("home_released");
    }
}

void Session::State::ReportError(std::string_view error) {
    LOG_ERROR(Frontend, "{}", error);
    if (gpui) {
        gpui->Error(error);
    }
}

bool Session::State::PersistSave(const std::vector<std::uint8_t>& current_save) {
    if (current_save.empty() || current_save == persisted_save) {
        return true;
    }
    std::string error;
    if (!Classic::WriteBatterySave(save_path, current_save, error)) {
        if (!std::exchange(save_error_reported, true)) {
            ReportError(error);
        }
        return false;
    }
    save_error_reported = false;
    persisted_save = current_save;
    return true;
}

bool Session::State::FinishRuntime() {
    if (runtime) {
        final_save = runtime->Finish();
        runtime.reset();
    }
    return PersistSave(final_save);
}

bool Session::State::Render() {
    if ((SDL_GetWindowFlags(window.get()) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0) {
        return true;
    }
    if ((runtime->Width() != texture_width || runtime->Height() != texture_height) &&
        !CreateTexture()) {
        return false;
    }
    const auto pixels = runtime->Video();
    if (pixels.size() < static_cast<std::size_t>(texture_width) * texture_height) {
        return SDL_SetError("The classic runtime returned an incomplete video frame");
    }
    return SDL_UpdateTexture(texture.get(), nullptr, pixels.data(),
                             static_cast<int>(texture_width * sizeof(std::uint32_t))) &&
           SDL_SetRenderDrawColor(renderer.get(), 0, 0, 0, 255) &&
           SDL_RenderClear(renderer.get()) &&
           SDL_RenderTexture(renderer.get(), texture.get(), nullptr, nullptr) &&
           SDL_RenderPresent(renderer.get());
}

bool Session::State::QueueAudio() {
    const auto samples = runtime->Audio();
    if (samples.empty()) {
        return true;
    }
    const int queued = SDL_GetAudioStreamQueued(audio.get());
    if (queued < 0 || (queued > MaxQueuedAudio && !SDL_ClearAudioStream(audio.get()))) {
        return false;
    }
    return SDL_PutAudioStreamData(audio.get(), samples.data(), static_cast<int>(samples.size_bytes()));
}

std::filesystem::path Session::State::SlotPath(std::uint8_t slot) const {
    return state_dir / fmt::format("{}.slot{}.state", rom_hash, slot);
}

bool Session::State::SaveSlot(std::uint8_t slot, std::string& error) const {
    if (!runtime || !runtime->SupportsGbaFeatures()) {
        error = "Save states are only supported for GBA sessions";
        return false;
    }
    if (slot < 1 || slot > MaxSaveSlots) {
        error = "Save state slot must be between 1 and 9";
        return false;
    }
    std::vector<std::uint8_t> bytes;
    return runtime->SaveState(bytes, error) && WriteAtomicState(SlotPath(slot), bytes, error);
}

bool Session::State::LoadSlot(std::uint8_t slot, std::string& error) {
    if (!runtime || !runtime->SupportsGbaFeatures()) {
        error = "Save states are only supported for GBA sessions";
        return false;
    }
    if (slot < 1 || slot > MaxSaveSlots) {
        error = "Save state slot must be between 1 and 9";
        return false;
    }
    std::vector<std::uint8_t> bytes;
    return ReadStateFile(SlotPath(slot), bytes, error) && runtime->LoadState(bytes, error);
}

bool Session::State::ApplyCheats(const std::vector<std::string>& codes, std::string& error) {
    if (!runtime || !runtime->SupportsGbaFeatures()) {
        error = "Cheats are only supported for GBA sessions";
        return false;
    }
    std::vector<std::string> non_empty;
    non_empty.reserve(codes.size());
    for (const auto& code : codes) {
        if (!code.empty()) {
            non_empty.push_back(code);
        }
    }
    return runtime->SetCheats(non_empty, error);
}

void Session::State::HandleRequests() {
    if (!gpui) {
        return;
    }
    std::vector<Gpui::Request> requests;
    if (!gpui->Poll(requests)) {
        RequestStop();
        return;
    }
    for (const auto& request : requests) {
        std::string error;
        if (stopping && request.command != Gpui::Command::Stop) {
            error = "The game is stopping";
        } else {
            switch (request.command) {
            case Gpui::Command::Pause:
                if (!paused) {
                    const bool hidden = embedded_parent
                                            ? GpuiEmbed::SetVisible(window.get(), false)
                                            : SDL_HideWindow(window.get());
                    if (!hidden) {
                        error = SdlError("Could not hide the paused game window");
                        break;
                    }
                    paused = true;
                    key_home = false;
                    if (!SDL_ClearAudioStream(audio.get())) {
                        ReportError(SdlError("Could not clear paused classic audio"));
                    }
                    gpui->SetState(Paused);
                    PersistSave(runtime->Save());
                }
                break;
            case Gpui::Command::Resume:
                if (paused) {
                    const bool shown = embedded_parent
                                           ? GpuiEmbed::SetVisible(window.get(), true)
                                           : (SDL_ShowWindow(window.get()) &&
                                              SDL_RaiseWindow(window.get()));
                    if (!shown) {
                        error = SdlError("Could not restore the game window");
                        break;
                    }
                    paused = false;
                    next_frame = Clock::now();
                    gpui->SetState(Running);
                }
                break;
            case Gpui::Command::Stop:
                RequestStop();
                break;
            case Gpui::Command::SetViewport:
                if (embedded_parent) {
                    const GpuiEmbed::Viewport viewport{.x = request.viewport_x,
                                                       .y = request.viewport_y,
                                                       .width = request.viewport_width,
                                                       .height = request.viewport_height};
                    if (!GpuiEmbed::Resize(window.get(), viewport)) {
                        error = "Could not resize the classic game viewport";
                        break;
                    }
                    embedded_parent->viewport = viewport;
                    (void)Render();
                }
                break;
            case Gpui::Command::SaveState:
                active_slot = request.slot;
                SaveSlot(request.slot, error);
                break;
            case Gpui::Command::LoadState:
                active_slot = request.slot;
                if (LoadSlot(request.slot, error)) {
                    (void)Render();
                }
                break;
            case Gpui::Command::SetFastForward:
                fast_forward = request.enabled;
                break;
            case Gpui::Command::RewindStep:
                if (runtime->RewindFrame(error)) {
                    (void)Render();
                }
                break;
            case Gpui::Command::SetVideoOptions:
                if (!runtime->SupportsGbaFeatures()) {
                    error = "Video options are only supported for GBA sessions";
                    break;
                }
                color_correction = request.color_correction;
                interframe_blending = request.interframe_blending;
                runtime->SetVideoOptions(color_correction, interframe_blending);
                (void)Render();
                break;
            case Gpui::Command::SetCheats:
                ApplyCheats(request.codes, error);
                break;
            case Gpui::Command::Reset:
                runtime->Reset();
                (void)Render();
                break;
            case Gpui::Command::SwkbdSubmit:
            case Gpui::Command::SwkbdCancel:
                error = "Software keyboard is not supported in classic sessions";
                break;
            }
        }
        gpui->Reply(request.id, error);
    }
}

void Session::State::RequestStop() {
    if (!std::exchange(stopping, true) && gpui) {
        gpui->SetState(Stopping);
    }
}

Session::Session(std::unique_ptr<State> state_) : state{std::move(state_)} {}
Session::~Session() = default;

std::unique_ptr<Session> Session::Open(const LaunchOptions& options, std::string& error) {
    using Common::FS::FileAccessMode;
    Common::FS::IOFile file{options.cartridge, FileAccessMode::Read};
    if (!file.IsOpen()) {
        error = "Could not open classic cartridge: " + Common::FS::PathToUTF8String(options.cartridge);
        return nullptr;
    }
    const auto size = file.GetSize();
    if (size > MaxCartridgeSize) {
        error = "The classic cartridge is too large";
        return nullptr;
    }
    const auto kind = Classic::IdentifyClassicCartridge(
        options.extension, static_cast<std::size_t>(size),
        [&file](std::span<std::uint8_t> header) { return file.ReadSpan(header); });
    if (!kind) {
        error = "The classic cartridge header or size is invalid";
        return nullptr;
    }
    std::vector<std::uint8_t> cartridge(static_cast<std::size_t>(size));
    if (!file.Seek(0) || file.ReadSpan<std::uint8_t>(cartridge) != cartridge.size()) {
        error = "Could not read the complete classic cartridge";
        return nullptr;
    }
    const auto hash = Digest(cartridge);
    if (hash.empty()) {
        error = "Could not calculate the classic cartridge save identity";
        return nullptr;
    }
    auto state = std::make_unique<State>();
    state->rom_hash = hash;
    state->save_path = options.save_root / SystemName(*kind) / (hash + ".sav");
    state->state_dir =
        (options.state_root.empty() ? options.save_root : options.state_root) / SystemName(*kind);
    if (!Classic::ReadBatterySave(state->save_path, state->persisted_save, error)) {
        return nullptr;
    }

    std::vector<std::uint8_t> bios_bytes;
    if (*kind == Classic::ClassicCartridge::GameBoyAdvance && !options.bios_path.empty()) {
        Common::FS::IOFile bios_file{options.bios_path, Common::FS::FileAccessMode::Read};
        if (bios_file.IsOpen()) {
            const auto bios_size = bios_file.GetSize();
            if (bios_size > 64 * 1024) {
                error = "The GBA BIOS file exceeds the size limit";
                return nullptr;
            }
            bios_bytes.resize(static_cast<std::size_t>(bios_size));
            if (bios_file.ReadSpan<std::uint8_t>(bios_bytes) != bios_bytes.size()) {
                error = "Could not read the GBA BIOS file";
                return nullptr;
            }
        } else if (options.bios_explicit) {
            error = "Could not open the configured GBA BIOS: " +
                    Common::FS::PathToUTF8String(options.bios_path);
            return nullptr;
        }
    }

    state->runtime =
        Classic::Runtime::Open(std::move(cartridge), state->persisted_save, error, bios_bytes);
    if (!state->runtime) {
        return nullptr;
    }
    state->color_correction = options.color_correction;
    state->interframe_blending = options.interframe_blending;
    if (state->runtime->SupportsGbaFeatures()) {
        state->runtime->SetVideoOptions(state->color_correction, state->interframe_blending);
        for (const auto& code : options.cheats) {
            std::string cheat_err;
            if (!code.empty() && !state->runtime->AddCheat(code, cheat_err)) {
                LOG_WARNING(Frontend, "Ignoring invalid GBA cheat '{}': {}", code, cheat_err);
            }
        }
    }
    const double frame_rate = state->runtime->FrameRate();
    if (!std::isfinite(frame_rate) || frame_rate < 1.0 || frame_rate > 240.0 ||
        state->runtime->Width() == 0 || state->runtime->Height() == 0 ||
        state->runtime->Width() > MaxFrameDimension || state->runtime->Height() > MaxFrameDimension) {
        error = "The classic runtime returned an unsupported frame size or rate";
        return nullptr;
    }
    state->frame_period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / frame_rate));
    state->gpui = options.gpui;
    state->home_key = options.home_key;
    state->home_button = options.home_button;
    if (!state->InitSdl(options, error)) {
        return nullptr;
    }
    state->next_frame = Clock::now();
    state->next_save = state->next_frame + BatterySaveInterval;
    if (state->gpui) {
        state->gpui->SetState(Running);
    }
    return std::unique_ptr<Session>{new Session(std::move(state))};
}

SDL_AppResult Session::Iterate() {
    state->HandleRequests();
    if (state->stopping) {
        return state->FinishRuntime() ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
    }
    state->UpdateHome();
    const auto now = Clock::now();
    if (state->paused || now < state->next_frame) {
        return SDL_APP_CONTINUE;
    }
    state->next_frame += state->frame_period;
    // Discard large timing slips instead of running a backlog of frames after a stalled window.
    if (now > state->next_frame + state->frame_period * 2) {
        state->next_frame = now + state->frame_period;
    }
    const bool* sdl_keys = state->HasFocus() ? SDL_GetKeyboardState(nullptr) : nullptr;
    const bool rewind_held = sdl_keys && sdl_keys[SDL_SCANCODE_F8] &&
                             state->runtime->SupportsGbaFeatures();
    if (rewind_held) {
        std::string rewind_err;
        if (state->runtime->RewindFrame(rewind_err)) {
            if (!state->Render()) {
                state->fatal_error_reported = true;
                state->ReportError(SdlError("Could not present the rewound classic game frame"));
                return SDL_APP_FAILURE;
            }
        }
        return SDL_APP_CONTINUE;
    }
    const bool tab_ff = sdl_keys && sdl_keys[SDL_SCANCODE_TAB];
    const int steps = (state->fast_forward || tab_ff) ? FastForwardMultiplier : 1;
    const auto input_keys = state->Keys();
    for (int step = 0; step < steps; ++step) {
        state->runtime->RunFrame(input_keys);
        if (step == 0 || SDL_GetAudioStreamQueued(state->audio.get()) <= MaxQueuedAudio) {
            if (!state->QueueAudio()) {
                state->fatal_error_reported = true;
                state->ReportError(SdlError("Could not queue classic game audio"));
                return SDL_APP_FAILURE;
            }
        }
    }
    if (!state->Render()) {
        state->fatal_error_reported = true;
        state->ReportError(SdlError("Could not present the classic game frame"));
        return SDL_APP_FAILURE;
    }
    if (now >= state->next_save) {
        state->next_save = now + BatterySaveInterval;
        state->PersistSave(state->runtime->Save());
    }
    return SDL_APP_CONTINUE;
}

SDL_AppResult Session::Event(const SDL_Event& event) {
    const SDL_WindowID window_id = SDL_GetWindowID(state->window.get());
    switch (event.type) {
    case SDL_EVENT_QUIT:
        state->RequestStop();
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (event.window.windowID == window_id) {
            state->RequestStop();
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (event.window.windowID == window_id) {
            state->key_home = false;
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event.button.windowID == window_id && state->embedded_parent) {
            GpuiEmbed::Focus(state->window.get());
        }
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (event.key.windowID == window_id && !event.key.repeat) {
            if (state->gpui && event.key.scancode == state->home_key) {
                state->key_home = event.key.down;
            } else if (!state->gpui && event.key.scancode == SDL_SCANCODE_ESCAPE && event.key.down) {
                state->RequestStop();
            } else if (event.key.down && state->runtime && state->runtime->SupportsGbaFeatures()) {
                std::string action_err;
                switch (event.key.scancode) {
                case SDL_SCANCODE_F1:
                    if (!state->SaveSlot(state->active_slot, action_err)) {
                        LOG_WARNING(Frontend, "GBA save state failed: {}", action_err);
                    }
                    break;
                case SDL_SCANCODE_F2:
                    if (!state->LoadSlot(state->active_slot, action_err)) {
                        LOG_WARNING(Frontend, "GBA load state failed: {}", action_err);
                    }
                    break;
                case SDL_SCANCODE_F3:
                    state->active_slot = (state->active_slot % MaxSaveSlots) + 1;
                    break;
                case SDL_SCANCODE_F5:
                    state->color_correction = !state->color_correction;
                    state->runtime->SetVideoOptions(state->color_correction,
                                                    state->interframe_blending);
                    break;
                case SDL_SCANCODE_F6:
                    state->interframe_blending = !state->interframe_blending;
                    state->runtime->SetVideoOptions(state->color_correction,
                                                    state->interframe_blending);
                    break;
                case SDL_SCANCODE_F7:
                    state->fast_forward = !state->fast_forward;
                    break;
                case SDL_SCANCODE_F9:
                    state->runtime->Reset();
                    break;
                default:
                    break;
                }
            }
        }
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        state->AddGamepad(event.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        std::erase_if(state->gamepads, [&event](const auto& pad) { return pad.first == event.gdevice.which; });
        break;
    case SDL_EVENT_RENDER_DEVICE_RESET:
        if (event.render.windowID == window_id && !state->CreateTexture()) {
            state->fatal_error_reported = true;
            state->ReportError(SdlError("Could not restore the classic game renderer"));
            return SDL_APP_FAILURE;
        }
        break;
    default:
        break;
    }
    state->UpdateHome();
    if (state->stopping) {
        return state->FinishRuntime() ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
    }
    return SDL_APP_CONTINUE;
}

void Session::Finish(SDL_AppResult result) {
    state->RequestStop();
    if (result == SDL_APP_FAILURE && !state->fatal_error_reported && !state->save_error_reported) {
        state->ReportError("The classic gameplay host failed");
    }
    state->FinishRuntime();
    if (state->gpui) {
        state->gpui->SetState(Stopped);
    }
}

} // namespace DesktopClassic
