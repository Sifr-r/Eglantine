// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_funcs.h"
#include "common/cpu_features.h"
#include "common/fs/path_util.h"
#include "common/page_table.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "core/core.h"
#include "core/crypto/key_manager.h"
#include "core/device_memory.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"
#include "core/memory.h"
#include "core/platform_profile.h"
#include "tests/core/system_test_context.h"
#include "video_core/buffer_cache/buffer_cache_base.h"
#include "video_core/control/channel_state.h"
#include "video_core/control/channel_state_bridge.h"
#include "video_core/control/channel_state_cache.h"
#include "video_core/dirty_flags.h"
#include "video_core/dma_pusher.h"
#include "video_core/engines/engine_interface.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/engines/puller.h"
#include "video_core/gpu.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/host1x/host1x.h"
#include "video_core/host1x/syncpoint_manager.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/query_cache/types.h"
#include "video_core/rasterizer_download_area.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/texture_cache/texture_cache_base.h"

#ifdef HAS_OPENGL
#include "video_core/renderer_opengl/gl_state_tracker.h"
#endif

namespace {

enum class ReplayMode {
    LegacyDirect,
    ChannelStateBridge,
};

enum class EventCategory : u32 {
    Draw,
    ComputeDispatch,
    SemaphoreRelease,
    MemoryWrite,
    SyncEvent,
};

enum class SemaphoreSource : u32 {
    MaxwellOrPullerQuery,
    ObservedGuestValue,
};

enum class WriteSource : u32 {
    QueryReport,
    InlineUpload,
    BufferClear,
    BufferCopy,
    SnapshotCheckpoint,
};

enum class SyncEventKind : u32 {
    ChannelBound,
    SyncpointSignal,
    AcquireSuspended,
    AcquireResumed,
    ChannelReleased,
};

struct DrawRecord {
    s32 channel_id{};
    bool is_clear{};
    bool is_indexed{};
    u32 count_or_layers{};
    u32 topology{};
    u32 draw_element_count{};
    u32 vertex_first{};
    u32 base_instance{};
    bool dirty_viewports{};
    bool dirty_scissors{};
    bool dirty_blending{};
    bool dirty_line_width{};
    bool dirty_depth_test{};
    bool dirty_stencil_test{};
    bool dirty_cull_mode{};
    bool dirty_front_face{};

    bool operator==(const DrawRecord&) const = default;
};

struct ComputeRecord {
    s32 channel_id{};
    bool is_native_mode{};
    GPUVAddr program_address{};
    std::optional<GPUVAddr> indirect_address{};
    std::array<u32, 3> grid_dims{};
    std::array<u32, 3> block_dims{};
    u32 cb_enable_mask{};

    bool operator==(const ComputeRecord&) const = default;
};

struct SemaphoreRecord {
    s32 channel_id{};
    SemaphoreSource source{};
    GPUVAddr gpu_addr{};
    u32 payload{};
    u32 query_type{};
    u32 properties_flags{};
    u32 subreport{};

    bool operator==(const SemaphoreRecord&) const = default;
};

struct MemoryWriteRecord {
    s32 channel_id{};
    WriteSource source{};
    GPUVAddr gpu_addr{};
    std::vector<u8> bytes;

    bool operator==(const MemoryWriteRecord&) const = default;
};

struct SyncEventRecord {
    SyncEventKind kind{};
    s32 channel_id{};
    u32 id_or_tag{};
    GPUVAddr gpu_addr{};
    u32 value{};

    bool operator==(const SyncEventRecord&) const = default;
};

struct TimelineEntry {
    EventCategory category{};
    std::size_t index{};

    bool operator==(const TimelineEntry&) const = default;
};

struct SideEffectLog {
    std::mutex mutex;
    std::vector<DrawRecord> draws;
    std::vector<ComputeRecord> computes;
    std::vector<SemaphoreRecord> semaphores;
    std::vector<MemoryWriteRecord> memory_writes;
    std::vector<SyncEventRecord> sync_events;
    std::vector<TimelineEntry> timeline;

    void RecordDraw(const DrawRecord& rec) {
        std::scoped_lock lk{mutex};
        timeline.push_back({EventCategory::Draw, draws.size()});
        draws.push_back(rec);
    }

    void RecordCompute(const ComputeRecord& rec) {
        std::scoped_lock lk{mutex};
        timeline.push_back({EventCategory::ComputeDispatch, computes.size()});
        computes.push_back(rec);
    }

    void RecordSemaphore(const SemaphoreRecord& rec) {
        std::scoped_lock lk{mutex};
        timeline.push_back({EventCategory::SemaphoreRelease, semaphores.size()});
        semaphores.push_back(rec);
    }

    void RecordMemoryWrite(s32 channel_id_, WriteSource source_, GPUVAddr gpu_addr_,
                           std::span<const u8> data) {
        std::scoped_lock lk{mutex};
        timeline.push_back({EventCategory::MemoryWrite, memory_writes.size()});
        memory_writes.push_back(MemoryWriteRecord{
            .channel_id = channel_id_,
            .source = source_,
            .gpu_addr = gpu_addr_,
            .bytes = std::vector<u8>(data.begin(), data.end()),
        });
    }

    void RecordSync(const SyncEventRecord& rec) {
        std::scoped_lock lk{mutex};
        timeline.push_back({EventCategory::SyncEvent, sync_events.size()});
        sync_events.push_back(rec);
    }
};

struct WorkloadTrace {
    SideEffectLog log;
    std::array<u32, 8> syncpoint_values{};
    std::vector<u8> shared_memory;
    std::vector<u8> ch0_memory;
    std::vector<u8> ch1_memory;
    std::vector<Tegra::Engines::Maxwell3D::DirtyState::Flags> vk_flag_snapshots;
#ifdef HAS_OPENGL
    std::vector<Tegra::Engines::Maxwell3D::DirtyState::Flags> gl_flag_snapshots;
#endif
};

class DummyGraphicsContext final : public Core::Frontend::GraphicsContext {};

class DummyEmuWindow final : public Core::Frontend::EmuWindow {
public:
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return std::make_unique<DummyGraphicsContext>();
    }

    bool IsShown() const override {
        return false;
    }
};

struct IsolatedParityEnv {
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
    bool prev_async_gpu = Settings::values.use_asynchronous_gpu_emulation.GetValue();
    bool prev_disk_shader_cache = Settings::values.use_disk_shader_cache.GetValue();
    bool prev_renderdoc = Settings::values.enable_renderdoc_hotkey.GetValue();
    s32 prev_user = Settings::values.current_user.GetValue();
    bool prev_nx2_ampere = Tegra::g_nx2_ampere.load(std::memory_order_relaxed);
    bool prev_nx2_driver = Service::Nvidia::Devices::g_nx2_driver.load(std::memory_order_relaxed);

    explicit IsolatedParityEnv(std::string_view suffix) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
               ("eglantine-channel-bridge-" + std::string(suffix) + "-" + std::to_string(stamp));
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
        Settings::values.use_asynchronous_gpu_emulation.SetValue(false);
        Settings::values.use_disk_shader_cache.SetValue(false);
        Settings::values.enable_renderdoc_hotkey.SetValue(false);

        auto& keys = Core::Crypto::KeyManager::Instance();
        if (!keys.HasKey(Core::Crypto::S128KeyType::KeyArea, 0, 0)) {
            keys.SetKey(Core::Crypto::S128KeyType::KeyArea,
                        Core::Crypto::Key128{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
                                             0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x01},
                        0, 0);
        }
    }

    ~IsolatedParityEnv() {
        Tegra::Nx2SemaphoreWaiters::Get().Clear();
        Tegra::g_nx2_ampere.store(prev_nx2_ampere, std::memory_order_relaxed);
        Service::Nvidia::Devices::g_nx2_driver.store(prev_nx2_driver, std::memory_order_relaxed);
        Common::g_wall_clock.SetGuestCNTFRQ(Common::WallClock::CNTFRQ);
        Settings::SetCurrentProgramIsSwitch2(false);

        Settings::values.renderer_backend.SetValue(prev_renderer);
        Settings::values.sink_id.SetValue(prev_audio);
        Settings::values.memory_layout_mode.SetGlobal(true);
        Settings::values.memory_layout_mode.SetValue(prev_memory);
        Settings::values.use_multi_core.SetValue(prev_multicore);
        Settings::values.use_asynchronous_gpu_emulation.SetValue(prev_async_gpu);
        Settings::values.use_disk_shader_cache.SetValue(prev_disk_shader_cache);
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

    IsolatedParityEnv(const IsolatedParityEnv&) = delete;
    IsolatedParityEnv& operator=(const IsolatedParityEnv&) = delete;
};

class RecordingRasterizer;

class ParityAccelerateDMA final : public Tegra::Engines::AccelerateDMAInterface {
public:
    explicit ParityAccelerateDMA(RecordingRasterizer& rasterizer_) noexcept
        : rasterizer{rasterizer_} {}

    bool BufferCopy(GPUVAddr src_address, GPUVAddr dest_address, u64 amount) override;
    bool BufferClear(GPUVAddr src_address, u64 amount, u32 value) override;
    bool ImageToBuffer(const Tegra::DMA::ImageCopy&, const Tegra::DMA::ImageOperand&,
                       const Tegra::DMA::BufferOperand&) override {
        return false;
    }
    bool BufferToImage(const Tegra::DMA::ImageCopy&, const Tegra::DMA::BufferOperand&,
                       const Tegra::DMA::ImageOperand&) override {
        return false;
    }

private:
    RecordingRasterizer& rasterizer;
};

