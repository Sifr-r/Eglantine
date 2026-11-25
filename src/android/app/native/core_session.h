// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "common/common_types.h"

struct SDL_Window;

namespace Eglantine::AndroidPrototype {

struct AndroidSettingsSnapshot {
    bool use_docked_mode{};
    bool async_gpu{true};
    bool disk_shader_cache{true};
    bool use_vsync{true};
    int resolution_index{2};
    int gpu_accuracy{0};
    std::string driver_name;
};

// Access from the SDL event thread. A launch or resume succeeds when it returns an empty string.
class CoreSession {
public:
    CoreSession();
    ~CoreSession();

    CoreSession(const CoreSession&) = delete;
    CoreSession& operator=(const CoreSession&) = delete;

    [[nodiscard]] std::string EnsureStorageReady();
    [[nodiscard]] std::string GetUserDataDir() const;
    [[nodiscard]] bool HasProdKeys() const;
    [[nodiscard]] std::string InstallKeysDocument(std::string_view uri);
    [[nodiscard]] AndroidSettingsSnapshot GetSettingsSnapshot() const;
    void ApplySettingsSnapshot(const AndroidSettingsSnapshot& snapshot);
    [[nodiscard]] std::string InstallDriverDocument(std::string_view uri);
    void ClearCustomDriver();

    [[nodiscard]] std::string Launch(SDL_Window* window, std::string_view uri);
    void Pause();
    [[nodiscard]] std::string Resume(SDL_Window* window);
    void Stop();
    [[nodiscard]] bool IsRunning() const;
    [[nodiscard]] bool ExitRequested() const;

    void TouchDown(int id, float x, float y);
    void TouchMove(int id, float x, float y);
    void TouchUp(int id);

    void SetVirtualButton(int button_id, bool pressed);
    void SetVirtualStick(int stick_id, float x, float y);
    void SetVirtualMotion(u64 delta_timestamp_us, float gyro_x, float gyro_y, float gyro_z,
                          float accel_x, float accel_y, float accel_z);
    void ResetVirtualGamepad();

    [[nodiscard]] double GetCurrentFps();
    [[nodiscard]] int GetShadersBuilding() const;

private:
    struct State;
    std::unique_ptr<State> state;
};

} // namespace Eglantine::AndroidPrototype
