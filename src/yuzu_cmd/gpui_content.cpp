// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_cmd/gpui_content.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <memory>
#include <ranges>
#include <system_error>
#include <vector>

#include <fmt/format.h>

#include "common/fs/fs_util.h"
#include "core/core.h"
#include "core/file_sys/card_image.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "frontend_common/content_manager.h"
#include "frontend_common/firmware_manager.h"

namespace Gpui {
namespace {

namespace FS = std::filesystem;
using FileSys::TitleType;

constexpr std::array NcaOperations{
    std::pair{"install_nca_system_program", TitleType::SystemProgram},
    std::pair{"install_nca_system_archive", TitleType::SystemDataArchive},
    std::pair{"install_nca_system_update", TitleType::SystemUpdate},
    std::pair{"install_nca_firmware_a", TitleType::FirmwarePackageA},
    std::pair{"install_nca_firmware_b", TitleType::FirmwarePackageB},
    std::pair{"install_nca_game", TitleType::Application},
    std::pair{"install_nca_update", TitleType::Update},
    std::pair{"install_nca_dlc", TitleType::AOC},
    std::pair{"install_nca_delta", TitleType::DeltaTitle},
};

constexpr auto NeverCancel = [](std::size_t, std::size_t) { return false; };

std::string LowerAscii(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string ReportInstall(ContentManager::InstallResult result) {
    using enum ContentManager::InstallResult;
    switch (result) {
    case Success:
    case Overwrite:
        return {};
    case BaseInstallAttempted:
        return "Add base games to the library. Install only updates and DLC as packages.";
    case Failure:
    default:
        return "The content could not be installed. Check the selected file and encryption keys.";
    }
}

std::string InstallPackage(Core::System& system, FileSys::VfsFilesystem& vfs,
                           std::string_view extension, const std::string& source) {
    auto* cache = system.GetFileSystemController().GetUserNANDContents();
    if (!cache) {
        return "The user NAND content directory is unavailable.";
    }
    if (extension == ".nsp" || extension == ".nsz") {
        return ReportInstall(ContentManager::InstallNSP(system, vfs, source, NeverCancel));
    }
    auto file = vfs.OpenFile(source, FileSys::OpenMode::Read);
    if (!file) {
        return "The cartridge image could not be opened.";
    }
    const FileSys::XCI xci{std::move(file)};
    using enum FileSys::InstallResult;
    switch (cache->InstallEntry(xci, true)) {
    case Success:
    case OverwriteExisting:
        return {};
    case ErrorBaseInstall:
        return ReportInstall(ContentManager::InstallResult::BaseInstallAttempted);
    default:
        return ReportInstall(ContentManager::InstallResult::Failure);
    }
}

std::string VerifyInstalled(Core::System& system, bool firmware_only) {
    FileSys::ManualContentProvider provider;
    const auto failed =
        ContentManager::VerifyInstalledContents(system, provider, NeverCancel, firmware_only);
    if (failed.empty()) {
        return {};
    }
    auto error = fmt::format("Integrity verification failed for {} installed file(s).", failed.size());
    for (const auto& entry : failed | std::views::take(4)) {
        error += fmt::format("\n{}", entry);
    }
    return error;
}

bool IsFirmwareNcaName(std::string_view name) {
    if (name.size() < 32 || (name.substr(32) != ".nca" && name.substr(32) != ".cnmt.nca")) {
        return false;
    }
    return std::ranges::all_of(name.substr(0, 32), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

class FirmwareStagingDirectory {
public:
    FirmwareStagingDirectory(const FS::path& parent, std::error_code& error) {
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 8; ++attempt) {
            const auto candidate = parent / fmt::format("gpui-firmware-{}-{}", timestamp, attempt);
            if (FS::create_directory(candidate, error)) {
                path = candidate;
                return;
            }
            if (error) {
                return;
            }
        }
    }

    ~FirmwareStagingDirectory() {
        if (!path.empty()) {
            std::error_code error;
            FS::remove_all(path, error);
        }
    }

    FirmwareStagingDirectory(const FirmwareStagingDirectory&) = delete;
    FirmwareStagingDirectory& operator=(const FirmwareStagingDirectory&) = delete;

    FS::path path;
};

std::string InstallFirmware(Core::System& system, FileSys::VfsFilesystem& vfs,
                            const FS::path& source) {
    auto& controller = system.GetFileSystemController();
    const auto directory = controller.GetSystemNANDContentDirectory();
    if (!directory) {
        return "The system NAND content directory is unavailable.";
    }
    const auto parent = FS::path{Common::FS::ToU8String(directory->GetFullPath())};
    const auto registered = parent / "registered";
    std::error_code ec;
    if (FS::is_symlink(FS::symlink_status(registered, ec)) || ec || !FS::is_directory(registered, ec)) {
        return "The system NAND registered directory is unavailable.";
    }
    if (!FS::is_empty(registered, ec) || ec) {
        return "Firmware installation in the GPUI preview requires an empty system NAND. Use the Qt frontend to replace existing firmware.";
    }

    std::vector<FS::path> files;
    for (FS::directory_iterator iterator{source, ec}, end; !ec && iterator != end;
         iterator.increment(ec)) {
        const auto& entry = *iterator;
        if (LowerAscii(Common::FS::PathToUTF8String(entry.path().extension())) != ".nca") {
            continue;
        }
        const auto name = LowerAscii(Common::FS::PathToUTF8String(entry.path().filename()));
        if (!FS::is_regular_file(entry.symlink_status(ec)) || ec || !IsFirmwareNcaName(name)) {
            return "The firmware folder contains an invalid NCA file name or type.";
        }
        files.push_back(entry.path());
        if (files.size() > 2048) {
            return "The firmware folder contains too many NCA files.";
        }
    }
    if (ec || files.empty()) {
        return "Choose a firmware folder containing NCA files.";
    }
    if (!ContentManager::AreKeysPresent()) {
        return "Install your encryption keys before installing firmware.";
    }

    FirmwareStagingDirectory stage{parent, ec};
    if (stage.path.empty()) {
        return "A firmware staging directory could not be created.";
    }
    for (const auto& file : files) {
        const auto target = stage.path / LowerAscii(Common::FS::PathToUTF8String(file.filename()));
        if (!FS::copy_file(file, target, FS::copy_options::none, ec)) {
            return "The firmware copy failed or contains duplicate file names.";
        }
    }

    {
        auto staged = vfs.OpenDirectory(Common::FS::PathToUTF8String(stage.path), FileSys::OpenMode::Read);
        if (!staged) {
            return "The staged firmware could not be read.";
        }
        FileSys::RegisteredCache cache{std::move(staged)};
        constexpr auto MiiEditId = static_cast<u64>(Service::AM::AppletProgramId::MiiEdit);
        constexpr u64 version_id = 0x0100000000000809;
        const auto version = cache.GetEntry(version_id, FileSys::ContentRecordType::Data);
        const auto mii = cache.GetEntry(MiiEditId, FileSys::ContentRecordType::Program);
        const auto romfs = version && version->GetRomFS() ? FileSys::ExtractRomFS(version->GetRomFS()) : nullptr;
        const auto version_file = romfs ? romfs->GetFile("file") : nullptr;
        if (!mii || !version_file || version_file->GetSize() != sizeof(Service::Set::FirmwareVersionFormat)) {
            return "The firmware folder is incomplete or cannot be decrypted.";
        }
        for (const auto& entry : cache.ListEntriesFilter()) {
            const auto raw = cache.GetEntryRaw(entry.title_id, entry.type);
            if (!raw || Loader::AppLoader_NCA(raw).VerifyIntegrity([](std::size_t, std::size_t) {
                    return true;
                }) != Loader::ResultStatus::Success) {
                return "The staged firmware failed integrity verification.";
            }
        }
    }

    // Windows cannot rename over a directory. This removes the placeholder only if still empty.
    if (!FS::remove(registered, ec) || ec) {
        return "The system NAND changed while firmware was being prepared.";
    }
    FS::rename(stage.path, registered, ec);
    if (ec) {
        std::error_code restore_error;
        FS::create_directory(registered, restore_error);
        return "The verified firmware could not be moved into the system NAND.";
    }
    stage.path.clear();
    controller.CreateFactories(vfs);
    if (FirmwareManager::VerifyFirmware(system) != FirmwareManager::FirmwareGood) {
        return "Firmware was copied, but Eglantine could not open it in the system NAND.";
    }
    return {};
}

} // namespace

std::string RunContentCommand(Core::System& system, std::string_view operation,
                              std::string_view path_utf8) {
    const auto nca_route = std::ranges::find_if(NcaOperations, [&](const auto& entry) {
        return entry.first == operation;
    });
    const bool verify = operation == "verify_content";
    const bool firmware = operation == "install_firmware";
    const bool package = operation == "install_content";
    if (!verify && !firmware && !package && nca_route == NcaOperations.end()) {
        return "Unknown content operation.";
    }
    if ((verify ? !path_utf8.empty() : path_utf8.empty()) ||
        path_utf8.find('\0') != std::string_view::npos) {
        return "The content operation requires the expected file or folder path.";
    }

    std::error_code ec;
    FS::path path;
    std::string extension;
    if (!verify) {
        path = FS::canonical(FS::path{Common::FS::ToU8String(path_utf8)}, ec);
        if (ec || !(firmware ? FS::is_directory(path, ec) : FS::is_regular_file(path, ec)) || ec) {
            return firmware ? "The firmware folder is unavailable." : "The selected file is unavailable.";
        }
        extension = LowerAscii(Common::FS::PathToUTF8String(path.extension()));
        if (package && extension != ".nsp" && extension != ".nsz" && extension != ".xci" &&
            extension != ".xcz") {
            return extension == ".nca" ? "Choose a title type to install a standalone NCA."
                                       : "Choose an NSP, NSZ, XCI, or XCZ content package.";
        }
        if (nca_route != NcaOperations.end() && extension != ".nca") {
            return "Choose an NCA file for the selected title type.";
        }
    }

    auto filesystem = std::make_shared<FileSys::RealVfsFilesystem>();
    system.SetFilesystem(filesystem);
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    auto& controller = system.GetFileSystemController();
    controller.CreateFactories(*filesystem);

    if (verify) {
        return VerifyInstalled(system, false);
    }
    if (firmware) {
        return InstallFirmware(system, *filesystem, path);
    }
    const auto path_string = Common::FS::PathToUTF8String(path);
    if (package) {
        return InstallPackage(system, *filesystem, extension, path_string);
    }
    const auto nca_type = nca_route->second;
    auto* cache = nca_type >= TitleType::Application ? controller.GetUserNANDContents()
                                                    : controller.GetSystemNANDContents();
    if (!cache) {
        return "The selected NAND content directory is unavailable.";
    }
    return ReportInstall(ContentManager::InstallNCA(*filesystem, path_string, *cache, nca_type,
                                                    NeverCancel));
}

} // namespace Gpui
