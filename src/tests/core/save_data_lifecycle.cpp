// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/alignment.h"
#include "common/common_funcs.h"
#include "common/div_ceil.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/file_sys/errors.h"
#include "core/file_sys/program_metadata.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/savedata_factory.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/ipc.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_scoped_resource_reservation.h"
#include "core/hle/kernel/k_session.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/svc_types.h"
#include "core/hle/result.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/hle/service/filesystem/fsp/fs_i_save_data_info_reader.h"
#include "core/hle/service/filesystem/fsp/fsp_srv.h"
#include "core/hle/service/filesystem/save_data_controller.h"
#include "core/hle/service/hle_ipc.h"
#include "core/hle/service/os/process.h"
#include "core/hle/service/server_manager.h"
#include "core/loader/loader.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"

namespace {

using namespace FileSys;

constexpr u64 TitleId = 0x0100000000001000;
constexpr u64 OtherTitleId = 0x0100000000002000;
constexpr u64 CacheTitleId = 0x0000000000000002;
constexpr u64 TestSystemSaveDataId = 0x0000000000001234;
constexpr u64 UnknownSaveDataId = 0x00000000DEADBEEF;

/// The user id as it is stored on disk: the high half of the id first, both halves big endian.
constexpr UserId TestUserId{0xFEDCBA9876543210, 0x0123456789ABCDEF};

constexpr std::string_view TestUserIdName = "0123456789ABCDEFFEDCBA9876543210";

/// A save data factory on top of a real, temporary directory.
class SaveDataTestContext {
public:
    SaveDataTestContext() {
        static std::atomic<u64> counter{0};
        const auto unique = std::to_string(counter.fetch_add(1));
        directory = std::filesystem::temp_directory_path() /
                    ("save-data-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     "-" + unique);
        std::filesystem::create_directories(directory);

        vfs = std::make_shared<RealVfsFilesystem>();
        root = vfs->OpenDirectory(Common::FS::PathToUTF8String(directory), OpenMode::ReadWrite);
        REQUIRE(root != nullptr);

        // The spaces of the factory live in the root of the save data directory.
        REQUIRE(root->CreateSubdirectory("user") != nullptr);
        REQUIRE(root->CreateSubdirectory("system") != nullptr);
        REQUIRE(root->CreateSubdirectory("temp") != nullptr);

        factory = std::make_unique<SaveDataFactory>(system, TitleId, root);
    }

    ~SaveDataTestContext() {
        factory.reset();
        root.reset();
        vfs.reset();
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    SaveDataTestContext(const SaveDataTestContext&) = delete;
    SaveDataTestContext& operator=(const SaveDataTestContext&) = delete;

    /// Creates a save data and fails the test when it cannot be created.
    void Create(SaveDataSpaceId space, const SaveDataAttribute& attribute) {
        REQUIRE(factory->Create(space, attribute) != nullptr);
    }

    /// Returns the save data of the given type, or nothing when the space holds none.
    [[nodiscard]] std::optional<SaveDataEntry> Find(SaveDataSpaceId space,
                                                    SaveDataType type) const {
        const auto entries = factory->Enumerate(space);
        for (const auto& entry : entries) {
            if (entry.type == type) {
                return entry;
            }
        }
        return std::nullopt;
    }

    /// Writes a file of the given size into a save data.
    void WriteFile(SaveDataSpaceId space, const SaveDataAttribute& attribute,
                   std::string_view name, std::size_t size) {
        const auto save_dir = factory->Open(space, attribute);
        REQUIRE(save_dir != nullptr);
        const auto file = save_dir->CreateFile(name);
        REQUIRE(file != nullptr);
        REQUIRE(file->Resize(size));
    }

    Core::System system;
    std::filesystem::path directory;
    VirtualFilesystem vfs;
    VirtualDir root;
    std::unique_ptr<SaveDataFactory> factory;
};

constexpr SaveDataAttribute AccountAttribute =
    SaveDataAttribute::Make(TitleId, SaveDataType::Account, TestUserId, 0);
constexpr SaveDataAttribute CacheAttribute =
    SaveDataAttribute::Make(CacheTitleId, SaveDataType::Cache, TestUserId, 0);
constexpr SaveDataAttribute SystemAttribute =
    SaveDataAttribute::Make(0, SaveDataType::System, TestUserId, TestSystemSaveDataId);
constexpr SaveDataAttribute TemporaryAttribute =
    SaveDataAttribute::Make(TitleId, SaveDataType::Temporary, TestUserId, 0);

class SaveProcessTestLoader final : public Loader::AppLoader {
public:
    SaveProcessTestLoader() : AppLoader(nullptr) {}

    Loader::FileType GetFileType() const override {
        return Loader::FileType::Unknown;
    }

    LoadResult Load(Kernel::KProcess& process, Core::System& system) override {
        const auto metadata = FileSys::ProgramMetadata::GetDefault();
        REQUIRE(process.LoadFromMetadata(system.Kernel(), metadata, Kernel::PageSize, {}, 0)
                    .IsSuccess());
        REQUIRE(process.IsInitialized());
        return {Loader::ResultStatus::Success, LoadParameters{0x2c, 0x100000}};
    }
};

class ScopedSaveDirectories {
public:
    ScopedSaveDirectories() {
        static std::atomic<u64> counter{0};
        directory = std::filesystem::temp_directory_path() /
                    ("fsp-srv-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     "-" + std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(directory / "nand");
        std::filesystem::create_directories(directory / "sdmc");
        std::filesystem::create_directories(directory / "load");
        std::filesystem::create_directories(directory / "dump");

        Common::FS::SetEdenPath(Common::FS::EdenPath::NANDDir, directory / "nand");
        Common::FS::SetEdenPath(Common::FS::EdenPath::SaveDir, directory / "nand");
        Common::FS::SetEdenPath(Common::FS::EdenPath::SDMCDir, directory / "sdmc");
        Common::FS::SetEdenPath(Common::FS::EdenPath::LoadDir, directory / "load");
        Common::FS::SetEdenPath(Common::FS::EdenPath::DumpDir, directory / "dump");
    }

    ~ScopedSaveDirectories() {
        Common::FS::SetEdenPath(Common::FS::EdenPath::NANDDir, previous_nand);
        Common::FS::SetEdenPath(Common::FS::EdenPath::SaveDir, previous_save);
        Common::FS::SetEdenPath(Common::FS::EdenPath::SDMCDir, previous_sdmc);
        Common::FS::SetEdenPath(Common::FS::EdenPath::LoadDir, previous_load);
        Common::FS::SetEdenPath(Common::FS::EdenPath::DumpDir, previous_dump);
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    ScopedSaveDirectories(const ScopedSaveDirectories&) = delete;
    ScopedSaveDirectories& operator=(const ScopedSaveDirectories&) = delete;

private:
    const std::filesystem::path previous_nand =
        Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir);
    const std::filesystem::path previous_save =
        Common::FS::GetEdenPath(Common::FS::EdenPath::SaveDir);
    const std::filesystem::path previous_sdmc =
        Common::FS::GetEdenPath(Common::FS::EdenPath::SDMCDir);
    const std::filesystem::path previous_load =
        Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir);
    const std::filesystem::path previous_dump =
        Common::FS::GetEdenPath(Common::FS::EdenPath::DumpDir);
    std::filesystem::path directory;
};

struct DomainIpcResult {
    Result result{ResultUnknown};
    u64 out_word{0};
};

DomainIpcResult SendDomainCommand(
    Core::System& system, Core::Memory::Memory& memory,
    const std::shared_ptr<Service::SessionRequestManager>& session_manager,
    Kernel::KServerSession* server_session, Kernel::KThread* guest_thread, u32 object_id,
    u32 cmd_id, std::span<const u8> raw_payload, bool send_pid = false, VAddr out_buf_addr = 0,
    u64 out_buf_size = 0) {
    std::array<u32, IPC::COMMAND_BUFFER_LENGTH> cmd_buf{};
    std::size_t idx = 0;

    const u32 data_words =
        static_cast<u32>(sizeof(IPC::DomainMessageHeader) / sizeof(u32) +
                         sizeof(IPC::DataPayloadHeader) / sizeof(u32) + 2 +
                         Common::DivCeil(raw_payload.size(), sizeof(u32)) + 4);

    IPC::CommandHeader header{};
    header.type.Assign(IPC::CommandType::Request);
    header.num_buf_b_descriptors.Assign(out_buf_size > 0 ? 1u : 0u);
    header.data_size.Assign(data_words);
    header.enable_handle_descriptor.Assign(send_pid ? 1u : 0u);
    std::memcpy(&cmd_buf[idx], &header, sizeof(header));
    idx += sizeof(header) / sizeof(u32);

    if (send_pid) {
        IPC::HandleDescriptorHeader handle_header{};
        handle_header.send_current_pid.Assign(1u);
        std::memcpy(&cmd_buf[idx], &handle_header, sizeof(handle_header));
        idx += 1 + 2; // 1 word descriptor + 2 words PID placeholder
    }

    if (out_buf_size > 0) {
        IPC::BufferDescriptorABW desc{};
        desc.size_bits_0_31 = static_cast<u32>(out_buf_size & 0xFFFFFFFFu);
        desc.size_bits_32_35.Assign(static_cast<u32>((out_buf_size >> 32) & 0xFu));
        desc.address_bits_0_31 = static_cast<u32>(out_buf_addr & 0xFFFFFFFFu);
        desc.address_bits_32_35.Assign(static_cast<u32>((out_buf_addr >> 32) & 0xFu));
        desc.address_bits_36_38.Assign(static_cast<u32>((out_buf_addr >> 36) & 0x7u));
        std::memcpy(&cmd_buf[idx], &desc, sizeof(desc));
        idx += sizeof(desc) / sizeof(u32);
    }

    if ((idx & 3u) != 0) {
        idx += 4u - (idx & 3u);
    }

    IPC::DomainMessageHeader domain_header{};
    domain_header.command.Assign(IPC::DomainMessageHeader::CommandType::SendMessage);
    domain_header.object_id = object_id;
    std::memcpy(&cmd_buf[idx], &domain_header, sizeof(domain_header));
    idx += sizeof(domain_header) / sizeof(u32);

    IPC::DataPayloadHeader payload_header{};
    payload_header.magic = Common::MakeMagic('S', 'F', 'C', 'I');
    std::memcpy(&cmd_buf[idx], &payload_header, sizeof(payload_header));
    idx += sizeof(payload_header) / sizeof(u32);

    cmd_buf[idx++] = cmd_id;
    cmd_buf[idx++] = 0;

    if (!raw_payload.empty()) {
        std::memcpy(&cmd_buf[idx], raw_payload.data(), raw_payload.size());
    }

    Service::HLERequestContext ctx{system.Kernel(), memory, server_session, guest_thread};
    ctx.SetSessionRequestManager(session_manager);
    REQUIRE(ctx.PopulateFromIncomingCommandBuffer(reinterpret_cast<u32_le*>(cmd_buf.data())) ==
            ResultSuccess);
    REQUIRE(session_manager->CompleteSyncRequest(server_session, ctx) == ResultSuccess);
    REQUIRE(ctx.WriteToOutgoingCommandBuffer() == ResultSuccess);

    DomainIpcResult out{};
    const u32* resp = ctx.CommandBuffer();
    for (std::size_t i = 0; i < 16; ++i) {
        if (resp[i] == Common::MakeMagic('S', 'F', 'C', 'O')) {
            out.result = Result{resp[i + 2]};
            if (i + 5 < IPC::COMMAND_BUFFER_LENGTH) {
                std::memcpy(&out.out_word, &resp[i + 4], sizeof(u64));
            }
            break;
        }
    }
    return out;
}

} // namespace

TEST_CASE("A save data space without saves enumerates nothing", "[core][save-data]") {
    const ScopedSystemTestContext context;
    SaveDataTestContext save_data;

    CHECK(save_data.factory->Enumerate(SaveDataSpaceId::User).empty());
    CHECK(save_data.factory->Enumerate(SaveDataSpaceId::System).empty());
    CHECK(save_data.factory->Enumerate(SaveDataSpaceId::Temporary).empty());
}

TEST_CASE("Enumerating a space reports the saves it holds", "[core][save-data]") {
    const ScopedSystemTestContext context;
    SaveDataTestContext save_data;

    save_data.Create(SaveDataSpaceId::System, SystemAttribute);
    save_data.Create(SaveDataSpaceId::User, AccountAttribute);
    save_data.Create(SaveDataSpaceId::User, CacheAttribute);

    const auto system_saves = save_data.factory->Enumerate(SaveDataSpaceId::System);
    REQUIRE(system_saves.size() == 1);
    CHECK(system_saves[0].type == SaveDataType::System);
    CHECK(system_saves[0].rank == SaveDataRank::Primary);
    CHECK(system_saves[0].system_save_data_id == TestSystemSaveDataId);
    CHECK(system_saves[0].title_id == 0);
    CHECK(system_saves[0].user_id == TestUserId);
    CHECK(system_saves[0].index == 0);
    CHECK(system_saves[0].relative_path ==
          "save/0000000000001234/" + std::string(TestUserIdName));

    const auto user_saves = save_data.factory->Enumerate(SaveDataSpaceId::User);
    REQUIRE(user_saves.size() == 2);

    const auto account_save = save_data.Find(SaveDataSpaceId::User, SaveDataType::Account);
    REQUIRE(account_save.has_value());
    CHECK(account_save->title_id == TitleId);
    CHECK(account_save->system_save_data_id == 0);
    CHECK(account_save->user_id == TestUserId);
    CHECK(account_save->relative_path == "save/0000000000000000/" + std::string(TestUserIdName) +
                                            "/0100000000001000");

    const auto cache_save = save_data.Find(SaveDataSpaceId::User, SaveDataType::Cache);
    REQUIRE(cache_save.has_value());
    CHECK(cache_save->title_id == CacheTitleId);
    CHECK(cache_save->system_save_data_id == 0);
    CHECK(cache_save->relative_path == "save/cache/0000000000000002");
}

TEST_CASE("Deleting a save data removes it and reports a second attempt", "[core][save-data]") {
    const ScopedSystemTestContext context;
    SaveDataTestContext save_data;

    save_data.Create(SaveDataSpaceId::System, SystemAttribute);
    REQUIRE(save_data.factory->Enumerate(SaveDataSpaceId::System).size() == 1);

    REQUIRE(save_data.factory->Delete(SaveDataSpaceId::System, SystemAttribute) == ResultSuccess);
    CHECK(save_data.factory->Enumerate(SaveDataSpaceId::System).empty());
    CHECK_FALSE(std::filesystem::exists(save_data.directory / "system" / "save" /
                                        "0000000000001234" / std::string(TestUserIdName)));

    // Deleting a save data that is gone reports that there was no target.
    CHECK(save_data.factory->Delete(SaveDataSpaceId::System, SystemAttribute) ==
          FileSys::ResultTargetNotFound);
}

TEST_CASE("Deleting the save data that match a filter keeps the other ones", "[core][save-data]") {
    const ScopedSystemTestContext context;
    SaveDataTestContext save_data;

    save_data.Create(SaveDataSpaceId::User, AccountAttribute);
    save_data.Create(SaveDataSpaceId::User, CacheAttribute);

    SaveDataFilter filter{};
    filter.use_save_data_type = true;
    filter.attribute.type = SaveDataType::Account;

    u64 deleted = 0;
    REQUIRE(save_data.factory->DeleteMatching(SaveDataSpaceId::User, filter, &deleted) ==
            ResultSuccess);
    CHECK(deleted == 1);

    const auto remaining = save_data.factory->Enumerate(SaveDataSpaceId::User);
    REQUIRE(remaining.size() == 1);
    CHECK(remaining[0].type == SaveDataType::Cache);

    // A filter that matches nothing reports that there was no target.
    SaveDataFilter missing{};
    missing.use_save_data_type = true;
    missing.attribute.type = SaveDataType::Device;

    u64 not_deleted = 42;
    CHECK(save_data.factory->DeleteMatching(SaveDataSpaceId::User, missing, &not_deleted) ==
          FileSys::ResultTargetNotFound);
    CHECK(not_deleted == 0);
}

TEST_CASE("Deleting a cache storage removes it", "[core][save-data]") {
    const ScopedSystemTestContext context;
    SaveDataTestContext save_data;

    save_data.Create(SaveDataSpaceId::User, CacheAttribute);
    REQUIRE(save_data.factory->Enumerate(SaveDataSpaceId::User).size() == 1);

    REQUIRE(save_data.factory->DeleteCacheStorage(static_cast<u16>(CacheTitleId)) == ResultSuccess);
    CHECK(save_data.factory->Enumerate(SaveDataSpaceId::User).empty());
    CHECK(save_data.factory->DeleteCacheStorage(static_cast<u16>(CacheTitleId)) ==
          FileSys::ResultTargetNotFound);
}

TEST_CASE("The size of a space grows with the save data it holds", "[core][save-data]") {
    const ScopedSystemTestContext context;
    SaveDataTestContext save_data;

    save_data.Create(SaveDataSpaceId::User, AccountAttribute);

    const auto free_space = save_data.factory->GetFreeSpace(SaveDataSpaceId::User);
    const auto total_size = save_data.factory->QueryTotalSize(SaveDataSpaceId::User);
    CHECK(free_space >= 0);
    CHECK(total_size >= 0);

    save_data.WriteFile(SaveDataSpaceId::User, AccountAttribute, "data.bin", 0x1000);

    CHECK(save_data.factory->QueryTotalSize(SaveDataSpaceId::User) > total_size);
    CHECK(save_data.factory->GetFreeSpace(SaveDataSpaceId::User) <= free_space);
}

TEST_CASE("The commit id of a save data increases when it is committed", "[core][save-data]") {
    const ScopedSystemTestContext context;
    SaveDataTestContext save_data;

    save_data.Create(SaveDataSpaceId::User, AccountAttribute);

    // A save data that does not exist has no commit id.
    CHECK(save_data.factory->GetCommitId(SaveDataSpaceId::User, UnknownSaveDataId) == 0);

    // A save data that was never committed has no commit id.
    CHECK(save_data.factory->GetCommitId(SaveDataSpaceId::User, TitleId) == 0);

    save_data.WriteFile(SaveDataSpaceId::User, AccountAttribute, "data.bin", 0x100);

    const auto commit_id = save_data.factory->GetCommitId(SaveDataSpaceId::User, TitleId);
    CHECK(commit_id > 0);

    // The commit id stays the same while the save data does not change.
    CHECK(save_data.factory->GetCommitId(SaveDataSpaceId::User, TitleId) == commit_id);
}

TEST_CASE("A save data has no meta file until the guest writes one", "[core][save-data]") {
    const ScopedSystemTestContext context;
    SaveDataTestContext save_data;

    save_data.Create(SaveDataSpaceId::User, AccountAttribute);

    CHECK(save_data.factory->OpenMetaFile(SaveDataSpaceId::User, AccountAttribute,
                                           SaveDataMetaType::Thumbnail) == nullptr);

    save_data.WriteFile(SaveDataSpaceId::User, AccountAttribute, "meta", 0x10);

    const auto meta_file = save_data.factory->OpenMetaFile(
        SaveDataSpaceId::User, AccountAttribute, SaveDataMetaType::Thumbnail);
    REQUIRE(meta_file != nullptr);
    CHECK(meta_file->GetName() == "meta");
}

TEST_CASE("Temporary saves enumerate with relative paths and delete cleanly",
          "[core][save-data]") {
    const ScopedSystemTestContext context;
    SaveDataTestContext save_data;

    save_data.Create(SaveDataSpaceId::Temporary, TemporaryAttribute);
    save_data.WriteFile(SaveDataSpaceId::Temporary, TemporaryAttribute, "temp.bin", 0x80);

    const auto entries = save_data.factory->Enumerate(SaveDataSpaceId::Temporary);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].space == SaveDataSpaceId::Temporary);
    CHECK(entries[0].type == SaveDataType::Temporary);
    CHECK(entries[0].rank == SaveDataRank::Primary);
    CHECK(entries[0].system_save_data_id == 0);
    CHECK(entries[0].title_id == TitleId);
    CHECK(entries[0].user_id == TestUserId);
    CHECK(entries[0].relative_path ==
          "0000000000000000/" + std::string(TestUserIdName) + "/0100000000001000");

