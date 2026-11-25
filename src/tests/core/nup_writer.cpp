// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <openssl/evp.h>

#include "common/fs/file.h"
#include "common/fs/path_util.h"
#include "core/core.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/nup_writer.h"
#include "core/file_sys/control_metadata.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/launch_params.h"
#include "core/loader/nup.h"

namespace {

namespace fs = std::filesystem;
using FileSys::NupContainer;

struct TempDir {
    fs::path path = fs::temp_directory_path() /
                    ("nup-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() {
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void Put(const fs::path& p, std::string_view value) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << value;
}

std::string ReadAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

std::string Read(const FileSys::VirtualFile& f) {
    std::string out(f->GetSize(), '\0');
    f->Read(reinterpret_cast<u8*>(out.data()), out.size(), 0);
    return out;
}

// Enough of an NPDM for the packer: the program ID at ACI0 + 0x10.
std::string Npdm(u64 program_id) {
    std::string npdm(0x100, '\0');
    npdm.replace(0x80, 4, "ACI0");
    for (int i = 0; i < 8; ++i) {
        npdm[0x90 + i] = static_cast<char>(program_id >> (8 * i));
    }
    return npdm;
}

std::array<u8, 32> Sha256(std::string_view data) {
    std::array<u8, 32> out{};
    unsigned int length{};
    EVP_Digest(data.data(), data.size(), out.data(), &length, EVP_sha256(), nullptr);
    return out;
}

u64 Le(const std::string& buf, size_t at, size_t bytes) {
    u64 value = 0;
    for (size_t i = 0; i < bytes; ++i) {
        value |= static_cast<u64>(static_cast<u8>(buf[at + i])) << (8 * i);
    }
    return value;
}

struct RawFile {
    std::string name;
    u64 size;
    std::array<u8, 32> sha256;
    u64 extent_offset;
    u64 extent_size;
    u8 codec;
};

// Reads the file records straight from the TOC, which NupContainer does not expose.
std::vector<RawFile> RawFiles(const std::string& nup) {
    const u64 toc = Le(nup, 0x08, 8);
    const u64 packages = Le(nup, toc + 0x04, 4);
    const u64 dirs = Le(nup, toc + 0x08, 8);
    const u64 files = Le(nup, toc + 0x10, 8);
    const u64 pool = toc + Le(nup, toc + 0x18, 8);
    std::vector<RawFile> out;
    u64 pos = toc + 0x30 + packages * 0x40 + dirs * 0x18;
    for (u64 i = 0; i < files; ++i, pos += 0x50) {
        RawFile file{nup.substr(pool + Le(nup, pos, 4), Le(nup, pos + 4, 4)),
                     Le(nup, pos + 0x08, 8),
                     {},
                     Le(nup, pos + 0x10, 8),
                     Le(nup, pos + 0x18, 8),
                     static_cast<u8>(nup[pos + 0x40])};
        std::copy_n(nup.begin() + static_cast<std::ptrdiff_t>(pos + 0x20), 32,
                    file.sha256.begin());
        out.push_back(file);
    }
    return out;
}

// Every file under `dir` with its contents, keyed by its path relative to `dir`.
std::map<std::string, std::string> SourceTree(const fs::path& dir) {
    std::map<std::string, std::string> out;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().filename() != ".listing") {
            out[fs::relative(entry.path(), dir).generic_string()] = ReadAll(entry.path());
        }
    }
    return out;
}

void PackedTree(const FileSys::VirtualDir& dir, const std::string& prefix,
                std::map<std::string, std::string>& out) {
    if (dir == nullptr) {
        return;
    }
    for (const auto& file : dir->GetFiles()) {
        out[prefix + file->GetName()] = Read(file);
    }
    for (const auto& sub : dir->GetSubdirectories()) {
        PackedTree(sub, prefix + sub->GetName() + "/", out);
    }
}

std::map<std::string, std::string> PackedTree(const FileSys::VirtualDir& dir) {
    std::map<std::string, std::string> out;
    PackedTree(dir, "", out);
    return out;
}

constexpr u64 TitleId = 0x0400ABCD00010000ULL;

void MakeDump(const fs::path& root) {
    Put(root / "Base/ExeFS/main", "base main");
    Put(root / "Base/ExeFS/main.npdm", Npdm(TitleId));
    Put(root / "Base/RomFS/shared.bin", std::string(5000, 's'));
    Put(root / "Base/RomFS/copy/shared.bin", std::string(5000, 's'));
    Put(root / "Base/RomFS/empty", "");
    Put(root / "Base/RomFS/.listing", "listing");
    Put(root / "Base/RomFS/System/Resource/ResourceSizeTable.Product.100.rsizetable", "rst");
    Put(root / "Base/RomFS/big.bin", std::string(64 * 1024, 'b'));
    Put(root / "Base/RomFS/random.bin", [] {
        std::string data(3000, '\0');
        u32 x = 12345;
        for (auto& c : data) {
            x = x * 1664525 + 1013904223;
            c = static_cast<char>(x >> 24);
        }
        return data;
    }());
    Put(root / "updates/65536/exefs/main", "update main");
    Put(root / "updates/65536/exefs/main.npdm", Npdm(TitleId));
    Put(root / "updates/65536/romfs/shared.bin", std::string(5000, 's'));
    Put(root / "updates/65536/romfs/System/Resource/ResourceSizeTable.Product.110.rsizetable",
        "rst");
    Put(root / "updates/65536/romfs/Zeta/a", "a");
    Put(root / "updates/65536/romfs/alpha", "alpha");
    Put(root / "updates/131072/exefs/main", "update 2 main");
    Put(root / "updates/131072/exefs/main.npdm", Npdm(TitleId));
    Put(root / "dlc/0400ABCD00011001/0/romfs/addon.bin", "addon data");
}

void SetLe(std::string& buf, size_t at, u64 value, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) {
        buf[at + i] = static_cast<char>(value >> (8 * i));
    }
}

std::string BootNpdm(u64 program_id, bool switch2) {
    const u32 shift = switch2 ? 0x100 : 0;
    std::string npdm(0x480 + shift, '\0');
    npdm.replace(0, 4, "META");
    SetLe(npdm, 0x70, 0x400 + shift, 4);
    SetLe(npdm, 0x74, 0x40, 4);
    SetLe(npdm, 0x78, 0x100, 4);
    SetLe(npdm, 0x7C, 0x240 + shift, 4);
    npdm.replace(0x300 + shift, 4, "ACID");
    SetLe(npdm, 0x320 + shift, 0x240 + shift, 4);
    SetLe(npdm, 0x324 + shift, 0x2C, 4);
    npdm.replace(0x400 + shift, 4, "ACI0");
    SetLe(npdm, 0x410 + shift, program_id, 8);
    SetLe(npdm, 0x420 + shift, 0x40, 4);
    SetLe(npdm, 0x424 + shift, 0x1C, 4);
    SetLe(npdm, 0x430 + shift, 0x60, 4);
    return npdm;
}

// Applies `patch` to a valid archive, fixes up the TOC hash and opens the result.
template <typename Patch>
Loader::ResultStatus StatusAfterPatch(std::string nup, Patch&& patch) {
    const u64 toc = Le(nup, 0x08, 8);
    patch(nup, toc);
    const auto hash = Sha256(std::string_view{nup}.substr(toc, Le(nup, 0x10, 8)));
    std::copy(hash.begin(), hash.end(), nup.begin() + 0x18);
    const auto data = std::vector<u8>(nup.begin(), nup.end());
    return NupContainer{std::make_shared<FileSys::VectorVfsFile>(data, "bad.nup")}.GetStatus();
}

} // namespace

