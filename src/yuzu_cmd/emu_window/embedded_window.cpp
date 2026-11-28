// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_cmd/emu_window/embedded_window.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace GpuiEmbed {

#ifdef _WIN32
namespace {

HWND ChildHwnd(SDL_Window* window) {
    if (!window) {
        return nullptr;
    }
    const SDL_PropertiesID props = SDL_GetWindowProperties(window);
    return static_cast<HWND>(
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
}

bool ParseViewport(const char* text, Viewport& out) {
    if (!text || *text == '\0') {
        return false;
    }
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    char trailing = '\0';
    if (std::sscanf(text, "%d,%d,%d,%d%c", &x, &y, &w, &h, &trailing) != 4) {
        return false;
    }
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || x < -16384 || x > 16384 || y < -16384 ||
        y > 16384) {
        return false;
    }
    out = Viewport{.x = x, .y = y, .width = w, .height = h};
    return true;
}

} // namespace
#endif

std::optional<ParentConfig> ReadParentFromEnv() {
#ifdef _WIN32
    const char* raw_hwnd = std::getenv("EGLANTINE_GPUI_PARENT_HWND");
    if (!raw_hwnd || *raw_hwnd == '\0') {
        return std::nullopt;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(raw_hwnd, &end, 10);
    if (end == raw_hwnd || *end != '\0' || parsed == 0) {
        return std::nullopt;
    }
    const HWND parent = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(parsed));
    if (!IsWindow(parent)) {
        return std::nullopt;
    }
    ParentConfig config{
        .parent_hwnd = static_cast<std::uintptr_t>(parsed),
        .viewport = {},
    };
    if (!ParseViewport(std::getenv("EGLANTINE_GPUI_VIEWPORT"), config.viewport)) {
        RECT rect{};
        if (GetClientRect(parent, &rect)) {
            config.viewport.x = 0;
            config.viewport.y = 0;
            config.viewport.width = std::max<int>(1, rect.right - rect.left);
            config.viewport.height = std::max<int>(1, rect.bottom - rect.top);
        }
    }
    return config;
#else
    return std::nullopt;
#endif
}

SDL_WindowFlags CreateFlags(bool embedded) {
    if (embedded) {
        return SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    }
    return SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
}

bool Attach(SDL_Window* window, const ParentConfig& config) {
#ifdef _WIN32
    const HWND child = ChildHwnd(window);
    const HWND parent = reinterpret_cast<HWND>(config.parent_hwnd);
    if (!child || !IsWindow(parent)) {
        return false;
    }
    LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
    style &= ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
               WS_SYSMENU | WS_BORDER | WS_DLGFRAME);
    style |= WS_CHILD | WS_CLIPSIBLINGS;
    SetWindowLongPtrW(child, GWL_STYLE, style);

    LONG_PTR ex_style = GetWindowLongPtrW(child, GWL_EXSTYLE);
    ex_style &= ~(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME);
    SetWindowLongPtrW(child, GWL_EXSTYLE, ex_style);

    SetParent(child, parent);
    SetWindowPos(child, HWND_TOP, config.viewport.x, config.viewport.y,
                 std::max(1, config.viewport.width), std::max(1, config.viewport.height),
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    SetFocus(child);
    return true;
#else
    (void)window;
    (void)config;
    return false;
#endif
}

bool Resize(SDL_Window* window, const Viewport& viewport) {
#ifdef _WIN32
    const HWND child = ChildHwnd(window);
    if (!child || !IsWindow(child)) {
        return false;
    }
    return SetWindowPos(child, HWND_TOP, viewport.x, viewport.y, std::max(1, viewport.width),
                        std::max(1, viewport.height), SWP_NOACTIVATE | SWP_NOZORDER) != 0;
#else
    (void)window;
    (void)viewport;
    return false;
#endif
}

bool SetVisible(SDL_Window* window, bool visible) {
#ifdef _WIN32
    const HWND child = ChildHwnd(window);
    if (!child || !IsWindow(child)) {
        return false;
    }
    ShowWindow(child, visible ? SW_SHOW : SW_HIDE);
    if (visible) {
        SetFocus(child);
    }
    return true;
#else
    (void)window;
    (void)visible;
    return false;
#endif
}

void Focus(SDL_Window* window) {
#ifdef _WIN32
    const HWND child = ChildHwnd(window);
    if (child && IsWindow(child)) {
        SetFocus(child);
    }
#else
    (void)window;
#endif
}

bool HasFocus(SDL_Window* window) {
#ifdef _WIN32
    const HWND child = ChildHwnd(window);
    if (child && IsWindow(child) && GetFocus() == child) {
        return true;
    }
    return window != nullptr && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
#else
    return window != nullptr && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
#endif
}

void Detach(SDL_Window* window) {
#ifdef _WIN32
    const HWND child = ChildHwnd(window);
    if (child && IsWindow(child)) {
        ShowWindow(child, SW_HIDE);
        SetParent(child, nullptr);
    }
#else
    (void)window;
#endif
}

} // namespace GpuiEmbed
