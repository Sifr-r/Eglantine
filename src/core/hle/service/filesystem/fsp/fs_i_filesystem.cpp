// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <string_view>

#include "common/string_util.h"
#include "core/file_sys/errors.h"
#include "core/file_sys/fssrv/fssrv_sf_path.h"
#include "core/hle/service/cmif_serialization.h"
#include "core/hle/service/filesystem/fsp/fs_i_directory.h"
#include "core/hle/service/filesystem/fsp/fs_i_file.h"
#include "core/hle/service/filesystem/fsp/fs_i_filesystem.h"

namespace Service::FileSystem {

namespace {

Result ExtractBoundedPath(const FileSys::Sf::Path& path, std::string_view& out_path) {
    const auto* begin = path.str;
    const auto* end = std::find(begin, begin + sizeof(path.str), '\0');
    R_UNLESS(end != begin + sizeof(path.str), FileSys::ResultInvalidPath);
    out_path = std::string_view{begin, static_cast<std::size_t>(end - begin)};
    R_SUCCEED();
}

} // namespace

IFileSystem::IFileSystem(Core::System& system_, FileSys::VirtualDir dir_, SizeGetter size_getter_)
    : ServiceFramework{system_, "IFileSystem"}, backend{std::make_unique<FileSys::Fsa::IFileSystem>(
                                                    dir_)},
      size_getter{std::move(size_getter_)} {
    static const FunctionInfo functions[] = {
        {0, D<&IFileSystem::CreateFile>, "CreateFile"},
        {1, D<&IFileSystem::DeleteFile>, "DeleteFile"},
        {2, D<&IFileSystem::CreateDirectory>, "CreateDirectory"},
        {3, D<&IFileSystem::DeleteDirectory>, "DeleteDirectory"},
        {4, D<&IFileSystem::DeleteDirectoryRecursively>, "DeleteDirectoryRecursively"},
        {5, D<&IFileSystem::RenameFile>, "RenameFile"},
        {6, D<&IFileSystem::RenameDirectory>, "RenameDirectory"},
        {7, D<&IFileSystem::GetEntryType>, "GetEntryType"},
        {8, D<&IFileSystem::OpenFile>, "OpenFile"},
        {9, D<&IFileSystem::OpenDirectory>, "OpenDirectory"},
        {10, D<&IFileSystem::Commit>, "Commit"},
        {11, D<&IFileSystem::GetFreeSpaceSize>, "GetFreeSpaceSize"},
        {12, D<&IFileSystem::GetTotalSpaceSize>, "GetTotalSpaceSize"},
        {13, D<&IFileSystem::CleanDirectoryRecursively>, "CleanDirectoryRecursively"},
        {14, D<&IFileSystem::GetFileTimeStampRaw>, "GetFileTimeStampRaw"},
        {15, nullptr, "QueryEntry"},
        {16, D<&IFileSystem::GetFileSystemAttribute>, "GetFileSystemAttribute"},
    };
    RegisterHandlers(functions);
}

Result IFileSystem::CreateFile(const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path,
                               s32 option, s64 size) {
    std::string_view path_view{};
    R_TRY(ExtractBoundedPath(*path, path_view));
    LOG_DEBUG(Service_FS, "called. file={}, option={:#x}, size={:#08x}", path_view, option, size);

    R_RETURN(backend->CreateFile(FileSys::Path(path_view.data()), size));
}

Result IFileSystem::DeleteFile(const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path) {
    std::string_view path_view{};
    R_TRY(ExtractBoundedPath(*path, path_view));
    LOG_DEBUG(Service_FS, "called. file={}", path_view);

    R_RETURN(backend->DeleteFile(FileSys::Path(path_view.data())));
}

Result IFileSystem::CreateDirectory(
    const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path) {
    std::string_view path_view{};
    R_TRY(ExtractBoundedPath(*path, path_view));
    LOG_DEBUG(Service_FS, "called. directory={}", path_view);

    R_RETURN(backend->CreateDirectory(FileSys::Path(path_view.data())));
}

Result IFileSystem::DeleteDirectory(
    const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path) {
    std::string_view path_view{};
    R_TRY(ExtractBoundedPath(*path, path_view));
    LOG_DEBUG(Service_FS, "called. directory={}", path_view);

    R_RETURN(backend->DeleteDirectory(FileSys::Path(path_view.data())));
}

Result IFileSystem::DeleteDirectoryRecursively(
    const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path) {
    std::string_view path_view{};
    R_TRY(ExtractBoundedPath(*path, path_view));
    LOG_DEBUG(Service_FS, "called. directory={}", path_view);

    R_RETURN(backend->DeleteDirectoryRecursively(FileSys::Path(path_view.data())));
}

Result IFileSystem::CleanDirectoryRecursively(
    const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path) {
    std::string_view path_view{};
    R_TRY(ExtractBoundedPath(*path, path_view));
    LOG_DEBUG(Service_FS, "called. Directory: {}", path_view);

    R_RETURN(backend->CleanDirectoryRecursively(FileSys::Path(path_view.data())));
}

Result IFileSystem::RenameFile(
    const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> old_path,
    const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> new_path) {
    std::string_view old_view{};
    std::string_view new_view{};
    R_TRY(ExtractBoundedPath(*old_path, old_view));
    R_TRY(ExtractBoundedPath(*new_path, new_view));
    LOG_DEBUG(Service_FS, "called. file '{}' to file '{}'", old_view, new_view);

    R_RETURN(backend->RenameFile(FileSys::Path(old_view.data()), FileSys::Path(new_view.data())));
}

Result IFileSystem::RenameDirectory(
    const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> old_path,
    const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> new_path) {
    std::string_view old_view{};
    std::string_view new_view{};
    R_TRY(ExtractBoundedPath(*old_path, old_view));
    R_TRY(ExtractBoundedPath(*new_path, new_view));
    LOG_DEBUG(Service_FS, "called. directory '{}' to directory '{}'", old_view, new_view);

    R_RETURN(
        backend->RenameDirectory(FileSys::Path(old_view.data()), FileSys::Path(new_view.data())));
}

Result IFileSystem::OpenFile(OutInterface<IFile> out_interface,
                             const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path,
                             u32 mode) {
    std::string_view path_view{};
    R_TRY(ExtractBoundedPath(*path, path_view));
    LOG_DEBUG(Service_FS, "called. file={}, mode={}", path_view, mode);

    FileSys::VirtualFile vfs_file{};
    R_TRY(backend->OpenFile(&vfs_file, FileSys::Path(path_view.data()),
                            static_cast<FileSys::OpenMode>(mode)));

    *out_interface = std::make_shared<IFile>(system, vfs_file);
    R_SUCCEED();
}

Result IFileSystem::OpenDirectory(OutInterface<IDirectory> out_interface,
                                  const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path,
                                  u32 mode) {
    std::string_view path_view{};
    R_TRY(ExtractBoundedPath(*path, path_view));
    LOG_DEBUG(Service_FS, "called. directory={}, mode={}", path_view, mode);

    FileSys::VirtualDir vfs_dir{};
    R_TRY(backend->OpenDirectory(&vfs_dir, FileSys::Path(path_view.data()),
                                 static_cast<FileSys::OpenDirectoryMode>(mode)));

    *out_interface = std::make_shared<IDirectory>(system, vfs_dir,
                                                  static_cast<FileSys::OpenDirectoryMode>(mode));
    R_SUCCEED();
}

Result IFileSystem::GetEntryType(
    Out<u32> out_type, const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path) {
    std::string_view path_view{};
    R_TRY(ExtractBoundedPath(*path, path_view));
    LOG_DEBUG(Service_FS, "called. file={}", path_view);

    FileSys::DirectoryEntryType vfs_entry_type{};
    R_TRY(backend->GetEntryType(&vfs_entry_type, FileSys::Path(path_view.data())));

    *out_type = static_cast<u32>(vfs_entry_type);
    R_SUCCEED();
}

Result IFileSystem::Commit() {
    LOG_WARNING(Service_FS, "(STUBBED) called");

    R_SUCCEED();
}

Result IFileSystem::GetFreeSpaceSize(
    Out<s64> out_size, const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path) {
    LOG_DEBUG(Service_FS, "called");

    *out_size = size_getter.get_free_size();
    R_SUCCEED();
}

Result IFileSystem::GetTotalSpaceSize(
    Out<s64> out_size, const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path) {
    LOG_DEBUG(Service_FS, "called");

    *out_size = size_getter.get_total_size();
    R_SUCCEED();
}

Result IFileSystem::GetFileTimeStampRaw(
    Out<FileSys::FileTimeStampRaw> out_timestamp,
    const InLargeData<FileSys::Sf::Path, BufferAttr_HipcPointer> path) {
    std::string_view path_view{};
    R_TRY(ExtractBoundedPath(*path, path_view));
    LOG_DEBUG(Service_FS, "(Partial Implementation) called. file={}", path_view);

    FileSys::FileTimeStampRaw vfs_timestamp{};
    R_TRY(backend->GetFileTimeStampRaw(&vfs_timestamp, FileSys::Path(path_view.data())));

    *out_timestamp = vfs_timestamp;
    R_SUCCEED();
}

Result IFileSystem::GetFileSystemAttribute(Out<FileSys::FileSystemAttribute> out_attribute) {
    LOG_WARNING(Service_FS, "(STUBBED) called");

    constexpr s32 kEntryNameLengthMax = 0x80;
    constexpr s32 kPathLengthMax = 0x300;

    FileSys::FileSystemAttribute savedata_attribute{};

    savedata_attribute.dir_entry_name_length_max_defined = true;
    savedata_attribute.file_entry_name_length_max_defined = true;
    savedata_attribute.dir_path_name_length_max_defined = true;
    savedata_attribute.file_path_name_length_max_defined = true;

    savedata_attribute.utf16_create_dir_path_len_max_defined = true;
    savedata_attribute.utf16_delete_dir_path_len_max_defined = true;
    savedata_attribute.utf16_rename_src_dir_path_len_max_defined = true;
    savedata_attribute.utf16_rename_dest_dir_path_len_max_defined = true;
    savedata_attribute.utf16_open_dir_path_len_max_defined = true;

    savedata_attribute.utf16_dir_entry_name_length_max_defined = true;
    savedata_attribute.utf16_file_entry_name_length_max_defined = true;
    savedata_attribute.utf16_dir_path_name_length_max_defined = true;
    savedata_attribute.utf16_file_path_name_length_max_defined = true;

    savedata_attribute.dir_entry_name_length_max = kEntryNameLengthMax;
    savedata_attribute.file_entry_name_length_max = kEntryNameLengthMax;
    savedata_attribute.dir_path_name_length_max = kPathLengthMax;
    savedata_attribute.file_path_name_length_max = kPathLengthMax;

    savedata_attribute.utf16_create_dir_path_length_max = kPathLengthMax;
    savedata_attribute.utf16_delete_dir_path_length_max = kPathLengthMax;
    savedata_attribute.utf16_rename_src_dir_path_length_max = kPathLengthMax;
    savedata_attribute.utf16_rename_dest_dir_path_length_max = kPathLengthMax;
    savedata_attribute.utf16_open_dir_path_length_max = kPathLengthMax;

    savedata_attribute.utf16_dir_entry_name_length_max = kEntryNameLengthMax;
    savedata_attribute.utf16_file_entry_name_length_max = kEntryNameLengthMax;
    savedata_attribute.utf16_dir_path_name_length_max = kPathLengthMax;
    savedata_attribute.utf16_file_path_name_length_max = kPathLengthMax;

    *out_attribute = savedata_attribute;
    R_SUCCEED();
}

} // namespace Service::FileSystem
