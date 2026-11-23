// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <openssl/evp.h>

#include "common/alignment.h"
#include "common/common_funcs.h"
#include "common/cpu_features.h"
#include "common/fs/path_util.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "common/swap.h"
#include "core/core.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/fs_filesystem.h"
#include "core/file_sys/fssystem/fssystem_nca_header.h"
#include "core/file_sys/nca_metadata.h"
#include "core/file_sys/patch_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/file_sys/vfs/vfs_vector.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/nvdrv/devices/nvhost_ctrl_gpu.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"
#include "core/hle/service/nvdrv/nvdata.h"
#include "core/hle/service/nvdrv/nvdrv.h"
#include "core/loader/deconstructed_rom_directory.h"
#include "core/loader/loader.h"
#include "core/loader/nro.h"
#include "core/loader/nso.h"
#include "core/platform_profile.h"
#include "video_core/host1x/host1x.h"
#include "video_core/nx2.h"

namespace {

struct IsolatedProfileTestEnv {
    std::filesystem::path root;
    std::filesystem::path prev_nand = Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir);
    std::filesystem::path prev_sdmc = Common::FS::GetEdenPath(Common::FS::EdenPath::SDMCDir);
    std::filesystem::path prev_load = Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir);
    std::filesystem::path prev_cache = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir);
    std::filesystem::path prev_config = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir);
    std::filesystem::path prev_shader = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir);
    std::filesystem::path prev_keys = Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir);

    Settings::RendererBackend prev_renderer = Settings::values.renderer_backend.GetValue();
    Settings::AudioEngine prev_audio = Settings::values.sink_id.GetValue();
    Settings::MemoryLayout prev_memory = Settings::values.memory_layout_mode.GetValue();
    bool prev_multicore = Settings::values.use_multi_core.GetValue();
    bool prev_renderdoc = Settings::values.enable_renderdoc_hotkey.GetValue();
    s32 prev_user = Settings::values.current_user.GetValue();
    bool prev_nx2_ampere = Tegra::g_nx2_ampere.load(std::memory_order_relaxed);
    bool prev_nx2_driver = Service::Nvidia::Devices::g_nx2_driver.load(std::memory_order_relaxed);

    explicit IsolatedProfileTestEnv(std::string_view suffix) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
               ("eglantine-platform-profile-" + std::string(suffix) + "-" + std::to_string(stamp));
        std::filesystem::create_directories(root / "nand");
        std::filesystem::create_directories(root / "sdmc");
        std::filesystem::create_directories(root / "load");
        std::filesystem::create_directories(root / "cache");
        std::filesystem::create_directories(root / "config");
        std::filesystem::create_directories(root / "shader");
        std::filesystem::create_directories(root / "keys");

        Common::FS::SetEdenPath(Common::FS::EdenPath::NANDDir, root / "nand");
        Common::FS::SetEdenPath(Common::FS::EdenPath::SDMCDir, root / "sdmc");
        Common::FS::SetEdenPath(Common::FS::EdenPath::LoadDir, root / "load");
        Common::FS::SetEdenPath(Common::FS::EdenPath::CacheDir, root / "cache");
        Common::FS::SetEdenPath(Common::FS::EdenPath::ConfigDir, root / "config");
        Common::FS::SetEdenPath(Common::FS::EdenPath::ShaderDir, root / "shader");
        Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, root / "keys");

        Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Null);
        Settings::values.sink_id.SetValue(Settings::AudioEngine::Null);
        Settings::values.memory_layout_mode.SetGlobal(true);
        Settings::values.memory_layout_mode.SetValue(Settings::MemoryLayout::Memory_4Gb);
        Settings::values.use_multi_core.SetValue(false);
        Settings::values.enable_renderdoc_hotkey.SetValue(false);

        auto& keys = Core::Crypto::KeyManager::Instance();
        if (!keys.HasKey(Core::Crypto::S128KeyType::KeyArea, 0, 0)) {
            keys.SetKey(Core::Crypto::S128KeyType::KeyArea,
                        Core::Crypto::Key128{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
                                             0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01},
                        0, 0);
        }
    }

    ~IsolatedProfileTestEnv() {
        Tegra::g_nx2_ampere.store(prev_nx2_ampere, std::memory_order_relaxed);
        Service::Nvidia::Devices::g_nx2_driver.store(prev_nx2_driver, std::memory_order_relaxed);
        Common::g_wall_clock.SetGuestCNTFRQ(Common::WallClock::CNTFRQ);
        Settings::SetCurrentProgramIsSwitch2(false);

        Settings::values.renderer_backend.SetValue(prev_renderer);
        Settings::values.sink_id.SetValue(prev_audio);
        Settings::values.memory_layout_mode.SetGlobal(true);
        Settings::values.memory_layout_mode.SetValue(prev_memory);
        Settings::values.use_multi_core.SetValue(prev_multicore);
        Settings::values.enable_renderdoc_hotkey.SetValue(prev_renderdoc);
        Settings::values.current_user = prev_user;

        Common::FS::SetEdenPath(Common::FS::EdenPath::NANDDir, prev_nand);
        Common::FS::SetEdenPath(Common::FS::EdenPath::SDMCDir, prev_sdmc);
        Common::FS::SetEdenPath(Common::FS::EdenPath::LoadDir, prev_load);
        Common::FS::SetEdenPath(Common::FS::EdenPath::CacheDir, prev_cache);
        Common::FS::SetEdenPath(Common::FS::EdenPath::ConfigDir, prev_config);
        Common::FS::SetEdenPath(Common::FS::EdenPath::ShaderDir, prev_shader);
        Common::FS::SetEdenPath(Common::FS::EdenPath::KeysDir, prev_keys);

        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    IsolatedProfileTestEnv(const IsolatedProfileTestEnv&) = delete;
    IsolatedProfileTestEnv& operator=(const IsolatedProfileTestEnv&) = delete;
};

