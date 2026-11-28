// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_cmd/gpui_library.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "common/fs/fs_util.h"
#include "common/hex_util.h"
#include "core/core.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "emulation/classic/cartridge.h"
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

std::string LowercaseExtension(const std::filesystem::path& path) {
    auto extension = Common::FS::PathToUTF8String(path.extension());
    if (!extension.empty()) {
        extension.erase(0, 1);
        std::ranges::transform(extension, extension.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    }
    return extension;
}

void ReportClassicGame(Session& session, const FileSys::VfsFile& file,
                       const std::filesystem::path& path, std::string_view name,
                       std::string_view extension) {
    using enum Emulation::Classic::ClassicCartridge;
    const auto metadata = Emulation::Classic::ReadClassicCartridgeMetadata(
        extension, file.GetSize(), [&file](std::span<std::uint8_t> header) {
            return file.Read(header.data(), header.size());
        });
    if (!metadata) {
        return;
    }
    const auto title = Common::FS::PathToUTF8String(path.stem());
    const GameInfo info{
        .publisher = metadata->publisher,
        .version = metadata->version,
        .region = metadata->region,
        .product_code = metadata->game_code,
        .internal_title = metadata->internal_title,
        .hardware = metadata->hardware,
        .file_size = metadata->file_size,
    };
    switch (metadata->system) {
    case GameBoy:
        session.Game(name, title, "GB", std::nullopt, {}, "game-boy", info);
        break;
    case GameBoyColor:
        session.Game(name, title, "GBC", std::nullopt, {}, "game-boy-color", info);
        break;
    case GameBoyAdvance:
        session.Game(name, title, "GBA", std::nullopt, {}, "game-boy-advance", info);
        break;
    }
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
            const auto extension = LowercaseExtension(path);
            return Emulation::Classic::IsClassicCartridgeExtension(extension) ||
                   std::ranges::find(extensions, extension) != extensions.end();
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
        if (!file) {
            continue;
        }
        const auto extension = LowercaseExtension(path);
        if (Emulation::Classic::IsClassicCartridgeExtension(extension)) {
            ReportClassicGame(session, *file, path, name, extension);
            continue;
        }
        const auto loader = Loader::GetLoader(system, file);
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
        FileSys::ProgramMetadata program_metadata;
        std::string_view console;
        if (loader->ReadProgramMetadata(system, program_metadata) == Loader::ResultStatus::Success) {
            console = program_metadata.IsSwitch2() ? "switch-2" : "switch";
        }
        const GameInfo info{
            .publisher = metadata.publisher,
            .version = metadata.version,
            .file_size = file->GetSize(),
            .languages = metadata.languages,
        };
        session.Game(name, metadata.title, Loader::GetFileTypeString(type), program_id, icon_hex,
                     console, info);
    }
    session.LibraryDone(discovery.errors);
    return session.IsConnected();
}

} // namespace Gpui