class RecordingRasterizer final
    : public VideoCore::RasterizerInterface,
      protected VideoCommon::ChannelSetupCaches<VideoCommon::ChannelInfo> {
public:
    explicit RecordingRasterizer(Tegra::GPU& gpu_, SideEffectLog& log_, ReplayMode mode_)
        : gpu{gpu_}
        , log{log_}
        , mode{mode_}
        , accelerate_dma{*this} {}

    ~RecordingRasterizer() override = default;

    Tegra::MemoryManager* ActiveMemory() noexcept {
        if (mode == ReplayMode::ChannelStateBridge) {
            return CurrentBridge().GpuMemoryPtr();
        }
        return gpu_memory;
    }

    const Vulkan::StateTracker& VkTracker() const noexcept {
        return vk_tracker;
    }

    Vulkan::StateTracker& VkTracker() noexcept {
        return vk_tracker;
    }

    std::size_t ActiveAddressSpaceCount() const noexcept {
        return address_spaces.size();
    }

    std::optional<std::size_t> LookupAddressSpaceStorageId(std::size_t as_id) const {
        return getStorageID(as_id);
    }

    Tegra::MemoryManager* LookupAddressSpaceMemory(std::size_t as_id) const {
        return GetFromID(as_id);
    }

    std::optional<Tegra::Control::ChannelStateBridge> LookupChannelBridge(s32 id) const {
        return FindBridge(id);
    }

    Tegra::Control::ChannelStateBridge ActiveChannelBridge() const noexcept {
        return CurrentBridge();
    }

    void LogInlineOrDmaWrite(WriteSource source, GPUVAddr gpu_addr, std::span<const u8> data) {
        log.RecordMemoryWrite(active_channel_id, source, gpu_addr, data);
    }

    void Draw(bool is_indexed, u32 instance_count) override {
        DrawRecord rec{};
        rec.channel_id = active_channel_id;
        rec.is_clear = false;
        rec.is_indexed = is_indexed;
        rec.count_or_layers = instance_count;
        if (mode == ReplayMode::ChannelStateBridge) {
            const auto bridge = CurrentBridge();
            const auto& draw_state = bridge.CurrentDrawState();
            rec.topology = static_cast<u32>(bridge.CurrentTopology());
            rec.draw_element_count = draw_state.draw_element_count;
            rec.vertex_first = draw_state.vertex_buffer.first;
            rec.base_instance = draw_state.base_instance;
        } else {
            ASSERT(maxwell3d != nullptr);
            const auto& draw_state = maxwell3d->draw_manager.draw_state;
            rec.topology = static_cast<u32>(draw_state.topology);
            rec.draw_element_count = draw_state.draw_element_count;
            rec.vertex_first = draw_state.vertex_buffer.first;
            rec.base_instance = draw_state.base_instance;
        }
        rec.dirty_viewports = vk_tracker.TouchViewports();
        rec.dirty_scissors = vk_tracker.TouchScissors();
        rec.dirty_blending = vk_tracker.TouchBlending();
        rec.dirty_line_width = vk_tracker.TouchLineWidth();
        rec.dirty_depth_test = vk_tracker.TouchDepthTestEnable();
        rec.dirty_stencil_test = vk_tracker.TouchStencilTestEnable();
        rec.dirty_cull_mode = vk_tracker.TouchCullMode();
        rec.dirty_front_face = vk_tracker.TouchFrontFace();
        log.RecordDraw(rec);
    }

    void DrawTexture() override {
        DrawRecord rec{};
        rec.channel_id = active_channel_id;
        rec.count_or_layers = 1;
        log.RecordDraw(rec);
    }

    void Clear(u32 layer_count) override {
        DrawRecord rec{};
        rec.channel_id = active_channel_id;
        rec.is_clear = true;
        rec.count_or_layers = layer_count;
        rec.dirty_viewports = vk_tracker.TouchViewports();
        rec.dirty_scissors = vk_tracker.TouchScissors();
        rec.dirty_depth_test = vk_tracker.TouchDepthTestEnable();
        log.RecordDraw(rec);
    }

    void DispatchCompute() override {
        ComputeRecord rec{};
        rec.channel_id = active_channel_id;
        if (mode == ReplayMode::ChannelStateBridge) {
            const auto bridge = CurrentBridge();
            const auto& launch = bridge.ComputeLaunchParams();
            rec.is_native_mode = bridge.IsComputeNativeMode();
            rec.program_address = bridge.ComputeProgramAddress();
            rec.indirect_address = bridge.IndirectComputeAddress();
            rec.grid_dims = bridge.ComputeGridDimensions();
            rec.block_dims = bridge.ComputeBlockDimensions();
            rec.cb_enable_mask = static_cast<u32>(launch.const_buffer_enable_mask);
        } else {
            ASSERT(kepler_compute != nullptr);
            const auto& launch = kepler_compute->launch_description;
            rec.is_native_mode = kepler_compute->IsNativeMode();
            rec.program_address =
                kepler_compute->regs.code_loc.Address() + launch.program_start;
            rec.indirect_address = kepler_compute->GetIndirectComputeAddress();
            rec.grid_dims = {
                static_cast<u32>(launch.grid_dim_x),
                static_cast<u32>(launch.grid_dim_y),
                static_cast<u32>(launch.grid_dim_z),
            };
            rec.block_dims = {
                static_cast<u32>(launch.block_dim_x),
                static_cast<u32>(launch.block_dim_y),
                static_cast<u32>(launch.block_dim_z),
            };
            rec.cb_enable_mask = static_cast<u32>(launch.const_buffer_enable_mask);
        }
        log.RecordCompute(rec);
    }

    void ResetCounter(VideoCommon::QueryType) override {}

    void Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
               VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) override {
        auto* const mem = ActiveMemory();
        const bool is_fence = True(flags & VideoCommon::QueryPropertiesFlags::IsAFence);
        if (mem != nullptr && gpu_addr != 0 && mem->GpuToCpuAddress(gpu_addr).has_value()) {
            if (is_fence) {
                mem->Write<u32>(gpu_addr, payload);
                std::array<u8, 4> bytes{};
                std::memcpy(bytes.data(), &payload, sizeof(payload));
                log.RecordMemoryWrite(active_channel_id, WriteSource::QueryReport, gpu_addr,
                                      bytes);
            } else {
                struct ReportWords {
                    u64 value;
                    u64 timestamp;
                };
                const ReportWords report{
                    .value = static_cast<u64>(payload),
                    .timestamp = 0x5A5A000000000000ULL | static_cast<u64>(log.semaphores.size()),
                };
                mem->WriteBlockUnsafe(gpu_addr, &report, sizeof(report));
                std::array<u8, sizeof(ReportWords)> bytes{};
                std::memcpy(bytes.data(), &report, sizeof(report));
                log.RecordMemoryWrite(active_channel_id, WriteSource::QueryReport, gpu_addr,
                                      bytes);
            }
        }
        log.RecordSemaphore(SemaphoreRecord{
            .channel_id = active_channel_id,
            .source = SemaphoreSource::MaxwellOrPullerQuery,
            .gpu_addr = gpu_addr,
            .payload = payload,
            .query_type = static_cast<u32>(type),
            .properties_flags = static_cast<u32>(flags),
            .subreport = subreport,
        });
    }

    void BindGraphicsUniformBuffer(size_t, u32, GPUVAddr, u32) override {}
    void DisableGraphicsUniformBuffer(size_t, u32) override {}
    void FlushAll() override {}
    void FlushRegion(DAddr, u64, VideoCommon::CacheType) override {}
    bool MustFlushRegion(DAddr, u64, VideoCommon::CacheType) override {
        return false;
    }
    void InvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void OnCacheInvalidation(DAddr, u64) override {}
    bool OnCPUWrite(DAddr, u64) override {
        return false;
    }
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override {
        return {addr, addr + size, true};
    }
    void InvalidateGPUCache() override {}
    void UnmapMemory(DAddr, u64) override {}
    void ModifyGPUMemory(size_t, GPUVAddr, u64) override {}

    void SignalFence(std::function<void()>&& func, bool) override {
        if (func) {
            func();
        }
    }

    void SyncOperation(std::function<void()>&& func) override {
        if (func) {
            func();
        }
    }

    void SignalSyncPoint(u32 value) override {
        const u32 guest_val = gpu.Host1x().GetSyncpointManager().GetGuestSyncpointValue(value);
        log.RecordSync(SyncEventRecord{
            .kind = SyncEventKind::SyncpointSignal,
            .channel_id = active_channel_id,
            .id_or_tag = value,
            .gpu_addr = 0,
            .value = guest_val,
        });
    }

    void SignalReference() override {}
    void ReleaseFences(bool) override {}
    void FlushAndInvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void WaitForIdle() override {}
    void FragmentBarrier() override {}
    void TiledCacheBarrier() override {}
    void FlushCommands() override {}
    void TickFrame() override {}

    bool AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface&,
                               const Tegra::Engines::Fermi2D::Surface&,
                               const Tegra::Engines::Fermi2D::Config&) override {
        return false;
    }

    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override {
        return accelerate_dma;
    }

    void AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                  std::span<const u8> memory) override {
        auto* const mem = ActiveMemory();
        const std::size_t write_size = (std::min)(copy_size, memory.size());
        if (mem != nullptr && write_size > 0) {
            mem->WriteBlockUnsafe(address, memory.data(), write_size);
        }
        log.RecordMemoryWrite(active_channel_id, WriteSource::InlineUpload, address,
                              memory.subspan(0, write_size));
    }

    void LoadDiskResources(u64, std::stop_token,
                           const VideoCore::DiskResourceLoadCallback&) override {}

    void InitializeChannel(Tegra::Control::ChannelState& channel) override {
        if (mode == ReplayMode::ChannelStateBridge) {
            const Tegra::Control::ChannelStateBridge bridge{channel};
            CreateChannelFromBridge(bridge);
            vk_tracker.SetupTables(bridge);
        } else {
            CreateChannel(channel);
            vk_tracker.SetupTables(channel);
        }
    }

    void BindChannel(Tegra::Control::ChannelState& channel) override {
        active_channel_id = channel.bind_id;
        BindToChannel(channel.bind_id);
        if (mode == ReplayMode::ChannelStateBridge) {
            const Tegra::Control::ChannelStateBridge bridge{channel};
            vk_tracker.ChangeChannel(bridge);
            REQUIRE(CurrentBridge().IsBound());
            REQUIRE(CurrentBridge().BindId() == channel.bind_id);
            REQUIRE(CurrentBridge().Maxwell3DPtr() == &channel.payload->maxwell_3d);
        } else {
            vk_tracker.ChangeChannel(channel);
        }
        log.RecordSync(SyncEventRecord{
            .kind = SyncEventKind::ChannelBound,
            .channel_id = active_channel_id,
            .id_or_tag = static_cast<u32>(active_channel_id),
            .gpu_addr = 0,
            .value = 0,
        });
    }

    void ReleaseChannel(s32 channel_id) override {
        if (active_channel_id == channel_id) {
            vk_tracker.ClearChannel();
            active_channel_id = -1;
        }
        EraseChannel(channel_id);
        log.RecordSync(SyncEventRecord{
            .kind = SyncEventKind::ChannelReleased,
            .channel_id = channel_id,
            .id_or_tag = static_cast<u32>(channel_id),
            .gpu_addr = 0,
            .value = 0,
        });
    }

private:
    Tegra::GPU& gpu;
    SideEffectLog& log;
    ReplayMode mode;
    s32 active_channel_id = -1;
    Vulkan::StateTracker vk_tracker;
    ParityAccelerateDMA accelerate_dma;
};

bool ParityAccelerateDMA::BufferCopy(GPUVAddr src_address, GPUVAddr dest_address, u64 amount) {
    auto* const mem = rasterizer.ActiveMemory();
    if (mem == nullptr || amount == 0) {
        return false;
    }
    std::vector<u8> buf(static_cast<std::size_t>(amount));
    mem->ReadBlockUnsafe(src_address, buf.data(), buf.size());
    mem->WriteBlockUnsafe(dest_address, buf.data(), buf.size());
    rasterizer.LogInlineOrDmaWrite(WriteSource::BufferCopy, dest_address, buf);
    return true;
}

bool ParityAccelerateDMA::BufferClear(GPUVAddr src_address, u64 amount, u32 value) {
    auto* const mem = rasterizer.ActiveMemory();
    if (mem == nullptr || amount == 0) {
        return false;
    }
    std::vector<u32> words(static_cast<std::size_t>(amount), value);
    const std::span<const u8> bytes{reinterpret_cast<const u8*>(words.data()),
                                    words.size() * sizeof(u32)};
    mem->WriteBlockUnsafe(src_address, bytes.data(), bytes.size());
    rasterizer.LogInlineOrDmaWrite(WriteSource::BufferClear, src_address, bytes);
    return true;
}

class RecordingRenderer final : public VideoCore::RendererBase {
public:
    RecordingRenderer(Core::Frontend::EmuWindow& emu_window_, Tegra::GPU& gpu_,
                      SideEffectLog& log_, ReplayMode mode_)
        : RendererBase{emu_window_, emu_window_.CreateSharedContext()}
        , m_gpu{gpu_}
        , m_rasterizer{m_gpu, log_, mode_} {}

    ~RecordingRenderer() override = default;

    void Composite(std::span<const Tegra::FramebufferConfig>) override {}

    std::vector<u8> GetAppletCaptureBuffer() override {
        return {};
    }

    [[nodiscard]] VideoCore::RasterizerInterface* ReadRasterizer() override {
        return &m_rasterizer;
    }

    [[nodiscard]] std::string GetDeviceVendor() const override {
        return "EGLANTINE_PARITY";
    }

    RecordingRasterizer& Rasterizer() noexcept {
        return m_rasterizer;
    }

private:
    Tegra::GPU& m_gpu;
    RecordingRasterizer m_rasterizer;
};

