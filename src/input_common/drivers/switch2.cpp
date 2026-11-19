// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <tuple>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/param_package.h"
#include "common/settings.h"
#include "common/thread.h"
#include "input_common/drivers/mouse.h"
#include "input_common/drivers/switch2.h"

namespace InputCommon {
namespace {

constexpr u32 GyroMouseTriggerMask = static_cast<u32>(Switch2::PadButton::GR) |
                                     static_cast<u32>(Switch2::PadButton::GL) |
                                     static_cast<u32>(Switch2::PadButton::RightSL);

constexpr std::array<Switch2::PadButton, 25> AllPadButtons{
    Switch2::PadButton::Y,       Switch2::PadButton::X,       Switch2::PadButton::B,
    Switch2::PadButton::A,       Switch2::PadButton::RightSR, Switch2::PadButton::RightSL,
    Switch2::PadButton::R,       Switch2::PadButton::ZR,      Switch2::PadButton::Minus,
    Switch2::PadButton::Plus,    Switch2::PadButton::StickR,  Switch2::PadButton::StickL,
    Switch2::PadButton::Home,    Switch2::PadButton::Capture, Switch2::PadButton::C,
    Switch2::PadButton::Down,    Switch2::PadButton::Up,      Switch2::PadButton::Right,
    Switch2::PadButton::Left,    Switch2::PadButton::LeftSR,  Switch2::PadButton::LeftSL,
    Switch2::PadButton::L,       Switch2::PadButton::ZL,      Switch2::PadButton::GR,
    Switch2::PadButton::GL,
};

constexpr std::array<Switch2::ControllerType, 5> PreSetControllerTypes{
    Switch2::ControllerType::Left, Switch2::ControllerType::Right, Switch2::ControllerType::Pro,
    Switch2::ControllerType::Dual, Switch2::ControllerType::GameCube,
};

int GetControllerPriority(Switch2::ControllerType type) {
    switch (type) {
    case Switch2::ControllerType::Pro:
        return 0;
    case Switch2::ControllerType::GameCube:
        return 1;
    case Switch2::ControllerType::Dual:
        return 2;
    case Switch2::ControllerType::Left:
        return 3;
    case Switch2::ControllerType::Right:
        return 4;
    case Switch2::ControllerType::None:
    default:
        return 5;
    }
}

Common::Input::BatteryLevel VoltageToBatteryLevel(f32 voltage) {
    if (voltage <= 0.0f) {
        return Common::Input::BatteryLevel::None;
    }
    if (voltage > 3.25f) {
        return Common::Input::BatteryLevel::Full;
    }
    if (voltage > 3.125f) {
        return Common::Input::BatteryLevel::Medium;
    }
    if (voltage > 2.95f) {
        return Common::Input::BatteryLevel::Low;
    }
    if (voltage > 2.75f) {
        return Common::Input::BatteryLevel::Critical;
    }
    return Common::Input::BatteryLevel::Empty;
}

u8 LedStatusToPlayerNumber(const Common::Input::LedStatus& led_status) {
    const u8 mask = static_cast<u8>((led_status.led_1 ? 0x01U : 0x00U) |
                                    (led_status.led_2 ? 0x02U : 0x00U) |
                                    (led_status.led_3 ? 0x04U : 0x00U) |
                                    (led_status.led_4 ? 0x08U : 0x00U));
    for (std::size_t i = 0; i < Switch2::LedPatterns.size(); ++i) {
        if (Switch2::LedPatterns[i] == mask) {
            return static_cast<u8>(i + 1);
        }
    }
    return 1;
}

u64 ComputeDeltaMicroseconds(std::chrono::steady_clock::time_point& last_timestamp,
                             std::chrono::steady_clock::time_point now) {
    u64 delta_us = 4000;
    if (last_timestamp != std::chrono::steady_clock::time_point{}) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(now - last_timestamp).count();
        delta_us = static_cast<u64>(
            std::clamp(elapsed, static_cast<std::chrono::microseconds::rep>(1),
                       static_cast<std::chrono::microseconds::rep>(100000)));
    }
    last_timestamp = now;
    return delta_us;
}

} // namespace

struct Switch2Controllers::CallbackGate {
    class InFlightGuard final {
    public:
        explicit InFlightGuard(std::shared_ptr<CallbackGate> gate_) : gate{std::move(gate_)} {
            if (!gate) {
                return;
            }
            std::lock_guard lock{gate->mutex};
            if (gate->alive) {
                ++gate->in_flight;
                entered = true;
            }
        }

        ~InFlightGuard() {
            if (!entered) {
                return;
            }
            std::lock_guard lock{gate->mutex};
            if (--gate->in_flight == 0 && !gate->alive) {
                gate->cv.notify_all();
            }
        }

        InFlightGuard(const InFlightGuard&) = delete;
        InFlightGuard& operator=(const InFlightGuard&) = delete;

        [[nodiscard]] bool Entered() const noexcept {
            return entered;
        }

    private:
        std::shared_ptr<CallbackGate> gate;
        bool entered{false};
    };

    std::mutex mutex;
    std::condition_variable cv;
    bool alive{true};
    std::size_t in_flight{0};
};

Switch2Controllers::Switch2Controllers(std::string input_engine_, Mouse* mouse_engine_)
    : InputEngine(std::move(input_engine_)), mouse_engine{mouse_engine_},
      callback_gate{std::make_shared<CallbackGate>()} {
    PreSetController(GetIdentifier(0, Switch2::ControllerType::None));
    for (std::size_t slot_idx = 0; slot_idx < MaxSupportedControllers; ++slot_idx) {
        for (const auto ctrl_type : PreSetControllerTypes) {
            PreSetController(GetIdentifier(slot_idx, ctrl_type));
        }
    }

    if (!Settings::values.enable_switch2_driver.GetValue()) {
        return;
    }
    LOG_INFO(Input, "Switch 2 controller driver initialization started");
    Setup();
}

Switch2Controllers::~Switch2Controllers() {
    Shutdown();
}

void Switch2Controllers::Setup() {
    ble_scanner = Switch2::CreateBleScanner();
    if (ble_scanner) {
        ble_scanner->Start(
            [this, gate = callback_gate](std::shared_ptr<Switch2::ISwitch2Transport> transport) {
                CallbackGate::InFlightGuard guard{gate};
                if (guard.Entered()) {
                    OnTransportDiscovered(std::move(transport));
                }
            });
    }

    usb_scanner = Switch2::CreateUsbScanner();
    if (usb_scanner) {
        usb_scanner->Start(
            [this, gate = callback_gate](std::shared_ptr<Switch2::ISwitch2Transport> transport) {
                CallbackGate::InFlightGuard guard{gate};
                if (guard.Entered()) {
                    OnTransportDiscovered(std::move(transport));
                }
            });
    }

    mouse_thread =
        std::jthread([this](std::stop_token stop_token) { MouseInterpolationThread(stop_token); });
}

