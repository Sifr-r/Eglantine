// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <span>
#include <string>

#include "common/common_types.h"
#include "common/input.h"
#include "input_common/helpers/switch2_protocol/switch2_transport.h"
#include "input_common/helpers/switch2_protocol/switch2_types.h"

namespace InputCommon::Switch2 {

class Switch2Driver final : public std::enable_shared_from_this<Switch2Driver> {
public:
    explicit Switch2Driver(std::shared_ptr<ISwitch2Transport> transport_);
    ~Switch2Driver();

    Switch2Driver(const Switch2Driver&) = delete;
    Switch2Driver& operator=(const Switch2Driver&) = delete;

    Common::Input::DriverResult Initialize(u8 player_number, bool reverse_leds);
    void Stop();

    void SetCallbacks(Switch2Callbacks callbacks_);

    [[nodiscard]] bool IsConnected() const;
    [[nodiscard]] ProductId GetProductId() const;
    [[nodiscard]] ControllerType GetControllerType() const;
    [[nodiscard]] TransportType GetTransportType() const;
    [[nodiscard]] std::string GetAddress() const;
    [[nodiscard]] ControllerInfo GetControllerInfo() const;
    [[nodiscard]] f32 GetBatteryVoltage() const;
    [[nodiscard]] bool WasSideButtonPressed() const;

    Common::Input::DriverResult SetVibration(const Common::Input::VibrationStatus& vibration);
    Common::Input::DriverResult SetLeds(u8 player_number, bool reverse_leds);

    void StartGyroCalibration();
    [[nodiscard]] bool IsCalibrating() const;

    void SetHorizontalHold(bool horizontal);
    [[nodiscard]] bool IsHorizontalHold() const;

    [[nodiscard]] GyroMouseOutput ProcessGyroMouse(const Switch2InputReport& report,
                                                   bool trigger_pressed);
    [[nodiscard]] OpticalMouseOutput ProcessOpticalMouse(const Switch2InputReport& report);
    [[nodiscard]] MotionOutput ConvertToMotion(const Switch2InputReport& report,
                                               u64 delta_us) const;

private:
    bool WriteCommandChecked(u8 cmd_id, u8 subcmd_id, std::span<const u8> payload,
                             std::vector<u8>& out_payload);
    bool ReadMemory(u8 length, u32 address, std::vector<u8>& out_data);
    void ReadStickCalibrations();
    void OnRawInputReport(std::span<const u8> data);
    void OnTransportDisconnected();
    [[nodiscard]] std::vector<u8> BuildVibrationPacket(u8 sequence,
                                                       const VibrationPacketData& motor) const;

    std::shared_ptr<ISwitch2Transport> transport;
    mutable std::mutex mutex;
    Switch2Callbacks callbacks{};
    ControllerInfo controller_info{};
    StickCalibration left_calibration{};
    StickCalibration right_calibration{};
    GyroMouseProcessor gyro_mouse{};
    OpticalMouseProcessor optical_mouse{};

    std::shared_ptr<std::atomic<bool>> alive_flag{std::make_shared<std::atomic<bool>>(false)};
    std::atomic<bool> initialized{false};
    std::atomic<bool> horizontal_hold{false};
    std::atomic<bool> side_buttons_pressed{false};
    std::atomic<bool> disconnect_notified{false};
    std::atomic<f32> battery_voltage{0.0f};
    u8 vibration_packet_id{0};
};

} // namespace InputCommon::Switch2
