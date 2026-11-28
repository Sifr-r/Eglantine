// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace Gpui {

class Session;

/// Runs the native Switch 2 controller bridge for the GPUI shell until stdin closes or `stop` is
/// received, emitting `controller` and `controller_input` events.
bool RunControllerBridge(Session& session);

} // namespace Gpui
