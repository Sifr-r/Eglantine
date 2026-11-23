// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <type_traits>
#include <utility>

#include "common/assert.h"
#include "common/common_types.h"
#include "video_core/control/channel_state.h"
#include "video_core/dirty_flags.h"
#include "video_core/dma_pusher.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/memory_manager.h"

namespace Core {
class System;
}

namespace VideoCore {
class RasterizerInterface;
}

namespace Tegra::Control {

/// Lightweight non-owning bridge over a GPU channel's engines, memory manager, DMA pusher,
/// and Maxwell3D dirty state tables/flags.
class ChannelStateBridge {
public:
    using DirtyState = Engines::Maxwell3D::DirtyState;
    using DirtyFlags = DirtyState::Flags;
    using DirtyTable = DirtyState::Table;
    using DirtyTables = DirtyState::Tables;
    using DrawManager = Engines::Maxwell3D::DrawManager;
    using ShaderType = Engines::Maxwell3D::Regs::ShaderType;
    using PrimitiveTopology = Engines::Maxwell3D::Regs::PrimitiveTopology;

    static constexpr s32 UnboundChannelId = -1;
    static constexpr std::size_t NumDirtyFlags = DirtyFlags{}.size();
    static constexpr std::size_t NumDirtyTables = sizeof(DirtyTables) / sizeof(DirtyTable);
    static constexpr std::size_t NumDirtyTableEntries = Engines::Maxwell3D::Regs::NUM_REGS;

    constexpr ChannelStateBridge() noexcept = default;

    explicit ChannelStateBridge(ChannelState& channel_state_) noexcept
        : channel_state{&channel_state_}
        , maxwell3d{channel_state_.payload.has_value() ? &channel_state_.payload->maxwell_3d : nullptr}
        , kepler_compute{channel_state_.payload.has_value() ? &channel_state_.payload->kepler_compute
                                                            : nullptr}
        , gpu_memory{channel_state_.memory_manager}
        , dma_pusher{channel_state_.payload.has_value() ? &channel_state_.payload->dma_pusher : nullptr}
        , dirty_flags{maxwell3d != nullptr ? &maxwell3d->dirty.flags : nullptr}
        , dirty_tables{maxwell3d != nullptr ? &maxwell3d->dirty.tables : nullptr}
        , bind_id{channel_state_.bind_id}
        , program_id{channel_state_.program_id} {}

    ChannelStateBridge(Engines::Maxwell3D& maxwell3d_,
                       Engines::KeplerCompute& kepler_compute_,
                       MemoryManager& gpu_memory_,
                       u64 program_id_ = 0,
                       s32 bind_id_ = 0) noexcept
        : maxwell3d{&maxwell3d_}
        , kepler_compute{&kepler_compute_}
        , gpu_memory{&gpu_memory_}
        , dirty_flags{&maxwell3d_.dirty.flags}
        , dirty_tables{&maxwell3d_.dirty.tables}
        , bind_id{bind_id_}
        , program_id{program_id_} {}

    explicit ChannelStateBridge(Engines::Maxwell3D& maxwell3d_,
                                MemoryManager* gpu_memory_ = nullptr,
                                u64 program_id_ = 0,
                                s32 bind_id_ = UnboundChannelId) noexcept
        : maxwell3d{&maxwell3d_}
        , gpu_memory{gpu_memory_ != nullptr ? gpu_memory_ : &maxwell3d_.GetMemoryManager()}
        , dirty_flags{&maxwell3d_.dirty.flags}
        , dirty_tables{&maxwell3d_.dirty.tables}
        , bind_id{bind_id_}
        , program_id{program_id_} {}

    explicit constexpr ChannelStateBridge(DirtyFlags& flags_,
                                          DirtyTables* tables_ = nullptr) noexcept
        : dirty_flags{&flags_}
        , dirty_tables{tables_} {}

    [[nodiscard]] static constexpr ChannelStateBridge FromDirtyFlags(
        DirtyFlags& flags_, DirtyTables* tables_ = nullptr) noexcept {
        return ChannelStateBridge{flags_, tables_};
    }

    void Reset() noexcept {
        *this = ChannelStateBridge{};
    }

    [[nodiscard]] constexpr bool IsBound() const noexcept {
        return dirty_flags != nullptr || maxwell3d != nullptr || kepler_compute != nullptr ||
               channel_state != nullptr;
    }

    [[nodiscard]] constexpr bool HasChannelBindings() const noexcept {
        return maxwell3d != nullptr && kepler_compute != nullptr && gpu_memory != nullptr;
    }

    [[nodiscard]] explicit constexpr operator bool() const noexcept {
        return IsBound();
    }

