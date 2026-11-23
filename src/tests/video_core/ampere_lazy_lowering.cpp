// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "common/page_table.h"
#include "common/scope_exit.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ampere/program_override.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/runtime_info.h"
#include "tests/core/system_test_context.h"
#include "video_core/control/channel_state_bridge.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/query_cache/types.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_vulkan/pipeline_optimization.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/shader_cache.h"
#include "video_core/shader_environment.h"
#include "video_core/shader_warm/warm_records.h"

#ifdef EGLANTINE_ENABLE_RUST_AMP_SPIRV
#include "eglantine_ampere_spirv.h"
#endif

namespace VideoCommon {
struct AmpereLazyLoweringTestAccess {
    static std::unique_ptr<ShaderCache> Create(Tegra::MaxwellDeviceMemoryManager& memory) {
        return std::unique_ptr<ShaderCache>{new ShaderCache{memory}};
    }

    static const ShaderInfo* RegisterFromEnv(ShaderCache& cache, GenericEnvironment& env,
                                             VAddr cpu_addr) {
        return cache.MakeShaderInfo(env, cpu_addr);
    }

    static ShaderInfo* Find(const ShaderCache& cache, VAddr cpu_addr) {
        return cache.TryGet(cpu_addr);
    }

    static bool EvictFailedLazyShader(ShaderCache& cache, const ShaderInfo* info) {
        return cache.EvictFailedLazyShader(info);
    }

    static size_t EvictFailedLazyShaders(ShaderCache& cache) {
        return cache.EvictFailedLazyShaders();
    }

    static const ShaderInfo* TryGetResolved(ShaderCache& cache, GPUVAddr shader_addr,
                                            size_t memory_id, u64 map_serial, u64 inv_serial,
                                            std::optional<VAddr>& out_cpu_addr) {
        return cache.TryGetResolved(shader_addr, memory_id, map_serial, inv_serial, out_cpu_addr);
    }

    static void CacheResolved(ShaderCache& cache, GPUVAddr shader_addr, size_t memory_id,
                              u64 map_serial, u64 inv_serial, const ShaderInfo* info) noexcept {
        cache.CacheResolved(shader_addr, memory_id, map_serial, inv_serial, info);
    }

    static u64 InvalidationSerial(const ShaderCache& cache) noexcept {
        return cache.invalidation_serial.load(std::memory_order_acquire);
    }

    static void SetShaderInfoSlot(ShaderCache& cache, size_t index, const ShaderInfo* info) {
        cache.shader_infos[index] = info;
        cache.last_shaders_valid = true;
    }

    static const ShaderInfo* GetShaderInfoSlot(const ShaderCache& cache, size_t index) {
        return cache.shader_infos[index];
    }

    static bool LastShadersValid(const ShaderCache& cache) noexcept {
        return cache.last_shaders_valid;
    }
};
} // namespace VideoCommon

namespace {

using Shader::Ampere::Instruction;
using VideoCommon::AmpereLazyLoweringTestAccess;
using VideoCommon::ComputeEnvironment;
using VideoCommon::FileEnvironment;
using VideoCommon::GenericEnvironment;
using VideoCommon::GraphicsEnvironment;
using VideoCommon::LoweredNativeCode;
using VideoCommon::LowerToMaxwellInvocationCount;
using VideoCommon::ResetLowerToMaxwellInvocationCount;
using VideoCommon::ShaderCache;
using VideoCommon::ShaderInfo;

constexpr Instruction ExitInsn{{0x0000794dU, 0U, 0x03800000U, 0U}};

// LEA.HI R4, R6, c[0x2][0x20], RZ, 4: lowered cleanly by LowerToMaxwell, declined by direct
// SM86 SSA lowering so fallback runs deterministically on demand.
constexpr Instruction ConstantLeaHiInsn{
    {0x0a11U | (7U << 12) | (4U << 16) | (6U << 24), (0x20U << 6) | (2U << 22),
     255U | (4U << 11) | (1U << 16) | (7U << 17), 0U}};

class DummyLazyTestRasterizer final : public VideoCore::RasterizerInterface {
public:
    void Draw(bool, u32) override {}
    void DrawTexture() override {}
    void Clear(u32) override {}
    void DispatchCompute() override {}
    void ResetCounter(VideoCommon::QueryType) override {}
    void Query(GPUVAddr, VideoCommon::QueryType, VideoCommon::QueryPropertiesFlags, u32,
               u32) override {}
    void BindGraphicsUniformBuffer(size_t, u32, GPUVAddr, u32) override {}
    void DisableGraphicsUniformBuffer(size_t, u32) override {}
    void SignalFence(std::function<void()>&& func, bool) override {
        func();
    }
    void SyncOperation(std::function<void()>&&) override {}
    void SignalSyncPoint(u32) override {}
    void SignalReference() override {}
    void ReleaseFences(bool) override {}
    void FlushAll() override {}
    void FlushRegion(DAddr, u64, VideoCommon::CacheType) override {}
    bool MustFlushRegion(DAddr, u64, VideoCommon::CacheType) override {
        return false;
    }
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override {
        return {addr, addr + size, false};
    }
    void InvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void OnCacheInvalidation(PAddr, u64) override {}
    bool OnCPUWrite(PAddr, u64) override {
        return false;
    }
    void InvalidateGPUCache() override {}
    void UnmapMemory(DAddr, u64) override {}
    void ModifyGPUMemory(size_t, GPUVAddr, u64) override {}
    void FlushAndInvalidateRegion(DAddr, u64, VideoCommon::CacheType) override {}
    void WaitForIdle() override {}
    void FragmentBarrier() override {}
    void TiledCacheBarrier() override {}
    void FlushCommands() override {}
    void TickFrame() override {}
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override {
        std::terminate();
    }
    void AccelerateInlineToMemory(GPUVAddr, size_t, std::span<const u8>) override {}
};

struct InitializedSystem {
    Core::System system;
    InitializedSystem() {
        system.Initialize();
    }
};

struct LazyLoweringTestFixture {
    static constexpr GPUVAddr MappedBase = 0x2000'0000ULL;
    static constexpr DAddr DeviceBase = 0x1000'0000ULL;
    static constexpr PAddr PhysicalBase = 0x4000ULL;
    static constexpr size_t PageSize = 0x1000;

