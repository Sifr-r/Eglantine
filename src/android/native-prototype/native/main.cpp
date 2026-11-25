// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "core_session.h"

extern "C" {
void* eglantine_android_create();
void eglantine_android_destroy(void* app);
const char* eglantine_android_title();
const char* eglantine_android_instruction();
const char* eglantine_android_picker_label();
const char* eglantine_android_launch_label();
const char* eglantine_android_status(const void* app);
bool eglantine_android_begin_selection(void* app);
void eglantine_android_select_result(void* app, int status, const char* uri_or_error);
void eglantine_android_set_suspended(void* app, bool suspended);
bool eglantine_android_can_launch(const void* app);
const char* eglantine_android_begin_launch(void* app);
void eglantine_android_launch_result(void* app, const char* error);
void eglantine_android_session_ended(void* app, const char* error);
}

namespace {

constexpr int ViewWidth = 360;
constexpr int ViewHeight = 240;
constexpr SDL_FRect PickerButton{20.0f, 80.0f, 154.0f, 48.0f};
constexpr SDL_FRect LaunchButton{186.0f, 80.0f, 154.0f, 48.0f};

using Eglantine::AndroidPrototype::CoreSession;
using Window = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>;
using Renderer = std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)>;

enum class PickerStatus : int { Cancelled = 0, Selected = 1, Error = 2 };
enum class MenuAction { None, OpenDocument, Launch };

struct PickerResult {
    PickerStatus status;
    std::array<char, 4096> text{};
};

// SDL can return a dialog result on another thread, including during shutdown.
// Keep results outside the Rust state and discard late results from a closed session.
std::mutex picker_mutex;
std::uintptr_t next_picker_token{};
std::uintptr_t active_picker_token{};
std::optional<PickerResult> picker_result;
std::atomic<bool> suspended{};

void SDLCALL OnDocumentPicked(void* userdata, const char* const* files, int) {
    const auto token = reinterpret_cast<std::uintptr_t>(userdata);
    std::scoped_lock lock{picker_mutex};
    if (token != active_picker_token) {
        return;
    }
    active_picker_token = 0;

    if (files == nullptr) {
        picker_result.emplace(PickerStatus::Error);
        SDL_strlcpy(picker_result->text.data(), SDL_GetError(), picker_result->text.size());
    } else if (files[0] == nullptr) {
        picker_result.emplace(PickerStatus::Cancelled);
    } else {
        picker_result.emplace(PickerStatus::Selected);
        auto& text = picker_result->text;
        if (SDL_strlcpy(text.data(), files[0], text.size()) >= text.size()) {
            picker_result->status = PickerStatus::Error;
            SDL_strlcpy(text.data(), "Document URI is too long.", text.size());
        }
    }
}

void BeginSelection(void* app, SDL_Window* window) {
    if (!eglantine_android_begin_selection(app)) {
        return;
    }
    std::uintptr_t token{};
    {
        std::scoped_lock lock{picker_mutex};
        token = active_picker_token = ++next_picker_token;
    }
    SDL_ShowOpenFileDialog(OnDocumentPicked, reinterpret_cast<void*>(token), window, nullptr, 0,
                           nullptr, false);
}

void DeliverSelection(void* app) {
    std::optional<PickerResult> result;
    {
        std::scoped_lock lock{picker_mutex};
        result = std::exchange(picker_result, std::nullopt);
    }
    if (!result) {
        return;
    }

    eglantine_android_select_result(app, static_cast<int>(result->status), result->text.data());
}

bool SDLCALL ObserveLifecycle(void*, SDL_Event* event) {
    switch (event->type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
        suspended.store(true, std::memory_order_relaxed);
        break;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        suspended.store(false, std::memory_order_relaxed);
        break;
    default:
        break;
    }
    return true;
}

bool DrawWrapped(SDL_Renderer* renderer, float y, std::string_view text) {
    constexpr std::size_t Columns = 40;
    while (!text.empty()) {
        std::array<char, Columns + 1> line{};
        const auto length = std::min(Columns, text.size());
        std::copy_n(text.begin(), length, line.begin());
        if (!SDL_RenderDebugText(renderer, 20.0f, y, line.data())) {
            return false;
        }
        text.remove_prefix(length);
        y += 13.0f;
    }
    return true;
}