Tegra::CommandHeader MakeMethodHeader(u32 method, u32 subchannel, u32 arg_count,
                                      Tegra::SubmissionMode mode) {
    Tegra::CommandHeader header{};
    header.method.Assign(method);
    header.subchannel.Assign(subchannel);
    header.arg_count.Assign(arg_count);
    header.mode.Assign(mode);
    return header;
}

void AppendPullerMethod(boost::container::small_vector<Tegra::CommandHeader, 512>& cmds,
                        Tegra::BufferMethods method, u32 value) {
    cmds.push_back(
        Tegra::BuildCommandHeader(method, 1, Tegra::SubmissionMode::Increasing));
    cmds.push_back(Tegra::CommandHeader{.argument = value});
}

void AppendEngineMethod(boost::container::small_vector<Tegra::CommandHeader, 512>& cmds,
                        u32 subchannel, u32 method, u32 value) {
    cmds.push_back(
        MakeMethodHeader(method, subchannel, 1, Tegra::SubmissionMode::Increasing));
    cmds.push_back(Tegra::CommandHeader{.argument = value});
}

void AppendEngineIncreasing(boost::container::small_vector<Tegra::CommandHeader, 512>& cmds,
                            u32 subchannel, u32 start_method, std::span<const u32> values) {
    cmds.push_back(MakeMethodHeader(start_method, subchannel, static_cast<u32>(values.size()),
                                    Tegra::SubmissionMode::Increasing));
    for (const u32 word : values) {
        cmds.push_back(Tegra::CommandHeader{.argument = word});
    }
}

void AppendEngineNonIncreasing(boost::container::small_vector<Tegra::CommandHeader, 512>& cmds,
                               u32 subchannel, u32 method, std::span<const u32> values) {
    cmds.push_back(MakeMethodHeader(method, subchannel, static_cast<u32>(values.size()),
                                    Tegra::SubmissionMode::NonIncreasing));
    for (const u32 word : values) {
        cmds.push_back(Tegra::CommandHeader{.argument = word});
    }
}

void AppendBindObject(boost::container::small_vector<Tegra::CommandHeader, 512>& cmds,
                      u32 subchannel, Tegra::EngineID engine) {
    AppendPullerMethod(cmds, Tegra::BufferMethods::BindObject, static_cast<u32>(engine));
    cmds[cmds.size() - 2].subchannel.Assign(subchannel);
}

void AppendSemaphoreWrite(boost::container::small_vector<Tegra::CommandHeader, 512>& cmds,
                          GPUVAddr gpu_addr, u32 payload, bool release_size_4bytes) {
    AppendPullerMethod(cmds, Tegra::BufferMethods::SemaphoreAddressHigh,
                       static_cast<u32>(gpu_addr >> 32));
    AppendPullerMethod(cmds, Tegra::BufferMethods::SemaphoreAddressLow,
                       static_cast<u32>(gpu_addr & 0xFFFFFFFFULL));
    AppendPullerMethod(cmds, Tegra::BufferMethods::SemaphoreSequencePayload, payload);
    const u32 trigger = 0x2U | (release_size_4bytes ? (1U << 24) : 0U);
    AppendPullerMethod(cmds, Tegra::BufferMethods::SemaphoreOperation, trigger);
}

void AppendSemaphoreAcquireEqual(boost::container::small_vector<Tegra::CommandHeader, 512>& cmds,
                                 GPUVAddr gpu_addr, u32 expected) {
    AppendPullerMethod(cmds, Tegra::BufferMethods::SemaphoreAddressHigh,
                       static_cast<u32>(gpu_addr >> 32));
    AppendPullerMethod(cmds, Tegra::BufferMethods::SemaphoreAddressLow,
                       static_cast<u32>(gpu_addr & 0xFFFFFFFFULL));
    AppendPullerMethod(cmds, Tegra::BufferMethods::SemaphoreSequencePayload, expected);
    AppendPullerMethod(cmds, Tegra::BufferMethods::SemaphoreOperation, 0x1U);
}

void AppendSyncpointIncrement(boost::container::small_vector<Tegra::CommandHeader, 512>& cmds,
                              u32 syncpoint_id) {
    Tegra::Engines::Puller::FenceAction action{};
    action.op.Assign(Tegra::Engines::Puller::FenceOperation::Increment);
    action.syncpoint_id.Assign(syncpoint_id);
    AppendPullerMethod(cmds, Tegra::BufferMethods::SyncpointOperation, action.raw);
}

void AppendMaxwellSemaphoreRelease(boost::container::small_vector<Tegra::CommandHeader, 512>& cmds,
                                   u32 subchannel, GPUVAddr gpu_addr, u32 payload,
                                   bool short_query) {
    using Maxwell3D = Tegra::Engines::Maxwell3D;
    u32 query_raw = static_cast<u32>(Maxwell3D::Regs::ReportSemaphore::Operation::Release);
    if (short_query) {
        query_raw |= (1U << 28);
    }
    const std::array<u32, 4> words{
        static_cast<u32>(gpu_addr >> 32),
        static_cast<u32>(gpu_addr & 0xFFFFFFFFULL),
        payload,
        query_raw,
    };
    AppendEngineIncreasing(cmds, subchannel,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(report_semaphore.address_high)),
                           words);
}

void AppendInlineComputeUpload(boost::container::small_vector<Tegra::CommandHeader, 512>& cmds,
                               u32 subchannel, GPUVAddr dest_addr, std::span<const u32> words) {
    const u32 byte_size = static_cast<u32>(words.size() * sizeof(u32));
    AppendEngineMethod(cmds, subchannel,
                       static_cast<u32>(KEPLER_COMPUTE_REG_INDEX(upload.line_length_in)),
                       byte_size);
    AppendEngineMethod(cmds, subchannel,
                       static_cast<u32>(KEPLER_COMPUTE_REG_INDEX(upload.line_count)), 1);
    AppendEngineMethod(cmds, subchannel,
                       static_cast<u32>(KEPLER_COMPUTE_REG_INDEX(upload.dest.address_high)),
                       static_cast<u32>(dest_addr >> 32));
    AppendEngineMethod(cmds, subchannel,
                       static_cast<u32>(KEPLER_COMPUTE_REG_INDEX(upload.dest.address_low)),
                       static_cast<u32>(dest_addr & 0xFFFFFFFFULL));
    AppendEngineMethod(cmds, subchannel,
                       static_cast<u32>(KEPLER_COMPUTE_REG_INDEX(exec_upload)), 1);
    AppendEngineNonIncreasing(cmds, subchannel,
                              static_cast<u32>(KEPLER_COMPUTE_REG_INDEX(data_upload)), words);
}

std::vector<u32> MakeNativeInlineQmdWords(GPUVAddr program_addr, u32 grid_x, u32 grid_y,
                                          u32 grid_z, u32 block_x, u32 block_y, u32 block_z) {
    std::vector<u32> qmd(64, 0);
    qmd[12] = grid_x;
    qmd[13] = grid_y & 0xFFFFU;
    qmd[14] = grid_z & 0xFFFFU;
    qmd[17] = 0x400U; // shared_alloc
    qmd[18] = ((block_x & 0xFFFFU) << 16) | 0x30U; // QMD V03_00 + block_dim_x
    qmd[19] = (block_y & 0xFFFFU) | ((block_z & 0xFFFFU) << 16);
    qmd[20] = 0x3U; // const_buffer_enable_mask
    qmd[48] = static_cast<u32>(program_addr & 0xFFFFFFFFULL);
    qmd[49] = static_cast<u32>((program_addr >> 32) & 0x1FFFFULL);
    return qmd;
}

constexpr GPUVAddr SharedGpuBase = 0x10000000ULL;
constexpr DAddr SharedDevBase = 0x10000ULL;
constexpr std::size_t SharedSize = 0x10000U;

constexpr GPUVAddr Ch0GpuBase = 0x20000000ULL;
constexpr DAddr Ch0DevBase = 0x20000ULL;
constexpr std::size_t Ch0Size = 0x10000U;

constexpr GPUVAddr Ch1GpuBase = 0x30000000ULL;
constexpr DAddr Ch1DevBase = 0x30000ULL;
constexpr std::size_t Ch1Size = 0x10000U;

constexpr GPUVAddr SharedSem0Addr = SharedGpuBase + 0x0100ULL;
constexpr GPUVAddr SharedSem1Addr = SharedGpuBase + 0x0200ULL;
constexpr GPUVAddr SharedSemWakeAddr = SharedGpuBase + 0x0300ULL;
constexpr GPUVAddr SharedSemDoneAddr = SharedGpuBase + 0x0400ULL;
constexpr GPUVAddr SharedDirectAcquireAddr = SharedGpuBase + 0x0500ULL;
constexpr GPUVAddr SharedDirectDoneAddr = SharedGpuBase + 0x0600ULL;

constexpr GPUVAddr Ch0IndexBufAddr = Ch0GpuBase + 0x0000ULL;
constexpr GPUVAddr Ch0QueryAddr = Ch0GpuBase + 0x0100ULL;
constexpr GPUVAddr Ch0DeferredQueryAddr = Ch0GpuBase + 0x0200ULL;

constexpr GPUVAddr Ch1LaunchDescAddr = Ch1GpuBase + 0x0000ULL;
constexpr GPUVAddr Ch1ComputeCodeLoc = Ch1GpuBase + 0x0400ULL;
constexpr GPUVAddr Ch1InlineUploadAddr = Ch1GpuBase + 0x1000ULL;
constexpr GPUVAddr Ch1ReportSemAddr = Ch1GpuBase + 0x2000ULL;
constexpr GPUVAddr Ch1NativeQmdAddr = Ch1GpuBase + 0x3000ULL;
constexpr GPUVAddr Ch1NativeProgAddr = Ch1GpuBase + 0x4000ULL;