    static constexpr GPUVAddr DirectComputeOffset = 0x000ULL;
    static constexpr GPUVAddr FallbackComputeOffset = 0x100ULL;
    static constexpr GPUVAddr VertexShaderOffset = 0x200ULL;
    static constexpr GPUVAddr FragmentShaderOffset = 0x400ULL;
    static constexpr GPUVAddr Cbuf2Offset = 0x800ULL;

    ScopedSystemTestContext context;
    InitializedSystem init_system;
    Core::System& system{init_system.system};
    Core::DeviceMemory physical;
    DummyLazyTestRasterizer rasterizer;
    Tegra::MaxwellDeviceMemoryManager device_memory{physical};
    Kernel::KProcess process{system.Kernel()};
    Core::Asid asid{};
    Tegra::MemoryManager memory{system, device_memory, 32, 1ULL << 31, 16};
    Tegra::Engines::KeplerCompute compute{memory};
    Tegra::Engines::Maxwell3D maxwell3d{memory};
    Tegra::MacroEngine compute_macro{true};
    std::array<u32, 0x80> compute_macro_positions{};

    LazyLoweringTestFixture() {
        device_memory.BindInterface(&rasterizer);
        auto& table = process.GetPageTable().GetImpl();
        table.Resize(20, 12);
        u8* const backing = physical.GetPointerFromRaw<u8>(0x10000);
        std::memset(backing, 0, PageSize);
        table.entries.GetAndFault(4).Store(false, Common::PageType::Memory, 1,
                                         reinterpret_cast<uintptr_t>(backing) - PhysicalBase);
        process.GetMemory().SetCurrentPageTable(process);
        asid = device_memory.RegisterProcess(&process.GetMemory());
        device_memory.Map(DeviceBase, PhysicalBase, PageSize, asid);
        memory.BindRasterizer(&rasterizer);
        memory.Map(MappedBase, DeviceBase, PageSize, Tegra::PTEKind::PITCH, false);

        compute.BindRasterizer(&rasterizer);
        compute.SetNativeMacroMode(&compute_macro, compute_macro_positions);
        compute.regs.code_loc.address_high = static_cast<u32>(MappedBase >> 32);
        compute.regs.code_loc.address_low = static_cast<u32>(MappedBase);
        compute.regs.tex_cb_index = 0;
        compute.launch_description.block_dim_x.Assign(1U);
        compute.launch_description.block_dim_y.Assign(1U);
        compute.launch_description.block_dim_z.Assign(1U);
        compute.launch_description.const_buffer_enable_mask.Assign(1U << 2);
        const GPUVAddr cbuf2_addr = MappedBase + Cbuf2Offset;
        compute.launch_description.const_buffer_config[2].address_low =
            static_cast<u32>(cbuf2_addr);
        compute.launch_description.const_buffer_config[2].address_high.Assign(
            static_cast<u32>(cbuf2_addr >> 32));
        compute.launch_description.const_buffer_config[2].size.Assign(0x100U);

        maxwell3d.BindRasterizer(&rasterizer);
        maxwell3d.SetNativeTuringMacroMode(true);
        maxwell3d.regs.bindless_texture_const_buffer_slot = 0;
    }

    ~LazyLoweringTestFixture() {
        device_memory.Unmap(DeviceBase, PageSize);
        device_memory.UnregisterProcess(asid);
        system.Kernel().UnregisterKernelObject(&process);
    }

    void WriteComputeProgram(GPUVAddr offset, std::span<const Instruction> instructions) {
        memory.WriteBlock(MappedBase + offset, instructions.data(), instructions.size_bytes());
    }

