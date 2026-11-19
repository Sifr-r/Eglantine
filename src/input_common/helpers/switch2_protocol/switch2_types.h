// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Protocol constants and packet structures derived in part from CareyScott/switch2controllerpc
// (MIT License, Copyright (c) 2026 Scott Carey).

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/input.h"
#include "common/settings_enums.h"

namespace InputCommon::Switch2 {

constexpr u16 NintendoVendorId = 0x057E;
constexpr u16 NintendoBleManufacturerId = 0x0553;

enum class ProductId : u16 {
    None = 0x0000,
    JoyCon2Right = 0x2066,
    JoyCon2Left = 0x2067,
    ProController2 = 0x2069,
    NsoGameCube = 0x2073,
};

enum class ControllerType : u8 {
    None = 0x00,
    Left = 0x01,
    Right = 0x02,
    Pro = 0x03,
    Dual = 0x05,
    GameCube = 0x08,
};

enum class TransportType : u8 {
    None = 0,
    Ble = 1,
    Usb = 2,
};

/// Parameter package key that carries the stable identity of a controller. It holds the
/// controller serial number when the controller reports one and the transport address
/// otherwise, so a controller keeps its identity when the driver moves it to another slot.
inline constexpr char DeviceIdKey[] = "device_id";

// BLE GATT Characteristic UUIDs
inline constexpr std::string_view InputReportUuid = "ab7de9be-89fe-49ad-828f-118f09df7fd2";
inline constexpr std::string_view VibrationJoyConRUuid = "fa19b0fb-cd1f-46a7-84a1-bbb09e00c149";
inline constexpr std::string_view VibrationJoyConLUuid = "289326cb-a471-485d-a8f4-240c14f18241";
inline constexpr std::string_view VibrationProControllerUuid =
    "cc483f51-9258-427d-a939-630c31f72b05";
inline constexpr std::string_view CommandWriteUuid = "649d4ac9-8eb7-4e6c-af44-1ea54fe5f005";
inline constexpr std::string_view CommandResponseUuid = "c765a961-d9d8-4d36-a20a-5315b111836a";

namespace CommandId {
constexpr u8 Memory = 0x02;
constexpr u8 UsbInit = 0x03;
constexpr u8 Leds = 0x09;
constexpr u8 Vibration = 0x0A;
constexpr u8 Feature = 0x0C;
constexpr u8 Pair = 0x15;
} // namespace CommandId

namespace SubCommandId {
constexpr u8 LedsSetPlayer = 0x07;
constexpr u8 VibrationPlayPreset = 0x02;
constexpr u8 MemoryRead = 0x04;
constexpr u8 PairSetMac = 0x01;
constexpr u8 PairLtk2 = 0x02;
constexpr u8 PairFinish = 0x03;
constexpr u8 PairLtk1 = 0x04;
constexpr u8 FeatureInit = 0x02;
constexpr u8 FeatureEnable = 0x04;
/// Selects the input report the controller streams (PROTOCOL.md section 38).
constexpr u8 SelectInputReport = 0x0A;
} // namespace SubCommandId

namespace FeatureFlags {
// PROTOCOL.md section 7: the feature-mask bits of command 0x0c.
constexpr u8 Buttons = 0x01;
constexpr u8 AnalogSticks = 0x02;
constexpr u8 Motion = 0x04;
constexpr u8 Mouse = 0x10;
constexpr u8 Rumble = 0x20;
constexpr u8 Magnetometer = 0x80;

/// Buttons + analog sticks + IMU + rumble, the ordinary controller feature set.
constexpr u8 Standard = Buttons | AnalogSticks | Motion | Rumble;
/// The Joy-Con 2 additionally enables the optical sensor.
constexpr u8 JoyCon = Standard | Mouse;
/// The temporary startup mask used before the normal controller feature set is selected.
constexpr u8 Startup = Motion | Mouse | Magnetometer;
} // namespace FeatureFlags

namespace MemoryAddress {
constexpr u32 ControllerInfo = 0x00013000;
constexpr u32 FactoryJoystick1 = 0x0130A8;
constexpr u32 FactoryJoystick2 = 0x0130E8;
/// User stick calibration blocks: a 2-byte magic followed by the 9-byte calibration.
constexpr u32 UserJoystick1 = 0x1FC040;
constexpr u32 UserJoystick2 = 0x1FC080;
} // namespace MemoryAddress

/// Magic that marks a user stick calibration block as written (PROTOCOL.md section 11).
constexpr u16 UserCalibrationMagic = 0xA1B2;
/// Size of a user stick calibration block: 2-byte magic + 9-byte calibration.
constexpr u8 UserCalibrationBlockSize = 11;
/// Size of a factory stick calibration block.
constexpr u8 FactoryCalibrationSize = 9;

enum class PadButton : u32 {
    None = 0,
    Y = 0x00000001,
    X = 0x00000002,
    B = 0x00000004,
    A = 0x00000008,
    RightSR = 0x00000010,
    RightSL = 0x00000020,
    R = 0x00000040,
    ZR = 0x00000080,
    Minus = 0x00000100,
    Plus = 0x00000200,
    StickR = 0x00000400,
    StickL = 0x00000800,
    Home = 0x00001000,
    Capture = 0x00002000,
    C = 0x00004000,
    Down = 0x00010000,
    Up = 0x00020000,
    Right = 0x00040000,
    Left = 0x00080000,
    LeftSR = 0x00100000,
    LeftSL = 0x00200000,
    L = 0x00400000,
    ZL = 0x00800000,
    GR = 0x01000000,
    GL = 0x02000000,
};

enum class PadAxes : int {
    LeftStickX = 0,
    LeftStickY = 1,
    RightStickX = 2,
    RightStickY = 3,
};

enum class PadMotion : int {
    LeftMotion = 0,
    RightMotion = 1,
};

constexpr std::array<u8, 8> LedPatterns{0x01, 0x03, 0x07, 0x0F, 0x09, 0x05, 0x0D, 0x06};

constexpr u16 DefaultStickDeadzone = 50;
/// Minimum number of bytes the common input parser reads.
constexpr std::size_t MinimumInputReportSize = 60;
/// Size of the native common input report, i.e. the BLE GATT value without a report ID
/// (PROTOCOL.md sections 5 and 29).
constexpr std::size_t NativeInputReportSize = 63;
/// HID report ID of the common input report. HID/USB reports carry it in byte 0, BLE GATT
/// report values omit it (PROTOCOL.md sections 5 and 29).
constexpr u8 CommonReportId = 0x05;

struct BleAdvertisementInfo {
    u16 vendor_id{};
    ProductId product_id{ProductId::None};
    u64 reconnect_mac{};
    bool is_pairing_mode{};
};

struct StickCalibration {
    u16 center_x{2048};
    u16 center_y{2048};
    u16 max_x{1500};
    u16 max_y{1500};
    u16 min_x{1500};
    u16 min_y{1500};
};

struct ControllerInfo {
    std::string serial_number{};
    u16 vendor_id{};
    ProductId product_id{ProductId::None};
    u32 body_color{};
    u32 buttons_color{};
    u32 left_grip_color{};
    u32 right_grip_color{};
};

struct VibrationPacketData {
    u16 lf_freq{0x0E1};
    u16 lf_amp{0x000};
    u16 hf_freq{0x1E1};
    u16 hf_amp{0x000};

