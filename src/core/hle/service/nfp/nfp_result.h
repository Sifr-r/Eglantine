// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "core/hle/result.h"

namespace Service::NFP {

constexpr Result ResultDeviceNotFound(ErrorModule::NFP, 64);
constexpr Result ResultNfcDeviceNotFound(ErrorModule::NFP, 64);
constexpr Result ResultInvalidArgument(ErrorModule::NFP, 65);
constexpr Result ResultWrongApplicationAreaSize(ErrorModule::NFP, 68);
constexpr Result ResultWrongDeviceState(ErrorModule::NFP, 73);
constexpr Result ResultUnknown74(ErrorModule::NFC, 74);
constexpr Result ResultNfcDisabled(ErrorModule::NFP, 80);
constexpr Result ResultWriteAmiiboFailed(ErrorModule::NFP, 88);
constexpr Result ResultNeedRetry(ErrorModule::NFP, 88);
constexpr Result ResultNeedRestart(ErrorModule::NFP, 96);
constexpr Result ResultTagRemoved(ErrorModule::NFP, 97);
constexpr Result ResultRegistrationIsNotInitialized(ErrorModule::NFP, 120);
constexpr Result ResultNeedRegister(ErrorModule::NFP, 120);
constexpr Result ResultApplicationAreaIsNotInitialized(ErrorModule::NFP, 128);
constexpr Result ResultNeedCreate(ErrorModule::NFP, 128);
constexpr Result ResultCorruptedDataWithBackup(ErrorModule::NFP, 136);
constexpr Result ResultNeedRestore(ErrorModule::NFP, 136);
constexpr Result ResultCorruptedData(ErrorModule::NFP, 144);
constexpr Result ResultNeedFormat(ErrorModule::NFP, 144);
constexpr Result ResultWrongApplicationAreaId(ErrorModule::NFP, 152);
constexpr Result ResultAccessIdMisMatch(ErrorModule::NFP, 152);
constexpr Result ResultNotBroken(ErrorModule::NFP, 160);
constexpr Result ResultApplicationAreaExist(ErrorModule::NFP, 168);
constexpr Result ResultAlreadyCreated(ErrorModule::NFP, 168);
constexpr Result ResultNotSupported(ErrorModule::NFP, 176);
constexpr Result ResultNotAnAmiibo(ErrorModule::NFP, 178);
constexpr Result ResultInvalidFormatVersion(ErrorModule::NFP, 184);
constexpr Result ResultMaxNfcDeviceActivated(ErrorModule::NFP, 192);
constexpr Result ResultUnableToAccessBackupFile(ErrorModule::NFP, 200);
constexpr Result ResultConflictFunction(ErrorModule::NFP, 200);
constexpr Result ResultNotUpdated(ErrorModule::NFP, 208);

} // namespace Service::NFP
