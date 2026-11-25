// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <string>
#include <string_view>

struct SDL_Window;

namespace Eglantine::AndroidPrototype {

// Access from the SDL event thread. A launch or resume succeeds when it returns an empty string.
class CoreSession {
public:
    CoreSession();
    ~CoreSession();

    CoreSession(const CoreSession&) = delete;
    CoreSession& operator=(const CoreSession&) = delete;

    [[nodiscard]] std::string Launch(SDL_Window* window, std::string_view uri);
    void Pause();
    [[nodiscard]] std::string Resume(SDL_Window* window);
    void Stop();
    [[nodiscard]] bool IsRunning() const;
    [[nodiscard]] bool ExitRequested() const;

    void TouchDown(int id, float x, float y);
    void TouchMove(int id, float x, float y);
    void TouchUp(int id);

private:
    struct State;
    std::unique_ptr<State> state;
};

} // namespace Eglantine::AndroidPrototype