    void WriteGraphicsProgram(GPUVAddr offset, std::span<const Instruction> instructions) {
        const std::array<u8, 128> header{};
        memory.WriteBlock(MappedBase + offset, header.data(), header.size());
        memory.WriteBlock(MappedBase + offset + header.size(), instructions.data(),
                          instructions.size_bytes());
    }
};

Shader::Profile MakeSpirv16Profile() {
    Shader::Profile profile;
    profile.supported_spirv = 0x00010600U;
    profile.unified_descriptor_binding = true;
    profile.support_int8 = true;
    profile.support_int16 = true;
    profile.support_int64 = true;
    return profile;
}

Shader::HostTranslateInfo MakeHostInfo() {
    Shader::HostTranslateInfo host_info;
    host_info.support_int64 = true;
    host_info.support_float16 = true;
    host_info.support_float64 = true;
    host_info.support_viewport_index_layer = true;
    host_info.ApplyDescriptorLimitPolicy();
    return host_info;
}

TEST_CASE("Cold direct SM86 compute discovery and compilation run with zero LowerToMaxwell calls",
          "[video_core][ampere-lazy-lowering]") {
    LazyLoweringTestFixture fix;
    const std::array direct_cs{ExitInsn};
    fix.WriteComputeProgram(fix.DirectComputeOffset, direct_cs);
    fix.WriteGraphicsProgram(fix.VertexShaderOffset, direct_cs);
    fix.WriteGraphicsProgram(fix.FragmentShaderOffset, direct_cs);

    Vulkan::SetDirectSm86EnabledOverride(true);
    SCOPE_EXIT { Vulkan::SetDirectSm86EnabledOverride(std::nullopt); };
    ResetLowerToMaxwellInvocationCount();

    auto cache = AmpereLazyLoweringTestAccess::Create(fix.device_memory);

    ComputeEnvironment cs_discovery{fix.compute, fix.memory,
                                    fix.MappedBase + fix.DirectComputeOffset, 0};
    const VAddr cs_cpu = *fix.memory.GpuToCpuAddress(fix.MappedBase + fix.DirectComputeOffset);
    const ShaderInfo* const cs_info =
        AmpereLazyLoweringTestAccess::RegisterFromEnv(*cache, cs_discovery, cs_cpu);

    REQUIRE(cs_info != nullptr);
    REQUIRE(cs_info->lowered_native != nullptr);
    CHECK(LowerToMaxwellInvocationCount() == 0);
    CHECK(cs_info->HasNativeCode());
    CHECK(cs_info->NativeProgramKey() != 0);
    CHECK(cs_info->unique_hash != 0);
    CHECK(cs_info->size_bytes == sizeof(Instruction));
    CHECK_FALSE(cs_info->lowered_native->HasLoweredCode());
    CHECK(AmpereLazyLoweringTestAccess::Find(*cache, cs_cpu) == cs_info);

    // Non-paired graphics discovery also captures SM86 instructions without lowering.
    GraphicsEnvironment vs_discovery{
        fix.maxwell3d, fix.memory, Tegra::Engines::Maxwell3D::Regs::ShaderType::VertexB,
        fix.MappedBase + fix.VertexShaderOffset, 0};
    GraphicsEnvironment fs_discovery{
        fix.maxwell3d, fix.memory, Tegra::Engines::Maxwell3D::Regs::ShaderType::Pixel,
        fix.MappedBase + fix.FragmentShaderOffset, 0};
    CHECK(LowerToMaxwellInvocationCount() == 0);
    CHECK(vs_discovery.IsNativeProgram());
    CHECK(fs_discovery.IsNativeProgram());
    CHECK_FALSE(vs_discovery.HasLoweredNativeCode());
    CHECK_FALSE(fs_discovery.HasLoweredNativeCode());

#ifdef EGLANTINE_ENABLE_RUST_AMP_SPIRV
    ComputeEnvironment cs_compile{fix.compute, fix.memory,
                                  fix.MappedBase + fix.DirectComputeOffset, 0,
                                  cs_info->lowered_native};
    cs_compile.SetCachedSize(cs_info->size_bytes);
    CHECK(cs_compile.IsNativeProgram());

    Vulkan::ShaderPools pools;
    const auto compiled = Vulkan::CompileComputeStageWithLazyFallback(
        pools, cs_compile, MakeSpirv16Profile(), MakeHostInfo(), 0, cs_info->unique_hash);

    CHECK(compiled.compiled_direct);
    REQUIRE(compiled.spirv.size() >= 5);
    CHECK(compiled.spirv[0] == 0x07230203U);
    CHECK(compiled.spirv[1] == 0x00010600U);
    CHECK(LowerToMaxwellInvocationCount() == 0);
    CHECK_FALSE(cs_compile.HasLoweredNativeCode());
    CHECK_FALSE(cs_info->lowered_native->HasLoweredCode());
#endif
}

TEST_CASE("Ineligible and mixed fallback stages invoke LowerToMaxwell lazily at most once",
          "[video_core][ampere-lazy-lowering]") {
    LazyLoweringTestFixture fix;
    const std::array direct_cs{ExitInsn};
    const std::array fallback_cs{ConstantLeaHiInsn, ExitInsn};
    fix.WriteComputeProgram(fix.DirectComputeOffset, direct_cs);
    fix.WriteComputeProgram(fix.FallbackComputeOffset, fallback_cs);
    fix.WriteGraphicsProgram(fix.VertexShaderOffset, direct_cs);
    fix.WriteGraphicsProgram(fix.FragmentShaderOffset, direct_cs);

    Vulkan::SetDirectSm86EnabledOverride(true);
    SCOPE_EXIT { Vulkan::SetDirectSm86EnabledOverride(std::nullopt); };
    ResetLowerToMaxwellInvocationCount();

    auto cache = AmpereLazyLoweringTestAccess::Create(fix.device_memory);
    const Shader::Profile profile = MakeSpirv16Profile();
    const Shader::HostTranslateInfo host_info = MakeHostInfo();
    Vulkan::ShaderPools pools;

    // Discover direct compute + fallback compute without lowering either program.
    ComputeEnvironment direct_discovery{fix.compute, fix.memory,
                                        fix.MappedBase + fix.DirectComputeOffset, 0};
    const VAddr direct_cpu =
        *fix.memory.GpuToCpuAddress(fix.MappedBase + fix.DirectComputeOffset);
    const ShaderInfo* const direct_info =
        AmpereLazyLoweringTestAccess::RegisterFromEnv(*cache, direct_discovery, direct_cpu);

    ComputeEnvironment fallback_discovery{fix.compute, fix.memory,
                                          fix.MappedBase + fix.FallbackComputeOffset, 0};
    const VAddr fallback_cpu =
        *fix.memory.GpuToCpuAddress(fix.MappedBase + fix.FallbackComputeOffset);
    const ShaderInfo* const fallback_info =
        AmpereLazyLoweringTestAccess::RegisterFromEnv(*cache, fallback_discovery, fallback_cpu);

    REQUIRE(LowerToMaxwellInvocationCount() == 0);
    REQUIRE(direct_info != nullptr);
    REQUIRE(fallback_info != nullptr);

    // Direct compute compiles without lowering; fallback compute lowers lazily once on miss.
    ComputeEnvironment direct_env{fix.compute, fix.memory,
                                  fix.MappedBase + fix.DirectComputeOffset, 0,
                                  direct_info->lowered_native};
    direct_env.SetCachedSize(direct_info->size_bytes);
    const auto direct_res = Vulkan::CompileComputeStageWithLazyFallback(
        pools, direct_env, profile, host_info, 0, direct_info->unique_hash);
#ifdef EGLANTINE_ENABLE_RUST_AMP_SPIRV
    CHECK(direct_res.compiled_direct);
    CHECK(LowerToMaxwellInvocationCount() == 0);
#else
    CHECK_FALSE(direct_res.compiled_direct);
    CHECK(LowerToMaxwellInvocationCount() == 1);
    ResetLowerToMaxwellInvocationCount();
#endif

    ComputeEnvironment fallback_env_1{fix.compute, fix.memory,
                                      fix.MappedBase + fix.FallbackComputeOffset, 0,
                                      fallback_info->lowered_native};
    fallback_env_1.SetCachedSize(fallback_info->size_bytes);
    const auto fallback_res_1 = Vulkan::CompileComputeStageWithLazyFallback(
        pools, fallback_env_1, profile, host_info, 0, fallback_info->unique_hash);
    CHECK_FALSE(fallback_res_1.compiled_direct);
    REQUIRE(fallback_res_1.spirv.size() >= 5);
    CHECK(fallback_res_1.spirv[0] == 0x07230203U);
    CHECK(LowerToMaxwellInvocationCount() == 1);
    CHECK(fallback_info->lowered_native->HasLoweredCode());
    CHECK(fallback_env_1.HasLoweredNativeCode());

    // Subsequent compilations sharing the ShaderInfo reuse cached lowered words cleanly.
    pools.ReleaseContents();
    ComputeEnvironment fallback_env_2{fix.compute, fix.memory,
                                      fix.MappedBase + fix.FallbackComputeOffset, 0,
                                      fallback_info->lowered_native};
    fallback_env_2.SetCachedSize(fallback_info->size_bytes);
    const auto fallback_res_2 = Vulkan::CompileComputeStageWithLazyFallback(
        pools, fallback_env_2, profile, host_info, 0, fallback_info->unique_hash);
    CHECK_FALSE(fallback_res_2.compiled_direct);
    CHECK(fallback_res_2.spirv == fallback_res_1.spirv);
    CHECK(LowerToMaxwellInvocationCount() == 1);

    // Check graphics direct-eligibility rules and per-stage lazy lowering on graphics fallback.
    ResetLowerToMaxwellInvocationCount();
    GraphicsEnvironment vs_env{
        fix.maxwell3d, fix.memory, Tegra::Engines::Maxwell3D::Regs::ShaderType::VertexB,
        fix.MappedBase + fix.VertexShaderOffset, 0};
    GraphicsEnvironment fs_env{
        fix.maxwell3d, fix.memory, Tegra::Engines::Maxwell3D::Regs::ShaderType::Pixel,
        fix.MappedBase + fix.FragmentShaderOffset, 0};
    CHECK(LowerToMaxwellInvocationCount() == 0);

    using MaxwellShader = Tegra::Engines::Maxwell3D::Regs::ShaderType;
#ifdef EGLANTINE_ENABLE_RUST_AMP_SPIRV
    CHECK(Vulkan::IsDirectSm86GraphicsStageEligible(vs_env, static_cast<size_t>(MaxwellShader::VertexB),
                                                    false, false, false, true));
    CHECK(Vulkan::IsDirectSm86GraphicsStageEligible(fs_env, static_cast<size_t>(MaxwellShader::Pixel),
                                                    false, false, false, true));
    CHECK(Vulkan::IsDirectSm86GraphicsStageEligible(fs_env, static_cast<size_t>(MaxwellShader::Pixel),
                                                    true, false, false, true));
#endif
    // Paired vertex, pruned geometry pipelines, mesh emulation, and wrong slots are ineligible.
    CHECK_FALSE(Vulkan::IsDirectSm86GraphicsStageEligible(
        vs_env, static_cast<size_t>(MaxwellShader::VertexB), true, false, false, true));
    CHECK_FALSE(Vulkan::IsDirectSm86GraphicsStageEligible(
        vs_env, static_cast<size_t>(MaxwellShader::Geometry), false, false, false, true));
    CHECK_FALSE(Vulkan::IsDirectSm86GraphicsStageEligible(
        fs_env, static_cast<size_t>(MaxwellShader::Pixel), false, true, false, true));
    CHECK_FALSE(Vulkan::IsDirectSm86GraphicsStageEligible(
        fs_env, static_cast<size_t>(MaxwellShader::Pixel), false, false, true, true));

    // Only the graphics stages falling back invoke LowerToMaxwell once each (and cache it).
    pools.ReleaseContents();
    Shader::RuntimeInfo vs_runtime;
    vs_runtime.previous_stage_stores.mask.set();
    const auto vs_compiled = Vulkan::CompileGraphicsStageWithLazyFallback(
        pools, vs_env, profile, host_info, vs_runtime, {},
        static_cast<size_t>(MaxwellShader::VertexB), false, false, false);
    REQUIRE(vs_compiled.spirv.size() >= 5);
    CHECK(vs_compiled.spirv[0] == 0x07230203U);
    CHECK(LowerToMaxwellInvocationCount() == 1);
    CHECK_FALSE(fs_env.HasLoweredNativeCode());

    // Paired VertexA+VertexB re-uses vs_env's lowered_native without re-lowering.
    pools.ReleaseContents();
    GraphicsEnvironment vs_paired_env{
        fix.maxwell3d, fix.memory, Tegra::Engines::Maxwell3D::Regs::ShaderType::VertexB,
        fix.MappedBase + fix.VertexShaderOffset, 0, vs_env.LoweredNative()};
    const auto vs_paired_compiled = Vulkan::CompileGraphicsStageWithLazyFallback(
        pools, vs_paired_env, profile, host_info, vs_runtime, {},
        static_cast<size_t>(MaxwellShader::VertexB), true, false, false);
    REQUIRE(vs_paired_compiled.spirv.size() >= 5);
    CHECK(LowerToMaxwellInvocationCount() == 1);

    // Fragment lowers lazily on its own fallback compile.
    pools.ReleaseContents();
    Shader::RuntimeInfo fs_runtime;
    fs_runtime.previous_stage_stores.mask.set();
    const auto fs_compiled = Vulkan::CompileGraphicsStageWithLazyFallback(
        pools, fs_env, profile, host_info, fs_runtime, {},
        static_cast<size_t>(MaxwellShader::Pixel), false, false, false);
    REQUIRE(fs_compiled.spirv.size() >= 5);
    CHECK(LowerToMaxwellInvocationCount() == 2);

    // Disabling direct SM86 dynamically forces lazy lowering even on eligible compute stages.
    Vulkan::SetDirectSm86EnabledOverride(false);
    ComputeEnvironment disabled_direct_env{fix.compute, fix.memory,
                                           fix.MappedBase + fix.DirectComputeOffset, 0};
    CHECK(LowerToMaxwellInvocationCount() == 2);
    pools.ReleaseContents();
    const auto disabled_res = Vulkan::CompileComputeStageWithLazyFallback(
        pools, disabled_direct_env, profile, host_info, 0, direct_info->unique_hash);
    CHECK_FALSE(disabled_res.compiled_direct);
    REQUIRE(disabled_res.spirv.size() >= 5);
    CHECK(LowerToMaxwellInvocationCount() == 3);
}

TEST_CASE("Warm FileEnvironment cache reload succeeds without lowering eligible SM86 shaders",
          "[video_core][ampere-lazy-lowering]") {
    LazyLoweringTestFixture fix;
    const std::array direct_cs{ExitInsn};
    fix.WriteComputeProgram(fix.DirectComputeOffset, direct_cs);

    const std::filesystem::path temp_dir =
        std::filesystem::temp_directory_path() / "eglantine_ampere_lazy_lowering_test";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir, ec);
    SCOPE_EXIT {
        std::error_code cleanup_ec;
        std::filesystem::remove_all(temp_dir, cleanup_ec);
    };
    const std::filesystem::path cache_file = temp_dir / "warm_direct.bin";