void ExecuteParityWorkload(WorkloadTrace& trace, bool is_switch2, ReplayMode mode,
                           std::string_view tag) {
    using Maxwell3D = Tegra::Engines::Maxwell3D;
    using KeplerCompute = Tegra::Engines::KeplerCompute;

    IsolatedParityEnv env{tag};
    Tegra::Nx2SemaphoreWaiters::Get().Clear();
    Settings::values.memory_layout_mode.SetValue(is_switch2 ? Settings::MemoryLayout::Memory_12Gb
                                                            : Settings::MemoryLayout::Memory_4Gb);

    DummyEmuWindow window;
    Core::System system;
    const u64 title_id = is_switch2 ? 0x0100F00D00000002ULL : 0x0100F00D00000001ULL;
    system.SetPlatformProfile(Core::MakePlatformProfile(
        is_switch2 ? Core::PlatformResolution::ConfirmedSwitch2
                   : Core::PlatformResolution::ConfirmedSwitch1));
    Settings::SetCurrentProgramIsSwitch2(is_switch2);
    Tegra::g_nx2_ampere.store(is_switch2, std::memory_order_relaxed);
    system.Initialize();
    system.Kernel().Initialize();
    system.InitializeGpuForTesting(window);

    auto& gpu = system.GPU();
    auto renderer_owned = std::make_unique<RecordingRenderer>(window, gpu, trace.log, mode);
    auto* const recorder_renderer = renderer_owned.get();
    gpu.BindRenderer(std::move(renderer_owned));
    gpu.Start();

    std::unique_ptr<Tegra::MemoryManager> mem0;
    std::unique_ptr<Tegra::MemoryManager> mem1;
    std::shared_ptr<Tegra::Control::ChannelState> ch0;
    std::shared_ptr<Tegra::Control::ChannelState> ch1;
    std::optional<Kernel::KProcess> backing_process;
    backing_process.emplace(system.Kernel());

    constexpr std::size_t backing_pages = 64;
    constexpr std::size_t backing_size = backing_pages * 0x1000U;
    auto& dev_mem = system.Host1x().MemoryManager();
    std::optional<Core::Asid> dev_asid;

    SCOPE_EXIT {
        ch1.reset();
        ch0.reset();
        if (dev_asid.has_value()) {
            dev_mem.UnregisterProcess(*dev_asid);
        }
        if (backing_process.has_value()) {
            system.Kernel().UnregisterKernelObject(&*backing_process);
            backing_process.reset();
        }
        system.ShutdownMainProcess();
        mem1.reset();
        mem0.reset();
    };

    // Map 0x40000 bytes (64 pages) starting at VAddr/DAddr 0x10000 into DeviceMemory.
    // Offset 0x10000..0x50000 sits strictly below DramMemoryMap::KernelReserveBase (0x60000),
    // so it never overlaps kernel slab heaps (SlabHeapBase = 0xE5000).
    u8* const dev_backing = system.DeviceMemory().buffer.BackingBasePointer() + 0x10000ULL;
    std::memset(dev_backing, 0, backing_size);

    auto& page_table = backing_process->GetPageTable().GetImpl();
    page_table.Resize(20, 12);
    for (std::size_t page_idx = 0; page_idx < backing_pages; ++page_idx) {
        const VAddr vaddr = 0x10000ULL + page_idx * 0x1000ULL;
        u8* const page_ptr = dev_backing + page_idx * 0x1000ULL;
        page_table.entries.GetAndFault(vaddr >> 12).Store(
            false, Common::PageType::Memory, 1, reinterpret_cast<uintptr_t>(page_ptr) - vaddr);
    }
    backing_process->GetMemory().SetCurrentPageTable(*backing_process);

    dev_asid = dev_mem.RegisterProcess(&backing_process->GetMemory());
    dev_mem.Map(SharedDevBase, SharedDevBase, backing_size, *dev_asid);

    mem0 = std::make_unique<Tegra::MemoryManager>(system, dev_mem, 32, 1ULL << 31, 16);
    mem1 = std::make_unique<Tegra::MemoryManager>(system, dev_mem, 32, 1ULL << 31, 16);
    gpu.InitAddressSpace(*mem0);
    gpu.InitAddressSpace(*mem1);
    mem0->Map(SharedGpuBase, SharedDevBase, SharedSize, Tegra::PTEKind::PITCH, false);
    mem0->Map(Ch0GpuBase, Ch0DevBase, Ch0Size, Tegra::PTEKind::PITCH, false);
    mem1->Map(SharedGpuBase, SharedDevBase, SharedSize, Tegra::PTEKind::PITCH, false);
    mem1->Map(Ch1GpuBase, Ch1DevBase, Ch1Size, Tegra::PTEKind::PITCH, false);

    // Seed Channel 0 index buffer and Channel 1 Switch 1 compute launch descriptor.
    const std::array<u32, 6> indices{0, 1, 2, 2, 1, 3};
    mem0->WriteBlockUnsafe(Ch0IndexBufAddr, indices.data(), indices.size() * sizeof(u32));

    KeplerCompute::LaunchParams launch_params{};
    launch_params.program_start = 0x80U;
    launch_params.grid_dim_x.Assign(4U);
    launch_params.grid_dim_y.Assign(2U);
    launch_params.grid_dim_z.Assign(1U);
    launch_params.block_dim_x.Assign(32U);
    launch_params.block_dim_y.Assign(2U);
    launch_params.block_dim_z.Assign(1U);
    launch_params.shared_alloc.Assign(0x200U);
    launch_params.const_buffer_enable_mask.Assign(0x5U);
    mem1->WriteBlockUnsafe(Ch1LaunchDescAddr, &launch_params, sizeof(launch_params));

    ch0 = gpu.AllocateChannel();
    ch0->memory_manager = mem0.get();
    gpu.InitChannel(*ch0, title_id);

    ch1 = gpu.AllocateChannel();
    ch1->memory_manager = mem1.get();
    gpu.InitChannel(*ch1, title_id);

    auto& rasterizer = recorder_renderer->Rasterizer();
    REQUIRE(rasterizer.ActiveAddressSpaceCount() == 2);
    REQUIRE(rasterizer.LookupAddressSpaceMemory(mem0->GetID()) == mem0.get());
    REQUIRE(rasterizer.LookupAddressSpaceMemory(mem1->GetID()) == mem1.get());
    REQUIRE(rasterizer.LookupAddressSpaceStorageId(mem0->GetID()).has_value());
    REQUIRE(rasterizer.LookupAddressSpaceStorageId(mem1->GetID()).has_value());

    const Tegra::Control::ChannelStateBridge bridge0{*ch0};
    const Tegra::Control::ChannelStateBridge bridge1{*ch1};
    REQUIRE(bridge0.IsInitialized());
    REQUIRE(bridge1.IsInitialized());
    REQUIRE(bridge0.IsNativeAmpere() == is_switch2);
    REQUIRE(bridge1.IsNativeAmpere() == is_switch2);

    if (mode == ReplayMode::ChannelStateBridge) {
        const auto found0 = rasterizer.LookupChannelBridge(ch0->bind_id);
        const auto found1 = rasterizer.LookupChannelBridge(ch1->bind_id);
        REQUIRE(found0.has_value());
        REQUIRE(found1.has_value());
        REQUIRE(found0->Maxwell3DPtr() == &ch0->payload->maxwell_3d);
        REQUIRE(found1->KeplerComputePtr() == &ch1->payload->kepler_compute);
    }

    constexpr u32 Sub3D = 0;
    constexpr u32 SubCompute = 1;

    // -------------------------------------------------------------------------
    // Phase I: Multi-channel GPU scheduler submission (with Switch 2 suspension)
    // -------------------------------------------------------------------------
    if (is_switch2) {
        // Channel 0 suspends waiting for SharedSemWakeAddr == 0xCAFE0001.
        boost::container::small_vector<Tegra::CommandHeader, 512> ch0_wait_cmds;
        AppendBindObject(ch0_wait_cmds, Sub3D, Tegra::EngineID::MAXWELL_B);
        AppendSemaphoreAcquireEqual(ch0_wait_cmds, SharedSemWakeAddr, 0xCAFE0001U);
        AppendEngineMethod(ch0_wait_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(line_width_smooth)),
                           std::bit_cast<u32>(2.5f));
        AppendEngineMethod(ch0_wait_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(depth_test_enable)), 1U);
        AppendEngineMethod(ch0_wait_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(draw.begin)),
                           static_cast<u32>(Maxwell3D::Regs::PrimitiveTopology::Triangles));
        AppendEngineMethod(ch0_wait_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(vertex_buffer.first)), 0U);
        AppendEngineMethod(ch0_wait_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(vertex_buffer.count)), 6U);
        AppendSemaphoreWrite(ch0_wait_cmds, SharedSemDoneAddr, 0xCAFE0002U, true);
        AppendSyncpointIncrement(ch0_wait_cmds, 3U);

        gpu.PushGPUEntries(ch0->bind_id, Tegra::CommandList{std::move(ch0_wait_cmds)});

        const bool ch0_pending = (mode == ReplayMode::ChannelStateBridge)
                                     ? bridge0.HasPendingAcquire()
                                     : ch0->payload->dma_pusher.HasPendingAcquire();
        REQUIRE(ch0_pending);
        REQUIRE(gpu.HasSuspendedChannels());
        trace.log.RecordSync(SyncEventRecord{
            .kind = SyncEventKind::AcquireSuspended,
            .channel_id = ch0->bind_id,
            .id_or_tag = 1U,
            .gpu_addr = SharedSemWakeAddr,
            .value = 0xCAFE0001U,
        });

        // Channel 1 executes graphics + inline upload + Switch 2 inline QMD + 0x6c3 semaphore,
        // then writes SharedSemWakeAddr == 0xCAFE0001 which wakes Channel 0 via Scheduler.
        boost::container::small_vector<Tegra::CommandHeader, 512> ch1_cmds;
        AppendBindObject(ch1_cmds, Sub3D, Tegra::EngineID::MAXWELL_B);
        AppendBindObject(ch1_cmds, SubCompute, Tegra::EngineID::KEPLER_COMPUTE_B);
        AppendEngineMethod(ch1_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(clear_surface)), 0x3CU);
        AppendEngineMethod(ch1_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(gl_cull_test_enabled)), 1U);
        AppendEngineMethod(ch1_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(draw.begin)),
                           static_cast<u32>(Maxwell3D::Regs::PrimitiveTopology::Triangles));
        AppendEngineMethod(ch1_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(vertex_buffer.first)), 1U);
        AppendEngineMethod(ch1_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(vertex_buffer.count)), 4U);

        const std::array<u32, 4> inline_words{0x11223344U, 0x55667788U, 0x99AABBCCU, 0xDDEEFF00U};
        AppendInlineComputeUpload(ch1_cmds, SubCompute, Ch1InlineUploadAddr, inline_words);

        // Native Switch 2 inline QMD (0xc6/0xc7 address>>8, 0xc8..0x106 QMD words, 0x107 trigger)
        AppendEngineMethod(ch1_cmds, SubCompute, 0xA7U, 0U);
        AppendEngineMethod(ch1_cmds, SubCompute, 0xC6U,
                           static_cast<u32>((Ch1NativeQmdAddr >> 8) >> 32));
        AppendEngineMethod(ch1_cmds, SubCompute, 0xC7U,
                           static_cast<u32>((Ch1NativeQmdAddr >> 8) & 0xFFFFFFFFULL));
        const auto qmd_words =
            MakeNativeInlineQmdWords(Ch1NativeProgAddr, 3U, 2U, 1U, 64U, 1U, 1U);
        AppendEngineIncreasing(ch1_cmds, SubCompute, 0xC8U, qmd_words);

        // Native report semaphore 0x6c0..0x6c3 (one_word = bit 28, Release = 0)
        AppendEngineMethod(ch1_cmds, SubCompute, 0x6C0U,
                           static_cast<u32>((Ch1ReportSemAddr >> 32) & 0x1FFFFFFU));
        AppendEngineMethod(ch1_cmds, SubCompute, 0x6C1U,
                           static_cast<u32>(Ch1ReportSemAddr & 0xFFFFFFFFULL));
        AppendEngineMethod(ch1_cmds, SubCompute, 0x6C2U, 0x55667788U);
        AppendEngineMethod(ch1_cmds, SubCompute, 0x6C3U, (1U << 28));

        AppendSemaphoreWrite(ch1_cmds, SharedSemWakeAddr, 0xCAFE0001U, true);
        AppendSyncpointIncrement(ch1_cmds, 2U);

        gpu.PushGPUEntries(ch1->bind_id, Tegra::CommandList{std::move(ch1_cmds)});

        const bool ch0_still_pending = (mode == ReplayMode::ChannelStateBridge)
                                           ? bridge0.HasPendingAcquire()
                                           : ch0->payload->dma_pusher.HasPendingAcquire();
        REQUIRE_FALSE(ch0_still_pending);
        REQUIRE_FALSE(gpu.HasSuspendedChannels());
        trace.log.RecordSync(SyncEventRecord{
            .kind = SyncEventKind::AcquireResumed,
            .channel_id = ch0->bind_id,
            .id_or_tag = 1U,
            .gpu_addr = SharedSemWakeAddr,
            .value = 0xCAFE0001U,
        });
    } else {
        // Switch 1: Channel 0 runs state setup + clear + draw + Maxwell query + Puller release + syncpoint
        boost::container::small_vector<Tegra::CommandHeader, 512> ch0_cmds;
        AppendBindObject(ch0_cmds, Sub3D, Tegra::EngineID::MAXWELL_B);
        AppendEngineMethod(ch0_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(clear_surface)), 0x3CU);
        AppendEngineMethod(ch0_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(viewport_scale_offset_enabled)),
                           1U);
        AppendEngineMethod(ch0_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(scissor_test)), 1U);
        AppendEngineMethod(ch0_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(line_width_smooth)),
                           std::bit_cast<u32>(1.75f));
        AppendEngineMethod(ch0_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(depth_test_enable)), 1U);
        AppendEngineMethod(ch0_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(stencil_enable)), 1U);
        AppendEngineMethod(ch0_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(gl_front_face)), 1U);
        AppendEngineMethod(ch0_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(draw.begin)),
                           static_cast<u32>(Maxwell3D::Regs::PrimitiveTopology::Triangles));
        AppendEngineMethod(ch0_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(vertex_buffer.first)), 0U);
        AppendEngineMethod(ch0_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(vertex_buffer.count)), 6U);
        AppendMaxwellSemaphoreRelease(ch0_cmds, Sub3D, Ch0QueryAddr, 0xAAA00001U, true);
        AppendSemaphoreWrite(ch0_cmds, SharedSem0Addr, 0x11110001U, false);
        AppendSyncpointIncrement(ch0_cmds, 1U);

        gpu.PushGPUEntries(ch0->bind_id, Tegra::CommandList{std::move(ch0_cmds)});

        // Channel 1 acquires SharedSem0Addr, uploads inline data, launches Switch 1 compute,
        // releases SharedSem1Addr, and increments syncpoint 2.
        boost::container::small_vector<Tegra::CommandHeader, 512> ch1_cmds;
        AppendBindObject(ch1_cmds, SubCompute, Tegra::EngineID::KEPLER_COMPUTE_B);
        AppendSemaphoreAcquireEqual(ch1_cmds, SharedSem0Addr, 0x11110001U);

        const std::array<u32, 4> inline_words{0xCAFE0001U, 0xCAFE0002U, 0xCAFE0003U, 0xCAFE0004U};
        AppendInlineComputeUpload(ch1_cmds, SubCompute, Ch1InlineUploadAddr, inline_words);

        AppendEngineMethod(ch1_cmds, SubCompute,
                           static_cast<u32>(KEPLER_COMPUTE_REG_INDEX(code_loc.address_high)),
                           static_cast<u32>(Ch1ComputeCodeLoc >> 32));
        AppendEngineMethod(ch1_cmds, SubCompute,
                           static_cast<u32>(KEPLER_COMPUTE_REG_INDEX(code_loc.address_low)),
                           static_cast<u32>(Ch1ComputeCodeLoc & 0xFFFFFFFFULL));
        AppendEngineMethod(ch1_cmds, SubCompute,
                           static_cast<u32>(KEPLER_COMPUTE_REG_INDEX(launch_desc_loc)),
                           static_cast<u32>(Ch1LaunchDescAddr >> 8));
        AppendEngineMethod(ch1_cmds, SubCompute,
                           static_cast<u32>(KEPLER_COMPUTE_REG_INDEX(launch)), 1U);

        AppendSemaphoreWrite(ch1_cmds, SharedSem1Addr, 0x22220002U, false);
        AppendSyncpointIncrement(ch1_cmds, 2U);

        gpu.PushGPUEntries(ch1->bind_id, Tegra::CommandList{std::move(ch1_cmds)});

        // Channel 0 acquires SharedSem1Addr and executes an indexed draw.
        boost::container::small_vector<Tegra::CommandHeader, 512> ch0_follow_cmds;
        AppendSemaphoreAcquireEqual(ch0_follow_cmds, SharedSem1Addr, 0x22220002U);
        AppendEngineMethod(ch0_follow_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(draw.begin)),
                           static_cast<u32>(Maxwell3D::Regs::PrimitiveTopology::Triangles));
        AppendEngineMethod(ch0_follow_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(index_buffer.start_addr_high)),
                           static_cast<u32>(Ch0IndexBufAddr >> 32));
        AppendEngineMethod(ch0_follow_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(index_buffer.start_addr_low)),
                           static_cast<u32>(Ch0IndexBufAddr & 0xFFFFFFFFULL));
        AppendEngineMethod(
            ch0_follow_cmds, Sub3D,
            static_cast<u32>(MAXWELL3D_REG_INDEX(index_buffer.limit_addr_high)),
            static_cast<u32>((Ch0IndexBufAddr + 0x100ULL) >> 32));
        AppendEngineMethod(
            ch0_follow_cmds, Sub3D,
            static_cast<u32>(MAXWELL3D_REG_INDEX(index_buffer.limit_addr_low)),
            static_cast<u32>((Ch0IndexBufAddr + 0x100ULL) & 0xFFFFFFFFULL));
        AppendEngineMethod(ch0_follow_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(index_buffer.format)),
                           static_cast<u32>(Maxwell3D::Regs::IndexFormat::UnsignedInt));
        AppendEngineMethod(ch0_follow_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(index_buffer.first)), 0U);
        AppendEngineMethod(ch0_follow_cmds, Sub3D,
                           static_cast<u32>(MAXWELL3D_REG_INDEX(index_buffer.count)), 6U);
        AppendMaxwellSemaphoreRelease(ch0_follow_cmds, Sub3D, Ch0DeferredQueryAddr, 0xBBB00002U,
                                      false);
        AppendSyncpointIncrement(ch0_follow_cmds, 3U);

        gpu.PushGPUEntries(ch0->bind_id, Tegra::CommandList{std::move(ch0_follow_cmds)});
    }

    gpu.WaitForIdle();

    // Capture StateTracker dirty flag snapshots across both paths.
    trace.vk_flag_snapshots.push_back(rasterizer.VkTracker().ChannelBridge().DirtyFlagsRef());

    // -------------------------------------------------------------------------
    // Phase II: Direct Engine + Deferred Draw + DmaPusher Parity
    // -------------------------------------------------------------------------
    gpu.BindChannel(ch0->bind_id);

    auto bridge0_mut = Tegra::Control::ChannelStateBridge{*ch0};
    auto bridge1_mut = Tegra::Control::ChannelStateBridge{*ch1};

    Maxwell3D::Regs::VertexArray deferred_va{};
    deferred_va.start.Assign(2U);
    deferred_va.count.Assign(4U);
    deferred_va.topology.Assign(Maxwell3D::Regs::PrimitiveTopology::Triangles);
    u32 deferred_va_word = 0;
    std::memcpy(&deferred_va_word, &deferred_va, sizeof(u32));

    const GPUVAddr phase2_sem_addr = Ch0DeferredQueryAddr + 0x40ULL;
    const std::array<u32, 4> phase2_sem_words{
        static_cast<u32>(phase2_sem_addr >> 32),
        static_cast<u32>(phase2_sem_addr & 0xFFFFFFFFULL),
        0xCCC00003U,
        static_cast<u32>(Maxwell3D::Regs::ReportSemaphore::Operation::Release) | (1U << 28),
    };

    if (mode == ReplayMode::ChannelStateBridge) {
        bridge0_mut.SetDirty(Vulkan::Dirty::LineWidth);
        bridge0_mut.CallMaxwellMethod(system,
                                      static_cast<u32>(MAXWELL3D_REG_INDEX(color_mask_common)),
                                      0xFU);
        // Deferred instanced draw loop flushed through ChannelStateBridge::FlushDeferredDraw
        bridge0_mut.CallMaxwellMethod(
            system, static_cast<u32>(MAXWELL3D_REG_INDEX(vertex_array_instance_first)),
            deferred_va_word);
        bridge0_mut.CallMaxwellMethod(
            system, static_cast<u32>(MAXWELL3D_REG_INDEX(vertex_array_instance_subsequent)),
            deferred_va_word);
        REQUIRE(bridge0_mut.HasDeferredDraw());
        bridge0_mut.FlushDeferredDraw();
        REQUIRE_FALSE(bridge0_mut.HasDeferredDraw());

        for (std::size_t idx = 0; idx < phase2_sem_words.size(); ++idx) {
            bridge0_mut.CallMaxwellMethod(
                system,
                static_cast<u32>(MAXWELL3D_REG_INDEX(report_semaphore.address_high) + idx),
                phase2_sem_words[idx]);
        }
    } else {
        auto& m3d0 = ch0->payload->maxwell_3d;
        m3d0.dirty.flags[Vulkan::Dirty::LineWidth] = true;
        m3d0.CallMethod(system, static_cast<u32>(MAXWELL3D_REG_INDEX(color_mask_common)), 0xFU,
                        true);
        m3d0.CallMethod(system,
                        static_cast<u32>(MAXWELL3D_REG_INDEX(vertex_array_instance_first)),
                        deferred_va_word, true);
        m3d0.CallMethod(system,
                        static_cast<u32>(MAXWELL3D_REG_INDEX(vertex_array_instance_subsequent)),
                        deferred_va_word, true);
        REQUIRE(m3d0.draw_manager.HasDeferredDraw());
        m3d0.draw_manager.DrawDeferred(m3d0);
        REQUIRE_FALSE(m3d0.draw_manager.HasDeferredDraw());

        for (std::size_t idx = 0; idx < phase2_sem_words.size(); ++idx) {
            m3d0.CallMethod(
                system,
                static_cast<u32>(MAXWELL3D_REG_INDEX(report_semaphore.address_high) + idx),
                phase2_sem_words[idx], true);
        }
    }

    // Direct cross-channel DmaPusher acquire -> wake -> dispatch on Switch 2
    if (is_switch2) {
        boost::container::small_vector<Tegra::CommandHeader, 512> direct_wait_cmds;
        AppendSemaphoreAcquireEqual(direct_wait_cmds, SharedDirectAcquireAddr, 0xFACE0001U);
        AppendSemaphoreWrite(direct_wait_cmds, SharedDirectDoneAddr, 0xFACE0002U, true);
        AppendSyncpointIncrement(direct_wait_cmds, 4U);

        const GPUVAddr phase2_report_addr = Ch1ReportSemAddr + 0x40ULL;
        const GPUVAddr phase2_qmd_addr = Ch1NativeQmdAddr + 0x100ULL;
        const auto phase2_qmd_words =
            MakeNativeInlineQmdWords(Ch1NativeProgAddr + 0x100ULL, 2U, 1U, 1U, 32U, 2U, 1U);

        if (mode == ReplayMode::ChannelStateBridge) {
            bridge0_mut.PushCommands(Tegra::CommandList{std::move(direct_wait_cmds)});
            bridge0_mut.DispatchCommands();
            REQUIRE(bridge0_mut.HasPendingAcquire());
            REQUIRE_FALSE(bridge0_mut.CanResumeAcquire());
            trace.log.RecordSync(SyncEventRecord{
                .kind = SyncEventKind::AcquireSuspended,
                .channel_id = bridge0_mut.BindId(),
                .id_or_tag = 2U,
                .gpu_addr = SharedDirectAcquireAddr,
                .value = 0xFACE0001U,
            });

            // Channel 1 writes the waking value via bridge1_mut's MemoryManager, dispatches a
            // direct inline QMD, and signals report.
            gpu.BindChannel(ch1->bind_id);
            bridge1_mut.GpuMemory().Write<u32>(SharedDirectAcquireAddr, 0xFACE0001U);
            bridge1_mut.CallComputeMethod(system, 0xA7U, 0U);
            bridge1_mut.CallComputeMethod(
                system, 0xC6U, static_cast<u32>((phase2_qmd_addr >> 8) >> 32));
            bridge1_mut.CallComputeMethod(
                system, 0xC7U, static_cast<u32>((phase2_qmd_addr >> 8) & 0xFFFFFFFFULL));
            for (std::size_t idx = 0; idx < phase2_qmd_words.size(); ++idx) {
                bridge1_mut.CallComputeMethod(
                    system, static_cast<u32>(0xC8U + idx), phase2_qmd_words[idx]);
            }
            bridge1_mut.CallComputeMethod(
                system, 0x6C0U, static_cast<u32>((phase2_report_addr >> 32) & 0x1FFFFFFU));
            bridge1_mut.CallComputeMethod(
                system, 0x6C1U, static_cast<u32>(phase2_report_addr & 0xFFFFFFFFULL));
            bridge1_mut.CallComputeMethod(system, 0x6C2U, 0xBEEF0001U);
            bridge1_mut.CallComputeMethod(system, 0x6C3U, (1U << 28));

            gpu.BindChannel(ch0->bind_id);
            REQUIRE(bridge0_mut.CanResumeAcquire());
            bridge0_mut.DispatchCommands();
            REQUIRE_FALSE(bridge0_mut.HasPendingAcquire());
            trace.log.RecordSync(SyncEventRecord{
                .kind = SyncEventKind::AcquireResumed,
                .channel_id = bridge0_mut.BindId(),
                .id_or_tag = 2U,
                .gpu_addr = SharedDirectAcquireAddr,
                .value = 0xFACE0001U,
            });
        } else {
            ch0->payload->dma_pusher.Push(Tegra::CommandList{std::move(direct_wait_cmds)});
            ch0->payload->dma_pusher.DispatchCalls();
            REQUIRE(ch0->payload->dma_pusher.HasPendingAcquire());
            REQUIRE_FALSE(ch0->payload->dma_pusher.CanResumeAcquire());
            trace.log.RecordSync(SyncEventRecord{
                .kind = SyncEventKind::AcquireSuspended,
                .channel_id = ch0->bind_id,
                .id_or_tag = 2U,
                .gpu_addr = SharedDirectAcquireAddr,
                .value = 0xFACE0001U,
            });

            gpu.BindChannel(ch1->bind_id);
            ch1->memory_manager->Write<u32>(SharedDirectAcquireAddr, 0xFACE0001U);
            ch1->payload->kepler_compute.CallMethod(system, 0xA7U, 0U, true);
            ch1->payload->kepler_compute.CallMethod(
                system, 0xC6U, static_cast<u32>((phase2_qmd_addr >> 8) >> 32), true);
            ch1->payload->kepler_compute.CallMethod(
                system, 0xC7U, static_cast<u32>((phase2_qmd_addr >> 8) & 0xFFFFFFFFULL), true);
            for (std::size_t idx = 0; idx < phase2_qmd_words.size(); ++idx) {
                ch1->payload->kepler_compute.CallMethod(
                    system, static_cast<u32>(0xC8U + idx), phase2_qmd_words[idx], true);
            }
            ch1->payload->kepler_compute.CallMethod(
                system, 0x6C0U, static_cast<u32>((phase2_report_addr >> 32) & 0x1FFFFFFU), true);
            ch1->payload->kepler_compute.CallMethod(
                system, 0x6C1U, static_cast<u32>(phase2_report_addr & 0xFFFFFFFFULL), true);
            ch1->payload->kepler_compute.CallMethod(system, 0x6C2U, 0xBEEF0001U, true);
            ch1->payload->kepler_compute.CallMethod(system, 0x6C3U, (1U << 28), true);

            gpu.BindChannel(ch0->bind_id);
            REQUIRE(ch0->payload->dma_pusher.CanResumeAcquire());
            ch0->payload->dma_pusher.DispatchCalls();
            REQUIRE_FALSE(ch0->payload->dma_pusher.HasPendingAcquire());
            trace.log.RecordSync(SyncEventRecord{
                .kind = SyncEventKind::AcquireResumed,
                .channel_id = ch0->bind_id,
                .id_or_tag = 2U,
                .gpu_addr = SharedDirectAcquireAddr,
                .value = 0xFACE0001U,
            });
        }
    }

    trace.vk_flag_snapshots.push_back(rasterizer.VkTracker().ChannelBridge().DirtyFlagsRef());