TEST_CASE("NUP packs base, updates and add-ons and reads back through NupContainer", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    FileSys::Nup::PackOptions options{.raw_threshold = 16 * 1024};
    std::string error;
    REQUIRE(FileSys::Nup::PackDump(root, archive, options, {}, nullptr, &error) ==
            FileSys::Nup::PackResult::Success);
    INFO(error);
    REQUIRE_FALSE(fs::exists(archive.string() + ".part"));

    FileSys::RealVfsFilesystem real;
    NupContainer nup{
        real.OpenFile(Common::FS::PathToUTF8String(archive), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);

    const auto& packages = nup.GetPackages();
    REQUIRE(packages.size() == 4);
    REQUIRE(packages[0].kind == NupContainer::PackageKind::Base);
    REQUIRE(packages[0].title_id == TitleId);
    REQUIRE(packages[0].version == 0);
    REQUIRE(packages[0].display_version == "1.0.0");
    REQUIRE(packages[0].npdm.size() == 0x100);
    REQUIRE(packages[1].kind == NupContainer::PackageKind::Update);
    REQUIRE(packages[1].version == 65536);
    REQUIRE(packages[1].display_version == "1.1.0");
    REQUIRE(packages[1].title_id == TitleId);
    REQUIRE_FALSE(packages[1].standalone_update);
    REQUIRE(packages[2].kind == NupContainer::PackageKind::Update);
    REQUIRE(packages[2].version == 131072);
    REQUIRE(packages[2].romfs_dir == 0xFFFFFFFF);
    REQUIRE(packages[3].kind == NupContainer::PackageKind::Addon);
    REQUIRE(packages[3].title_id == 0x0400ABCD00011001ULL);
    REQUIRE(packages[3].exefs_dir == 0xFFFFFFFF);
    REQUIRE(nup.FindBestUpdatePackage() == std::size_t{2});

    REQUIRE(PackedTree(nup.GetExeFSDir(0)) == SourceTree(root / "Base/ExeFS"));
    REQUIRE(PackedTree(nup.GetRomFSDir(0)) == SourceTree(root / "Base/RomFS"));
    REQUIRE(PackedTree(nup.GetExeFSDir(1)) == SourceTree(root / "updates/65536/exefs"));
    REQUIRE(PackedTree(nup.GetRomFSDir(1)) == SourceTree(root / "updates/65536/romfs"));
    REQUIRE(PackedTree(nup.GetExeFSDir(2)) == SourceTree(root / "updates/131072/exefs"));
    REQUIRE(PackedTree(nup.GetRomFSDir(3)) == SourceTree(root / "dlc/0400ABCD00011001/0/romfs"));
    REQUIRE_FALSE(nup.GetRomFSDir(0)->GetFile(".listing"));

    const auto bytes = ReadAll(archive);
    const auto files = RawFiles(bytes);
    std::map<std::string, std::vector<RawFile>> by_name;
    for (const auto& file : files) {
        by_name[file.name].push_back(file);
        if (file.size != 0) {
            REQUIRE(file.extent_offset % 16 == 0);
        }
    }
    const auto shared = by_name.at("shared.bin");
    REQUIRE(shared.size() == 3);
    for (const auto& file : shared) {
        REQUIRE(file.sha256 == Sha256(std::string(5000, 's')));
        REQUIRE(file.codec == 1);
        REQUIRE(file.extent_offset == shared[0].extent_offset);
        REQUIRE(file.extent_size == shared[0].extent_size);
    }
    const auto& empty = by_name.at("empty").at(0);
    REQUIRE(empty.size == 0);
    REQUIRE(empty.extent_offset == 0);
    REQUIRE(empty.extent_size == 0);
    REQUIRE(empty.codec == 0);
    REQUIRE(empty.sha256 == Sha256(""));
    // Highly compressible, but above the raw threshold.
    const auto& big = by_name.at("big.bin").at(0);
    REQUIRE(big.codec == 0);
    REQUIRE(big.extent_size == 64 * 1024);
    REQUIRE(big.sha256 == Sha256(std::string(64 * 1024, 'b')));
    // Incompressible, so stored raw.
    const auto& random = by_name.at("random.bin").at(0);
    REQUIRE(random.codec == 0);
    REQUIRE(random.extent_size == 3000);
    REQUIRE_FALSE(by_name.contains(".listing"));
}

TEST_CASE("NUP reads zstd members above the memory cache through a temporary file", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    std::string large(300 * 1024, '\0');
    u32 x = 7;
    for (auto& c : large) {
        x = x * 1664525 + 1013904223;
        c = static_cast<char>('a' + (x >> 28));
    }
    Put(root / "Base/RomFS/large.bin", large);
    const auto archive = t.path / "Game.nup";
    FileSys::Nup::PackOptions options{.raw_threshold = 1024 * 1024};
    REQUIRE(FileSys::Nup::PackDump(root, archive, options) == FileSys::Nup::PackResult::Success);
    for (const auto& file : RawFiles(ReadAll(archive))) {
        if (file.name == "large.bin") {
            REQUIRE(file.codec == 1);
        }
    }

    const auto spill_dir = t.path / "spill";
    FileSys::RealVfsFilesystem real;
    {
        // Members above 32 KiB do not fit the 64 KiB cache budget and are spilled to disk.
        NupContainer nup{
            real.OpenFile(Common::FS::PathToUTF8String(archive), FileSys::OpenMode::Read),
            64 * 1024, spill_dir};
        REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
        const auto file = nup.GetRomFSDir(0)->GetFile("large.bin");
        REQUIRE(file != nullptr);
        REQUIRE(file->GetSize() == large.size());

        std::string chunk(4096, '\0');
        for (size_t offset = 0; offset < large.size(); offset += chunk.size()) {
            const size_t count =
                file->Read(reinterpret_cast<u8*>(chunk.data()), chunk.size(), offset);
            REQUIRE(count == std::min(chunk.size(), large.size() - offset));
            REQUIRE(chunk.compare(0, count, large, offset, count) == 0);
        }
        for (const size_t offset : {size_t{0}, size_t{123456}, size_t{77}, large.size() - 5}) {
            const size_t count = file->Read(reinterpret_cast<u8*>(chunk.data()), 100, offset);
            REQUIRE(count == std::min<size_t>(100, large.size() - offset));
            REQUIRE(chunk.compare(0, count, large, offset, count) == 0);
        }
        REQUIRE(file->Read(reinterpret_cast<u8*>(chunk.data()), 10, large.size()) == 0);

        REQUIRE(std::distance(fs::directory_iterator(spill_dir), fs::directory_iterator{}) > 0);
        REQUIRE(PackedTree(nup.GetRomFSDir(0)) == SourceTree(root / "Base/RomFS"));
        REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);
    }
    REQUIRE(std::distance(fs::directory_iterator(spill_dir), fs::directory_iterator{}) == 0);
}

TEST_CASE("NUP rejects a TOC whose counts or offsets wrap", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);
    const auto nup = ReadAll(archive);
    const auto no_patch = [](std::string&, u64) {};
    REQUIRE(StatusAfterPatch(nup, no_patch) == Loader::ResultStatus::Success);

    // 2^61 directories wrap dir_count * 0x18 back to the real record size.
    REQUIRE(StatusAfterPatch(nup, [](std::string& buf, u64 toc) {
                SetLe(buf, toc + 0x08, Le(buf, toc + 0x08, 8) + (1ULL << 61), 8);
            }) == Loader::ResultStatus::ErrorNUPBadFormat);

    const auto first_dir = [](const std::string& buf, u64 toc) {
        return toc + 0x30 + Le(buf, toc + 0x04, 4) * 0x40;
    };
    // first_child + child_count wraps to a small value.
    REQUIRE(StatusAfterPatch(nup, [&](std::string& buf, u64 toc) {
                SetLe(buf, first_dir(buf, toc) + 0x08, 0xFFFFFFFF, 4);
                SetLe(buf, first_dir(buf, toc) + 0x0C, 2, 4);
            }) == Loader::ResultStatus::ErrorNUPBadFormat);
}

TEST_CASE("NUP rejects a directory that contains its own ancestor", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    // Give the base RomFS directory the same children as the package root, which lists it.
    REQUIRE(StatusAfterPatch(ReadAll(archive), [](std::string& buf, u64 toc) {
                const u64 dirs = toc + 0x30 + Le(buf, toc + 0x04, 4) * 0x40;
                const u64 root_dir = Le(buf, toc + 0x30 + 0x08, 4);
                const u64 romfs_dir = Le(buf, toc + 0x30 + 0x10, 4);
                const u64 root_record = dirs + root_dir * 0x18;
                const u64 romfs_record = dirs + romfs_dir * 0x18;
                SetLe(buf, romfs_record + 0x08, Le(buf, root_record + 0x08, 4), 4);
                SetLe(buf, romfs_record + 0x0C, Le(buf, root_record + 0x0C, 4), 4);
            }) == Loader::ResultStatus::ErrorNUPBadFormat);
}

TEST_CASE("NUP validates directory ownership and raw extent sizes", "[nup]") {
    const auto archive_status = [](std::initializer_list<std::pair<u32, u32>> dirs,
                                   std::initializer_list<u32> children, size_t raw_size = 0,
                                   size_t extent_size = 0) {
        constexpr size_t toc = 0x80;
        constexpr size_t first_dir = toc + 0x30 + 0x40;
        const size_t first_file = first_dir + dirs.size() * 0x18;
        const size_t first_child = first_file + (raw_size != 0 ? 0x50 : 0);
        const size_t toc_end = first_child + children.size() * sizeof(u32);
        const size_t blob_offset = (toc_end + 15) & ~size_t{15};
        std::string nup(blob_offset + extent_size, '\0');

        nup.replace(0, 4, "NUP\x01");
        SetLe(nup, 0x04, toc, 2);
        SetLe(nup, 0x06, 1, 2);
        SetLe(nup, 0x08, toc, 8);
        SetLe(nup, 0x10, toc_end - toc, 8);
        SetLe(nup, 0x38, blob_offset, 8);
        SetLe(nup, 0x40, nup.size(), 8);
        SetLe(nup, 0x48, 1, 4);

        nup.replace(toc, 4, "TOC\x01");
        SetLe(nup, toc + 0x04, 1, 4);
        SetLe(nup, toc + 0x08, dirs.size(), 8);
        SetLe(nup, toc + 0x10, raw_size != 0, 8);
        SetLe(nup, toc + 0x18, toc_end - toc, 8);
        size_t pos = first_dir;
        for (const auto& [start, count] : dirs) {
            SetLe(nup, pos + 0x08, start, 4);
            SetLe(nup, pos + 0x0C, count, 4);
            pos += 0x18;
        }
        if (raw_size != 0) {
            SetLe(nup, first_file + 0x08, raw_size, 8);
            SetLe(nup, first_file + 0x10, blob_offset, 8);
            SetLe(nup, first_file + 0x18, extent_size, 8);
        }
        pos = first_child;
        for (const u32 child : children) {
            SetLe(nup, pos, child, sizeof(child));
            pos += sizeof(child);
        }
        return StatusAfterPatch(std::move(nup), [](std::string&, u64) {});
    };

    SECTION("an unreferenced directory tree can point to an earlier record") {
        CHECK(archive_status({{0, 0}, {0, 0}, {0, 1}}, {1}) ==
              Loader::ResultStatus::Success);
    }
    SECTION("a directory cannot appear under two parents") {
        CHECK(archive_status({{0, 2}, {2, 1}, {3, 1}, {0, 0}}, {1, 2, 3, 3}) ==
              Loader::ResultStatus::ErrorNUPBadFormat);
    }
    SECTION("directories outside the package tree cannot form a cycle") {
        CHECK(archive_status({{0, 0}, {0, 1}, {1, 1}}, {2, 1}) ==
              Loader::ResultStatus::ErrorNUPBadFormat);
    }
    SECTION("the size of a raw extent must match its file record") {
        CHECK(archive_status({{0, 1}}, {1}, 32, 64) == Loader::ResultStatus::ErrorNUPBadFormat);
        CHECK(archive_status({{0, 1}}, {1}, 64, 32) == Loader::ResultStatus::ErrorNUPBadFormat);
        CHECK(archive_status({{0, 1}}, {1}, 32, 32) == Loader::ResultStatus::Success);
    }
}

