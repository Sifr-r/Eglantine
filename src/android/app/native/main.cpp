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
#include <unordered_map>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "core_session.h"
#include "document_vfs.h"
#include "eglantine_android.h"

namespace {

using Eglantine::AndroidPrototype::CoreSession;
using Eglantine::AndroidPrototype::ScanDocumentTree;
using Eglantine::AndroidPrototype::TakePersistableReadPermission;
using Window = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>;
using Renderer = std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)>;

enum class PickerStatus : int { Cancelled = 0, Selected = 1, Error = 2 };
enum class PickerKind { GameFile, GameFolder, KeysFile, DriverFile };

struct PickerResult {
    PickerKind kind;
    PickerStatus status;
    std::array<char, 4096> text{};
};

// SDL can return a dialog result on another thread, including during shutdown.
// Keep results outside the Rust state and discard late results from a closed session.
std::mutex picker_mutex;
std::uintptr_t next_picker_token{};
std::uintptr_t active_picker_token{};
PickerKind active_picker_kind{PickerKind::GameFile};
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
        picker_result.emplace(PickerResult{active_picker_kind, PickerStatus::Error});
        SDL_strlcpy(picker_result->text.data(), SDL_GetError(), picker_result->text.size());
    } else if (files[0] == nullptr) {
        picker_result.emplace(PickerResult{active_picker_kind, PickerStatus::Cancelled});
    } else {
        picker_result.emplace(PickerResult{active_picker_kind, PickerStatus::Selected});
        auto& text = picker_result->text;
        if (SDL_strlcpy(text.data(), files[0], text.size()) >= text.size()) {
            picker_result->status = PickerStatus::Error;
            SDL_strlcpy(text.data(), "Document URI is too long.", text.size());
        }
    }
}

std::uintptr_t ArmPickerToken(PickerKind kind) {
    std::scoped_lock lock{picker_mutex};
    active_picker_kind = kind;
    return active_picker_token = ++next_picker_token;
}

void BeginSelection(void* app, SDL_Window* window) {
    if (!eglantine_android_begin_selection(app)) {
        return;
    }
    const auto token = ArmPickerToken(PickerKind::GameFile);
    SDL_ShowOpenFileDialog(OnDocumentPicked, reinterpret_cast<void*>(token), window, nullptr, 0,
                           nullptr, false);
}

void BeginFolderSelection(void* app, SDL_Window* window) {
    if (!eglantine_android_begin_folder_selection(app)) {
        return;
    }
    const auto token = ArmPickerToken(PickerKind::GameFolder);
    SDL_ShowOpenFolderDialog(OnDocumentPicked, reinterpret_cast<void*>(token), window, nullptr,
                             false);
}

void BeginKeysSelection(void* app, SDL_Window* window) {
    if (!eglantine_android_begin_keys_selection(app)) {
        return;
    }
    const auto token = ArmPickerToken(PickerKind::KeysFile);
    SDL_ShowOpenFileDialog(OnDocumentPicked, reinterpret_cast<void*>(token), window, nullptr, 0,
                           nullptr, false);
}

void BeginDriverSelection(void* app, SDL_Window* window) {
    if (!eglantine_android_begin_driver_selection(app)) {
        return;
    }
    const auto token = ArmPickerToken(PickerKind::DriverFile);
    SDL_ShowOpenFileDialog(OnDocumentPicked, reinterpret_cast<void*>(token), window, nullptr, 0,
                           nullptr, false);
}

void SyncSettingsToRust(const CoreSession& session, void* app) {
    const auto snapshot = session.GetSettingsSnapshot();
    const EglAndroidSettings rust_settings{
        .use_docked_mode = snapshot.use_docked_mode,
        .async_gpu = snapshot.async_gpu,
        .disk_shader_cache = snapshot.disk_shader_cache,
        .use_vsync = snapshot.use_vsync,
        .resolution_index = snapshot.resolution_index,
        .gpu_accuracy = snapshot.gpu_accuracy,
    };
    eglantine_android_set_settings(app, rust_settings, snapshot.driver_name.c_str());
}

