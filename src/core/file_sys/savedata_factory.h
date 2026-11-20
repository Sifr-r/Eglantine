// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "common/common_funcs.h"
#include "common/common_types.h"
#include "core/file_sys/fs_save_data_types.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/result.h"

namespace Core {
class System;
}

namespace FileSys {

constexpr const char* GetSaveDataSizeFileName() {
    return ".yuzu_save_size";
}

constexpr const char* GetSaveDataCommitIdFileName() {
    return ".yuzu_save_commit_id";
}

using ProgramId = u64;

/// A save data entry discovered while enumerating a save data space.
struct SaveDataEntry {
    SaveDataSpaceId space;
    SaveDataType type;
    SaveDataRank rank;
    /// System save data id. Zero for every type other than SaveDataType::System.
    u64 system_save_data_id;
    /// Title id of the save. Zero for SaveDataType::System.
    u64 title_id;
    UserId user_id;
    u16 index;
    /// Path of the save, relative to the root of its space.
    std::string relative_path;
};

/// Stable guest-visible identity; hash collisions are rejected by FindSaveDataEntryById.
[[nodiscard]] u64 GetSaveDataId(const SaveDataEntry& entry);

/// Resolves a unique identity, or an unambiguous legacy title id scoped to the active account.
[[nodiscard]] const SaveDataEntry* FindSaveDataEntryById(
    std::span<const SaveDataEntry> entries, u64 save_data_id, std::optional<UserId> active_user);


/// File system interface to the SaveData archive
class SaveDataFactory {
public:
    explicit SaveDataFactory(Core::System& system_, ProgramId program_id_,
                             VirtualDir save_directory_);
    ~SaveDataFactory();

    VirtualDir Create(SaveDataSpaceId space, const SaveDataAttribute& meta) const;
    VirtualDir Open(SaveDataSpaceId space, const SaveDataAttribute& meta) const;

    VirtualDir GetSaveDataSpaceDirectory(SaveDataSpaceId space) const;

    static std::string GetSaveDataSpaceIdPath(SaveDataSpaceId space);
    static std::string GetFullPath(ProgramId program_id, VirtualDir dir, SaveDataSpaceId space,
                                   SaveDataType type, u64 title_id, u128 user_id, u64 save_id);
    static std::string GetUserGameSaveDataRoot(u128 user_id, bool future);

    SaveDataSize ReadSaveDataSize(SaveDataType type, u64 title_id, u128 user_id) const;
    void WriteSaveDataSize(SaveDataType type, u64 title_id, u128 user_id,
                           SaveDataSize new_value) const;

    /// Enumerates every save data stored in the given space.
    ///
    /// The returned entries follow the on disk layout: system saves live in
    /// `save/<save id>/<user id>`, account and device saves in
    /// `save/0/<user id>/<title id>`, cache saves in `save/cache/<title id>` or
    /// `save/cache/<title id>_<index>`, and temporary saves in `<save id>/<user id>/<title id>`.
    [[nodiscard]] std::vector<SaveDataEntry> Enumerate(SaveDataSpaceId space) const;

    /// Deletes the save data that matches the given attribute.
    ///
    /// A title id of zero is resolved to the title of the owning process, matching the
    /// behaviour of GetFullPath.
    ///
    /// @returns ResultTargetNotFound when no save matches the attribute, and
    ///          ResultPermissionDenied when the save exists but could not be removed.
    Result Delete(SaveDataSpaceId space, const SaveDataAttribute& meta) const;

    /// Deletes every save data in the space that matches the given filter.
    ///
    /// @param out_deleted receives the number of save data that were removed.
    /// @returns ResultTargetNotFound when the filter matched nothing.
    Result DeleteMatching(SaveDataSpaceId space, const SaveDataFilter& filter,
                          u64* out_deleted) const;

    /// Deletes the cache storage with the given index.
    ///
    /// @returns ResultTargetNotFound when the cache storage does not exist.
    Result DeleteCacheStorage(u16 index) const;

    /// Returns the free space of the space in bytes.
    [[nodiscard]] s64 GetFreeSpace(SaveDataSpaceId space) const;

    /// Returns the commit id of a save data, or zero when it was never committed.
    [[nodiscard]] u64 GetCommitId(SaveDataSpaceId space, u64 save_data_id) const;

    /// Opens the meta file of a save data.
    ///
    /// @returns The meta file, or nullptr when the save data has no meta file.
    [[nodiscard]] VirtualFile OpenMetaFile(SaveDataSpaceId space, const SaveDataAttribute& meta,
                                           SaveDataMetaType meta_type) const;

    /// Returns the total size of all save data in the space, in bytes.
    [[nodiscard]] s64 QueryTotalSize(SaveDataSpaceId space) const;

    void SetAutoCreate(bool state);

private:
    Core::System& system;
    ProgramId program_id;
    VirtualDir dir;
    bool auto_create{true};
};

} // namespace FileSys