    Vulkan::SetDirectSm86EnabledOverride(true);
    SCOPE_EXIT { Vulkan::SetDirectSm86EnabledOverride(std::nullopt); };
    ResetLowerToMaxwellInvocationCount();

    const Shader::Profile profile = MakeSpirv16Profile();
    const Shader::HostTranslateInfo host_info = MakeHostInfo();
    Vulkan::ShaderPools pools;

    ComputeEnvironment cold_env{fix.compute, fix.memory,
                                fix.MappedBase + fix.DirectComputeOffset, 0};
    const std::optional<u64> analyzed = cold_env.Analyze();
    REQUIRE(analyzed.has_value());
    cold_env.SetCachedSize(cold_env.CachedSizeBytes());

    const auto cold_res = Vulkan::CompileComputeStageWithLazyFallback(pools, cold_env, profile,
                                                                      host_info, 0, *analyzed);
#ifdef EGLANTINE_ENABLE_RUST_AMP_SPIRV
    REQUIRE(cold_res.compiled_direct);
    CHECK(LowerToMaxwellInvocationCount() == 0);
#else
    CHECK_FALSE(cold_res.compiled_direct);
    REQUIRE(cold_res.spirv.size() >= 5);
    ResetLowerToMaxwellInvocationCount();
#endif