void Switch2Controllers::Shutdown() {
    mouse_thread = {};

    if (callback_gate) {
        std::lock_guard lock{callback_gate->mutex};
        callback_gate->alive = false;
    }

    if (ble_scanner) {
        ble_scanner->Stop();
        ble_scanner.reset();
    }
    if (usb_scanner) {
        usb_scanner->Stop();
        usb_scanner.reset();
    }

    std::vector<std::shared_ptr<Switch2::Switch2Driver>> active_drivers;
    {
        std::scoped_lock lock{slots_mutex};
        for (auto& slot : slots) {
            if (slot.primary) {
                active_drivers.push_back(std::move(slot.primary));
            }
            if (slot.secondary) {
                active_drivers.push_back(std::move(slot.secondary));
            }
            slot = LogicalSlot{};
        }
    }

    for (const auto& driver : active_drivers) {
        if (driver) {
            driver->Stop();
        }
    }

    if (callback_gate) {
        std::unique_lock lock{callback_gate->mutex};
        callback_gate->cv.wait(lock, [this] { return callback_gate->in_flight == 0; });
    }

    std::scoped_lock mouse_lock{mouse_mutex};
    mouse_active = false;
    mouse_vel_x = 0.0f;
    mouse_vel_y = 0.0f;
    mouse_pos_x = 0.5f;
    mouse_pos_y = 0.5f;
    if (mouse_engine != nullptr) {
        bool released_any = false;
        if (prev_mouse_left) {
            mouse_engine->ReleaseButton(MouseButton::Left);
            released_any = true;
        }
        if (prev_mouse_middle) {
            mouse_engine->ReleaseButton(MouseButton::Wheel);
            released_any = true;
        }
        if (prev_mouse_right) {
            mouse_engine->ReleaseButton(MouseButton::Right);
            released_any = true;
        }
        if (released_any) {
            mouse_engine->NotifyChanged();
        }
    }
    prev_mouse_left = false;
    prev_mouse_middle = false;
    prev_mouse_right = false;
}

void Switch2Controllers::OnTransportDiscovered(
    std::shared_ptr<Switch2::ISwitch2Transport> transport) {
    if (!transport) {
        return;
    }

    const std::string address = transport->GetAddress();
    {
        std::scoped_lock lock{slots_mutex};
        if (FindSlotByAddressLocked(address).has_value()) {
            return;
        }
    }

    auto driver = std::make_shared<Switch2::Switch2Driver>(std::move(transport));
    RegisterDriver(std::move(driver));
}

void Switch2Controllers::RegisterDriver(std::shared_ptr<Switch2::Switch2Driver> driver) {
    if (!driver) {
        return;
    }

    const auto init_result = driver->Initialize(1, false);
    if (init_result != Common::Input::DriverResult::Success) {
        LOG_WARNING(Input, "Failed to initialize Switch 2 controller {}", driver->GetAddress());
        driver->Stop();
        return;
    }

    const std::string address = driver->GetAddress();
    const auto driver_type = driver->GetControllerType();
    if (driver_type == Switch2::ControllerType::None) {
        LOG_WARNING(Input, "Unsupported Switch 2 controller type for {}", address);
        driver->Stop();
        return;
    }

    std::vector<std::pair<std::size_t, std::shared_ptr<Switch2::Switch2Driver>>> rebind_list;
    bool early_disconnect = false;
    bool duplicate = false;
    bool no_slot = false;

    {
        std::scoped_lock lock{slots_mutex};
        if (!driver->IsConnected()) {
            early_disconnect = true;
        } else if (FindSlotByAddressLocked(address).has_value()) {
            duplicate = true;
        } else {
            std::optional<std::size_t> target_slot;

            if (Settings::values.switch2_combine_joycons.GetValue() &&
                !driver->WasSideButtonPressed()) {
                if (driver_type == Switch2::ControllerType::Left ||
                    driver_type == Switch2::ControllerType::Right) {
                    const auto partner_type = driver_type == Switch2::ControllerType::Left
                                                  ? Switch2::ControllerType::Right
                                                  : Switch2::ControllerType::Left;
                    for (std::size_t i = 0; i < MaxSupportedControllers; ++i) {
                        const auto& candidate = slots[i];
                        // A controller that is gone is not a partner: its slot would keep
                        // waiting for input that never arrives.
                        if (candidate.primary && !candidate.secondary &&
                            candidate.primary->IsConnected() &&
                            candidate.primary->GetControllerType() == partner_type) {
                            target_slot = i;
                            break;
                        }
                    }
                }
            }

            if (target_slot.has_value()) {
                auto& slot = slots[*target_slot];
                slot.secondary = driver;
                slot.horizontal_hold = false;
                slot.left_buttons = 0;
                slot.right_buttons = 0;
                slot.left_gyro_trigger = false;
                slot.right_gyro_trigger = false;
                slot.primary->SetHorizontalHold(false);
                slot.secondary->SetHorizontalHold(false);
            } else {
                const auto free_slot = FindFreeSlotLocked();
                if (!free_slot.has_value()) {
                    no_slot = true;
                } else {
                    auto& slot = slots[*free_slot];
                    slot = LogicalSlot{};
                    slot.primary = driver;
                    slot.horizontal_hold = (driver_type == Switch2::ControllerType::Left ||
                                            driver_type == Switch2::ControllerType::Right);
                    slot.primary->SetHorizontalHold(slot.horizontal_hold);
                }
            }

            if (!no_slot) {
                ReorderSlotsLocked();
                CollectActiveSlotDriversLocked(rebind_list);
            }
        }
    }

    if (early_disconnect || duplicate || no_slot) {
        if (no_slot) {
            LOG_WARNING(Input, "No free Switch 2 controller slots available for {}", address);
        }
        driver->Stop();
        return;
    }

    ApplyReorderedCallbacksAndLeds(rebind_list);

    // If a controller disconnected during Initialize() or before BindDriverCallbacks()
    // completed, its disconnect callback fired while on_disconnect was still null.
    for (const auto& [slot_idx, slot_driver] : rebind_list) {
        (void)slot_idx;
        if (slot_driver && !slot_driver->IsConnected()) {
            UnregisterDriver(slot_driver->GetAddress());
        }
    }

    if (!driver->IsConnected()) {
        return;
    }

    LOG_INFO(Input, "Registered Switch 2 controller {} ({})", address,
             Switch2::ProductName(driver->GetProductId()));
}