    [[nodiscard]] constexpr s32 BindId() const noexcept {
        return bind_id;
    }

    [[nodiscard]] constexpr u64 ProgramId() const noexcept {
        return program_id;
    }

    [[nodiscard]] bool IsInitialized() const noexcept {
        return channel_state != nullptr ? channel_state->initialized : HasChannelBindings();
    }

    [[nodiscard]] bool IsNativeAmpere() const noexcept {
        return maxwell3d != nullptr && maxwell3d->GetNativeMacroEngine().native_turing;
    }

    [[nodiscard]] constexpr bool HasChannelState() const noexcept {
        return channel_state != nullptr;
    }

    [[nodiscard]] constexpr ChannelState* RawChannelState() const noexcept {
        return channel_state;
    }

    [[nodiscard]] constexpr bool HasMaxwell3D() const noexcept {
        return maxwell3d != nullptr;
    }

    [[nodiscard]] constexpr Engines::Maxwell3D* Maxwell3DPtr() const noexcept {
        return maxwell3d;
    }

    [[nodiscard]] Engines::Maxwell3D& Maxwell3D() {
        ASSERT(maxwell3d != nullptr);
        return *maxwell3d;
    }

    [[nodiscard]] const Engines::Maxwell3D& Maxwell3D() const {
        ASSERT(maxwell3d != nullptr);
        return *maxwell3d;
    }

    [[nodiscard]] constexpr bool HasKeplerCompute() const noexcept {
        return kepler_compute != nullptr;
    }

    [[nodiscard]] constexpr Engines::KeplerCompute* KeplerComputePtr() const noexcept {
        return kepler_compute;
    }

    [[nodiscard]] Engines::KeplerCompute& KeplerCompute() {
        ASSERT(kepler_compute != nullptr);
        return *kepler_compute;
    }

    [[nodiscard]] const Engines::KeplerCompute& KeplerCompute() const {
        ASSERT(kepler_compute != nullptr);
        return *kepler_compute;
    }

    [[nodiscard]] constexpr bool HasGpuMemory() const noexcept {
        return gpu_memory != nullptr;
    }

    [[nodiscard]] constexpr MemoryManager* GpuMemoryPtr() const noexcept {
        return gpu_memory;
    }

    [[nodiscard]] MemoryManager& GpuMemory() {
        ASSERT(gpu_memory != nullptr);
        return *gpu_memory;
    }

    [[nodiscard]] const MemoryManager& GpuMemory() const {
        ASSERT(gpu_memory != nullptr);
        return *gpu_memory;
    }

    [[nodiscard]] std::size_t AddressSpaceId() const noexcept {
        return gpu_memory != nullptr ? gpu_memory->GetID() : 0;
    }

    [[nodiscard]] constexpr bool HasDmaPusher() const noexcept {
        return dma_pusher != nullptr;
    }

    [[nodiscard]] constexpr DmaPusher* DmaPusherPtr() const noexcept {
        return dma_pusher;
    }

    void BindRasterizer(VideoCore::RasterizerInterface* rasterizer) {
        if (channel_state != nullptr && channel_state->payload.has_value()) {
            channel_state->BindRasterizer(rasterizer);
            return;
        }
        if (dma_pusher != nullptr) {
            dma_pusher->BindRasterizer(rasterizer);
        }
        if (gpu_memory != nullptr) {
            gpu_memory->BindRasterizer(rasterizer);
        }
        if (maxwell3d != nullptr) {
            maxwell3d->BindRasterizer(rasterizer);
        }
        if (kepler_compute != nullptr) {
            kepler_compute->BindRasterizer(rasterizer);
        }
    }

    [[nodiscard]] constexpr bool HasDirtyFlags() const noexcept {
        return dirty_flags != nullptr;
    }

    [[nodiscard]] constexpr DirtyFlags* DirtyFlagsPtr() const noexcept {
        return dirty_flags;
    }

    [[nodiscard]] DirtyFlags& DirtyFlagsRef() {
        ASSERT(dirty_flags != nullptr);
        return *dirty_flags;
    }

    [[nodiscard]] const DirtyFlags& DirtyFlagsRef() const {
        ASSERT(dirty_flags != nullptr);
        return *dirty_flags;
    }

    [[nodiscard]] constexpr bool HasDirtyTables() const noexcept {
        return dirty_tables != nullptr;
    }

    [[nodiscard]] constexpr DirtyTables* DirtyTablesPtr() const noexcept {
        return dirty_tables;
    }

    [[nodiscard]] DirtyTables& DirtyTablesRef() {
        ASSERT(dirty_tables != nullptr);
        return *dirty_tables;
    }

