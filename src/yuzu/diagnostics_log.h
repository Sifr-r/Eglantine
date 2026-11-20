// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

namespace DiagnosticsLog {

struct Snapshot {
    QString gpu;
    QString driver;
    QString log_tail;
    QString recent_errors;
    QString actionable_items;
    bool readable = false;
    bool truncated = false;
};

[[nodiscard]] Snapshot Read(const QString& path);

} // namespace DiagnosticsLog