void Switch2Controllers::UnregisterDriver(const std::string& address) {
    std::shared_ptr<Switch2::Switch2Driver> removed_driver;
    std::vector<std::pair<std::size_t, std::shared_ptr<Switch2::Switch2Driver>>> rebind_list;

    {
        std::scoped_lock lock{slots_mutex};
        const auto found_slot = FindSlotByAddressLocked(address);
        if (!found_slot.has_value()) {
            return;
        }

        auto& slot = slots[*found_slot];
        if (slot.primary && slot.primary->GetAddress() == address) {
            removed_driver = std::move(slot.primary);
            if (slot.secondary) {
                slot.primary = std::move(slot.secondary);
                slot.secondary.reset();
                const auto remaining_type = slot.primary->GetControllerType();
                slot.horizontal_hold = (remaining_type == Switch2::ControllerType::Left ||
                                        remaining_type == Switch2::ControllerType::Right);
                slot.primary->SetHorizontalHold(slot.horizontal_hold);
                slot.left_buttons = 0;
                slot.right_buttons = 0;
                slot.left_gyro_trigger = false;
                slot.right_gyro_trigger = false;
            } else {
                slot = LogicalSlot{};
            }
        } else if (slot.secondary && slot.secondary->GetAddress() == address) {
            removed_driver = std::move(slot.secondary);
            slot.secondary.reset();
            const auto remaining_type = slot.primary->GetControllerType();
            slot.horizontal_hold = (remaining_type == Switch2::ControllerType::Left ||
                                    remaining_type == Switch2::ControllerType::Right);
            slot.primary->SetHorizontalHold(slot.horizontal_hold);
            slot.left_buttons = 0;
            slot.right_buttons = 0;
            slot.left_gyro_trigger = false;
            slot.right_gyro_trigger = false;
        }

        ReorderSlotsLocked();
        CollectActiveSlotDriversLocked(rebind_list);
    }

    if (removed_driver) {
        removed_driver->Stop();
    }

    ApplyReorderedCallbacksAndLeds(rebind_list);

    {
        std::scoped_lock mouse_lock{mouse_mutex};
        mouse_active = false;
        mouse_vel_x = 0.0f;
        mouse_vel_y = 0.0f;
        mouse_pos_x = 0.5f;
        mouse_pos_y = 0.5f;
        if (mouse_engine != nullptr) {
            bool released_any = false;
            if (prev_mouse_left) {
                mouse_engine->ReleaseButton(MouseButton::Left);
                released_any = true;
            }
            if (prev_mouse_middle) {
                mouse_engine->ReleaseButton(MouseButton::Wheel);
                released_any = true;
            }
            if (prev_mouse_right) {
                mouse_engine->ReleaseButton(MouseButton::Right);
                released_any = true;
            }
            if (released_any) {
                mouse_engine->NotifyChanged();
            }
        }
        prev_mouse_left = false;
        prev_mouse_middle = false;
        prev_mouse_right = false;
    }

    LOG_INFO(Input, "Unregistered Switch 2 controller {}", address);
}

void Switch2Controllers::ReorderSlotsLocked() {
    std::vector<LogicalSlot> active_slots;
    active_slots.reserve(MaxSupportedControllers);

    for (auto& slot : slots) {
        if (slot.primary) {
            active_slots.push_back(std::move(slot));
        }
        slot = LogicalSlot{};
    }

    std::stable_sort(active_slots.begin(), active_slots.end(),
                     [](const LogicalSlot& a, const LogicalSlot& b) {
                         return GetControllerPriority(GetSlotType(a)) <
                                GetControllerPriority(GetSlotType(b));
                     });

    for (std::size_t i = 0; i < active_slots.size() && i < MaxSupportedControllers; ++i) {
        slots[i] = std::move(active_slots[i]);
        slots[i].player_number = i + 1;
    }
}

void Switch2Controllers::CollectActiveSlotDriversLocked(
    std::vector<std::pair<std::size_t, std::shared_ptr<Switch2::Switch2Driver>>>& out_drivers)
    const {
    for (std::size_t i = 0; i < MaxSupportedControllers; ++i) {
        if (slots[i].primary) {
            out_drivers.emplace_back(i, slots[i].primary);
        }
        if (slots[i].secondary) {
            out_drivers.emplace_back(i, slots[i].secondary);
        }
    }
}

void Switch2Controllers::ApplyReorderedCallbacksAndLeds(
    const std::vector<std::pair<std::size_t, std::shared_ptr<Switch2::Switch2Driver>>>&
        rebind_list) {
    for (const auto& [slot_idx, slot_driver] : rebind_list) {
        if (slot_driver->IsConnected()) {
            (void)slot_driver->SetLeds(static_cast<u8>(slot_idx + 1), false);
        }
        BindDriverCallbacks(slot_idx, slot_driver);
        HandleDriverColor(slot_idx, slot_driver->GetControllerType(),
                          slot_driver->GetControllerInfo());
    }
}

void Switch2Controllers::BindDriverCallbacks(
    std::size_t slot_index, const std::shared_ptr<Switch2::Switch2Driver>& driver) {
    if (!driver) {
        return;
    }

    const std::string address = driver->GetAddress();
    const Switch2::ControllerType driver_type = driver->GetControllerType();
    auto gate = callback_gate;
    driver->SetCallbacks(Switch2::Switch2Callbacks{
        .on_input_report =
            [this, gate, address](const Switch2::Switch2InputReport& report) {
                CallbackGate::InFlightGuard guard{gate};
                if (guard.Entered()) {
                    HandleDriverInputReport(address, report);
                }
            },
        .on_controller_info =
            [this, gate, slot_index, driver_type](const Switch2::ControllerInfo& info) {
                CallbackGate::InFlightGuard guard{gate};
                if (guard.Entered()) {
                    HandleDriverColor(slot_index, driver_type, info);
                }
            },
        .on_disconnect =
            [this, gate, address]() {
                CallbackGate::InFlightGuard guard{gate};
                if (guard.Entered()) {
                    HandleDriverDisconnect(address);
                }
            },
    });
}

