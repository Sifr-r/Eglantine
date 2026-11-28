// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/result.h"

namespace Service::Account {

// The published codes below are cross-checked against the Nintendo Switch SDK
// 15.3.2 (February 2023) header nn/account/account_Result.public.h, which
// Nintendo generates from the service's internal error table.
constexpr Result ResultCancelled{ErrorModule::Account, 0};
constexpr Result ResultCancelledByUser{ErrorModule::Account, 1};
constexpr Result ResultUserNotExist{ErrorModule::Account, 100};
constexpr Result ResultNetworkServiceAccountUnavailable{ErrorModule::Account, 200};
constexpr Result ResultNintendoAccountAuthorizationInteractionRequired{ErrorModule::Account, 280};
constexpr Result ResultTokenCacheUnavailable{ErrorModule::Account, 430};
constexpr Result ResultNetworkCommunicationError{ErrorModule::Account, 3000};

// Codes for conditions the SDK does not publish. They keep the values this
// service has always returned, which the SDK table does not contradict.
constexpr Result ResultNoNotifications{ErrorModule::Account, 15};
constexpr Result ResultInvalidUserId{ErrorModule::Account, 20};
constexpr Result ResultInvalidApplication{ErrorModule::Account, 22};
constexpr Result ResultNullptr{ErrorModule::Account, 30};
constexpr Result ResultInvalidArrayLength{ErrorModule::Account, 32};
constexpr Result ResultApplicationInfoAlreadyInitialized{ErrorModule::Account, 41};

} // namespace Service::Account
