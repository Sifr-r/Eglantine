// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "core/hle/result.h"

namespace Service::PlayReport {

constexpr Result ResultInvalidArgument{ErrorModule::PlayReport, 1};
constexpr Result ResultInvalidKey{ErrorModule::PlayReport, 2};
constexpr Result ResultDuplicatedKey{ErrorModule::PlayReport, 3};
constexpr Result ResultInvalidValue{ErrorModule::PlayReport, 4};
constexpr Result ResultInvalidEventId{ErrorModule::PlayReport, 5};
constexpr Result ResultOutOfResource{ErrorModule::PlayReport, 6};
constexpr Result ResultKeyValueCountLimitReached{ErrorModule::PlayReport, 7};
constexpr Result ResultInvalidApplicationId{ErrorModule::PlayReport, 8};
constexpr Result ResultInvalidReportData{ErrorModule::PlayReport, 9};
constexpr Result ResultTooBigReportData{ErrorModule::PlayReport, 10};
constexpr Result ResultTransmissionNotAgreed{ErrorModule::PlayReport, 11};
constexpr Result ResultNetworkUnavailable{ErrorModule::PlayReport, 12};
constexpr Result ResultInProgress{ErrorModule::PlayReport, 60};
constexpr Result ResultCanceled{ErrorModule::PlayReport, 61};
constexpr Result ResultNotPermitted{ErrorModule::PlayReport, 90};
constexpr Result ResultNotImplemented{ErrorModule::PlayReport, 99};

} // namespace Service::PlayReport
