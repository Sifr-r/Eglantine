// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "migration_worker.h"

#include <array>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <QFile>
#include <QSaveFile>
#ifdef _WIN32
#include <windows.h>
#endif
#include "common/fs/path_util.h"
#include "common/fs/symlink.h"
#include "frontend_common/config.h"

namespace {

namespace fs = std::filesystem;

fs::path RelativePathWithin(const fs::path& path, const fs::path& root) {
    const fs::path normalized = path.lexically_normal();
    auto candidate = normalized.begin();
    for (const auto& component : root.lexically_normal()) {
        if (candidate == normalized.end()) {
            return {};
        }
        if (*candidate != component) {
#ifdef _WIN32
            if (CompareStringOrdinal(candidate->c_str(), -1, component.c_str(), -1, TRUE) !=
                CSTR_EQUAL) {
                return {};
            }
#else
            return {};
#endif
        }
        ++candidate;
    }
    fs::path relative;
    for (; candidate != normalized.end(); ++candidate) {
        relative /= *candidate;
    }
    return relative.empty() ? fs::path{"."} : relative;
}

bool IsLinkedPath([[maybe_unused]] const fs::path& path, fs::file_status status) {
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return fs::is_symlink(status) ||
           (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT));
#else
    return fs::is_symlink(status);
#endif
}

std::error_code ClearShaderCache(const fs::path& shader_dir, const fs::path& cache_root) {
    std::error_code error;
    for (fs::path path = shader_dir; !path.empty();) {
        const auto status = fs::symlink_status(path, error);
        if (status.type() == fs::file_type::not_found) {
            error.clear();
        }
        if (error) {
            return error;
        }
        if (IsLinkedPath(path, status)) {
            return std::make_error_code(std::errc::operation_not_permitted);
        }
        if (path == cache_root) {
            fs::remove_all(shader_dir, error);
            if (!error) {
                fs::create_directories(shader_dir, error);
            }
            return error;
        }
        const fs::path parent = path.parent_path();
        if (parent == path) {
            break;
        }
        path = parent;
    }
    return std::make_error_code(std::errc::invalid_argument);
}

void ValidateSourceConfig(const fs::path& config_dir) {
    for (const fs::path& path : {config_dir, config_dir / "qt-config.ini"}) {
        const auto status = fs::symlink_status(path);
        if (IsLinkedPath(path, status)) {
            throw fs::filesystem_error("Migration config path is linked", path,
                                       std::make_error_code(std::errc::invalid_argument));
        }
        if (path != config_dir && fs::exists(status) && !fs::is_regular_file(status)) {
            throw fs::filesystem_error("Expected a regular migration config", path,
                                       std::make_error_code(std::errc::invalid_argument));
        }
    }
}

void RewriteConfigPaths(const fs::path& config_path, const fs::path& old_root,
                        const fs::path& new_root) {
    const fs::file_status status = fs::symlink_status(config_path);
    if (!fs::exists(status)) {
        return;
    }
    if (!fs::is_regular_file(status)) {
        throw fs::filesystem_error("Expected a regular migration config", config_path,
                                   std::make_error_code(std::errc::invalid_argument));
    }

    const std::string config_name = Common::FS::PathToUTF8String(config_path);
    const QString filename = QString::fromStdString(config_name);
    QFile input(filename);
    if (!input.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(config_name + ": " + input.errorString().toStdString());
    }
    const QByteArray data = input.readAll();
    if (input.error() != QFileDevice::NoError) {
        throw std::runtime_error(config_name + ": " + input.errorString().toStdString());
    }
    input.close();

    CSimpleIniA ini;
    ini.SetUnicode(true);
    ini.SetSpaces(false);
    if (ini.LoadData(data.constData(), static_cast<size_t>(data.size())) < 0) {
        throw std::runtime_error("Could not read migration config: " + config_name);
    }

    static constexpr std::array path_settings{
        std::pair{"Data%20Storage", "nand_directory"},
        std::pair{"Data%20Storage", "sdmc_directory"},
        std::pair{"Data%20Storage", "load_directory"},
        std::pair{"Data%20Storage", "dump_directory"},
        std::pair{"Data%20Storage", "tas_directory"},
        std::pair{"Data%20Storage", "save_directory"},
        std::pair{"UI", "Screenshots\\screenshot_path"},
    };

    bool changed = false;
    for (const auto& [section, key] : path_settings) {
        std::string_view value = ini.GetValue(section, key, "");
        const bool quoted = value.size() >= 2 && value.front() == '"' && value.back() == '"';
        if (quoted) {
            value.remove_prefix(1);
            value.remove_suffix(1);
        }
        const fs::path old_path{Common::FS::ToU8String(value)};
        if (!old_path.is_absolute()) {
            continue;
        }
        const fs::path relative = RelativePathWithin(old_path, old_root);
        if (relative.empty()) {
            continue;
        }
        const fs::path new_path = relative == "." ? new_root : new_root / relative;
        std::string replacement = Common::FS::ToUTF8String(new_path.generic_u8string());
        if (quoted) {
            replacement = '"' + replacement + '"';
        }
        if (ini.SetValue(section, key, replacement.c_str()) < 0) {
            throw std::runtime_error("Could not update migration config: " + config_name);
        }
        changed = true;
    }
    if (!changed) {
        return;
    }

    std::string contents;
    if (ini.Save(contents, data.startsWith("\xef\xbb\xbf")) < 0) {
        throw std::runtime_error("Could not serialize migration config: " + config_name);
    }
    QSaveFile output(filename);
    const auto size = static_cast<qint64>(contents.size());
    if (!output.open(QIODevice::WriteOnly) || output.write(contents.data(), size) != size ||
        !output.commit()) {
        throw std::runtime_error(config_name + ": " + output.errorString().toStdString());
    }
}