bool Render(SDL_Renderer* renderer, const void* app) {
    const bool can_launch = eglantine_android_can_launch(app);
    return SDL_SetRenderDrawColor(renderer, 22, 27, 35, 255) && SDL_RenderClear(renderer) &&
           SDL_SetRenderDrawColor(renderer, 46, 77, 103, 255) &&
           SDL_RenderFillRect(renderer, &PickerButton) &&
           SDL_SetRenderDrawColor(renderer, can_launch ? 46 : 38, can_launch ? 77 : 44,
                                  can_launch ? 103 : 55, 255) &&
           SDL_RenderFillRect(renderer, &LaunchButton) &&
           SDL_SetRenderDrawColor(renderer, 239, 244, 247, 255) &&
           SDL_RenderDebugText(renderer, 20.0f, 25.0f, eglantine_android_title()) &&
           DrawWrapped(renderer, 47.0f, eglantine_android_instruction()) &&
           SDL_RenderDebugText(renderer, PickerButton.x + 9.0f, 100.0f,
                               eglantine_android_picker_label()) &&
           SDL_RenderDebugText(renderer, LaunchButton.x + 9.0f, 100.0f,
                               eglantine_android_launch_label()) &&
           DrawWrapped(renderer, 150.0f, eglantine_android_status(app)) &&
           SDL_RenderPresent(renderer);
}

MenuAction GetMenuAction(SDL_Renderer* renderer, SDL_Event& event) {
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        switch (event.key.key) {
        case SDLK_RETURN:
        case SDLK_SPACE:
            return MenuAction::OpenDocument;
        case SDLK_L:
            return MenuAction::Launch;
        default:
            break;
        }
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT &&
        SDL_ConvertEventToRenderCoordinates(renderer, &event)) {
        const SDL_FPoint pointer{event.button.x, event.button.y};
        if (SDL_PointInRectFloat(&pointer, &PickerButton)) {
            return MenuAction::OpenDocument;
        }
        if (SDL_PointInRectFloat(&pointer, &LaunchButton)) {
            return MenuAction::Launch;
        }
    }
    return MenuAction::None;
}

class GameTouches {
public:
    [[nodiscard]] bool Handle(CoreSession& session, const SDL_Event& event) {
        const auto type = event.type;
        if (type != SDL_EVENT_FINGER_DOWN && type != SDL_EVENT_FINGER_MOTION &&
            type != SDL_EVENT_FINGER_UP && type != SDL_EVENT_FINGER_CANCELED) {
            return false;
        }
        const Touch touch{event.tfinger.touchID, event.tfinger.fingerID};
        auto slot = std::find(touches.begin(), touches.end(), touch);
        if (type == SDL_EVENT_FINGER_DOWN && slot == touches.end()) {
            slot = std::find(touches.begin(), touches.end(), std::nullopt);
            if (slot != touches.end()) {
                *slot = touch;
                if (std::count_if(touches.begin(), touches.end(),
                                  [](const auto& entry) { return entry.has_value(); }) >= 3) {
                    return true;
                }
                session.TouchDown(Index(slot), event.tfinger.x, event.tfinger.y);
            }
        } else if (slot != touches.end()) {
            if (type == SDL_EVENT_FINGER_MOTION) {
                session.TouchMove(Index(slot), event.tfinger.x, event.tfinger.y);
            } else if (type == SDL_EVENT_FINGER_UP || type == SDL_EVENT_FINGER_CANCELED) {
                session.TouchUp(Index(slot));
                slot->reset();
            }
        }
        return false;
    }

    void Release(CoreSession& session) {
        for (std::size_t index = 0; index < touches.size(); ++index) {
            if (touches[index]) {
                session.TouchUp(static_cast<int>(index));
                touches[index].reset();
            }
        }
    }

private:
    using Touch = std::pair<SDL_TouchID, SDL_FingerID>;
    using Slots = std::array<std::optional<Touch>, 3>;

    int Index(Slots::const_iterator slot) const {
        return static_cast<int>(slot - touches.cbegin());
    }

    Slots touches{};
};

Window CreateWindow(SDL_WindowFlags flags = 0) {
    return {SDL_CreateWindow(eglantine_android_title(), ViewWidth, ViewHeight,
                             flags | SDL_WINDOW_HIGH_PIXEL_DENSITY),
            SDL_DestroyWindow};
}

