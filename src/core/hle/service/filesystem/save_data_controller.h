// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/savedata_factory.h"
#include "core/file_sys/vfs/vfs_types.h"

namespace Service::FileSystem {

class SaveDataController {
public:
    explicit SaveDataController(Core::System& system,
                                std::shared_ptr<FileSys::SaveDataFactory> factory_);
    ~SaveDataController();

    Result CreateSaveData(FileSys::VirtualDir* out_save_data, FileSys::SaveDataSpaceId space,
                          const FileSys::SaveDataAttribute& attribute);
    Result OpenSaveData(FileSys::VirtualDir* out_save_data, FileSys::SaveDataSpaceId space,
                        const FileSys::SaveDataAttribute& attribute);
    Result OpenSaveDataSpace(FileSys::VirtualDir* out_save_data_space,
                             FileSys::SaveDataSpaceId space);

    /// Enumerates every save data stored in the given space.
    [[nodiscard]] std::vector<FileSys::SaveDataEntry> EnumerateSaveData(
        FileSys::SaveDataSpaceId space);

    /// Deletes the save data that matches the given attribute.
    Result DeleteSaveData(FileSys::SaveDataSpaceId space,
                          const FileSys::SaveDataAttribute& attribute);

    /// Deletes every save data in the space that matches the given filter.
    Result DeleteSaveDataMatching(FileSys::SaveDataSpaceId space,
                                  const FileSys::SaveDataFilter& filter, u64* out_deleted);

    /// Deletes the cache storage with the given index.
    Result DeleteCacheStorage(u16 index);

    /// Returns the free space of the space in bytes.
    [[nodiscard]] s64 GetFreeSpace(FileSys::SaveDataSpaceId space);

    /// Returns the commit id of a save data, or zero when it was never committed.
    [[nodiscard]] u64 GetCommitId(FileSys::SaveDataSpaceId space, u64 save_data_id);

    /// Opens the meta file of a save data, or nullptr when it does not exist.
    [[nodiscard]] FileSys::VirtualFile OpenMetaFile(FileSys::SaveDataSpaceId space,
                                                    const FileSys::SaveDataAttribute& attribute,
                                                    FileSys::SaveDataMetaType meta_type);

    /// Returns the total size of all save data in the space, in bytes.
    [[nodiscard]] s64 QueryTotalSize(FileSys::SaveDataSpaceId space);

    FileSys::SaveDataSize ReadSaveDataSize(FileSys::SaveDataType type, u64 title_id, u128 user_id);
    void WriteSaveDataSize(FileSys::SaveDataType type, u64 title_id, u128 user_id,
                           FileSys::SaveDataSize new_value);
    void SetAutoCreate(bool state);

private:
    Core::System& system;
    const std::shared_ptr<FileSys::SaveDataFactory> factory;
};

} // namespace Service::FileSystem