template <typename T>
void WriteLe(std::vector<u8>& buf, std::size_t offset, T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(buf.data() + offset, &value, sizeof(T));
}

template <typename T>
T ReadLe(const std::vector<u8>& buf, std::size_t offset) {
    static_assert(std::is_trivially_copyable_v<T>);
    T value{};
    std::memcpy(&value, buf.data() + offset, sizeof(T));
    return value;
}

std::array<u8, 32> Sha256(const void* data, std::size_t size) {
    std::array<u8, 32> digest{};
    unsigned int len = 0;
    EVP_Digest(data, size, digest.data(), &len, EVP_sha256(), nullptr);
    REQUIRE(len == digest.size());
    return digest;
}

std::vector<u8> MakeNpdm(u64 title_id, bool is_switch2) {
    constexpr std::size_t AciOffset = 0x80;
    constexpr std::size_t AciSize = 0x80;
    constexpr std::size_t AcidOffset = 0x100;
    const std::size_t acid_sig_size = is_switch2 ? 0x300 : 0x200;
    const u32 fac_rel_offset = static_cast<u32>(acid_sig_size + 0x40);
    constexpr u32 FacSize = 0x2C;
    const u32 acid_size = fac_rel_offset + FacSize;

    std::vector<u8> npdm(AcidOffset + acid_size, 0);

    // Header (0x80 bytes)
    std::memcpy(npdm.data(), "META", 4);
    npdm[0x0C] = 0x07; // 64-bit instructions + Is39Bit address space
    npdm[0x0E] = 0x2C; // main_thread_priority
    npdm[0x0F] = 0x00; // main_thread_cpu
    WriteLe<u32>(npdm, 0x1C, 0x10000); // main_stack_size
    WriteLe<u32>(npdm, 0x70, static_cast<u32>(AciOffset));
    WriteLe<u32>(npdm, 0x74, static_cast<u32>(AciSize));
    WriteLe<u32>(npdm, 0x78, static_cast<u32>(AcidOffset));
    WriteLe<u32>(npdm, 0x7C, acid_size);

    // ACI0 header at AciOffset
    std::memcpy(npdm.data() + AciOffset, "ACI0", 4);
    WriteLe<u64>(npdm, AciOffset + 0x10, title_id);
    WriteLe<u32>(npdm, AciOffset + 0x20, 0x40); // fah_offset
    WriteLe<u32>(npdm, AciOffset + 0x24, 0x1C); // fah_size
    WriteLe<u32>(npdm, AciOffset + 0x28, 0x5C); // sac_offset
    WriteLe<u32>(npdm, AciOffset + 0x2C, 0x00); // sac_size
    WriteLe<u32>(npdm, AciOffset + 0x30, 0x60); // kac_offset
    WriteLe<u32>(npdm, AciOffset + 0x34, 0x04); // kac_size

    // FileAccessHeader (ACI0 + 0x40)
    npdm[AciOffset + 0x40] = 1;
    WriteLe<u64>(npdm, AciOffset + 0x44, 0xFFFFFFFFFFFFFFFFULL);

    // KernelAccessControl (ACI0 + 0x60): allow default core mask & priority range
    WriteLe<u32>(npdm, AciOffset + 0x60, 0x30043F7U);

    // ACID body after signature/key block
    const std::size_t body = AcidOffset + acid_sig_size;
    std::memcpy(npdm.data() + body, "ACID", 4);
    WriteLe<u64>(npdm, body + 0x10, 0);
    WriteLe<u64>(npdm, body + 0x18, 0xFFFFFFFFFFFFFFFFULL);
    WriteLe<u32>(npdm, body + 0x20, fac_rel_offset);
    WriteLe<u32>(npdm, body + 0x24, FacSize);

    // FileAccessControl
    const std::size_t fac = AcidOffset + fac_rel_offset;
    npdm[fac] = 1;
    WriteLe<u64>(npdm, fac + 0x04, 0xFFFFFFFFFFFFFFFFULL);

    return npdm;
}