    const Vulkan::ComputePipelineCacheKey key{
        .unique_hash = *analyzed,
        .shared_memory_size = 0,
        .workgroup_size = {1, 1, 1},
    };
    VideoCommon::SerializePipeline(key, std::array<const GenericEnvironment*, 1>{&cold_env},
                                   cache_file, VideoCommon::ShaderWarm::WarmCacheVersion);
    REQUIRE(std::filesystem::exists(cache_file));
#ifdef EGLANTINE_ENABLE_RUST_AMP_SPIRV
    CHECK(LowerToMaxwellInvocationCount() == 0);
#endif

    std::vector<Vulkan::ComputePipelineCacheKey> loaded_keys;
    std::vector<FileEnvironment> loaded_envs;
    VideoCommon::LoadPipelines(
        {}, cache_file, VideoCommon::ShaderWarm::WarmCacheVersion,
        [&](std::ifstream& file, FileEnvironment env) {
            Vulkan::ComputePipelineCacheKey read_key{};
            file.read(reinterpret_cast<char*>(&read_key), sizeof(read_key));
            loaded_keys.push_back(read_key);
            loaded_envs.push_back(std::move(env));
        },
        [](std::ifstream&, std::vector<FileEnvironment>) {});

    REQUIRE(loaded_envs.size() == 1);
    CHECK(loaded_keys[0] == key);
    CHECK(loaded_envs[0].IsNativeProgram());
    CHECK(loaded_envs[0].HasNativeInstructions());
#ifdef EGLANTINE_ENABLE_RUST_AMP_SPIRV
    CHECK(LowerToMaxwellInvocationCount() == 0);
#endif

    pools.ReleaseContents();
    const auto warm_res = Vulkan::CompileComputeStageWithLazyFallback(pools, loaded_envs[0], profile,
                                                                      host_info, 0, key.unique_hash);
#ifdef EGLANTINE_ENABLE_RUST_AMP_SPIRV
    CHECK(warm_res.compiled_direct);
    REQUIRE(warm_res.spirv.size() >= 5);
    CHECK(warm_res.spirv[0] == 0x07230203U);
    CHECK(warm_res.spirv[1] == 0x00010600U);
    CHECK(LowerToMaxwellInvocationCount() == 0);
#else
    CHECK_FALSE(warm_res.compiled_direct);
    REQUIRE(warm_res.spirv.size() >= 5);
#endif

    // Warm FileEnvironment also supports lazy fallback on demand and caches it cleanly.
    std::vector<FileEnvironment> fallback_warm_envs;
    VideoCommon::LoadPipelines(
        {}, cache_file, VideoCommon::ShaderWarm::WarmCacheVersion,
        [&](std::ifstream& file, FileEnvironment env) {
            Vulkan::ComputePipelineCacheKey read_key{};
            file.read(reinterpret_cast<char*>(&read_key), sizeof(read_key));
            fallback_warm_envs.push_back(std::move(env));
        },
        [](std::ifstream&, std::vector<FileEnvironment>) {});
    REQUIRE(fallback_warm_envs.size() == 1);

    Vulkan::SetDirectSm86EnabledOverride(false);
    ResetLowerToMaxwellInvocationCount();
    pools.ReleaseContents();
    const auto warm_fallback = Vulkan::CompileComputeStageWithLazyFallback(
        pools, fallback_warm_envs[0], profile, host_info, 0, key.unique_hash);
    CHECK_FALSE(warm_fallback.compiled_direct);
    REQUIRE(warm_fallback.spirv.size() >= 5);
    CHECK(warm_fallback.spirv[0] == 0x07230203U);
    CHECK(LowerToMaxwellInvocationCount() == 1);
    CHECK(fallback_warm_envs[0].ReadInstruction(sizeof(Shader::ProgramHeader)) != 0);
    CHECK(LowerToMaxwellInvocationCount() == 1);

    // Pre-lowered legacy v34 disk records remain directly loadable with zero lowering calls.
    auto prelowered = std::make_shared<LoweredNativeCode>();
    auto lowered_prog = Shader::Ampere::LowerToMaxwell(direct_cs, {});
    REQUIRE(lowered_prog);
    const std::array<u64, sizeof(Shader::ProgramHeader) / sizeof(u64)> zero_header{};
    prelowered->native_program_key = Shader::Ampere::NativeProgramKey(direct_cs);
    prelowered->native_code.assign(direct_cs.begin(), direct_cs.end());
    prelowered->source_size = direct_cs.size() * sizeof(Instruction);
    prelowered->texture_bound = lowered_prog.required_texture_buffer;
    prelowered->code = VideoCommon::LayOutLoweredNativeCode(zero_header, direct_cs, lowered_prog);