TEST_CASE("NUP marks updates standalone when the dump has no base", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    Put(root / "updates/65536/exefs/main", "main");
    Put(root / "updates/65536/exefs/main.npdm", Npdm(TitleId));
    Put(root / "updates/65536/romfs/file", "rom");
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    FileSys::RealVfsFilesystem real;
    NupContainer nup{
        real.OpenFile(Common::FS::PathToUTF8String(archive), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nup.GetPackages().size() == 1);
    REQUIRE(nup.GetPackages()[0].kind == NupContainer::PackageKind::Update);
    REQUIRE(nup.GetPackages()[0].standalone_update);
    REQUIRE(nup.GetPackages()[0].title_id == TitleId);
    REQUIRE(Read(nup.GetRomFSDir(0)->GetFile("file")) == "rom");
}

TEST_CASE("NUP packs a bare exefs/romfs dump as one base", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    Put(root / "ExeFS/main", "main");
    Put(root / "ExeFS/main.npdm", Npdm(TitleId));
    Put(root / "RomFS/file", "rom");
    Put(root / "notes.txt", "extra");
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    FileSys::RealVfsFilesystem real;
    NupContainer nup{
        real.OpenFile(Common::FS::PathToUTF8String(archive), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nup.GetPackages().size() == 1);
    REQUIRE(nup.GetPackages()[0].kind == NupContainer::PackageKind::Base);
    REQUIRE_FALSE(nup.GetPackages()[0].standalone_update);
    REQUIRE(Read(nup.GetExeFSDir(0)->GetFile("main")) == "main");
    REQUIRE(Read(nup.GetRomFSDir(0)->GetFile("file")) == "rom");
    const auto files = RawFiles(ReadAll(archive));
    REQUIRE(std::ranges::count(files, std::string{"notes.txt"}, &RawFile::name) == 1);
}

TEST_CASE("NUP low storage resumes and deletes the packed sources", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto copy = t.path / "copy" / "Game";
    fs::create_directories(copy);
    fs::copy(root, copy, fs::copy_options::recursive);
    const auto archive = t.path / "Game.nup";

    std::atomic_bool cancel = false;
    bool cancelled = false;
    FileSys::Nup::PackOptions options{.delete_sources = true, .chunk_bytes = 1};
    auto result = FileSys::Nup::PackDump(
        root, archive, options,
        [&](const auto& p) {
            if (p.files_done >= 4 && !cancelled) {
                cancel = true;
                cancelled = true;
            }
        },
        &cancel);
    REQUIRE(result == FileSys::Nup::PackResult::Cancelled);
    REQUIRE(FileSys::Nup::HasResumableJournal(archive));
    REQUIRE_FALSE(fs::exists(root / "Base/ExeFS/main"));
    REQUIRE_FALSE(fs::exists(root / "Base/ExeFS/main.npdm"));

    cancel = false;
    std::string error;
    REQUIRE(FileSys::Nup::PackDump(root, archive, options, {}, &cancel, &error) ==
            FileSys::Nup::PackResult::Success);
    INFO(error);
    REQUIRE_FALSE(fs::exists(archive.string() + ".journal"));
    REQUIRE_FALSE(fs::exists(archive.string() + ".part"));
    REQUIRE_FALSE(fs::exists(root / "Base/ExeFS"));
    REQUIRE_FALSE(fs::exists(root / "updates"));
    REQUIRE_FALSE(fs::exists(root / "dlc"));
    // The listing is not packed, so it is left behind with the folders holding it.
    REQUIRE(ReadAll(root / "Base/RomFS/.listing") == "listing");
    REQUIRE_FALSE(fs::exists(root / "Base/RomFS/shared.bin"));

    FileSys::RealVfsFilesystem real;
    NupContainer nup{
        real.OpenFile(Common::FS::PathToUTF8String(archive), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);
    REQUIRE(nup.GetPackages().size() == 4);
    const auto npdm = Npdm(TitleId);
    REQUIRE(nup.GetPackages()[0].npdm == std::vector<u8>(npdm.begin(), npdm.end()));
    REQUIRE(PackedTree(nup.GetRomFSDir(0)) == SourceTree(copy / "Base/RomFS"));
    REQUIRE(PackedTree(nup.GetRomFSDir(1)) == SourceTree(copy / "updates/65536/romfs"));

    // The resumed archive matches one packed in a single pass.
    const auto again = t.path / "again.nup";
    REQUIRE(FileSys::Nup::PackDump(copy, again) == FileSys::Nup::PackResult::Success);
    REQUIRE(ReadAll(archive) == ReadAll(again));
}

TEST_CASE("NUP rejects overwrite and a dump without NPDM", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    Put(root / "exefs/main", "main");
    Put(root / "romfs/a", "rom");
    const auto archive = t.path / "out.nup";
    Put(archive, "existing");
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::IoError);
    REQUIRE(ReadAll(archive) == "existing");
    fs::remove(archive);
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::InvalidSource);
    REQUIRE_FALSE(fs::exists(archive.string() + ".part"));
}

// Archives are meant to be byte-identical on every platform. A change to this hash means
// packing a dump no longer reproduces archives made by earlier builds (for example after a zstd
// update), so make it deliberately.
TEST_CASE("NUP output is reproducible", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    // Large and compressible, so it covers what used to be zstd's multithreaded path.
    Put(root / "Base/RomFS/large.bin", [] {
        std::string data(20 * 1024 * 1024, '\0');
        u32 x = 1;
        for (auto& c : data) {
            x = x * 1664525 + 1013904223;
            c = static_cast<char>('a' + (x >> 28));
        }
        return data;
    }());
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    std::string hex;
    for (const u8 b : Sha256(ReadAll(archive))) {
        hex += fmt::format("{:02x}", b);
    }
    // The mixed-case Base/RomFS directory supplies base version 1.0.0 on every platform.
    REQUIRE(hex == "718231e0974ca04c7be7bd6ed4334015b777c6e5f1ed51de23f899f81ee004ab");
}

TEST_CASE("NUP cancellation before publication leaves the dump intact", "[nup]") {
    using namespace FileSys::Nup;
    TempDir t;
    const auto dump = t.path / "Game";
    MakeDump(dump);
    const auto sources = SourceTree(dump);
    const auto archive = t.path / "Game.nup";
    std::atomic_bool cancel{true};
    CHECK(PackDump(dump, archive, {}, {}, &cancel) == PackResult::Cancelled);

    cancel = false;
    CHECK(PackDump(
              dump, archive, {},
              [&](const PackProgress& progress) {
                  if (progress.files_done == progress.files_total) {
                      cancel = true;
                  }
              },
              &cancel) == PackResult::Cancelled);
    CHECK_FALSE(fs::exists(archive));
    CHECK_FALSE(fs::exists(archive.string() + ".part"));
    CHECK_FALSE(fs::exists(archive.string() + ".journal"));
    CHECK(SourceTree(dump) == sources);
}

TEST_CASE("NUP writes metadata overrides to the base and the newest update", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto probed = FileSys::Nup::ProbeMetadata(root);
    REQUIRE(probed.title_name.empty());
    REQUIRE(probed.icon.empty());
    // The version comes from the RomFS resource size table of the newest update that has one.
    REQUIRE(probed.display_version == "1.1.0");
    REQUIRE(probed.version_source == "update 65536");

    FileSys::Nup::PackOptions options;
    options.metadata.title_name = "Custom Name";
    options.metadata.icon = {0xFF, 0xD8, 0xFF, 0xD9};
    options.metadata.display_version = "9.8.7";
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive, options) == FileSys::Nup::PackResult::Success);

    FileSys::RealVfsFilesystem real;
    NupContainer nup{
        real.OpenFile(Common::FS::PathToUTF8String(archive), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    const auto& packages = nup.GetPackages();
    REQUIRE(packages.size() == 4);
    REQUIRE(packages[0].title_name == "Custom Name");
    REQUIRE(packages[0].icon == options.metadata.icon);
    REQUIRE(packages[0].display_version == "9.8.7");
    REQUIRE(packages[2].version == 131072);
    REQUIRE(packages[2].display_version == "9.8.7");
    REQUIRE(packages[1].display_version == "1.1.0");
}

TEST_CASE("NUP probes the version from the game data list tag", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    Put(root / "base/exefs/main", "main");
    Put(root / "base/exefs/main.npdm", Npdm(TitleId));
    Put(root / "base/romfs/GameData/GameDataList.Product.100.byml.zs", "x");
    Put(root / "base/romfs/GameData/GameDataList.Product.99x.byml.zs", "x");
    Put(root / "base/romfs/System/Resource/ResourceSizeTable.Product.rsizetable.zs", "x");
    Put(root / "updates/458752/exefs/main", "main");
    Put(root / "updates/458752/exefs/main.npdm", Npdm(TitleId));
    Put(root / "updates/458752/romfs/GameData/GameDataList.Product.320.byml.zs", "x");
    Put(root / "updates/458752/romfs/GameData/GameDataList.Product.320.revision.txt", "1");

    auto probed = FileSys::Nup::ProbeMetadata(root);
    REQUIRE(probed.display_version == "3.2.0");
    REQUIRE(probed.version_source == "update 458752");

    fs::remove_all(root / "updates");
    probed = FileSys::Nup::ProbeMetadata(root);
    REQUIRE(probed.display_version == "1.0.0");
    REQUIRE(probed.version_source == "base");
}

TEST_CASE("NUP extracts a full dump and repacks it", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    const auto out = t.path / "Extracted";
    std::string error;
    REQUIRE(FileSys::Nup::ExtractNup(archive, out, {}, nullptr, &error) ==
            FileSys::Nup::ExtractResult::Success);
    INFO(error);
    REQUIRE(SourceTree(out / "base/romfs") == SourceTree(root / "Base/RomFS"));
    REQUIRE(SourceTree(out / "base/exefs") == SourceTree(root / "Base/ExeFS"));
    REQUIRE(SourceTree(out / "updates/65536/romfs") == SourceTree(root / "updates/65536/romfs"));
    REQUIRE(SourceTree(out / "updates/65536/exefs") == SourceTree(root / "updates/65536/exefs"));
    REQUIRE(SourceTree(out / "updates/131072/exefs") ==
            SourceTree(root / "updates/131072/exefs"));
    REQUIRE(SourceTree(out / "dlc/0400ABCD00011001/0/romfs") ==
            SourceTree(root / "dlc/0400ABCD00011001/0/romfs"));

    const auto repacked = t.path / "Repacked.nup";
    REQUIRE(FileSys::Nup::PackDump(out, repacked) == FileSys::Nup::PackResult::Success);
    FileSys::RealVfsFilesystem real;
    FileSys::NupContainer original{
        real.OpenFile(Common::FS::PathToUTF8String(archive), FileSys::OpenMode::Read)};
    FileSys::NupContainer copy{
        real.OpenFile(Common::FS::PathToUTF8String(repacked), FileSys::OpenMode::Read)};
    REQUIRE(original.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(copy.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(original.GetPackages().size() == copy.GetPackages().size());
    REQUIRE(copy.VerifyIntegrity({}) == Loader::ResultStatus::Success);
}

TEST_CASE("NUP extracts a bare dump without a base wrapper", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    Put(root / "ExeFS/main", "main");
    Put(root / "ExeFS/main.npdm", Npdm(TitleId));
    Put(root / "RomFS/file", "rom");
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    const auto out = t.path / "Extracted";
    REQUIRE(FileSys::Nup::ExtractNup(archive, out) == FileSys::Nup::ExtractResult::Success);
    REQUIRE(SourceTree(out / "exefs") == SourceTree(root / "ExeFS"));
    REQUIRE(SourceTree(out / "romfs") == SourceTree(root / "RomFS"));
    REQUIRE_FALSE(fs::exists(out / "base"));
}

TEST_CASE("NUP extract refuses to overwrite", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    const auto out = t.path / "Extracted";
    fs::create_directories(out);
    Put(out / "existing.txt", "x");
    std::string error;
    REQUIRE(FileSys::Nup::ExtractNup(archive, out, {}, nullptr, &error) ==
            FileSys::Nup::ExtractResult::IoError);
    REQUIRE(!error.empty());
}

TEST_CASE("NUP edits default-language metadata in place", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    FileSys::Nup::PackOptions options;
    options.metadata.title_name = "Before";
    options.metadata.publisher = "Before Inc";
    options.metadata.display_version = "1.0.0";
    options.metadata.icon = {0xFF, 0xD8, 0xFF, 0xD9};
    REQUIRE(FileSys::Nup::PackDump(root, archive, options) == FileSys::Nup::PackResult::Success);

    const auto before = ReadAll(archive);
    FileSys::Nup::MetadataEdit edit;
    edit.title_name = "After";
    edit.publisher = "After Inc";
    edit.display_version = "2.0.0";
    edit.icon = std::vector<u8>{0x89, 0x50, 0x4E, 0x47};
    std::string error;
    INFO(error);
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, edit, &error) ==
            FileSys::Nup::EditResult::Success);

    {
        FileSys::RealVfsFilesystem real;
        FileSys::NupContainer nup{
            real.OpenFile(Common::FS::PathToUTF8String(archive), FileSys::OpenMode::Read)};
        REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
        REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);
        REQUIRE(nup.GetPackages()[0].title_name == "After");
        REQUIRE(nup.GetPackages()[0].publisher == "After Inc");
        REQUIRE(nup.GetPackages()[0].display_version == "2.0.0");
        REQUIRE(nup.GetPackages()[0].icon == std::vector<u8>{0x89, 0x50, 0x4E, 0x47});
    }

    const auto probed = FileSys::Nup::ProbeNupMetadata(archive);
    REQUIRE(probed.title_name == "After");
    REQUIRE(probed.publisher == "After Inc");
    REQUIRE(probed.icon == std::vector<u8>{0x89, 0x50, 0x4E, 0x47});

    FileSys::Nup::MetadataEdit clear;
    clear.title_name = "";
    clear.icon = std::vector<u8>{};
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, clear, &error) ==
            FileSys::Nup::EditResult::Success);
    {
        FileSys::RealVfsFilesystem real2;
        FileSys::NupContainer cleared{
            real2.OpenFile(Common::FS::PathToUTF8String(archive), FileSys::OpenMode::Read)};
        REQUIRE(cleared.GetStatus() == Loader::ResultStatus::Success);
        REQUIRE(cleared.GetPackages()[0].title_name.empty());
        REQUIRE(cleared.GetPackages()[0].icon.empty());
        REQUIRE(cleared.VerifyIntegrity({}) == Loader::ResultStatus::Success);
    }
    REQUIRE(ReadAll(archive) != before);

    FileSys::Nup::MetadataEdit noop;
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, noop, &error) ==
            FileSys::Nup::EditResult::NoChange);
}