std::vector<u8> MakeMinimalNso() {
    Loader::NSOHeader header{};
    header.magic = Common::MakeMagic('N', 'S', 'O', '0');
    header.flags = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        header.segments[i].offset = static_cast<u32>(sizeof(Loader::NSOHeader) + i * 0x1000);
        header.segments[i].location = static_cast<u32>(i * 0x1000);
        header.segments[i].size = 0x1000;
        header.segments[i].alignment = 1;
        header.segments_compressed_size[i] = 0x1000;
    }
    header.segments[2].bss_size = 0;

    std::vector<u8> nso(sizeof(Loader::NSOHeader) + 3 * 0x1000, 0);
    std::memcpy(nso.data(), &header, sizeof(header));
    // Place an ARM64 RET instruction in .text
    WriteLe<u32>(nso, sizeof(Loader::NSOHeader), 0xD65F03C0U);
    return nso;
}

std::vector<u8> MakeMinimalNro() {
    std::vector<u8> nro(0x80, 0);
    WriteLe<u32>(nro, 0x10, Common::MakeMagic('N', 'R', 'O', '0'));
    WriteLe<u32>(nro, 0x18, 0x80);
    return nro;
}

struct PfsFileSpec {
    std::string name;
    std::vector<u8> data;
};

std::vector<u8> MakePfs0(const std::vector<PfsFileSpec>& entries) {
    struct RawPfsHeader {
        u32_le magic;
        u32_le num_entries;
        u32_le strtab_size;
        u32_le reserved;
    };
    struct RawPfsEntry {
        u64_le offset;
        u64_le size;
        u32_le strtab_offset;
        u32_le reserved;
    };

    std::vector<u8> strtab;
    std::vector<RawPfsEntry> raw_entries;
    u64 data_offset = 0;
    for (const auto& entry : entries) {
        RawPfsEntry raw{};
        raw.offset = data_offset;
        raw.size = entry.data.size();
        raw.strtab_offset = static_cast<u32>(strtab.size());
        raw_entries.push_back(raw);
        strtab.insert(strtab.end(), entry.name.begin(), entry.name.end());
        strtab.push_back('\0');
        data_offset += entry.data.size();
    }
    while (strtab.size() % 0x20 != 0) {
        strtab.push_back('\0');
    }

    const std::size_t header_size =
        sizeof(RawPfsHeader) + raw_entries.size() * sizeof(RawPfsEntry) + strtab.size();
    std::vector<u8> out(header_size + static_cast<std::size_t>(data_offset), 0);

    RawPfsHeader header{};
    header.magic = Common::MakeMagic('P', 'F', 'S', '0');
    header.num_entries = static_cast<u32>(entries.size());
    header.strtab_size = static_cast<u32>(strtab.size());
    std::memcpy(out.data(), &header, sizeof(header));
    if (!raw_entries.empty()) {
        std::memcpy(out.data() + sizeof(RawPfsHeader), raw_entries.data(),
                    raw_entries.size() * sizeof(RawPfsEntry));
    }
    if (!strtab.empty()) {
        std::memcpy(out.data() + sizeof(RawPfsHeader) + raw_entries.size() * sizeof(RawPfsEntry),
                    strtab.data(), strtab.size());
    }
    std::size_t write_pos = header_size;
    for (const auto& entry : entries) {
        std::memcpy(out.data() + write_pos, entry.data.data(), entry.data.size());
        write_pos += entry.data.size();
    }
    return out;
}

