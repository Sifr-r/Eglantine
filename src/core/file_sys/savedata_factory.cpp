// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <fmt/format.h>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/uuid.h"
#include "core/core.h"
#include "core/file_sys/errors.h"
#include "core/file_sys/savedata_factory.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/hle/service/acc/profile_manager.h"

namespace FileSys {

namespace {

bool ShouldSaveDataBeAutomaticallyCreated(SaveDataSpaceId space, const SaveDataAttribute& attr) {
    return attr.type == SaveDataType::Cache || attr.type == SaveDataType::Temporary ||
           (space == SaveDataSpaceId::User && ///< Normal Save Data -- Current Title & User
            (attr.type == SaveDataType::Account || attr.type == SaveDataType::Device) &&
            attr.program_id == 0 && attr.system_save_data_id == 0);
}

std::string GetFutureSaveDataPath(SaveDataSpaceId space_id, SaveDataType type, u64 title_id,
                                  u128 user_id) {
    // Only detect nand user saves.
    const auto space_id_path = [space_id]() -> std::string_view {
        switch (space_id) {
        case SaveDataSpaceId::User:
            return "/user/save";
        default:
            return "";
        }
    }();

    if (space_id_path.empty()) {
        return "";
    }

    Common::UUID uuid;
    std::memcpy(uuid.uuid.data(), user_id.data(), sizeof(Common::UUID));

    // Only detect account/device saves from the future location.
    switch (type) {
    case SaveDataType::Account:
        return fmt::format("{}/account/{}/{:016X}/0", space_id_path, uuid.RawString(), title_id);
    case SaveDataType::Device:
        return fmt::format("{}/device/{:016X}/0", space_id_path, title_id);
    default:
        return "";
    }
}

// The emulator does not enforce a quota on the save data directory, so the free space that is
// reported to the guest is derived from a nominal size of the space. It matches the default size
// of a title save.
constexpr s64 NominalSpaceSize = 0xF0000000;

std::optional<UserId> GetActiveUserId(const Core::System& system) {
    const s32 current_user = Settings::values.current_user.GetValue();
    if (current_user < 0) {
        return std::nullopt;
    }

    const auto& manager = system.GetProfileManager();
    const auto index = static_cast<std::size_t>(current_user);

    if (!manager.UserExistsIndex(index)) {
        return std::nullopt;
    }

    const auto user_id = manager.GetUser(index);
    if (!user_id.has_value() || !user_id->IsValid()) {
        return std::nullopt;
    }

    return user_id->AsU128();
}

// The state that is persisted next to a save data to track its commit id.
struct SaveDataCommitRecord {
    u64 commit_id;
    u64 file_count;
    u64 total_size;
};

/// Returns whether the space is one that maps to a save data directory.
bool IsSupportedSpace(SaveDataSpaceId space) {
    switch (space) {
    case SaveDataSpaceId::System:
    case SaveDataSpaceId::User:
    case SaveDataSpaceId::SdUser:
    case SaveDataSpaceId::Temporary:
        return true;
    default:
        return false;
    }
}

/// Returns whether the type is one that the on disk layout can store.
bool IsSupportedType(SaveDataType type) {
    switch (type) {
    case SaveDataType::System:
    case SaveDataType::Account:
    case SaveDataType::Device:
    case SaveDataType::Temporary:
    case SaveDataType::Cache:
        return true;
    default:
        return false;
    }
}

bool IsHexString(std::string_view text) {
    return !text.empty() &&
           std::ranges::all_of(text, [](unsigned char ch) { return std::isxdigit(ch) != 0; });
}

/// Parses a 16 character hexadecimal directory name into the number it holds.
u64 ParseSaveId(std::string_view name) {
    if (name.size() != 16 || !IsHexString(name)) {
        return 0;
    }

    const auto bytes = Common::HexStringToArray<0x8>(name);
    u64 out{};
    for (const auto byte : bytes) {
        out = (out << 8) | byte;
    }

    return out;
}

/// Parses a 32 character hexadecimal user id directory name into the user id it holds.
///
/// The directory holds the user id as two big endian halves, the high half first.
UserId ParseUserId(std::string_view name) {
    if (name.size() != 0x20 || !IsHexString(name)) {
        return InvalidUserId;
    }

    const u64 high = ParseSaveId(name.substr(0, 16));
    const u64 low = ParseSaveId(name.substr(16, 16));
    return UserId{low, high};
}

std::optional<u16> ParseCacheIndex(std::string_view text) {
    if (text.size() != 4 || !IsHexString(text)) {
        return std::nullopt;
    }

    const auto bytes = Common::HexStringToArray<0x2>(text);
    return static_cast<u16>((static_cast<u16>(bytes[0]) << 8) | bytes[1]);
}

/// Resolves the relative save path for an attribute, including cache storage indices.
std::string GetSaveDataPath(ProgramId program_id, const VirtualDir& dir, SaveDataSpaceId space,
                            const SaveDataAttribute& meta) {
    auto path = SaveDataFactory::GetFullPath(program_id, dir, space, meta.type, meta.program_id,
                                             meta.user_id, meta.system_save_data_id);
    if (meta.type == SaveDataType::Cache && meta.index != 0) {
        return fmt::format("{}_{:04X}", path, meta.index);
    }
    return path;
}

/// Returns the name of the meta file of a save data, if the meta type has one.
std::optional<std::string_view> GetMetaFileName(SaveDataMetaType meta_type) {
    switch (meta_type) {
    case SaveDataMetaType::Thumbnail:
        return "meta";
    case SaveDataMetaType::ExtensionContext:
        return "meta0";
    default:
        return std::nullopt;
    }
}

/// Returns whether the path points inside the root of the space.
bool IsWithinSpace(SaveDataSpaceId space, const std::string& path) {
    const auto root = SaveDataFactory::GetSaveDataSpaceIdPath(space);
    return path.size() > root.size() && path.compare(0, root.size(), root) == 0;
}

/// Removes the save directory at path, which has to be a child of the save data root.
Result DeleteSaveDirectory(const VirtualDir& root, const std::string& path) {
    const auto parent_path = Common::FS::GetParentPath(path);
    const auto leaf = std::string(Common::FS::GetFilename(path));

    // Only a single directory name may be removed, so that a malformed path cannot escape the
    // save data space.
    if (leaf.empty() || leaf.find_first_of("/\\") != std::string::npos) {
        LOG_ERROR(Service_FS, "Refusing to delete the invalid save data path: {}", path);
        return FileSys::ResultInvalidArgument;
    }

    const auto parent = root->GetDirectoryRelative(parent_path);
    if (parent == nullptr || parent->GetSubdirectory(leaf) == nullptr) {
        LOG_ERROR(Service_FS, "The save data does not exist: {}", path);
        return FileSys::ResultTargetNotFound;
    }

    if (!parent->DeleteSubdirectoryRecursive(leaf)) {
        LOG_ERROR(Service_FS, "Failed to delete the save data: {}", path);
        return FileSys::ResultPermissionDenied;
    }

    return ResultSuccess;
}

/// Builds the attribute that addresses an enumerated save data.
SaveDataAttribute AttributeFromEntry(const SaveDataEntry& entry) {
    SaveDataAttribute attribute{};
    attribute.type = entry.type;
    attribute.user_id = entry.user_id;
    attribute.rank = SaveDataRank::Primary;
    attribute.index = entry.index;

    if (entry.type == SaveDataType::System) {
        attribute.system_save_data_id = entry.system_save_data_id;
    } else {
        attribute.program_id = entry.title_id;
    }

    return attribute;
}

/// Returns whether an enumerated save data matches the filter.
///
/// Only the fields the filter enables are compared, the rank is never compared because the on
/// disk layout does not store it.
bool MatchesFilter(const SaveDataEntry& entry, const SaveDataFilter& filter) {
    const auto& attribute = filter.attribute;

    if (filter.use_program_id && attribute.program_id != entry.title_id) {
        return false;
    }
    if (filter.use_save_data_type && attribute.type != entry.type) {
        return false;
    }
    if (filter.use_user_id && attribute.user_id != entry.user_id) {
        return false;
    }
    if (filter.use_save_data_id && attribute.system_save_data_id != entry.system_save_data_id) {
        return false;
    }
    if (filter.use_index && attribute.index != entry.index) {
        return false;
    }

    return true;
}

/// Summarizes the contents of a save directory, so that a change of it can be detected.
SaveDataCommitRecord GetSaveContents(const VirtualDir& save_dir) {
    SaveDataCommitRecord contents{};

    for (const auto& file : save_dir->GetFiles()) {
        const auto& name = file->GetName();
        if (name == GetSaveDataCommitIdFileName() || name == GetSaveDataSizeFileName()) {
            // Sidecar metadata tracks the save data, it is not part of it.
            continue;
        }
        contents.file_count++;
        contents.total_size += file->GetSize();
    }

    for (const auto& sub_dir : save_dir->GetSubdirectories()) {
        contents.file_count++;
        contents.total_size += sub_dir->GetSize();
    }

    return contents;
}

/// Collects the saves of the save directory of a space.
///
/// System saves are stored as save/<save id>/<user id>, account and device saves as
/// save/0/<user id>/<title id> and cache saves as save/cache/<title id>.
void EnumerateNormalSaves(SaveDataSpaceId space, const VirtualDir& save_dir,
                          std::vector<SaveDataEntry>& out) {
    for (const auto& save_id : save_dir->GetSubdirectories()) {
        const auto& save_id_name = save_id->GetName();

        if (save_id_name == "cache") {
            for (const auto& title : save_id->GetSubdirectories()) {
                const auto& name = title->GetName();
                u64 title_id = 0;
                u16 index = 0;

                if (name.size() == 16) {
                    title_id = ParseSaveId(name);
                } else if (name.size() == 21 && name[16] == '_') {
                    const auto parsed_index = ParseCacheIndex(std::string_view{name}.substr(17, 4));
                    if (!parsed_index.has_value()) {
                        continue;
                    }
                    title_id = ParseSaveId(std::string_view{name}.substr(0, 16));
                    index = *parsed_index;
                } else {
                    continue;
                }

                if (title_id == 0) {
                    continue;
                }
                out.push_back(SaveDataEntry{
                    .space = space,
                    .type = SaveDataType::Cache,
                    .rank = SaveDataRank::Primary,
                    .system_save_data_id = 0,
                    .title_id = title_id,
                    .user_id = InvalidUserId,
                    .index = index,
                    .relative_path = "save/cache/" + name,
                });
            }
            continue;
        }

        if (save_id_name.size() != 16 || !IsHexString(save_id_name)) {
            continue;
        }

        const auto system_save_data_id = ParseSaveId(save_id_name);
        for (const auto& user_dir : save_id->GetSubdirectories()) {
            const auto& user_id_name = user_dir->GetName();
            if (user_id_name.size() != 0x20 || !IsHexString(user_id_name)) {
                // Not a user id directory.
                continue;
            }
            const auto user_id = ParseUserId(user_id_name);

            if (system_save_data_id != 0) {
                out.push_back(SaveDataEntry{
                    .space = space,
                    .type = SaveDataType::System,
                    .rank = SaveDataRank::Primary,
                    .system_save_data_id = system_save_data_id,
                    .title_id = 0,
                    .user_id = user_id,
                    .index = 0,
                    .relative_path = "save/" + save_id_name + "/" + user_id_name,
                });
                continue;
            }

            // A user id of zero marks a device save.
            const auto type = user_id == InvalidUserId ? SaveDataType::Device : SaveDataType::Account;
            for (const auto& title : user_dir->GetSubdirectories()) {
                const auto title_id = ParseSaveId(title->GetName());
                if (title_id == 0) {
                    continue;
                }
                out.push_back(SaveDataEntry{
                    .space = space,
                    .type = type,
                    .rank = SaveDataRank::Primary,
                    .system_save_data_id = 0,
                    .title_id = title_id,
                    .user_id = user_id,
                    .index = 0,
                    .relative_path =
                        "save/" + save_id_name + "/" + user_id_name + "/" + title->GetName(),
                });
            }
        }
    }
}

/// Collects the temporary saves of a space, which are stored as <save id>/<user id>/<title id>.
void EnumerateTemporarySaves(SaveDataSpaceId space, const VirtualDir& save_id_dir,
                             std::vector<SaveDataEntry>& out) {
    const auto& save_id_name = save_id_dir->GetName();
    if (save_id_name != "0000000000000000") {
        // Temporary saves have no system save id; GetFullPath always addresses the zero root.
        return;
    }

    for (const auto& user_dir : save_id_dir->GetSubdirectories()) {
        const auto& user_id_name = user_dir->GetName();
        if (user_id_name.size() != 0x20 || !IsHexString(user_id_name)) {
            // Not a user id directory.
            continue;
        }
        const auto user_id = ParseUserId(user_id_name);

        for (const auto& title : user_dir->GetSubdirectories()) {
            const auto title_id = ParseSaveId(title->GetName());
            if (title_id == 0) {
                continue;
            }
            out.push_back(SaveDataEntry{
                .space = space,
                .type = SaveDataType::Temporary,
                .rank = SaveDataRank::Primary,
                .system_save_data_id = 0,
                .title_id = title_id,
                .user_id = user_id,
                .index = 0,
                .relative_path = save_id_name + "/" + user_id_name + "/" + title->GetName(),
            });
        }
    }
}

} // Anonymous namespace

u64 GetSaveDataId(const SaveDataEntry& entry) {
    if (entry.type == SaveDataType::System) {
        return entry.system_save_data_id;
    }

    // Encode each field explicitly so IDs do not depend on padding, endianness or directory order.
    u64 hash = 0xCBF29CE484222325ULL;
    for (const u64 field : {static_cast<u64>(entry.space), static_cast<u64>(entry.type),
                            static_cast<u64>(entry.rank),
                            entry.title_id, entry.user_id[0], entry.user_id[1],
                            static_cast<u64>(entry.index)}) {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            hash ^= (field >> shift) & 0xFF;
            hash *= 0x100000001B3ULL;
        }
    }
    return 0x4000000000000000ULL | (hash & 0x0FFFFFFFFFFFFFFFULL);
}

