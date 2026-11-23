// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_funcs.h"
#include "common/cpu_features.h"
#include "common/fs/path_util.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "core/core.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"
#include "core/loader/loader.h"
#include "core/loader/nso.h"
#include "core/platform_profile.h"
#include "video_core/control/channel_state.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu.h"
#include "video_core/host1x/host1x.h"
#include "video_core/macro.h"
#include "video_core/nx2.h"
#include "video_core/video_core.h"

namespace {

using Tegra::Engines::Maxwell3D;

class DummyEmuWindow final : public Core::Frontend::EmuWindow {
public:
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return std::make_unique<Core::Frontend::GraphicsContext>();
    }

    bool IsShown() const override {
        return false;
    }
};

struct IsolatedMacroTestEnv {
    std::filesystem::path root;
    std::filesystem::path prev_nand = Common::FS::GetEdenPath(Common::FS::EdenPath::NANDDir);
    std::filesystem::path prev_sdmc = Common::FS::GetEdenPath(Common::FS::EdenPath::SDMCDir);
    std::filesystem::path prev_load = Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir);
    std::filesystem::path prev_cache = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir);
    std::filesystem::path prev_config = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir);
    std::filesystem::path prev_shader = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir);

    Settings::RendererBackend prev_renderer = Settings::values.renderer_backend.GetValue();
    Settings::AudioEngine prev_audio = Settings::values.sink_id.GetValue();
    Settings::MemoryLayout prev_memory = Settings::values.memory_layout_mode.GetValue();
    bool prev_multicore = Settings::values.use_multi_core.GetValue();
    bool prev_renderdoc = Settings::values.enable_renderdoc_hotkey.GetValue();
    s32 prev_user = Settings::values.current_user.GetValue();
    bool prev_nx2_ampere = Tegra::g_nx2_ampere.load(std::memory_order_relaxed);
    bool prev_nx2_driver = Service::Nvidia::Devices::g_nx2_driver.load(std::memory_order_relaxed);

    explicit IsolatedMacroTestEnv(std::string_view suffix) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
               ("eglantine-macro-context-" + std::string(suffix) + "-" + std::to_string(stamp));
        std::filesystem::create_directories(root / "nand");
        std::filesystem::create_directories(root / "sdmc");
        std::filesystem::create_directories(root / "load");
        std::filesystem::create_directories(root / "cache");
        std::filesystem::create_directories(root / "config");
        std::filesystem::create_directories(root / "shader");

        Common::FS::SetEdenPath(Common::FS::EdenPath::NANDDir, root / "nand");
        Common::FS::SetEdenPath(Common::FS::EdenPath::SDMCDir, root / "sdmc");
        Common::FS::SetEdenPath(Common::FS::EdenPath::LoadDir, root / "load");
        Common::FS::SetEdenPath(Common::FS::EdenPath::CacheDir, root / "cache");
        Common::FS::SetEdenPath(Common::FS::EdenPath::ConfigDir, root / "config");
        Common::FS::SetEdenPath(Common::FS::EdenPath::ShaderDir, root / "shader");

        Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Null);
        Settings::values.sink_id.SetValue(Settings::AudioEngine::Null);
        Settings::values.memory_layout_mode.SetGlobal(true);
        Settings::values.memory_layout_mode.SetValue(Settings::MemoryLayout::Memory_4Gb);
        Settings::values.use_multi_core.SetValue(false);
        Settings::values.enable_renderdoc_hotkey.SetValue(false);
        Tegra::g_nx2_ampere.store(false, std::memory_order_relaxed);
        Service::Nvidia::Devices::g_nx2_driver.store(false, std::memory_order_relaxed);
    }

    ~IsolatedMacroTestEnv() {
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

        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    IsolatedMacroTestEnv(const IsolatedMacroTestEnv&) = delete;
    IsolatedMacroTestEnv& operator=(const IsolatedMacroTestEnv&) = delete;
};