std::vector<u8> MakePlaintextProgramNca(u64 title_id, const std::vector<u8>& exefs_pfs0,
                                        bool include_exefs) {
    constexpr std::size_t HeaderAreaSize =
        FileSys::NcaHeader::Size + FileSys::NcaFsHeader::Size * FileSys::NcaHeader::FsCountMax;
    static_assert(HeaderAreaSize == 0xC00);
    constexpr std::size_t HashBlockSize = 0x1000;

    FileSys::NcaHeader header{};
    header.magic = FileSys::NcaHeader::Magic3;
    header.distribution_type = FileSys::NcaHeader::DistributionType::Download;
    header.content_type = FileSys::NcaHeader::ContentType::Program;
    header.key_generation = 0;
    header.key_index = 0;
    header.program_id = title_id;
    header.sdk_addon_version = 0x000B0000;
    header.key_generation_2 = 0;

    if (!include_exefs) {
        header.content_size = HeaderAreaSize;
        std::vector<u8> out(HeaderAreaSize, 0);
        std::memcpy(out.data(), &header, sizeof(header));
        return out;
    }

    const std::size_t aligned_data_size = Common::AlignUp<std::size_t>(exefs_pfs0.size(), HashBlockSize);
    const std::size_t num_blocks = aligned_data_size / HashBlockSize;
    const std::size_t layer0_hash_size = num_blocks * 0x20;
    const std::size_t fs_body_size = Common::AlignUp<std::size_t>(
        HashBlockSize + aligned_data_size, FileSys::NcaHeader::SectorSize);

    std::vector<u8> fs_body(fs_body_size, 0);
    std::memcpy(fs_body.data() + HashBlockSize, exefs_pfs0.data(), exefs_pfs0.size());
    for (std::size_t i = 0; i < num_blocks; ++i) {
        const auto block_hash =
            Sha256(fs_body.data() + HashBlockSize + i * HashBlockSize, HashBlockSize);
        std::memcpy(fs_body.data() + i * block_hash.size(), block_hash.data(), block_hash.size());
    }

    FileSys::NcaFsHeader fs_header{};
    fs_header.version = 2;
    fs_header.fs_type = FileSys::NcaFsHeader::FsType::PartitionFs;
    fs_header.hash_type = FileSys::NcaFsHeader::HashType::HierarchicalSha256Hash;
    fs_header.encryption_type = FileSys::NcaFsHeader::EncryptionType::None;
    auto& sha_data = fs_header.hash_data.hierarchical_sha256_data;
    sha_data.fs_data_master_hash.value = Sha256(fs_body.data(), layer0_hash_size);
    sha_data.hash_block_size = static_cast<s32>(HashBlockSize);
    sha_data.hash_layer_count = 2;
    sha_data.hash_layer_region[0].offset = 0;
    sha_data.hash_layer_region[0].size = static_cast<s64>(layer0_hash_size);
    sha_data.hash_layer_region[1].offset = static_cast<s64>(HashBlockSize);
    sha_data.hash_layer_region[1].size = static_cast<s64>(aligned_data_size);

    header.fs_info[0].start_sector = FileSys::NcaHeader::ByteToSector(HeaderAreaSize);
    header.fs_info[0].end_sector = FileSys::NcaHeader::ByteToSector(HeaderAreaSize + fs_body_size);
    header.fs_header_hash[0].value = Sha256(&fs_header, sizeof(fs_header));
    header.content_size = HeaderAreaSize + fs_body_size;

    std::vector<u8> out(HeaderAreaSize + fs_body_size, 0);
    std::memcpy(out.data(), &header, sizeof(header));
    std::memcpy(out.data() + FileSys::NcaHeader::Size, &fs_header, sizeof(fs_header));
    std::memcpy(out.data() + HeaderAreaSize, fs_body.data(), fs_body_size);
    return out;
}

