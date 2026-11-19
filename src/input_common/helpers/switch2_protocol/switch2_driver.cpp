// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/logging.h"
#include "common/settings.h"
#include "input_common/helpers/switch2_protocol/switch2_driver.h"

namespace InputCommon::Switch2 {

Switch2Driver::Switch2Driver(std::shared_ptr<ISwitch2Transport> transport_)
    : transport{std::move(transport_)} {}

Switch2Driver::~Switch2Driver() {
    Stop();
}

Common::Input::DriverResult Switch2Driver::Initialize(u8 player_number, bool reverse_leds) {
    if (transport == nullptr || !transport->IsConnected()) {
        return Common::Input::DriverResult::InvalidHandle;
    }
    if (initialized.load()) {
        return Common::Input::DriverResult::Success;
    }

    side_buttons_pressed.store(false, std::memory_order_relaxed);
    battery_voltage.store(0.0f, std::memory_order_relaxed);
    disconnect_notified.store(false, std::memory_order_relaxed);
    alive_flag->store(true, std::memory_order_release);
    {
        std::scoped_lock lock{mutex};
        gyro_mouse.Reset();
        optical_mouse.Reset();
        vibration_packet_id = 0;
    }

    ControllerInfo info{};
    bool info_parsed = false;
    std::vector<u8> info_data;
    if (ReadMemory(0x40U, MemoryAddress::ControllerInfo, info_data)) {
        if (const auto parsed = ParseControllerInfo(info_data)) {
            info = *parsed;
            info_parsed = true;
        }
    }
    if (!info_parsed) {
        info.product_id = transport->GetProductId();
        info.vendor_id = NintendoVendorId;
    } else {
        if (info.product_id == ProductId::None) {
            info.product_id = transport->GetProductId();
        }
        if (info.vendor_id == 0U) {
            info.vendor_id = NintendoVendorId;
        }
    }
    {
        std::scoped_lock lock{mutex};
        controller_info = info;
    }

    ReadStickCalibrations();

    if (auto self_weak = weak_from_this(); !self_weak.expired()) {
        transport->SetInputCallback([self_weak](std::span<const u8> data) {
            if (const auto self = self_weak.lock()) {
                self->OnRawInputReport(data);
            }
        });
        transport->SetDisconnectCallback([self_weak]() {
            if (const auto self = self_weak.lock()) {
                self->OnTransportDisconnected();
            }
        });
    } else {
        auto alive = alive_flag;
        transport->SetInputCallback([this, alive](std::span<const u8> data) {
            if (alive->load(std::memory_order_acquire)) {
                OnRawInputReport(data);
            }
        });
        transport->SetDisconnectCallback([this, alive]() {
            if (alive->load(std::memory_order_acquire)) {
                OnTransportDisconnected();
            }
        });
    }

    // PROTOCOL.md section 7: the normal controller feature set is buttons + analog sticks +
    // IMU + rumble, plus the optical sensor on the Joy-Con 2.
    const u8 feature_flags = FeatureMaskForProduct(info.product_id);
    const std::array<u8, 4> feature_payload{feature_flags, 0x00, 0x00, 0x00};
    std::vector<u8> cmd_response;
    if (!WriteCommandChecked(CommandId::Feature, SubCommandId::FeatureInit, feature_payload,
                             cmd_response)) {
        LOG_WARNING(Input, "Switch2Driver: FeatureInit failed for {}", GetAddress());
    }
    if (!WriteCommandChecked(CommandId::Feature, SubCommandId::FeatureEnable, feature_payload,
                             cmd_response)) {
        LOG_WARNING(Input, "Switch2Driver: FeatureEnable failed for {}", GetAddress());
    }

    if (SetLeds(player_number, reverse_leds) != Common::Input::DriverResult::Success) {
        LOG_WARNING(Input, "Switch2Driver: SetLeds failed for {}", GetAddress());
    }

    // Short connection haptic feedback pulse (single bass thump then stop).
    const VibrationPacketData bass_thump{
        .lf_freq = 0x060,
        .lf_amp = 0x350,
        .hf_freq = 0x0C0,
        .hf_amp = 0x250,
    };
    const VibrationPacketData stop_vibration{};
    {
        u8 start_packet_id = 0;
        {
            std::scoped_lock lock{mutex};
            start_packet_id = vibration_packet_id;
            vibration_packet_id = static_cast<u8>((vibration_packet_id + 1U) & 0x0FU);
        }
        const auto start_report = BuildVibrationPacket(start_packet_id, bass_thump);
        if (!transport->WriteVibration(start_report)) {
            LOG_WARNING(Input, "Switch2Driver: Failed to send connect haptic pulse for {}",
                        GetAddress());
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{30});
    {
        u8 stop_packet_id = 0;
        {
            std::scoped_lock lock{mutex};
            stop_packet_id = vibration_packet_id;
            vibration_packet_id = static_cast<u8>((vibration_packet_id + 1U) & 0x0FU);
        }
        const auto stop_report = BuildVibrationPacket(stop_packet_id, stop_vibration);
        if (!transport->WriteVibration(stop_report)) {
            LOG_WARNING(Input, "Switch2Driver: Failed to stop connect haptic pulse for {}",
                        GetAddress());
        }
    }

    initialized.store(true);

    std::function<void(const ControllerInfo&)> on_info;
    {
        std::scoped_lock lock{mutex};
        on_info = callbacks.on_controller_info;
    }
    if (on_info) {
        on_info(info);
    }

    return Common::Input::DriverResult::Success;
}

void Switch2Driver::Stop() {
    initialized.store(false, std::memory_order_release);
    if (alive_flag) {
        alive_flag->store(false, std::memory_order_release);
    }
    {
        std::scoped_lock lock{mutex};
        callbacks = {};
    }
    if (transport != nullptr) {
        transport->SetInputCallback({});
        transport->SetDisconnectCallback({});
        transport->Disconnect();
    }
}

void Switch2Driver::SetCallbacks(Switch2Callbacks callbacks_) {
    std::scoped_lock lock{mutex};
    callbacks = std::move(callbacks_);
}

bool Switch2Driver::IsConnected() const {
    return transport != nullptr && transport->IsConnected();
}

ProductId Switch2Driver::GetProductId() const {
    {
        std::scoped_lock lock{mutex};
        if (controller_info.product_id != ProductId::None) {
            return controller_info.product_id;
        }
    }
    return transport != nullptr ? transport->GetProductId() : ProductId::None;
}

ControllerType Switch2Driver::GetControllerType() const {
    return ProductIdToControllerType(GetProductId());
}

TransportType Switch2Driver::GetTransportType() const {
    return transport != nullptr ? transport->GetTransportType() : TransportType::None;
}

std::string Switch2Driver::GetAddress() const {
    return transport != nullptr ? transport->GetAddress() : std::string{};
}

ControllerInfo Switch2Driver::GetControllerInfo() const {
    std::scoped_lock lock{mutex};
    return controller_info;
}

f32 Switch2Driver::GetBatteryVoltage() const {
    return battery_voltage.load(std::memory_order_relaxed);
}

bool Switch2Driver::WasSideButtonPressed() const {
    return side_buttons_pressed.load(std::memory_order_relaxed);
}

Common::Input::DriverResult Switch2Driver::SetVibration(
    const Common::Input::VibrationStatus& vibration) {
    if (transport == nullptr || !transport->IsConnected()) {
        return Common::Input::DriverResult::InvalidHandle;
    }

    const VibrationPacketData vib_data = VibrationPacketData::FromVibrationStatus(vibration);

    u8 packet_id = 0;
    {
        std::scoped_lock lock{mutex};
        packet_id = vibration_packet_id;
        vibration_packet_id = static_cast<u8>((vibration_packet_id + 1U) & 0x0FU);
    }

    const auto report = BuildVibrationPacket(packet_id, vib_data);
    if (!transport->WriteVibration(report)) {
        return Common::Input::DriverResult::ErrorWritingData;
    }
    return Common::Input::DriverResult::Success;
}

Common::Input::DriverResult Switch2Driver::SetLeds(u8 player_number, bool reverse_leds) {
    if (transport == nullptr || !transport->IsConnected()) {
        return Common::Input::DriverResult::InvalidHandle;
    }

    const u8 clamped_player = std::clamp<u8>(player_number, 1, 8);
    u8 pattern = LedPatterns[static_cast<std::size_t>(clamped_player) - 1U];
    if (reverse_leds) {
        pattern = ReverseLowerBits4(pattern);
    }

    const std::array<u8, 4> payload{pattern, 0x00, 0x00, 0x00};
    std::vector<u8> response;
    if (!WriteCommandChecked(CommandId::Leds, SubCommandId::LedsSetPlayer, payload, response)) {
        return Common::Input::DriverResult::ErrorWritingData;
    }
    return Common::Input::DriverResult::Success;
}

void Switch2Driver::StartGyroCalibration() {
    std::scoped_lock lock{mutex};
    gyro_mouse.StartCalibration(std::chrono::steady_clock::now());
}

bool Switch2Driver::IsCalibrating() const {
    std::scoped_lock lock{mutex};
    return gyro_mouse.IsCalibrating();
}

void Switch2Driver::SetHorizontalHold(bool horizontal) {
    horizontal_hold.store(horizontal, std::memory_order_relaxed);
}

bool Switch2Driver::IsHorizontalHold() const {
    return horizontal_hold.load(std::memory_order_relaxed);
}

GyroMouseOutput Switch2Driver::ProcessGyroMouse(const Switch2InputReport& report,
                                                bool trigger_pressed) {
    const ControllerType controller_type = GetControllerType();
    const bool horiz = horizontal_hold.load(std::memory_order_relaxed);
    const auto mode = Settings::values.switch2_gyro_mouse_mode.GetValue();
    const auto activation_mode = Settings::values.switch2_gyro_activation_mode.GetValue();
    const f32 sensitivity =
        static_cast<f32>(Settings::values.switch2_gyro_sensitivity.GetValue()) / 10.0f;

    std::scoped_lock lock{mutex};
    return gyro_mouse.Process(report, controller_type, horiz, trigger_pressed, mode,
                              activation_mode, sensitivity, std::chrono::steady_clock::now());
}

OpticalMouseOutput Switch2Driver::ProcessOpticalMouse(const Switch2InputReport& report) {
    const ControllerType controller_type = GetControllerType();
    const bool enabled = Settings::values.switch2_optical_mouse.GetValue();

    std::scoped_lock lock{mutex};
    return optical_mouse.Process(report, controller_type, enabled);
}

MotionOutput Switch2Driver::ConvertToMotion(const Switch2InputReport& report, u64 delta_us) const {
    std::array<f32, 3> bias{};
    {
        std::scoped_lock lock{mutex};
        bias = gyro_mouse.GetGyroBias();
    }
    return ConvertImuToMotion(report, bias, delta_us);
}

bool Switch2Driver::WriteCommandChecked(u8 cmd_id, u8 subcmd_id, std::span<const u8> payload,
                                        std::vector<u8>& out_payload) {
    out_payload.clear();
    if (transport == nullptr || !transport->IsConnected() || payload.size() > 255U) {
        return false;
    }

    const bool is_ble = transport->GetTransportType() == TransportType::Ble;
    const auto packet = BuildCommandPacket(cmd_id, subcmd_id, payload, is_ble);

    std::vector<u8> resp;
    if (!transport->WriteCommand(packet, resp, std::chrono::milliseconds{1000})) {
        return false;
    }

    if (resp.size() < 8U || resp[0] != cmd_id) {
        return false;
    }
    if (is_ble && resp[1] != 0x01U) {
        return false;
    }

    out_payload.assign(resp.begin() + 8, resp.end());
    return true;
}

bool Switch2Driver::ReadMemory(u8 length, u32 address, std::vector<u8>& out_data) {
    out_data.clear();
    if (length == 0U || length > 0x4FU) {
        return false;
    }

    const auto req = BuildMemoryReadPayload(length, address);
    std::vector<u8> payload;
    if (!WriteCommandChecked(CommandId::Memory, SubCommandId::MemoryRead, req, payload)) {
        return false;
    }

    const std::size_t expected_min_size = 8U + static_cast<std::size_t>(length);
    if (payload.size() < expected_min_size || payload[0] != length) {
        return false;
    }

    const u32 resp_addr = static_cast<u32>(payload[4]) | (static_cast<u32>(payload[5]) << 8) |
                          (static_cast<u32>(payload[6]) << 16) |
                          (static_cast<u32>(payload[7]) << 24);
    if (resp_addr != address) {
        return false;
    }

    const auto begin_it = payload.begin() + 8;
    const auto end_it = begin_it + static_cast<std::ptrdiff_t>(length);
    out_data.assign(begin_it, end_it);
    return true;
}

void Switch2Driver::ReadStickCalibrations() {
    const auto is_erased = [](const std::vector<u8>& buf) {
        return buf.size() < 3U || (buf[0] == 0xFFU && buf[1] == 0xFFU && buf[2] == 0xFFU);
    };

    // PROTOCOL.md sections 10 and 11: the user calibration block carries the 0xa1b2 magic
    // followed by the 9-byte calibration; the factory block holds the calibration directly.
    // A user block without the magic is not a calibration and the factory block is used.
    const auto read_stick_cal = [this, &is_erased](u32 user_addr, u32 factory_addr) {
        std::vector<u8> user_block;
        if (ReadMemory(UserCalibrationBlockSize, user_addr, user_block)) {
            if (const auto cal = ParseUserStickCalibration(user_block)) {
                return *cal;
            }
        }

        std::vector<u8> factory_data;
        if (!ReadMemory(FactoryCalibrationSize, factory_addr, factory_data) ||
            is_erased(factory_data)) {
            return StickCalibration{};
        }
        return ParseStickCalibration(factory_data);
    };

    const StickCalibration cal1 =
        read_stick_cal(MemoryAddress::UserJoystick1, MemoryAddress::FactoryJoystick1);

    StickCalibration left_cal{};
    StickCalibration right_cal{};

    const ProductId pid = GetProductId();
    if (pid == ProductId::JoyCon2Right) {
        right_cal = cal1;
    } else if (pid == ProductId::ProController2 || pid == ProductId::NsoGameCube) {
        left_cal = cal1;
        right_cal = read_stick_cal(MemoryAddress::UserJoystick2, MemoryAddress::FactoryJoystick2);
    } else {
        left_cal = cal1;
    }

    std::scoped_lock lock{mutex};
    left_calibration = left_cal;
    right_calibration = right_cal;
}

std::vector<u8> Switch2Driver::BuildVibrationPacket(u8 sequence,
                                                    const VibrationPacketData& motor) const {
    if (transport != nullptr && transport->GetTransportType() == TransportType::Usb) {
        // PROTOCOL.md section 44.1: the USB rumble packet is not the BLE one.
        return BuildUsbVibrationReport(GetProductId(), sequence, motor);
    }
    return BuildVibrationReport(sequence, GetProductId() == ProductId::ProController2, motor);
}

void Switch2Driver::OnTransportDisconnected() {
    if (disconnect_notified.exchange(true)) {
        return;
    }
    initialized.store(false);

    std::function<void()> on_disconnect;
    {
        std::scoped_lock lock{mutex};
        on_disconnect = callbacks.on_disconnect;
    }
    if (on_disconnect) {
        on_disconnect();
    }
}

void Switch2Driver::OnRawInputReport(std::span<const u8> data) {
    if (!alive_flag || !alive_flag->load(std::memory_order_acquire)) {
        return;
    }

    StickCalibration left_cal{};
    StickCalibration right_cal{};
    std::function<void(const Switch2InputReport&)> on_report;
    {
        std::scoped_lock lock{mutex};
        left_cal = left_calibration;
        right_cal = right_calibration;
        on_report = callbacks.on_input_report;
    }

    const auto parsed = ParseInputReport(data, left_cal, right_cal, DefaultStickDeadzone);
    if (!parsed) {
        return;
    }

    battery_voltage.store(parsed->battery_voltage, std::memory_order_relaxed);

    constexpr u32 side_button_mask =
        static_cast<u32>(PadButton::RightSR) | static_cast<u32>(PadButton::RightSL) |
        static_cast<u32>(PadButton::LeftSL) | static_cast<u32>(PadButton::LeftSR);
    if ((parsed->buttons & side_button_mask) != 0U) {
        side_buttons_pressed.store(true, std::memory_order_relaxed);
    }

    if (initialized.load(std::memory_order_acquire) && on_report) {
        on_report(*parsed);
    }
}

} // namespace InputCommon::Switch2