#ifdef HAS_OPENGL
    // Exercise OpenGL::StateTracker table + notify + channel-switch parity on isolated dirty state.
    Tegra::Engines::Maxwell3D::DirtyState::Flags gl_flags_saved{};
    {
        auto gl_flags_orig = ch0->payload->maxwell_3d.dirty.flags;
        auto gl_tables_orig = ch0->payload->maxwell_3d.dirty.tables;
        SCOPE_EXIT {
            ch0->payload->maxwell_3d.dirty.flags = gl_flags_orig;
            ch0->payload->maxwell_3d.dirty.tables = gl_tables_orig;
        };

        ch0->payload->maxwell_3d.dirty.flags.reset();
        OpenGL::StateTracker gl_tracker;
        if (mode == ReplayMode::ChannelStateBridge) {
            gl_tracker.SetupTables(bridge0_mut);
            gl_tracker.ChangeChannel(bridge0_mut);
            bridge0_mut.ApplyDirtyTables(static_cast<u32>(MAXWELL3D_REG_INDEX(line_width_smooth)));
        } else {
            gl_tracker.SetupTables(*ch0);
            gl_tracker.ChangeChannel(*ch0);
            for (const auto& table : ch0->payload->maxwell_3d.dirty.tables) {
                ch0->payload->maxwell_3d.dirty.flags
                    [table[MAXWELL3D_REG_INDEX(line_width_smooth)]] = true;
            }
        }
        gl_tracker.NotifyViewport0();
        gl_tracker.NotifyColorMask(2);
        gl_tracker.NotifyFramebuffer();
        gl_tracker.NotifyRange(250, 255);
        REQUIRE(gl_tracker.ChannelBridge().HasDirtyTables());
        gl_flags_saved = gl_tracker.ChannelBridge().DirtyFlagsRef();
        gl_tracker.ClearChannel();
        REQUIRE_FALSE(gl_tracker.ChannelBridge().HasMaxwell3D());
    }
    trace.gl_flag_snapshots.push_back(gl_flags_saved);
