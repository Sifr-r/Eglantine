// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/polyfill_thread.h"
#include "input_common/helpers/switch2_protocol/switch2_driver.h"
#include "input_common/helpers/switch2_protocol/switch2_transport.h"
#include "input_common/helpers/switch2_protocol/switch2_types.h"
#include "input_common/input_engine.h"

namespace InputCommon {

class Mouse;

/// A connected Switch 2 controller together with the parameters that address it.
struct Switch2DeviceBinding {
    Switch2::Switch2DeviceInfo info{};
    /// Parameter package that addresses the controller: engine, guid, port, pad and device_id.
    Common::ParamPackage params{};
};

class Switch2Controllers final : public InputCommon::InputEngine {
public:
    explicit Switch2Controllers(std::string input_engine_, Mouse* mouse_engine_);
    ~Switch2Controllers() override;

    bool IsVibrationEnabled(const PadIdentifier& identifier) override;
    Common::Input::DriverResult SetVibration(
        const PadIdentifier& identifier, const Common::Input::VibrationStatus& vibration) override;
    Common::Input::DriverResult SetLeds(const PadIdentifier& identifier,
                                        const Common::Input::LedStatus& led_status) override;
    Common::Input::DriverResult SetPollingMode(
        const PadIdentifier& identifier, Common::Input::PollingMode polling_mode) override;

    std::vector<Common::ParamPackage> GetInputDevices() const override;
    ButtonMapping GetButtonMappingForDevice(const Common::ParamPackage& params) override;
    AnalogMapping GetAnalogMappingForDevice(const Common::ParamPackage& params) override;
    MotionMapping GetMotionMappingForDevice(const Common::ParamPackage& params) override;
    Common::Input::ButtonNames GetUIName(const Common::ParamPackage& params) const override;

    [[nodiscard]] std::vector<Switch2::Switch2DeviceInfo> GetConnectedDevices() const;
    /// Returns the connected controllers together with the parameter packages that address them.
    [[nodiscard]] std::vector<Switch2DeviceBinding> GetDeviceBindings() const;
    /// Returns the stable identity of the controller in the given slot: its serial number when
    /// the controller reports one, the transport address otherwise.
    [[nodiscard]] std::string GetSlotDeviceId(std::size_t slot_index) const;
    bool SplitSlot(std::size_t slot_index);
    bool MergeSlots(std::size_t first_slot, std::size_t second_slot);
    bool CalibrateGyro(std::size_t slot_index);
    bool SetSlotHorizontalHold(std::size_t slot_index, bool horizontal);

    void RegisterDriver(std::shared_ptr<Switch2::Switch2Driver> driver);
    void UnregisterDriver(const std::string& address);

private:
    static constexpr std::size_t MaxSupportedControllers = 8;

    struct LogicalSlot {
        std::shared_ptr<Switch2::Switch2Driver> primary;
        std::shared_ptr<Switch2::Switch2Driver> secondary;
        std::size_t player_number{1};
        bool horizontal_hold{false};
        u32 left_buttons{0};
        u32 right_buttons{0};
        bool left_gyro_trigger{false};
        bool right_gyro_trigger{false};
        f32 last_left_stick_x{0.0f};
        std::optional<f32> dual_roll_steer_x{};
        std::chrono::steady_clock::time_point last_left_motion{};
        std::chrono::steady_clock::time_point last_right_motion{};
    };

    struct CallbackGate;

    void Setup();
    void Shutdown();
    void OnTransportDiscovered(std::shared_ptr<Switch2::ISwitch2Transport> transport);
    void ReorderSlotsLocked();
    void ApplyReorderedCallbacksAndLeds(
        const std::vector<std::pair<std::size_t, std::shared_ptr<Switch2::Switch2Driver>>>&
            rebind_list);
    void CollectActiveSlotDriversLocked(
        std::vector<std::pair<std::size_t, std::shared_ptr<Switch2::Switch2Driver>>>& out_drivers)
        const;
    void BindDriverCallbacks(std::size_t slot_index,
                             const std::shared_ptr<Switch2::Switch2Driver>& driver);
    void HandleDriverInputReport(const std::string& address,
                                 const Switch2::Switch2InputReport& report);
    void HandleDriverColor(std::size_t slot_index, Switch2::ControllerType type,
                           const Switch2::ControllerInfo& info);
    void HandleDriverDisconnect(const std::string& address);
    void ResetMouseStateLocked(bool reset_cursor);
    void MouseInterpolationThread(std::stop_token stop_token);

    [[nodiscard]] std::optional<std::size_t> FindSlotByAddressLocked(
        const std::string& address) const;
    [[nodiscard]] std::optional<std::size_t> FindFreeSlotLocked() const;
    [[nodiscard]] std::shared_ptr<Switch2::Switch2Driver> GetDriverForIdentifier(
        const PadIdentifier& identifier) const;

    [[nodiscard]] PadIdentifier GetIdentifier(std::size_t slot_index,
                                              Switch2::ControllerType type) const;
    [[nodiscard]] Common::ParamPackage GetParamPackage(std::size_t slot_index,
                                                       Switch2::ControllerType type) const;
    /// Builds a parameter package without locking; the caller must hold slots_mutex.
    [[nodiscard]] Common::ParamPackage GetParamPackageLocked(std::size_t slot_index,
                                                            Switch2::ControllerType type) const;
    /// Returns the stable identity of a slot without locking; the caller must hold slots_mutex.
    [[nodiscard]] std::string GetSlotDeviceIdLocked(std::size_t slot_index) const;
    /// Builds the device info of a slot without locking; the caller must hold slots_mutex.
    [[nodiscard]] Switch2::Switch2DeviceInfo BuildDeviceInfoLocked(std::size_t slot_index) const;
    [[nodiscard]] static Switch2::ControllerType GetSlotType(const LogicalSlot& slot);
    [[nodiscard]] static std::string GetControllerDisplayName(Switch2::ControllerType type,
                                                                Switch2::ProductId pid);
    [[nodiscard]] Common::Input::ButtonNames GetUIButtonName(
        const Common::ParamPackage& params) const;

    Mouse* mouse_engine{nullptr};
    mutable std::mutex slots_mutex;
    // Qt defines `slots` as an empty macro, so this member is expanded away in
    // translation units that include a Qt header before this one.
#pragma push_macro("slots")
#undef slots
    std::array<LogicalSlot, MaxSupportedControllers> slots{};
#pragma pop_macro("slots")

    std::unique_ptr<Switch2::ISwitch2Scanner> ble_scanner;
    std::unique_ptr<Switch2::ISwitch2Scanner> usb_scanner;

    std::shared_ptr<CallbackGate> callback_gate;
    std::mutex mouse_mutex;
    std::string mouse_owner_address{};
    f32 mouse_vel_x{0.0f};
    f32 mouse_vel_y{0.0f};
    f32 mouse_pos_x{0.5f};
    f32 mouse_pos_y{0.5f};
    bool mouse_active{false};
    bool prev_mouse_left{false};
    bool prev_mouse_middle{false};
    bool prev_mouse_right{false};
    std::jthread mouse_thread;
};

} // namespace InputCommon
