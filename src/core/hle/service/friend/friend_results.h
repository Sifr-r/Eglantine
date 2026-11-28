// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/hle/result.h"

namespace Service::Friend {

constexpr Result ResultNotInitialized{ErrorModule::Friends, 1};
constexpr Result ResultInvalidArgument{ErrorModule::Friends, 2};
constexpr Result ResultUserNotOpened{ErrorModule::Friends, 3};
constexpr Result ResultNetworkServiceAccountNotLinked{ErrorModule::Friends, 4};
constexpr Result ResultOwnNetworkServiceAccountSpecified{ErrorModule::Friends, 5};
constexpr Result ResultInternetRequestNotAccepted{ErrorModule::Friends, 6};
constexpr Result ResultNotCalled{ErrorModule::Friends, 7};
constexpr Result ResultCallInProgress{ErrorModule::Friends, 8};
constexpr Result ResultCanceled{ErrorModule::Friends, 9};
constexpr Result ResultProfileImageCacheNotFound{ErrorModule::Friends, 10};
constexpr Result ResultOutOfMemory{ErrorModule::Friends, 11};
constexpr Result ResultOutOfResource{ErrorModule::Friends, 12};
constexpr Result ResultReservedKey{ErrorModule::Friends, 13};
constexpr Result ResultDuplicatedKey{ErrorModule::Friends, 14};
constexpr Result ResultNotificationNotFound{ErrorModule::Friends, 15};
constexpr Result ResultPlayHistoryRegistrationKeyBroken{ErrorModule::Friends, 21};
constexpr Result ResultOwnPlayHistoryRegistrationKey{ErrorModule::Friends, 22};
constexpr Result ResultAppletCanceled{ErrorModule::Friends, 30};
constexpr Result ResultApplicationInfoNotRegistered{ErrorModule::Friends, 40};
constexpr Result ResultNotPermitted{ErrorModule::Friends, 90};
constexpr Result ResultInvalidOperation{ErrorModule::Friends, 92};
constexpr Result ResultNotImplemented{ErrorModule::Friends, 99};
constexpr Result ResultResponseFormatError{ErrorModule::Friends, 100};
constexpr Result ResultHttpError{ErrorModule::Friends, 1000};
constexpr Result ResultServerError{ErrorModule::Friends, 2000};

} // namespace Service::Friend