const SaveDataEntry* FindSaveDataEntryById(std::span<const SaveDataEntry> entries,
                                         u64 save_data_id, std::optional<UserId> active_user) {
    if (save_data_id == 0) {
        return nullptr;
    }

    const SaveDataEntry* matched = nullptr;
    for (const auto& entry : entries) {
        const bool exact_match = GetSaveDataId(entry) == save_data_id;
        const bool legacy_match =
            entry.type != SaveDataType::System && entry.title_id == save_data_id &&
            (entry.type != SaveDataType::Account || !active_user.has_value() ||
             entry.user_id == *active_user);
        if (!exact_match && !legacy_match) {
            continue;
        }
        if (matched != nullptr) {
            return nullptr;
        }
        matched = &entry;
    }
    return matched;
}

SaveDataFactory::SaveDataFactory(Core::System& system_, ProgramId program_id_,
                                 VirtualDir save_directory_)
    : system{system_}, program_id{program_id_}, dir{std::move(save_directory_)} {}

SaveDataFactory::~SaveDataFactory() = default;

VirtualDir SaveDataFactory::Create(SaveDataSpaceId space, const SaveDataAttribute& meta) const {
    const auto save_directory = GetSaveDataPath(program_id, dir, space, meta);

    return dir->CreateDirectoryRelative(save_directory);
}