    const std::filesystem::path legacy_file = temp_dir / "warm_legacy_v34.bin";
    ComputeEnvironment legacy_env{fix.compute, fix.memory,
                                  fix.MappedBase + fix.DirectComputeOffset, 0, prelowered};
    const std::optional<u64> legacy_analyzed = legacy_env.Analyze();
    REQUIRE(legacy_analyzed.has_value());
    const u64 legacy_hash = *legacy_analyzed;
    const Vulkan::ComputePipelineCacheKey legacy_key{
        .unique_hash = legacy_hash,
        .shared_memory_size = 0,
        .workgroup_size = {1, 1, 1},
    };
    VideoCommon::SerializePipeline(legacy_key, std::array<const GenericEnvironment*, 1>{&legacy_env},
                                   legacy_file, VideoCommon::ShaderWarm::WarmCacheVersion);

    std::vector<FileEnvironment> loaded_legacy;
    VideoCommon::LoadPipelines(
        {}, legacy_file, VideoCommon::ShaderWarm::WarmCacheVersion,
        [&](std::ifstream& file, FileEnvironment env) {
            Vulkan::ComputePipelineCacheKey read_key{};
            file.read(reinterpret_cast<char*>(&read_key), sizeof(read_key));
            loaded_legacy.push_back(std::move(env));
        },
        [](std::ifstream&, std::vector<FileEnvironment>) {});
    REQUIRE(loaded_legacy.size() == 1);
    CHECK(loaded_legacy[0].IsNativeProgram());
    CHECK_FALSE(loaded_legacy[0].HasNativeInstructions());

    ResetLowerToMaxwellInvocationCount();
    pools.ReleaseContents();
    const auto legacy_res = Vulkan::CompileComputeStageWithLazyFallback(
        pools, loaded_legacy[0], profile, host_info, 0, legacy_hash);
    CHECK_FALSE(legacy_res.compiled_direct);
    REQUIRE(legacy_res.spirv.size() >= 5);
    CHECK(legacy_res.spirv[0] == 0x07230203U);
    CHECK(LowerToMaxwellInvocationCount() == 0);
}

TEST_CASE("DirectSm86SupportsStage gates compute vs graphics and BuildsUnoptimized matches native_key",
          "[video_core][ampere-lazy-lowering]") {
#ifdef EGLANTINE_ENABLE_RUST_AMP_SPIRV
    CHECK(Vulkan::DirectSm86SupportsStage(Shader::Stage::Compute));
#else
    CHECK_FALSE(Vulkan::DirectSm86SupportsStage(Shader::Stage::Compute));
#endif
    CHECK_FALSE(Vulkan::DirectSm86SupportsStage(Shader::Stage::VertexB));
    CHECK_FALSE(Vulkan::DirectSm86SupportsStage(Shader::Stage::Fragment));
    CHECK_FALSE(Vulkan::DirectSm86SupportsStage(Shader::Stage::Geometry));
    CHECK_FALSE(Vulkan::DirectSm86SupportsStage(Shader::Stage::Task));
    CHECK_FALSE(Vulkan::DirectSm86SupportsStage(Shader::Stage::Mesh));

    constexpr u64 known_amd_unopt_hash = 0xeff3ccbe1438679dULL;
    CHECK(Vulkan::BuildsUnoptimized(VK_DRIVER_ID_AMD_PROPRIETARY, 0x12345678ULL,
                                    known_amd_unopt_hash));
    CHECK_FALSE(Vulkan::BuildsUnoptimized(VK_DRIVER_ID_AMD_PROPRIETARY, 0x12345678ULL, 0ULL));
    CHECK_FALSE(Vulkan::BuildsUnoptimized(VK_DRIVER_ID_NVIDIA_PROPRIETARY, 0x12345678ULL,
                                          known_amd_unopt_hash));
}

TEST_CASE("Failed lazy ShaderInfo entries are evicted cleanly and do not stay wedged",
          "[video_core][ampere-lazy-lowering]") {
    LazyLoweringTestFixture fix;
    constexpr Instruction UnsupportedOpInsn{{0x00007fffU, 0U, 0U, 0U}};
    const std::array bad_cs{UnsupportedOpInsn, ExitInsn};
    fix.WriteComputeProgram(fix.DirectComputeOffset, bad_cs);

    auto cache = AmpereLazyLoweringTestAccess::Create(fix.device_memory);
    const GPUVAddr gpu_addr = fix.MappedBase + fix.DirectComputeOffset;
    const VAddr cpu_addr = *fix.memory.GpuToCpuAddress(gpu_addr);

    std::optional<VAddr> unbound_cpu;
    CHECK(AmpereLazyLoweringTestAccess::TryGetResolved(*cache, gpu_addr, fix.memory.GetID(),
                                                       fix.memory.GetMapSerial(), 1,
                                                       unbound_cpu) == nullptr);
    CHECK_FALSE(unbound_cpu.has_value());

    const Tegra::Control::ChannelStateBridge bridge{fix.maxwell3d, fix.compute, fix.memory, 0ULL,
                                                    0};
    cache->CreateChannelFromBridge(bridge);
    cache->BindToChannel(0);
    SCOPE_EXIT { cache->EraseChannel(0); };

    ComputeEnvironment env{fix.compute, fix.memory, gpu_addr, 0};
    const ShaderInfo* const info =
        AmpereLazyLoweringTestAccess::RegisterFromEnv(*cache, env, cpu_addr);
    REQUIRE(info != nullptr);
    REQUIRE(info->lowered_native != nullptr);
    REQUIRE_FALSE(info->lowered_native->HasFailedLowering());

    const size_t memory_id = fix.memory.GetID();
    const u64 map_serial = fix.memory.GetMapSerial();
    const u64 inv_serial = AmpereLazyLoweringTestAccess::InvalidationSerial(*cache);
    AmpereLazyLoweringTestAccess::CacheResolved(*cache, gpu_addr, memory_id, map_serial,
                                                inv_serial, info);
    AmpereLazyLoweringTestAccess::SetShaderInfoSlot(*cache, 0, info);

    Vulkan::SetDirectSm86EnabledOverride(false);
    SCOPE_EXIT { Vulkan::SetDirectSm86EnabledOverride(std::nullopt); };
    Vulkan::ShaderPools pools;
    REQUIRE_THROWS(Vulkan::CompileComputeStageWithLazyFallback(pools, env, MakeSpirv16Profile(),
                                                               MakeHostInfo(), 0,
                                                               info->unique_hash));
    REQUIRE(info->lowered_native->HasFailedLowering());

    // TryGetResolved detects HasFailedLowering() and evicts the failed ShaderInfo automatically.
    std::optional<VAddr> out_cpu_addr;
    CHECK(AmpereLazyLoweringTestAccess::TryGetResolved(*cache, gpu_addr, memory_id, map_serial,
                                                       inv_serial, out_cpu_addr) == nullptr);
    CHECK(out_cpu_addr == cpu_addr);
    CHECK(AmpereLazyLoweringTestAccess::Find(*cache, cpu_addr) == nullptr);
    CHECK(AmpereLazyLoweringTestAccess::GetShaderInfoSlot(*cache, 0) == nullptr);
    CHECK_FALSE(AmpereLazyLoweringTestAccess::LastShadersValid(*cache));

    // Re-registering and using EvictFailedLazyShaders() explicitly also cleans lookup & shader_infos.
    ComputeEnvironment env2{fix.compute, fix.memory, gpu_addr, 0};
    const ShaderInfo* const info2 =
        AmpereLazyLoweringTestAccess::RegisterFromEnv(*cache, env2, cpu_addr);
    REQUIRE(info2 != nullptr);
    AmpereLazyLoweringTestAccess::SetShaderInfoSlot(*cache, 1, info2);
    REQUIRE_THROWS(env2.EnsureLoweredNative());
    REQUIRE(info2->lowered_native->HasFailedLowering());
    CHECK(AmpereLazyLoweringTestAccess::EvictFailedLazyShaders(*cache) == 1);
    CHECK(AmpereLazyLoweringTestAccess::Find(*cache, cpu_addr) == nullptr);
    CHECK(AmpereLazyLoweringTestAccess::GetShaderInfoSlot(*cache, 1) == nullptr);
}

