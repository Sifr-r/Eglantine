// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "yuzu/migration_worker.h"

class UserDataMigrator {
public:
    static bool MigrateIfNeeded();

private:
    static bool ShowMigrationPrompt();
    static void ShowMigrationCancelledMessage();
    static bool MigrateUserData(Emulator selected_emu, bool clear_shader_cache,
                                MigrationWorker::MigrationStrategy strategy);
};