VirtualDir SaveDataFactory::Open(SaveDataSpaceId space, const SaveDataAttribute& meta) const {
    const auto save_directory = GetSaveDataPath(program_id, dir, space, meta);

    auto out = dir->GetDirectoryRelative(save_directory);

    if (out == nullptr && (ShouldSaveDataBeAutomaticallyCreated(space, meta) && auto_create)) {
        return Create(space, meta);
    }

    return out;
}

VirtualDir SaveDataFactory::GetSaveDataSpaceDirectory(SaveDataSpaceId space) const {
    return dir->GetDirectoryRelative(GetSaveDataSpaceIdPath(space));
}

std::string SaveDataFactory::GetSaveDataSpaceIdPath(SaveDataSpaceId space) {
    switch (space) {
    case SaveDataSpaceId::System:
        return "/system/";
    case SaveDataSpaceId::User:
    case SaveDataSpaceId::SdUser:
        return "/user/";
    case SaveDataSpaceId::Temporary:
        return "/temp/";
    default:
        ASSERT_MSG(false, "Unrecognized SaveDataSpaceId: {:02X}", static_cast<u8>(space));
        return "/unrecognized/"; ///< To prevent corruption when ignoring asserts.
    }
}

std::string SaveDataFactory::GetFullPath(ProgramId program_id, VirtualDir dir,
                                         SaveDataSpaceId space, SaveDataType type, u64 title_id,
                                         u128 user_id, u64 save_id) {
    // According to switchbrew, if a save is of type SaveData and the title id field is 0, it should
    // be interpreted as the title id of the current process.
    if (type == SaveDataType::Account || type == SaveDataType::Device ||
        type == SaveDataType::Temporary || type == SaveDataType::Cache) {
        if (title_id == 0) {
            title_id = program_id;
        }
    }

    // For compat with a future impl.
    if (std::string future_path =
            GetFutureSaveDataPath(space, type, title_id & ~(0xFFULL), user_id);
        !future_path.empty()) {
        // Check if this location exists, and prefer it over the old.
        if (const auto future_dir = dir->GetDirectoryRelative(future_path); future_dir != nullptr) {
            LOG_INFO(Service_FS, "Using save at new location: {}", future_path);
            return future_path;
        }
    }

    std::string out = GetSaveDataSpaceIdPath(space);

    switch (type) {
    case SaveDataType::System:
        return fmt::format("{}save/{:016X}/{:016X}{:016X}", out, save_id, user_id[1], user_id[0]);
    case SaveDataType::Account:
    case SaveDataType::Device:
        return fmt::format("{}save/{:016X}/{:016X}{:016X}/{:016X}", out, 0, user_id[1], user_id[0],
                           title_id);
    case SaveDataType::Temporary:
        return fmt::format("{}{:016X}/{:016X}{:016X}/{:016X}", out, 0, user_id[1], user_id[0],
                           title_id);
    case SaveDataType::Cache:
        return fmt::format("{}save/cache/{:016X}", out, title_id);
    default:
        ASSERT_MSG(false, "Unrecognized SaveDataType: {:02X}", static_cast<u8>(type));
        return fmt::format("{}save/unknown_{:X}/{:016X}", out, static_cast<u8>(type), title_id);
    }
}

