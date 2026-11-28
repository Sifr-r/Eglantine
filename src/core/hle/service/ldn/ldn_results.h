// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/result.h"

namespace Service::LDN {

// Error codes for nn::ldn (Horizon OS error module 203).
//
// The published codes below are cross-checked against the Nintendo Switch SDK
// 15.3.2 (February 2023) header nn/ldn/ldn_Result.public.h. Nintendo generates
// that header from the service's internal error table and publishes the ranges
// the library can return:
//
//   16..31    communication device and radio availability
//   32..47    invalid library state
//   48..63    node lookup
//   64..79    connection
//   80..95    cancellation
//  112..127   local communication version compatibility
//  144..159   node count limits
constexpr Result ResultDeviceNotAvailable{ErrorModule::LDN, 16};
constexpr Result ResultDeviceOccupied{ErrorModule::LDN, 17};
constexpr Result ResultDeviceDisabled{ErrorModule::LDN, 22};
constexpr Result ResultWifiOff{ErrorModule::LDN, 23};
constexpr Result ResultSleep{ErrorModule::LDN, 24};
constexpr Result ResultDeviceNotFound{ErrorModule::LDN, 25};
constexpr Result ResultInvalidState{ErrorModule::LDN, 32};
constexpr Result ResultNodeNotFound{ErrorModule::LDN, 48};
constexpr Result ResultConnectionFailed{ErrorModule::LDN, 64};
constexpr Result ResultConnectionTimeout{ErrorModule::LDN, 66};
constexpr Result ResultConnectionRejected{ErrorModule::LDN, 67};
constexpr Result ResultInvalidNetwork{ErrorModule::LDN, 68};
constexpr Result ResultCancelled{ErrorModule::LDN, 80};
constexpr Result ResultLowerVersion{ErrorModule::LDN, 113};
constexpr Result ResultHigherVersion{ErrorModule::LDN, 114};
constexpr Result ResultLowerVersionCannotUpdate{ErrorModule::LDN, 115};
constexpr Result ResultHigherVersionCannotUpdate{ErrorModule::LDN, 116};
constexpr Result ResultNodeCountLimitation{ErrorModule::LDN, 144};

// Conditions the SDK does not publish a code for. They keep the values this
// service has always returned, which the SDK table does not contradict.
// Slot 65 is the SDK's ResultNetworkNotFound.
constexpr Result ResultAdvertiseDataTooLarge{ErrorModule::LDN, 10};
constexpr Result ResultNoIpAddress{ErrorModule::LDN, 33};
constexpr Result ResultInvalidBufferCount{ErrorModule::LDN, 50};
constexpr Result ResultAccessPointConnectionFailed{ErrorModule::LDN, 65};
constexpr Result ResultBadInput{ErrorModule::LDN, 96};

} // namespace Service::LDN
