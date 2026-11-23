// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/fiber.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/thread.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/cpu_manager.h"
#include "core/hle/kernel/k_interrupt_manager.h"
#include "core/hle/kernel/k_scheduler.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "video_core/gpu.h"

namespace Core {

CpuManager::CpuManager(System& system_) : system{system_} {}
CpuManager::~CpuManager() = default;

void CpuManager::SetActiveCoreCount(u32 core_count) {
    const std::size_t clamped =
        std::clamp<std::size_t>(core_count, 1, Core::Hardware::NUM_CPU_CORES);
    if (num_cores != 0 || gpu_barrier.has_value()) {
        if (clamped != active_core_count) {
            LOG_ERROR(Core,
                      "Ignoring CpuManager::SetActiveCoreCount({}) after initialization (active={})",
                      clamped, active_core_count);
        }
        return;
    }
    active_core_count = clamped;
}

void CpuManager::Initialize() {
    current_core.store(0);
    idle_count = 0;
    num_cores = is_multicore ? active_core_count : 1;
    gpu_barrier.emplace(num_cores + 1);
    for (std::size_t core = 0; core < num_cores; core++)
        core_data[core].host_thread = std::jthread([this, core](std::stop_token token) {
            RunThread(token, core);
        });
}

void CpuManager::Shutdown() {
    for (std::size_t core = 0; core < num_cores; core++) {
        if (core_data[core].host_thread.joinable()) {
            core_data[core].host_thread.request_stop();
            core_data[core].host_thread.join();
        }
        core_data[core].host_context.reset();
    }
    num_cores = 0;
    gpu_barrier.reset();
}

void CpuManager::GuestThreadFunction(Kernel::KernelCore& kernel) {
    if (is_multicore) {
        MultiCoreRunGuestThread(kernel);
    } else {
        SingleCoreRunGuestThread(kernel);
    }
}

void CpuManager::IdleThreadFunction(Kernel::KernelCore& kernel) {
    if (is_multicore) {
        MultiCoreRunIdleThread(kernel);
    } else {
        SingleCoreRunIdleThread(kernel);
    }
}

void CpuManager::ShutdownThreadFunction(Kernel::KernelCore& kernel) {
    ShutdownThread(kernel);
}

void CpuManager::HandleInterrupt(Kernel::KernelCore& kernel) {
    auto core_index = kernel.CurrentPhysicalCoreIndex();
    Kernel::KInterruptManager::HandleInterrupt(kernel, s32(core_index));
}

///////////////////////////////////////////////////////////////////////////////
///                             MultiCore                                   ///
///////////////////////////////////////////////////////////////////////////////

void CpuManager::MultiCoreRunGuestThread(Kernel::KernelCore& kernel) {
    // Similar to UserModeThreadStarter in HOS
    auto* thread = Kernel::GetCurrentThreadPointer(kernel);
    kernel.CurrentScheduler()->OnThreadStart(kernel);

    while (true) {
        auto* physical_core = &kernel.CurrentPhysicalCore();
        while (!physical_core->IsInterrupted()) {
            physical_core->RunThread(kernel, thread);
            physical_core = &kernel.CurrentPhysicalCore();
        }
        HandleInterrupt(kernel);
    }
}

void CpuManager::MultiCoreRunIdleThread(Kernel::KernelCore& kernel) {
    // Not accurate to HOS. Remove this entire method when singlecore is removed.
    // See notes in KScheduler::ScheduleImpl for more information about why this
    // is inaccurate.
    kernel.CurrentScheduler()->OnThreadStart(kernel);
    while (true) {
        auto& physical_core = kernel.CurrentPhysicalCore();
        if (!physical_core.IsInterrupted()) {
            physical_core.Idle(kernel);
        }
        HandleInterrupt(kernel);
    }
}

///////////////////////////////////////////////////////////////////////////////
///                             SingleCore                                   ///
///////////////////////////////////////////////////////////////////////////////

void CpuManager::SingleCoreRunGuestThread(Kernel::KernelCore& kernel) {
    auto* thread = Kernel::GetCurrentThreadPointer(kernel);
    kernel.CurrentScheduler()->OnThreadStart(kernel);

    while (true) {
        auto* physical_core = &kernel.CurrentPhysicalCore();
        if (!physical_core->IsInterrupted()) {
            physical_core->RunThread(kernel, thread);
            physical_core = &kernel.CurrentPhysicalCore();
        }

        kernel.SetIsPhantomModeForSingleCore(true);
        system.CoreTiming().Advance();
        kernel.SetIsPhantomModeForSingleCore(false);

        PreemptSingleCore(kernel);
        HandleInterrupt(kernel);
    }
}

void CpuManager::SingleCoreRunIdleThread(Kernel::KernelCore& kernel) {
    kernel.CurrentScheduler()->OnThreadStart(kernel);
    while (true) {
        PreemptSingleCore(kernel, false);
        system.CoreTiming().AddTicks(1000U);
        idle_count++;
        HandleInterrupt(kernel);
    }
}

void CpuManager::PreemptSingleCore(Kernel::KernelCore& kernel, bool from_running_environment) {
    if (idle_count >= active_core_count || from_running_environment) {
        if (!from_running_environment) {
            system.CoreTiming().Idle();
            idle_count = 0;
        }
        kernel.SetIsPhantomModeForSingleCore(true);
        system.CoreTiming().Advance();
        kernel.SetIsPhantomModeForSingleCore(false);
    }
    const std::size_t next_core = (current_core.load() + 1) % active_core_count;
    current_core.store(next_core);
    system.CoreTiming().ResetTicks();
    kernel.Scheduler(next_core).PreemptSingleCore(kernel);

    // We've now been scheduled again, and we may have exchanged schedulers.
    // Reload the scheduler in case it's different.
    if (!kernel.Scheduler(current_core.load()).IsIdle()) {
        idle_count = 0;
    }
}

void CpuManager::GuestActivate(Kernel::KernelCore& kernel) {
    // Similar to the HorizonKernelMain callback in HOS
    auto* scheduler = kernel.CurrentScheduler();
    scheduler->Activate(kernel);
    UNREACHABLE();
}

void CpuManager::ShutdownThread(Kernel::KernelCore& kernel) {
    auto* thread = kernel.GetCurrentEmuThread();
    auto core = is_multicore ? kernel.CurrentPhysicalCoreIndex() : 0;
    Common::Fiber::YieldTo(thread->GetHostContext(), *core_data[core].host_context);
    UNREACHABLE();
}

void CpuManager::RunThread(std::stop_token token, std::size_t core) {
    /// Initialization
    system.RegisterCoreThread(core);
    std::string name = is_multicore ? ("CPUCore_" + std::to_string(core)) : std::string{"CPUThread"};
    Common::SetCurrentThreadName(name.c_str());
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    Common::SetCurrentThreadToPerformanceCores();
    auto& data = core_data[core];
    data.host_context = Common::Fiber::ThreadToFiber();

    // Cleanup
    SCOPE_EXIT {
        data.host_context->Exit();
    };

    // Running
    if (!gpu_barrier->Sync(token)) {
        return;
    }

    if (!is_async_gpu && !is_multicore) {
        system.GPU().ObtainContext();
    }

    auto& kernel = system.Kernel();
    auto& scheduler = *kernel.CurrentScheduler();
    auto* thread = scheduler.GetSchedulerCurrentThread();
    Kernel::SetCurrentThread(kernel, thread);

    Common::Fiber::YieldTo(data.host_context, *thread->GetHostContext());
}

} // namespace Core