TEST_CASE("NUP metadata edits preserve file extents when metadata precedes them", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    constexpr std::string_view original_title = "Original title";
    constexpr std::string_view new_title = "New title";
    FileSys::Nup::PackOptions options;
    options.metadata.title_name = original_title;
    REQUIRE(FileSys::Nup::PackDump(root, archive, options) == FileSys::Nup::PackResult::Success);

    auto bytes = ReadAll(archive);
    const auto toc = Le(bytes, 0x08, 8);
    const auto package_count = Le(bytes, toc + 0x04, 4);
    const auto dir_count = Le(bytes, toc + 0x08, 8);
    const auto file_count = Le(bytes, toc + 0x10, 8);
    const auto blob_offset = Le(bytes, 0x38, 8);
    const auto first_package = toc + 0x30;
    const auto old_meta_offset = Le(bytes, first_package + 0x20, 8);
    const auto meta_size = Le(bytes, first_package + 0x28, 4);
    REQUIRE(meta_size > 0);
    REQUIRE(old_meta_offset > blob_offset);
    const auto meta = bytes.substr(old_meta_offset, meta_size);

    u64 insert_at = blob_offset;
    bool shared_meta = false;
    SECTION("Metadata between TOC and blob region") {}
    SECTION("Shared metadata between TOC and blob region") {
        shared_meta = true;
    }
    SECTION("Metadata between file extents") {
        insert_at = old_meta_offset;
        for (const auto& file : RawFiles(bytes)) {
            if (file.extent_offset > blob_offset) {
                insert_at = std::min(insert_at, file.extent_offset);
            }
        }
        REQUIRE(insert_at < old_meta_offset);
    }

    const auto inserted_size = (meta_size + 15) & ~u64{15};
    bytes.erase(old_meta_offset, meta_size);
    bytes.insert(insert_at, inserted_size, '\0');
    bytes.replace(insert_at, meta_size, meta);
    SetLe(bytes, first_package + 0x20, insert_at, 8);
    for (u64 i = 1; i < package_count; ++i) {
        const auto field = first_package + i * 0x40 + 0x20;
        const auto offset = Le(bytes, field, 8);
        if (offset > old_meta_offset) {
            SetLe(bytes, field, offset - meta_size + inserted_size, 8);
        }
    }
    if (shared_meta) {
        REQUIRE(package_count > 1);
        const auto field = first_package + (package_count - 1) * 0x40 + 0x20;
        SetLe(bytes, field, insert_at, 8);
        SetLe(bytes, field + 8, meta_size, 4);
    }
    const auto first_file = first_package + package_count * 0x40 + dir_count * 0x18;
    for (u64 i = 0; i < file_count; ++i) {
        const auto field = first_file + i * 0x50 + 0x10;
        const auto offset = Le(bytes, field, 8);
        if (offset >= insert_at) {
            SetLe(bytes, field, offset + inserted_size, 8);
        }
    }
    if (insert_at == blob_offset) {
        SetLe(bytes, 0x38, blob_offset + inserted_size, 8);
    }
    SetLe(bytes, 0x40, bytes.size(), 8);
    const auto toc_hash = Sha256(std::string_view{bytes}.substr(toc, Le(bytes, 0x10, 8)));
    std::copy(toc_hash.begin(), toc_hash.end(), bytes.begin() + 0x18);
    Put(archive, bytes);

    const auto check_archive = [&](std::string_view title, const std::vector<u8>& icon) {
        FileSys::RealVfsFilesystem vfs;
        NupContainer nup{vfs.OpenFile(Common::FS::PathToUTF8String(archive), FileSys::OpenMode::Read)};
        REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
        REQUIRE(nup.GetPackages().front().title_name == title);
        REQUIRE(nup.GetPackages().front().icon == icon);
        if (shared_meta) {
            REQUIRE(nup.GetPackages().back().title_name == original_title);
            REQUIRE(nup.GetPackages().back().icon.empty());
        }
        REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);
        const auto actual = ReadAll(archive);
        REQUIRE(Le(actual, 0x38, 8) == Le(bytes, 0x38, 8));
        const auto file_records_size = file_count * 0x50;
        REQUIRE(actual.substr(first_file, file_records_size) ==
                bytes.substr(first_file, file_records_size));
        for (const auto& file : RawFiles(bytes)) {
            REQUIRE(actual.substr(file.extent_offset, file.extent_size) ==
                    bytes.substr(file.extent_offset, file.extent_size));
        }
    };
    check_archive(original_title, {});

    FileSys::Nup::MetadataEdit edit;
    const std::vector<u8> icon{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A};
    edit.icon = icon;
    std::string error;
    const auto result = FileSys::Nup::EditNupMetadata(archive, edit, &error);
    INFO(error);
    REQUIRE(result == FileSys::Nup::EditResult::Success);
    check_archive(original_title, icon);
    const auto size_after_icon = fs::file_size(archive);
    REQUIRE(ReadAll(archive).substr(insert_at, meta_size) == std::string(meta_size, '\0'));

    edit.title_name = new_title;
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, edit) == FileSys::Nup::EditResult::Success);
    check_archive(new_title, icon);
    REQUIRE(fs::file_size(archive) + original_title.size() - new_title.size() == size_after_icon);
    const auto edited = ReadAll(archive);
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, edit) == FileSys::Nup::EditResult::NoChange);
    REQUIRE(ReadAll(archive) == edited);
}

TEST_CASE("NUP extract refuses a non-empty directory without touching it", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    const auto out = t.path / "Occupied";
    fs::create_directories(out);
    Put(out / "keep.txt", "precious");
    Put(out / "sub" / "keep2.txt", "precious");
    std::string error;
    REQUIRE(FileSys::Nup::ExtractNup(archive, out, {}, nullptr, &error) ==
            FileSys::Nup::ExtractResult::IoError);
    REQUIRE(!error.empty());
    REQUIRE(ReadAll(out / "keep.txt") == "precious");
    REQUIRE(ReadAll(out / "sub" / "keep2.txt") == "precious");
    REQUIRE_FALSE(fs::exists(out / "base"));
}

