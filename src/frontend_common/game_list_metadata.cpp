// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend_common/game_list_metadata.h"

#include "core/loader/loader.h"

namespace GameListDiscovery {

TitleMetadata ReadTitleMetadata(Loader::AppLoader& loader) {
    TitleMetadata metadata{.title = " "};
    loader.ReadIcon(metadata.icon);
    metadata.title_valid = loader.ReadTitle(metadata.title) == Loader::ResultStatus::Success;
    return metadata;
}

} // namespace GameListDiscovery
