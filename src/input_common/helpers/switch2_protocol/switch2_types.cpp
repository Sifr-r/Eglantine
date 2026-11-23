// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Protocol constants and packet parsing derived in part from CareyScott/switch2controllerpc
// (MIT License, Copyright (c) 2026 Scott Carey).

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "common/logging.h"
#include "input_common/helpers/switch2_protocol/switch2_types.h"

#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
#include "eglantine_switch2_io.h"
#endif

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

bool IsSupportedProductIdCpp(u16 raw_pid) {
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

ControllerType ProductIdToControllerTypeCpp(ProductId pid) {
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

std::string_view ProductNameCpp(ProductId pid) {
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
        return "Unknown Switch 2 Controller";
    }
}

std::optional<std::span<const u8>> StripHidReportIdCpp(std::span<const u8> usb_report) {
    if (usb_report.size() < MinimumInputReportSize + 1) {
        return std::nullopt;
    }
    return usb_report.subspan(1);
}

std::optional<BleAdvertisementInfo> ParseBleManufacturerDataCpp(std::span<const u8> data) {
    if (data.size() < 16) {
        return std::nullopt;
    }

    const u16 vendor_id = DecodeU16LE(data, 3);
    const u16 raw_pid = DecodeU16LE(data, 5);
    if (vendor_id != NintendoVendorId || !IsSupportedProductIdCpp(raw_pid)) {
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

std::pair<u16, u16> UnpackStickXYCpp(std::span<const u8> three_bytes) {
    if (three_bytes.size() < 3) {
        return {u16{2048}, u16{2048}};
    }
    const u32 packed = DecodeU24LE(three_bytes, 0);
    return {
        static_cast<u16>(packed & 0xFFFU),
        static_cast<u16>((packed >> 12) & 0xFFFU),
    };
}

StickCalibration ParseStickCalibrationCpp(std::span<const u8> data) {
    StickCalibration cal{};
    if (data.size() < 9) {
        return cal;
    }

    const auto [cx, cy] = UnpackStickXYCpp(data.subspan(0, 3));
    const auto [maxx, maxy] = UnpackStickXYCpp(data.subspan(3, 3));
    const auto [minx, miny] = UnpackStickXYCpp(data.subspan(6, 3));

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

std::optional<StickCalibration> ParseUserStickCalibrationCpp(std::span<const u8> block) {
    if (block.size() < UserCalibrationBlockSize) {
        return std::nullopt;
    }
    if (DecodeU16LE(block, 0) != UserCalibrationMagic) {
        return std::nullopt;
    }
    return ParseStickCalibrationCpp(block.subspan(2, FactoryCalibrationSize));
}

f32 ApplyStickCalibrationAxisCpp(u16 raw_value, u16 center, u16 max_offset, u16 min_offset,
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

std::optional<ControllerInfo> ParseControllerInfoCpp(std::span<const u8> data) {
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

std::array<u8, 5> Encode5BytesCpp(const VibrationPacketData& pkt) {
    // PROTOCOL.md section 44: the five-byte HD-rumble frame packs, in this order, the high
    // frequency, the high amplitude, the low frequency and the low amplitude. There is no
    // tone bit in this encoding.
    std::array<u8, 5> out{};
    out[0] = static_cast<u8>(pkt.hf_freq & 0xFFU);
    out[1] = static_cast<u8>((pkt.hf_freq >> 8) | (pkt.hf_amp << 2));
    out[2] = static_cast<u8>((pkt.hf_amp >> 6) | (pkt.lf_freq << 4));
    out[3] = static_cast<u8>((pkt.lf_freq >> 4) | (pkt.lf_amp << 6));
    out[4] = static_cast<u8>(pkt.lf_amp >> 2);
    return out;
}

VibrationPacketData FromVibrationStatusCpp(const Common::Input::VibrationStatus& status) {
    VibrationPacketData data{};
    const f32 clamped_lf = std::clamp(status.low_amplitude, 0.0f, 1.0f);
    const f32 clamped_hf = std::clamp(status.high_amplitude, 0.0f, 1.0f);
    data.lf_amp = static_cast<u16>(std::lround(clamped_lf * 800.0f));
    data.hf_amp = static_cast<u16>(std::lround(clamped_hf * 800.0f));

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

std::optional<Switch2InputReport> ParseInputReportCpp(std::span<const u8> data,
                                                      const StickCalibration& left_cal,
                                                      const StickCalibration& right_cal,
                                                      u16 deadzone) {
    if (data.size() < MinimumInputReportSize) {
        return std::nullopt;
    }

    Switch2InputReport report{};
    report.timestamp = DecodeU32LE(data, 0);
    report.buttons = DecodeU32LE(data, 4);

    const auto [lx_raw, ly_raw] = UnpackStickXYCpp(data.subspan(10, 3));
    const auto [rx_raw, ry_raw] = UnpackStickXYCpp(data.subspan(13, 3));
    report.raw_left_x = lx_raw;
    report.raw_left_y = ly_raw;
    report.raw_right_x = rx_raw;
    report.raw_right_y = ry_raw;

    report.left_x = ApplyStickCalibrationAxisCpp(lx_raw, left_cal.center_x, left_cal.max_x,
                                                 left_cal.min_x, deadzone);
    report.left_y = ApplyStickCalibrationAxisCpp(ly_raw, left_cal.center_y, left_cal.max_y,
                                                 left_cal.min_y, deadzone);
    report.right_x = ApplyStickCalibrationAxisCpp(rx_raw, right_cal.center_x, right_cal.max_x,
                                                  right_cal.min_x, deadzone);
    report.right_y = ApplyStickCalibrationAxisCpp(ry_raw, right_cal.center_y, right_cal.max_y,
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

MotionOutput ConvertImuToMotionCpp(const Switch2InputReport& report,
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

std::vector<u8> BuildCommandPacketCpp(u8 command_id, u8 subcommand_id, std::span<const u8> payload,
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

std::array<u8, 8> BuildMemoryReadPayloadCpp(u8 length, u32 address) {
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

std::vector<u8> BuildVibrationReportCpp(u8 packet_id, bool is_pro_controller,
                                        const VibrationPacketData& motor1,
                                        const VibrationPacketData& motor2,
                                        const VibrationPacketData& motor3) {
    std::array<u8, 16> motor_block{};
    motor_block[0] = static_cast<u8>(0x50U + (packet_id & 0x0FU));
    const auto m1 = Encode5BytesCpp(motor1);
    const auto m2 = Encode5BytesCpp(motor2);
    const auto m3 = Encode5BytesCpp(motor3);
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

std::vector<u8> BuildUsbVibrationReportCpp(ProductId pid, u8 sequence,
                                           const VibrationPacketData& motor) {
    // PROTOCOL.md section 44.1: the USB rumble report is the controller's output report ID
    // (section 1), 0x50 | sequence and a single five-byte rumble frame.
    const auto frame = Encode5BytesCpp(motor);
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

u8 ReverseLowerBits4Cpp(u8 value) {
    u8 result = 0;
    for (int i = 0; i < 4; ++i) {
        result = static_cast<u8>((result << 1) | (value & 1U));
        value >>= 1;
    }
    return result;
}

s32 SignedLoopingDifference16Cpp(u16 previous, u16 current) {
    s32 diff = (static_cast<s32>(current) - static_cast<s32>(previous)) & 0xFFFF;
    if (diff > 32768) {
        diff -= 65536;
    }
    return diff;
}

#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
enum class Switch2IoBackendMode {
    Cpp,
    Rust,
    Verify,
};

Switch2IoBackendMode GetSwitch2IoBackendMode() {
    const char* verify_env = std::getenv("EDEN_NX2_RUST_SWITCH2_IO_VERIFY");
    if (verify_env != nullptr && verify_env[0] == '1') {
        return Switch2IoBackendMode::Verify;
    }
    const char* mode_env = std::getenv("EDEN_NX2_RUST_SWITCH2_IO");
    if (mode_env != nullptr) {
        if (std::string_view{mode_env} == "verify") {
            return Switch2IoBackendMode::Verify;
        }
        if (mode_env[0] == '1') {
            return Switch2IoBackendMode::Rust;
        }
    }
    return Switch2IoBackendMode::Cpp;
}

struct Sw2BytesGuard {
    EglSw2Bytes raw{};
    Sw2BytesGuard() = default;
    ~Sw2BytesGuard() {
        egl_sw2_free_bytes(&raw);
    }
    Sw2BytesGuard(const Sw2BytesGuard&) = delete;
    Sw2BytesGuard& operator=(const Sw2BytesGuard&) = delete;
};

[[noreturn]] void FailSwitch2Parity(std::string_view fn_name) {
    LOG_ERROR(Input, "Switch2 Rust parity mismatch in {}", fn_name);
    std::abort();
}

bool FloatNear(f32 a, f32 b) {
    if (std::isnan(a) && std::isnan(b)) {
        return true;
    }
    return std::abs(a - b) <= 1e-5f;
}

EglSw2StickCalibration ToRustCal(const StickCalibration& cal) {
    return EglSw2StickCalibration{
        .center_x = cal.center_x,
        .center_y = cal.center_y,
        .max_x = cal.max_x,
        .max_y = cal.max_y,
        .min_x = cal.min_x,
        .min_y = cal.min_y,
    };
}

StickCalibration FromRustCal(const EglSw2StickCalibration& cal) {
    return StickCalibration{
        .center_x = cal.center_x,
        .center_y = cal.center_y,
        .max_x = cal.max_x,
        .max_y = cal.max_y,
        .min_x = cal.min_x,
        .min_y = cal.min_y,
    };
}

EglSw2VibrationPacketData ToRustVib(const VibrationPacketData& pkt) {
    return EglSw2VibrationPacketData{
        .lf_freq = pkt.lf_freq,
        .lf_amp = pkt.lf_amp,
        .hf_freq = pkt.hf_freq,
        .hf_amp = pkt.hf_amp,
    };
}

VibrationPacketData FromRustVib(const EglSw2VibrationPacketData& pkt) {
    return VibrationPacketData{
        .lf_freq = pkt.lf_freq,
        .lf_amp = pkt.lf_amp,
        .hf_freq = pkt.hf_freq,
        .hf_amp = pkt.hf_amp,
    };
}

EglSw2InputReport ToRustReport(const Switch2InputReport& r) {
    return EglSw2InputReport{
        .timestamp = r.timestamp,
        .buttons = r.buttons,
        .raw_left_x = r.raw_left_x,
        .raw_left_y = r.raw_left_y,
        .raw_right_x = r.raw_right_x,
        .raw_right_y = r.raw_right_y,
        .left_x = r.left_x,
        .left_y = r.left_y,
        .right_x = r.right_x,
        .right_y = r.right_y,
        .mouse_x = r.mouse_x,
        .mouse_y = r.mouse_y,
        .mouse_roughness = r.mouse_roughness,
        .mouse_distance = r.mouse_distance,
        .magnetometer = {r.magnetometer[0], r.magnetometer[1], r.magnetometer[2]},
        ._pad_mag = 0,
        .battery_voltage = r.battery_voltage,
        .battery_current = r.battery_current,
        .temperature = r.temperature,
        .accelerometer = {r.accelerometer[0], r.accelerometer[1], r.accelerometer[2]},
        .gyroscope = {r.gyroscope[0], r.gyroscope[1], r.gyroscope[2]},
    };
}

Switch2InputReport FromRustReport(const EglSw2InputReport& r) {
    return Switch2InputReport{
        .timestamp = r.timestamp,
        .buttons = r.buttons,
        .raw_left_x = r.raw_left_x,
        .raw_left_y = r.raw_left_y,
        .raw_right_x = r.raw_right_x,
        .raw_right_y = r.raw_right_y,
        .left_x = r.left_x,
        .left_y = r.left_y,
        .right_x = r.right_x,
        .right_y = r.right_y,
        .mouse_x = r.mouse_x,
        .mouse_y = r.mouse_y,
        .mouse_roughness = r.mouse_roughness,
        .mouse_distance = r.mouse_distance,
        .magnetometer = {r.magnetometer[0], r.magnetometer[1], r.magnetometer[2]},
        .battery_voltage = r.battery_voltage,
        .battery_current = r.battery_current,
        .temperature = r.temperature,
        .accelerometer = {r.accelerometer[0], r.accelerometer[1], r.accelerometer[2]},
        .gyroscope = {r.gyroscope[0], r.gyroscope[1], r.gyroscope[2]},
    };
}

bool ReportsEqual(const Switch2InputReport& a, const Switch2InputReport& b) {
    return a.timestamp == b.timestamp && a.buttons == b.buttons &&
           a.raw_left_x == b.raw_left_x && a.raw_left_y == b.raw_left_y &&
           a.raw_right_x == b.raw_right_x && a.raw_right_y == b.raw_right_y &&
           FloatNear(a.left_x, b.left_x) && FloatNear(a.left_y, b.left_y) &&
           FloatNear(a.right_x, b.right_x) && FloatNear(a.right_y, b.right_y) &&
           a.mouse_x == b.mouse_x && a.mouse_y == b.mouse_y &&
           a.mouse_roughness == b.mouse_roughness && a.mouse_distance == b.mouse_distance &&
           a.magnetometer == b.magnetometer &&
           FloatNear(a.battery_voltage, b.battery_voltage) &&
           FloatNear(a.battery_current, b.battery_current) &&
           FloatNear(a.temperature, b.temperature) &&
           a.accelerometer == b.accelerometer && a.gyroscope == b.gyroscope;
}

std::vector<u8> BytesToVec(const Sw2BytesGuard& guard) {
    if (guard.raw.data == nullptr || guard.raw.len == 0) {
        return {};
    }
    return std::vector<u8>(guard.raw.data, guard.raw.data + guard.raw.len);
}

u64 TimePointNs(std::chrono::steady_clock::time_point tp) {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
    return static_cast<u64>(std::max<s64>(0, ns));
}
#endif

} // namespace

bool IsSupportedProductId(u16 raw_pid) {
#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    const auto mode = GetSwitch2IoBackendMode();
    if (mode == Switch2IoBackendMode::Rust) {
        return egl_sw2_is_supported_product_id(raw_pid) != 0;
    }
    const bool cpp_res = IsSupportedProductIdCpp(raw_pid);
    if (mode == Switch2IoBackendMode::Verify &&
        (egl_sw2_is_supported_product_id(raw_pid) != 0) != cpp_res) {
        FailSwitch2Parity("IsSupportedProductId");
    }
    return cpp_res;
#else
    return IsSupportedProductIdCpp(raw_pid);
#endif
}

ControllerType ProductIdToControllerType(ProductId pid) {
#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    const auto mode = GetSwitch2IoBackendMode();
    const u16 raw_pid = static_cast<u16>(pid);
    if (mode == Switch2IoBackendMode::Rust) {
        return static_cast<ControllerType>(egl_sw2_product_id_to_controller_type(raw_pid));
    }
    const ControllerType cpp_res = ProductIdToControllerTypeCpp(pid);
    if (mode == Switch2IoBackendMode::Verify &&
        static_cast<ControllerType>(egl_sw2_product_id_to_controller_type(raw_pid)) != cpp_res) {
        FailSwitch2Parity("ProductIdToControllerType");
    }
    return cpp_res;
#else
    return ProductIdToControllerTypeCpp(pid);
#endif
}

std::string_view ProductName(ProductId pid) {
#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    const auto mode = GetSwitch2IoBackendMode();
    const u16 raw_pid = static_cast<u16>(pid);
    if (mode == Switch2IoBackendMode::Rust) {
        std::size_t len = 0;
        const char* ptr = egl_sw2_product_name(raw_pid, &len);
        return std::string_view{ptr, len};
    }
    const std::string_view cpp_res = ProductNameCpp(pid);
    if (mode == Switch2IoBackendMode::Verify) {
        std::size_t len = 0;
        const char* ptr = egl_sw2_product_name(raw_pid, &len);
        if (std::string_view{ptr, len} != cpp_res) {
            FailSwitch2Parity("ProductName");
        }
    }
    return cpp_res;
#else
    return ProductNameCpp(pid);
#endif
}

std::optional<std::span<const u8>> StripHidReportId(std::span<const u8> usb_report) {
#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    const auto mode = GetSwitch2IoBackendMode();
    const auto run_rust = [&]() -> std::optional<std::span<const u8>> {
        std::size_t offset = 0;
        std::size_t len = 0;
        if (egl_sw2_strip_hid_report_id(usb_report.data(), usb_report.size(), &offset, &len) == 0) {
            return std::nullopt;
        }
        return usb_report.subspan(offset, len);
    };
    if (mode == Switch2IoBackendMode::Rust) {
        return run_rust();
    }
    const auto cpp_res = StripHidReportIdCpp(usb_report);
    if (mode == Switch2IoBackendMode::Verify) {
        const auto rust_res = run_rust();
        if (cpp_res.has_value() != rust_res.has_value() ||
            (cpp_res.has_value() &&
             (cpp_res->data() != rust_res->data() || cpp_res->size() != rust_res->size()))) {
            FailSwitch2Parity("StripHidReportId");
        }
    }
    return cpp_res;
#else
    return StripHidReportIdCpp(usb_report);
#endif
}

std::optional<BleAdvertisementInfo> ParseBleManufacturerData(std::span<const u8> data) {
#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    const auto mode = GetSwitch2IoBackendMode();
    const auto run_rust = [&]() -> std::optional<BleAdvertisementInfo> {
        EglSw2BleAdvertisementInfo raw{};
        if (egl_sw2_parse_ble_manufacturer_data(data.data(), data.size(), &raw) == 0) {
            return std::nullopt;
        }
        return BleAdvertisementInfo{
            .vendor_id = raw.vendor_id,
            .product_id = static_cast<ProductId>(raw.product_id),
            .reconnect_mac = raw.reconnect_mac,
            .is_pairing_mode = raw.is_pairing_mode != 0,
        };
    };
    if (mode == Switch2IoBackendMode::Rust) {
        return run_rust();
    }
    const auto cpp_res = ParseBleManufacturerDataCpp(data);
    if (mode == Switch2IoBackendMode::Verify) {
        const auto rust_res = run_rust();
        if (cpp_res.has_value() != rust_res.has_value() ||
            (cpp_res.has_value() &&
             (cpp_res->vendor_id != rust_res->vendor_id ||
              cpp_res->product_id != rust_res->product_id ||
              cpp_res->reconnect_mac != rust_res->reconnect_mac ||
              cpp_res->is_pairing_mode != rust_res->is_pairing_mode))) {
            FailSwitch2Parity("ParseBleManufacturerData");
        }
    }
    return cpp_res;
#else
    return ParseBleManufacturerDataCpp(data);
#endif
}

std::pair<u16, u16> UnpackStickXY(std::span<const u8> three_bytes) {
#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    const auto mode = GetSwitch2IoBackendMode();
    const auto run_rust = [&]() -> std::pair<u16, u16> {
        u16 x = 2048;
        u16 y = 2048;
        egl_sw2_unpack_stick_xy(three_bytes.data(), three_bytes.size(), &x, &y);
        return {x, y};
    };
    if (mode == Switch2IoBackendMode::Rust) {
        return run_rust();
    }
    const auto cpp_res = UnpackStickXYCpp(three_bytes);
    if (mode == Switch2IoBackendMode::Verify && run_rust() != cpp_res) {
        FailSwitch2Parity("UnpackStickXY");
    }
    return cpp_res;
#else
    return UnpackStickXYCpp(three_bytes);
#endif
}

StickCalibration ParseStickCalibration(std::span<const u8> data) {
#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    const auto mode = GetSwitch2IoBackendMode();
    const auto run_rust = [&]() -> StickCalibration {
        EglSw2StickCalibration raw{};
        egl_sw2_parse_stick_calibration(data.data(), data.size(), &raw);
        return FromRustCal(raw);
    };
    if (mode == Switch2IoBackendMode::Rust) {
        return run_rust();
    }
    const auto cpp_res = ParseStickCalibrationCpp(data);
    if (mode == Switch2IoBackendMode::Verify) {
        const auto rust_res = run_rust();
        if (cpp_res.center_x != rust_res.center_x || cpp_res.center_y != rust_res.center_y ||
            cpp_res.max_x != rust_res.max_x || cpp_res.max_y != rust_res.max_y ||
            cpp_res.min_x != rust_res.min_x || cpp_res.min_y != rust_res.min_y) {
            FailSwitch2Parity("ParseStickCalibration");
        }
    }
    return cpp_res;
#else
    return ParseStickCalibrationCpp(data);
#endif
}

std::optional<StickCalibration> ParseUserStickCalibration(std::span<const u8> block) {
#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    const auto mode = GetSwitch2IoBackendMode();
    const auto run_rust = [&]() -> std::optional<StickCalibration> {
        EglSw2StickCalibration raw{};
        if (egl_sw2_parse_user_stick_calibration(block.data(), block.size(), &raw) == 0) {
            return std::nullopt;
        }
        return FromRustCal(raw);
    };
    if (mode == Switch2IoBackendMode::Rust) {
        return run_rust();
    }
    const auto cpp_res = ParseUserStickCalibrationCpp(block);
    if (mode == Switch2IoBackendMode::Verify) {
        const auto rust_res = run_rust();
        if (cpp_res.has_value() != rust_res.has_value() ||
            (cpp_res.has_value() &&
             (cpp_res->center_x != rust_res->center_x || cpp_res->center_y != rust_res->center_y ||
              cpp_res->max_x != rust_res->max_x || cpp_res->max_y != rust_res->max_y ||
              cpp_res->min_x != rust_res->min_x || cpp_res->min_y != rust_res->min_y))) {
            FailSwitch2Parity("ParseUserStickCalibration");
        }
    }
    return cpp_res;
#else
    return ParseUserStickCalibrationCpp(block);
#endif
}

f32 ApplyStickCalibrationAxis(u16 raw_value, u16 center, u16 max_offset, u16 min_offset,
                                u16 deadzone) {
#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    const auto mode = GetSwitch2IoBackendMode();
    if (mode == Switch2IoBackendMode::Rust) {
        return egl_sw2_apply_stick_calibration_axis(raw_value, center, max_offset, min_offset,
                                                    deadzone);
    }
    const f32 cpp_res =
        ApplyStickCalibrationAxisCpp(raw_value, center, max_offset, min_offset, deadzone);
    if (mode == Switch2IoBackendMode::Verify &&
        !FloatNear(cpp_res, egl_sw2_apply_stick_calibration_axis(raw_value, center, max_offset,
                                                                 min_offset, deadzone))) {
        FailSwitch2Parity("ApplyStickCalibrationAxis");
    }
    return cpp_res;
#else
    return ApplyStickCalibrationAxisCpp(raw_value, center, max_offset, min_offset, deadzone);
#endif
}

std::optional<ControllerInfo> ParseControllerInfo(std::span<const u8> data) {
#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    const auto mode = GetSwitch2IoBackendMode();
    const auto run_rust = [&]() -> std::optional<ControllerInfo> {
        EglSw2ControllerInfo raw{};
        if (egl_sw2_parse_controller_info(data.data(), data.size(), &raw) == 0) {
            return std::nullopt;
        }
        const std::size_t serial_len =
            std::min<std::size_t>(raw.serial_len, sizeof(raw.serial_number));
        return ControllerInfo{
            .serial_number = std::string(raw.serial_number, serial_len),
            .vendor_id = raw.vendor_id,
            .product_id = static_cast<ProductId>(raw.product_id),
            .body_color = raw.body_color,
            .buttons_color = raw.buttons_color,
            .left_grip_color = raw.left_grip_color,
            .right_grip_color = raw.right_grip_color,
        };
    };
    if (mode == Switch2IoBackendMode::Rust) {
        return run_rust();
    }
    const auto cpp_res = ParseControllerInfoCpp(data);
    if (mode == Switch2IoBackendMode::Verify) {
        const auto rust_res = run_rust();
        if (cpp_res.has_value() != rust_res.has_value() ||
            (cpp_res.has_value() &&
             (cpp_res->serial_number != rust_res->serial_number ||
              cpp_res->vendor_id != rust_res->vendor_id ||
              cpp_res->product_id != rust_res->product_id ||
              cpp_res->body_color != rust_res->body_color ||
              cpp_res->buttons_color != rust_res->buttons_color ||
              cpp_res->left_grip_color != rust_res->left_grip_color ||
              cpp_res->right_grip_color != rust_res->right_grip_color))) {
            FailSwitch2Parity("ParseControllerInfo");
        }
    }
    return cpp_res;
#else
    return ParseControllerInfoCpp(data);
#endif
}

std::array<u8, 5> VibrationPacketData::Encode5Bytes() const {
#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    const auto mode = GetSwitch2IoBackendMode();
    const auto run_rust = [&]() {
        const EglSw2VibrationPacketData raw = ToRustVib(*this);
        std::array<u8, 5> out{};
        egl_sw2_vibration_encode_5bytes(&raw, out.data());
        return out;
    };
    if (mode == Switch2IoBackendMode::Rust) {
        return run_rust();
    }
    const auto cpp_res = Encode5BytesCpp(*this);
    if (mode == Switch2IoBackendMode::Verify && run_rust() != cpp_res) {
        FailSwitch2Parity("VibrationPacketData::Encode5Bytes");
    }
    return cpp_res;
#else
    return Encode5BytesCpp(*this);
#endif
}

VibrationPacketData VibrationPacketData::FromVibrationStatus(
    const Common::Input::VibrationStatus& status) {
#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    const auto mode = GetSwitch2IoBackendMode();
    const auto run_rust = [&]() {
        EglSw2VibrationPacketData raw{};
        egl_sw2_vibration_from_status(status.low_amplitude, status.low_frequency,
                                      status.high_amplitude, status.high_frequency, &raw);
        return FromRustVib(raw);
    };
    if (mode == Switch2IoBackendMode::Rust) {
        return run_rust();
    }
    const auto cpp_res = FromVibrationStatusCpp(status);
    if (mode == Switch2IoBackendMode::Verify) {
        const auto rust_res = run_rust();
        if (cpp_res.lf_freq != rust_res.lf_freq || cpp_res.lf_amp != rust_res.lf_amp ||
            cpp_res.hf_freq != rust_res.hf_freq || cpp_res.hf_amp != rust_res.hf_amp) {
            FailSwitch2Parity("VibrationPacketData::FromVibrationStatus");
        }
    }
    return cpp_res;
#else
    return FromVibrationStatusCpp(status);
#endif
}

std::optional<Switch2InputReport> ParseInputReport(std::span<const u8> data,
                                                   const StickCalibration& left_cal,
                                                   const StickCalibration& right_cal,
                                                   u16 deadzone) {
#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    const auto mode = GetSwitch2IoBackendMode();
    const auto run_rust = [&]() -> std::optional<Switch2InputReport> {
        const EglSw2StickCalibration lc = ToRustCal(left_cal);
        const EglSw2StickCalibration rc = ToRustCal(right_cal);
        EglSw2InputReport raw{};
        if (egl_sw2_parse_input_report(data.data(), data.size(), &lc, &rc, deadzone, &raw) == 0) {
            return std::nullopt;
        }
        return FromRustReport(raw);
    };
    if (mode == Switch2IoBackendMode::Rust) {
        return run_rust();
    }
    const auto cpp_res = ParseInputReportCpp(data, left_cal, right_cal, deadzone);
    if (mode == Switch2IoBackendMode::Verify) {
        const auto rust_res = run_rust();
        if (cpp_res.has_value() != rust_res.has_value() ||
            (cpp_res.has_value() && !ReportsEqual(*cpp_res, *rust_res))) {
            FailSwitch2Parity("ParseInputReport");
        }
    }
    return cpp_res;
#else
    return ParseInputReportCpp(data, left_cal, right_cal, deadzone);
#endif
}

MotionOutput ConvertImuToMotion(const Switch2InputReport& report,
                                const std::array<f32, 3>& gyro_bias, u64 delta_timestamp_us) {
#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    const auto mode = GetSwitch2IoBackendMode();
    const auto run_rust = [&]() -> MotionOutput {
        const EglSw2InputReport r = ToRustReport(report);
        EglSw2MotionOutput raw{};
        egl_sw2_convert_imu_to_motion(&r, gyro_bias.data(), delta_timestamp_us, &raw);
        return MotionOutput{
            .gyro_x = raw.gyro_x,
            .gyro_y = raw.gyro_y,
            .gyro_z = raw.gyro_z,
            .accel_x = raw.accel_x,
            .accel_y = raw.accel_y,
            .accel_z = raw.accel_z,
            .delta_timestamp = raw.delta_timestamp,
        };
    };
    if (mode == Switch2IoBackendMode::Rust) {
        return run_rust();
    }
    const auto cpp_res = ConvertImuToMotionCpp(report, gyro_bias, delta_timestamp_us);
    if (mode == Switch2IoBackendMode::Verify) {
        const auto rust_res = run_rust();
        if (!FloatNear(cpp_res.gyro_x, rust_res.gyro_x) ||
            !FloatNear(cpp_res.gyro_y, rust_res.gyro_y) ||
            !FloatNear(cpp_res.gyro_z, rust_res.gyro_z) ||
            !FloatNear(cpp_res.accel_x, rust_res.accel_x) ||
            !FloatNear(cpp_res.accel_y, rust_res.accel_y) ||
            !FloatNear(cpp_res.accel_z, rust_res.accel_z) ||
            cpp_res.delta_timestamp != rust_res.delta_timestamp) {
            FailSwitch2Parity("ConvertImuToMotion");
        }
    }
    return cpp_res;
#else
    return ConvertImuToMotionCpp(report, gyro_bias, delta_timestamp_us);
#endif
}

std::vector<u8> BuildCommandPacket(u8 command_id, u8 subcommand_id, std::span<const u8> payload,
                                   bool is_ble) {
#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    const auto mode = GetSwitch2IoBackendMode();
    const auto run_rust = [&]() {
        Sw2BytesGuard guard;
        if (egl_sw2_build_command_packet(command_id, subcommand_id, payload.data(), payload.size(),
                                         is_ble ? uint8_t{1} : uint8_t{0}, &guard.raw) != 0) {
            return std::vector<u8>{};
        }
        return BytesToVec(guard);
    };
    if (mode == Switch2IoBackendMode::Rust) {
        return run_rust();
    }
    const auto cpp_res = BuildCommandPacketCpp(command_id, subcommand_id, payload, is_ble);
    if (mode == Switch2IoBackendMode::Verify && run_rust() != cpp_res) {
        FailSwitch2Parity("BuildCommandPacket");
    }
    return cpp_res;
#else
    return BuildCommandPacketCpp(command_id, subcommand_id, payload, is_ble);
#endif
}

std::array<u8, 8> BuildMemoryReadPayload(u8 length, u32 address) {
#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    const auto mode = GetSwitch2IoBackendMode();
    const auto run_rust = [&]() {
        std::array<u8, 8> out{};
        egl_sw2_build_memory_read_payload(length, address, out.data());
        return out;
    };
    if (mode == Switch2IoBackendMode::Rust) {
        return run_rust();
    }
    const auto cpp_res = BuildMemoryReadPayloadCpp(length, address);
    if (mode == Switch2IoBackendMode::Verify && run_rust() != cpp_res) {
        FailSwitch2Parity("BuildMemoryReadPayload");
    }
    return cpp_res;
#else
    return BuildMemoryReadPayloadCpp(length, address);
#endif
}

std::vector<u8> BuildVibrationReport(u8 packet_id, bool is_pro_controller,
                                     const VibrationPacketData& motor1,
                                     const VibrationPacketData& motor2,
                                     const VibrationPacketData& motor3) {
#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    const auto mode = GetSwitch2IoBackendMode();
    const auto run_rust = [&]() {
        const auto m1 = ToRustVib(motor1);
        const auto m2 = ToRustVib(motor2);
        const auto m3 = ToRustVib(motor3);
        Sw2BytesGuard guard;
        if (egl_sw2_build_vibration_report(packet_id, is_pro_controller ? uint8_t{1} : uint8_t{0},
                                           &m1, &m2, &m3, &guard.raw) != 0) {
            return std::vector<u8>{};
        }
        return BytesToVec(guard);
    };
    if (mode == Switch2IoBackendMode::Rust) {
        return run_rust();
    }
    const auto cpp_res =
        BuildVibrationReportCpp(packet_id, is_pro_controller, motor1, motor2, motor3);
    if (mode == Switch2IoBackendMode::Verify && run_rust() != cpp_res) {
        FailSwitch2Parity("BuildVibrationReport");
    }
    return cpp_res;
#else
    return BuildVibrationReportCpp(packet_id, is_pro_controller, motor1, motor2, motor3);
#endif
}

std::vector<u8> BuildUsbVibrationReport(ProductId pid, u8 sequence,
                                         const VibrationPacketData& motor) {
#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    const auto mode = GetSwitch2IoBackendMode();
    const auto run_rust = [&]() {
        const auto m = ToRustVib(motor);
        Sw2BytesGuard guard;
        if (egl_sw2_build_usb_vibration_report(static_cast<u16>(pid), sequence, &m, &guard.raw) !=
            0) {
            return std::vector<u8>{};
        }
        return BytesToVec(guard);
    };
    if (mode == Switch2IoBackendMode::Rust) {
        return run_rust();
    }
    const auto cpp_res = BuildUsbVibrationReportCpp(pid, sequence, motor);
    if (mode == Switch2IoBackendMode::Verify && run_rust() != cpp_res) {
        FailSwitch2Parity("BuildUsbVibrationReport");
    }
    return cpp_res;
#else
    return BuildUsbVibrationReportCpp(pid, sequence, motor);
#endif
}

u8 ReverseLowerBits4(u8 value) {
#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    const auto mode = GetSwitch2IoBackendMode();
    if (mode == Switch2IoBackendMode::Rust) {
        return egl_sw2_reverse_lower_bits4(value);
    }
    const u8 cpp_res = ReverseLowerBits4Cpp(value);
    if (mode == Switch2IoBackendMode::Verify && egl_sw2_reverse_lower_bits4(value) != cpp_res) {
        FailSwitch2Parity("ReverseLowerBits4");
    }
    return cpp_res;
#else
    return ReverseLowerBits4Cpp(value);
#endif
}

s32 SignedLoopingDifference16(u16 previous, u16 current) {
#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    const auto mode = GetSwitch2IoBackendMode();
    if (mode == Switch2IoBackendMode::Rust) {
        return egl_sw2_signed_looping_difference16(previous, current);
    }
    const s32 cpp_res = SignedLoopingDifference16Cpp(previous, current);
    if (mode == Switch2IoBackendMode::Verify &&
        egl_sw2_signed_looping_difference16(previous, current) != cpp_res) {
        FailSwitch2Parity("SignedLoopingDifference16");
    }
    return cpp_res;
#else
    return SignedLoopingDifference16Cpp(previous, current);
#endif
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
#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    const auto backend_mode = GetSwitch2IoBackendMode();
    const auto run_rust = [&](EglSw2GyroMouseState& r_state, u8& sample_action) -> GyroMouseOutput {
        const EglSw2InputReport r_rep = ToRustReport(report);
        const s16* samples_ptr =
            calibration_samples.empty() ? nullptr : calibration_samples.front().data();
        EglSw2GyroMouseOutput r_out{};
        if (egl_sw2_gyro_mouse_process(
                &r_state, samples_ptr, calibration_samples.size(), &r_rep,
                static_cast<u8>(controller_type), horizontal_hold ? uint8_t{1} : uint8_t{0},
                trigger_pressed ? uint8_t{1} : uint8_t{0}, static_cast<u32>(mode),
                static_cast<u32>(activation_mode), sensitivity, TimePointNs(now), &sample_action,
                &r_out) != 0) {
            FailSwitch2Parity("GyroMouseProcessor::Process");
        }
        return GyroMouseOutput{
            .active = r_out.active != 0,
            .velocity_x = r_out.velocity_x,
            .velocity_y = r_out.velocity_y,
            .roll_steer_x =
                r_out.has_roll_steer_x != 0 ? std::optional<f32>{r_out.roll_steer_x} : std::nullopt,
            .left_click = r_out.left_click != 0,
            .right_click = r_out.right_click != 0,
        };
    };
    const auto make_state = [&]() {
        return EglSw2GyroMouseState{
            .calibration_end_ns = TimePointNs(calibration_end),
            .gyro_bias = {gyro_bias[0], gyro_bias[1], gyro_bias[2]},
            .is_calibrating = is_calibrating ? uint8_t{1} : uint8_t{0},
            .gyro_mouse_enabled = gyro_mouse_enabled ? uint8_t{1} : uint8_t{0},
            .prev_trigger = prev_trigger ? uint8_t{1} : uint8_t{0},
            ._pad = 0,
        };
    };
    if (backend_mode == Switch2IoBackendMode::Rust) {
        EglSw2GyroMouseState r_state = make_state();
        u8 sample_action = 0;
        const GyroMouseOutput out = run_rust(r_state, sample_action);
        is_calibrating = r_state.is_calibrating != 0;
        gyro_mouse_enabled = r_state.gyro_mouse_enabled != 0;
        prev_trigger = r_state.prev_trigger != 0;
        gyro_bias = {r_state.gyro_bias[0], r_state.gyro_bias[1], r_state.gyro_bias[2]};
        if (sample_action == 1) {
            calibration_samples.push_back(report.gyroscope);
        } else if (sample_action == 2) {
            calibration_samples.clear();
        }
        return out;
    }
    EglSw2GyroMouseState verify_state{};
    u8 verify_action = 0;
    GyroMouseOutput verify_out{};
    if (backend_mode == Switch2IoBackendMode::Verify) {
        verify_state = make_state();
        verify_out = run_rust(verify_state, verify_action);
    }
#endif

    GyroMouseOutput output{};

    if (is_calibrating) {
        if (now < calibration_end) {
            calibration_samples.push_back(report.gyroscope);
#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
            if (backend_mode == Switch2IoBackendMode::Verify &&
                (verify_out.active || verify_action != 1)) {
                FailSwitch2Parity("GyroMouseProcessor::Process (calibrating)");
            }
#endif
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

#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    if (backend_mode == Switch2IoBackendMode::Verify) {
        const bool match =
            output.active == verify_out.active &&
            FloatNear(output.velocity_x, verify_out.velocity_x) &&
            FloatNear(output.velocity_y, verify_out.velocity_y) &&
            output.roll_steer_x.has_value() == verify_out.roll_steer_x.has_value() &&
            (!output.roll_steer_x || FloatNear(*output.roll_steer_x, *verify_out.roll_steer_x)) &&
            output.left_click == verify_out.left_click &&
            output.right_click == verify_out.right_click &&
            is_calibrating == (verify_state.is_calibrating != 0) &&
            gyro_mouse_enabled == (verify_state.gyro_mouse_enabled != 0) &&
            prev_trigger == (verify_state.prev_trigger != 0) &&
            FloatNear(gyro_bias[0], verify_state.gyro_bias[0]) &&
            FloatNear(gyro_bias[1], verify_state.gyro_bias[1]) &&
            FloatNear(gyro_bias[2], verify_state.gyro_bias[2]);
        if (!match) {
            FailSwitch2Parity("GyroMouseProcessor::Process");
        }
    }
#endif

    return output;
}

void OpticalMouseProcessor::Reset() {
    has_previous = false;
    prev_x = 0;
    prev_y = 0;
}

OpticalMouseOutput OpticalMouseProcessor::Process(const Switch2InputReport& report,
                                                  ControllerType controller_type, bool enabled) {
#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    const auto backend_mode = GetSwitch2IoBackendMode();
    const auto make_state = [&]() {
        return EglSw2OpticalMouseState{
            .has_previous = has_previous ? uint8_t{1} : uint8_t{0},
            ._pad = 0,
            .prev_x = prev_x,
            .prev_y = prev_y,
        };
    };
    const auto run_rust = [&](EglSw2OpticalMouseState& r_state) -> OpticalMouseOutput {
        const EglSw2InputReport r_rep = ToRustReport(report);
        EglSw2OpticalMouseOutput r_out{};
        if (egl_sw2_optical_mouse_process(&r_state, &r_rep, static_cast<u8>(controller_type),
                                          enabled ? uint8_t{1} : uint8_t{0}, &r_out) != 0) {
            FailSwitch2Parity("OpticalMouseProcessor::Process");
        }
        return OpticalMouseOutput{
            .active = r_out.active != 0,
            .delta_x = r_out.delta_x,
            .delta_y = r_out.delta_y,
            .left_button = r_out.left_button != 0,
            .middle_button = r_out.middle_button != 0,
            .right_button = r_out.right_button != 0,
            .scroll_y = r_out.scroll_y,
        };
    };
    if (backend_mode == Switch2IoBackendMode::Rust) {
        EglSw2OpticalMouseState r_state = make_state();
        const OpticalMouseOutput out = run_rust(r_state);
        has_previous = r_state.has_previous != 0;
        prev_x = r_state.prev_x;
        prev_y = r_state.prev_y;
        return out;
    }
    EglSw2OpticalMouseState verify_state{};
    OpticalMouseOutput verify_out{};
    if (backend_mode == Switch2IoBackendMode::Verify) {
        verify_state = make_state();
        verify_out = run_rust(verify_state);
    }
#endif

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

#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    if (backend_mode == Switch2IoBackendMode::Verify) {
        const bool match =
            output.active == verify_out.active &&
            output.delta_x == verify_out.delta_x &&
            output.delta_y == verify_out.delta_y &&
            output.left_button == verify_out.left_button &&
            output.middle_button == verify_out.middle_button &&
            output.right_button == verify_out.right_button &&
            FloatNear(output.scroll_y, verify_out.scroll_y) &&
            has_previous == (verify_state.has_previous != 0) &&
            prev_x == verify_state.prev_x && prev_y == verify_state.prev_y;
        if (!match) {
            FailSwitch2Parity("OpticalMouseProcessor::Process");
        }
    }
#endif

    return output;
}

} // namespace InputCommon::Switch2