void Switch2Controllers::HandleDriverInputReport(const std::string& address,
                                                 const Switch2::Switch2InputReport& report) {
    std::shared_ptr<Switch2::Switch2Driver> driver;
    std::size_t slot_idx = 0;
    Switch2::ControllerType slot_type = Switch2::ControllerType::None;
    Switch2::ControllerType driver_type = Switch2::ControllerType::None;
    u64 delta_us = 4000;
    bool shared_gyro_trigger = false;
    u32 merged_dual_buttons = 0;
    const auto now = std::chrono::steady_clock::now();

    {
        std::scoped_lock lock{slots_mutex};
        const auto found_slot = FindSlotByAddressLocked(address);
        if (!found_slot.has_value()) {
            return;
        }

        slot_idx = *found_slot;
        auto& slot = slots[slot_idx];
        if (slot.primary && slot.primary->GetAddress() == address) {
            driver = slot.primary;
        } else if (slot.secondary && slot.secondary->GetAddress() == address) {
            driver = slot.secondary;
        }
        if (!driver) {
            return;
        }

        slot_type = GetSlotType(slot);
        driver_type = driver->GetControllerType();

        const bool own_trigger = (report.buttons & GyroMouseTriggerMask) != 0;
        if (driver_type == Switch2::ControllerType::Left) {
            slot.left_buttons = report.buttons;
            slot.left_gyro_trigger = own_trigger;
            delta_us = ComputeDeltaMicroseconds(slot.last_left_motion, now);
        } else if (driver_type == Switch2::ControllerType::Right) {
            slot.right_buttons = report.buttons;
            slot.right_gyro_trigger = own_trigger;
            delta_us = ComputeDeltaMicroseconds(slot.last_right_motion, now);
        } else {
            slot.left_buttons = report.buttons;
            slot.left_gyro_trigger = own_trigger;
            delta_us = ComputeDeltaMicroseconds(slot.last_left_motion, now);
        }

        if (slot_type == Switch2::ControllerType::Dual) {
            shared_gyro_trigger = slot.left_gyro_trigger || slot.right_gyro_trigger;
            merged_dual_buttons = slot.left_buttons | slot.right_buttons;
        } else {
            shared_gyro_trigger = own_trigger;
            merged_dual_buttons = report.buttons;
        }
    }

    const bool is_dual = slot_type == Switch2::ControllerType::Dual;
    const PadIdentifier target_id = GetIdentifier(slot_idx, driver_type);

    // NPad reads native JoyconLeft (D-pad, LeftSL/SR, L/ZL, Minus, StickL, LStick) and
    // JoyconRight (A/B/X/Y, RightSL/SR, R/ZR, Plus, StickR, RStick) inputs directly with
    // is_horizontal=1, so single Joy-Con buttons and sticks must not be rotated onto the
    // opposite side's mask or axes.
    const u32 mapped_buttons = report.buttons;
    f32 mapped_lx = report.left_x;
    const f32 mapped_ly = report.left_y;
    f32 mapped_rx = report.right_x;
    const f32 mapped_ry = report.right_y;

    Switch2::GyroMouseOutput gyro_mouse{};
    if (!is_dual || driver_type == Switch2::ControllerType::Right) {
        Switch2::Switch2InputReport gyro_report = report;
        if (is_dual) {
            gyro_report.buttons = merged_dual_buttons;
        }
        gyro_mouse = driver->ProcessGyroMouse(gyro_report, shared_gyro_trigger);
    }

    const auto optical_mouse = driver->ProcessOpticalMouse(report);

    if (gyro_mouse.active && gyro_mouse.roll_steer_x.has_value()) {
        if (!is_dual && driver_type == Switch2::ControllerType::Right) {
            mapped_rx = *gyro_mouse.roll_steer_x;
        } else {
            mapped_lx = *gyro_mouse.roll_steer_x;
        }
    }

    for (const auto pad_button : AllPadButtons) {
        const bool pressed = (mapped_buttons & static_cast<u32>(pad_button)) != 0;
        SetButton(target_id, static_cast<int>(pad_button), pressed);
    }

    SetAxis(target_id, static_cast<int>(Switch2::PadAxes::LeftStickX), mapped_lx);
    SetAxis(target_id, static_cast<int>(Switch2::PadAxes::LeftStickY), mapped_ly);
    SetAxis(target_id, static_cast<int>(Switch2::PadAxes::RightStickX), mapped_rx);
    SetAxis(target_id, static_cast<int>(Switch2::PadAxes::RightStickY), mapped_ry);

    SetBattery(target_id, VoltageToBatteryLevel(report.battery_voltage));

    const auto motion = driver->ConvertToMotion(report, delta_us);
    const BasicMotion basic_motion{
        .gyro_x = motion.gyro_x,
        .gyro_y = motion.gyro_y,
        .gyro_z = motion.gyro_z,
        .accel_x = motion.accel_x,
        .accel_y = motion.accel_y,
        .accel_z = motion.accel_z,
        .delta_timestamp = motion.delta_timestamp,
    };

    if (is_dual) {
        const int motion_id = driver_type == Switch2::ControllerType::Left
                                  ? static_cast<int>(Switch2::PadMotion::LeftMotion)
                                  : static_cast<int>(Switch2::PadMotion::RightMotion);
        SetMotion(target_id, motion_id, basic_motion);
    } else {
        SetMotion(target_id, static_cast<int>(Switch2::PadMotion::LeftMotion), basic_motion);
        SetMotion(target_id, static_cast<int>(Switch2::PadMotion::RightMotion), basic_motion);
    }

    const bool should_update_mouse =
        !is_dual || driver_type == Switch2::ControllerType::Right || optical_mouse.active;
    if (should_update_mouse) {
        const f32 optical_sens =
            static_cast<f32>(Settings::values.switch2_mouse_sensitivity.GetValue()) / 10.0f;
        const bool any_mouse_active = gyro_mouse.active || optical_mouse.active;

        std::scoped_lock mouse_lock{mouse_mutex};
        mouse_active = any_mouse_active;
        if (any_mouse_active) {
            const f32 gyro_vx = gyro_mouse.active ? gyro_mouse.velocity_x : 0.0f;
            const f32 gyro_vy = gyro_mouse.active ? gyro_mouse.velocity_y : 0.0f;
            const f32 opt_vx =
                optical_mouse.active
                    ? static_cast<f32>(optical_mouse.delta_x) * optical_sens * 0.009f
                    : 0.0f;
            const f32 opt_vy =
                optical_mouse.active
                    ? static_cast<f32>(optical_mouse.delta_y) * optical_sens * 0.009f
                    : 0.0f;
            mouse_vel_x = gyro_vx + opt_vx;
            mouse_vel_y = gyro_vy + opt_vy;
        } else {
            mouse_vel_x = 0.0f;
            mouse_vel_y = 0.0f;
        }

        if (mouse_engine != nullptr) {
            const bool want_left = (gyro_mouse.active && gyro_mouse.left_click) ||
                                   (optical_mouse.active && optical_mouse.left_button);
            const bool want_middle = optical_mouse.active && optical_mouse.middle_button;
            const bool want_right = (gyro_mouse.active && gyro_mouse.right_click) ||
                                    (optical_mouse.active && optical_mouse.right_button);

            bool changed = false;
            if (want_left != prev_mouse_left) {
                if (want_left) {
                    mouse_engine->PressMouseButton(MouseButton::Left);
                } else {
                    mouse_engine->ReleaseButton(MouseButton::Left);
                }
                prev_mouse_left = want_left;
                changed = true;
            }
            if (want_middle != prev_mouse_middle) {
                if (want_middle) {
                    mouse_engine->PressMouseButton(MouseButton::Wheel);
                } else {
                    mouse_engine->ReleaseButton(MouseButton::Wheel);
                }
                prev_mouse_middle = want_middle;
                changed = true;
            }
            if (want_right != prev_mouse_right) {
                if (want_right) {
                    mouse_engine->PressMouseButton(MouseButton::Right);
                } else {
                    mouse_engine->ReleaseButton(MouseButton::Right);
                }
                prev_mouse_right = want_right;
                changed = true;
            }
            if (optical_mouse.active && optical_mouse.scroll_y != 0.0f) {
                mouse_engine->MouseWheelChange(
                    0, static_cast<int>(std::lround(optical_mouse.scroll_y * 30.0f)));
                changed = true;
            }
            if (changed) {
                mouse_engine->NotifyChanged();
            }
        }
    }
}