    [[nodiscard]] std::array<u8, 5> Encode5Bytes() const;
    [[nodiscard]] static VibrationPacketData FromVibrationStatus(
        const Common::Input::VibrationStatus& status);
};

struct Switch2InputReport {
    u32 timestamp{};
    u32 buttons{};
    u16 raw_left_x{2048};
    u16 raw_left_y{2048};
    u16 raw_right_x{2048};
    u16 raw_right_y{2048};
    f32 left_x{};
    f32 left_y{};
    f32 right_x{};
    f32 right_y{};
    u16 mouse_x{};
    u16 mouse_y{};
    u16 mouse_roughness{};
    u16 mouse_distance{};
    std::array<s16, 3> magnetometer{};
    f32 battery_voltage{};
    f32 battery_current{};
    f32 temperature{};
    std::array<s16, 3> accelerometer{};
    std::array<s16, 3> gyroscope{};
};

struct MotionOutput {
    f32 gyro_x{};
    f32 gyro_y{};
    f32 gyro_z{};
    f32 accel_x{};
    f32 accel_y{};
    f32 accel_z{};
    u64 delta_timestamp{4000};
};

struct GyroMouseOutput {
    bool active{};
    f32 velocity_x{};
    f32 velocity_y{};
    std::optional<f32> roll_steer_x{};
    bool left_click{};
    bool right_click{};
};

struct OpticalMouseOutput {
    bool active{};
    s32 delta_x{};
    s32 delta_y{};
    bool left_button{};
    bool middle_button{};
    bool right_button{};
    f32 scroll_y{};
};

struct Switch2Callbacks {
    std::function<void(const Switch2InputReport&)> on_input_report;
    std::function<void(const ControllerInfo&)> on_controller_info;
    std::function<void()> on_disconnect;
};

struct Switch2DeviceInfo {
    std::size_t slot_index{};
    ControllerType type{ControllerType::None};
    ProductId product_id{ProductId::None};
    TransportType transport{TransportType::None};
    std::string address{};
    std::string display_name{};
    bool horizontal_hold{false};
    bool is_calibrating{false};
    f32 battery_voltage{};
};

[[nodiscard]] bool IsSupportedProductId(u16 raw_pid);
[[nodiscard]] ControllerType ProductIdToControllerType(ProductId pid);
[[nodiscard]] std::string_view ProductName(ProductId pid);

/// Feature mask the controller should run with (PROTOCOL.md section 7).
[[nodiscard]] constexpr u8 FeatureMaskForProduct(ProductId pid) {
    switch (pid) {
    case ProductId::JoyCon2Left:
    case ProductId::JoyCon2Right:
        return FeatureFlags::JoyCon;
    default:
        return FeatureFlags::Standard;
    }
}

/// Removes the leading HID report ID byte of a USB input report.
/// HID/USB report offsets include the report ID while BLE GATT report values omit it
/// (PROTOCOL.md sections 5 and 29), so the USB transport strips it and hands the parser the
/// native layout. Returns nullopt when the report is too short to hold the native payload.
[[nodiscard]] std::optional<std::span<const u8>> StripHidReportId(std::span<const u8> usb_report);

[[nodiscard]] std::optional<BleAdvertisementInfo> ParseBleManufacturerData(
    std::span<const u8> data);

[[nodiscard]] std::pair<u16, u16> UnpackStickXY(std::span<const u8> three_bytes);
[[nodiscard]] StickCalibration ParseStickCalibration(std::span<const u8> data);
/// Parses a user stick calibration block: the 0xa1b2 magic followed by the 9-byte calibration
/// structure (PROTOCOL.md section 11). Returns nullopt when the magic is missing.
[[nodiscard]] std::optional<StickCalibration> ParseUserStickCalibration(std::span<const u8> block);
[[nodiscard]] f32 ApplyStickCalibrationAxis(u16 raw_value, u16 center, u16 max_offset,
                                            u16 min_offset, u16 deadzone = DefaultStickDeadzone);

[[nodiscard]] std::optional<ControllerInfo> ParseControllerInfo(std::span<const u8> data);
[[nodiscard]] std::optional<Switch2InputReport> ParseInputReport(
    std::span<const u8> data, const StickCalibration& left_cal,
    const StickCalibration& right_cal, u16 deadzone = DefaultStickDeadzone);

[[nodiscard]] MotionOutput ConvertImuToMotion(const Switch2InputReport& report,
                                              const std::array<f32, 3>& gyro_bias,
                                              u64 delta_timestamp_us);

[[nodiscard]] std::vector<u8> BuildCommandPacket(u8 command_id, u8 subcommand_id,
                                                 std::span<const u8> payload, bool is_ble);
[[nodiscard]] std::array<u8, 8> BuildMemoryReadPayload(u8 length, u32 address);
/// Builds the BLE vibration report: 0x00, 0x50 | sequence and the rumble frame repeated
/// three times (PROTOCOL.md section 44.2).
[[nodiscard]] std::vector<u8> BuildVibrationReport(
    u8 packet_id, bool is_pro_controller, const VibrationPacketData& motor1,
    const VibrationPacketData& motor2 = {}, const VibrationPacketData& motor3 = {});
/// Builds the USB vibration report: the controller's output report ID, 0x50 | sequence and a
/// single rumble frame (PROTOCOL.md section 44.1).
[[nodiscard]] std::vector<u8> BuildUsbVibrationReport(ProductId pid, u8 sequence,
                                                      const VibrationPacketData& motor);

[[nodiscard]] u8 ReverseLowerBits4(u8 value);
[[nodiscard]] s32 SignedLoopingDifference16(u16 previous, u16 current);

class GyroMouseProcessor {
public:
    void StartCalibration(std::chrono::steady_clock::time_point now);
    void Reset();