    CHECK(save_data.factory->QueryTotalSize(SaveDataSpaceId::Temporary) > 0);
    CHECK(save_data.factory->GetCommitId(SaveDataSpaceId::Temporary, TitleId) > 0);

    REQUIRE(save_data.factory->Delete(SaveDataSpaceId::Temporary, TemporaryAttribute) ==
            ResultSuccess);
    CHECK(save_data.factory->Enumerate(SaveDataSpaceId::Temporary).empty());
    CHECK(save_data.factory->Delete(SaveDataSpaceId::Temporary, TemporaryAttribute) ==
          FileSys::ResultTargetNotFound);
}

TEST_CASE("Cache storages support 64-bit title ids and indices", "[core][save-data]") {
    const ScopedSystemTestContext context;
    SaveDataTestContext save_data;

    const auto default_cache =
        SaveDataAttribute::Make(TitleId, SaveDataType::Cache, InvalidUserId, 0, 0);
    const auto indexed_cache =
        SaveDataAttribute::Make(TitleId, SaveDataType::Cache, InvalidUserId, 0, 2);
    const auto other_title_cache =
        SaveDataAttribute::Make(OtherTitleId, SaveDataType::Cache, InvalidUserId, 0, 0);

    save_data.Create(SaveDataSpaceId::User, default_cache);
    save_data.Create(SaveDataSpaceId::User, indexed_cache);
    save_data.Create(SaveDataSpaceId::User, other_title_cache);

    const auto entries = save_data.factory->Enumerate(SaveDataSpaceId::User);
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].type == SaveDataType::Cache);
    CHECK(entries[0].title_id == TitleId);
    CHECK(entries[0].index == 0);
    CHECK(entries[0].relative_path == "save/cache/0100000000001000");

    CHECK(entries[1].type == SaveDataType::Cache);
    CHECK(entries[1].title_id == TitleId);
    CHECK(entries[1].index == 2);
    CHECK(entries[1].relative_path == "save/cache/0100000000001000_0002");

    CHECK(entries[2].type == SaveDataType::Cache);
    CHECK(entries[2].title_id == OtherTitleId);
    CHECK(entries[2].index == 0);
    CHECK(entries[2].relative_path == "save/cache/0100000000002000");

    // DeleteCacheStorage removes only the current title's indexed cache storage.
    REQUIRE(save_data.factory->DeleteCacheStorage(0) == ResultSuccess);
    CHECK(save_data.factory->DeleteCacheStorage(0) == FileSys::ResultTargetNotFound);

    REQUIRE(save_data.factory->DeleteCacheStorage(2) == ResultSuccess);
    const auto remaining = save_data.factory->Enumerate(SaveDataSpaceId::User);
    REQUIRE(remaining.size() == 1);
    CHECK(remaining[0].title_id == OtherTitleId);

    REQUIRE(save_data.factory->Delete(SaveDataSpaceId::User, other_title_cache) == ResultSuccess);
    CHECK(save_data.factory->Enumerate(SaveDataSpaceId::User).empty());
}

