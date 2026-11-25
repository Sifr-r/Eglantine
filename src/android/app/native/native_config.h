// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "frontend_common/config.h"

namespace Eglantine::AndroidPrototype {

class NativeAndroidConfig final : public Config {
public:
    explicit NativeAndroidConfig(const std::string& config_name = "config",
                                 ConfigType config_type = ConfigType::GlobalConfig);

    void ReloadAllValues() override;
    void SaveAllValues() override;

protected:
    void ReadAndroidValues();
    void SaveAndroidValues();

    void ReadHidbusValues() override {}
    void ReadDebugControlValues() override {}
    void ReadPathValues() override {}
    void ReadShortcutValues() override {}
    void ReadUIValues() override {}
    void ReadUIGamelistValues() override {}
    void ReadUILayoutValues() override {}
    void ReadMultiplayerValues() override {}

    void SaveHidbusValues() override {}
    void SaveDebugControlValues() override {}
    void SavePathValues() override {}
    void SaveShortcutValues() override {}
    void SaveUIValues() override {}
    void SaveUIGamelistValues() override {}
    void SaveUILayoutValues() override {}
    void SaveMultiplayerValues() override {}

    std::vector<Settings::BasicSetting*>& FindRelevantList(Settings::Category category) override;
};

} // namespace Eglantine::AndroidPrototype