#endif

    // Snapshot guest memory contents across shared + per-channel regions.
    trace.shared_memory.resize(SharedSize);
    trace.ch0_memory.resize(Ch0Size);
    trace.ch1_memory.resize(Ch1Size);
    mem0->ReadBlockUnsafe(SharedGpuBase, trace.shared_memory.data(), trace.shared_memory.size());
    mem0->ReadBlockUnsafe(Ch0GpuBase, trace.ch0_memory.data(), trace.ch0_memory.size());
    mem1->ReadBlockUnsafe(Ch1GpuBase, trace.ch1_memory.data(), trace.ch1_memory.size());

    const auto& sp_mgr = system.Host1x().GetSyncpointManager();
    for (u32 sp_id = 0; sp_id < static_cast<u32>(trace.syncpoint_values.size()); ++sp_id) {
        trace.syncpoint_values[sp_id] = sp_mgr.GetGuestSyncpointValue(sp_id);
    }

    // Exercise channel release & ClearChannel path.
    rasterizer.ReleaseChannel(ch1->bind_id);
    rasterizer.ReleaseChannel(ch0->bind_id);
}

void AssertFullParity(const WorkloadTrace& legacy, const WorkloadTrace& bridged) {
    // 1. Ordered draw calls
    REQUIRE(legacy.log.draws == bridged.log.draws);

    // 2. Ordered compute dispatches
    REQUIRE(legacy.log.computes == bridged.log.computes);

    // 3. Ordered semaphore releases
    REQUIRE(legacy.log.semaphores == bridged.log.semaphores);

    // 4. Resulting guest-memory writes (ordered write stream + full memory snapshots)
    REQUIRE(legacy.log.memory_writes == bridged.log.memory_writes);
    REQUIRE(legacy.shared_memory == bridged.shared_memory);
    REQUIRE(legacy.ch0_memory == bridged.ch0_memory);
    REQUIRE(legacy.ch1_memory == bridged.ch1_memory);

    // 5. Cross-channel wait / synchronization ordering & global interleaved timeline
    REQUIRE(legacy.log.sync_events == bridged.log.sync_events);
    REQUIRE(legacy.log.timeline == bridged.log.timeline);
    REQUIRE(legacy.syncpoint_values == bridged.syncpoint_values);

    // State tracker parity
    REQUIRE(legacy.vk_flag_snapshots == bridged.vk_flag_snapshots);
#ifdef HAS_OPENGL
    REQUIRE(legacy.gl_flag_snapshots == bridged.gl_flag_snapshots);
#endif
}

template <typename ChannelP>
class TypedSetupCache final : public VideoCommon::ChannelSetupCaches<ChannelP> {
public:
    [[nodiscard]] Tegra::Control::ChannelStateBridge Active() const noexcept {
        return this->CurrentBridge();
    }
    [[nodiscard]] std::optional<Tegra::Control::ChannelStateBridge> Find(s32 id) const {
        return this->FindBridge(id);
    }
    [[nodiscard]] std::size_t AsCount() const noexcept {
        return this->address_spaces.size();
    }
};

} // namespace

