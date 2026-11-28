// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>

namespace Gpui {
class Session;
}

namespace DesktopClassic {

struct LaunchOptions {
    std::filesystem::path cartridge;
    std::filesystem::path save_root;
    std::filesystem::path state_root;
    std::filesystem::path bios_path;
    bool bios_explicit{};
    std::string_view extension;
    bool fullscreen{};
    float volume{1.0f};
    bool color_correction{};
    bool interframe_blending{};
    std::vector<std::string> cheats;
    Gpui::Session* gpui{};
    SDL_Scancode home_key{SDL_SCANCODE_F10};
    SDL_GamepadButton home_button{SDL_GAMEPAD_BUTTON_GUIDE};
};

class Session {
public:
    [[nodiscard]] static std::unique_ptr<Session> Open(const LaunchOptions& options,
                                                       std::string& error);
    ~Session();

    [[nodiscard]] SDL_AppResult Iterate();
    [[nodiscard]] SDL_AppResult Event(const SDL_Event& event);
    void Finish(SDL_AppResult result);

private:
    struct State;

    explicit Session(std::unique_ptr<State> state);
    std::unique_ptr<State> state;
};

} // namespace DesktopClassic