void DeliverSelection(void* app, CoreSession& session) {
    std::optional<PickerResult> result;
    {
        std::scoped_lock lock{picker_mutex};
        result = std::exchange(picker_result, std::nullopt);
    }
    if (!result) {
        return;
    }

    switch (result->kind) {
    case PickerKind::GameFile:
        eglantine_android_select_result(app, static_cast<int>(result->status), result->text.data());
        break;
    case PickerKind::GameFolder:
        if (result->status == PickerStatus::Cancelled) {
            eglantine_android_folder_scan_result(app, nullptr, nullptr, 0, nullptr);
        } else if (result->status == PickerStatus::Error) {
            eglantine_android_folder_scan_result(app, nullptr, nullptr, 0, result->text.data());
        } else {
            TakePersistableReadPermission(result->text.data());
            const auto scan = ScanDocumentTree(result->text.data());
            std::vector<EglAndroidGameEntry> entries;
            entries.reserve(scan.games.size());
            for (const auto& game : scan.games) {
                entries.push_back(EglAndroidGameEntry{
                    .uri = game.uri.c_str(),
                    .title = game.title.c_str(),
                    .program_id = game.program_id,
                });
            }
            eglantine_android_folder_scan_result(
                app, result->text.data(), entries.data(), entries.size(),
                scan.error.empty() ? nullptr : scan.error.c_str());
        }
        break;
    case PickerKind::KeysFile:
        if (result->status == PickerStatus::Cancelled) {
            eglantine_android_keys_install_result(app, "Selection cancelled.",
                                                  session.HasProdKeys());
        } else if (result->status == PickerStatus::Error) {
            eglantine_android_keys_install_result(app, result->text.data(), session.HasProdKeys());
        } else {
            const auto error = session.InstallKeysDocument(result->text.data());
            eglantine_android_keys_install_result(app, error.empty() ? nullptr : error.c_str(),
                                                  session.HasProdKeys());
        }
        break;
    case PickerKind::DriverFile:
        if (result->status == PickerStatus::Cancelled) {
            const auto snapshot = session.GetSettingsSnapshot();
            eglantine_android_driver_install_result(app, "Selection cancelled.",
                                                    snapshot.driver_name.c_str());
        } else if (result->status == PickerStatus::Error) {
            const auto snapshot = session.GetSettingsSnapshot();
            eglantine_android_driver_install_result(app, result->text.data(),
                                                    snapshot.driver_name.c_str());
        } else {
            const auto error = session.InstallDriverDocument(result->text.data());
            const auto snapshot = session.GetSettingsSnapshot();
            eglantine_android_driver_install_result(app, error.empty() ? nullptr : error.c_str(),
                                                    snapshot.driver_name.c_str());
        }
        break;
    }
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

bool Render(SDL_Renderer* renderer, void* app) {
    const EglAndroidDrawCmd* cmds = nullptr;
    const std::size_t count = eglantine_android_build_draw_list(app, &cmds);
    if (count == 0 || cmds == nullptr) {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto& cmd = cmds[i];
        if (!SDL_SetRenderDrawColor(renderer, cmd.color.r, cmd.color.g, cmd.color.b, cmd.color.a)) {
            return false;
        }
        if (cmd.kind == EGL_ANDROID_DRAW_FILL_RECT) {
            if (i == 0 && !SDL_RenderClear(renderer)) {
                return false;
            }
            const SDL_FRect rect{cmd.x, cmd.y, cmd.w, cmd.h};
            if (!SDL_RenderFillRect(renderer, &rect)) {
                return false;
            }
        } else if (cmd.kind == EGL_ANDROID_DRAW_TEXT && cmd.text != nullptr) {
            if (!SDL_RenderDebugText(renderer, cmd.x, cmd.y, cmd.text)) {
                return false;
            }
        }
    }
    return SDL_RenderPresent(renderer);
}

int32_t GetMenuAction(SDL_Renderer* renderer, void* app, SDL_Event& event) {
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        switch (event.key.key) {
        case SDLK_RETURN:
        case SDLK_SPACE:
            return eglantine_android_key_down(app, EGL_ANDROID_KEY_CONFIRM);
        case SDLK_L:
            return eglantine_android_key_down(app, EGL_ANDROID_KEY_LAUNCH);
        default:
            break;
        }
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT &&
        SDL_ConvertEventToRenderCoordinates(renderer, &event)) {
        return eglantine_android_pointer_up(app, event.button.x, event.button.y);
    }
    return EGL_ANDROID_ACTION_NONE;
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

class GamepadHost {
public:
    void HandleEvent(CoreSession& session, void* app, const SDL_Event& event) {
        switch (event.type) {
        case SDL_EVENT_GAMEPAD_ADDED: {
            if (auto* pad = SDL_OpenGamepad(event.gdevice.which)) {
                if (SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) {
                    SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
                }
                if (SDL_GamepadHasSensor(pad, SDL_SENSOR_ACCEL)) {
                    SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_ACCEL, true);
                }
                pads.insert_or_assign(event.gdevice.which, PadState{GamepadPtr{pad, SDL_CloseGamepad}});
                UpdateOverlayControllerState(session, app);
            }
            break;
        }
        case SDL_EVENT_GAMEPAD_REMOVED: {
            pads.erase(event.gdevice.which);
            if (pads.empty()) {
                session.ResetVirtualGamepad();
            }
            UpdateOverlayControllerState(session, app);
            break;
        }
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP: {
            if (const auto id = MapButton(static_cast<SDL_GamepadButton>(event.gbutton.button))) {
                session.SetVirtualButton(*id, event.gbutton.down);
            }
            break;
        }
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
            auto it = pads.find(event.gaxis.which);
            if (it == pads.end()) {
                break;
            }
            auto& state = it->second;
            const float norm = std::clamp(static_cast<float>(event.gaxis.value) / 32767.0f, -1.0f, 1.0f);
            switch (static_cast<SDL_GamepadAxis>(event.gaxis.axis)) {
            case SDL_GAMEPAD_AXIS_LEFTX:
                state.left_x = norm;
                session.SetVirtualStick(0, state.left_x, state.left_y);
                break;
            case SDL_GAMEPAD_AXIS_LEFTY:
                state.left_y = -norm;
                session.SetVirtualStick(0, state.left_x, state.left_y);
                break;
            case SDL_GAMEPAD_AXIS_RIGHTX:
                state.right_x = norm;
                session.SetVirtualStick(1, state.right_x, state.right_y);
                break;
            case SDL_GAMEPAD_AXIS_RIGHTY:
                state.right_y = -norm;
                session.SetVirtualStick(1, state.right_x, state.right_y);
                break;
            case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: {
                const bool pressed = event.gaxis.value > 8192;
                if (pressed != state.zl_down) {
                    state.zl_down = pressed;
                    session.SetVirtualButton(8, pressed);
                }
                break;
            }
            case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: {
                const bool pressed = event.gaxis.value > 8192;
                if (pressed != state.zr_down) {
                    state.zr_down = pressed;
                    session.SetVirtualButton(9, pressed);
                }
                break;
            }
            default:
                break;
            }
            break;
        }
        case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: {
            auto it = pads.find(event.gsensor.which);
            if (it == pads.end()) {
                break;
            }
            auto& state = it->second;
            if (event.gsensor.sensor == SDL_SENSOR_GYRO) {
                std::copy_n(event.gsensor.data, 3, state.gyro.begin());
            } else if (event.gsensor.sensor == SDL_SENSOR_ACCEL) {
                std::copy_n(event.gsensor.data, 3, state.accel.begin());
            }
            const Uint64 ts = event.gsensor.sensor_timestamp;
            const Uint64 delta_us =
                (state.last_sensor_ns != 0 && ts > state.last_sensor_ns)
                    ? (ts - state.last_sensor_ns) / 1000
                    : 0;
            state.last_sensor_ns = ts;
            session.SetVirtualMotion(static_cast<u64>(delta_us), state.gyro[0], state.gyro[1],
                                     state.gyro[2], state.accel[0], state.accel[1], state.accel[2]);
            break;
        }
        default:
            break;
        }
    }

