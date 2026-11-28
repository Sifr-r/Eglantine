// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/hle/result.h"

namespace Service::NGC {

constexpr Result ResultNotInitialized{ErrorModule::NGC, 1};
constexpr Result ResultAlreadyInitialized{ErrorModule::NGC, 2};
constexpr Result ResultInvalidPointer{ErrorModule::NGC, 3};
constexpr Result ResultInvalidSize{ErrorModule::NGC, 4};
constexpr Result ResultInvalidCharacter{ErrorModule::NGC, 5};

} // namespace Service::NGC