FileSys::VirtualDir MakeExeFsDir(u64 title_id, bool is_switch2, std::string name = "exefs") {
    std::vector<FileSys::VirtualFile> files;
    files.push_back(std::make_shared<FileSys::VectorVfsFile>(MakeMinimalNso(), "main"));
    files.push_back(
        std::make_shared<FileSys::VectorVfsFile>(MakeNpdm(title_id, is_switch2), "main.npdm"));
    return std::make_shared<FileSys::VectorVfsDirectory>(std::move(files),
                                                         std::vector<FileSys::VirtualDir>{},
                                                         std::move(name));
}

FileSys::VirtualFile MakeProgramNcaFile(u64 title_id, bool is_switch2, bool include_exefs,
                                          std::string name) {
    std::vector<u8> pfs0;
    if (include_exefs) {
        pfs0 = MakePfs0({
            {"main", MakeMinimalNso()},
            {"main.npdm", MakeNpdm(title_id, is_switch2)},
        });
    }
    return std::make_shared<FileSys::VectorVfsFile>(
        MakePlaintextProgramNca(title_id, pfs0, include_exefs), std::move(name));
}

void WriteBytes(const std::filesystem::path& path, const std::vector<u8>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE(file.is_open());
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.good());
}

} // namespace

TEST_CASE("ResolvePlatformProfile distinguishes Switch 1, Switch 2, missing metadata, and overrides",
          "[core][platform-profile]") {
    IsolatedProfileTestEnv env{"resolve"};
    Core::System system;
    system.Initialize();

    constexpr u64 BaseTitleId = 0x0100123400000000ULL;

    SECTION("DeconstructedRomDirectory confirms Switch 1 and Switch 2 NPDMs") {
        auto sw1_dir = MakeExeFsDir(BaseTitleId, false);
        Loader::AppLoader_DeconstructedRomDirectory sw1_loader{sw1_dir, false};
        const auto sw1_profile = Core::ResolvePlatformProfile(system, &sw1_loader);
        CHECK(sw1_profile.kind == Core::PlatformKind::Switch1);
        CHECK(sw1_profile.resolution == Core::PlatformResolution::ConfirmedSwitch1);
        CHECK(sw1_profile.IsConfirmed());
        CHECK_FALSE(sw1_profile.IsSwitch2());
        CHECK(sw1_profile.guest_cntfrq == 19'200'000ULL);
        CHECK_FALSE(sw1_profile.force_12gb_memory_layout);
        CHECK_FALSE(sw1_profile.reset_dram_on_shutdown);
        CHECK_FALSE(sw1_profile.allow_contradictory_nx2_gpu_query);

        auto sw2_dir = MakeExeFsDir(BaseTitleId, true);
        Loader::AppLoader_DeconstructedRomDirectory sw2_loader{sw2_dir, false};
        const auto sw2_profile = Core::ResolvePlatformProfile(system, &sw2_loader);
        CHECK(sw2_profile.kind == Core::PlatformKind::Switch2);
        CHECK(sw2_profile.resolution == Core::PlatformResolution::ConfirmedSwitch2);
        CHECK(sw2_profile.IsConfirmed());
        CHECK(sw2_profile.IsSwitch2());
        CHECK(sw2_profile.guest_cntfrq == Common::WallClock::Switch2CNTFRQ);
        CHECK(sw2_profile.force_12gb_memory_layout);
        CHECK(sw2_profile.reset_dram_on_shutdown);
        CHECK_FALSE(sw2_profile.allow_contradictory_nx2_gpu_query);
    }

    SECTION("PatchExeFS update overrides base metadata both ways without stale is_switch2") {
        FileSys::ManualContentProvider manual;
        system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual, &manual);
        SCOPE_EXIT {
            system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual, nullptr);
        };

        // Switch 1 base updated to Switch 2
        constexpr u64 Tid1 = 0x0100123400002000ULL;
        manual.AddEntry(FileSys::TitleType::Update, FileSys::ContentRecordType::Program,
                        FileSys::GetUpdateTitleID(Tid1),
                        MakeProgramNcaFile(FileSys::GetUpdateTitleID(Tid1), true, true, "upd2.nca"));
        Loader::AppLoader_DeconstructedRomDirectory loader1{MakeExeFsDir(Tid1, false), true};
        const auto upgraded = Core::ResolvePlatformProfile(system, &loader1);
        CHECK(upgraded.resolution == Core::PlatformResolution::ConfirmedSwitch2);
        CHECK(upgraded.IsSwitch2());

        // Switch 2 base patched back to Switch 1 (proves Reload() resets is_switch2)
        constexpr u64 Tid2 = 0x0100123400004000ULL;
        manual.AddEntry(FileSys::TitleType::Update, FileSys::ContentRecordType::Program,
                        FileSys::GetUpdateTitleID(Tid2),
                        MakeProgramNcaFile(FileSys::GetUpdateTitleID(Tid2), false, true, "upd1.nca"));
        Loader::AppLoader_DeconstructedRomDirectory loader2{MakeExeFsDir(Tid2, true), true};
        const auto downgraded = Core::ResolvePlatformProfile(system, &loader2);
        CHECK(downgraded.resolution == Core::PlatformResolution::ConfirmedSwitch1);
        CHECK_FALSE(downgraded.IsSwitch2());
    }

    SECTION("Standalone NCA ignores sibling main.npdm and uses its internal ExeFS") {
        constexpr u64 StandaloneTid = 0x0100AAAA00000000ULL;
        const auto folder = env.root / "standalone";
        const auto decoy_npdm_path = folder / "main.npdm";
        const auto nca_path = folder / "program.nca";
        WriteBytes(decoy_npdm_path, MakeNpdm(StandaloneTid, false));

        const auto pfs0 = MakePfs0({
            {"main", MakeMinimalNso()},
            {"main.npdm", MakeNpdm(StandaloneTid, true)},
        });
        WriteBytes(nca_path, MakePlaintextProgramNca(StandaloneTid, pfs0, true));

        auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();
        const auto nca_file = vfs->OpenFile(nca_path.string(), FileSys::OpenMode::Read);
        REQUIRE(nca_file != nullptr);
        auto loader = Loader::GetLoader(system, nca_file, StandaloneTid, 0);
        REQUIRE(loader != nullptr);
        const auto profile = Core::ResolvePlatformProfile(system, loader.get());
        CHECK(profile.resolution == Core::PlatformResolution::ConfirmedSwitch2);
        CHECK(profile.IsSwitch2());
    }

    SECTION("Base NCA without ExeFS resolves metadata from installed update NCA") {
        constexpr u64 BaseNoExeFsTid = 0x0100BBBB00000000ULL;
        FileSys::ManualContentProvider manual;
        system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual, &manual);
        SCOPE_EXIT {
            system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual, nullptr);
        };

        const auto update_tid = FileSys::GetUpdateTitleID(BaseNoExeFsTid);
        manual.AddEntry(FileSys::TitleType::Update, FileSys::ContentRecordType::Program, update_tid,
                        MakeProgramNcaFile(update_tid, true, true, "upd.nca"));

        auto base_nca_file = MakeProgramNcaFile(BaseNoExeFsTid, false, false, "base.nca");
        auto loader = Loader::GetLoader(system, base_nca_file, BaseNoExeFsTid, 0);
        REQUIRE(loader != nullptr);
        const auto profile = Core::ResolvePlatformProfile(system, loader.get());
        CHECK(profile.resolution == Core::PlatformResolution::ConfirmedSwitch2);
        CHECK(profile.IsSwitch2());
    }

    SECTION("Indexed program resolution covers base and update-only Switch 2 NCAs") {
        constexpr u64 MultiBaseTid = 0x0100CCCC00000000ULL;
        constexpr u64 IndexedTid = FileSys::GetBaseTitleIDWithProgramIndex(MultiBaseTid, 1);
        constexpr u64 IndexedUpdateTid = FileSys::GetUpdateTitleID(IndexedTid);

        auto dummy_file = MakeProgramNcaFile(MultiBaseTid, false, true, "base0.nca");

        {
            FileSys::ManualContentProvider manual;
            system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual,
                                           &manual);
            SCOPE_EXIT {
                system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual,
                                               nullptr);
            };
            manual.AddEntry(FileSys::TitleType::Application, FileSys::ContentRecordType::Program,
                            IndexedTid, MakeProgramNcaFile(IndexedTid, true, true, "idx1.nca"));
            auto loader = Loader::GetLoader(system, dummy_file, MultiBaseTid, 1);
            REQUIRE(loader != nullptr);
            const auto profile = Core::ResolvePlatformProfile(system, loader.get());
            CHECK(profile.resolution == Core::PlatformResolution::ConfirmedSwitch2);
        }

        {
            FileSys::ManualContentProvider manual;
            system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual,
                                           &manual);
            SCOPE_EXIT {
                system.RegisterContentProvider(FileSys::ContentProviderUnionSlot::FrontendManual,
                                               nullptr);
            };
            manual.AddEntry(
                FileSys::TitleType::Update, FileSys::ContentRecordType::Program, IndexedUpdateTid,
                MakeProgramNcaFile(IndexedUpdateTid, true, true, "idx1_upd.nca"));
            auto loader = Loader::GetLoader(system, dummy_file, MultiBaseTid, 1);
            REQUIRE(loader != nullptr);
            const auto profile = Core::ResolvePlatformProfile(system, loader.get());
            CHECK(profile.resolution == Core::PlatformResolution::ConfirmedSwitch2);
        }
    }

    SECTION("Missing metadata and homebrew formats fall back to DefaultSwitch1") {
        const auto null_profile = Core::ResolvePlatformProfile(system, nullptr);
        CHECK(null_profile.resolution == Core::PlatformResolution::DefaultSwitch1);
        CHECK(null_profile.allow_contradictory_nx2_gpu_query);
        CHECK_FALSE(null_profile.IsSwitch2());

        auto nro_file = std::make_shared<FileSys::VectorVfsFile>(MakeMinimalNro(), "brew.nro");
        Loader::AppLoader_NRO nro_loader{std::move(nro_file)};
        const auto nro_profile = Core::ResolvePlatformProfile(system, &nro_loader);
        CHECK(nro_profile.resolution == Core::PlatformResolution::DefaultSwitch1);
        CHECK(nro_profile.allow_contradictory_nx2_gpu_query);

        auto nso_file = std::make_shared<FileSys::VectorVfsFile>(MakeMinimalNso(), "main.nso");
        Loader::AppLoader_NSO nso_loader{std::move(nso_file)};
        const auto nso_profile = Core::ResolvePlatformProfile(system, &nso_loader);
        CHECK(nso_profile.resolution == Core::PlatformResolution::DefaultSwitch1);
        CHECK(nso_profile.allow_contradictory_nx2_gpu_query);
    }

    SECTION("ParsePlatformOverride and override precedence") {
        CHECK_FALSE(Core::ParsePlatformOverride(nullptr).has_value());
        CHECK_FALSE(Core::ParsePlatformOverride("").has_value());
        CHECK_FALSE(Core::ParsePlatformOverride("0").has_value());
        CHECK_FALSE(Core::ParsePlatformOverride("3").has_value());
        CHECK(Core::ParsePlatformOverride("1") == Core::PlatformResolution::OverrideSwitch1);
        CHECK(Core::ParsePlatformOverride("2") == Core::PlatformResolution::OverrideSwitch2);

        Loader::AppLoader_DeconstructedRomDirectory sw2_loader{MakeExeFsDir(BaseTitleId, true),
                                                               false};
        const auto forced_sw1 = Core::ResolvePlatformProfile(
            system, &sw2_loader, Core::ParsePlatformOverride("1"));
        CHECK(forced_sw1.resolution == Core::PlatformResolution::OverrideSwitch1);
        CHECK_FALSE(forced_sw1.IsSwitch2());
        CHECK_FALSE(forced_sw1.allow_contradictory_nx2_gpu_query);

        Loader::AppLoader_DeconstructedRomDirectory sw1_loader{MakeExeFsDir(BaseTitleId, false),
                                                               false};
        const auto forced_sw2 = Core::ResolvePlatformProfile(
            system, &sw1_loader, Core::ParsePlatformOverride("2"));
        CHECK(forced_sw2.resolution == Core::PlatformResolution::OverrideSwitch2);
        CHECK(forced_sw2.IsSwitch2());
        CHECK(forced_sw2.force_12gb_memory_layout);
    }
}