private:
    using GamepadPtr = std::unique_ptr<SDL_Gamepad, decltype(&SDL_CloseGamepad)>;

    struct PadState {
        GamepadPtr handle;
        float left_x{};
        float left_y{};
        float right_x{};
        float right_y{};
        bool zl_down{};
        bool zr_down{};
        std::array<float, 3> gyro{};
        std::array<float, 3> accel{};
        Uint64 last_sensor_ns{};
    };

    void UpdateOverlayControllerState(CoreSession& session, void* app) const {
        if (eglantine_android_overlay_set_controller_active(app, !pads.empty())) {
            session.ResetVirtualGamepad();
        }
    }

    static std::optional<int> MapButton(SDL_GamepadButton button) {
        switch (button) {
        case SDL_GAMEPAD_BUTTON_SOUTH:
            return 0;
        case SDL_GAMEPAD_BUTTON_EAST:
            return 1;
        case SDL_GAMEPAD_BUTTON_WEST:
            return 2;
        case SDL_GAMEPAD_BUTTON_NORTH:
            return 3;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK:
            return 4;
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
            return 5;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
            return 6;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
            return 7;
        case SDL_GAMEPAD_BUTTON_START:
            return 10;
        case SDL_GAMEPAD_BUTTON_BACK:
            return 11;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
            return 12;
        case SDL_GAMEPAD_BUTTON_DPAD_UP:
            return 13;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
            return 14;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
            return 15;
        case SDL_GAMEPAD_BUTTON_MISC1:
            return 19;
        default:
            return std::nullopt;
        }
    }

    std::unordered_map<SDL_JoystickID, PadState> pads;
};