TEST_CASE("ChannelStateBridge: value semantics, dirty tables, and cache/tracker bindings",
          "[video_core][channel-bridge-parity]") {
    using Bridge = Tegra::Control::ChannelStateBridge;
    static_assert(std::is_trivially_copyable_v<Bridge>);
    static_assert(std::is_trivially_destructible_v<Bridge>);

    Bridge empty_bridge;
    REQUIRE_FALSE(empty_bridge.IsBound());
    REQUIRE_FALSE(static_cast<bool>(empty_bridge));
    REQUIRE_FALSE(empty_bridge.HasChannelBindings());
    REQUIRE_FALSE(empty_bridge.HasChannelState());
    REQUIRE_FALSE(empty_bridge.HasDirtyFlags());
    REQUIRE_FALSE(empty_bridge.HasDirtyTables());
    REQUIRE(empty_bridge.BindId() == Bridge::UnboundChannelId);
    REQUIRE(empty_bridge.ProgramId() == 0);

    // Safe no-ops when unbound
    empty_bridge.SetDirty(5);
    empty_bridge.SetDirtyRange(10, 20);
    REQUIRE_FALSE(empty_bridge.IsDirty(5));
    REQUIRE_FALSE(empty_bridge.ExchangeDirty(5, true));
    empty_bridge.SetAllDirtyFlags();
    empty_bridge.ResetDirtyFlags();
    empty_bridge.SetupDirtyTables();
    empty_bridge.ApplyDirtyTables(0);
    REQUIRE(empty_bridge.DirtyTableEntry(0, 0) == 0);

    // Dirty flags/tables mode parity
    Bridge::DirtyFlags legacy_flags{};
    Bridge::DirtyFlags bridged_flags{};
    Bridge::DirtyTables legacy_tables{};
    Bridge::DirtyTables bridged_tables{};

    VideoCommon::Dirty::SetupDirtyFlags(legacy_tables);
    auto dirty_bridge = Bridge::FromDirtyFlags(bridged_flags, &bridged_tables);
    REQUIRE(dirty_bridge.IsBound());
    REQUIRE(dirty_bridge.HasDirtyFlags());
    REQUIRE(dirty_bridge.HasDirtyTables());
    dirty_bridge.SetupDirtyTables();
    REQUIRE(legacy_tables == bridged_tables);

    // Range clamp safety up to NumDirtyFlags - 1
    dirty_bridge.SetDirtyRange(250, 300);
    for (std::size_t idx = 250; idx < Bridge::NumDirtyFlags; ++idx) {
        REQUIRE(bridged_flags[idx]);
    }
    REQUIRE(dirty_bridge.ExchangeDirty(252, false));
    REQUIRE_FALSE(bridged_flags[252]);

    // Vulkan::StateTracker standalone dirty-table parity
    Vulkan::StateTracker vk_legacy;
    Vulkan::StateTracker vk_bridged;
    vk_legacy.InvalidateViewports();
    vk_bridged.InvalidateViewports();
    REQUIRE(vk_legacy.TouchViewports() == vk_bridged.TouchViewports());
    REQUIRE(vk_legacy.ChannelBridge().DirtyFlagsRef() ==
            vk_bridged.ChannelBridge().DirtyFlagsRef());

    // Standalone engine-backed bridge (Maxwell3D + KeplerCompute + MemoryManager without ChannelState)
    const ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    system.Kernel().Initialize();
    SCOPE_EXIT {
        system.ShutdownMainProcess();
    };

    {
        Tegra::MaxwellDeviceMemoryManager device_mem{system.DeviceMemory()};
    Tegra::MemoryManager gpu_mem_a{system, device_mem, 32, 1ULL << 31, 16};
    Tegra::MemoryManager gpu_mem_b{system, device_mem, 32, 1ULL << 31, 16};

    Tegra::Engines::Maxwell3D maxwell_a{gpu_mem_a};
    Tegra::Engines::KeplerCompute compute_a{gpu_mem_a};
    Tegra::Engines::Maxwell3D maxwell_b{gpu_mem_b};
    Tegra::Engines::KeplerCompute compute_b{gpu_mem_b};

    class TestSetupCache final : public VideoCommon::ChannelSetupCaches<VideoCommon::ChannelInfo> {
    public:
        using ChannelSetupCaches::ChannelSetupCaches;

        [[nodiscard]] Tegra::Control::ChannelStateBridge Active() const noexcept {
            return CurrentBridge();
        }
        [[nodiscard]] std::optional<Tegra::Control::ChannelStateBridge> Find(s32 id) const {
            return FindBridge(id);
        }
        [[nodiscard]] std::size_t AsCount() const noexcept {
            return address_spaces.size();
        }
        [[nodiscard]] std::size_t StorageSize() const noexcept {
            return channel_storage.size();
        }
        [[nodiscard]] bool HasStorageSlot(std::size_t slot) const noexcept {
            return slot < channel_storage.size() && channel_storage[slot].has_value();
        }
        [[nodiscard]] std::optional<std::size_t> StorageSlotForBindId(s32 id) const {
            std::unique_lock<std::mutex> lk(config_mutex);
            const auto it = channel_map.find(id);
            if (it == channel_map.end()) {
                return std::nullopt;
            }
            return it->second;
        }
        [[nodiscard]] std::size_t ActiveChannelCount() const noexcept {
            return active_channel_ids.size();
        }
        void ClearActiveSlotBridgeForFallbackTest() {
            if (channel_state != nullptr) {
                channel_state->Bridge().Reset();
            }
        }
    };

    class OverrideSetupCache final : public VideoCommon::ChannelSetupCaches<VideoCommon::ChannelInfo> {
    public:
        using ChannelSetupCaches::ChannelSetupCaches;

        void CreateChannelFromBridge(const Tegra::Control::ChannelStateBridge& bridge) override {
            ++create_calls;
            ChannelSetupCaches::CreateChannelFromBridge(bridge);
        }

        [[nodiscard]] std::optional<Tegra::Control::ChannelStateBridge> Find(s32 id) const {
            return FindBridge(id);
        }

        std::size_t create_calls{};
    };

    static_assert(
        std::is_constructible_v<VideoCommon::BufferCacheChannelInfo, const Bridge&>);
    static_assert(
        std::is_constructible_v<VideoCommon::TextureCacheChannelInfo, const Bridge&>);

    // Const overloads + GraphicsProgramAddress (Switch 1 and native Ampere)
    const bool prev_native_turing = maxwell_a.GetNativeMacroEngine().native_turing;
    SCOPE_EXIT {
        maxwell_a.GetNativeMacroEngine().native_turing = prev_native_turing;
    };
    const auto vertex_b_stage = static_cast<std::size_t>(Bridge::ShaderType::VertexB);
    const auto pixel_stage = static_cast<std::size_t>(Bridge::ShaderType::Pixel);
    maxwell_a.regs.program_region.address_high = 0x12U;
    maxwell_a.regs.program_region.address_low = 0x34000000U;
    maxwell_a.regs.pipelines[vertex_b_stage].offset = 0x5600U;
    maxwell_a.regs.pipelines[pixel_stage].enable.Assign(1U);
    maxwell_a.regs.pipelines[pixel_stage].offset = 0x7800U;
    maxwell_a.regs.pipelines[pixel_stage].reserved[0] = 0xDEAD00ABU;
    maxwell_a.regs.pipelines[pixel_stage].reserved[1] = 0xCDEF1234U;

    Bridge prog_bridge{maxwell_a, compute_a, gpu_mem_a, 0xAAAAULL, 40};
    const Bridge& const_prog_bridge = prog_bridge;
    static_assert(std::is_same_v<decltype(prog_bridge.Maxwell3D()), Tegra::Engines::Maxwell3D&>);
    static_assert(
        std::is_same_v<decltype(const_prog_bridge.Maxwell3D()), const Tegra::Engines::Maxwell3D&>);
    static_assert(
        std::is_same_v<decltype(prog_bridge.KeplerCompute()), Tegra::Engines::KeplerCompute&>);
    static_assert(std::is_same_v<decltype(const_prog_bridge.KeplerCompute()),
                                 const Tegra::Engines::KeplerCompute&>);
    static_assert(std::is_same_v<decltype(prog_bridge.GpuMemory()), Tegra::MemoryManager&>);
    static_assert(
        std::is_same_v<decltype(const_prog_bridge.GpuMemory()), const Tegra::MemoryManager&>);
    static_assert(
        std::is_same_v<decltype(prog_bridge.DirtyFlagsRef()), Bridge::DirtyFlags&>);
    static_assert(
        std::is_same_v<decltype(const_prog_bridge.DirtyFlagsRef()), const Bridge::DirtyFlags&>);
    static_assert(
        std::is_same_v<decltype(prog_bridge.DirtyTablesRef()), Bridge::DirtyTables&>);
    static_assert(
        std::is_same_v<decltype(const_prog_bridge.DirtyTablesRef()), const Bridge::DirtyTables&>);
    static_assert(std::is_same_v<decltype(prog_bridge.MaxwellRegsRef()),
                                 Tegra::Engines::Maxwell3D::Regs&>);
    static_assert(std::is_same_v<decltype(const_prog_bridge.MaxwellRegsRef()),
                                 const Tegra::Engines::Maxwell3D::Regs&>);
    static_assert(
        std::is_same_v<decltype(prog_bridge.MaxwellDrawManager()), Bridge::DrawManager&>);
    static_assert(std::is_same_v<decltype(const_prog_bridge.MaxwellDrawManager()),
                                 const Bridge::DrawManager&>);
    static_assert(std::is_same_v<decltype(prog_bridge.ComputeRegsRef()),
                                 Tegra::Engines::KeplerCompute::Regs&>);
    static_assert(std::is_same_v<decltype(const_prog_bridge.ComputeRegsRef()),
                                 const Tegra::Engines::KeplerCompute::Regs&>);

    REQUIRE(&const_prog_bridge.Maxwell3D() == &prog_bridge.Maxwell3D());
    REQUIRE(&const_prog_bridge.KeplerCompute() == &prog_bridge.KeplerCompute());
    REQUIRE(&const_prog_bridge.GpuMemory() == &prog_bridge.GpuMemory());
    REQUIRE(&const_prog_bridge.DirtyFlagsRef() == &prog_bridge.DirtyFlagsRef());
    REQUIRE(&const_prog_bridge.DirtyTablesRef() == &prog_bridge.DirtyTablesRef());
    REQUIRE(&const_prog_bridge.MaxwellRegsRef() == &maxwell_a.regs);
    REQUIRE(&const_prog_bridge.MaxwellDrawManager() == &maxwell_a.draw_manager);
    REQUIRE(&const_prog_bridge.ComputeRegsRef() == &compute_a.regs);

    maxwell_a.GetNativeMacroEngine().native_turing = false;
    REQUIRE_FALSE(const_prog_bridge.IsNativeAmpere());
    REQUIRE(const_prog_bridge.GraphicsProgramAddress(vertex_b_stage) == 0x1234005600ULL);
    REQUIRE(const_prog_bridge.GraphicsProgramAddress(Bridge::ShaderType::VertexB) ==
            0x1234005600ULL);
    REQUIRE(const_prog_bridge.GraphicsProgramAddress(pixel_stage) == 0x1234007800ULL);
    REQUIRE(const_prog_bridge.GraphicsProgramAddress(Bridge::ShaderType::Pixel) ==
            0x1234007800ULL);
    REQUIRE(const_prog_bridge.IsShaderConfigEnabled(Bridge::ShaderType::VertexB));
    REQUIRE(const_prog_bridge.IsShaderConfigEnabled(Bridge::ShaderType::Pixel));
    REQUIRE_FALSE(const_prog_bridge.IsShaderConfigEnabled(Bridge::ShaderType::Geometry));

    maxwell_a.GetNativeMacroEngine().native_turing = true;
    REQUIRE(const_prog_bridge.IsNativeAmpere());
    REQUIRE(const_prog_bridge.GraphicsProgramAddress(pixel_stage) == 0x000000ABCDEF1234ULL);
    REQUIRE(const_prog_bridge.GraphicsProgramAddress(Bridge::ShaderType::Pixel) ==
            0x000000ABCDEF1234ULL);
    REQUIRE(const_prog_bridge.GraphicsProgramAddress(Tegra::Engines::Maxwell3D::Regs::MaxShaderProgram) ==
            0);
    REQUIRE_FALSE(
        const_prog_bridge.IsShaderConfigEnabled(Tegra::Engines::Maxwell3D::Regs::MaxShaderProgram));
    REQUIRE(empty_bridge.GraphicsProgramAddress(Bridge::ShaderType::Pixel) == 0);
    REQUIRE_FALSE(empty_bridge.IsShaderConfigEnabled(Bridge::ShaderType::VertexB));
    maxwell_a.GetNativeMacroEngine().native_turing = prev_native_turing;

    TestSetupCache setup_cache;
    const Bridge standalone_a{maxwell_a, compute_a, gpu_mem_a, 0x1234ULL, 10};
    const Bridge standalone_b{maxwell_b, compute_b, gpu_mem_b, 0x5678ULL, 20};
    setup_cache.CreateChannelFromBridge(standalone_a);
    setup_cache.CreateChannelFromBridge(standalone_b);
    REQUIRE(setup_cache.AsCount() == 2);

    // Invalid, negative bind_id, and duplicate bridges are rejected without mutating cache state.
    setup_cache.CreateChannelFromBridge(Bridge{});
    setup_cache.CreateChannelFromBridge(Bridge::FromDirtyFlags(bridged_flags));
    setup_cache.CreateChannelFromBridge(
        Bridge{maxwell_a, compute_a, gpu_mem_a, 0x1111ULL, Bridge::UnboundChannelId});
    setup_cache.CreateChannelFromBridge(Bridge{maxwell_a, compute_a, gpu_mem_a, 0x2222ULL, -42});
    setup_cache.CreateChannelFromBridge(Bridge{maxwell_b, compute_b, gpu_mem_b, 0xDEADULL, 20});
    REQUIRE(setup_cache.StorageSize() == 2);
    REQUIRE(setup_cache.ActiveChannelCount() == 2);
    REQUIRE(setup_cache.AsCount() == 2);
    REQUIRE(setup_cache.Find(20).has_value());
    REQUIRE(setup_cache.Find(20)->ProgramId() == 0x5678ULL);

    setup_cache.BindToChannel(10);
    REQUIRE(setup_cache.Active().Maxwell3DPtr() == &maxwell_a);
    REQUIRE(setup_cache.Active().ProgramId() == 0x1234ULL);

    setup_cache.BindToChannel(20);
    REQUIRE(setup_cache.Active().KeplerComputePtr() == &compute_b);
    REQUIRE(setup_cache.Active().GpuMemoryPtr() == &gpu_mem_b);

    const auto slot10 = setup_cache.StorageSlotForBindId(10);
    REQUIRE(slot10.has_value());
    REQUIRE(setup_cache.HasStorageSlot(*slot10));

    setup_cache.EraseChannel(10);
    REQUIRE_FALSE(setup_cache.HasStorageSlot(*slot10));
    REQUIRE_FALSE(setup_cache.Find(10).has_value());
    REQUIRE(setup_cache.Find(20).has_value());

    // Reuses freed std::optional slot cleanly
    const Bridge standalone_a_reused{maxwell_a, compute_a, gpu_mem_a, 0x9999ULL, 30};
    setup_cache.CreateChannelFromBridge(standalone_a_reused);
    REQUIRE(setup_cache.HasStorageSlot(*slot10));
    REQUIRE(setup_cache.Find(30).has_value());
    REQUIRE(setup_cache.Find(30)->ProgramId() == 0x9999ULL);

    // CurrentBridge() fallback when active slot's Bridge is unbound, and active channel erase
    setup_cache.BindToChannel(30);
    setup_cache.ClearActiveSlotBridgeForFallbackTest();
    REQUIRE(setup_cache.Active().IsBound());
    REQUIRE(setup_cache.Active().BindId() == 30);
    REQUIRE(setup_cache.Active().ProgramId() == 0x9999ULL);
    REQUIRE(setup_cache.Active().Maxwell3DPtr() == &maxwell_a);

    setup_cache.EraseChannel(30);
    REQUIRE_FALSE(setup_cache.Find(30).has_value());
    REQUIRE_FALSE(setup_cache.HasStorageSlot(*slot10));
    REQUIRE_FALSE(setup_cache.Active().IsBound());
    REQUIRE(setup_cache.Active().BindId() == Bridge::UnboundChannelId);

    // Virtual CreateChannelFromBridge dispatch through base ChannelSetupCaches* pointer
    OverrideSetupCache override_cache;
    VideoCommon::ChannelSetupCaches<VideoCommon::ChannelInfo>* base_cache_ptr = &override_cache;
    base_cache_ptr->CreateChannelFromBridge(standalone_a);
    REQUIRE(override_cache.create_calls == 1);
    REQUIRE(override_cache.Find(10).has_value());

    // Explicit instantiations for BufferCacheChannelInfo and TextureCacheChannelInfo
    TypedSetupCache<VideoCommon::BufferCacheChannelInfo> buffer_setup_cache;
    buffer_setup_cache.CreateChannelFromBridge(standalone_a);
    buffer_setup_cache.CreateChannelFromBridge(standalone_b);
    REQUIRE(buffer_setup_cache.AsCount() == 2);
    buffer_setup_cache.BindToChannel(10);
    REQUIRE(buffer_setup_cache.Active().BindId() == 10);
    REQUIRE(buffer_setup_cache.Active().Maxwell3DPtr() == &maxwell_a);
    REQUIRE(buffer_setup_cache.Find(20).has_value());
    buffer_setup_cache.EraseChannel(10);
    REQUIRE_FALSE(buffer_setup_cache.Active().IsBound());

    TypedSetupCache<VideoCommon::TextureCacheChannelInfo> texture_setup_cache;
    texture_setup_cache.CreateChannelFromBridge(standalone_a);
    texture_setup_cache.CreateChannelFromBridge(standalone_b);
    REQUIRE(texture_setup_cache.AsCount() == 2);
    texture_setup_cache.BindToChannel(20);
    REQUIRE(texture_setup_cache.Active().BindId() == 20);
    REQUIRE(texture_setup_cache.Active().GpuMemoryPtr() == &gpu_mem_b);
    REQUIRE(texture_setup_cache.Find(10).has_value());
    texture_setup_cache.EraseChannel(20);
    REQUIRE_FALSE(texture_setup_cache.Active().IsBound());

    // Dirty flag table setup + channel switching + ClearChannel isolation
    Bridge engine_bridge_a{maxwell_a, compute_a, gpu_mem_a, 0x1234ULL, 10};
    Bridge engine_bridge_b{maxwell_b, compute_b, gpu_mem_b, 0x5678ULL, 20};
    vk_bridged.SetupTables(engine_bridge_b);
    vk_bridged.ChangeChannel(engine_bridge_b);
    using Tegra::Engines::Maxwell3D;
    engine_bridge_b.ApplyDirtyTables(static_cast<u32>(MAXWELL3D_REG_INDEX(line_width_smooth)));
    REQUIRE(vk_bridged.TouchLineWidth());
    REQUIRE_FALSE(vk_bridged.TouchLineWidth());

    vk_bridged.SetupTables(engine_bridge_a);
    vk_bridged.ChangeChannel(engine_bridge_a);
    maxwell_a.dirty.flags.reset();
    maxwell_b.dirty.flags.reset();
    engine_bridge_a.ApplyDirtyTables(static_cast<u32>(MAXWELL3D_REG_INDEX(line_width_smooth)));
    REQUIRE(maxwell_a.dirty.flags[Vulkan::Dirty::LineWidth]);
    REQUIRE_FALSE(maxwell_b.dirty.flags[Vulkan::Dirty::LineWidth]);
    REQUIRE(vk_bridged.TouchLineWidth());
    REQUIRE_FALSE(maxwell_a.dirty.flags[Vulkan::Dirty::LineWidth]);

    vk_bridged.ClearChannel();
    REQUIRE(vk_bridged.ChannelBridge().IsBound());
    REQUIRE_FALSE(vk_bridged.ChannelBridge().HasChannelBindings());
    vk_bridged.InvalidateState();
    REQUIRE_FALSE(maxwell_a.dirty.flags[Vulkan::Dirty::LineWidth]);
    REQUIRE_FALSE(maxwell_b.dirty.flags[Vulkan::Dirty::LineWidth]);
    REQUIRE(vk_bridged.TouchLineWidth());

    vk_bridged.ChangeChannel(engine_bridge_a);
    maxwell_a.dirty.flags.reset();
    vk_bridged.ChangeChannel(empty_bridge);
    REQUIRE_FALSE(vk_bridged.ChannelBridge().HasChannelBindings());
    vk_bridged.InvalidateState();
    REQUIRE_FALSE(maxwell_a.dirty.flags[Vulkan::Dirty::LineWidth]);

#ifdef HAS_OPENGL
    OpenGL::StateTracker gl_tracker;
    gl_tracker.SetupTables(engine_bridge_b);
    gl_tracker.ChangeChannel(engine_bridge_b);
    gl_tracker.NotifyViewport0();
    REQUIRE(gl_tracker.ChannelBridge().IsDirty(OpenGL::Dirty::Viewports));
    REQUIRE(gl_tracker.ChannelBridge().IsDirty(OpenGL::Dirty::Viewport0));
    gl_tracker.ClearChannel();
    REQUIRE_FALSE(gl_tracker.ChannelBridge().IsDirty(OpenGL::Dirty::Viewport0));
    REQUIRE_FALSE(gl_tracker.ChannelBridge().HasChannelBindings());

    gl_tracker.SetupTables(engine_bridge_a);
    gl_tracker.ChangeChannel(engine_bridge_a);
    maxwell_a.dirty.flags.reset();
    maxwell_b.dirty.flags.reset();
    gl_tracker.NotifyScissor0();
    REQUIRE(maxwell_a.dirty.flags[OpenGL::Dirty::Scissors]);
    REQUIRE(maxwell_a.dirty.flags[OpenGL::Dirty::Scissor0]);
    REQUIRE_FALSE(maxwell_b.dirty.flags[OpenGL::Dirty::Scissor0]);

    gl_tracker.NotifyRange(245, 240);
    REQUIRE_FALSE(maxwell_a.dirty.flags[240]);
    REQUIRE_FALSE(maxwell_a.dirty.flags[245]);
    gl_tracker.NotifyRange(253, 255);
    REQUIRE(maxwell_a.dirty.flags[253]);
    REQUIRE(maxwell_a.dirty.flags[254]);
    REQUIRE(maxwell_a.dirty.flags[255]);

    gl_tracker.ClearChannel();
    gl_tracker.InvalidateState();
    REQUIRE_FALSE(maxwell_a.dirty.flags[OpenGL::Dirty::PolygonModes]);
    REQUIRE_FALSE(maxwell_b.dirty.flags[OpenGL::Dirty::PolygonModes]);
    REQUIRE(gl_tracker.ChannelBridge().IsDirty(OpenGL::Dirty::PolygonModes));

    gl_tracker.ChangeChannel(engine_bridge_a);
    maxwell_a.dirty.flags.reset();
    gl_tracker.ChangeChannel(empty_bridge);
    REQUIRE_FALSE(gl_tracker.ChannelBridge().HasChannelBindings());
    gl_tracker.InvalidateState();
    REQUIRE_FALSE(maxwell_a.dirty.flags[OpenGL::Dirty::PolygonModes]);
#endif
    }
}