    [[nodiscard]] const DirtyTables& DirtyTablesRef() const {
        ASSERT(dirty_tables != nullptr);
        return *dirty_tables;
    }

    [[nodiscard]] bool IsDirty(std::size_t id) const noexcept {
        return dirty_flags != nullptr && id < NumDirtyFlags && (*dirty_flags)[id];
    }

    [[nodiscard]] bool AnyDirty() const noexcept {
        return dirty_flags != nullptr && dirty_flags->any();
    }

    void SetDirty(std::size_t id, bool value = true) noexcept {
        if (dirty_flags == nullptr || id >= NumDirtyFlags) {
            return;
        }
        (*dirty_flags)[id] = value;
    }

    void SetDirtyRange(std::size_t start, std::size_t end, bool value = true) noexcept {
        if (dirty_flags == nullptr || start > end || start >= NumDirtyFlags) {
            return;
        }
        const std::size_t clamped_end = (std::min)(end, NumDirtyFlags - 1);
        for (std::size_t flag = start; flag <= clamped_end; ++flag) {
            (*dirty_flags)[flag] = value;
        }
    }

    [[nodiscard]] bool ExchangeDirty(std::size_t id, bool new_value) noexcept {
        if (dirty_flags == nullptr || id >= NumDirtyFlags) {
            return false;
        }
        const bool is_dirty = (*dirty_flags)[id];
        (*dirty_flags)[id] = new_value;
        return is_dirty;
    }

    void OrDirtyFlags(const DirtyFlags& mask) noexcept {
        if (dirty_flags != nullptr) {
            *dirty_flags |= mask;
        }
    }

    void SetAllDirtyFlags() noexcept {
        if (dirty_flags != nullptr) {
            dirty_flags->set();
        }
    }

    void ResetDirtyFlags() noexcept {
        if (dirty_flags != nullptr) {
            dirty_flags->reset();
        }
    }

    void SetupDirtyTables() {
        if (dirty_tables != nullptr) {
            VideoCommon::Dirty::SetupDirtyFlags(*dirty_tables);
        }
    }

    [[nodiscard]] u8 DirtyTableEntry(std::size_t table_index, std::size_t reg) const noexcept {
        if (dirty_tables == nullptr || table_index >= NumDirtyTables ||
            reg >= NumDirtyTableEntries) {
            return 0;
        }
        return (*dirty_tables)[table_index][reg];
    }

    void SetDirtyTableEntry(std::size_t table_index, std::size_t reg, u8 dirty_flag) noexcept {
        if (dirty_tables == nullptr || table_index >= NumDirtyTables ||
            reg >= NumDirtyTableEntries) {
            return;
        }
        (*dirty_tables)[table_index][reg] = dirty_flag;
    }

    void ApplyDirtyTables(u32 method) noexcept {
        if (dirty_flags == nullptr || dirty_tables == nullptr || method >= NumDirtyTableEntries) {
            return;
        }
        for (const auto& table : *dirty_tables) {
            (*dirty_flags)[table[method]] = true;
        }
    }

    [[nodiscard]] Engines::Maxwell3D::Regs& MaxwellRegsRef() {
        ASSERT(maxwell3d != nullptr);
        return maxwell3d->regs;
    }

    [[nodiscard]] const Engines::Maxwell3D::Regs& MaxwellRegsRef() const {
        ASSERT(maxwell3d != nullptr);
        return maxwell3d->regs;
    }

    [[nodiscard]] u32 ReadMaxwellRegister(u32 method) const {
        return maxwell3d != nullptr ? maxwell3d->GetRegisterValue(method) : 0;
    }

    [[nodiscard]] u32 ReadShadowRegister(u32 method) const noexcept {
        if (maxwell3d == nullptr || method >= Engines::Maxwell3D::Regs::NUM_REGS) {
            return 0;
        }
        return maxwell3d->shadow_state.reg_array[method];
    }

    [[nodiscard]] bool IsMaxwellMethodExecutable(u32 method) const noexcept {
        return maxwell3d != nullptr && method < maxwell3d->execution_mask.size() &&
               maxwell3d->execution_mask[method];
    }

    void CallMaxwellMethod(Core::System& system, u32 method, u32 argument,
                           bool is_last_call = true) {
        if (maxwell3d != nullptr) {
            maxwell3d->CallMethod(system, method, argument, is_last_call);
        }
    }

    void CallMaxwellStateMethod(Core::System& system, u32 method, u32 argument) {
        if (maxwell3d != nullptr) {
            maxwell3d->CallStateMethod(system, method, argument);
        }
    }

