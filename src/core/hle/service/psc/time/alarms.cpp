// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/core.h"
#include "core/hle/service/psc/time/alarms.h"
#include "core/hle/service/psc/time/manager.h"

namespace Service::PSC::Time {
Alarm::Alarm(Core::System& system, KernelHelpers::ServiceContext& ctx, AlarmType type)
    : m_ctx{ctx}
    , m_event{ctx.CreateEvent("Psc:Alarm:Event")}
{
    m_event->Clear(system.Kernel());

    switch (type) {
    case WakeupAlarm:
        m_priority = 1;
        break;
    case BackgroundTaskAlarm:
        m_priority = 0;
        break;
    default:
        UNREACHABLE();
        return;
    }
}

Alarm::~Alarm() {
    m_ctx.CloseEvent(m_event);
}

Alarms::Alarms(Core::System& system, StandardSteadyClockCore& steady_clock, PowerStateRequestManager& power_state_request_manager)
    : m_system{system}
    , m_ctx{system, "Psc:Alarms"}
    , m_steady_clock{steady_clock}
    , m_power_state_request_manager{power_state_request_manager}
    , m_event{m_ctx.CreateEvent("Psc:Alarms:Event")}
{}

Alarms::~Alarms() {
    std::scoped_lock l{m_mutex};
    m_alarms.clear();
    m_closest_alarm = nullptr;
    m_ctx.CloseEvent(m_event);
}

Result Alarms::Enable(Kernel::KernelCore& kernel, Alarm& alarm, s64 time) {
    R_UNLESS(m_steady_clock.IsInitialized(), ResultClockUninitialized);

    std::scoped_lock l{m_mutex};
    if (alarm.IsLinked()) {
        Erase(kernel, alarm);
    }

    auto time_ns{time + m_steady_clock.GetRawTime()};
    auto one_second_ns{
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(1)).count()};
    time_ns = Common::AlignUp(time_ns, one_second_ns);
    alarm.SetAlertTime(time_ns);

    Insert(kernel, alarm);
    R_RETURN(UpdateClosestAndSignal(kernel));
}

void Alarms::Disable(Kernel::KernelCore& kernel, Alarm& alarm) {
    std::scoped_lock l{m_mutex};
    if (!alarm.IsLinked()) {
        return;
    }

    Erase(kernel, alarm);
    UpdateClosestAndSignal(kernel);
}

bool Alarms::IsEnabled(const Alarm& alarm) {
    std::scoped_lock l{m_mutex};
    return alarm.IsLinked();
}

void Alarms::CheckAndSignal(Kernel::KernelCore& kernel) {
    std::scoped_lock l{m_mutex};
    if (m_alarms.empty()) {
        return;
    }

    bool alarm_signalled{false};
    const s64 current_time{m_steady_clock.GetRawTime()};
    for (auto it = m_alarms.begin(); it != m_alarms.end();) {
        auto& alarm = *it;
        if (current_time >= alarm.GetAlertTime()) {
            alarm.Signal(kernel);
            alarm.Lock();
            it = m_alarms.erase(it);

            m_power_state_request_manager.UpdatePendingPowerStateRequestPriority(
                alarm.GetPriority());
            alarm_signalled = true;
        } else {
            ++it;
        }
    }

    if (!alarm_signalled) {
        return;
    }

    m_power_state_request_manager.SignalPowerStateRequestAvailability();
    UpdateClosestAndSignal(kernel);
}

bool Alarms::GetClosestAlarm(Alarm** out_alarm) {
    std::scoped_lock l{m_mutex};
    auto alarm = m_alarms.empty() ? nullptr : std::addressof(m_alarms.front());
    *out_alarm = alarm;
    return alarm != nullptr;
}

void Alarms::Insert(Kernel::KernelCore& kernel, Alarm& alarm) {
    // Alarms are sorted by alert time, then priority
    auto it{m_alarms.begin()};
    while (it != m_alarms.end()) {
        if (alarm.GetAlertTime() < it->GetAlertTime() ||
            (alarm.GetAlertTime() == it->GetAlertTime() &&
             alarm.GetPriority() < it->GetPriority())) {
            m_alarms.insert(it, alarm);
            return;
        }
        it++;
    }

    m_alarms.push_back(alarm);
}

