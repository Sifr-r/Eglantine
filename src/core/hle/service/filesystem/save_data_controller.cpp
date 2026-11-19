// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/core.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/errors.h"
#include "core/file_sys/patch_manager.h"
#include "core/hle/service/filesystem/save_data_controller.h"
#include "core/loader/loader.h"

namespace Service::FileSystem {

namespace {

// A default size for normal/journal save data size if application control metadata cannot be found.
// This should be large enough to satisfy even the most extreme requirements (~4.2GB)
constexpr u64 SufficientSaveDataSize = 0xF0000000;

FileSys::SaveDataSize GetDefaultSaveDataSize(Core::System& system, u64 program_id) {
    const FileSys::PatchManager pm{program_id, system.GetFileSystemController(),
                                   system.GetContentProvider()};
    const auto metadata = pm.GetControlMetadata();
    const auto& nacp = metadata.first;

    if (nacp != nullptr) {
        return {nacp->GetDefaultNormalSaveSize(), nacp->GetDefaultJournalSaveSize()};
    }

    return {SufficientSaveDataSize, SufficientSaveDataSize};
}

} // namespace

SaveDataController::SaveDataController(Core::System& system_,
                                       std::shared_ptr<FileSys::SaveDataFactory> factory_)
    : system{system_}, factory{std::move(factory_)} {}
SaveDataController::~SaveDataController() = default;

Result SaveDataController::CreateSaveData(FileSys::VirtualDir* out_save_data,
                                          FileSys::SaveDataSpaceId space,
                                          const FileSys::SaveDataAttribute& attribute) {
    LOG_TRACE(Service_FS, "Creating Save Data for space_id={:01X}, save_struct={}", space,
              attribute.DebugInfo());

    auto save_data = factory->Create(space, attribute);
    if (save_data == nullptr) {
        return FileSys::ResultTargetNotFound;
    }

    *out_save_data = save_data;
    return ResultSuccess;
}

Result SaveDataController::OpenSaveData(FileSys::VirtualDir* out_save_data,
                                        FileSys::SaveDataSpaceId space,
                                        const FileSys::SaveDataAttribute& attribute) {
    auto save_data = factory->Open(space, attribute);
    if (save_data == nullptr) {
        return FileSys::ResultTargetNotFound;
    }

    *out_save_data = save_data;
    return ResultSuccess;
}

Result SaveDataController::OpenSaveDataSpace(FileSys::VirtualDir* out_save_data_space,
                                             FileSys::SaveDataSpaceId space) {
    auto save_data_space = factory->GetSaveDataSpaceDirectory(space);
    if (save_data_space == nullptr) {
        return FileSys::ResultTargetNotFound;
    }

    *out_save_data_space = save_data_space;
    return ResultSuccess;
}

std::vector<FileSys::SaveDataEntry> SaveDataController::EnumerateSaveData(
    FileSys::SaveDataSpaceId space) {
    LOG_TRACE(Service_FS, "Enumerating the save data of the space_id={:01X}", space);
    return factory->Enumerate(space);
}

Result SaveDataController::DeleteSaveData(FileSys::SaveDataSpaceId space,
                                          const FileSys::SaveDataAttribute& attribute) {
    LOG_TRACE(Service_FS, "Deleting the save data of the space_id={:01X}, save_struct={}", space,
              attribute.DebugInfo());
    return factory->Delete(space, attribute);
}

Result SaveDataController::DeleteSaveDataMatching(FileSys::SaveDataSpaceId space,
                                                  const FileSys::SaveDataFilter& filter,
                                                  u64* out_deleted) {
    LOG_TRACE(Service_FS, "Deleting the save data of the space_id={:01X} that match a filter",
              space);
    return factory->DeleteMatching(space, filter, out_deleted);
}

Result SaveDataController::DeleteCacheStorage(u16 index) {
    LOG_TRACE(Service_FS, "Deleting the cache storage with the index={}", index);
    return factory->DeleteCacheStorage(index);
}

s64 SaveDataController::GetFreeSpace(FileSys::SaveDataSpaceId space) {
    LOG_TRACE(Service_FS, "Getting the free space of the space_id={:01X}", space);
    return factory->GetFreeSpace(space);
}

u64 SaveDataController::GetCommitId(FileSys::SaveDataSpaceId space, u64 save_data_id) {
    LOG_TRACE(Service_FS, "Getting the commit id of the save data {:016X} of the space_id={:01X}",
              save_data_id, space);
    return factory->GetCommitId(space, save_data_id);
}

FileSys::VirtualFile SaveDataController::OpenMetaFile(FileSys::SaveDataSpaceId space,
                                                      const FileSys::SaveDataAttribute& attribute,
                                                      FileSys::SaveDataMetaType meta_type) {
    LOG_TRACE(Service_FS, "Opening the meta file of the save data of the space_id={:01X}, "
                          "save_struct={}",
              space, attribute.DebugInfo());
    return factory->OpenMetaFile(space, attribute, meta_type);
}

s64 SaveDataController::QueryTotalSize(FileSys::SaveDataSpaceId space) {
    LOG_TRACE(Service_FS, "Getting the total size of the space_id={:01X}", space);
    return factory->QueryTotalSize(space);
}

FileSys::SaveDataSize SaveDataController::ReadSaveDataSize(FileSys::SaveDataType type, u64 title_id,
                                                           u128 user_id) {
    const auto value = factory->ReadSaveDataSize(type, title_id, user_id);

    if (value.normal == 0 && value.journal == 0) {
        const auto size = GetDefaultSaveDataSize(system, title_id);
        factory->WriteSaveDataSize(type, title_id, user_id, size);
        return size;
    }

    return value;
}

void SaveDataController::WriteSaveDataSize(FileSys::SaveDataType type, u64 title_id, u128 user_id,
                                           FileSys::SaveDataSize new_value) {
    factory->WriteSaveDataSize(type, title_id, user_id, new_value);
}

void SaveDataController::SetAutoCreate(bool state) {
    factory->SetAutoCreate(state);
}

} // namespace Service::FileSystem