TEST_CASE("NUP extract removes output it created when it fails", "[nup]") {
    TempDir t;
    Put(t.path / "not-a-nup.bin", "junk");
    const auto out = t.path / "Created";
    std::string error;
    REQUIRE(FileSys::Nup::ExtractNup(t.path / "not-a-nup.bin", out, {}, nullptr, &error) ==
            FileSys::Nup::ExtractResult::InvalidSource);
    REQUIRE(!error.empty());
    REQUIRE_FALSE(fs::exists(out));

    const auto preexisting = t.path / "Preexisting";
    fs::create_directories(preexisting);
    REQUIRE(FileSys::Nup::ExtractNup(t.path / "not-a-nup.bin", preexisting, {}, nullptr,
                                     &error) == FileSys::Nup::ExtractResult::InvalidSource);
    REQUIRE(fs::exists(preexisting));
    REQUIRE(fs::is_empty(preexisting));
}

TEST_CASE("NUP edit propagates display version to the newest update", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    FileSys::RealVfsFilesystem real;
    {
        FileSys::NupContainer before{
            real.OpenFile(Common::FS::PathToUTF8String(archive), FileSys::OpenMode::Read)};
        REQUIRE(before.GetStatus() == Loader::ResultStatus::Success);
        REQUIRE(before.GetPackages()[1].display_version == "1.1.0");
    }

    FileSys::Nup::MetadataEdit edit;
    edit.display_version = "7.7.7";
    std::string error;
    INFO(error);
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, edit, &error) ==
            FileSys::Nup::EditResult::Success);

    FileSys::RealVfsFilesystem real2;
    FileSys::NupContainer nup{
        real2.OpenFile(Common::FS::PathToUTF8String(archive), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
    REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);
    REQUIRE(nup.GetPackages()[0].display_version == "7.7.7");
    REQUIRE(nup.GetPackages()[2].display_version == "7.7.7");
    REQUIRE(nup.GetPackages()[1].display_version == "1.1.0");

    const auto probed = FileSys::Nup::ProbeNupMetadata(archive);
    REQUIRE(probed.display_version == "7.7.7");

    REQUIRE(FileSys::Nup::EditNupMetadata(archive, edit, &error) ==
            FileSys::Nup::EditResult::NoChange);
}

TEST_CASE("NUP loader exposes title and publisher through control data", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    FileSys::Nup::PackOptions options;
    options.metadata.title_name = "Packed";
    options.metadata.publisher = "Packed Inc";
    REQUIRE(FileSys::Nup::PackDump(root, archive, options) == FileSys::Nup::PackResult::Success);

    const auto check = [&](const std::string& title, const std::string& publisher) {
        FileSys::RealVfsFilesystem real;
        Loader::AppLoader_NUP loader{
            real.OpenFile(Common::FS::PathToUTF8String(archive), FileSys::OpenMode::Read)};
        FileSys::NACP control;
        REQUIRE(loader.ReadControlData(control) == Loader::ResultStatus::Success);
        REQUIRE(control.GetApplicationName() == title);
        REQUIRE(control.GetDeveloperName() == publisher);
    };
    check("Packed", "Packed Inc");

    FileSys::Nup::MetadataEdit edit;
    edit.title_name = "Edited";
    edit.publisher = "Edited Inc";
    std::string error;
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, edit, &error) ==
            FileSys::Nup::EditResult::Success);
    INFO(error);
    check("Edited", "Edited Inc");
}

TEST_CASE("NUP loader rejects program ID when the selected ExeFS is incomplete", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    Put(root / "base/exefs/main", "main");
    Put(root / "base/exefs/main.npdm", Npdm(TitleId));
    Put(root / "base/romfs/file", "rom");

    SECTION("Missing main") {
        Put(root / "updates/65536/exefs/main.npdm", Npdm(TitleId));
    }
    SECTION("Missing main.npdm") {
        Put(root / "updates/65536/exefs/main", "main");
    }

    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    FileSys::RealVfsFilesystem real;
    Loader::AppLoader_NUP loader{
        real.OpenFile(Common::FS::PathToUTF8String(archive), FileSys::OpenMode::Read)};
    u64 program_id{};
    REQUIRE(loader.ReadProgramId(program_id) == Loader::ResultStatus::ErrorNotInitialized);
}

TEST_CASE("NUP edit applies name/icon/version to a metadata-less dump", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    // No metadata overrides: base has no title, icon or publisher, like a bare dump.
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);
    {
        FileSys::RealVfsFilesystem real;
        FileSys::NupContainer packed{
            real.OpenFile(Common::FS::PathToUTF8String(archive), FileSys::OpenMode::Read)};
        REQUIRE(packed.GetStatus() == Loader::ResultStatus::Success);
        REQUIRE(packed.GetPackages()[0].title_name.empty());
        REQUIRE(packed.GetPackages()[0].icon.empty());
    }

    FileSys::Nup::MetadataEdit edit;
    edit.title_name = "Hyrule Warriors: Age of Imprisonment";
    edit.display_version = "1.0.3";
    edit.publisher = "Nintendo";
    edit.icon = std::vector<u8>{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A};
    std::string error;
    INFO(error);
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, edit, &error) ==
            FileSys::Nup::EditResult::Success);

    FileSys::RealVfsFilesystem real;
    Loader::AppLoader_NUP loader{
        real.OpenFile(Common::FS::PathToUTF8String(archive), FileSys::OpenMode::Read)};
    std::string title;
    REQUIRE(loader.ReadTitle(title) == Loader::ResultStatus::Success);
    REQUIRE(title == "Hyrule Warriors: Age of Imprisonment");
    std::vector<u8> icon;
    REQUIRE(loader.ReadIcon(icon) == Loader::ResultStatus::Success);
    REQUIRE(icon == std::vector<u8>{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A});
    FileSys::NACP control;
    REQUIRE(loader.ReadControlData(control) == Loader::ResultStatus::Success);
    REQUIRE(control.GetApplicationName() == "Hyrule Warriors: Age of Imprisonment");
    REQUIRE(control.GetDeveloperName() == "Nintendo");
    std::string version;
    REQUIRE(loader.ReadDisplayVersion(version) == Loader::ResultStatus::Success);
    REQUIRE(version == "1.0.3");
}

TEST_CASE("NUP edits localized titles and keeps the rest", "[nup]") {
    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    const auto archive = t.path / "Game.nup";
    FileSys::Nup::PackOptions options;
    options.metadata.title_name = "Default";
    options.metadata.icon = {0xFF, 0xD8, 0xFF, 0xD9};
    REQUIRE(FileSys::Nup::PackDump(root, archive, options) == FileSys::Nup::PackResult::Success);

    FileSys::Nup::MetadataEdit edit;
    edit.localized_titles = std::vector<std::pair<std::string, std::string>>{
        {"de", "Standard"}, {"ja", "デフォルト"}, {"pt-br", "Padrão"}};
    std::string error;
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, edit, &error) ==
            FileSys::Nup::EditResult::Success);
    INFO(error);

    FileSys::RealVfsFilesystem real;
    {
        FileSys::NupContainer nup{
            real.OpenFile(Common::FS::PathToUTF8String(archive), FileSys::OpenMode::Read)};
        REQUIRE(nup.GetStatus() == Loader::ResultStatus::Success);
        REQUIRE(nup.VerifyIntegrity({}) == Loader::ResultStatus::Success);
        REQUIRE(nup.GetPackages()[0].title_name == "Default");
        REQUIRE(nup.GetPackages()[0].icon == std::vector<u8>{0xFF, 0xD8, 0xFF, 0xD9});
        REQUIRE(nup.GetPackages()[0].localized_titles == edit.localized_titles.value());
    }
    REQUIRE(FileSys::Nup::ProbeNupMetadata(archive).localized_titles ==
            edit.localized_titles.value());

    // Editing only the default name leaves the localized titles alone; an empty list clears them.
    FileSys::Nup::MetadataEdit rename;
    rename.title_name = "Renamed";
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, rename, &error) ==
            FileSys::Nup::EditResult::Success);
    REQUIRE(FileSys::Nup::ProbeNupMetadata(archive).localized_titles ==
            edit.localized_titles.value());

    FileSys::Nup::MetadataEdit clear;
    clear.localized_titles = std::vector<std::pair<std::string, std::string>>{};
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, clear, &error) ==
            FileSys::Nup::EditResult::Success);
    FileSys::NupContainer nup{
        real.OpenFile(Common::FS::PathToUTF8String(archive), FileSys::OpenMode::Read)};
    REQUIRE(nup.GetPackages()[0].localized_titles.empty());
    REQUIRE(nup.GetPackages()[0].title_name == "Renamed");
}

TEST_CASE("IOFile, VFS, and NUP handle non-ASCII UTF-8 paths", "[nup][fs]") {
    TempDir t;
    const fs::path non_ascii_dir = t.path / std::u8string{u8"Pok\u00e9mon\u2122 Pokopia"};
    fs::create_directories(non_ascii_dir);

    const fs::path io_file_path = non_ascii_dir / std::u8string{u8"Pok\u00e9mon\u2122.bin"};
    const std::string utf8_io_path = Common::FS::PathToUTF8String(io_file_path);

    {
        Common::FS::IOFile out{utf8_io_path, Common::FS::FileAccessMode::Write};
        REQUIRE(out.IsOpen());
        const std::string payload = "pokopia-utf8";
        REQUIRE(out.WriteString(payload) == payload.size());
    }
    {
        Common::FS::IOFile in;
        in.Open(std::string_view{utf8_io_path}, Common::FS::FileAccessMode::Read);
        REQUIRE(in.IsOpen());
        REQUIRE(in.ReadString(in.GetSize()) == "pokopia-utf8");
    }

    {
        FileSys::RealVfsFilesystem real;
        {
            const auto vfs_file = real.OpenFile(utf8_io_path, FileSys::OpenMode::Read);
            REQUIRE(vfs_file != nullptr);
            REQUIRE(Read(vfs_file) == "pokopia-utf8");
        }

        const auto recreated = real.CreateFile(utf8_io_path, FileSys::OpenMode::ReadWrite);
        REQUIRE(recreated != nullptr);
        const std::string replaced = "replaced-utf8";
        REQUIRE(recreated->Write(reinterpret_cast<const u8*>(replaced.data()), replaced.size(),
                                 0) == replaced.size());
    }
    REQUIRE(ReadAll(io_file_path) == "replaced-utf8");

    const auto root = t.path / "Game";
    MakeDump(root);
    const fs::path archive = non_ascii_dir / std::u8string{u8"Pok\u00e9mon\u2122.nup"};
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    FileSys::Nup::MetadataEdit edit;
    edit.title_name = "Pokopia UTF-8";
    std::string error;
    INFO(error);
    REQUIRE(FileSys::Nup::EditNupMetadata(archive, edit, &error) ==
            FileSys::Nup::EditResult::Success);
    REQUIRE(FileSys::Nup::ProbeNupMetadata(archive).title_name == "Pokopia UTF-8");
}

