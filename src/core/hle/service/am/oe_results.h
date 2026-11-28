// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/hle/result.h"

namespace Service::OE {

constexpr Result ResultInvalidConfiguration{ErrorModule::OE, 1};
constexpr Result ResultNotAllowed{ErrorModule::OE, 11};
constexpr Result ResultInternalError{ErrorModule::OE, 1023};
constexpr Result ResultUnknown{ErrorModule::OE, 1024};

} // namespace Service::OE
