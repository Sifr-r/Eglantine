// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>

namespace Core {
class System;
}

namespace Gpui {

class Session;

bool ScanLibrary(Session& session, Core::System& system, const std::filesystem::path& directory,
                 bool deep);

} // namespace Gpui