TEST_CASE("ParseLaunchParams preserves non-ASCII UTF-8 paths across options and positional args",
          "[core][launch_params]") {
    Core::System system;
    const std::string utf8_game =
        "D:\\games\\Pok\xC3\xA9mon\xE2\x84\xA2 Pokopia\\Pok\xC3\xA9mon.nup";
    const std::string utf8_config = "D:\\cfg\\Pok\xC3\xA9mon\xE2\x84\xA2.ini";

    {
        std::vector<std::string> storage{"eden-cli", "-c", utf8_config, "-g", utf8_game};
        std::vector<char*> argv;
        for (auto& s : storage) {
            argv.push_back(s.data());
        }
        argv.push_back(nullptr);
        const auto lp = Core::ParseLaunchParams(
            system, static_cast<int>(storage.size()), argv.data(), nullptr);
        REQUIRE(lp.config_path == utf8_config);
        REQUIRE(lp.filepath == utf8_game);
    }

    {
        // Positional game before an option: getopt_long permutes argv; optind must still match.
        std::vector<std::string> storage{"eden-cli", utf8_game, "-f"};
        std::vector<char*> argv;
        for (auto& s : storage) {
            argv.push_back(s.data());
        }
        argv.push_back(nullptr);
        const auto lp = Core::ParseLaunchParams(
            system, static_cast<int>(storage.size()), argv.data(), nullptr);
        REQUIRE(lp.fullscreen);
        REQUIRE(lp.filepath == utf8_game);
    }

#ifdef _WIN32
    {
        // Narrow argv is mangled ACP, while argv_w holds the real UTF-16 strings; options and
        // positional args (even before -f) must all use argv_w without permutation mismatch.
        std::vector<std::string> narrow_storage{"eden-cli", "D:\\games\\Pok?mon?.nup", "-f",
                                                "-c",       "D:\\cfg\\Pok?mon?.ini",
                                                "--nup-name", "Pok?mon?"};
        std::vector<std::wstring> wide_storage{
            L"eden-cli",
            L"D:\\games\\Pok\u00e9mon\u2122 Pokopia\\Pok\u00e9mon.nup",
            L"-f",
            L"-c",
            L"D:\\cfg\\Pok\u00e9mon\u2122.ini",
            L"--nup-name",
            L"Pok\u00e9mon\u2122",
        };
        std::vector<char*> argv;
        std::vector<wchar_t*> argv_w;
        for (auto& s : narrow_storage) {
            argv.push_back(s.data());
        }
        for (auto& ws : wide_storage) {
            argv_w.push_back(ws.data());
        }
        argv.push_back(nullptr);
        argv_w.push_back(nullptr);

        const auto lp = Core::ParseLaunchParams(
            system, static_cast<int>(narrow_storage.size()), argv.data(), argv_w.data());
        REQUIRE(lp.fullscreen);
        REQUIRE(lp.filepath == utf8_game);
        REQUIRE(lp.config_path == utf8_config);
        REQUIRE(lp.edit_nup_name == std::string{"Pok\xC3\xA9mon\xE2\x84\xA2"});
    }

    {
        std::vector<std::string> narrow_storage{"eden-cli", "-g", "D:\\games\\Pok?mon?.nup"};
        std::vector<std::wstring> wide_storage{
            L"eden-cli", L"-g", L"D:\\games\\Pok\u00e9mon\u2122 Pokopia\\Pok\u00e9mon.nup"};
        std::vector<char*> argv;
        std::vector<wchar_t*> argv_w;
        for (auto& s : narrow_storage) {
            argv.push_back(s.data());
        }
        for (auto& ws : wide_storage) {
            argv_w.push_back(ws.data());
        }
        argv.push_back(nullptr);
        argv_w.push_back(nullptr);

        const auto lp = Core::ParseLaunchParams(
            system, static_cast<int>(narrow_storage.size()), argv.data(), argv_w.data());
        REQUIRE(lp.filepath == utf8_game);
    }
#endif
}

TEST_CASE("NUP large archive roundtrip preserves bytes and metadata", "[nup]") {
    using namespace FileSys::Nup;

    TempDir t;
    const auto root = t.path / "Game";
    MakeDump(root);
    Put(root / "Base/RomFS/large.bin", [] {
        std::string data(20 * 1024 * 1024, '\0');
        u32 x = 1;
        for (auto& c : data) {
            x = x * 1664525 + 1013904223;
            c = static_cast<char>('a' + (x >> 28));
        }
        return data;
    }());

    REQUIRE(IsLooseDump(root));
    const auto resolved_root = DumpRootFromMain(root / "Base/ExeFS/main");
    REQUIRE(resolved_root.has_value());

    const auto meta = ProbeMetadata(root);
    REQUIRE(meta.display_version == "1.1.0");
    REQUIRE(meta.version_source == "update 65536");

    const auto archive = t.path / "Game.nup";
    REQUIRE(PackDump(root, archive) == PackResult::Success);

    std::string hex;
    for (const u8 b : Sha256(ReadAll(archive))) {
        hex += fmt::format("{:02x}", b);
    }
    REQUIRE(hex == "718231e0974ca04c7be7bd6ed4334015b777c6e5f1ed51de23f899f81ee004ab");

    {
        FileSys::RealVfsFilesystem vfs;
        const auto vf =
            vfs.OpenFile(Common::FS::PathToUTF8String(archive), FileSys::OpenMode::Read);
        REQUIRE(vf != nullptr);
        REQUIRE(NupContainer::IsNupFile(vf));
        NupContainer container{vf};
        REQUIRE(container.GetStatus() == Loader::ResultStatus::Success);
        REQUIRE(container.VerifyIntegrity({}) == Loader::ResultStatus::Success);
    }

    MetadataEdit edit;
    edit.title_name = "Edited Title";
    edit.display_version = "2.5.0";
    REQUIRE(EditNupMetadata(archive, edit) == EditResult::Success);

    const auto updated_meta = ProbeNupMetadata(archive);
    REQUIRE(updated_meta.title_name == "Edited Title");
    REQUIRE(updated_meta.display_version == "2.5.0");

    const auto extracted = t.path / "Extracted";
    REQUIRE(ExtractNup(archive, extracted / "") == ExtractResult::Success);
    REQUIRE(fs::is_regular_file(extracted / "base/exefs/main"));
    REQUIRE(fs::is_regular_file(extracted / "base/romfs/large.bin"));
}

TEST_CASE("NUP boots the program that matches its base", "[nup][boot-inputs]") {
    TempDir t;
    const auto root = t.path / "Game";
    constexpr u64 edition_program = TitleId + 1;
    const u64 legacy_program = FileSys::WithPlatformOf(TitleId, 0x0100000000000000ULL);
    bool base_switch2 = true;
    bool update_switch2 = false;
    bool base_exefs = true;

    SECTION("a legacy update does not replace the native base program") {}
    SECTION("a native update supersedes a legacy base") {
        base_switch2 = false;
        update_switch2 = true;
    }
    SECTION("a native update boots over a base packed without an executable") {
        base_exefs = false;
        update_switch2 = true;
    }

    const auto write_exefs = [](const fs::path& path, u64 program_id, bool switch2) {
        Put(path / "main", "main");
        Put(path / "main.npdm", BootNpdm(program_id, switch2));
    };
    if (base_exefs) {
        write_exefs(root / "base/exefs", base_switch2 ? edition_program : legacy_program,
                    base_switch2);
    } else {
        fs::create_directories(root / "base/exefs");
    }
    Put(root / "base/romfs/base.bin", "base");
    write_exefs(root / "updates/65536/exefs", update_switch2 ? edition_program : legacy_program,
                update_switch2);
    Put(root / "updates/65536/romfs/update.bin", "update");
    const auto archive = t.path / "Game.nup";
    REQUIRE(FileSys::Nup::PackDump(root, archive) == FileSys::Nup::PackResult::Success);

    Core::System system;
    FileSys::RealVfsFilesystem vfs;
    Loader::AppLoader_NUP loader{
        vfs.OpenFile(Common::FS::PathToUTF8String(archive), FileSys::OpenMode::Read)};
    FileSys::ProgramMetadata metadata;
    REQUIRE(loader.ReadProgramMetadata(system, metadata) == Loader::ResultStatus::Success);
    CHECK(metadata.IsSwitch2());
    CHECK(metadata.GetTitleID() == edition_program);
    u64 program_id{};
    REQUIRE(loader.ReadProgramId(program_id) == Loader::ResultStatus::Success);
    CHECK(program_id == edition_program);
}

