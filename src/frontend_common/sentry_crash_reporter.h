// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace Frontend {

enum class SentryFrontend { Qt, Sdl };

class SentryCrashReporter {
public:
    SentryCrashReporter() = default;
    ~SentryCrashReporter();

    SentryCrashReporter(const SentryCrashReporter&) = delete;
    SentryCrashReporter& operator=(const SentryCrashReporter&) = delete;

    void Start(SentryFrontend frontend);
    [[nodiscard]] bool IsActive() const;

private:
    bool active{};
};

} // namespace Frontend