Renderer CreateRenderer(SDL_Window* window) {
    Renderer renderer{SDL_CreateRenderer(window, nullptr), SDL_DestroyRenderer};
    if (renderer && !SDL_SetRenderLogicalPresentation(renderer.get(), ViewWidth, ViewHeight,
                                                       SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
        renderer.reset();
    }
    return renderer;
}

void Launch(CoreSession& session, Window& window, Renderer& renderer, void* app) {
    const char* uri = eglantine_android_begin_launch(app);
    if (uri == nullptr) {
        return;
    }

    renderer.reset();
    window.reset();
    window = CreateWindow(SDL_WINDOW_VULKAN);
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    const auto error = window ? session.Launch(window.get(), uri) : "Could not create a game window.";
    if (!error.empty()) {
        window.reset();
        SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "0");
    }
    eglantine_android_launch_result(app, error.empty() ? nullptr : error.c_str());
}

void Stop(CoreSession& session, GameTouches& touches, Window& window, void* app,
          const char* error = nullptr) {
    touches.Release(session);
    session.Stop();
    window.reset();
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "0");
    eglantine_android_session_ended(app, error);
}

bool IsStopEvent(const SDL_Event& event) {
    return event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
           (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_AC_BACK);
}

void RunApp(Window window, void* app) {
    Renderer renderer{nullptr, SDL_DestroyRenderer};
    CoreSession session;
    GameTouches touches;
    bool last_suspended = false;
    bool last_render_ok = true;
    for (;;) {
        SDL_Event event{};
        const bool has_event = SDL_WaitEventTimeout(&event, last_suspended || !renderer ? 50 : 16);
        if (has_event && event.type == SDL_EVENT_QUIT) {
            break;
        }

        const bool is_suspended = suspended.load(std::memory_order_relaxed);
        const bool suspension_changed = is_suspended != last_suspended;
        if (suspension_changed) {
            eglantine_android_set_suspended(app, is_suspended);
            last_suspended = is_suspended;
        }

        if (session.IsRunning()) {
            if (session.ExitRequested() || (has_event && IsStopEvent(event))) {
                Stop(session, touches, window, app);
            } else if (is_suspended && suspension_changed) {
                touches.Release(session);
                session.Pause();
            } else if (!is_suspended &&
                       (suspension_changed ||
                        (has_event && event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED))) {
                const auto error = session.Resume(window.get());
                if (!error.empty()) {
                    Stop(session, touches, window, app, error.c_str());
                }
            } else if (!is_suspended && has_event && touches.Handle(session, event)) {
                Stop(session, touches, window, app);
            }
            continue;
        }
        if (is_suspended) {
            continue;
        }

        DeliverSelection(app);
        if (!window) {
            window = CreateWindow();
        }
        if (window && !renderer) {
            renderer = CreateRenderer(window.get());
        }
        if (has_event && renderer) {
            switch (GetMenuAction(renderer.get(), event)) {
            case MenuAction::OpenDocument:
                BeginSelection(app, window.get());
                break;
            case MenuAction::Launch:
                Launch(session, window, renderer, app);
                continue;
            case MenuAction::None:
                break;
            }
        }

        const bool rendered = renderer && Render(renderer.get(), app);
        if (!rendered && last_render_ok) {
            SDL_LogError(SDL_LOG_CATEGORY_RENDER, "Android prototype render: %s", SDL_GetError());
        }
        last_render_ok = rendered;
    }
    if (session.IsRunning()) {
        Stop(session, touches, window, app);
    }
}

int RunPrototype() {
    auto window = CreateWindow();
    if (!window) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Create Android prototype window: %s",
                     SDL_GetError());
        return 1;
    }
    const std::unique_ptr<void, decltype(&eglantine_android_destroy)> app{
        eglantine_android_create(), eglantine_android_destroy};
    if (!app) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Create Android prototype state");
        return 1;
    }
    if (!SDL_AddEventWatch(ObserveLifecycle, nullptr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Configure Android prototype window: %s",
                     SDL_GetError());
        return 1;
    }

    RunApp(std::move(window), app.get());
    SDL_RemoveEventWatch(ObserveLifecycle, nullptr);
    {
        std::scoped_lock lock{picker_mutex};
        active_picker_token = 0;
        picker_result.reset();
    }
    return 0;
}

} // namespace

int main(int, char**) {
    suspended.store(false, std::memory_order_relaxed);
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Initialize SDL for Android prototype: %s",
                     SDL_GetError());
        return 1;
    }
    const int result = RunPrototype();
    SDL_Quit();
    return result;
}