void Alarms::Erase(Kernel::KernelCore& kernel, Alarm& alarm) {
    m_alarms.erase(m_alarms.iterator_to(alarm));
}

Result Alarms::UpdateClosestAndSignal(Kernel::KernelCore& kernel) {
    m_closest_alarm = m_alarms.empty() ? nullptr : std::addressof(m_alarms.front());
    R_SUCCEED_IF(m_closest_alarm == nullptr);

    m_event->Signal(kernel);

    R_SUCCEED();
}

IAlarmService::IAlarmService(Core::System& system_, std::shared_ptr<TimeManager> manager)
    : ServiceFramework{system_, "time:al"}
    , m_manager{std::move(manager)}
    , m_alarms{m_manager->m_alarms}
{
    // clang-format off
    static const FunctionInfo functions[] = {
        {0, &IAlarmService::CreateWakeupAlarm, "CreateWakeupAlarm"},
        {1, &IAlarmService::CreateBackgroundTaskAlarm, "CreateBackgroundTaskAlarm"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

void IAlarmService::CreateWakeupAlarm(HLERequestContext& ctx) {
    LOG_DEBUG(Service_Time, "called.");

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<ISteadyClockAlarm>(ctx, system, m_manager, AlarmType::WakeupAlarm);
}

void IAlarmService::CreateBackgroundTaskAlarm(HLERequestContext& ctx) {
    LOG_DEBUG(Service_Time, "called.");

    IPC::ResponseBuilder rb{ctx, 2, 0, 1};
    rb.Push(ResultSuccess);
    rb.PushIpcInterface<ISteadyClockAlarm>(ctx, system, m_manager, AlarmType::BackgroundTaskAlarm);
}

ISteadyClockAlarm::ISteadyClockAlarm(Core::System& system_, std::shared_ptr<TimeManager> manager,
                                     AlarmType type)
    : ServiceFramework{system_, "ISteadyClockAlarm"}, m_ctx{system, "Psc:ISteadyClockAlarm"},
      m_manager{std::move(manager)}, m_alarms{m_manager->m_alarms},
      m_alarm{system, m_ctx, type} {
    // clang-format off
    static const FunctionInfo functions[] = {
        {0,  &ISteadyClockAlarm::GetAlarmEvent, "GetAlarmEvent"},
        {1,  &ISteadyClockAlarm::Enable, "Enable"},
        {2,  &ISteadyClockAlarm::Disable, "Disable"},
        {3,  &ISteadyClockAlarm::IsEnabled, "IsEnabled"},
        {10, nullptr, "CreateWakeLock"},
        {11, nullptr, "DestroyWakeLock"},
    };
    // clang-format on
    RegisterHandlers(functions);
}

ISteadyClockAlarm::~ISteadyClockAlarm() {
    m_alarms.Disable(system.Kernel(), m_alarm);
}

void ISteadyClockAlarm::GetAlarmEvent(HLERequestContext& ctx) {
    LOG_DEBUG(Service_Time, "called.");

    IPC::ResponseBuilder rb{ctx, 2, 1};
    rb.Push(ResultSuccess);
    rb.PushCopyObjects(ctx, m_alarm.GetEventHandle());
}

void ISteadyClockAlarm::Enable(HLERequestContext& ctx) {
    LOG_DEBUG(Service_Time, "called.");

    IPC::RequestParser rp{ctx};
    auto time{rp.Pop<s64>()};

    auto res = m_alarms.Enable(system.Kernel(), m_alarm, time);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(res);
}

void ISteadyClockAlarm::Disable(HLERequestContext& ctx) {
    LOG_DEBUG(Service_Time, "called.");

    m_alarms.Disable(system.Kernel(), m_alarm);

    IPC::ResponseBuilder rb{ctx, 2};
    rb.Push(ResultSuccess);
}

void ISteadyClockAlarm::IsEnabled(HLERequestContext& ctx) {
    LOG_DEBUG(Service_Time, "called.");

    IPC::ResponseBuilder rb{ctx, 3};
    rb.Push(ResultSuccess);
    rb.Push<bool>(m_alarms.IsEnabled(m_alarm));
}

} // namespace Service::PSC::Time