std::string SaveDataFactory::GetUserGameSaveDataRoot(u128 user_id, bool future) {
    if (future) {
        Common::UUID uuid;
        std::memcpy(uuid.uuid.data(), user_id.data(), sizeof(Common::UUID));
        return fmt::format("/user/save/account/{}", uuid.RawString());
    }
    return fmt::format("/user/save/{:016X}/{:016X}{:016X}", 0, user_id[1], user_id[0]);
}

SaveDataSize SaveDataFactory::ReadSaveDataSize(SaveDataType type, u64 title_id,
                                               u128 user_id) const {
    if (dir == nullptr) {
        return {0, 0};
    }

    const auto path =
        GetFullPath(program_id, dir, SaveDataSpaceId::User, type, title_id, user_id, 0);
    const auto relative_dir = GetOrCreateDirectoryRelative(dir, path);
    if (relative_dir == nullptr) {
        return {0, 0};
    }

    const auto size_file = relative_dir->GetFile(GetSaveDataSizeFileName());
    if (size_file == nullptr || size_file->GetSize() < sizeof(SaveDataSize)) {
        return {0, 0};
    }

    SaveDataSize out;
    if (size_file->ReadObject(&out) != sizeof(SaveDataSize)) {
        return {0, 0};
    }

    return out;
}

