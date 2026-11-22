// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include "common/logging.h"
#include "common/settings.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/set/key_code_map.h"
#include "core/hle/service/set/settings_server.h"

namespace Service::Set {
namespace {
constexpr std::size_t PRE_4_0_0_MAX_ENTRIES = 0xF;
constexpr std::size_t POST_4_0_0_MAX_ENTRIES = 0x40;

constexpr Result ResultInvalidLanguage{ErrorModule::Settings, 625};
constexpr Result ResultNullPointer{ErrorModule::Settings, 1261};

Result FillKeyCodeMapFromSettings(OutLargeData<KeyCodeMap, BufferAttr_HipcMapAlias>& out_key_code_map) {
    R_UNLESS(out_key_code_map != nullptr, ResultNullPointer);

    const auto language_index =
        static_cast<std::size_t>(Settings::values.language_index.GetValue());
    bool found = false;
    const auto& key_code_map = GetKeyCodeMapForLanguageIndex(language_index, &found);
    if (!found) {
        LOG_ERROR(Service_SET,
                  "Could not find keyboard layout for language index {}, defaulting to English us",
                  Settings::values.language_index.GetValue());
    }

    *out_key_code_map = key_code_map;
    R_SUCCEED();
}

} // Anonymous namespace

LanguageCode GetLanguageCodeFromIndex(std::size_t index) {
    return available_language_codes.at(index);
}

ISettingsServer::ISettingsServer(Core::System& system_) : ServiceFramework{system_, "set"} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, C<&ISettingsServer::GetLanguageCode>, "GetLanguageCode"},
        {1, C<&ISettingsServer::GetAvailableLanguageCodes>, "GetAvailableLanguageCodes"},
        {2, C<&ISettingsServer::MakeLanguageCode>, "MakeLanguageCode"},
        {3, C<&ISettingsServer::GetAvailableLanguageCodeCount>, "GetAvailableLanguageCodeCount"},
        {4, C<&ISettingsServer::GetRegionCode>, "GetRegionCode"},
        {5, C<&ISettingsServer::GetAvailableLanguageCodes2>, "GetAvailableLanguageCodes2"},
        {6, C<&ISettingsServer::GetAvailableLanguageCodeCount2>, "GetAvailableLanguageCodeCount2"},
        {7, C<&ISettingsServer::GetKeyCodeMap>, "GetKeyCodeMap"},
        {8, C<&ISettingsServer::GetQuestFlag>, "GetQuestFlag"},
        {9, C<&ISettingsServer::GetKeyCodeMap2>, "GetKeyCodeMap2"},
        {10, &ISettingsServer::StubReplyZeroed, "GetFirmwareVersionForDebug"},
        {11, C<&ISettingsServer::GetDeviceNickName>, "GetDeviceNickName"},
        {12, C<&ISettingsServer::GetKeyCodeMapByPort>, "GetKeyCodeMapByPort"},
        {13, &ISettingsServer::StubReplyZeroed, "Unknown13"},
        {14, &ISettingsServer::StubReplyZeroed, "Unknown14"},
        {15, &ISettingsServer::StubReplyZeroed, "Unknown15"},
        {16, &ISettingsServer::StubReplyZeroed, "Unknown16"},
        {17, &ISettingsServer::StubReplyZeroed, "Unknown17"},
    };
    // clang-format on

    RegisterHandlers(functions);
}

ISettingsServer::~ISettingsServer() = default;

Result ISettingsServer::GetLanguageCode(Out<LanguageCode> out_language_code) {
    LOG_DEBUG(Service_SET, "called {}", Settings::values.language_index.GetValue());

    *out_language_code = available_language_codes[static_cast<std::size_t>(
        Settings::values.language_index.GetValue())];
    R_SUCCEED();
}

