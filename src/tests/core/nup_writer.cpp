// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <openssl/evp.h>

#include "common/fs/file.h"
#include "common/fs/path_util.h"
#include "core/core.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/nup_writer.h"
#include "core/file_sys/control_metadata.h"
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

TEST_CASE("NUP roundtrip and Rust FFI differential parity", "[nup]") {
    using namespace FileSys::Nup;

    const auto roundtrip_check = [&](const std::string& expected_repro_sha,
                                     bool check_scratch_preservation = false) {
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
        const auto legacy_archive = t.path / "Game.nup.rust_verify_tmp";
        const auto legacy_part = t.path / "Game.nup.rust_verify_tmp.part";
        const auto legacy_journal = t.path / "Game.nup.rust_verify_tmp.journal";
        if (check_scratch_preservation) {
            Put(legacy_archive, "existing archive");
            Put(legacy_part, "existing partial archive");
            Put(legacy_journal, "existing journal");
        }
        REQUIRE(PackDump(root, archive) == PackResult::Success);
        if (check_scratch_preservation) {
            CHECK(ReadAll(legacy_archive) == "existing archive");
            CHECK(ReadAll(legacy_part) == "existing partial archive");
            CHECK(ReadAll(legacy_journal) == "existing journal");
            Put(legacy_archive, "existing archive before edit");
            Put(legacy_part, "existing partial archive before edit");
        }

        std::string hex;
        for (const u8 b : Sha256(ReadAll(archive))) {
            hex += fmt::format("{:02x}", b);
        }
        REQUIRE(hex == expected_repro_sha);

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
        edit.title_name = "Rust Parity Title";
        edit.display_version = "2.5.0";
        REQUIRE(EditNupMetadata(archive, edit) == EditResult::Success);
        if (check_scratch_preservation) {
            CHECK(ReadAll(legacy_archive) == "existing archive before edit");
            CHECK(ReadAll(legacy_part) == "existing partial archive before edit");
        }

        const auto updated_meta = ProbeNupMetadata(archive);
        REQUIRE(updated_meta.title_name == "Rust Parity Title");
        REQUIRE(updated_meta.display_version == "2.5.0");

        const auto extracted = t.path / "Extracted";
        const auto legacy_extract_sentinel = t.path / "Extracted.rust_verify_tmp/nested/keep.txt";
        if (check_scratch_preservation) {
            Put(legacy_extract_sentinel, "existing extracted data");
        }
        REQUIRE(ExtractNup(archive, extracted) == ExtractResult::Success);
        if (check_scratch_preservation) {
            CHECK(ReadAll(legacy_extract_sentinel) == "existing extracted data");
        }
        REQUIRE(fs::is_regular_file(extracted / "base/exefs/main"));
        REQUIRE(fs::is_regular_file(extracted / "base/romfs/large.bin"));

        if (check_scratch_preservation) {
            const auto output_entries = [&] {
                std::vector<fs::path> names;
                for (const auto& entry : fs::directory_iterator{t.path}) {
                    names.push_back(entry.path().filename());
                }
                std::sort(names.begin(), names.end());
                return names;
            };
            const auto trailing = t.path / "Trailing";
            auto expected_entries = output_entries();
            expected_entries.push_back(trailing.filename());
            std::sort(expected_entries.begin(), expected_entries.end());

            REQUIRE(ExtractNup(archive, trailing / "") == ExtractResult::Success);
            CHECK(fs::is_regular_file(trailing / "base/exefs/main"));
            CHECK(output_entries() == expected_entries);
        }
    };

    constexpr auto kExpectedReproSha =
        "718231e0974ca04c7be7bd6ed4334015b777c6e5f1ed51de23f899f81ee004ab";
    roundtrip_check(kExpectedReproSha);

#ifdef EGLANTINE_ENABLE_RUST_NUP
    struct ScopedEnvVar {
        const char* name;
        std::optional<std::string> prev;
        explicit ScopedEnvVar(const char* n, const char* value) : name{n} {
            if (const char* old = std::getenv(n); old != nullptr) {
                prev = std::string{old};
            }
            Set(value);
        }
        ~ScopedEnvVar() {
            Set(prev ? prev->c_str() : nullptr);
        }
        void Set(const char* val) const {
#ifdef _WIN32
            _putenv_s(name, val != nullptr ? val : "");
#else
            if (val != nullptr) {
                setenv(name, val, 1);
            } else {
                unsetenv(name);
            }
#endif
        }
    };

    {
        ScopedEnvVar rust_mode{"EDEN_NX2_RUST_NUP", nullptr};
        ScopedEnvVar verify_mode{"EDEN_NX2_RUST_NUP_VERIFY", "1"};
        roundtrip_check(kExpectedReproSha, true);
    }

    {
        ScopedEnvVar verify_mode{"EDEN_NX2_RUST_NUP_VERIFY", nullptr};
        ScopedEnvVar rust_mode{"EDEN_NX2_RUST_NUP", "1"};
        roundtrip_check(kExpectedReproSha);

        // Also test cancel + journal resume with delete_sources under Rust backend.
        TempDir rt;
        const auto dump = rt.path / "ResumeGame";
        MakeDump(dump);
        const auto archive = rt.path / "ResumeGame.nup";
        PackOptions opts;
        opts.delete_sources = true;
        opts.chunk_bytes = 1;
        std::atomic_bool cancel{false};
        const auto cancelled = PackDump(
            dump, archive, opts,
            [&](const PackProgress& p) {
                if (p.files_done >= 3) {
                    cancel.store(true, std::memory_order_relaxed);
                }
            },
            &cancel);
        REQUIRE(cancelled == PackResult::Cancelled);
        REQUIRE(HasResumableJournal(archive));
        REQUIRE(PackDump(dump, archive, opts) == PackResult::Success);
        REQUIRE_FALSE(HasResumableJournal(archive));
        FileSys::RealVfsFilesystem vfs;
        const auto vf = vfs.OpenFile(Common::FS::PathToUTF8String(archive), FileSys::OpenMode::Read);
        REQUIRE(vf != nullptr);
        NupContainer resumed{vf};
        REQUIRE(resumed.GetStatus() == Loader::ResultStatus::Success);
        REQUIRE(resumed.VerifyIntegrity({}) == Loader::ResultStatus::Success);
    }
#endif
}