TEST_CASE("ChannelStateBridge: Switch 1 multi-channel GPU parity",
          "[video_core][channel-bridge-parity]") {
    WorkloadTrace legacy{};
    WorkloadTrace bridged{};
    ExecuteParityWorkload(legacy, false, ReplayMode::LegacyDirect, "sw1-legacy");
    ExecuteParityWorkload(bridged, false, ReplayMode::ChannelStateBridge, "sw1-bridge");

    REQUIRE_FALSE(legacy.log.draws.empty());
    REQUIRE_FALSE(legacy.log.computes.empty());
    REQUIRE_FALSE(legacy.log.semaphores.empty());
    REQUIRE_FALSE(legacy.log.memory_writes.empty());
    REQUIRE_FALSE(legacy.log.sync_events.empty());

    AssertFullParity(legacy, bridged);
}

TEST_CASE("ChannelStateBridge: Switch 2 native mode and cross-channel acquire suspension parity",
          "[video_core][channel-bridge-parity]") {
    WorkloadTrace legacy{};
    WorkloadTrace bridged{};
    ExecuteParityWorkload(legacy, true, ReplayMode::LegacyDirect, "sw2-legacy");
    ExecuteParityWorkload(bridged, true, ReplayMode::ChannelStateBridge, "sw2-bridge");

    REQUIRE(legacy.log.draws.size() >= 3);
    REQUIRE(legacy.log.computes.size() >= 2);
    REQUIRE(legacy.log.semaphores.size() >= 4);
    REQUIRE(legacy.log.memory_writes.size() >= 4);

    // Verify Switch 2 cross-channel acquire actually suspended ch0 before ch1 ran, then resumed ch0.
    const auto suspended_it = std::find_if(
        legacy.log.sync_events.begin(), legacy.log.sync_events.end(),
        [](const SyncEventRecord& ev) { return ev.kind == SyncEventKind::AcquireSuspended; });
    const auto resumed_it = std::find_if(
        legacy.log.sync_events.begin(), legacy.log.sync_events.end(),
        [](const SyncEventRecord& ev) { return ev.kind == SyncEventKind::AcquireResumed; });
    REQUIRE(suspended_it != legacy.log.sync_events.end());
    REQUIRE(resumed_it != legacy.log.sync_events.end());
    REQUIRE(suspended_it < resumed_it);

    AssertFullParity(legacy, bridged);
}