Result ISettingsServer::GetAvailableLanguageCodes(
    Out<s32> out_count, OutArray<LanguageCode, BufferAttr_HipcPointer> out_language_codes) {
    LOG_DEBUG(Service_SET, "called");

    const std::size_t max_amount = (std::min)(PRE_4_0_0_MAX_ENTRIES, out_language_codes.size());
    *out_count = static_cast<s32>((std::min)(available_language_codes.size(), max_amount));

    memcpy(out_language_codes.data(), available_language_codes.data(),
           static_cast<std::size_t>(*out_count) * sizeof(LanguageCode));

    R_SUCCEED();
}

Result ISettingsServer::MakeLanguageCode(Out<LanguageCode> out_language_code, Language language) {
    LOG_DEBUG(Service_SET, "called, language={}", language);

    const auto index = static_cast<std::size_t>(language);
    R_UNLESS(index < available_language_codes.size(), Set::ResultInvalidLanguage);

    *out_language_code = available_language_codes[index];
    R_SUCCEED();
}

Result ISettingsServer::GetAvailableLanguageCodeCount(Out<s32> out_count) {
    LOG_DEBUG(Service_SET, "called");

    *out_count = PRE_4_0_0_MAX_ENTRIES;
    R_SUCCEED();
}

Result ISettingsServer::GetRegionCode(Out<SystemRegionCode> out_region_code) {
    LOG_DEBUG(Service_SET, "called");

    *out_region_code = static_cast<SystemRegionCode>(Settings::values.region_index.GetValue());
    R_SUCCEED();
}

Result ISettingsServer::GetAvailableLanguageCodes2(
    Out<s32> out_count, OutArray<LanguageCode, BufferAttr_HipcMapAlias> language_codes) {
    LOG_DEBUG(Service_SET, "called");

    const std::size_t max_amount = (std::min)(POST_4_0_0_MAX_ENTRIES, language_codes.size());
    *out_count = static_cast<s32>((std::min)(available_language_codes.size(), max_amount));

    memcpy(language_codes.data(), available_language_codes.data(),
           static_cast<std::size_t>(*out_count) * sizeof(LanguageCode));

    R_SUCCEED();
}

Result ISettingsServer::GetAvailableLanguageCodeCount2(Out<s32> out_count) {
    LOG_DEBUG(Service_SET, "called");

    *out_count = POST_4_0_0_MAX_ENTRIES;
    R_SUCCEED();
}

Result ISettingsServer::GetKeyCodeMap(
    OutLargeData<KeyCodeMap, BufferAttr_HipcMapAlias> out_key_code_map) {
    LOG_DEBUG(Service_SET, "called");
    R_RETURN(FillKeyCodeMapFromSettings(out_key_code_map));
}

Result ISettingsServer::GetQuestFlag(Out<bool> out_quest_flag) {
    LOG_DEBUG(Service_SET, "called");

    *out_quest_flag = Settings::values.quest_flag.GetValue();
    R_SUCCEED();
}

Result ISettingsServer::GetKeyCodeMap2(
    OutLargeData<KeyCodeMap, BufferAttr_HipcMapAlias> out_key_code_map) {
    LOG_DEBUG(Service_SET, "called");
    R_RETURN(FillKeyCodeMapFromSettings(out_key_code_map));
}

Result ISettingsServer::GetDeviceNickName(
    OutLargeData<std::array<u8, 0x80>, BufferAttr_HipcMapAlias> out_device_name) {
    LOG_DEBUG(Service_SET, "called");

    const std::size_t string_size =
        (std::min)(Settings::values.device_name.GetValue().size(), out_device_name->size());

    *out_device_name = {};
    memcpy(out_device_name->data(), Settings::values.device_name.GetValue().data(), string_size);
    R_SUCCEED();
}

Result ISettingsServer::GetKeyCodeMapByPort(
    OutLargeData<KeyCodeMap, BufferAttr_HipcMapAlias> out_key_code_map, u32 port) {
    LOG_DEBUG(Service_SET, "called, port={}", port);
    R_RETURN(FillKeyCodeMapFromSettings(out_key_code_map));
}

} // namespace Service::Set