void SaveDataFactory::WriteSaveDataSize(SaveDataType type, u64 title_id, u128 user_id,
                                        SaveDataSize new_value) const {
    if (dir == nullptr) {
        return;
    }

    const auto path =
        GetFullPath(program_id, dir, SaveDataSpaceId::User, type, title_id, user_id, 0);
    const auto relative_dir = GetOrCreateDirectoryRelative(dir, path);
    if (relative_dir == nullptr) {
        return;
    }

    const auto size_file = relative_dir->CreateFile(GetSaveDataSizeFileName());
    if (size_file == nullptr) {
        return;
    }

    size_file->Resize(sizeof(SaveDataSize));
    size_file->WriteObject(new_value);
}

std::vector<SaveDataEntry> SaveDataFactory::Enumerate(SaveDataSpaceId space) const {
    std::vector<SaveDataEntry> entries;

    if (!IsSupportedSpace(space)) {
        LOG_ERROR(Service_FS, "Unsupported save data space: {:02X}", static_cast<u8>(space));
        return entries;
    }

    const auto root = GetSaveDataSpaceDirectory(space);
    if (root == nullptr) {
        // The space does not exist yet.
        return entries;
    }

    for (const auto& sub_dir : root->GetSubdirectories()) {
        if (sub_dir->GetName() == "save") {
            EnumerateNormalSaves(space, sub_dir, entries);
        } else if (space == SaveDataSpaceId::Temporary) {
            EnumerateTemporarySaves(space, sub_dir, entries);
        }
    }

    // The order of the directories is not defined by the file system, so the entries are sorted
    // to keep the listing stable.
    std::ranges::sort(entries, {}, &SaveDataEntry::relative_path);
    return entries;
}

