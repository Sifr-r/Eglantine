// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>
#include "core/file_sys/savedata_factory.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fs_i_save_data_info_reader.h"
#include "core/hle/service/filesystem/save_data_controller.h"

namespace Service::FileSystem {

namespace {

/// Checks a single enumerated save against a save data filter.
/// A field is only compared when its use_* flag is set, the rank is never compared.
bool MatchesFilter(const ISaveDataInfoReader::SaveDataInfo& entry, const FileSys::SaveDataFilter& filter) {
    const auto& attribute = filter.attribute;

    if (filter.use_program_id && entry.title_id != attribute.program_id) {
        return false;
    }

    if (filter.use_save_data_type && entry.type != attribute.type) {
        return false;
    }

    if (filter.use_user_id) {
        std::array<u8, 0x10> user_id{};
        std::memcpy(user_id.data(), attribute.user_id.data(), user_id.size());
        if (entry.user_id != user_id) {
            return false;
        }
    }

    if (filter.use_save_data_id && entry.save_id != attribute.system_save_data_id) {
        return false;
    }

    if (filter.use_index && entry.index != attribute.index) {
        return false;
    }

    return true;
}

} // Anonymous namespace

ISaveDataInfoReader::ISaveDataInfoReader(Core::System& system_,
                                         std::shared_ptr<SaveDataController> save_data_controller_,
                                         FileSys::SaveDataSpaceId space,
                                         std::optional<FileSys::SaveDataFilter> filter_)
    : ServiceFramework{system_, "ISaveDataInfoReader"},
      save_data_controller{save_data_controller_}, filter{std::move(filter_)} {
    static const FunctionInfo functions[] = {
        {0, D<&ISaveDataInfoReader::ReadSaveDataInfo>, "ReadSaveDataInfo"},
    };
    RegisterHandlers(functions);

    FindAllSaves(space);

    if (this->filter.has_value()) {
        ApplyFilter();
    }
}

ISaveDataInfoReader::~ISaveDataInfoReader() = default;

Result ISaveDataInfoReader::ReadSaveDataInfo(
    Out<u64> out_count, OutArray<SaveDataInfo, BufferAttr_HipcMapAlias> out_entries) {
    LOG_DEBUG(Service_FS, "called");

    // Calculate how many entries we can fit in the output buffer
    const u64 count_entries = out_entries.size();

    // Cap at total number of entries.
    const u64 actual_entries = (std::min)(count_entries, info.size() - next_entry_index);

    // Determine data start and end
    const auto* begin = reinterpret_cast<u8*>(info.data() + next_entry_index);
    const auto* end = reinterpret_cast<u8*>(info.data() + next_entry_index + actual_entries);
    const auto range_size = static_cast<std::size_t>(std::distance(begin, end));

    next_entry_index += actual_entries;

    // Write the data to memory
    std::memcpy(out_entries.data(), begin, range_size);
    *out_count = actual_entries;

    R_SUCCEED();
}

void ISaveDataInfoReader::ApplyFilter() {
    const auto& active_filter = *filter;

    info.erase(std::remove_if(info.begin(), info.end(),
                               [&active_filter](const SaveDataInfo& entry) {
                                   return !MatchesFilter(entry, active_filter);
                               }),
               info.end());

    LOG_DEBUG(Service_FS, "Applied save data filter, {} entries remain", info.size());
}

void ISaveDataInfoReader::FindAllSaves(FileSys::SaveDataSpaceId space) {
    const auto entries = save_data_controller->EnumerateSaveData(space);
    if (entries.empty()) {
        return;
    }

    FileSys::VirtualDir save_root{};
    if (const auto result = save_data_controller->OpenSaveDataSpace(&save_root, space);
        result != ResultSuccess || save_root == nullptr) {
        LOG_ERROR(Service_FS, "The save root for the space_id={:02X} was invalid!", space);
        save_root.reset();
    }

    info.reserve(entries.size());
    for (const auto& entry : entries) {
        u64 save_image_size = 0;
        if (save_root != nullptr) {
            if (const auto save_dir = save_root->GetDirectoryRelative(entry.relative_path);
                save_dir != nullptr) {
                save_image_size = save_dir->GetSize();
            }
        }

        SaveDataInfo save_info{};
        save_info.save_id_unknown = FileSys::GetSaveDataId(entry);
        save_info.space = entry.space;
        save_info.type = entry.type;
        std::memcpy(save_info.user_id.data(), entry.user_id.data(), save_info.user_id.size());
        save_info.save_id = entry.system_save_data_id;
        save_info.title_id = entry.title_id;
        save_info.save_image_size = save_image_size;
        save_info.index = entry.index;
        save_info.rank = entry.rank;
        info.emplace_back(save_info);
    }
}

} // namespace Service::FileSystem