TEST_CASE("ShaderCache invokes OnDebugRejectionsRefreshed and refreshes optional channel state",
          "[video_core][ampere-lazy-lowering]") {
    class RecordingShaderCache final : public ShaderCache {
    public:
        explicit RecordingShaderCache(Tegra::MaxwellDeviceMemoryManager& memory)
            : ShaderCache{memory} {}

        size_t calls{};
        bool last_unrejected{};
        std::vector<u64> last_hashes;

    protected:
        void OnDebugRejectionsRefreshed(bool unrejected,
                                        std::span<const u64> dropped_hashes) override {
            ++calls;
            last_unrejected = unrejected;
            last_hashes.assign(dropped_hashes.begin(), dropped_hashes.end());
        }
    };

    LazyLoweringTestFixture fix;
    const std::array direct_cs{ExitInsn};
    fix.WriteComputeProgram(fix.DirectComputeOffset, direct_cs);

    RecordingShaderCache cache{fix.device_memory};
    const Tegra::Control::ChannelStateBridge bridge{fix.maxwell3d, fix.compute, fix.memory, 0ULL,
                                                    0};
    cache.CreateChannelFromBridge(bridge);
    cache.BindToChannel(0);
    SCOPE_EXIT { cache.EraseChannel(0); };

    const GPUVAddr gpu_addr = fix.MappedBase + fix.DirectComputeOffset;
    const VAddr cpu_addr = *fix.memory.GpuToCpuAddress(gpu_addr);
    ComputeEnvironment env{fix.compute, fix.memory, gpu_addr, 0};
    const ShaderInfo* const info =
        AmpereLazyLoweringTestAccess::RegisterFromEnv(cache, env, cpu_addr);
    REQUIRE(info != nullptr);
    const u64 unique_hash = info->unique_hash;
    const u64 program_key = info->NativeProgramKey();
    AmpereLazyLoweringTestAccess::SetShaderInfoSlot(cache, 0, info);

    fix.maxwell3d.dirty.flags[VideoCommon::Dirty::Shaders] = false;
    VideoCore::Nx2Debug::BeginSession();
    SCOPE_EXIT { VideoCore::Nx2Debug::EndSession(); };

    const std::array<u64, 1> reject_keys{program_key};
    VideoCore::Nx2Debug::SetRejected(reject_keys, true);
    cache.RefreshDebugRejections(false);

    CHECK(cache.calls == 1);
    CHECK_FALSE(cache.last_unrejected);
    CHECK(std::ranges::find(cache.last_hashes, unique_hash) != cache.last_hashes.end());
    CHECK(std::ranges::find(cache.last_hashes, program_key) != cache.last_hashes.end());
    CHECK(AmpereLazyLoweringTestAccess::Find(cache, cpu_addr) == nullptr);
    CHECK(AmpereLazyLoweringTestAccess::GetShaderInfoSlot(cache, 0) == nullptr);
    CHECK(fix.maxwell3d.dirty.flags[VideoCommon::Dirty::Shaders]);

    fix.maxwell3d.dirty.flags[VideoCommon::Dirty::Shaders] = false;
    VideoCore::Nx2Debug::SetRejected(reject_keys, false);
    cache.RefreshDebugRejections(true);
    CHECK(cache.calls == 2);
    CHECK(cache.last_unrejected);
    CHECK(fix.maxwell3d.dirty.flags[VideoCommon::Dirty::Shaders]);
}

