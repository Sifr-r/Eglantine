// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/string_util.h"
#include "common/swap.h"
#include "core/file_sys/fsmitm_romfsbuild.h"
#include "core/file_sys/romfs.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/file_sys/vfs/vfs_cached.h"
#include "core/file_sys/vfs/vfs_concat.h"
#include "core/file_sys/vfs/vfs_offset.h"
#include "core/file_sys/vfs/vfs_vector.h"

namespace FileSys {
namespace {
constexpr u32 ROMFS_ENTRY_EMPTY = 0xFFFFFFFF;

struct TableLocation {
    u64_le offset;
    u64_le size;
};
static_assert(sizeof(TableLocation) == 0x10, "TableLocation has incorrect size.");

struct RomFSHeader {
    u64_le header_size;
    TableLocation directory_hash;
    TableLocation directory_meta;
    TableLocation file_hash;
    TableLocation file_meta;
    u64_le data_offset;
};
static_assert(sizeof(RomFSHeader) == 0x50, "RomFSHeader has incorrect size.");

struct DirectoryEntry {
    u32_le parent;
    u32_le sibling;
    u32_le child_dir;
    u32_le child_file;
    u32_le hash;
    u32_le name_length;
};
static_assert(sizeof(DirectoryEntry) == 0x18, "DirectoryEntry has incorrect size.");

struct FileEntry {
    u32_le parent;
    u32_le sibling;
    u64_le offset;
    u64_le size;
    u32_le hash;
    u32_le name_length;
};
static_assert(sizeof(FileEntry) == 0x20, "FileEntry has incorrect size.");

struct RomFSTraversalContext {
    RomFSHeader header;
    VirtualFile file;
    std::vector<u8> directory_meta;
    std::vector<u8> file_meta;
};

template <typename EntryType, auto Member>
std::optional<std::pair<EntryType, std::string>> GetEntry(const RomFSTraversalContext& ctx,
                                                          size_t offset) {
    const std::vector<u8>& vec = ctx.*Member;
    const size_t size = vec.size();
    if (offset > size || sizeof(EntryType) > size - offset) {
        return std::nullopt;
    }

    const size_t entry_end = offset + sizeof(EntryType);
    const u8* data = vec.data();
    EntryType entry{};
    std::memcpy(&entry, data + offset, sizeof(EntryType));

    const size_t name_length =
        (std::min)(static_cast<size_t>(entry.name_length), size - entry_end);
    std::string name(reinterpret_cast<const char*>(data + entry_end), name_length);

    return std::make_pair(entry, std::move(name));
}

std::optional<std::pair<DirectoryEntry, std::string>> GetDirectoryEntry(
    const RomFSTraversalContext& ctx, size_t directory_offset) {
    return GetEntry<DirectoryEntry, &RomFSTraversalContext::directory_meta>(ctx, directory_offset);
}

std::optional<std::pair<FileEntry, std::string>> GetFileEntry(const RomFSTraversalContext& ctx,
                                                              size_t file_offset) {
    return GetEntry<FileEntry, &RomFSTraversalContext::file_meta>(ctx, file_offset);
}

void ProcessFile(const RomFSTraversalContext& ctx, u32 this_file_offset,
                 std::shared_ptr<VectorVfsDirectory>& parent,
                 std::unordered_set<u32>& visited_files) {
    constexpr u64 max_size_t = (std::numeric_limits<std::size_t>::max)();
    while (this_file_offset != ROMFS_ENTRY_EMPTY) {
        if (!visited_files.insert(this_file_offset).second) {
            break;
        }
        auto entry = GetFileEntry(ctx, this_file_offset);
        if (!entry) {
            break;
        }

        const u64 file_offset = entry->first.offset;
        const u64 file_size = entry->first.size;
        if (file_offset <= (std::numeric_limits<u64>::max)() - ctx.header.data_offset) {
            const u64 abs_offset = file_offset + ctx.header.data_offset;
            if (abs_offset <= max_size_t && file_size <= max_size_t) {
                parent->AddFile(std::make_shared<OffsetVfsFile>(
                    ctx.file, static_cast<std::size_t>(file_size),
                    static_cast<std::size_t>(abs_offset), std::move(entry->second)));
            }
        }

        this_file_offset = entry->first.sibling;
    }
}

void ProcessDirectory(const RomFSTraversalContext& ctx, u32 root_dir_offset,
                      std::shared_ptr<VectorVfsDirectory>& root_parent) {
    struct PendingDir {
        u32 offset;
        std::shared_ptr<VectorVfsDirectory> parent;
    };

    std::unordered_set<u32> visited_dirs;
    std::unordered_set<u32> visited_files;
    std::vector<PendingDir> stack;
    stack.push_back({root_dir_offset, root_parent});

    while (!stack.empty()) {
        auto [this_dir_offset, parent] = std::move(stack.back());
        stack.pop_back();

        while (this_dir_offset != ROMFS_ENTRY_EMPTY) {
            if (!visited_dirs.insert(this_dir_offset).second) {
                break;
            }
            auto entry = GetDirectoryEntry(ctx, this_dir_offset);
            if (!entry) {
                break;
            }

            auto current = std::make_shared<VectorVfsDirectory>(
                std::vector<VirtualFile>{}, std::vector<VirtualDir>{}, std::move(entry->second));

            if (entry->first.child_file != ROMFS_ENTRY_EMPTY) {
                ProcessFile(ctx, entry->first.child_file, current, visited_files);
            }

            if (entry->first.child_dir != ROMFS_ENTRY_EMPTY) {
                stack.push_back({entry->first.child_dir, current});
            }

            parent->AddDirectory(std::move(current));
            this_dir_offset = entry->first.sibling;
        }
    }
}
} // Anonymous namespace

VirtualDir ExtractRomFS(VirtualFile file) {
    auto root_container = std::make_shared<VectorVfsDirectory>();
    if (!file) {
        return root_container;
    }

    RomFSTraversalContext ctx{};

    if (file->ReadObject(&ctx.header) != sizeof(RomFSHeader)) {
        return nullptr;
    }

    if (ctx.header.header_size != sizeof(RomFSHeader)) {
        return nullptr;
    }

    const u64 file_size = file->GetSize();
    constexpr u64 max_size_t = (std::numeric_limits<std::size_t>::max)();
    const auto is_valid_range = [file_size, max_size_t](const TableLocation& loc) {
        return loc.offset <= file_size && loc.size <= file_size - loc.offset &&
               loc.size <= max_size_t;
    };
    if (!is_valid_range(ctx.header.directory_meta) || !is_valid_range(ctx.header.file_meta)) {
        return nullptr;
    }

    ctx.file = file;
    ctx.directory_meta = file->ReadBytes(static_cast<std::size_t>(ctx.header.directory_meta.size),
                                         static_cast<std::size_t>(ctx.header.directory_meta.offset));
    ctx.file_meta = file->ReadBytes(static_cast<std::size_t>(ctx.header.file_meta.size),
                                    static_cast<std::size_t>(ctx.header.file_meta.offset));
    if (ctx.directory_meta.size() != ctx.header.directory_meta.size ||
        ctx.file_meta.size() != ctx.header.file_meta.size) {
        return nullptr;
    }

    ProcessDirectory(ctx, 0, root_container);

    if (auto root = root_container->GetSubdirectory(""); root) {
        return root;
    }

    return nullptr;
}

VirtualFile CreateRomFS(VirtualDir dir, VirtualDir ext) {
    if (dir == nullptr)
        return nullptr;

    RomFSBuildContext ctx{dir, ext};
    return ConcatenatedVfsFile::MakeConcatenatedFile(0, dir->GetName(), ctx.Build());
}

} // namespace FileSys