    [[nodiscard]] bool IsCalibrating() const {
        return is_calibrating;
    }

    [[nodiscard]] const std::array<f32, 3>& GetGyroBias() const {
        return gyro_bias;
    }

    void SetGyroBias(const std::array<f32, 3>& bias) {
        gyro_bias = bias;
    }

    [[nodiscard]] GyroMouseOutput Process(
        const Switch2InputReport& report, ControllerType controller_type, bool horizontal_hold,
        bool trigger_pressed, Settings::Switch2GyroMouseMode mode,
        Settings::Switch2GyroActivationMode activation_mode, f32 sensitivity,
        std::chrono::steady_clock::time_point now);

private:
    bool is_calibrating{false};
    std::chrono::steady_clock::time_point calibration_end{};
    std::vector<std::array<s16, 3>> calibration_samples{};
    std::array<f32, 3> gyro_bias{0.0f, 0.0f, 0.0f};
    bool gyro_mouse_enabled{false};
    bool prev_trigger{false};
};

class OpticalMouseProcessor {
public:
    void Reset();

    [[nodiscard]] OpticalMouseOutput Process(const Switch2InputReport& report,
                                             ControllerType controller_type, bool enabled);

private:
    bool has_previous{false};
    u16 prev_x{};
    u16 prev_y{};
};

} // namespace InputCommon::Switch2
