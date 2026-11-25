// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

#include "core/file_sys/vfs/vfs_types.h"

namespace Eglantine::AndroidPrototype {

struct DocumentVfs {
    FileSys::VirtualFilesystem filesystem;
    std::string path;
    std::string error;
};

[[nodiscard]] DocumentVfs OpenDocumentVfs(std::string_view uri);

} // namespace Eglantine::AndroidPrototype