TEST_CASE("FSP_SRV cmd 68 uses space_id before filter and cmd 28 does not wildcard program_id 0",
          "[core][save-data]") {
    const ScopedSystemTestContext context;
    const ScopedSaveDirectories save_dirs;

    Core::System system;
    system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system.Initialize();
    system.Kernel().Initialize();
    system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
    SCOPE_EXIT {
        system.ShutdownMainProcess();
    };

    auto server_manager = std::make_unique<Service::ServerManager>(system);
    auto* server_manager_ptr = server_manager.get();
    auto server_thread = system.Kernel().RunOnHostCoreProcess(
        "fsp-srv-server", [&] { server_manager_ptr->LoopProcess(); });
    SCOPE_EXIT {
        server_manager.reset();
        server_thread.join();
    };

    auto worker = system.Kernel().RunOnHostCoreProcess("fsp-srv-save-test", [&] {
        Service::Process process{system};
        SaveProcessTestLoader loader;
        Loader::ResultStatus load_status = Loader::ResultStatus::Success;
        REQUIRE(process.Initialize(loader, load_status));
        REQUIRE(load_status == Loader::ResultStatus::Success);
        SCOPE_EXIT {
            process.Finalize();
        };

        auto* guest_process = process.GetHandle();
        auto& memory = guest_process->GetMemory();
        memory.SetCurrentPageTable(*guest_process);
        system.Kernel().SetApplicationProcess(guest_process);
        SCOPE_EXIT {
            system.Kernel().SetApplicationProcess(nullptr);
        };

        constexpr size_t StackPages = 16;
        Kernel::KScopedResourceReservation mem_res(
            system.Kernel(), guest_process, Kernel::Svc::LimitableResource::PhysicalMemoryMax,
            StackPages * Kernel::PageSize);
        REQUIRE(mem_res.Succeeded());
        Kernel::KProcessAddress stack_bottom{};
        REQUIRE(guest_process->GetPageTable().MapPages(
                    &stack_bottom, StackPages, Kernel::KMemoryState::Stack,
                    Kernel::KMemoryPermission::UserReadWrite) == ResultSuccess);
        mem_res.Commit();

        Kernel::KScopedResourceReservation thread_res(
            system.Kernel(), guest_process, Kernel::Svc::LimitableResource::ThreadCountMax);
        REQUIRE(thread_res.Succeeded());
        auto* guest_thread = Kernel::KThread::Create(system.Kernel());
        REQUIRE(guest_thread != nullptr);
        REQUIRE(Kernel::KThread::InitializeUserThread(
                    system, guest_thread, Kernel::KProcessAddress{}, 0,
                    stack_bottom + StackPages * Kernel::PageSize, 0x2c, 0,
                    guest_process) == ResultSuccess);
        Kernel::KThread::Register(system.Kernel(), guest_thread);
        thread_res.Commit();
        SCOPE_EXIT {
            guest_thread->Close(system.Kernel());
        };

        auto* session = Kernel::KSession::Create(system.Kernel());
        REQUIRE(session != nullptr);
        session->Initialize(system.Kernel(), nullptr, 0);
        Kernel::KSession::Register(system.Kernel(), session);
        SCOPE_EXIT {
            session->GetClientSession().Close(system.Kernel());
            session->GetServerSession().Close(system.Kernel());
        };
        auto* server_session = &session->GetServerSession();

        auto& fsc = system.GetFileSystemController();
        REQUIRE(fsc.RegisterProcess(process.GetProcessId(), TitleId, nullptr) == ResultSuccess);

        auto fsp_srv = std::make_shared<Service::FileSystem::FSP_SRV>(system);
        auto session_manager = std::make_shared<Service::SessionRequestManager>(
            system.Kernel(), *server_manager_ptr);
        session_manager->SetSessionHandler(fsp_srv);
        session_manager->ConvertToDomain();

        // cmd 1: SetCurrentProcess
        const auto set_pid_res = SendDomainCommand(system, memory, session_manager, server_session,
                                                   guest_thread, 1, 1, {}, true);
        REQUIRE(set_pid_res.result == ResultSuccess);

        // Create saves for two different titles and a temporary save for the current title.
        auto controller = fsc.OpenSaveDataController();
        const auto other_account =
            SaveDataAttribute::Make(OtherTitleId, SaveDataType::Account, TestUserId, 0);
        FileSys::VirtualDir dir{};
        REQUIRE(controller->CreateSaveData(&dir, SaveDataSpaceId::User, AccountAttribute) ==
                ResultSuccess);
        REQUIRE(controller->CreateSaveData(&dir, SaveDataSpaceId::User, other_account) ==
                ResultSuccess);
        REQUIRE(controller->CreateSaveData(&dir, SaveDataSpaceId::Temporary, TemporaryAttribute) ==
                ResultSuccess);
        REQUIRE(dir != nullptr);
        const auto temp_file = dir->CreateFile("payload.bin");
        REQUIRE(temp_file != nullptr);
        REQUIRE(temp_file->Resize(0x20));

        // cmd 68: OpenSaveDataInfoReaderBySaveDataFilter(space_id, filter)
        constexpr std::size_t FilterOffset =
            Common::AlignUp(sizeof(SaveDataSpaceId), alignof(SaveDataFilter));
        static_assert(FilterOffset == 8);

        const auto open_reader = [&](SaveDataSpaceId space_id, const SaveDataFilter& filter) {
            std::array<u8, FilterOffset + sizeof(SaveDataFilter)> payload{};
            std::memcpy(payload.data(), &space_id, sizeof(space_id));
            std::memcpy(payload.data() + FilterOffset, &filter, sizeof(filter));
            return SendDomainCommand(system, memory, session_manager, server_session, guest_thread,
                                     1, 68, payload);
        };

        SaveDataFilter user_filter{};
        user_filter.use_program_id = true;
        user_filter.attribute.program_id = OtherTitleId;

        // Invalid space_id must be rejected.
        CHECK(open_reader(SaveDataSpaceId::ProperSystem, user_filter).result ==
              FileSys::ResultInvalidArgument);

        // User space + filter on OtherTitleId returns only OtherTitleId's save.
        REQUIRE(open_reader(SaveDataSpaceId::User, user_filter).result == ResultSuccess);
        const u32 user_reader_id = static_cast<u32>(session_manager->DomainHandlerCount());
        REQUIRE(user_reader_id >= 2);

        const VAddr info_buf_addr = stack_bottom.GetValue() + 0x1000;
        std::array<Service::FileSystem::ISaveDataInfoReader::SaveDataInfo, 4> info_entries{};
        const auto read_user =
            SendDomainCommand(system, memory, session_manager, server_session, guest_thread,
                              user_reader_id, 0, {}, false, info_buf_addr, sizeof(info_entries));
        REQUIRE(read_user.result == ResultSuccess);
        REQUIRE(read_user.out_word == 1);
        memory.ReadBlock(info_buf_addr, info_entries.data(), sizeof(info_entries));
        CHECK(info_entries[0].space == SaveDataSpaceId::User);
        CHECK(info_entries[0].type == SaveDataType::Account);
        CHECK(info_entries[0].title_id == OtherTitleId);

        // Temporary space + filter on SaveDataType::Temporary returns the temporary save.
        SaveDataFilter temp_filter{};
        temp_filter.use_save_data_type = true;
        temp_filter.attribute.type = SaveDataType::Temporary;
        REQUIRE(open_reader(SaveDataSpaceId::Temporary, temp_filter).result == ResultSuccess);
        const u32 temp_reader_id = static_cast<u32>(session_manager->DomainHandlerCount());
        REQUIRE(temp_reader_id > user_reader_id);

        info_entries = {};
        const auto read_temp =
            SendDomainCommand(system, memory, session_manager, server_session, guest_thread,
                              temp_reader_id, 0, {}, false, info_buf_addr, sizeof(info_entries));
        REQUIRE(read_temp.result == ResultSuccess);
        REQUIRE(read_temp.out_word == 1);
        memory.ReadBlock(info_buf_addr, info_entries.data(), sizeof(info_entries));
        CHECK(info_entries[0].space == SaveDataSpaceId::Temporary);
        CHECK(info_entries[0].type == SaveDataType::Temporary);
        CHECK(info_entries[0].title_id == TitleId);

        // cmd 28: DeleteSaveDataFileSystemBySaveDataAttribute(space_id, attribute)
        // program_id == 0 must resolve to TitleId and delete ONLY TitleId's save.
        constexpr std::size_t AttrOffset =
            Common::AlignUp(sizeof(SaveDataSpaceId), alignof(SaveDataAttribute));
        static_assert(AttrOffset == 8);

        const auto delete_by_attr = [&](SaveDataSpaceId space_id,
                                        const SaveDataAttribute& attribute) {
            std::array<u8, AttrOffset + sizeof(SaveDataAttribute)> payload{};
            std::memcpy(payload.data(), &space_id, sizeof(space_id));
            std::memcpy(payload.data() + AttrOffset, &attribute, sizeof(attribute));
            return SendDomainCommand(system, memory, session_manager, server_session, guest_thread,
                                     1, 28, payload);
        };

        const auto current_title_attr =
            SaveDataAttribute::Make(0, SaveDataType::Account, TestUserId, 0);
        REQUIRE(delete_by_attr(SaveDataSpaceId::User, current_title_attr).result == ResultSuccess);

        const auto remaining = controller->EnumerateSaveData(SaveDataSpaceId::User);
        REQUIRE(remaining.size() == 1);
        CHECK(remaining[0].title_id == OtherTitleId);

        // Second delete targets only the already-deleted TitleId save and leaves OtherTitleId intact.
        CHECK(delete_by_attr(SaveDataSpaceId::User, current_title_attr).result ==
              FileSys::ResultTargetNotFound);
        CHECK(controller->EnumerateSaveData(SaveDataSpaceId::User).size() == 1);
    });
    worker.join();
}