TEST_CASE("nvhost_ctrl_gpu GetCharacteristics3 enforces PlatformProfile on 0xD0 queries",
          "[core][platform-profile]") {
    IsolatedProfileTestEnv env{"nvhost"};

    Service::Nvidia::Ioctl ioctl{};
    ioctl.cmd.Assign(0x5);
    ioctl.group.Assign(static_cast<u32>('G'));
    ioctl.length.Assign(0x10);
    ioctl.is_in.Assign(1);
    ioctl.is_out.Assign(1);

    auto run_query = [&](Core::PlatformResolution resolution, u64 requested_size,
                         std::size_t inline_size) {
        Tegra::g_nx2_ampere.store(false, std::memory_order_relaxed);
        Service::Nvidia::Devices::g_nx2_driver.store(false, std::memory_order_relaxed);
        Core::System system;
        system.SetPlatformProfile(Core::MakePlatformProfile(resolution));
        system.Initialize();
        system.Kernel().Initialize();
        SCOPE_EXIT {
            system.ShutdownMainProcess();
        };

        auto host1x = std::make_unique<Tegra::Host1x::Host1x>(system);
        Service::Nvidia::Module nvdrv{system, *host1x};

        const auto fd = nvdrv.Open("/dev/nvhost-ctrl-gpu", Service::Nvidia::NvCore::SessionId{1});
        REQUIRE(fd != Service::Nvidia::INVALID_NVDRV_FD);
        SCOPE_EXIT {
            CHECK(nvdrv.Close(fd) == Service::Nvidia::NvResult::Success);
        };

        std::vector<u8> input(0xB0, 0);
        std::vector<u8> output(0xB0, 0);
        std::vector<u8> inline_output(inline_size, 0);
        WriteLe<u64>(input, 0x00, requested_size);

        const auto result = nvdrv.Ioctl3(fd, ioctl, input, output, inline_output);
        return std::tuple{
            result,
            output,
            inline_output,
            Tegra::g_nx2_ampere.load(std::memory_order_relaxed),
            Service::Nvidia::Devices::g_nx2_driver.load(std::memory_order_relaxed),
        };
    };

    for (const auto rejected_res : {Core::PlatformResolution::ConfirmedSwitch1,
                                    Core::PlatformResolution::OverrideSwitch1}) {
        const auto [result, output, inline_output, ampere, driver] =
            run_query(rejected_res, 0xD0, 0xD0);
        CHECK(result == Service::Nvidia::NvResult::NotSupported);
        CHECK_FALSE(ampere);
        CHECK_FALSE(driver);
        CHECK(std::all_of(inline_output.begin(), inline_output.end(),
                          [](u8 byte) { return byte == 0; }));
        CHECK(ReadLe<u64>(output, 0x00) == 0xD0);
        CHECK(ReadLe<u32>(output, 0x10) == 0);
    }

    for (const auto allowed_nx2_res : {Core::PlatformResolution::DefaultSwitch1,
                                       Core::PlatformResolution::ConfirmedSwitch2}) {
        const auto [result, output, inline_output, ampere, driver] =
            run_query(allowed_nx2_res, 0xD0, 0xD0);
        CHECK(result == Service::Nvidia::NvResult::Success);
        CHECK(ampere);
        CHECK(driver);
        CHECK(ReadLe<u64>(output, 0x00) == 0xD0);
        CHECK(ReadLe<u32>(inline_output, 0x00) == 0x170);
        CHECK(ReadLe<u32>(inline_output, 0x04) == 0xE);
        CHECK(ReadLe<u32>(inline_output, 0xB4) == 64);
    }

    {
        const auto [result, output, inline_output, ampere, driver] =
            run_query(Core::PlatformResolution::ConfirmedSwitch1, 0xA0, 0xA0);
        CHECK(result == Service::Nvidia::NvResult::Success);
        CHECK_FALSE(ampere);
        CHECK_FALSE(driver);
        CHECK(ReadLe<u64>(output, 0x00) == 0xA0);
        CHECK(ReadLe<u32>(inline_output, 0x00) == 0x120);
        CHECK(ReadLe<u32>(inline_output, 0x04) == 0xB);
    }
}