    void CallMaxwellMultiMethod(Core::System& system, u32 method,
                                std::span<const u32> arguments) {
        if (maxwell3d != nullptr && !arguments.empty()) {
            const auto amount = static_cast<u32>(arguments.size());
            maxwell3d->CallMultiMethod(system, method, arguments.data(), amount, amount);
        }
    }

    void ConsumeMaxwellSink(Core::System& system) {
        if (maxwell3d != nullptr) {
            maxwell3d->ConsumeSink(system);
        }
    }

    [[nodiscard]] DrawManager& MaxwellDrawManager() {
        ASSERT(maxwell3d != nullptr);
        return maxwell3d->draw_manager;
    }

    [[nodiscard]] const DrawManager& MaxwellDrawManager() const {
        ASSERT(maxwell3d != nullptr);
        return maxwell3d->draw_manager;
    }

    [[nodiscard]] const DrawManager::State& CurrentDrawState() const {
        ASSERT(maxwell3d != nullptr);
        return maxwell3d->draw_manager.draw_state;
    }

    [[nodiscard]] const DrawManager::DrawTextureState& DrawTextureState() const {
        ASSERT(maxwell3d != nullptr);
        return maxwell3d->draw_manager.draw_texture_state;
    }

    [[nodiscard]] const DrawManager::IndirectParams& IndirectDrawState() const {
        ASSERT(maxwell3d != nullptr);
        return maxwell3d->draw_manager.indirect_state;
    }

    [[nodiscard]] PrimitiveTopology CurrentTopology() const noexcept {
        return maxwell3d != nullptr ? maxwell3d->draw_manager.draw_state.topology
                                    : PrimitiveTopology{};
    }

    [[nodiscard]] bool ShouldExecuteDraw() const {
        return maxwell3d != nullptr && maxwell3d->ShouldExecute();
    }

    [[nodiscard]] bool HasDeferredDraw() const noexcept {
        return maxwell3d != nullptr && maxwell3d->draw_manager.HasDeferredDraw();
    }

    void FlushDeferredDraw(bool loop_continues = false) {
        if (maxwell3d != nullptr && maxwell3d->draw_manager.HasDeferredDraw()) {
            maxwell3d->draw_manager.DrawDeferred(*maxwell3d, loop_continues);
        }
    }

    [[nodiscard]] bool IsShaderConfigEnabled(std::size_t index) const noexcept {
        return maxwell3d != nullptr && index < Engines::Maxwell3D::Regs::MaxShaderProgram &&
               maxwell3d->regs.IsShaderConfigEnabled(index);
    }

    [[nodiscard]] bool IsShaderConfigEnabled(ShaderType type) const noexcept {
        return IsShaderConfigEnabled(static_cast<std::size_t>(type));
    }

    [[nodiscard]] bool IsMeshPipelineEnabled() const noexcept {
        return maxwell3d != nullptr && maxwell3d->regs.IsMeshEnabled();
    }

    [[nodiscard]] GPUVAddr GraphicsProgramAddress(std::size_t index) const noexcept {
        if (maxwell3d == nullptr || index >= Engines::Maxwell3D::Regs::MaxShaderProgram) {
            return 0;
        }
        const auto& config = maxwell3d->regs.pipelines[index];
        if (IsNativeAmpere()) {
            return (GPUVAddr{config.reserved[0] & 0xffU} << 32) | GPUVAddr{config.reserved[1]};
        }
        return maxwell3d->regs.program_region.Address() + config.offset;
    }

    [[nodiscard]] GPUVAddr GraphicsProgramAddress(ShaderType type) const noexcept {
        return GraphicsProgramAddress(static_cast<std::size_t>(type));
    }

    [[nodiscard]] const Engines::ConstBufferInfo& GraphicsConstBuffer(std::size_t stage,
                                                                      std::size_t slot) const {
        ASSERT(maxwell3d != nullptr);
        ASSERT(stage < Engines::Maxwell3D::Regs::MaxShaderStage);
        ASSERT(slot < Engines::Maxwell3D::Regs::MaxConstBuffers);
        return maxwell3d->state.shader_stages[stage].const_buffers[slot];
    }

    [[nodiscard]] const Engines::Maxwell3D::RootTables& RootTables() const {
        ASSERT(maxwell3d != nullptr);
        return maxwell3d->root_tables;
    }

    [[nodiscard]] Engines::KeplerCompute::Regs& ComputeRegsRef() {
        ASSERT(kepler_compute != nullptr);
        return kepler_compute->regs;
    }

    [[nodiscard]] const Engines::KeplerCompute::Regs& ComputeRegsRef() const {
        ASSERT(kepler_compute != nullptr);
        return kepler_compute->regs;
    }