struct MigrationDirectory {
    fs::path source;
    fs::path destination;
    bool removed_empty{};
    bool installed{};
};

bool CanReplaceDestination(const fs::path& path) {
    const auto status = fs::symlink_status(path);
    if (fs::exists(status) &&
        (IsLinkedPath(path, status) || !fs::is_directory(status) || !fs::is_empty(path))) {
        throw fs::filesystem_error("Migration destination already contains data", path,
                                   std::make_error_code(std::errc::file_exists));
    }
    return fs::exists(status);
}

void TransferDirectories(std::vector<MigrationDirectory>& directories,
                         MigrationWorker::MigrationStrategy strategy,
                         const fs::path& config_path) {
    using Strategy = MigrationWorker::MigrationStrategy;
    constexpr auto copy_options = fs::copy_options::update_existing | fs::copy_options::recursive;
    for (const auto& directory : directories) {
        CanReplaceDestination(directory.destination);
    }

    try {
        for (auto& directory : directories) {
            auto& [source, destination, removed_empty, installed] = directory;
            if (CanReplaceDestination(destination)) {
                // A portable profile has an existing user directory, even on first run.
                removed_empty = fs::remove(destination);
            }
            switch (strategy) {
            case Strategy::Copy:
                installed = fs::create_directory(destination);
                if (!installed) {
                    throw fs::filesystem_error("Could not reserve migration destination",
                                               destination,
                                               std::make_error_code(std::errc::file_exists));
                }
                fs::copy(source, destination, copy_options);
                break;
            case Strategy::Move:
                fs::rename(source, destination);
                installed = true;
                break;
            case Strategy::Link:
                if (!Common::FS::CreateSymlink(source, destination)) {
                    throw fs::filesystem_error("Could not link the migration directory", source,
                                               destination,
                                               std::make_error_code(std::errc::io_error));
                }
                installed = true;
                break;
            }
        }
        if (strategy != Strategy::Link) {
            RewriteConfigPaths(config_path, directories.front().source,
                               directories.front().destination);
        }
    } catch (...) {
        std::optional<fs::filesystem_error> rollback_error;
        for (auto it = directories.rbegin(); it != directories.rend(); ++it) {
            const auto& [source, destination, removed_empty, installed] = *it;
            std::error_code error;
            if (installed) {
                switch (strategy) {
                case Strategy::Copy:
                    fs::remove_all(destination, error);
                    break;
                case Strategy::Move:
                    fs::rename(destination, source, error);
                    break;
                case Strategy::Link:
                    fs::remove(destination, error);
                    break;
                }
            }
            if (!error && removed_empty) {
                fs::create_directory(destination, error);
            }
            if (error && !rollback_error) {
                rollback_error.emplace("Could not restore the migration directory",
                                       destination, source, error);
            }
        }
        if (rollback_error) {
            throw *rollback_error;
        }
        throw;
    }
}

} // namespace

MigrationWorker::MigrationWorker(const Emulator selected_emu_, const bool clear_shader_cache_,
                                 const MigrationStrategy strategy_)
    : QObject(), selected_emu(selected_emu_), clear_shader_cache(clear_shader_cache_),
      strategy(strategy_) {}