template <typename T>
void WriteLe(std::vector<u8>& buf, std::size_t offset, T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(buf.data() + offset, &value, sizeof(T));
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
    std::memcpy(npdm.data(), "META", 4);
    npdm[0x0C] = 0x07;
    npdm[0x0E] = 0x2C;
    WriteLe<u32>(npdm, 0x1C, 0x10000);
    WriteLe<u32>(npdm, 0x70, static_cast<u32>(AciOffset));
    WriteLe<u32>(npdm, 0x74, static_cast<u32>(AciSize));
    WriteLe<u32>(npdm, 0x78, static_cast<u32>(AcidOffset));
    WriteLe<u32>(npdm, 0x7C, acid_size);

    std::memcpy(npdm.data() + AciOffset, "ACI0", 4);
    WriteLe<u64>(npdm, AciOffset + 0x10, title_id);
    WriteLe<u32>(npdm, AciOffset + 0x20, 0x40);
    WriteLe<u32>(npdm, AciOffset + 0x24, 0x1C);
    WriteLe<u32>(npdm, AciOffset + 0x28, 0x5C);
    WriteLe<u32>(npdm, AciOffset + 0x2C, 0x00);
    WriteLe<u32>(npdm, AciOffset + 0x30, 0x60);
    WriteLe<u32>(npdm, AciOffset + 0x34, 0x04);

    npdm[AciOffset + 0x40] = 1;
    WriteLe<u64>(npdm, AciOffset + 0x44, 0xFFFFFFFFFFFFFFFFULL);
    WriteLe<u32>(npdm, AciOffset + 0x60, 0x30043F7U);

    const std::size_t body = AcidOffset + acid_sig_size;
    std::memcpy(npdm.data() + body, "ACID", 4);
    WriteLe<u64>(npdm, body + 0x10, 0);
    WriteLe<u64>(npdm, body + 0x18, 0xFFFFFFFFFFFFFFFFULL);
    WriteLe<u32>(npdm, body + 0x20, fac_rel_offset);
    WriteLe<u32>(npdm, body + 0x24, FacSize);

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
    std::vector<u8> nso(sizeof(Loader::NSOHeader) + 3 * 0x1000, 0);
    std::memcpy(nso.data(), &header, sizeof(header));
    WriteLe<u32>(nso, sizeof(Loader::NSOHeader), 0xD65F03C0U);
    return nso;
}

void WriteBytes(const std::filesystem::path& path, const std::vector<u8>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE(file.is_open());
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.good());
}

std::filesystem::path WriteLooseExeFs(const std::filesystem::path& dir, u64 title_id,
                                      bool is_switch2) {
    const auto main_path = dir / "main";
    WriteBytes(main_path, MakeMinimalNso());
    WriteBytes(dir / "main.npdm", MakeNpdm(title_id, is_switch2));
    return main_path;
}

Core::PlatformProfile ResolveAndApplyProfile(Core::System& system,
                                               const std::filesystem::path& main_path,
                                               u64 title_id) {
    auto vfs = std::make_shared<FileSys::RealVfsFilesystem>();
    const auto file = vfs->OpenFile(main_path.string(), FileSys::OpenMode::Read);
    REQUIRE(file != nullptr);
    auto loader = Loader::GetLoader(system, file, title_id, 0);
    REQUIRE(loader != nullptr);
    const auto profile = Core::ResolvePlatformProfile(system, loader.get());
    system.SetPlatformProfile(profile);
    Settings::SetCurrentProgramIsSwitch2(profile.IsSwitch2());
    return profile;
}

std::shared_ptr<Tegra::Control::ChannelState> CreateSessionChannel(Tegra::GPU& gpu,
                                                                   Tegra::Host1x::Host1x& host1x,
                                                                   u64 program_id) {
    auto channel = gpu.AllocateChannel();
    REQUIRE(channel != nullptr);
    channel->memory_manager = &host1x.gmmu_manager;
    gpu.InitChannel(*channel, program_id);
    REQUIRE(channel->payload.has_value());
    return channel;
}

} // namespace

