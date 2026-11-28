// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <optional>

#include <SDL3/SDL.h>

namespace GpuiEmbed {

struct Viewport {
    int x{0};
    int y{0};
    int width{1280};
    int height{720};
};

struct ParentConfig {
    std::uintptr_t parent_hwnd{0};
    Viewport viewport{};
};

[[nodiscard]] std::optional<ParentConfig> ReadParentFromEnv();
[[nodiscard]] SDL_WindowFlags CreateFlags(bool embedded);
bool Attach(SDL_Window* window, const ParentConfig& config);
bool Resize(SDL_Window* window, const Viewport& viewport);
bool SetVisible(SDL_Window* window, bool visible);
void Focus(SDL_Window* window);
[[nodiscard]] bool HasFocus(SDL_Window* window);
void Detach(SDL_Window* window);

} // namespace GpuiEmbed