Result SaveDataFactory::Delete(SaveDataSpaceId space, const SaveDataAttribute& meta) const {
    if (!IsSupportedSpace(space)) {
        LOG_ERROR(Service_FS, "Unsupported save data space: {:02X}", static_cast<u8>(space));
        return FileSys::ResultInvalidArgument;
    }
    if (!IsSupportedType(meta.type)) {
        LOG_ERROR(Service_FS, "Unsupported save data type: {:02X}", static_cast<u8>(meta.type));
        return FileSys::ResultInvalidArgument;
    }
    if (meta.type == SaveDataType::System &&
        meta.system_save_data_id == InvalidSystemSaveDataId) {
        LOG_ERROR(Service_FS, "Refusing to delete system save data with id 0");
        return FileSys::ResultInvalidArgument;
    }
    if (meta.type != SaveDataType::System && meta.program_id == 0 && program_id == 0) {
        LOG_ERROR(Service_FS, "Refusing to delete save data with title id 0");
        return FileSys::ResultInvalidArgument;
    }

    const auto path = GetSaveDataPath(program_id, dir, space, meta);
    if (!IsWithinSpace(space, path)) {
        LOG_ERROR(Service_FS, "Refusing to delete the save data outside of its space: {}", path);
        return FileSys::ResultInvalidArgument;
    }

    return DeleteSaveDirectory(dir, path);
}

Result SaveDataFactory::DeleteMatching(SaveDataSpaceId space, const SaveDataFilter& filter,
                                       u64* out_deleted) const {
    if (out_deleted == nullptr) {
        LOG_ERROR(Service_FS, "The number of deleted save data cannot be stored");
        return FileSys::ResultInvalidArgument;
    }
    *out_deleted = 0;

    if (!IsSupportedSpace(space)) {
        LOG_ERROR(Service_FS, "Unsupported save data space: {:02X}", static_cast<u8>(space));
        return FileSys::ResultInvalidArgument;
    }
    if (filter.use_save_data_type && !IsSupportedType(filter.attribute.type)) {
        LOG_ERROR(Service_FS, "Unsupported save data type: {:02X}",
                  static_cast<u8>(filter.attribute.type));
        return FileSys::ResultInvalidArgument;
    }

    Result result = ResultSuccess;
    u64 deleted = 0;
    for (const auto& entry : Enumerate(space)) {
        if (!MatchesFilter(entry, filter)) {
            continue;
        }

        const auto delete_result = Delete(space, AttributeFromEntry(entry));
        if (delete_result.IsSuccess()) {
            deleted++;
        } else {
            LOG_ERROR(Service_FS, "Failed to delete the save data: {}", entry.relative_path);
            result = delete_result;
        }
    }

    *out_deleted = deleted;
    if (deleted == 0 && result.IsSuccess()) {
        LOG_ERROR(Service_FS, "No save data matched the filter");
        return FileSys::ResultTargetNotFound;
    }

    return result;
}

Result SaveDataFactory::DeleteCacheStorage(u16 index) const {
    if (program_id != 0) {
        const auto attribute =
            SaveDataAttribute::Make(program_id, SaveDataType::Cache, InvalidUserId, 0, index);
        const auto result = Delete(SaveDataSpaceId::User, attribute);
        if (result != FileSys::ResultTargetNotFound) {
            return result;
        }
    }

    // Legacy fallback: earlier code addressed cache storages directly under save/cache/{:016X}
    // by their 16-bit index/id regardless of program_id.
    const auto path = fmt::format("{}save/cache/{:016X}",
                                  GetSaveDataSpaceIdPath(SaveDataSpaceId::User), index);
    return DeleteSaveDirectory(dir, path);
}

s64 SaveDataFactory::GetFreeSpace(SaveDataSpaceId space) const {
    if (!IsSupportedSpace(space)) {
        LOG_ERROR(Service_FS, "Unsupported save data space: {:02X}", static_cast<u8>(space));
        return 0;
    }

    const auto root = GetSaveDataSpaceDirectory(space);
    if (root == nullptr) {
        // The space does not exist yet, so all of its nominal capacity is free.
        return NominalSpaceSize;
    }

    const auto used = static_cast<s64>(root->GetSize());
    const auto free_space = NominalSpaceSize - used;
    return free_space > 0 ? free_space : 0;
}