TEST_CASE("BuildNativeSnapshot reads viewport and texture bound without ALU pointer probing",
          "[video_core][ampere-lazy-lowering]") {
    LazyLoweringTestFixture fix;

    // Form=3, non-tex/mem ALU instruction reading c[2][0x10] (8-byte aligned), plus EXIT.
    // Words encode: op=0x610 (form 3, low nibble 0x10), pred=7, cbuf offset=0x10, bank=2, bit91=0.
    constexpr Instruction AluCbufInsn{{0x00007610U, (2U << 22) | (0x10U << 6), 0U, 0U}};
    const std::array alu_prog{AluCbufInsn, ExitInsn};
    fix.WriteComputeProgram(fix.DirectComputeOffset, alu_prog);

    const u64 mapped_ptr = fix.MappedBase + 0x300ULL;
    fix.memory.WriteBlock(fix.MappedBase + fix.Cbuf2Offset + 0x10, &mapped_ptr, sizeof(mapped_ptr));
    fix.compute.regs.tex_cb_index = 2;

    ComputeEnvironment cs_env{fix.compute, fix.memory, fix.MappedBase + fix.DirectComputeOffset, 0};
    const auto cs_snap = cs_env.BuildNativeSnapshot();
    CHECK(cs_snap.texture_bound == 2);
    CHECK(cs_snap.cbuf_values.contains((2ULL << 32) | 0x10ULL));
    CHECK(cs_snap.storage_pointer_types.empty());

    // Graphics BuildNativeSnapshot must refresh viewport_scale_offset_enabled and texture_bound.
    fix.WriteGraphicsProgram(fix.VertexShaderOffset, std::array{ExitInsn});
    fix.maxwell3d.regs.bindless_texture_const_buffer_slot = 4;
    fix.maxwell3d.regs.viewport_scale_offset_enabled = 0;
    GraphicsEnvironment vs_env{
        fix.maxwell3d, fix.memory, Tegra::Engines::Maxwell3D::Regs::ShaderType::VertexB,
        fix.MappedBase + fix.VertexShaderOffset, 0};
    const auto vs_snap = vs_env.BuildNativeSnapshot();
    CHECK(vs_snap.texture_bound == 4);
    CHECK(vs_snap.viewport_transform_state == 0);
}

TEST_CASE("GenericEnvironment::Serialize preserves SM86 blob after concurrent lazy lowering",
          "[video_core][ampere-lazy-lowering]") {
    LazyLoweringTestFixture fix;
    const std::array direct_cs{ExitInsn};
    fix.WriteComputeProgram(fix.DirectComputeOffset, direct_cs);

    ComputeEnvironment direct_env{fix.compute, fix.memory,
                                  fix.MappedBase + fix.DirectComputeOffset, 0};
    const std::optional<u64> analyzed = direct_env.Analyze();
    REQUIRE(analyzed.has_value());
    const u64 expected_key = direct_env.NativeProgramKey();
    REQUIRE(expected_key != 0);

    // Simulate another pipeline lazily lowering the shared LoweredNativeCode before serialization.
    ComputeEnvironment sibling_env{fix.compute, fix.memory,
                                   fix.MappedBase + fix.DirectComputeOffset, 0,
                                   direct_env.LoweredNative()};
    sibling_env.EnsureLoweredNative();
    REQUIRE(direct_env.LoweredNative()->HasLoweredCode());

    const std::filesystem::path temp_dir =
        std::filesystem::temp_directory_path() / "eglantine_serialize_sm86_race_test";
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    std::filesystem::create_directories(temp_dir, ec);
    SCOPE_EXIT {
        std::error_code cleanup_ec;
        std::filesystem::remove_all(temp_dir, cleanup_ec);
    };
    const std::filesystem::path cache_file = temp_dir / "race_test.bin";

    const Vulkan::ComputePipelineCacheKey key{
        .unique_hash = *analyzed,
        .shared_memory_size = 0,
        .workgroup_size = {1, 1, 1},
    };
    VideoCommon::SerializePipeline(key, std::array<const GenericEnvironment*, 1>{&direct_env},
                                   cache_file, VideoCommon::ShaderWarm::WarmCacheVersion);

    std::vector<FileEnvironment> loaded;
    VideoCommon::LoadPipelines(
        {}, cache_file, VideoCommon::ShaderWarm::WarmCacheVersion,
        [&](std::ifstream& file, FileEnvironment env) {
            Vulkan::ComputePipelineCacheKey read_key{};
            file.read(reinterpret_cast<char*>(&read_key), sizeof(read_key));
            loaded.push_back(std::move(env));
        },
        [](std::ifstream&, std::vector<FileEnvironment>) {});
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].IsNativeProgram());
    CHECK(loaded[0].HasNativeInstructions());
    CHECK(loaded[0].NativeProgramKey() == expected_key);
}

#ifdef EGLANTINE_ENABLE_RUST_AMP_SPIRV
TEST_CASE("ampere_spirv FFI error and fallback paths always populate error text",
          "[video_core][ampere-lazy-lowering]") {
    EglSpirvCompileOutput out{};
    CHECK(egl_ampere_spirv_compile(nullptr, &out) == -1);
    CHECK(out.error != nullptr);
    CHECK(out.error_len > 0);
    egl_ampere_spirv_free(&out);
    CHECK(out.error == nullptr);
    CHECK(out.error_len == 0);

    EglSpirvCompileInput invalid_slice{};
    invalid_slice.stage = EGL_SPIRV_STAGE_COMPUTE;
    invalid_slice.code = nullptr;
    invalid_slice.code_count = 1;
    CHECK(egl_ampere_spirv_compile(&invalid_slice, &out) == -1);
    CHECK(out.error != nullptr);
    CHECK(out.error_len > 0);
    egl_ampere_spirv_free(&out);

    const std::array<EglAmpereInstruction, 1> code{
        EglAmpereInstruction{{0x0000794dU, 0U, 0x03800000U, 0U}}};
    EglSpirvCompileInput non_compute{};
    non_compute.stage = EGL_SPIRV_STAGE_VERTEX_B;
    non_compute.code = code.data();
    non_compute.code_count = code.size();
    non_compute.profile.supported_spirv = 0x00010600U;
    CHECK(egl_ampere_spirv_compile(&non_compute, &out) == 0);
    CHECK(out.fallback_to_maxwell == 1);
    CHECK(out.error == nullptr);
    CHECK(out.error_len == 0);
    egl_ampere_spirv_free(&out);

    std::array<EglSpirvCompileOutput, 2> batch_outs{};
    CHECK(egl_ampere_spirv_compile_batch(nullptr, batch_outs.size(), batch_outs.data()) == -1);
    CHECK(batch_outs[0].error != nullptr);
    CHECK(batch_outs[0].error_len > 0);
    for (auto& slot : batch_outs) {
        egl_ampere_spirv_free(&slot);
    }
}
#endif

} // namespace
