// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_cmd/gpui_library.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>

#include "common/fs/fs_util.h"
#include "common/hex_util.h"
#include "core/core.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "frontend_common/game_list_discovery.h"
#include "frontend_common/game_list_metadata.h"
#include "yuzu_cmd/gpui_session.h"

namespace Gpui {
namespace {

constexpr std::size_t MaxIconBytes = 256 * 1024;

bool NeedsBootableContainer(Loader::FileType type) {
    using enum Loader::FileType;
    return type == XCI || type == XCZ || type == NSP || type == NSZ || type == NUP;
}

} // namespace

bool ScanLibrary(Session& session, Core::System& system, const std::filesystem::path& directory,
                 bool deep) {
    std::error_code ec;
    const auto root = std::filesystem::canonical(directory, ec);
    if (ec || !std::filesystem::is_directory(root, ec) || ec) {
        session.Error("The library folder is unavailable");
        return false;
    }

    const auto extensions = Loader::GetFileExtensions(Loader::FileExtensionUse::DesktopGame);
    auto discovery = GameListDiscovery::Discover(
        root, deep,
        [&](const auto& path) {
            auto extension = Common::FS::PathToUTF8String(path.extension());
            if (extension.empty()) {
                return false;
            }
            extension.erase(0, 1);
            std::ranges::transform(extension, extension.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return std::ranges::find(extensions, extension) != extensions.end();
        },
        [] { return false; });

    auto filesystem = std::make_shared<FileSys::RealVfsFilesystem>();
    system.SetFilesystem(filesystem);
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system.GetFileSystemController().CreateFactories(*filesystem);

    std::ranges::sort(discovery.files);
    for (const auto& path : discovery.files) {
        if (!session.IsConnected()) {
            return false;
        }
        const auto canonical = std::filesystem::canonical(path, ec);
        if (ec) {
            ++discovery.errors;
            continue;
        }
        const auto name = Common::FS::PathToUTF8String(canonical);
        const auto file = filesystem->OpenFile(name, FileSys::OpenMode::Read);
        const auto loader = file ? Loader::GetLoader(system, file) : nullptr;
        if (!loader) {
            continue;
        }
        const auto type = loader->GetFileType();
        if (type == Loader::FileType::Unknown || type == Loader::FileType::Error ||
            (NeedsBootableContainer(type) && !Loader::IsBootableGameContainer(file, type))) {
            continue;
        }
        auto metadata = GameListDiscovery::ReadTitleMetadata(*loader);
        if (!metadata.title_valid || metadata.title.empty() || metadata.title == " ") {
            const auto display_path = path.filename() == "main" ? path.parent_path().parent_path() : path;
            metadata.title = Common::FS::PathToUTF8String(display_path.stem());
        }
        u64 id{};
        const auto program_id = loader->ReadProgramId(id) == Loader::ResultStatus::Success && id != 0
                                    ? std::optional{id}
                                    : std::nullopt;
        const auto icon_hex = metadata.icon.size() <= MaxIconBytes
                                  ? Common::HexToString(metadata.icon, false)
                                  : std::string{};
        session.Game(name, metadata.title, Loader::GetFileTypeString(type), program_id, icon_hex);
    }
    session.LibraryDone(discovery.errors);
    return session.IsConnected();
}

} // namespace Gpui