u64 SaveDataFactory::GetCommitId(SaveDataSpaceId space, u64 save_data_id) const {
    if (!IsSupportedSpace(space)) {
        LOG_ERROR(Service_FS, "Unsupported save data space: {:02X}", static_cast<u8>(space));
        return 0;
    }

    const auto root = GetSaveDataSpaceDirectory(space);
    if (root == nullptr) {
        return 0;
    }

    const auto entries = Enumerate(space);
    const auto* entry = FindSaveDataEntryById(entries, save_data_id, GetActiveUserId(system));

    if (entry == nullptr) {
        LOG_TRACE(Service_FS, "No save data with the id {:016X} in the space {:02X}", save_data_id,
                  static_cast<u8>(space));
        return 0;
    }

    const auto save_dir = root->GetDirectoryRelative(entry->relative_path);
    if (save_dir == nullptr) {
        return 0;
    }

    SaveDataCommitRecord record{};
    if (const auto counter_file = save_dir->GetFile(GetSaveDataCommitIdFileName());
        counter_file != nullptr && counter_file->GetSize() >= sizeof(record)) {
        if (counter_file->ReadObject(&record) != sizeof(record)) {
            LOG_ERROR(Service_FS, "Failed to read the commit id of the save data: {}",
                      entry->relative_path);
            record = {};
        }
    }

    // A save data that changed since the last query was committed, which issues a new commit id.
    const auto contents = GetSaveContents(save_dir);
    if (contents.file_count != record.file_count || contents.total_size != record.total_size) {
        record.commit_id++;
        record.file_count = contents.file_count;
        record.total_size = contents.total_size;

        const auto counter_file = save_dir->CreateFile(GetSaveDataCommitIdFileName());
        if (counter_file == nullptr) {
            LOG_ERROR(Service_FS, "Failed to store the commit id of the save data: {}",
                      entry->relative_path);
            return record.commit_id;
        }
        if (!counter_file->Resize(sizeof(record)) ||
            counter_file->WriteObject(record) != sizeof(record)) {
            LOG_ERROR(Service_FS, "Failed to store the commit id of the save data: {}",
                      entry->relative_path);
        }
    }

    return record.commit_id;
}

VirtualFile SaveDataFactory::OpenMetaFile(SaveDataSpaceId space, const SaveDataAttribute& meta,
                                           SaveDataMetaType meta_type) const {
    if (!IsSupportedSpace(space)) {
        LOG_ERROR(Service_FS, "Unsupported save data space: {:02X}", static_cast<u8>(space));
        return nullptr;
    }
    if (!IsSupportedType(meta.type)) {
        LOG_ERROR(Service_FS, "Unsupported save data type: {:02X}", static_cast<u8>(meta.type));
        return nullptr;
    }

    const auto meta_file_name = GetMetaFileName(meta_type);
    if (!meta_file_name.has_value()) {
        LOG_ERROR(Service_FS, "Unsupported save data meta type: {:02X}", static_cast<u8>(meta_type));
        return nullptr;
    }

    const auto path = GetSaveDataPath(program_id, dir, space, meta);
    const auto save_dir = dir->GetDirectoryRelative(path);
    if (save_dir == nullptr) {
        LOG_TRACE(Service_FS, "The save data does not exist: {}", path);
        return nullptr;
    }

    return save_dir->GetFile(*meta_file_name);
}

s64 SaveDataFactory::QueryTotalSize(SaveDataSpaceId space) const {
    if (!IsSupportedSpace(space)) {
        LOG_ERROR(Service_FS, "Unsupported save data space: {:02X}", static_cast<u8>(space));
        return 0;
    }

    const auto root = GetSaveDataSpaceDirectory(space);
    if (root == nullptr) {
        // The space does not exist yet.
        return 0;
    }

    s64 total_size = 0;
    for (const auto& entry : Enumerate(space)) {
        const auto save_dir = root->GetDirectoryRelative(entry.relative_path);
        if (save_dir == nullptr) {
            continue;
        }
        total_size += static_cast<s64>(save_dir->GetSize());
    }

    return total_size;
}

void SaveDataFactory::SetAutoCreate(bool state) {
    auto_create = state;
}

} // namespace FileSys
