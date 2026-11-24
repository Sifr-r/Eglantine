// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace Core {
class System;
}

namespace Gpui {

// The private content host calls this before opening an emulation session.
// An empty result means the operation succeeded.
std::string RunContentCommand(Core::System& system, std::string_view operation,
                              std::string_view path_utf8);

} // namespace Gpui
