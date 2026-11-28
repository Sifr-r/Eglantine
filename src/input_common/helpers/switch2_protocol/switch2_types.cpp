// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Protocol constants and packet parsing derived in part from CareyScott/switch2controllerpc
// (MIT License, Copyright (c) 2026 Scott Carey).

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

#include "input_common/helpers/switch2_protocol/switch2_types.h"

namespace InputCommon::Switch2 {

namespace {

constexpr u16 DecodeU16LE(std::span<const u8> data, std::size_t offset) {
    return static_cast<u16>(data[offset]) | (static_cast<u16>(data[offset + 1]) << 8);
}

constexpr s16 DecodeS16LE(std::span<const u8> data, std::size_t offset) {
    return static_cast<s16>(DecodeU16LE(data, offset));
}

constexpr u32 DecodeU24LE(std::span<const u8> data, std::size_t offset) {
    return static_cast<u32>(data[offset]) | (static_cast<u32>(data[offset + 1]) << 8) |
           (static_cast<u32>(data[offset + 2]) << 16);
}

constexpr u32 DecodeU32LE(std::span<const u8> data, std::size_t offset) {
    return static_cast<u32>(data[offset]) | (static_cast<u32>(data[offset + 1]) << 8) |
           (static_cast<u32>(data[offset + 2]) << 16) |
           (static_cast<u32>(data[offset + 3]) << 24);
}

constexpr u32 DecodeRgb24(std::span<const u8> data, std::size_t offset) {
    return (static_cast<u32>(data[offset]) << 16) | (static_cast<u32>(data[offset + 1]) << 8) |
           static_cast<u32>(data[offset + 2]);
}

f32 ApplySoftDeadzone(f32 value, f32 deadzone) {
    if (std::abs(value) < deadzone) {
        return 0.0f;
    }
    return value > 0.0f ? (value - deadzone) : (value + deadzone);
}

u16 EncodeVibrationAmplitude(f32 amplitude) {
    if (std::isnan(amplitude)) {
        return 0;
    }
    return static_cast<u16>(std::lround(std::clamp(amplitude, 0.0f, 1.0f) * 800.0f));
}

std::optional<u64> ParseFixedHexU64(std::string_view hex) {
    if (hex.empty() || hex.size() > 16) {
        return std::nullopt;
    }
    u64 value = 0;
    for (const char ch : hex) {
        u64 digit = 0;
        if (ch >= '0' && ch <= '9') {
            digit = static_cast<u64>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            digit = static_cast<u64>(10 + (ch - 'a'));
        } else if (ch >= 'A' && ch <= 'F') {
            digit = static_cast<u64>(10 + (ch - 'A'));
        } else {
            return std::nullopt;
        }
        value = (value << 4) | digit;
    }
    return value;
}

} // namespace

std::vector<BleHandoffDevice> ParseBleHandoffList(std::string_view spec) {
    std::vector<BleHandoffDevice> devices;
    while (!spec.empty()) {
        const std::size_t comma = spec.find(',');
        const std::string_view token = spec.substr(0, comma);
        if (comma == std::string_view::npos) {
            spec = {};
        } else {
            spec.remove_prefix(comma + 1);
        }
        if (token.size() != 17 || token[12] != ':') {
            continue;
        }
        const auto addr = ParseFixedHexU64(token.substr(0, 12));
        const auto raw_pid = ParseFixedHexU64(token.substr(13, 4));
        if (!addr || *addr == 0 || !raw_pid || !IsSupportedProductId(static_cast<u16>(*raw_pid))) {
            continue;
        }
        devices.push_back(BleHandoffDevice{
            .bluetooth_addr = *addr,
            .product_id = static_cast<ProductId>(static_cast<u16>(*raw_pid)),
        });
    }
    return devices;
}

std::optional<std::span<const u8>> StripHidReportId(std::span<const u8> usb_report) {
    if (usb_report.size() < MinimumInputReportSize + 1) {
        return std::nullopt;
    }
    return usb_report.subspan(1);
}

std::optional<BleAdvertisementInfo> ParseBleManufacturerData(std::span<const u8> data) {
    if (data.size() < 16) {
        return std::nullopt;
    }

    const u16 vendor_id = DecodeU16LE(data, 3);
    const u16 raw_pid = DecodeU16LE(data, 5);
    if (vendor_id != NintendoVendorId || !IsSupportedProductId(raw_pid)) {
        return std::nullopt;
    }

    u64 reconnect_mac = 0;
    for (std::size_t i = 0; i < 6; ++i) {
        reconnect_mac |= static_cast<u64>(data[10 + i]) << (i * 8);
    }

    return BleAdvertisementInfo{
        .vendor_id = vendor_id,
        .product_id = static_cast<ProductId>(raw_pid),
        .reconnect_mac = reconnect_mac,
        .is_pairing_mode = reconnect_mac == 0,
    };
}

std::pair<u16, u16> UnpackStickXY(std::span<const u8> three_bytes) {
    if (three_bytes.size() < 3) {
        return {u16{2048}, u16{2048}};
    }
    const u32 packed = DecodeU24LE(three_bytes, 0);
    return {
        static_cast<u16>(packed & 0xFFFU),
        static_cast<u16>((packed >> 12) & 0xFFFU),
    };
}

StickCalibration ParseStickCalibration(std::span<const u8> data) {
    StickCalibration cal{};
    if (data.size() < 9) {
        return cal;
    }

    const auto [cx, cy] = UnpackStickXY(data.subspan(0, 3));
    const auto [maxx, maxy] = UnpackStickXY(data.subspan(3, 3));
    const auto [minx, miny] = UnpackStickXY(data.subspan(6, 3));

    if (cx == 0 && cy == 0 && maxx == 0 && maxy == 0) {
        return cal;
    }

    cal.center_x = cx;
    cal.center_y = cy;
    cal.max_x = maxx > 0 ? maxx : 1500;
    cal.max_y = maxy > 0 ? maxy : 1500;
    cal.min_x = minx > 0 ? minx : 1500;
    cal.min_y = miny > 0 ? miny : 1500;
    return cal;
}

std::optional<StickCalibration> ParseUserStickCalibration(std::span<const u8> block) {
    if (block.size() < UserCalibrationBlockSize) {
        return std::nullopt;
    }
    if (DecodeU16LE(block, 0) != UserCalibrationMagic) {
        return std::nullopt;
    }
    return ParseStickCalibration(block.subspan(2, FactoryCalibrationSize));
}

f32 ApplyStickCalibrationAxis(u16 raw_value, u16 center, u16 max_offset, u16 min_offset,
                              u16 deadzone) {
    const s32 signed_val = static_cast<s32>(raw_value) - static_cast<s32>(center);
    const s32 dz = static_cast<s32>(deadzone);
    if (signed_val > dz) {
        if (max_offset == 0) {
            return 1.0f;
        }
        return std::min(static_cast<f32>(signed_val) / static_cast<f32>(max_offset), 1.0f);
    }
    if (signed_val < -dz) {
        if (min_offset == 0) {
            return -1.0f;
        }
        return -std::min(static_cast<f32>(-signed_val) / static_cast<f32>(min_offset), 1.0f);
    }
    return 0.0f;
}

std::optional<ControllerInfo> ParseControllerInfo(std::span<const u8> data) {
    if (data.size() < 37) {
        return std::nullopt;
    }

    ControllerInfo info{};
    std::size_t serial_len = 0;
    while (serial_len < 14 && data[2 + serial_len] != 0) {
        ++serial_len;
    }
    info.serial_number.assign(reinterpret_cast<const char*>(data.data() + 2), serial_len);
    info.vendor_id = DecodeU16LE(data, 18);
    info.product_id = static_cast<ProductId>(DecodeU16LE(data, 20));
    info.body_color = DecodeRgb24(data, 25);
    info.buttons_color = DecodeRgb24(data, 28);
    info.left_grip_color = DecodeRgb24(data, 31);
    info.right_grip_color = DecodeRgb24(data, 34);
    return info;
}

std::array<u8, 5> VibrationPacketData::Encode5Bytes() const {
    // PROTOCOL.md section 44: the five-byte HD-rumble frame packs, in this order, the high
    // frequency, the high amplitude, the low frequency and the low amplitude. There is no
    // tone bit in this encoding.
    std::array<u8, 5> out{};
    out[0] = static_cast<u8>(hf_freq & 0xFFU);
    out[1] = static_cast<u8>((hf_freq >> 8) | (hf_amp << 2));
    out[2] = static_cast<u8>((hf_amp >> 6) | (lf_freq << 4));
    out[3] = static_cast<u8>((lf_freq >> 4) | (lf_amp << 6));
    out[4] = static_cast<u8>(lf_amp >> 2);
    return out;
}

VibrationPacketData VibrationPacketData::FromVibrationStatus(
    const Common::Input::VibrationStatus& status) {
    VibrationPacketData data{};
    data.lf_amp = EncodeVibrationAmplitude(status.low_amplitude);
    data.hf_amp = EncodeVibrationAmplitude(status.high_amplitude);

    if (status.low_frequency > 0.0f) {
        const f32 encoded_lf = std::clamp(status.low_frequency, 40.0f, 320.0f);
        data.lf_freq = static_cast<u16>(std::clamp(std::lround(encoded_lf * 1.4f), 1L, 0x1FFL));
    }
    if (status.high_frequency > 0.0f) {
        const f32 encoded_hf = std::clamp(status.high_frequency, 80.0f, 640.0f);
        data.hf_freq = static_cast<u16>(std::clamp(std::lround(encoded_hf * 1.5f), 1L, 0x1FFL));
    }
    return data;
}

std::optional<Switch2InputReport> ParseInputReport(std::span<const u8> data,
                                                   const StickCalibration& left_cal,
                                                   const StickCalibration& right_cal,
                                                   u16 deadzone) {
    if (data.size() < MinimumInputReportSize) {
        return std::nullopt;
    }

    Switch2InputReport report{};
    report.timestamp = DecodeU32LE(data, 0);
    report.buttons = DecodeU32LE(data, 4);

    const auto [lx_raw, ly_raw] = UnpackStickXY(data.subspan(10, 3));
    const auto [rx_raw, ry_raw] = UnpackStickXY(data.subspan(13, 3));
    report.raw_left_x = lx_raw;
    report.raw_left_y = ly_raw;
    report.raw_right_x = rx_raw;
    report.raw_right_y = ry_raw;

    report.left_x = ApplyStickCalibrationAxis(lx_raw, left_cal.center_x, left_cal.max_x,
                                              left_cal.min_x, deadzone);
    report.left_y = ApplyStickCalibrationAxis(ly_raw, left_cal.center_y, left_cal.max_y,
                                              left_cal.min_y, deadzone);
    report.right_x = ApplyStickCalibrationAxis(rx_raw, right_cal.center_x, right_cal.max_x,
                                               right_cal.min_x, deadzone);
    report.right_y = ApplyStickCalibrationAxis(ry_raw, right_cal.center_y, right_cal.max_y,
                                               right_cal.min_y, deadzone);

    report.mouse_x = DecodeU16LE(data, 16);
    report.mouse_y = DecodeU16LE(data, 18);
    report.mouse_roughness = DecodeU16LE(data, 20);
    report.mouse_distance = DecodeU16LE(data, 22);

    report.magnetometer = {
        DecodeS16LE(data, 25),
        DecodeS16LE(data, 27),
        DecodeS16LE(data, 29),
    };

    report.battery_voltage = static_cast<f32>(DecodeU16LE(data, 31)) / 1000.0f;
    // PROTOCOL.md sections 29 and 35: offset 33 is the charging state byte, the battery
    // current is the two bytes at offset 34.
    report.battery_current = static_cast<f32>(DecodeU16LE(data, 34)) / 100.0f;
    report.temperature = 25.0f + static_cast<f32>(DecodeU16LE(data, 46)) / 127.0f;

    report.accelerometer = {
        DecodeS16LE(data, 48),
        DecodeS16LE(data, 50),
        DecodeS16LE(data, 52),
    };
    report.gyroscope = {
        DecodeS16LE(data, 54),
        DecodeS16LE(data, 56),
        DecodeS16LE(data, 58),
    };

    return report;
}

MotionOutput ConvertImuToMotion(const Switch2InputReport& report,
                                const std::array<f32, 3>& gyro_bias, u64 delta_timestamp_us) {
    // Switch 2 IMU outputs 16-bit signed readings at +-8G (4096 LSB/g). The common report
    // gyro covers 40 rad/s over 32767 counts (PROTOCOL.md sections 37 and 52).
    // Eglantine's MotionInput expects accelerometer in G and gyroscope in revolutions/sec.
    constexpr f32 accel_scale = 1.0f / 4096.0f;
    constexpr f32 gyro_rad_per_count = 40.0f / 32767.0f;
    constexpr f32 gyro_scale = gyro_rad_per_count / (2.0f * 3.14159265358979323846f);

    const f32 gx = static_cast<f32>(report.gyroscope[0]) - gyro_bias[0];
    const f32 gy = static_cast<f32>(report.gyroscope[1]) - gyro_bias[1];
    const f32 gz = static_cast<f32>(report.gyroscope[2]) - gyro_bias[2];

    return MotionOutput{
        .gyro_x = -gx * gyro_scale,
        .gyro_y = gy * gyro_scale,
        .gyro_z = gz * gyro_scale,
        .accel_x = static_cast<f32>(report.accelerometer[0]) * accel_scale,
        .accel_y = -static_cast<f32>(report.accelerometer[1]) * accel_scale,
        .accel_z = -static_cast<f32>(report.accelerometer[2]) * accel_scale,
        .delta_timestamp = delta_timestamp_us > 0 ? delta_timestamp_us : 4000ULL,
    };
}

std::vector<u8> BuildCommandPacket(u8 command_id, u8 subcommand_id, std::span<const u8> payload,
                                    bool is_ble) {
    std::vector<u8> packet;
    packet.reserve(8 + payload.size());
    packet.push_back(command_id);
    packet.push_back(0x91);
    packet.push_back(is_ble ? 0x01 : 0x00);
    packet.push_back(subcommand_id);
    packet.push_back(0x00);
    packet.push_back(static_cast<u8>(payload.size()));
    packet.push_back(0x00);
    packet.push_back(0x00);
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

std::optional<std::span<const u8>> ParseCommandResponse(std::span<const u8> response,
                                                        u8 command_id, bool is_ble) {
    constexpr std::size_t header_size = 8;
    if (response.size() < header_size || response[0] != command_id ||
        (is_ble && response[1] != 0x01)) {
        return std::nullopt;
    }
    return response.subspan(header_size);
}

std::array<u8, 8> BuildMemoryReadPayload(u8 length, u32 address) {
    return {
        length,
        0x7E,
        0x00,
        0x00,
        static_cast<u8>(address & 0xFFU),
        static_cast<u8>((address >> 8) & 0xFFU),
        static_cast<u8>((address >> 16) & 0xFFU),
        static_cast<u8>((address >> 24) & 0xFFU),
    };
}

std::optional<std::span<const u8>> ParseMemoryReadResponse(std::span<const u8> payload, u8 length,
                                                           u32 address) {
    constexpr std::size_t header_size = 8;
    if (!IsValidMemoryReadLength(length) || payload.size() < header_size + length ||
        payload[0] != length || DecodeU32LE(payload, 4) != address) {
        return std::nullopt;
    }
    return payload.subspan(header_size, length);
}

std::array<u8, 14> BuildPairSetMacPayload(u64 host_mac) {
    std::array<u8, 14> payload{0x00, 0x02};
    for (std::size_t i = 0; i < 6; ++i) {
        const u8 byte = static_cast<u8>(host_mac >> (i * 8));
        payload[2 + i] = byte;
        payload[8 + i] = byte;
    }
    return payload;
}

std::vector<u8> BuildVibrationReport(u8 packet_id, bool is_pro_controller,
                                     const VibrationPacketData& motor1,
                                     const VibrationPacketData& motor2,
                                     const VibrationPacketData& motor3) {
    std::array<u8, 16> motor_block{};
    motor_block[0] = static_cast<u8>(0x50U + (packet_id & 0x0FU));
    const auto m1 = motor1.Encode5Bytes();
    const auto m2 = motor2.Encode5Bytes();
    const auto m3 = motor3.Encode5Bytes();
    std::memcpy(motor_block.data() + 1, m1.data(), 5);
    std::memcpy(motor_block.data() + 6, m2.data(), 5);
    std::memcpy(motor_block.data() + 11, m3.data(), 5);

    std::vector<u8> report;
    report.reserve(is_pro_controller ? 33 : 17);
    report.push_back(0x00);
    report.insert(report.end(), motor_block.begin(), motor_block.end());
    if (is_pro_controller) {
        report.insert(report.end(), motor_block.begin(), motor_block.end());
    }
    return report;
}

std::vector<u8> BuildUsbVibrationReport(ProductId pid, u8 sequence,
                                        const VibrationPacketData& motor) {
    // PROTOCOL.md section 44.1: the USB rumble report is the controller's output report ID
    // (section 1), 0x50 | sequence and a single five-byte rumble frame.
    const auto frame = motor.Encode5Bytes();
    const u8 sequence_byte = static_cast<u8>(0x50U | (sequence & 0x0FU));

    switch (pid) {
    case ProductId::ProController2: {
        // The Pro Controller drives one LRA per side and therefore carries two rumble blocks.
        std::vector<u8> report(64, 0);
        report[0] = 0x02;
        report[1] = sequence_byte;
        std::copy(frame.begin(), frame.end(), report.begin() + 2);
        report[0x11] = sequence_byte;
        std::copy(frame.begin(), frame.end(), report.begin() + 0x12);
        return report;
    }
    case ProductId::NsoGameCube: {
        // The GameCube controller takes a simple on/off rumble byte instead of an HD frame.
        std::vector<u8> report(7, 0);
        report[0] = 0x03;
        report[1] = sequence_byte;
        report[2] = (motor.lf_amp != 0U || motor.hf_amp != 0U) ? 0x01U : 0x02U;
        return report;
    }
    case ProductId::JoyCon2Left:
    case ProductId::JoyCon2Right:
    default: {
        std::vector<u8> report(7, 0);
        report[0] = 0x01;
        report[1] = sequence_byte;
        std::copy(frame.begin(), frame.end(), report.begin() + 2);
        return report;
    }
    }
}

u8 ReverseLowerBits4(u8 value) {
    u8 result = 0;
    for (int i = 0; i < 4; ++i) {
        result = static_cast<u8>((result << 1) | (value & 1U));
        value >>= 1;
    }
    return result;
}

s32 SignedLoopingDifference16(u16 previous, u16 current) {
    s32 diff = (static_cast<s32>(current) - static_cast<s32>(previous)) & 0xFFFF;
    if (diff > 32768) {
        diff -= 65536;
    }
    return diff;
}

bool IsSupportedProductId(u16 raw_pid) {
    switch (static_cast<ProductId>(raw_pid)) {
    case ProductId::JoyCon2Right:
    case ProductId::JoyCon2Left:
    case ProductId::ProController2:
    case ProductId::NsoGameCube:
        return true;
    default:
        return false;
    }
}

ControllerType ProductIdToControllerType(ProductId pid) {
    switch (pid) {
    case ProductId::JoyCon2Left:
        return ControllerType::Left;
    case ProductId::JoyCon2Right:
        return ControllerType::Right;
    case ProductId::ProController2:
        return ControllerType::Pro;
    case ProductId::NsoGameCube:
        return ControllerType::GameCube;
    default:
        return ControllerType::None;
    }
}

std::string_view ProductName(ProductId pid) {
    switch (pid) {
    case ProductId::JoyCon2Right:
        return "Switch 2 Joy-Con (Right)";
    case ProductId::JoyCon2Left:
        return "Switch 2 Joy-Con (Left)";
    case ProductId::ProController2:
        return "Switch 2 Pro Controller";
    case ProductId::NsoGameCube:
        return "NSO GameCube Controller";
    default:
        break;
    }
    return "Unknown Switch 2 Controller";
}

void GyroMouseProcessor::StartCalibration(std::chrono::steady_clock::time_point now) {
    is_calibrating = true;
    calibration_end = now + std::chrono::seconds{2};
    calibration_samples.clear();
}

void GyroMouseProcessor::Reset() {
    is_calibrating = false;
    calibration_samples.clear();
    gyro_mouse_enabled = false;
    prev_trigger = false;
}

GyroMouseOutput GyroMouseProcessor::Process(
    const Switch2InputReport& report, ControllerType controller_type, bool horizontal_hold,
    bool trigger_pressed, Settings::Switch2GyroMouseMode mode,
    Settings::Switch2GyroActivationMode activation_mode, f32 sensitivity,
    std::chrono::steady_clock::time_point now) {

    GyroMouseOutput output{};

    if (is_calibrating) {
        if (now < calibration_end) {
            calibration_samples.push_back(report.gyroscope);
            return output;
        }
        is_calibrating = false;
        if (!calibration_samples.empty()) {
            f64 sx = 0.0;
            f64 sy = 0.0;
            f64 sz = 0.0;
            for (const auto& sample : calibration_samples) {
                sx += static_cast<f64>(sample[0]);
                sy += static_cast<f64>(sample[1]);
                sz += static_cast<f64>(sample[2]);
            }
            const f64 count = static_cast<f64>(calibration_samples.size());
            gyro_bias = {
                static_cast<f32>(sx / count),
                static_cast<f32>(sy / count),
                static_cast<f32>(sz / count),
            };
        }
        calibration_samples.clear();
    }

    if (mode == Settings::Switch2GyroMouseMode::Off) {
        gyro_mouse_enabled = false;
        prev_trigger = trigger_pressed;
    } else {
        if (activation_mode == Settings::Switch2GyroActivationMode::Hold) {
            gyro_mouse_enabled = trigger_pressed;
        } else {
            if (trigger_pressed && !prev_trigger) {
                gyro_mouse_enabled = !gyro_mouse_enabled;
            }
        }
        prev_trigger = trigger_pressed;

        if (gyro_mouse_enabled) {
            output.active = true;

            if (mode == Settings::Switch2GyroMouseMode::Roll) {
                const f32 tilt_normalized = static_cast<f32>(report.accelerometer[0]) / 4000.0f;
                output.roll_steer_x =
                    std::clamp(tilt_normalized * (sensitivity * -2.0f), -1.0f, 1.0f);
            } else {
                output.left_click = (report.buttons & static_cast<u32>(PadButton::ZR)) != 0;
                output.right_click = (report.buttons & static_cast<u32>(PadButton::ZL)) != 0;

                constexpr f32 bias_threshold = 5.0f;
                const f32 bx = std::abs(gyro_bias[0]) > bias_threshold ? gyro_bias[0] : 0.0f;
                const f32 by = std::abs(gyro_bias[1]) > bias_threshold ? gyro_bias[1] : 0.0f;
                const f32 bz = std::abs(gyro_bias[2]) > bias_threshold ? gyro_bias[2] : 0.0f;

                constexpr f32 soft_deadzone = 8.0f;
                const f32 gx =
                    ApplySoftDeadzone(static_cast<f32>(report.gyroscope[0]) - bx, soft_deadzone);
                const f32 gy =
                    ApplySoftDeadzone(static_cast<f32>(report.gyroscope[1]) - by, soft_deadzone);
                const f32 gz =
                    ApplySoftDeadzone(static_cast<f32>(report.gyroscope[2]) - bz, soft_deadzone);

                constexpr f32 gyro_deadzone = 0.2f;
                if (std::abs(gx) > gyro_deadzone || std::abs(gy) > gyro_deadzone ||
                    std::abs(gz) > gyro_deadzone) {
                    const f32 eff_h = ApplySoftDeadzone(-gz, gyro_deadzone);
                    f32 raw_v = gy;
                    if (controller_type == ControllerType::Pro || !horizontal_hold) {
                        raw_v = gx;
                    } else if (controller_type == ControllerType::Left) {
                        raw_v = -gy;
                    }
                    const f32 eff_v = ApplySoftDeadzone(raw_v, gyro_deadzone);

                    constexpr f32 accel_factor = 0.002f;
                    output.velocity_x = eff_h * sensitivity * accel_factor;
                    output.velocity_y = eff_v * -sensitivity * accel_factor;
                }
            }
        }
    }

    return output;
}

void OpticalMouseProcessor::Reset() {
    has_previous = false;
    prev_x = 0;
    prev_y = 0;
}

OpticalMouseOutput OpticalMouseProcessor::Process(const Switch2InputReport& report,
                                                  ControllerType controller_type, bool enabled) {

    OpticalMouseOutput output{};
    const bool is_joycon =
        controller_type == ControllerType::Left || controller_type == ControllerType::Right;
    if (!enabled || !is_joycon || report.mouse_distance == 0 || report.mouse_distance >= 1000 ||
        report.mouse_roughness >= 4000) {
        Reset();
    } else {
        output.active = true;
        if (controller_type == ControllerType::Left) {
            output.left_button = (report.buttons & static_cast<u32>(PadButton::L)) != 0;
            output.middle_button = (report.buttons & static_cast<u32>(PadButton::StickL)) != 0;
            output.right_button = (report.buttons & static_cast<u32>(PadButton::ZL)) != 0;
            output.scroll_y = std::abs(report.left_y) > 0.2f ? report.left_y : 0.0f;
        } else {
            output.left_button = (report.buttons & static_cast<u32>(PadButton::R)) != 0;
            output.middle_button = (report.buttons & static_cast<u32>(PadButton::StickR)) != 0;
            output.right_button = (report.buttons & static_cast<u32>(PadButton::ZR)) != 0;
            output.scroll_y = std::abs(report.right_y) > 0.2f ? report.right_y : 0.0f;
        }

        if (has_previous) {
            output.delta_x = SignedLoopingDifference16(prev_x, report.mouse_x);
            output.delta_y = SignedLoopingDifference16(prev_y, report.mouse_y);
        }
        prev_x = report.mouse_x;
        prev_y = report.mouse_y;
        has_previous = true;
    }

    return output;
}

} // namespace InputCommon::Switch2
