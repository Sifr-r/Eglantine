// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/android/android_settings.h"
#include "common/settings.h"
#include "native_config.h"

namespace Eglantine::AndroidPrototype {

NativeAndroidConfig::NativeAndroidConfig(const std::string& config_name, ConfigType config_type)
    : Config(config_type) {
    Initialize(config_name);
    if (config_type != ConfigType::InputProfile) {
        ReadAndroidValues();
        SaveAndroidValues();
    }
}

void NativeAndroidConfig::ReloadAllValues() {
    SetUpIni();
    Reload();
    ReadAndroidValues();
    SaveAndroidValues();
}

void NativeAndroidConfig::SaveAllValues() {
    SaveValues();
    SaveAndroidValues();
}

void NativeAndroidConfig::ReadAndroidValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::Android));
    ReadCategory(Settings::Category::Android);
    EndGroup();

    if (global) {
        BeginGroup(Settings::TranslateCategory(Settings::Category::Overlay));
        ReadCategory(Settings::Category::Overlay);
        EndGroup();
    }

    BeginGroup(Settings::TranslateCategory(Settings::Category::GpuDriver));
    ReadCategory(Settings::Category::GpuDriver);
    EndGroup();
}

void NativeAndroidConfig::SaveAndroidValues() {
    BeginGroup(Settings::TranslateCategory(Settings::Category::Android));
    WriteCategory(Settings::Category::Android);
    EndGroup();

    if (global) {
        BeginGroup(Settings::TranslateCategory(Settings::Category::Overlay));
        WriteCategory(Settings::Category::Overlay);
        EndGroup();
    }

    BeginGroup(Settings::TranslateCategory(Settings::Category::GpuDriver));
    WriteCategory(Settings::Category::GpuDriver);
    EndGroup();

    WriteToIni();
}

std::vector<Settings::BasicSetting*>& NativeAndroidConfig::FindRelevantList(
    Settings::Category category) {
    auto& map = Settings::values.linkage.by_category;
    if (map.contains(category)) {
        return map[category];
    }
    return AndroidSettings::values.linkage.by_category[category];
}

} // namespace Eglantine::AndroidPrototype