TEST_CASE("NUP recovery from malformed journals, destructive pack, and legacy archives", "[nup]") {
    using namespace FileSys::Nup;

    const auto copy_dump = [](const fs::path& src, const fs::path& dst) {
        fs::create_directories(dst);
        fs::copy(src, dst, fs::copy_options::recursive);
    };

    const auto run_malformed_journal_suite = [&]() {
        TempDir t;
        const auto template_dump = t.path / "template" / "Game";
        MakeDump(template_dump);
        const auto ref_archive = t.path / "ref.nup";
        REQUIRE(PackDump(template_dump, ref_archive) == PackResult::Success);
        const auto ref_bytes = ReadAll(ref_archive);

        const auto make_partial_run = [&](const fs::path& work_dir, const fs::path& archive,
                                          std::string& out_plan_line,
                                          std::vector<std::string>& out_ckpt_lines) {
            copy_dump(template_dump, work_dir);
            PackOptions opts{.delete_sources = true, .chunk_bytes = 1};
            std::atomic_bool cancel{false};
            REQUIRE(PackDump(
                        work_dir, archive, opts,
                        [&](const PackProgress& p) {
                            if (p.files_done >= 3) {
                                cancel.store(true, std::memory_order_relaxed);
                            }
                        },
                        &cancel) == PackResult::Cancelled);
            const auto journal_text = ReadAll(archive.string() + ".journal");
            std::vector<std::string> lines;
            size_t pos = 0;
            while (pos < journal_text.size()) {
                const auto nl = journal_text.find('\n', pos);
                if (nl == std::string::npos) {
                    break;
                }
                lines.push_back(journal_text.substr(pos, nl - pos));
                pos = nl + 1;
            }
            REQUIRE(lines.size() >= 2);
            out_plan_line = lines[0];
            out_ckpt_lines.assign(lines.begin() + 1, lines.end());
        };

        // 1. Line 1 truncated (no '\n') and invalid JSON line 1 fall back to a fresh pack.
        for (const std::string& bad_line1 : {std::string{"{\"format\":\"nup-journal\""},
                                             std::string{"{not-json\n"}}) {
            const auto case_name =
                bad_line1.back() == '\n' ? std::string{"bad_j_json"} : std::string{"bad_j_trunc"};
            const auto dump = t.path / case_name / "Game";
            copy_dump(template_dump, dump);
            const auto archive = t.path / (case_name + ".nup");
            Put(archive.string() + ".part", "stale");
            Put(archive.string() + ".journal", bad_line1);
            PackOptions opts{.delete_sources = true, .chunk_bytes = 1};
            REQUIRE(PackDump(dump, archive, opts) == PackResult::Success);
            REQUIRE(ReadAll(archive) == ref_bytes);
            REQUIRE_FALSE(fs::exists(archive.string() + ".part"));
            REQUIRE_FALSE(fs::exists(archive.string() + ".journal"));
        }

        // 2. Corrupt line 1 valid JSON schema must fail with IoError (never silently reset).
        {
            const auto dump = t.path / "bad_plan_schema" / "Game";
            copy_dump(template_dump, dump);
            const auto archive = t.path / "bad_plan_schema.nup";
            Put(archive.string() + ".part", "stale");
            Put(archive.string() + ".journal", "{\"format\":\"wrong\",\"version\":1}\n");
            PackOptions opts{.delete_sources = true, .chunk_bytes = 1};
            REQUIRE(PackDump(dump, archive, opts) == PackResult::IoError);
            REQUIRE_FALSE(fs::exists(archive));
            REQUIRE(fs::exists(archive.string() + ".part"));
            REQUIRE(fs::exists(archive.string() + ".journal"));
            REQUIRE(fs::exists(dump / "Base/ExeFS/main"));
        }

        // Do not reset an unreadable journal or a valid JSON plan with invalid fields.
        {
            const auto dump = t.path / "bad_plan" / "Game";
            copy_dump(template_dump, dump);
            const auto archive = t.path / "bad_plan.nup";
            const auto part = archive.string() + ".part";
            const auto journal = archive.string() + ".journal";
            Put(part, "keep partial data");
            PackOptions opts{.delete_sources = true, .chunk_bytes = 1};

            for (const std::string_view plan : {
                     R"({"format":"nup-journal","version":1,"packages":[],"entries":[]})",
                     R"({"format":"nup-journal","version":1.0,"entries":[],)"
                     R"("packages":[{"kind":0,"flags":0,"title_id":1,"version":0,"meta":""}]})",
                     R"({"format":"nup-journal","version":1,"packages":{},"entries":[]})",
                 }) {
                Put(journal, std::string{plan} + '\n');
                REQUIRE(PackDump(dump, archive, opts) == PackResult::IoError);
                REQUIRE(ReadAll(part) == "keep partial data");
                REQUIRE_FALSE(fs::exists(archive));
                REQUIRE(fs::exists(dump / "Base/ExeFS/main"));
            }

            REQUIRE(fs::remove(journal));
            REQUIRE(fs::create_directory(journal));
            REQUIRE(PackDump(dump, archive, opts) == PackResult::IoError);
            REQUIRE(ReadAll(part) == "keep partial data");
            REQUIRE_FALSE(fs::exists(archive));
            REQUIRE(fs::exists(dump / "Base/ExeFS/main"));
        }

        // 3. Torn trailing checkpoint line + oversized .part tail recovers cleanly.
        for (const std::string& torn_tail :
             {std::string{"{\"file\":99"}, std::string{"{corrupt-json\n"}, std::string{"\n"}}) {
            const auto case_name = "torn_" + std::to_string(torn_tail.size());
            const auto dump = t.path / case_name / "Game";
            const auto archive = t.path / (case_name + ".nup");
            std::string plan_line;
            std::vector<std::string> ckpts;
            make_partial_run(dump, archive, plan_line, ckpts);

            std::string rewritten = plan_line + "\n";
            for (const auto& c : ckpts) {
                rewritten += c + "\n";
            }
            rewritten += torn_tail;
            Put(archive.string() + ".journal", rewritten);
            {
                std::ofstream app(archive.string() + ".part", std::ios::binary | std::ios::app);
                app << std::string(128, '\xAB');
            }
            PackOptions opts{.delete_sources = true, .chunk_bytes = 1};
            std::atomic_bool cancel{false};
            REQUIRE(PackDump(
                        dump, archive, opts,
                        [&](const PackProgress& p) {
                            if (p.files_done >= ckpts.size() + 2) {
                                cancel = true;
                            }
                        },
                        &cancel) == PackResult::Cancelled);
            REQUIRE_FALSE(fs::exists(archive));
            const auto resumed_journal = ReadAll(archive.string() + ".journal");
            REQUIRE(static_cast<size_t>(
                        std::count(resumed_journal.begin(), resumed_journal.end(), '\n')) >
                    ckpts.size() + 1);
            REQUIRE(resumed_journal.find(torn_tail, rewritten.size() - torn_tail.size()) !=
                    rewritten.size() - torn_tail.size());
            REQUIRE(PackDump(dump, archive, opts) == PackResult::Success);
            REQUIRE(ReadAll(archive) == ref_bytes);
        }

        // 4. Valid JSON checkpoint errors must reject with IoError:
        // missing fields, out-of-range file_index, bad sha256, invalid codec, short .part.
        {
            const auto dump = t.path / "bad_ckpt_base";
            const auto archive = t.path / "bad_ckpt_base.nup";
            std::string plan_line;
            std::vector<std::string> ckpts;
            make_partial_run(dump, archive, plan_line, ckpts);
            const auto orig_part = ReadAll(archive.string() + ".part");
            std::string good_first = plan_line + "\n";
            for (const auto& c : ckpts) {
                good_first += c + "\n";
            }

            const auto expect_io_error = [&](const std::string& jcontent, const std::string& pdata) {
                Put(archive.string() + ".journal", jcontent);
                Put(archive.string() + ".part", pdata);
                PackOptions opts{.delete_sources = true, .chunk_bytes = 1};
                REQUIRE(PackDump(dump, archive, opts) == PackResult::IoError);
                REQUIRE_FALSE(fs::exists(archive));
            };

            expect_io_error(good_first + "{\"file\":0}\n", orig_part);
            expect_io_error(
                good_first +
                    "{\"file\":9999,\"size\":1,\"sha256\":\"" + std::string(64, '0') +
                    "\",\"offset\":1024,\"stored_size\":1,\"codec\":0}\n",
                orig_part);
            expect_io_error(
                good_first +
                    "{\"file\":0,\"size\":1,\"sha256\":\"deadbeef\",\"offset\":1024,"
                    "\"stored_size\":1,\"codec\":0}\n",
                orig_part);
            expect_io_error(
                good_first +
                    "{\"file\":0,\"size\":1,\"sha256\":\"" + std::string(64, '0') +
                    "\",\"offset\":1024,\"stored_size\":1,\"codec\":2}\n",
                orig_part);
            expect_io_error(
                good_first +
                    "{\"file\":0,\"size\":10,\"sha256\":\"" + std::string(64, '0') +
                    "\",\"offset\":1024,\"stored_size\":5,\"codec\":0}\n",
                orig_part);
            expect_io_error(
                good_first +
                    "{\"file\":0,\"size\":0,\"sha256\":\"" + std::string(64, '0') +
                    "\",\"offset\":0,\"stored_size\":0,\"codec\":0.5}\n",
                orig_part);
            // Offset + stored_size past .part EOF
            expect_io_error(plan_line + "\n" + ckpts.back() + "\n", orig_part.substr(0, 64));

            REQUIRE(fs::remove(dump / "dlc/0400ABCD00011001/0/romfs/addon.bin"));
            Put(archive.string() + ".journal", good_first);
            Put(archive.string() + ".part", orig_part);
            PackOptions opts{.delete_sources = true, .chunk_bytes = 1};
            REQUIRE(PackDump(dump, archive, opts) == PackResult::InvalidSource);
            REQUIRE_FALSE(fs::exists(archive));
            REQUIRE(HasResumableJournal(archive));
        }
    };

    const auto run_destructive_pack_suite = [&]() {
        TempDir t;
        const auto template_dump = t.path / "template";
        MakeDump(template_dump);
        const auto ref_archive = t.path / "ref.nup";
        REQUIRE(PackDump(template_dump, ref_archive) == PackResult::Success);
        const auto ref_bytes = ReadAll(ref_archive);

        // 1. Packing over existing archive fails without touching archive or sources.
        for (bool del : {false, true}) {
            const auto dump = t.path / (del ? "exist_del" : "exist_keep");
            copy_dump(template_dump, dump);
            const auto archive = t.path / (dump.filename().string() + ".nup");
            const auto part = archive.string() + ".part";
            Put(archive, "keep-me");
            Put(part, "keep partial data");
            PackOptions opts{.delete_sources = del, .chunk_bytes = 1};
            REQUIRE(PackDump(dump, archive, opts) == PackResult::IoError);
            REQUIRE(ReadAll(archive) == "keep-me");
            REQUIRE(ReadAll(part) == "keep partial data");
            REQUIRE(fs::exists(dump / "Base/ExeFS/main"));
        }

        // 2. Mid-chunk cancel leaves .journal clean, then resume produces exact archive & keeps root.
        {
            const auto dump = t.path / "clean_cancel";
            Put(dump / "base/exefs/main", "main");
            Put(dump / "base/exefs/main.npdm", Npdm(TitleId));
            Put(dump / "base/romfs/a.bin", std::string(4096, 'a'));
            Put(dump / "base/romfs/b.bin", std::string(4096, 'b'));
            const auto single = t.path / "single.nup";
            REQUIRE(PackDump(dump, single) == PackResult::Success);

            const auto archive = t.path / "clean_cancel.nup";
            PackOptions opts{.delete_sources = true, .chunk_bytes = 64ULL * 1024 * 1024};
            std::atomic_bool cancel{false};
            REQUIRE(PackDump(
                        dump, archive, opts,
                        [&](const PackProgress& p) {
                            if (p.files_done >= 2) {
                                cancel.store(true, std::memory_order_relaxed);
                            }
                        },
                        &cancel) == PackResult::Cancelled);
            REQUIRE(HasResumableJournal(archive));
            REQUIRE(fs::exists(dump / "base/exefs/main"));
            const auto jtext = ReadAll(archive.string() + ".journal");
            REQUIRE(std::count(jtext.begin(), jtext.end(), '\n') == 1);

            REQUIRE(PackDump(dump, archive, opts) == PackResult::Success);
            REQUIRE_FALSE(HasResumableJournal(archive));
            REQUIRE(ReadAll(archive) == ReadAll(single));
            REQUIRE(fs::is_directory(dump));
            REQUIRE_FALSE(fs::exists(dump / "base"));
        }

        // 3. Packing onto a directory fails with IoError and preserves destructive state.
        {
            const auto dump = t.path / "dir_target_dump";
            copy_dump(template_dump, dump);
            const auto archive = t.path / "dir_target.nup";
            fs::create_directories(archive);
            PackOptions opts{.delete_sources = true, .chunk_bytes = 1};
            REQUIRE(PackDump(dump, archive, opts) == PackResult::IoError);
            REQUIRE(fs::is_directory(archive));
            REQUIRE(fs::exists(dump / "Base/ExeFS/main"));
        }
    };

    const auto run_legacy_archive_suite = [&]() {
        TempDir t;
        const auto root = t.path / "LegacyDump";
        Put(root / "updates/65536/exefs/main", "legacy-main");
        Put(root / "updates/65536/exefs/main.npdm", Npdm(TitleId));
        Put(root / "updates/65536/romfs/data.bin", std::string(1024, 'L'));
        const auto archive = t.path / "Legacy.nup";
        PackOptions opts;
        opts.metadata.title_name = "Legacy Title";
        opts.metadata.display_version = "1.2.3";
        REQUIRE(PackDump(root, archive, opts) == PackResult::Success);

        // Variant A: Legacy archive with non-zero reserved fields, LocalizedIcons (0x000A),
        // unknown TLV (0x0077), trailing <8 byte TLV padding, and toc_offset=0x80 (editable).
        {
            auto bytes = ReadAll(archive);
            const u64 toc_off = Le(bytes, 0x08, 8);
            const u64 toc_sz = Le(bytes, 0x10, 8);
            const u64 pkg_pos = toc_off + 0x30;
            const u64 old_meta_off = Le(bytes, pkg_pos + 0x20, 8);
            const u32 old_meta_sz = static_cast<u32>(Le(bytes, pkg_pos + 0x28, 4));
            std::string meta = bytes.substr(old_meta_off, old_meta_sz);

            // Append LocalizedIcons (0x000A) + unknown TLV (0x0077) + 3-byte trailing pad.
            const std::string lang = "en-us";
            const std::string icon_data = "ICONDATA";
            std::string loc_payload;
            loc_payload.push_back(static_cast<char>(lang.size()));
            loc_payload += lang;
            loc_payload.resize(loc_payload.size() + 4, '\0');
            SetLe(loc_payload, loc_payload.size() - 4, icon_data.size(), 4);
            loc_payload += icon_data;
            auto append_tlv = [&](u16 type, const std::string& payload) {
                const size_t at = meta.size();
                meta.resize(at + 8 + payload.size(), '\0');
                SetLe(meta, at, type, 2);
                SetLe(meta, at + 2, 0xBEEF, 2); // non-zero TLV reserved
                SetLe(meta, at + 4, payload.size(), 4);
                meta.replace(at + 8, payload.size(), payload);
            };
            append_tlv(0x000A, loc_payload);
            append_tlv(0x0077, "LEGACY_EXTRA");
            meta.append(3, '\x55'); // trailing <8 byte alignment padding

            bytes.resize(old_meta_off);
            bytes += meta;
            SetLe(bytes, pkg_pos + 0x28, meta.size(), 4);
            // Non-zero reserved fields in superblock, TOC header, package, dir, and file records.
            SetLe(bytes, 0x06, 0, 2); // older header_version = 0
            bytes[0x60] = 0x42;
            SetLe(bytes, toc_off + 0x28, 0x1122334455667788ULL, 8);
            SetLe(bytes, pkg_pos + 0x1A, 0x1234, 2);
            SetLe(bytes, pkg_pos + 0x1C, 0x56789ABC, 4);
            SetLe(bytes, 0x40, bytes.size(), 8);
            const auto toc_hash = Sha256(std::string_view{bytes}.substr(toc_off, toc_sz));
            std::copy(toc_hash.begin(), toc_hash.end(), bytes.begin() + 0x18);

            const auto legacy_path = t.path / "LegacyEditable.nup";
            Put(legacy_path, bytes);

            {
                FileSys::RealVfsFilesystem real;
                NupContainer c{real.OpenFile(Common::FS::PathToUTF8String(legacy_path),
                                             FileSys::OpenMode::Read)};
                REQUIRE(c.GetStatus() == Loader::ResultStatus::Success);
                REQUIRE(c.VerifyIntegrity({}) == Loader::ResultStatus::Success);
                REQUIRE(c.GetPackages().size() == 1);
                REQUIRE(c.GetPackages()[0].standalone_update);
                REQUIRE(c.GetPackages()[0].title_name == "Legacy Title");
                REQUIRE(c.GetPackages()[0].localized_icons.size() == 1);
                REQUIRE(c.GetPackages()[0].localized_icons[0].first == "en-us");
            }

            MetadataEdit edit;
            edit.title_name = "Updated Legacy";
            REQUIRE(EditNupMetadata(legacy_path, edit) == EditResult::Success);
            REQUIRE(ProbeNupMetadata(legacy_path).title_name == "Updated Legacy");

            const auto out_dir = t.path / ("Extract_" + std::to_string(bytes.size()));
            if (fs::exists(out_dir)) {
                fs::remove_all(out_dir);
            }
            REQUIRE(ExtractNup(legacy_path, out_dir) == ExtractResult::Success);
            REQUIRE(ReadAll(out_dir / "updates/65536/romfs/data.bin") == std::string(1024, 'L'));
        }

        // Variant B: Extended superblock (header_size = 0x100, toc_offset = 0x100).
        {
            auto bytes = ReadAll(archive);
            const u64 old_toc_off = Le(bytes, 0x08, 8);
            const u64 toc_sz = Le(bytes, 0x10, 8);
            const u64 old_blob = Le(bytes, 0x38, 8);
            const u64 shift = 0x80;
            const auto toc = bytes.substr(old_toc_off, toc_sz);
            const auto tail = bytes.substr(old_blob);

            std::string ext(0x100, '\0');
            std::copy_n(bytes.begin(), 0x80, ext.begin());
            SetLe(ext, 0x04, 0x100, 2); // extended header_size
            SetLe(ext, 0x06, 2, 2);     // newer/legacy header_version
            SetLe(ext, 0x08, old_toc_off + shift, 8);
            SetLe(ext, 0x38, old_blob + shift, 8);

            std::string new_toc = toc;
            const u64 pkg_pos = 0x30;
            const u64 file_count = Le(new_toc, 0x10, 8);
            const u64 dir_count = Le(new_toc, 0x08, 8);
            const u64 pkg_count = Le(new_toc, 0x04, 4);
            for (u64 i = 0; i < pkg_count; ++i) {
                const u64 off = pkg_pos + i * 0x40 + 0x20;
                if (Le(new_toc, off, 8) != 0) {
                    SetLe(new_toc, off, Le(new_toc, off, 8) + shift, 8);
                }
            }
            const u64 files_pos = pkg_pos + pkg_count * 0x40 + dir_count * 0x18;
            for (u64 i = 0; i < file_count; ++i) {
                const u64 off = files_pos + i * 0x50 + 0x10;
                if (Le(new_toc, off, 8) != 0) {
                    SetLe(new_toc, off, Le(new_toc, off, 8) + shift, 8);
                }
            }
            const auto toc_hash = Sha256(new_toc);
            std::copy(toc_hash.begin(), toc_hash.end(), ext.begin() + 0x18);
            const u64 gap = (old_blob + shift) - (0x100 + toc_sz);
            std::string full = ext + new_toc + std::string(gap, '\0') + tail;
            SetLe(full, 0x40, full.size(), 8);

            const auto ext_path = t.path / "LegacyExtSuper.nup";
            Put(ext_path, full);
            FileSys::RealVfsFilesystem real;
            NupContainer c{
                real.OpenFile(Common::FS::PathToUTF8String(ext_path), FileSys::OpenMode::Read)};
            REQUIRE(c.GetStatus() == Loader::ResultStatus::Success);
            REQUIRE(c.VerifyIntegrity({}) == Loader::ResultStatus::Success);
            REQUIRE(c.GetPackages()[0].title_name == "Legacy Title");
        }
    };

    run_malformed_journal_suite();
    run_destructive_pack_suite();
    run_legacy_archive_suite();
}