void Switch2Controllers::HandleDriverColor(std::size_t slot_index, Switch2::ControllerType type,
                                           const Switch2::ControllerInfo& info) {
    const auto identifier = GetIdentifier(slot_index, type);
    const Common::Input::BodyColorStatus color{
        .body = info.body_color,
        .buttons = info.buttons_color,
        .left_grip = info.left_grip_color,
        .right_grip = info.right_grip_color,
    };
    SetColor(identifier, color);
}

void Switch2Controllers::HandleDriverDisconnect(const std::string& address) {
    UnregisterDriver(address);
}

void Switch2Controllers::MouseInterpolationThread(std::stop_token stop_token) {
    Common::SetCurrentThreadName("Switch2MouseInterp");
    auto last_time = std::chrono::steady_clock::now();
    f32 residual_x = 0.0f;
    f32 residual_y = 0.0f;

    while (!stop_token.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        if (stop_token.stop_requested()) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        f32 dt = std::chrono::duration<f32>(now - last_time).count();
        last_time = now;
        if (dt > 0.05f) {
            dt = 0.015f;
        }

        // InputCommon::Mouse is not internally synchronized, so all mouse_engine
        // accesses must hold mouse_mutex.
        std::scoped_lock lock{mouse_mutex};
        if (!mouse_active) {
            residual_x = 0.0f;
            residual_y = 0.0f;
            continue;
        }

        const f32 time_scale = dt / 0.001f;
        const f32 total_dx = mouse_vel_x * time_scale + residual_x;
        const f32 total_dy = mouse_vel_y * time_scale + residual_y;
        const int move_x = static_cast<int>(total_dx);
        const int move_y = static_cast<int>(total_dy);
        residual_x = total_dx - static_cast<f32>(move_x);
        residual_y = total_dy - static_cast<f32>(move_y);

        if (mouse_engine != nullptr && (move_x != 0 || move_y != 0)) {
            if (Settings::values.mouse_enabled.GetValue()) {
                mouse_pos_x =
                    std::clamp(mouse_pos_x + static_cast<f32>(move_x) / 1280.0f, 0.0f, 1.0f);
                mouse_pos_y =
                    std::clamp(mouse_pos_y + static_cast<f32>(move_y) / 720.0f, 0.0f, 1.0f);
                mouse_engine->MouseMove(mouse_pos_x, mouse_pos_y);
            } else {
                // Mouse::Move updates panning/motion when mouse_panning is enabled without
                // requiring PressButton drag state.
                mouse_engine->Move(move_x, move_y, 0, 0);
            }
            mouse_engine->NotifyChanged();
        }
    }
}