bool HandleGameplayTouch(CoreSession& session, GameTouches& touches, void* app,
                         const SDL_Event& event) {
    const auto type = event.type;
    if (type != SDL_EVENT_FINGER_DOWN && type != SDL_EVENT_FINGER_MOTION &&
        type != SDL_EVENT_FINGER_UP && type != SDL_EVENT_FINGER_CANCELED) {
        return false;
    }
    const auto finger = static_cast<int64_t>(event.tfinger.fingerID);
    EglAndroidOverlayEvent overlay_ev{};
    if (type == SDL_EVENT_FINGER_DOWN) {
        overlay_ev = eglantine_android_overlay_touch_down(app, finger, event.tfinger.x,
                                                          event.tfinger.y);
    } else if (type == SDL_EVENT_FINGER_MOTION) {
        overlay_ev = eglantine_android_overlay_touch_move(app, finger, event.tfinger.x,
                                                          event.tfinger.y);
    } else {
        overlay_ev = eglantine_android_overlay_touch_up(app, finger);
    }

    if (overlay_ev.consumed) {
        switch (overlay_ev.kind) {
        case EGL_ANDROID_OVERLAY_BUTTON:
            session.SetVirtualButton(overlay_ev.id, overlay_ev.pressed);
            break;
        case EGL_ANDROID_OVERLAY_STICK:
            session.SetVirtualStick(overlay_ev.id, overlay_ev.x, overlay_ev.y);
            break;
        case EGL_ANDROID_OVERLAY_PAUSE_MENU:
            return true;
        default:
            break;
        }
        return false;
    }
    return touches.Handle(session, event);
}

