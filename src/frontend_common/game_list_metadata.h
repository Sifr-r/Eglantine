// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

#include "common/common_types.h"

namespace Loader {
class AppLoader;
}

namespace GameListDiscovery {

struct TitleMetadata {
    std::string title;
    std::vector<u8> icon;
    bool title_valid{};
};

[[nodiscard]] TitleMetadata ReadTitleMetadata(Loader::AppLoader& loader);

} // namespace GameListDiscovery