std::optional<std::size_t> Switch2Controllers::FindSlotByAddressLocked(
    const std::string& address) const {
    for (std::size_t i = 0; i < MaxSupportedControllers; ++i) {
        const auto& slot = slots[i];
        if (slot.primary && slot.primary->GetAddress() == address) {
            return i;
        }
        if (slot.secondary && slot.secondary->GetAddress() == address) {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> Switch2Controllers::FindFreeSlotLocked() const {
    for (std::size_t i = 0; i < MaxSupportedControllers; ++i) {
        if (slots[i].primary == nullptr) {
            return i;
        }
    }
    return std::nullopt;
}

std::shared_ptr<Switch2::Switch2Driver> Switch2Controllers::GetDriverForIdentifier(
    const PadIdentifier& identifier) const {
    if (identifier.port >= MaxSupportedControllers) {
        return nullptr;
    }

    std::scoped_lock lock{slots_mutex};
    const auto& slot = slots[identifier.port];
    if (!slot.primary) {
        return nullptr;
    }

    const auto requested_type = static_cast<Switch2::ControllerType>(identifier.pad);
    if (slot.secondary != nullptr) {
        if (slot.primary->GetControllerType() == requested_type && slot.primary->IsConnected()) {
            return slot.primary;
        }
        if (slot.secondary->GetControllerType() == requested_type &&
            slot.secondary->IsConnected()) {
            return slot.secondary;
        }
        if (requested_type == Switch2::ControllerType::Dual && slot.primary->IsConnected()) {
            return slot.primary;
        }
        return nullptr;
    }

    if (!slot.primary->IsConnected()) {
        return nullptr;
    }
    if (slot.primary->GetControllerType() == requested_type ||
        requested_type == Switch2::ControllerType::None) {
        return slot.primary;
    }
    return nullptr;
}

bool Switch2Controllers::IsVibrationEnabled(const PadIdentifier& identifier) {
    const auto driver = GetDriverForIdentifier(identifier);
    return driver != nullptr && driver->IsConnected();
}

Common::Input::DriverResult Switch2Controllers::SetVibration(
    const PadIdentifier& identifier, const Common::Input::VibrationStatus& vibration) {
    const auto driver = GetDriverForIdentifier(identifier);
    if (driver == nullptr) {
        return Common::Input::DriverResult::InvalidHandle;
    }
    return driver->SetVibration(vibration);
}

Common::Input::DriverResult Switch2Controllers::SetLeds(
    const PadIdentifier& identifier, const Common::Input::LedStatus& led_status) {
    const auto driver = GetDriverForIdentifier(identifier);
    if (driver == nullptr) {
        return Common::Input::DriverResult::InvalidHandle;
    }
    return driver->SetLeds(LedStatusToPlayerNumber(led_status), false);
}

Common::Input::DriverResult Switch2Controllers::SetPollingMode(
    const PadIdentifier& identifier, Common::Input::PollingMode polling_mode) {
    const auto driver = GetDriverForIdentifier(identifier);
    if (driver == nullptr) {
        return Common::Input::DriverResult::InvalidHandle;
    }

    switch (polling_mode) {
    case Common::Input::PollingMode::Active:
    case Common::Input::PollingMode::Passive:
        return Common::Input::DriverResult::Success;
    case Common::Input::PollingMode::NFC:
    case Common::Input::PollingMode::IR:
    case Common::Input::PollingMode::Ring:
    default:
        return Common::Input::DriverResult::NotSupported;
    }
}

std::vector<Switch2DeviceBinding> Switch2Controllers::GetDeviceBindings() const {
    std::vector<Switch2DeviceBinding> bindings;
    std::scoped_lock lock{slots_mutex};

    for (std::size_t i = 0; i < MaxSupportedControllers; ++i) {
        const auto& slot = slots[i];
        if (!slot.primary || !slot.primary->IsConnected()) {
            continue;
        }

        const auto slot_type = GetSlotType(slot);
        if (slot_type == Switch2::ControllerType::Dual &&
            (!slot.secondary || !slot.secondary->IsConnected())) {
            continue;
        }

        Switch2DeviceBinding binding{};
        binding.info = BuildDeviceInfoLocked(i);
        if (slot_type == Switch2::ControllerType::Dual) {
            auto main_param = GetParamPackageLocked(i, Switch2::ControllerType::Left);
            const auto second_param = GetParamPackageLocked(i, Switch2::ControllerType::Right);
            main_param.Set("guid2", second_param.Get("guid", ""));
            main_param.Set("pad", std::to_string(static_cast<std::size_t>(slot_type)));
            binding.params = std::move(main_param);
        } else {
            binding.params = GetParamPackageLocked(i, slot_type);
        }
        bindings.push_back(std::move(binding));
    }

    return bindings;
}

std::vector<Common::ParamPackage> Switch2Controllers::GetInputDevices() const {
    std::vector<Common::ParamPackage> devices;
    for (const auto& binding : GetDeviceBindings()) {
        auto param = binding.params;
        param.Set("display", binding.info.display_name);
        devices.emplace_back(std::move(param));
    }

    return devices;
}

ButtonMapping Switch2Controllers::GetButtonMappingForDevice(const Common::ParamPackage& params) {
    static constexpr std::array<std::tuple<Settings::NativeButton::Values, Switch2::PadButton, bool>,
                                18>
        switch_to_pad_button{
            std::tuple{Settings::NativeButton::A, Switch2::PadButton::A, true},
            {Settings::NativeButton::B, Switch2::PadButton::B, true},
            {Settings::NativeButton::X, Switch2::PadButton::X, true},
            {Settings::NativeButton::Y, Switch2::PadButton::Y, true},
            {Settings::NativeButton::DLeft, Switch2::PadButton::Left, false},
            {Settings::NativeButton::DUp, Switch2::PadButton::Up, false},
            {Settings::NativeButton::DRight, Switch2::PadButton::Right, false},
            {Settings::NativeButton::DDown, Switch2::PadButton::Down, false},
            {Settings::NativeButton::L, Switch2::PadButton::L, false},
            {Settings::NativeButton::R, Switch2::PadButton::R, true},
            {Settings::NativeButton::ZL, Switch2::PadButton::ZL, false},
            {Settings::NativeButton::ZR, Switch2::PadButton::ZR, true},
            {Settings::NativeButton::Plus, Switch2::PadButton::Plus, true},
            {Settings::NativeButton::Minus, Switch2::PadButton::Minus, false},
            {Settings::NativeButton::Home, Switch2::PadButton::Home, true},
            {Settings::NativeButton::Screenshot, Switch2::PadButton::Capture, false},
            {Settings::NativeButton::LStick, Switch2::PadButton::StickL, false},
            {Settings::NativeButton::RStick, Switch2::PadButton::StickR, true},
        };

    if (!params.Has("port")) {
        return {};
    }

    const std::size_t port = static_cast<std::size_t>(params.Get("port", 0));
    const auto base_pad = static_cast<Switch2::ControllerType>(params.Get("pad", 0));

    ButtonMapping mapping{};
    for (const auto& [native_button, pad_button, side] : switch_to_pad_button) {
        auto pad = base_pad;
        if (pad == Switch2::ControllerType::Dual) {
            pad = side ? Switch2::ControllerType::Right : Switch2::ControllerType::Left;
        }
        Common::ParamPackage button_params = GetParamPackage(port, pad);
        button_params.Set("button", static_cast<int>(pad_button));
        mapping.insert_or_assign(native_button, std::move(button_params));
    }

    if (base_pad == Switch2::ControllerType::Left) {
        Common::ParamPackage sl_params = GetParamPackage(port, Switch2::ControllerType::Left);
        Common::ParamPackage sr_params = sl_params;
        sl_params.Set("button", static_cast<int>(Switch2::PadButton::LeftSL));
        sr_params.Set("button", static_cast<int>(Switch2::PadButton::LeftSR));
        mapping.insert_or_assign(Settings::NativeButton::SLLeft, std::move(sl_params));
        mapping.insert_or_assign(Settings::NativeButton::SRLeft, std::move(sr_params));
    } else if (base_pad == Switch2::ControllerType::Right) {
        Common::ParamPackage sl_params = GetParamPackage(port, Switch2::ControllerType::Right);
        Common::ParamPackage sr_params = sl_params;
        sl_params.Set("button", static_cast<int>(Switch2::PadButton::RightSL));
        sr_params.Set("button", static_cast<int>(Switch2::PadButton::RightSR));
        mapping.insert_or_assign(Settings::NativeButton::SLRight, std::move(sl_params));
        mapping.insert_or_assign(Settings::NativeButton::SRRight, std::move(sr_params));
    }

    return mapping;
}

AnalogMapping Switch2Controllers::GetAnalogMappingForDevice(const Common::ParamPackage& params) {
    if (!params.Has("port")) {
        return {};
    }

    const std::size_t port = static_cast<std::size_t>(params.Get("port", 0));
    auto pad_left = static_cast<Switch2::ControllerType>(params.Get("pad", 0));
    auto pad_right = pad_left;
    if (pad_left == Switch2::ControllerType::Dual) {
        pad_left = Switch2::ControllerType::Left;
        pad_right = Switch2::ControllerType::Right;
    }

    AnalogMapping mapping{};
    Common::ParamPackage left_analog_params = GetParamPackage(port, pad_left);
    left_analog_params.Set("axis_x", static_cast<int>(Switch2::PadAxes::LeftStickX));
    left_analog_params.Set("axis_y", static_cast<int>(Switch2::PadAxes::LeftStickY));
    mapping.insert_or_assign(Settings::NativeAnalog::LStick, std::move(left_analog_params));

    Common::ParamPackage right_analog_params = GetParamPackage(port, pad_right);
    right_analog_params.Set("axis_x", static_cast<int>(Switch2::PadAxes::RightStickX));
    right_analog_params.Set("axis_y", static_cast<int>(Switch2::PadAxes::RightStickY));
    mapping.insert_or_assign(Settings::NativeAnalog::RStick, std::move(right_analog_params));
    return mapping;
}

MotionMapping Switch2Controllers::GetMotionMappingForDevice(const Common::ParamPackage& params) {
    if (!params.Has("port")) {
        return {};
    }

    const std::size_t port = static_cast<std::size_t>(params.Get("port", 0));
    auto pad_left = static_cast<Switch2::ControllerType>(params.Get("pad", 0));
    auto pad_right = pad_left;
    if (pad_left == Switch2::ControllerType::Dual) {
        pad_left = Switch2::ControllerType::Left;
        pad_right = Switch2::ControllerType::Right;
    }

    MotionMapping mapping{};
    Common::ParamPackage left_motion_params = GetParamPackage(port, pad_left);
    left_motion_params.Set("motion", static_cast<int>(Switch2::PadMotion::LeftMotion));
    mapping.insert_or_assign(Settings::NativeMotion::MotionLeft, std::move(left_motion_params));

    Common::ParamPackage right_motion_params = GetParamPackage(port, pad_right);
    right_motion_params.Set("motion", static_cast<int>(Switch2::PadMotion::RightMotion));
    mapping.insert_or_assign(Settings::NativeMotion::MotionRight, std::move(right_motion_params));
    return mapping;
}

Common::Input::ButtonNames Switch2Controllers::GetUIName(const Common::ParamPackage& params) const {
    if (params.Has("button")) {
        return GetUIButtonName(params);
    }
    if (params.Has("axis")) {
        return Common::Input::ButtonNames::Value;
    }
    if (params.Has("motion")) {
        return Common::Input::ButtonNames::Engine;
    }
    return Common::Input::ButtonNames::Invalid;
}

Common::Input::ButtonNames Switch2Controllers::GetUIButtonName(
    const Common::ParamPackage& params) const {
    const auto button = static_cast<Switch2::PadButton>(params.Get("button", 0));
    switch (button) {
    case Switch2::PadButton::Left:
        return Common::Input::ButtonNames::ButtonLeft;
    case Switch2::PadButton::Right:
        return Common::Input::ButtonNames::ButtonRight;
    case Switch2::PadButton::Down:
        return Common::Input::ButtonNames::ButtonDown;
    case Switch2::PadButton::Up:
        return Common::Input::ButtonNames::ButtonUp;
    case Switch2::PadButton::LeftSL:
    case Switch2::PadButton::RightSL:
        return Common::Input::ButtonNames::TriggerSL;
    case Switch2::PadButton::LeftSR:
    case Switch2::PadButton::RightSR:
        return Common::Input::ButtonNames::TriggerSR;
    case Switch2::PadButton::L:
        return Common::Input::ButtonNames::TriggerL;
    case Switch2::PadButton::R:
        return Common::Input::ButtonNames::TriggerR;
    case Switch2::PadButton::ZL:
        return Common::Input::ButtonNames::TriggerZL;
    case Switch2::PadButton::ZR:
        return Common::Input::ButtonNames::TriggerZR;
    case Switch2::PadButton::A:
        return Common::Input::ButtonNames::ButtonA;
    case Switch2::PadButton::B:
        return Common::Input::ButtonNames::ButtonB;
    case Switch2::PadButton::X:
        return Common::Input::ButtonNames::ButtonX;
    case Switch2::PadButton::Y:
        return Common::Input::ButtonNames::ButtonY;
    case Switch2::PadButton::Plus:
        return Common::Input::ButtonNames::ButtonPlus;
    case Switch2::PadButton::Minus:
        return Common::Input::ButtonNames::ButtonMinus;
    case Switch2::PadButton::Home:
        return Common::Input::ButtonNames::ButtonHome;
    case Switch2::PadButton::Capture:
        return Common::Input::ButtonNames::ButtonCapture;
    case Switch2::PadButton::StickL:
        return Common::Input::ButtonNames::ButtonStickL;
    case Switch2::PadButton::StickR:
        return Common::Input::ButtonNames::ButtonStickR;
    case Switch2::PadButton::C:
    case Switch2::PadButton::GR:
    case Switch2::PadButton::GL:
        return Common::Input::ButtonNames::Value;
    case Switch2::PadButton::None:
    default:
        return Common::Input::ButtonNames::Undefined;
    }
}

std::vector<Switch2::Switch2DeviceInfo> Switch2Controllers::GetConnectedDevices() const {
    std::vector<Switch2::Switch2DeviceInfo> devices;
    std::scoped_lock lock{slots_mutex};

    for (std::size_t i = 0; i < MaxSupportedControllers; ++i) {
        const auto& slot = slots[i];
        if (!slot.primary || !slot.primary->IsConnected()) {
            continue;
        }

        devices.push_back(BuildDeviceInfoLocked(i));
    }

    return devices;
}

Switch2::Switch2DeviceInfo Switch2Controllers::BuildDeviceInfoLocked(
    std::size_t slot_index) const {
    const auto& slot = slots[slot_index];
    const auto slot_type = GetSlotType(slot);
    const auto pid = slot.primary->GetProductId();
    const bool calibrating =
        slot.primary->IsCalibrating() || (slot.secondary && slot.secondary->IsCalibrating());
    std::string addr = slot.primary->GetAddress();
    if (slot.secondary) {
        addr = fmt::format("{} + {}", addr, slot.secondary->GetAddress());
    }

    return Switch2::Switch2DeviceInfo{
        .slot_index = slot_index,
        .type = slot_type,
        .product_id = pid,
        .transport = slot.primary->GetTransportType(),
        .address = std::move(addr),
        .display_name =
            fmt::format("{} {}", GetControllerDisplayName(slot_type, pid), slot_index + 1),
        .horizontal_hold = slot.horizontal_hold,
        .is_calibrating = calibrating,
        .battery_voltage = slot.primary->GetBatteryVoltage(),
    };
}

bool Switch2Controllers::SplitSlot(std::size_t slot_index) {
    if (slot_index >= MaxSupportedControllers) {
        return false;
    }

    std::vector<std::pair<std::size_t, std::shared_ptr<Switch2::Switch2Driver>>> rebind_list;
    {
        std::scoped_lock lock{slots_mutex};
        auto& slot = slots[slot_index];
        if (!slot.primary || !slot.secondary) {
            return false;
        }

        const auto free_slot = FindFreeSlotLocked();
        if (!free_slot.has_value()) {
            return false;
        }

        auto second = std::move(slot.secondary);
        slot.secondary.reset();
        slot.horizontal_hold = true;
        slot.left_buttons = 0;
        slot.right_buttons = 0;
        slot.left_gyro_trigger = false;
        slot.right_gyro_trigger = false;
        slot.primary->SetHorizontalHold(true);

        auto& new_slot = slots[*free_slot];
        new_slot = LogicalSlot{};
        new_slot.primary = std::move(second);
        new_slot.horizontal_hold = true;
        new_slot.primary->SetHorizontalHold(true);

        ReorderSlotsLocked();
        CollectActiveSlotDriversLocked(rebind_list);
    }

    ApplyReorderedCallbacksAndLeds(rebind_list);
    return true;
}

bool Switch2Controllers::MergeSlots(std::size_t first_slot, std::size_t second_slot) {
    if (first_slot >= MaxSupportedControllers || second_slot >= MaxSupportedControllers ||
        first_slot == second_slot) {
        return false;
    }

    if (first_slot > second_slot) {
        std::swap(first_slot, second_slot);
    }

    std::vector<std::pair<std::size_t, std::shared_ptr<Switch2::Switch2Driver>>> rebind_list;
    {
        std::scoped_lock lock{slots_mutex};
        auto& slot1 = slots[first_slot];
        auto& slot2 = slots[second_slot];
        if (!slot1.primary || slot1.secondary || !slot2.primary || slot2.secondary) {
            return false;
        }

        const auto type1 = slot1.primary->GetControllerType();
        const auto type2 = slot2.primary->GetControllerType();
        const bool valid_pair =
            (type1 == Switch2::ControllerType::Left && type2 == Switch2::ControllerType::Right) ||
            (type1 == Switch2::ControllerType::Right && type2 == Switch2::ControllerType::Left);
        if (!valid_pair) {
            return false;
        }

        slot1.secondary = std::move(slot2.primary);
        slot2 = LogicalSlot{};
        slot1.horizontal_hold = false;
        slot1.left_buttons = 0;
        slot1.right_buttons = 0;
        slot1.left_gyro_trigger = false;
        slot1.right_gyro_trigger = false;
        slot1.primary->SetHorizontalHold(false);
        slot1.secondary->SetHorizontalHold(false);

        ReorderSlotsLocked();
        CollectActiveSlotDriversLocked(rebind_list);
    }

    ApplyReorderedCallbacksAndLeds(rebind_list);
    return true;
}

bool Switch2Controllers::CalibrateGyro(std::size_t slot_index) {
    if (slot_index >= MaxSupportedControllers) {
        return false;
    }

    std::scoped_lock lock{slots_mutex};
    const auto& slot = slots[slot_index];
    if (!slot.primary) {
        return false;
    }
    slot.primary->StartGyroCalibration();
    if (slot.secondary) {
        slot.secondary->StartGyroCalibration();
    }
    return true;
}

bool Switch2Controllers::SetSlotHorizontalHold(std::size_t slot_index, bool horizontal) {
    if (slot_index >= MaxSupportedControllers) {
        return false;
    }

    std::scoped_lock lock{slots_mutex};
    auto& slot = slots[slot_index];
    if (!slot.primary) {
        return false;
    }
    slot.horizontal_hold = horizontal;
    slot.primary->SetHorizontalHold(horizontal);
    if (slot.secondary) {
        slot.secondary->SetHorizontalHold(horizontal);
    }
    return true;
}

PadIdentifier Switch2Controllers::GetIdentifier(std::size_t slot_index,
                                                Switch2::ControllerType type) const {
    const std::array<u8, 16> guid{0, 0, 0, 0, 0, 0, 0, 0,
                                  0, 0, 0, 0, 0, 0, 1, static_cast<u8>(type)};
    return {
        .guid = Common::UUID{guid},
        .port = slot_index,
        .pad = static_cast<std::size_t>(type),
    };
}

Common::ParamPackage Switch2Controllers::GetParamPackage(std::size_t slot_index,
                                                         Switch2::ControllerType type) const {
    std::scoped_lock lock{slots_mutex};
    return GetParamPackageLocked(slot_index, type);
}

Common::ParamPackage Switch2Controllers::GetParamPackageLocked(
    std::size_t slot_index, Switch2::ControllerType type) const {
    const auto identifier = GetIdentifier(slot_index, type);
    Common::ParamPackage params{
        {"engine", GetEngineName()},
        {"guid", identifier.guid.RawString()},
        {"port", std::to_string(identifier.port)},
        {"pad", std::to_string(identifier.pad)},
    };

    const auto device_id = GetSlotDeviceIdLocked(slot_index);
    if (!device_id.empty()) {
        params.Set(std::string{Switch2::DeviceIdKey}, device_id);
    }

    return params;
}

std::string Switch2Controllers::GetSlotDeviceId(std::size_t slot_index) const {
    std::scoped_lock lock{slots_mutex};
    return GetSlotDeviceIdLocked(slot_index);
}

std::string Switch2Controllers::GetSlotDeviceIdLocked(std::size_t slot_index) const {
    if (slot_index >= MaxSupportedControllers) {
        return {};
    }

    const auto& slot = slots[slot_index];
    if (!slot.primary) {
        return {};
    }

    const auto info = slot.primary->GetControllerInfo();
    if (!info.serial_number.empty()) {
        return info.serial_number;
    }
    return slot.primary->GetAddress();
}

Switch2::ControllerType Switch2Controllers::GetSlotType(const LogicalSlot& slot) {
    if (!slot.primary) {
        return Switch2::ControllerType::None;
    }
    if (slot.secondary) {
        return Switch2::ControllerType::Dual;
    }
    return slot.primary->GetControllerType();
}

std::string Switch2Controllers::GetControllerDisplayName(Switch2::ControllerType type,
                                                         Switch2::ProductId pid) {
    switch (type) {
    case Switch2::ControllerType::Left:
        return "Switch 2 Left Joy-Con";
    case Switch2::ControllerType::Right:
        return "Switch 2 Right Joy-Con";
    case Switch2::ControllerType::Pro:
        return "Switch 2 Pro Controller";
    case Switch2::ControllerType::Dual:
        return "Switch 2 Dual Joy-Con";
    case Switch2::ControllerType::GameCube:
        return "NSO GameCube Controller";
    case Switch2::ControllerType::None:
    default:
        return std::string(Switch2::ProductName(pid));
    }
}

} // namespace InputCommon