TEST_CASE("NativeMacroContext is shared across channels and isolated per session",
          "[video_core][nx2-macro-context]") {
    IsolatedMacroTestEnv env{"sessions"};

    constexpr u64 Sw2Tid1 = 0x0100EEEE00000000ULL;
    constexpr u64 Sw2Tid2 = 0x0100EEEE00002000ULL;
    constexpr u64 Sw1Tid = 0x0100FFFF00000000ULL;

    const auto sw2_main_1 = WriteLooseExeFs(env.root / "sw2_1", Sw2Tid1, true);
    const auto sw2_main_2 = WriteLooseExeFs(env.root / "sw2_2", Sw2Tid2, true);
    const auto sw1_main = WriteLooseExeFs(env.root / "sw1", Sw1Tid, false);

    DummyEmuWindow window;
    Core::System system;
    system.Initialize();
    system.Kernel().Initialize();
    SCOPE_EXIT {
        system.ShutdownMainProcess();
    };

    SECTION("Two channels in one Switch 2 session share NativeMacroContext") {
        REQUIRE(ResolveAndApplyProfile(system, sw2_main_1, Sw2Tid1).resolution ==
                Core::PlatformResolution::ConfirmedSwitch2);

        auto host1x = std::make_unique<Tegra::Host1x::Host1x>(system);
        std::optional<Tegra::GPU> gpu;
        VideoCore::CreateGPU(gpu, window, system);
        REQUIRE(gpu.has_value());
        CHECK(gpu->GetNativeMacroContext() == nullptr);

        {
            auto ch_a = CreateSessionChannel(*gpu, *host1x, Sw2Tid1);
            auto ch_b = CreateSessionChannel(*gpu, *host1x, Sw2Tid1);

            const auto ctx = gpu->GetNativeMacroContext();
            REQUIRE(ctx != nullptr);
            CHECK(&ch_a->payload->maxwell_3d.GetNativeMacroEngine() == &ctx->engine);
            CHECK(&ch_b->payload->maxwell_3d.GetNativeMacroEngine() == &ctx->engine);
            CHECK(ch_a->payload->maxwell_3d.GetNativeMacroPositions().data() ==
                  ctx->positions.data());
            CHECK(ch_b->payload->maxwell_3d.GetNativeMacroPositions().data() ==
                  ctx->positions.data());
            CHECK(ch_a->payload->maxwell_3d.GetNativeMacroEngine().native_turing);
            CHECK(ch_b->payload->kepler_compute.MayWriteMemory(0x45));

            // Upload macro code and start position via ch_a and observe it via ch_b
            constexpr u32 InstrIndex = 2;
            constexpr std::array<u32, 3> MacroWords{0x11112222U, 0x33334444U, 0x55556666U};
            constexpr u32 StartSlot = 1;
            auto& m3d_a = ch_a->payload->maxwell_3d;
            m3d_a.CallMethod(system, MAXWELL3D_REG_INDEX(load_mme.instruction_ptr), InstrIndex,
                             true);
            for (const u32 word : MacroWords) {
                m3d_a.CallMethod(system, MAXWELL3D_REG_INDEX(load_mme.instruction), word, true);
            }
            m3d_a.CallMethod(system, MAXWELL3D_REG_INDEX(load_mme.start_address_ptr), StartSlot,
                             true);
            m3d_a.CallMethod(system, MAXWELL3D_REG_INDEX(load_mme.start_address), InstrIndex, true);

            const auto& m3d_b_engine = ch_b->payload->maxwell_3d.GetNativeMacroEngine();
            CHECK(m3d_b_engine.native_code[InstrIndex * 3 + 0] == MacroWords[0]);
            CHECK(m3d_b_engine.native_code[InstrIndex * 3 + 1] == MacroWords[1]);
            CHECK(m3d_b_engine.native_code[InstrIndex * 3 + 2] == MacroWords[2]);
            CHECK(ch_b->payload->maxwell_3d.GetNativeMacroPositions()[StartSlot] == InstrIndex);
        }

        gpu->NotifyShutdown();
        gpu.reset();
    }

    SECTION("Consecutive Switch 2 sessions isolate NativeMacroContext across shutdown") {
        std::weak_ptr<Tegra::NativeMacroContext> session1_ctx;
        constexpr u32 InstrIndex = 4;
        constexpr u32 StartSlot = 3;

        {
            REQUIRE(ResolveAndApplyProfile(system, sw2_main_1, Sw2Tid1).resolution ==
                    Core::PlatformResolution::ConfirmedSwitch2);

            auto host1x = std::make_unique<Tegra::Host1x::Host1x>(system);
            std::optional<Tegra::GPU> gpu;
            VideoCore::CreateGPU(gpu, window, system);
            REQUIRE(gpu.has_value());

            {
                auto ch1 = CreateSessionChannel(*gpu, *host1x, Sw2Tid1);
                session1_ctx = gpu->GetNativeMacroContext();
                REQUIRE_FALSE(session1_ctx.expired());

                auto& m3d = ch1->payload->maxwell_3d;
                m3d.CallMethod(system, MAXWELL3D_REG_INDEX(load_mme.instruction_ptr), InstrIndex,
                               true);
                m3d.CallMethod(system, MAXWELL3D_REG_INDEX(load_mme.instruction), 0xCAFEBABEU,
                               true);
                m3d.CallMethod(system, MAXWELL3D_REG_INDEX(load_mme.start_address_ptr), StartSlot,
                               true);
                m3d.CallMethod(system, MAXWELL3D_REG_INDEX(load_mme.start_address), InstrIndex,
                               true);
            }

            gpu->NotifyShutdown();
            gpu.reset();
            CHECK(session1_ctx.expired());
        }

        system.ShutdownMainProcess();
        CHECK(system.Platform().resolution == Core::PlatformResolution::DefaultSwitch1);
        CHECK_FALSE(Settings::CurrentProgramIsSwitch2());

        system.Initialize();
        system.Kernel().Initialize();

        {
            REQUIRE(ResolveAndApplyProfile(system, sw2_main_2, Sw2Tid2).resolution ==
                    Core::PlatformResolution::ConfirmedSwitch2);

            auto host1x = std::make_unique<Tegra::Host1x::Host1x>(system);
            std::optional<Tegra::GPU> gpu;
            VideoCore::CreateGPU(gpu, window, system);
            REQUIRE(gpu.has_value());

            {
                auto ch2 = CreateSessionChannel(*gpu, *host1x, Sw2Tid2);
                const auto session2_ctx = gpu->GetNativeMacroContext();
                REQUIRE(session2_ctx != nullptr);
                CHECK(ch2->payload->maxwell_3d.GetNativeMacroEngine()
                          .native_code[InstrIndex * 3] == 0);
                CHECK(ch2->payload->maxwell_3d.GetNativeMacroPositions()[StartSlot] == 0);
            }

            gpu->NotifyShutdown();
            gpu.reset();
        }
    }

    SECTION("Switch 1 session does not allocate NativeMacroContext") {
        REQUIRE(ResolveAndApplyProfile(system, sw1_main, Sw1Tid).resolution ==
                Core::PlatformResolution::ConfirmedSwitch1);

        auto host1x = std::make_unique<Tegra::Host1x::Host1x>(system);
        std::optional<Tegra::GPU> gpu;
        VideoCore::CreateGPU(gpu, window, system);
        REQUIRE(gpu.has_value());

        {
            auto ch = CreateSessionChannel(*gpu, *host1x, Sw1Tid);
            CHECK(gpu->GetNativeMacroContext() == nullptr);
            CHECK_FALSE(ch->payload->maxwell_3d.GetNativeMacroEngine().native_turing);
            CHECK_FALSE(ch->payload->kepler_compute.MayWriteMemory(0x45));
        }

        gpu->NotifyShutdown();
        gpu.reset();
    }
}