Window CreateWindow(SDL_WindowFlags flags = 0) {
    int32_t width = 0;
    int32_t height = 0;
    eglantine_android_viewport_size(&width, &height);
    return {SDL_CreateWindow(eglantine_android_title(), width, height,
                             flags | SDL_WINDOW_HIGH_PIXEL_DENSITY),
            SDL_DestroyWindow};
}

Renderer CreateRenderer(SDL_Window* window) {
    int32_t width = 0;
    int32_t height = 0;
    eglantine_android_viewport_size(&width, &height);
    Renderer renderer{SDL_CreateRenderer(window, nullptr), SDL_DestroyRenderer};
    if (renderer && !SDL_SetRenderLogicalPresentation(renderer.get(), width, height,
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
    eglantine_android_overlay_reset(app);
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
    if (session.EnsureStorageReady().empty()) {
        eglantine_android_set_storage_dir(app, session.GetUserDataDir().c_str());
        eglantine_android_set_has_prod_keys(app, session.HasProdKeys());
        SyncSettingsToRust(session, app);
    }
    GameTouches touches;
    GamepadHost gamepads;
    bool last_suspended = false;
    bool last_render_ok = true;
    for (;;) {
        SDL_Event event{};
        const bool has_event = SDL_WaitEventTimeout(&event, last_suspended || !renderer ? 50 : 16);
        if (has_event && event.type == SDL_EVENT_QUIT) {
            break;
        }
        if (has_event) {
            gamepads.HandleEvent(session, app, event);
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
                eglantine_android_overlay_reset(app);
                session.Pause();
            } else if (!is_suspended &&
                       (suspension_changed ||
                        (has_event && (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
                                       event.type == SDL_EVENT_WINDOW_RESTORED)))) {
                const auto error = session.Resume(window.get());
                if (!error.empty()) {
                    Stop(session, touches, window, app, error.c_str());
                }
            } else if (!is_suspended && has_event &&
                       HandleGameplayTouch(session, touches, app, event)) {
                Stop(session, touches, window, app);
            }
            continue;
        }
        if (is_suspended) {
            continue;
        }

        DeliverSelection(app, session);
        if (!window) {
            window = CreateWindow();
        }
        if (window && !renderer) {
            renderer = CreateRenderer(window.get());
        }
        if (has_event && renderer) {
            switch (GetMenuAction(renderer.get(), app, event)) {
            case EGL_ANDROID_ACTION_OPEN_DOCUMENT:
                BeginSelection(app, window.get());
                break;
            case EGL_ANDROID_ACTION_ADD_FOLDER:
                BeginFolderSelection(app, window.get());
                break;
            case EGL_ANDROID_ACTION_INSTALL_KEYS:
                BeginKeysSelection(app, window.get());
                break;
            case EGL_ANDROID_ACTION_INSTALL_DRIVER:
                BeginDriverSelection(app, window.get());
                break;
            case EGL_ANDROID_ACTION_CLEAR_DRIVER:
                session.ClearCustomDriver();
                SyncSettingsToRust(session, app);
                break;
            case EGL_ANDROID_ACTION_APPLY_SETTINGS: {
                const auto s = eglantine_android_get_settings(app);
                session.ApplySettingsSnapshot({
                    .use_docked_mode = s.use_docked_mode,
                    .async_gpu = s.async_gpu,
                    .disk_shader_cache = s.disk_shader_cache,
                    .use_vsync = s.use_vsync,
                    .resolution_index = s.resolution_index,
                    .gpu_accuracy = s.gpu_accuracy,
                });
                break;
            }
            case EGL_ANDROID_ACTION_LAUNCH:
                Launch(session, window, renderer, app);
                continue;
            default:
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
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_SENSOR)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Initialize SDL for Android prototype: %s",
                     SDL_GetError());
        return 1;
    }
    const int result = RunPrototype();
    SDL_Quit();
    return result;
}