    [[nodiscard]] u32 ReadComputeRegister(u32 method) const noexcept {
        if (kepler_compute == nullptr || method >= Engines::KeplerCompute::Regs::NUM_REGS) {
            return 0;
        }
        return kepler_compute->regs.reg_array[method];
    }

    [[nodiscard]] const Engines::KeplerCompute::LaunchParams& ComputeLaunchParams() const {
        ASSERT(kepler_compute != nullptr);
        return kepler_compute->launch_description;
    }

    [[nodiscard]] bool IsComputeNativeMode() const noexcept {
        return kepler_compute != nullptr && kepler_compute->IsNativeMode();
    }

    [[nodiscard]] bool ComputeMayWriteMemory(u32 method) const noexcept {
        return kepler_compute != nullptr && kepler_compute->MayWriteMemory(method);
    }

    [[nodiscard]] bool IsComputeMethodExecutable(u32 method) const noexcept {
        return kepler_compute != nullptr && method < kepler_compute->execution_mask.size() &&
               kepler_compute->execution_mask[method];
    }

    void CallComputeMethod(Core::System& system, u32 method, u32 argument,
                           bool is_last_call = true) {
        if (kepler_compute != nullptr) {
            kepler_compute->CallMethod(system, method, argument, is_last_call);
        }
    }

    void CallComputeStateMethod(Core::System& system, u32 method, u32 argument) {
        if (kepler_compute != nullptr) {
            kepler_compute->CallStateMethod(system, method, argument);
        }
    }

    void CallComputeMultiMethod(Core::System& system, u32 method,
                                std::span<const u32> arguments) {
        if (kepler_compute != nullptr && !arguments.empty()) {
            const auto amount = static_cast<u32>(arguments.size());
            kepler_compute->CallMultiMethod(system, method, arguments.data(), amount, amount);
        }
    }

    void ConsumeComputeSink(Core::System& system) {
        if (kepler_compute != nullptr) {
            kepler_compute->ConsumeSink(system);
        }
    }

    [[nodiscard]] std::optional<GPUVAddr> IndirectComputeAddress() const {
        return kepler_compute != nullptr ? kepler_compute->GetIndirectComputeAddress()
                                         : std::nullopt;
    }

    [[nodiscard]] GPUVAddr ComputeProgramAddress() const noexcept {
        if (kepler_compute == nullptr) {
            return 0;
        }
        return kepler_compute->regs.code_loc.Address() +
               kepler_compute->launch_description.program_start;
    }

    [[nodiscard]] std::array<u32, 3> ComputeGridDimensions() const noexcept {
        if (kepler_compute == nullptr) {
            return {};
        }
        const auto& launch = kepler_compute->launch_description;
        return {
            static_cast<u32>(launch.grid_dim_x),
            static_cast<u32>(launch.grid_dim_y),
            static_cast<u32>(launch.grid_dim_z),
        };
    }

    [[nodiscard]] std::array<u32, 3> ComputeBlockDimensions() const noexcept {
        if (kepler_compute == nullptr) {
            return {};
        }
        const auto& launch = kepler_compute->launch_description;
        return {
            static_cast<u32>(launch.block_dim_x),
            static_cast<u32>(launch.block_dim_y),
            static_cast<u32>(launch.block_dim_z),
        };
    }

    [[nodiscard]] bool HasPendingAcquire() const {
        return dma_pusher != nullptr && dma_pusher->HasPendingAcquire();
    }

    [[nodiscard]] bool CanResumeAcquire() const {
        return dma_pusher != nullptr && dma_pusher->CanResumeAcquire();
    }

    void PushCommands(CommandList&& entries) {
        if (dma_pusher != nullptr) {
            dma_pusher->Push(std::move(entries));
        }
    }

    void DispatchCommands() {
        if (dma_pusher != nullptr) {
            dma_pusher->DispatchCalls();
        }
    }

private:
    ChannelState* channel_state = nullptr;
    Engines::Maxwell3D* maxwell3d = nullptr;
    Engines::KeplerCompute* kepler_compute = nullptr;
    MemoryManager* gpu_memory = nullptr;
    DmaPusher* dma_pusher = nullptr;
    DirtyFlags* dirty_flags = nullptr;
    DirtyTables* dirty_tables = nullptr;
    s32 bind_id = UnboundChannelId;
    u64 program_id = 0;
};

static_assert(std::is_trivially_copyable_v<ChannelStateBridge>);
static_assert(std::is_trivially_destructible_v<ChannelStateBridge>);

} // namespace Tegra::Control