void MigrationWorker::process() try {
    if (strategy != MigrationStrategy::Copy && strategy != MigrationStrategy::Move &&
        strategy != MigrationStrategy::Link) {
        emit error(tr("Unknown migration strategy."));
        return;
    }
    const fs::path legacy_user_dir = Common::FS::GetLegacyPath(selected_emu.e_user_dir);
    const fs::path legacy_config_dir = Common::FS::GetLegacyPath(selected_emu.e_config_dir);
    const fs::path legacy_cache_dir = Common::FS::GetLegacyPath(selected_emu.e_cache_dir);

    // TODO(crueter): Make these constexpr since they're defaulted
    fs::path eden_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir);
    fs::path config_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir);
    fs::path cache_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir);
    fs::path shader_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir);

    eden_dir.make_preferred();
    config_dir.make_preferred();
    cache_dir.make_preferred();
    shader_dir.make_preferred();

    if (!legacy_user_dir.is_absolute() || !eden_dir.is_absolute() ||
        !fs::is_directory(legacy_user_dir)) {
        throw fs::filesystem_error("Invalid migration source", legacy_user_dir, eden_dir,
                                   std::make_error_code(std::errc::invalid_argument));
    }
    const fs::path source_root = fs::canonical(legacy_user_dir);
    const fs::path destination_root = fs::weakly_canonical(eden_dir);
    if (!RelativePathWithin(source_root, destination_root).empty() ||
        !RelativePathWithin(destination_root, source_root).empty()) {
        throw fs::filesystem_error("Migration directories overlap", legacy_user_dir, eden_dir,
                                   std::make_error_code(std::errc::invalid_argument));
    }

    const fs::path config_path = config_dir / "qt-config.ini";
    std::vector<MigrationDirectory> directories{{legacy_user_dir, eden_dir}};

#ifndef WIN32
    if (cache_dir != eden_dir && fs::is_directory(legacy_cache_dir)) {
        directories.push_back({legacy_cache_dir, cache_dir});
    }
    if (config_dir != eden_dir && config_dir != cache_dir &&
        fs::is_directory(legacy_config_dir)) {
        directories.push_back({legacy_config_dir, config_dir});
    }
#endif

    if (strategy == MigrationStrategy::Link) {
        for (const auto& directory : directories) {
            if (directory.destination != eden_dir &&
                !RelativePathWithin(directory.destination, eden_dir).empty()) {
                throw fs::filesystem_error("Could not link separate legacy directories into a "
                                           "portable profile",
                                           directory.source, directory.destination,
                                           std::make_error_code(std::errc::operation_not_supported));
            }
        }
    } else {
        if (IsLinkedPath(legacy_user_dir, fs::symlink_status(legacy_user_dir))) {
            throw fs::filesystem_error("Migration source is linked", legacy_user_dir,
                                       std::make_error_code(std::errc::invalid_argument));
        }
        ValidateSourceConfig(legacy_config_dir);
        const fs::path relative_config = RelativePathWithin(config_dir, eden_dir);
        if (!relative_config.empty()) {
            ValidateSourceConfig(legacy_user_dir / relative_config);
        }
    }
    TransferDirectories(directories, strategy, config_path);

    switch (strategy) {
    case MigrationStrategy::Link:
        success_text.append(tr("\n\nNote that your configuration and data will be shared with %1.\n"
                               "If this is not desirable, delete the following files:\n%2\n%3\n%4")
                                .arg(selected_emu.name(),
                                     QString::fromStdString(Common::FS::PathToUTF8String(eden_dir)),
                                     QString::fromStdString(
                                         Common::FS::PathToUTF8String(config_dir)),
                                     QString::fromStdString(
                                         Common::FS::PathToUTF8String(cache_dir))));

        break;
    case MigrationStrategy::Move:
        break;
    case MigrationStrategy::Copy:
        success_text.append(tr("\n\nIf you wish to clean up the files which were left in the old "
                               "data location, you can do so by deleting the following directory:\n"
                               "%1")
                                .arg(QString::fromStdString(
                                    Common::FS::PathToUTF8String(legacy_user_dir))));
        break;
    }

    // Delete and re-create shader dir
    if (clear_shader_cache) {
        const fs::path& cache_root =
            RelativePathWithin(cache_dir, eden_dir).empty() ? cache_dir : eden_dir;
        const std::error_code error = ClearShaderCache(shader_dir, cache_root);
        if (error) {
            success_text.append(tr("\n\nCould not clear the shader cache at %1.\n%2")
                                    .arg(QString::fromStdString(
                                             Common::FS::PathToUTF8String(shader_dir)),
                                         QString::fromStdString(error.message())));
        }
    }

    emit finished(success_text, Common::FS::PathToUTF8String(legacy_user_dir));
} catch (const std::exception& exception) {
    emit error(tr("Data migration failed.\n%1").arg(QString::fromUtf8(exception.what())));
}
