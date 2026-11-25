// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "common/input.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "input_common/drivers/mouse.h"
#include "input_common/drivers/switch2.h"
#include "input_common/helpers/switch2_protocol/switch2_driver.h"
#include "input_common/helpers/switch2_protocol/switch2_transport.h"
#include "input_common/helpers/switch2_protocol/switch2_types.h"

#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
#include "eglantine_switch2_io.h"
#endif

using namespace InputCommon::Switch2;
using namespace std::chrono_literals;

namespace {

void WriteU16LE(std::span<u8> dst, std::size_t offset, u16 value) {
    dst[offset] = static_cast<u8>(value & 0xFFU);
    dst[offset + 1] = static_cast<u8>((value >> 8) & 0xFFU);
}

void WriteS16LE(std::span<u8> dst, std::size_t offset, s16 value) {
    WriteU16LE(dst, offset, static_cast<u16>(value));
}

void WriteU32LE(std::span<u8> dst, std::size_t offset, u32 value) {
    dst[offset] = static_cast<u8>(value & 0xFFU);
    dst[offset + 1] = static_cast<u8>((value >> 8) & 0xFFU);
    dst[offset + 2] = static_cast<u8>((value >> 16) & 0xFFU);
    dst[offset + 3] = static_cast<u8>((value >> 24) & 0xFFU);
}

std::array<u8, 3> PackStickXY(u16 x, u16 y) {
    const u32 packed = static_cast<u32>(x & 0xFFFU) | (static_cast<u32>(y & 0xFFFU) << 12);
    return {
        static_cast<u8>(packed & 0xFFU),
        static_cast<u8>((packed >> 8) & 0xFFU),
        static_cast<u8>((packed >> 16) & 0xFFU),
    };
}

void WriteRgb24(std::span<u8> dst, std::size_t offset, u32 rgb) {
    dst[offset] = static_cast<u8>((rgb >> 16) & 0xFFU);
    dst[offset + 1] = static_cast<u8>((rgb >> 8) & 0xFFU);
    dst[offset + 2] = static_cast<u8>(rgb & 0xFFU);
}

Switch2InputReport MakeValidMouseReport(u16 mouse_x, u16 mouse_y, u16 distance, u16 roughness) {
    Switch2InputReport report{};
    report.mouse_x = mouse_x;
    report.mouse_y = mouse_y;
    report.mouse_distance = distance;
    report.mouse_roughness = roughness;
    return report;
}

} // namespace

TEST_CASE("Switch 2 product IDs resolve to controller types and names", "[switch2]") {
    struct ProductExpectation {
        u16 id;
        bool supported;
        ControllerType type;
        std::string_view name;
    };
    constexpr std::array<ProductExpectation, 6> products{{
        {0x2066, true, ControllerType::Right, "Switch 2 Joy-Con (Right)"},
        {0x2067, true, ControllerType::Left, "Switch 2 Joy-Con (Left)"},
        {0x2069, true, ControllerType::Pro, "Switch 2 Pro Controller"},
        {0x2073, true, ControllerType::GameCube, "NSO GameCube Controller"},
        {0x0000, false, ControllerType::None, "Unknown Switch 2 Controller"},
        {0x2009, false, ControllerType::None, "Unknown Switch 2 Controller"},
    }};
    for (const auto& [id, supported, type, name] : products) {
        CAPTURE(id);
        const auto product = static_cast<ProductId>(id);
        REQUIRE(IsSupportedProductId(id) == supported);
        REQUIRE(ProductIdToControllerType(product) == type);
        REQUIRE(ProductName(product) == name);
    }
}

TEST_CASE("ParseBleManufacturerData parses pairing, paired MAC, and rejects invalid frames",
          "[switch2]") {
    SECTION("Rejects short buffers") {
        std::array<u8, 15> short_buf{};
        REQUIRE_FALSE(ParseBleManufacturerData(std::span<const u8>{}).has_value());
        REQUIRE_FALSE(ParseBleManufacturerData(short_buf).has_value());
    }

    SECTION("Parses valid Pro Controller 2 in pairing mode") {
        std::array<u8, 16> data{};
        WriteU16LE(data, 3, NintendoVendorId);
        WriteU16LE(data, 5, static_cast<u16>(ProductId::ProController2));

        const auto parsed = ParseBleManufacturerData(data);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->vendor_id == NintendoVendorId);
        REQUIRE(parsed->product_id == ProductId::ProController2);
        REQUIRE(parsed->reconnect_mac == 0ULL);
        REQUIRE(parsed->is_pairing_mode);
    }

    SECTION("Parses paired MAC address and clears pairing mode") {
        std::array<u8, 16> data{};
        WriteU16LE(data, 3, NintendoVendorId);
        WriteU16LE(data, 5, static_cast<u16>(ProductId::JoyCon2Left));
        data[10] = 0xAA;
        data[11] = 0xBB;
        data[12] = 0xCC;
        data[13] = 0xDD;
        data[14] = 0xEE;
        data[15] = 0xFF;

        const auto parsed = ParseBleManufacturerData(data);
        REQUIRE(parsed.has_value());
        REQUIRE(parsed->product_id == ProductId::JoyCon2Left);
        REQUIRE(parsed->reconnect_mac == 0xFFEEDDCCBBAAULL);
        REQUIRE_FALSE(parsed->is_pairing_mode);
    }

    SECTION("Rejects invalid vendor ID and unsupported product ID") {
        std::array<u8, 16> bad_vid{};
        WriteU16LE(bad_vid, 3, 0x1234U);
        WriteU16LE(bad_vid, 5, static_cast<u16>(ProductId::ProController2));
        REQUIRE_FALSE(ParseBleManufacturerData(bad_vid).has_value());

        std::array<u8, 16> bad_pid{};
        WriteU16LE(bad_pid, 3, NintendoVendorId);
        WriteU16LE(bad_pid, 5, 0x2009U);
        REQUIRE_FALSE(ParseBleManufacturerData(bad_pid).has_value());
    }
}

TEST_CASE("Stick unpacking, calibration parsing, and axis normalization", "[switch2]") {
    SECTION("UnpackStickXY decodes 12-bit pairs and falls back on short input") {
        const auto short_two = std::array<u8, 2>{0x12, 0x34};
        const auto fallback_empty = UnpackStickXY(std::span<const u8>{});
        const auto fallback_short = UnpackStickXY(short_two);
        REQUIRE(fallback_empty == std::pair<u16, u16>{2048U, 2048U});
        REQUIRE(fallback_short == std::pair<u16, u16>{2048U, 2048U});

        const auto packed = PackStickXY(0x123U, 0xABCU);
        REQUIRE(packed == std::array<u8, 3>{0x23, 0xC1, 0xAB});
        REQUIRE(UnpackStickXY(packed) == std::pair<u16, u16>{0x123U, 0xABCU});

        REQUIRE(UnpackStickXY(PackStickXY(0U, 4095U)) == std::pair<u16, u16>{0U, 4095U});
        REQUIRE(UnpackStickXY(PackStickXY(4095U, 0U)) == std::pair<u16, u16>{4095U, 0U});
    }

    SECTION("ParseStickCalibration handles short buffers, all-zero fallback, and asymmetric ranges") {
        const std::array<u8, 8> short_cal{};
        const auto short_parsed = ParseStickCalibration(short_cal);
        REQUIRE(short_parsed.center_x == 2048U);
        REQUIRE(short_parsed.center_y == 2048U);
        REQUIRE(short_parsed.max_x == 1500U);
        REQUIRE(short_parsed.max_y == 1500U);
        REQUIRE(short_parsed.min_x == 1500U);
        REQUIRE(short_parsed.min_y == 1500U);

        const std::array<u8, 9> zero_cal{};
        const auto zero_parsed = ParseStickCalibration(zero_cal);
        REQUIRE(zero_parsed.center_x == 2048U);
        REQUIRE(zero_parsed.center_y == 2048U);
        REQUIRE(zero_parsed.max_x == 1500U);
        REQUIRE(zero_parsed.max_y == 1500U);
        REQUIRE(zero_parsed.min_x == 1500U);
        REQUIRE(zero_parsed.min_y == 1500U);

        std::array<u8, 9> asym_bytes{};
        const auto c_bytes = PackStickXY(2060U, 2030U);
        const auto max_bytes = PackStickXY(1400U, 1600U);
        const auto min_bytes = PackStickXY(1200U, 1000U);
        std::copy(c_bytes.begin(), c_bytes.end(), asym_bytes.begin());
        std::copy(max_bytes.begin(), max_bytes.end(), asym_bytes.begin() + 3);
        std::copy(min_bytes.begin(), min_bytes.end(), asym_bytes.begin() + 6);

        const auto asym = ParseStickCalibration(asym_bytes);
        REQUIRE(asym.center_x == 2060U);
        REQUIRE(asym.center_y == 2030U);
        REQUIRE(asym.max_x == 1400U);
        REQUIRE(asym.max_y == 1600U);
        REQUIRE(asym.min_x == 1200U);
        REQUIRE(asym.min_y == 1000U);

        std::array<u8, 9> partial_zero{};
        const auto nz_center = PackStickXY(2040U, 2050U);
        const auto zero_max = PackStickXY(0U, 1100U);
        const auto zero_min = PackStickXY(900U, 0U);
        std::copy(nz_center.begin(), nz_center.end(), partial_zero.begin());
        std::copy(zero_max.begin(), zero_max.end(), partial_zero.begin() + 3);
        std::copy(zero_min.begin(), zero_min.end(), partial_zero.begin() + 6);

        const auto partial = ParseStickCalibration(partial_zero);
        REQUIRE(partial.center_x == 2040U);
        REQUIRE(partial.center_y == 2050U);
        REQUIRE(partial.max_x == 1500U);
        REQUIRE(partial.max_y == 1100U);
        REQUIRE(partial.min_x == 900U);
        REQUIRE(partial.min_y == 1500U);
    }

    SECTION("ApplyStickCalibrationAxis clamps deadzone and normalizes asymmetrically") {
        constexpr u16 center = 2000U;
        constexpr u16 max_offset = 1000U;
        constexpr u16 min_offset = 500U;
        constexpr u16 deadzone = 50U;

        REQUIRE(ApplyStickCalibrationAxis(2000U, center, max_offset, min_offset, deadzone) == 0.0f);
        REQUIRE(ApplyStickCalibrationAxis(2050U, center, max_offset, min_offset, deadzone) == 0.0f);
        REQUIRE(ApplyStickCalibrationAxis(1950U, center, max_offset, min_offset, deadzone) == 0.0f);

        REQUIRE(ApplyStickCalibrationAxis(2500U, center, max_offset, min_offset, deadzone) ==
                Catch::Approx(0.5));
        REQUIRE(ApplyStickCalibrationAxis(3000U, center, max_offset, min_offset, deadzone) ==
                Catch::Approx(1.0));
        REQUIRE(ApplyStickCalibrationAxis(3500U, center, max_offset, min_offset, deadzone) ==
                Catch::Approx(1.0));

        REQUIRE(ApplyStickCalibrationAxis(1750U, center, max_offset, min_offset, deadzone) ==
                Catch::Approx(-0.5));
        REQUIRE(ApplyStickCalibrationAxis(1500U, center, max_offset, min_offset, deadzone) ==
                Catch::Approx(-1.0));
        REQUIRE(ApplyStickCalibrationAxis(1000U, center, max_offset, min_offset, deadzone) ==
                Catch::Approx(-1.0));

        REQUIRE(ApplyStickCalibrationAxis(2100U, center, 0U, min_offset, deadzone) == 1.0f);
        REQUIRE(ApplyStickCalibrationAxis(1900U, center, max_offset, 0U, deadzone) == -1.0f);
    }
}

TEST_CASE("ParseControllerInfo extracts serial, VID/PID, RGB colors, and rejects short buffers",
          "[switch2]") {
    SECTION("Rejects short buffers") {
        std::array<u8, 36> short_buf{};
        REQUIRE_FALSE(ParseControllerInfo(std::span<const u8>{}).has_value());
        REQUIRE_FALSE(ParseControllerInfo(short_buf).has_value());
    }

    SECTION("Extracts NUL-terminated serial, VID/PID, and RGB24 colors") {
        std::array<u8, 37> data{};
        const char serial[] = "HAC1234567";
        for (std::size_t i = 0; i < 10; ++i) {
            data[2 + i] = static_cast<u8>(serial[i]);
        }
        WriteU16LE(data, 18, NintendoVendorId);
        WriteU16LE(data, 20, static_cast<u16>(ProductId::ProController2));
        WriteRgb24(data, 25, 0x112233U);
        WriteRgb24(data, 28, 0x445566U);
        WriteRgb24(data, 31, 0x778899U);
        WriteRgb24(data, 34, 0xAABBCCU);

        const auto info = ParseControllerInfo(data);
        REQUIRE(info.has_value());
        REQUIRE(info->serial_number == "HAC1234567");
        REQUIRE(info->vendor_id == NintendoVendorId);
        REQUIRE(info->product_id == ProductId::ProController2);
        REQUIRE(info->body_color == 0x00112233U);
        REQUIRE(info->buttons_color == 0x00445566U);
        REQUIRE(info->left_grip_color == 0x00778899U);
        REQUIRE(info->right_grip_color == 0x00AABBCCU);
    }

    SECTION("Caps serial extraction at 14 bytes when not NUL-terminated") {
        std::array<u8, 37> data{};
        for (std::size_t i = 0; i < 16; ++i) {
            data[2 + i] = static_cast<u8>('A' + i);
        }
        const auto info = ParseControllerInfo(data);
        REQUIRE(info.has_value());
        REQUIRE(info->serial_number == "ABCDEFGHIJKLMN");
    }
}

TEST_CASE("ParseInputReport decodes buttons, sticks, mouse, telemetry, and signed IMU",
          "[switch2]") {
    const StickCalibration default_cal{};

    SECTION("Rejects short buffers") {
        std::array<u8, MinimumInputReportSize - 1> short_report{};
        REQUIRE_FALSE(ParseInputReport(std::span<const u8>{}, default_cal, default_cal).has_value());
        REQUIRE_FALSE(ParseInputReport(short_report, default_cal, default_cal).has_value());
    }

    SECTION("Decodes all requested button bitmasks individually") {
        const std::array<PadButton, 15> buttons{
            PadButton::A,      PadButton::B,      PadButton::X,      PadButton::Y,
            PadButton::L,      PadButton::R,      PadButton::ZL,     PadButton::ZR,
            PadButton::C,      PadButton::GL,     PadButton::GR,     PadButton::LeftSL,
            PadButton::LeftSR, PadButton::RightSL, PadButton::RightSR,
        };

        std::array<u8, MinimumInputReportSize> raw{};
        for (const auto button : buttons) {
            const u32 mask = static_cast<u32>(button);
            WriteU32LE(raw, 4, mask);
            const auto report = ParseInputReport(raw, default_cal, default_cal);
            REQUIRE(report.has_value());
            REQUIRE(report->buttons == mask);
        }
    }

    SECTION("Decodes sticks, optical mouse, battery, temperature, and signed 16-bit IMU") {
        std::array<u8, MinimumInputReportSize> raw{};
        WriteU32LE(raw, 0, 0x89ABCDEFU);
        const u32 combined_buttons = static_cast<u32>(PadButton::A) | static_cast<u32>(PadButton::ZR) |
                                     static_cast<u32>(PadButton::C) |
                                     static_cast<u32>(PadButton::GL) |
                                     static_cast<u32>(PadButton::LeftSL) |
                                     static_cast<u32>(PadButton::RightSR);
        WriteU32LE(raw, 4, combined_buttons);

        const auto left_packed = PackStickXY(2500U, 1750U);
        const auto right_packed = PackStickXY(2020U, 3200U);
        std::copy(left_packed.begin(), left_packed.end(), raw.begin() + 10);
        std::copy(right_packed.begin(), right_packed.end(), raw.begin() + 13);

        WriteU16LE(raw, 16, 0x1234U);
        WriteU16LE(raw, 18, 0xABCDU);
        WriteU16LE(raw, 20, 1234U);
        WriteU16LE(raw, 22, 567U);

        WriteS16LE(raw, 25, -120);
        WriteS16LE(raw, 27, 0);
        WriteS16LE(raw, 29, 320);

        WriteU16LE(raw, 31, 4150U);
        WriteU16LE(raw, 33, 0x34U);
        WriteU16LE(raw, 34, 325U);
        WriteU16LE(raw, 46, 635U);

        WriteS16LE(raw, 48, -4096);
        WriteS16LE(raw, 50, 2048);
        WriteS16LE(raw, 52, -32768);

        WriteS16LE(raw, 54, -500);
        WriteS16LE(raw, 56, 1234);
        WriteS16LE(raw, 58, 32767);

        const StickCalibration asym_cal{
            .center_x = 2000U,
            .center_y = 2000U,
            .max_x = 1000U,
            .max_y = 1000U,
            .min_x = 500U,
            .min_y = 500U,
        };

        const auto report = ParseInputReport(raw, asym_cal, asym_cal, 50U);
        REQUIRE(report.has_value());
        REQUIRE(report->timestamp == 0x89ABCDEFU);
        REQUIRE(report->buttons == combined_buttons);

        REQUIRE(report->raw_left_x == 2500U);
        REQUIRE(report->raw_left_y == 1750U);
        REQUIRE(report->raw_right_x == 2020U);
        REQUIRE(report->raw_right_y == 3200U);
        REQUIRE(report->left_x == Catch::Approx(0.5));
        REQUIRE(report->left_y == Catch::Approx(-0.5));
        REQUIRE(report->right_x == Catch::Approx(0.0));
        REQUIRE(report->right_y == Catch::Approx(1.0));

        REQUIRE(report->mouse_x == 0x1234U);
        REQUIRE(report->mouse_y == 0xABCDU);
        REQUIRE(report->mouse_roughness == 1234U);
        REQUIRE(report->mouse_distance == 567U);

        REQUIRE(report->magnetometer == std::array<s16, 3>{-120, 0, 320});
        REQUIRE(report->battery_voltage == Catch::Approx(4.15));
        REQUIRE(report->battery_current == Catch::Approx(3.25));
        REQUIRE(report->temperature == Catch::Approx(30.0));

        REQUIRE(report->accelerometer == std::array<s16, 3>{-4096, 2048, -32768});
        REQUIRE(report->gyroscope == std::array<s16, 3>{-500, 1234, 32767});
    }
}

TEST_CASE("Vibration encoding, report framing, and status conversion", "[switch2]") {
    SECTION("Encode5Bytes packs the high/low frequency and amplitude layout of section 44") {
        const VibrationPacketData zero_pkt{
            .lf_freq = 0U,
            .lf_amp = 0U,
            .hf_freq = 0U,
            .hf_amp = 0U,
        };
        REQUIRE(zero_pkt.Encode5Bytes() == std::array<u8, 5>{0x00, 0x00, 0x00, 0x00, 0x00});

        const VibrationPacketData known_pkt{
            .lf_freq = 0x155U,
            .lf_amp = 0x2AAU,
            .hf_freq = 0x0F0U,
            .hf_amp = 0x301U,
        };
        REQUIRE(known_pkt.Encode5Bytes() == std::array<u8, 5>{0xF0, 0x04, 0x5C, 0x95, 0xAA});

        const VibrationPacketData overflow_pkt{
            .lf_freq = 0xFFFFU,
            .lf_amp = 0xFFFFU,
            .hf_freq = 0xFFFFU,
            .hf_amp = 0xFFFFU,
        };
        REQUIRE(overflow_pkt.Encode5Bytes() == std::array<u8, 5>{0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    }

    SECTION("BuildVibrationReport frames 17-byte Joy-Con vs 33-byte Pro Controller reports") {
        const VibrationPacketData m1{0x101U, 0x100U, 0x102U, 0x200U};
        const VibrationPacketData m2{0x050U, 0x080U, 0x0A0U, 0x0C0U};
        const VibrationPacketData m3{0x011U, 0x022U, 0x033U, 0x044U};

        const auto jc_report = BuildVibrationReport(0x13U, false, m1, m2, m3);
        REQUIRE(jc_report.size() == std::size_t{17});
        REQUIRE(jc_report[0] == 0x00U);
        REQUIRE(jc_report[1] == 0x53U);
        const auto e1 = m1.Encode5Bytes();
        const auto e2 = m2.Encode5Bytes();
        const auto e3 = m3.Encode5Bytes();
        REQUIRE(std::equal(e1.begin(), e1.end(), jc_report.begin() + 2));
        REQUIRE(std::equal(e2.begin(), e2.end(), jc_report.begin() + 7));
        REQUIRE(std::equal(e3.begin(), e3.end(), jc_report.begin() + 12));

        const auto pro_report = BuildVibrationReport(0x05U, true, m1, m2, m3);
        REQUIRE(pro_report.size() == std::size_t{33});
        REQUIRE(pro_report[0] == 0x00U);
        REQUIRE(pro_report[1] == 0x55U);
        REQUIRE(pro_report[17] == 0x55U);
        REQUIRE(std::equal(pro_report.begin() + 1, pro_report.begin() + 17, pro_report.begin() + 17));
    }

    SECTION("FromVibrationStatus clamps amplitudes and frequencies") {
        const Common::Input::VibrationStatus idle_status{
            .low_amplitude = 0.0f,
            .low_frequency = 0.0f,
            .high_amplitude = 0.0f,
            .high_frequency = 0.0f,
            .type = Common::Input::VibrationAmplificationType::Linear,
        };
        const auto idle_pkt = VibrationPacketData::FromVibrationStatus(idle_status);
        REQUIRE(idle_pkt.lf_amp == 0U);
        REQUIRE(idle_pkt.hf_amp == 0U);
        REQUIRE(idle_pkt.lf_freq == 0x0E1U);
        REQUIRE(idle_pkt.hf_freq == 0x1E1U);

        const Common::Input::VibrationStatus mid_status{
            .low_amplitude = 0.5f,
            .low_frequency = 100.0f,
            .high_amplitude = 0.25f,
            .high_frequency = 200.0f,
            .type = Common::Input::VibrationAmplificationType::Linear,
        };
        const auto mid_pkt = VibrationPacketData::FromVibrationStatus(mid_status);
        REQUIRE(mid_pkt.lf_amp == 400U);
        REQUIRE(mid_pkt.hf_amp == 200U);
        REQUIRE(mid_pkt.lf_freq == 140U);
        REQUIRE(mid_pkt.hf_freq == 300U);

        const Common::Input::VibrationStatus clamped_status{
            .low_amplitude = 2.0f,
            .low_frequency = 500.0f,
            .high_amplitude = -0.5f,
            .high_frequency = 10.0f,
            .type = Common::Input::VibrationAmplificationType::Linear,
        };
        const auto clamped_pkt = VibrationPacketData::FromVibrationStatus(clamped_status);
        REQUIRE(clamped_pkt.lf_amp == 800U);
        REQUIRE(clamped_pkt.hf_amp == 0U);
        REQUIRE(clamped_pkt.lf_freq == 448U);
        REQUIRE(clamped_pkt.hf_freq == 120U);
    }
}

TEST_CASE("BuildCommandPacket and BuildMemoryReadPayload frame BLE vs USB headers", "[switch2]") {
    const auto payload = BuildMemoryReadPayload(0x40U, MemoryAddress::ControllerInfo);
    REQUIRE(payload == std::array<u8, 8>{0x40, 0x7E, 0x00, 0x00, 0x00, 0x30, 0x01, 0x00});

    const auto ble_packet =
        BuildCommandPacket(CommandId::Memory, SubCommandId::MemoryRead, payload, true);
    REQUIRE(ble_packet == std::vector<u8>{
                              0x02, 0x91, 0x01, 0x04, 0x00, 0x08, 0x00, 0x00,
                              0x40, 0x7E, 0x00, 0x00, 0x00, 0x30, 0x01, 0x00,
                          });

    const auto usb_packet =
        BuildCommandPacket(CommandId::Memory, SubCommandId::MemoryRead, payload, false);
    REQUIRE(usb_packet == std::vector<u8>{
                              0x02, 0x91, 0x00, 0x04, 0x00, 0x08, 0x00, 0x00,
                              0x40, 0x7E, 0x00, 0x00, 0x00, 0x30, 0x01, 0x00,
                          });

    const auto empty_ble =
        BuildCommandPacket(CommandId::UsbInit, SubCommandId::FeatureInit, std::span<const u8>{}, true);
    REQUIRE(empty_ble == std::vector<u8>{0x03, 0x91, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00});
}

TEST_CASE("SignedLoopingDifference16 handles normal deltas and 16-bit wrap-around", "[switch2]") {
    REQUIRE(SignedLoopingDifference16(100U, 150U) == 50);
    REQUIRE(SignedLoopingDifference16(150U, 100U) == -50);
    REQUIRE(SignedLoopingDifference16(1234U, 1234U) == 0);

    REQUIRE(SignedLoopingDifference16(65530U, 5U) == 11);
    REQUIRE(SignedLoopingDifference16(65535U, 0U) == 1);

    REQUIRE(SignedLoopingDifference16(5U, 65530U) == -11);
    REQUIRE(SignedLoopingDifference16(0U, 65535U) == -1);

    REQUIRE(SignedLoopingDifference16(0U, 32768U) == 32768);
    REQUIRE(SignedLoopingDifference16(0U, 32769U) == -32767);
}

TEST_CASE("GyroMouseProcessor handles calibration, activation modes, Yaw, Roll, and Off",
          "[switch2]") {
    using Mode = Settings::Switch2GyroMouseMode;
    using Activation = Settings::Switch2GyroActivationMode;

    const auto t0 = std::chrono::steady_clock::time_point{10s};

    SECTION("Two-second calibration collects samples and computes average bias") {
        GyroMouseProcessor processor;
        processor.StartCalibration(t0);
        REQUIRE(processor.IsCalibrating());

        Switch2InputReport sample{};
        sample.gyroscope = {10, -20, 30};
        REQUIRE_FALSE(processor
                          .Process(sample, ControllerType::Pro, false, true, Mode::Yaw,
                                   Activation::Hold, 1.0f, t0)
                          .active);

        sample.gyroscope = {20, -40, 60};
        REQUIRE_FALSE(processor
                          .Process(sample, ControllerType::Pro, false, true, Mode::Yaw,
                                   Activation::Hold, 1.0f, t0 + 1000ms)
                          .active);

        sample.gyroscope = {30, -60, 90};
        REQUIRE_FALSE(processor
                          .Process(sample, ControllerType::Pro, false, true, Mode::Yaw,
                                   Activation::Hold, 1.0f, t0 + 1999ms)
                          .active);
        REQUIRE(processor.IsCalibrating());

        Switch2InputReport active_report{};
        active_report.gyroscope = {20, -40, 60};
        const auto out = processor.Process(active_report, ControllerType::Pro, false, true,
                                           Mode::Yaw, Activation::Hold, 1.0f, t0 + 2000ms);
        REQUIRE_FALSE(processor.IsCalibrating());
        REQUIRE(processor.GetGyroBias()[0] == Catch::Approx(20.0));
        REQUIRE(processor.GetGyroBias()[1] == Catch::Approx(-40.0));
        REQUIRE(processor.GetGyroBias()[2] == Catch::Approx(60.0));
        REQUIRE(out.active);
        REQUIRE(out.velocity_x == 0.0f);
        REQUIRE(out.velocity_y == 0.0f);
    }

    SECTION("Off mode stays inactive regardless of trigger") {
        GyroMouseProcessor processor;
        const Switch2InputReport report{};
        const auto hold_out = processor.Process(report, ControllerType::Pro, false, true, Mode::Off,
                                                Activation::Hold, 1.0f, t0);
        REQUIRE_FALSE(hold_out.active);

        const auto toggle_out = processor.Process(report, ControllerType::Pro, false, true,
                                                  Mode::Off, Activation::Toggle, 1.0f, t0 + 16ms);
        REQUIRE_FALSE(toggle_out.active);
    }

    SECTION("Hold vs Toggle activation modes") {
        GyroMouseProcessor processor;
        const Switch2InputReport report{};

        REQUIRE_FALSE(processor
                          .Process(report, ControllerType::Pro, false, false, Mode::Yaw,
                                   Activation::Hold, 1.0f, t0)
                          .active);
        REQUIRE(processor
                    .Process(report, ControllerType::Pro, false, true, Mode::Yaw, Activation::Hold,
                             1.0f, t0 + 10ms)
                    .active);
        REQUIRE_FALSE(processor
                          .Process(report, ControllerType::Pro, false, false, Mode::Yaw,
                                   Activation::Hold, 1.0f, t0 + 20ms)
                          .active);

        processor.Reset();
        REQUIRE_FALSE(processor
                          .Process(report, ControllerType::Pro, false, false, Mode::Yaw,
                                   Activation::Toggle, 1.0f, t0)
                          .active);
        REQUIRE(processor
                    .Process(report, ControllerType::Pro, false, true, Mode::Yaw,
                             Activation::Toggle, 1.0f, t0 + 10ms)
                    .active);
        REQUIRE(processor
                    .Process(report, ControllerType::Pro, false, true, Mode::Yaw,
                             Activation::Toggle, 1.0f, t0 + 20ms)
                    .active);
        REQUIRE(processor
                    .Process(report, ControllerType::Pro, false, false, Mode::Yaw,
                             Activation::Toggle, 1.0f, t0 + 30ms)
                    .active);
        REQUIRE_FALSE(processor
                          .Process(report, ControllerType::Pro, false, true, Mode::Yaw,
                                   Activation::Toggle, 1.0f, t0 + 40ms)
                          .active);
        REQUIRE_FALSE(processor
                          .Process(report, ControllerType::Pro, false, false, Mode::Yaw,
                                   Activation::Toggle, 1.0f, t0 + 50ms)
                          .active);
    }

    SECTION("Yaw velocity output respects bias threshold, deadzones, and horizontal hold") {
        GyroMouseProcessor processor;
        processor.SetGyroBias({4.0f, 10.0f, -20.0f});

        Switch2InputReport pro_report{};
        pro_report.buttons = static_cast<u32>(PadButton::ZR) | static_cast<u32>(PadButton::ZL);
        pro_report.gyroscope = {20, 10, -120};

        const auto pro_out = processor.Process(pro_report, ControllerType::Pro, false, true,
                                               Mode::Yaw, Activation::Hold, 2.0f, t0);
        REQUIRE(pro_out.active);
        REQUIRE(pro_out.left_click);
        REQUIRE(pro_out.right_click);
        REQUIRE_FALSE(pro_out.roll_steer_x.has_value());
        REQUIRE(pro_out.velocity_x == Catch::Approx(0.3672));
        REQUIRE(pro_out.velocity_y == Catch::Approx(-0.0472));

        processor.SetGyroBias({0.0f, 0.0f, 0.0f});
        Switch2InputReport horiz_report{};
        horiz_report.gyroscope = {0, 20, 0};

        const auto left_out = processor.Process(horiz_report, ControllerType::Left, true, true,
                                                Mode::Yaw, Activation::Hold, 1.0f, t0 + 10ms);
        REQUIRE(left_out.velocity_x == Catch::Approx(0.0));
        REQUIRE(left_out.velocity_y == Catch::Approx(0.0236));

        const auto right_out = processor.Process(horiz_report, ControllerType::Right, true, true,
                                                 Mode::Yaw, Activation::Hold, 1.0f, t0 + 20ms);
        REQUIRE(right_out.velocity_x == Catch::Approx(0.0));
        REQUIRE(right_out.velocity_y == Catch::Approx(-0.0236));
    }

    SECTION("Roll steering output scales and clamps accelerometer tilt") {
        GyroMouseProcessor processor;
        Switch2InputReport report{};
        report.buttons = static_cast<u32>(PadButton::ZR);
        report.accelerometer = {1000, 0, 0};

        const auto mid_out = processor.Process(report, ControllerType::Pro, false, true, Mode::Roll,
                                               Activation::Hold, 1.5f, t0);
        REQUIRE(mid_out.active);
        REQUIRE_FALSE(mid_out.left_click);
        REQUIRE_FALSE(mid_out.right_click);
        REQUIRE(mid_out.velocity_x == 0.0f);
        REQUIRE(mid_out.velocity_y == 0.0f);
        REQUIRE(mid_out.roll_steer_x.has_value());
        REQUIRE(*mid_out.roll_steer_x == Catch::Approx(-0.75));

        report.accelerometer = {-4000, 0, 0};
        const auto clamped_out = processor.Process(report, ControllerType::Pro, false, true,
                                                   Mode::Roll, Activation::Hold, 1.5f, t0 + 10ms);
        REQUIRE(clamped_out.roll_steer_x.has_value());
        REQUIRE(*clamped_out.roll_steer_x == Catch::Approx(1.0));
    }
}

TEST_CASE("OpticalMouseProcessor gates controllers/surface and maps delta, buttons, and scroll",
          "[switch2]") {
    SECTION("Rejects disabled state, non-Joy-Con controllers, and invalid distance/roughness") {
        OpticalMouseProcessor processor;
        const auto valid = MakeValidMouseReport(100U, 200U, 500U, 1000U);

        REQUIRE_FALSE(processor.Process(valid, ControllerType::Right, false).active);
        REQUIRE_FALSE(processor.Process(valid, ControllerType::Pro, true).active);
        REQUIRE_FALSE(processor.Process(valid, ControllerType::None, true).active);
        REQUIRE_FALSE(processor.Process(valid, ControllerType::Dual, true).active);
        REQUIRE_FALSE(processor.Process(valid, ControllerType::GameCube, true).active);

        REQUIRE_FALSE(
            processor.Process(MakeValidMouseReport(100U, 200U, 0U, 1000U), ControllerType::Left, true)
                .active);
        REQUIRE_FALSE(
            processor
                .Process(MakeValidMouseReport(100U, 200U, 1000U, 1000U), ControllerType::Left, true)
                .active);
        REQUIRE_FALSE(
            processor
                .Process(MakeValidMouseReport(100U, 200U, 500U, 4000U), ControllerType::Left, true)
                .active);

        REQUIRE(
            processor
                .Process(MakeValidMouseReport(100U, 200U, 999U, 3999U), ControllerType::Left, true)
                .active);
    }

    SECTION("Computes wrap-around delta and resets previous coordinates after surface loss") {
        OpticalMouseProcessor processor;

        const auto frame1 = processor.Process(MakeValidMouseReport(65530U, 10U, 300U, 500U),
                                              ControllerType::Right, true);
        REQUIRE(frame1.active);
        REQUIRE(frame1.delta_x == 0);
        REQUIRE(frame1.delta_y == 0);

        const auto frame2 = processor.Process(MakeValidMouseReport(5U, 65530U, 300U, 500U),
                                              ControllerType::Right, true);
        REQUIRE(frame2.active);
        REQUIRE(frame2.delta_x == 11);
        REQUIRE(frame2.delta_y == -16);

        const auto lost = processor.Process(MakeValidMouseReport(100U, 100U, 0U, 500U),
                                            ControllerType::Right, true);
        REQUIRE_FALSE(lost.active);

        const auto reacquired = processor.Process(MakeValidMouseReport(250U, 400U, 300U, 500U),
                                                  ControllerType::Right, true);
        REQUIRE(reacquired.active);
        REQUIRE(reacquired.delta_x == 0);
        REQUIRE(reacquired.delta_y == 0);
    }

    SECTION("Maps Joy-Con Left and Right mouse buttons and stick scroll") {
        OpticalMouseProcessor left_proc;
        auto left_report = MakeValidMouseReport(10U, 20U, 200U, 200U);
        left_report.buttons = static_cast<u32>(PadButton::L) | static_cast<u32>(PadButton::StickL) |
                              static_cast<u32>(PadButton::ZL);
        left_report.left_y = 0.2f;
        left_report.right_y = 0.9f;

        const auto left_deadzone = left_proc.Process(left_report, ControllerType::Left, true);
        REQUIRE(left_deadzone.active);
        REQUIRE(left_deadzone.left_button);
        REQUIRE(left_deadzone.middle_button);
        REQUIRE(left_deadzone.right_button);
        REQUIRE(left_deadzone.scroll_y == 0.0f);

        left_report.left_y = -0.65f;
        const auto left_scroll = left_proc.Process(left_report, ControllerType::Left, true);
        REQUIRE(left_scroll.scroll_y == Catch::Approx(-0.65));

        OpticalMouseProcessor right_proc;
        auto right_report = MakeValidMouseReport(10U, 20U, 200U, 200U);
        right_report.buttons = static_cast<u32>(PadButton::R) |
                               static_cast<u32>(PadButton::StickR) |
                               static_cast<u32>(PadButton::ZR);
        right_report.left_y = -0.9f;
        right_report.right_y = -0.2f;

        const auto right_deadzone = right_proc.Process(right_report, ControllerType::Right, true);
        REQUIRE(right_deadzone.active);
        REQUIRE(right_deadzone.left_button);
        REQUIRE(right_deadzone.middle_button);
        REQUIRE(right_deadzone.right_button);
        REQUIRE(right_deadzone.scroll_y == 0.0f);

        right_report.right_y = 0.85f;
        const auto right_scroll = right_proc.Process(right_report, ControllerType::Right, true);
        REQUIRE(right_scroll.scroll_y == Catch::Approx(0.85));
    }
}

namespace {

class MockSwitch2Transport final : public ISwitch2Transport {
public:
    MockSwitch2Transport(ProductId pid_, std::string addr_, TransportType transport_)
        : pid{pid_}, addr{std::move(addr_)}, transport{transport_} {}

    bool IsConnected() const override {
        return connected;
    }

    TransportType GetTransportType() const override {
        return transport;
    }

    std::string GetAddress() const override {
        return addr;
    }

    ProductId GetProductId() const override {
        return pid;
    }

    bool WriteCommand(std::span<const u8> packet, std::vector<u8>& out_response,
                      std::chrono::milliseconds) override {
        if (packet.size() < 8) {
            return false;
        }
        const u8 cmd = packet[0];
        out_response.assign(8, 0);
        out_response[0] = cmd;
        out_response[1] = 0x01;
        out_response[3] = packet[3];
        if (cmd == CommandId::Memory && packet.size() >= 16) {
            const u8 len = packet[8];
            out_response.resize(16 + len, 0);
            out_response[8] = len;
            std::copy_n(packet.begin() + 12, 4, out_response.begin() + 12);
            const u32 memory_addr = static_cast<u32>(packet[12]) |
                                    (static_cast<u32>(packet[13]) << 8) |
                                    (static_cast<u32>(packet[14]) << 16) |
                                    (static_cast<u32>(packet[15]) << 24);
            if (memory_addr == MemoryAddress::ControllerInfo && len >= 22) {
                const u16 raw_pid = static_cast<u16>(pid);
                out_response[16 + 18] = static_cast<u8>(NintendoVendorId & 0xFFU);
                out_response[16 + 19] = static_cast<u8>((NintendoVendorId >> 8) & 0xFFU);
                out_response[16 + 20] = static_cast<u8>(raw_pid & 0xFFU);
                out_response[16 + 21] = static_cast<u8>((raw_pid >> 8) & 0xFFU);
            }
            const auto it = memory.find(memory_addr);
            if (it != memory.end()) {
                const std::size_t copy_len = std::min<std::size_t>(it->second.size(), len);
                std::copy_n(it->second.begin(), copy_len, out_response.begin() + 16);
            }
        }
        return true;
    }

    bool WriteVibration(std::span<const u8>) override {
        if (on_write_vibration) {
            on_write_vibration();
        }
        return true;
    }

    void SetInputCallback(std::function<void(std::span<const u8>)> callback) override {
        input_cb = std::move(callback);
    }

    void SetDisconnectCallback(std::function<void()> callback) override {
        disconnect_cb = std::move(callback);
    }

    void Disconnect() override {
        connected = false;
    }

    void FireInputReport(std::span<const u8> data) const {
        if (input_cb) {
            input_cb(data);
        }
    }

    void FireDisconnect() const {
        if (disconnect_cb) {
            disconnect_cb();
        }
    }

    std::map<u32, std::vector<u8>> memory{};
    std::function<void()> on_write_vibration{};

private:
    ProductId pid;
    std::string addr;
    TransportType transport;
    bool connected{true};
    std::function<void(std::span<const u8>)> input_cb;
    std::function<void()> disconnect_cb;
};

} // namespace

TEST_CASE("Switch2Controllers slot auto-combine, split, merge, and gyro calibration", "[switch2]") {
    const bool prev_enable = Settings::values.enable_switch2_driver.GetValue();
    const bool prev_combine = Settings::values.switch2_combine_joycons.GetValue();
    SCOPE_EXIT {
        Settings::values.enable_switch2_driver = prev_enable;
        Settings::values.switch2_combine_joycons = prev_combine;
    };
    Settings::values.enable_switch2_driver = false;
    Settings::values.switch2_combine_joycons = true;

    InputCommon::Switch2Controllers engine{"switch2", nullptr};

    auto left = std::make_shared<Switch2Driver>(
        std::make_shared<MockSwitch2Transport>(ProductId::JoyCon2Left, "AA:BB:CC:DD:EE:01",
                                               TransportType::Ble));
    auto right = std::make_shared<Switch2Driver>(
        std::make_shared<MockSwitch2Transport>(ProductId::JoyCon2Right, "AA:BB:CC:DD:EE:02",
                                               TransportType::Ble));
    auto pro = std::make_shared<Switch2Driver>(
        std::make_shared<MockSwitch2Transport>(ProductId::ProController2, "usb:1:2",
                                               TransportType::Usb));

    engine.RegisterDriver(left);
    REQUIRE(engine.GetConnectedDevices().size() == 1U);
    REQUIRE(engine.GetConnectedDevices()[0].type == ControllerType::Left);

    engine.RegisterDriver(right);
    auto devices = engine.GetConnectedDevices();
    REQUIRE(devices.size() == 1U);
    REQUIRE(devices[0].type == ControllerType::Dual);

    engine.RegisterDriver(pro);
    devices = engine.GetConnectedDevices();
    REQUIRE(devices.size() == 2U);
    REQUIRE(devices[0].type == ControllerType::Pro);
    REQUIRE(devices[1].type == ControllerType::Dual);

    REQUIRE(engine.SplitSlot(1U));
    devices = engine.GetConnectedDevices();
    REQUIRE(devices.size() == 3U);
    REQUIRE(devices[1].type == ControllerType::Left);
    REQUIRE(devices[2].type == ControllerType::Right);
    REQUIRE(devices[1].horizontal_hold);
    REQUIRE(devices[2].horizontal_hold);

    REQUIRE(engine.MergeSlots(1U, 2U));
    devices = engine.GetConnectedDevices();
    REQUIRE(devices.size() == 2U);
    REQUIRE(devices[1].type == ControllerType::Dual);
    REQUIRE_FALSE(devices[1].horizontal_hold);

    REQUIRE(engine.CalibrateGyro(0U));
    REQUIRE(engine.GetConnectedDevices()[0].is_calibrating);

    engine.UnregisterDriver("usb:1:2");
    REQUIRE(engine.GetConnectedDevices().size() == 1U);
}

namespace {

std::array<u8, MinimumInputReportSize> MakeNativeInputReport(u32 buttons, u16 left_x, u16 left_y,
                                                             u16 right_x, u16 right_y) {
    std::array<u8, MinimumInputReportSize> raw{};
    WriteU32LE(raw, 4, buttons);
    const auto left_packed = PackStickXY(left_x, left_y);
    const auto right_packed = PackStickXY(right_x, right_y);
    std::copy(left_packed.begin(), left_packed.end(), raw.begin() + 10);
    std::copy(right_packed.begin(), right_packed.end(), raw.begin() + 13);
    return raw;
}

PadIdentifier IdentifierFromParam(const Common::ParamPackage& param) {
    return PadIdentifier{
        .guid = Common::UUID{param.Get("guid", "")},
        .port = static_cast<std::size_t>(param.Get("port", 0)),
        .pad = static_cast<std::size_t>(param.Get("pad", 0)),
    };
}

bool ReadMappedButton(const InputCommon::Switch2Controllers& engine,
                      const InputCommon::ButtonMapping& mapping,
                      Settings::NativeButton::Values button) {
    const auto it = mapping.find(button);
    REQUIRE(it != mapping.end());
    return engine.GetButton(IdentifierFromParam(it->second), it->second.Get("button", 0));
}

f32 ReadMappedAxis(const InputCommon::Switch2Controllers& engine,
                   const InputCommon::AnalogMapping& mapping,
                   Settings::NativeAnalog::Values stick, bool y_axis) {
    const auto it = mapping.find(stick);
    REQUIRE(it != mapping.end());
    const int axis = it->second.Get(y_axis ? "axis_y" : "axis_x", 0);
    return engine.GetAxis(IdentifierFromParam(it->second), axis);
}

} // namespace

TEST_CASE("Switch2Controllers single Joy-Con FireInputReport drives NPad-compatible mappings",
          "[switch2]") {
    const bool prev_enable = Settings::values.enable_switch2_driver.GetValue();
    const bool prev_combine = Settings::values.switch2_combine_joycons.GetValue();
    SCOPE_EXIT {
        Settings::values.enable_switch2_driver = prev_enable;
        Settings::values.switch2_combine_joycons = prev_combine;
    };
    Settings::values.enable_switch2_driver = false;
    Settings::values.switch2_combine_joycons = false;

    InputCommon::Switch2Controllers engine{"switch2", nullptr};

    SECTION("Left Joy-Con reports D-pad, SL/SR, L/ZL, Minus, StickL, and LStick") {
        auto transport = std::make_shared<MockSwitch2Transport>(
            ProductId::JoyCon2Left, "AA:BB:CC:DD:EE:11", TransportType::Ble);
        auto driver = std::make_shared<Switch2Driver>(transport);
        engine.RegisterDriver(driver);

        const auto bindings = engine.GetDeviceBindings();
        REQUIRE(bindings.size() == 1U);
        REQUIRE(bindings[0].info.type == ControllerType::Left);

        const u32 buttons = static_cast<u32>(PadButton::Up) | static_cast<u32>(PadButton::Left) |
                            static_cast<u32>(PadButton::LeftSL) |
                            static_cast<u32>(PadButton::LeftSR) | static_cast<u32>(PadButton::L) |
                            static_cast<u32>(PadButton::ZL) | static_cast<u32>(PadButton::Minus) |
                            static_cast<u32>(PadButton::StickL);
        const auto report = MakeNativeInputReport(buttons, 3048U, 1048U, 2048U, 2048U);
        transport->FireInputReport(report);

        const auto btn_map = engine.GetButtonMappingForDevice(bindings[0].params);
        const auto axis_map = engine.GetAnalogMappingForDevice(bindings[0].params);

        REQUIRE(ReadMappedButton(engine, btn_map, Settings::NativeButton::DUp));
        REQUIRE(ReadMappedButton(engine, btn_map, Settings::NativeButton::DLeft));
        REQUIRE(ReadMappedButton(engine, btn_map, Settings::NativeButton::SLLeft));
        REQUIRE(ReadMappedButton(engine, btn_map, Settings::NativeButton::SRLeft));
        REQUIRE(ReadMappedButton(engine, btn_map, Settings::NativeButton::L));
        REQUIRE(ReadMappedButton(engine, btn_map, Settings::NativeButton::ZL));
        REQUIRE(ReadMappedButton(engine, btn_map, Settings::NativeButton::Minus));
        REQUIRE(ReadMappedButton(engine, btn_map, Settings::NativeButton::LStick));

        REQUIRE_FALSE(ReadMappedButton(engine, btn_map, Settings::NativeButton::DDown));
        REQUIRE_FALSE(ReadMappedButton(engine, btn_map, Settings::NativeButton::DRight));
        REQUIRE_FALSE(ReadMappedButton(engine, btn_map, Settings::NativeButton::A));
        REQUIRE_FALSE(ReadMappedButton(engine, btn_map, Settings::NativeButton::B));
        REQUIRE_FALSE(ReadMappedButton(engine, btn_map, Settings::NativeButton::X));
        REQUIRE_FALSE(ReadMappedButton(engine, btn_map, Settings::NativeButton::Y));
        REQUIRE_FALSE(ReadMappedButton(engine, btn_map, Settings::NativeButton::Plus));

        REQUIRE(ReadMappedAxis(engine, axis_map, Settings::NativeAnalog::LStick, false) ==
                Catch::Approx(1000.0 / 1500.0));
        REQUIRE(ReadMappedAxis(engine, axis_map, Settings::NativeAnalog::LStick, true) ==
                Catch::Approx(-1000.0 / 1500.0));
    }

    SECTION("Right Joy-Con reports A/B/X/Y, SL/SR, R/ZR, Plus, StickR, and RStick") {
        auto transport = std::make_shared<MockSwitch2Transport>(
            ProductId::JoyCon2Right, "AA:BB:CC:DD:EE:12", TransportType::Ble);
        auto driver = std::make_shared<Switch2Driver>(transport);
        engine.RegisterDriver(driver);

        const auto bindings = engine.GetDeviceBindings();
        REQUIRE(bindings.size() == 1U);
        REQUIRE(bindings[0].info.type == ControllerType::Right);

        const u32 buttons = static_cast<u32>(PadButton::A) | static_cast<u32>(PadButton::X) |
                            static_cast<u32>(PadButton::RightSL) |
                            static_cast<u32>(PadButton::RightSR) | static_cast<u32>(PadButton::R) |
                            static_cast<u32>(PadButton::ZR) | static_cast<u32>(PadButton::Plus) |
                            static_cast<u32>(PadButton::StickR);
        const auto report = MakeNativeInputReport(buttons, 2048U, 2048U, 1048U, 3048U);
        transport->FireInputReport(report);

        const auto btn_map = engine.GetButtonMappingForDevice(bindings[0].params);
        const auto axis_map = engine.GetAnalogMappingForDevice(bindings[0].params);

        REQUIRE(ReadMappedButton(engine, btn_map, Settings::NativeButton::A));
        REQUIRE(ReadMappedButton(engine, btn_map, Settings::NativeButton::X));
        REQUIRE(ReadMappedButton(engine, btn_map, Settings::NativeButton::SLRight));
        REQUIRE(ReadMappedButton(engine, btn_map, Settings::NativeButton::SRRight));
        REQUIRE(ReadMappedButton(engine, btn_map, Settings::NativeButton::R));
        REQUIRE(ReadMappedButton(engine, btn_map, Settings::NativeButton::ZR));
        REQUIRE(ReadMappedButton(engine, btn_map, Settings::NativeButton::Plus));
        REQUIRE(ReadMappedButton(engine, btn_map, Settings::NativeButton::RStick));

        REQUIRE_FALSE(ReadMappedButton(engine, btn_map, Settings::NativeButton::B));
        REQUIRE_FALSE(ReadMappedButton(engine, btn_map, Settings::NativeButton::Y));
        REQUIRE_FALSE(ReadMappedButton(engine, btn_map, Settings::NativeButton::LStick));
        REQUIRE_FALSE(ReadMappedButton(engine, btn_map, Settings::NativeButton::DUp));

        REQUIRE(ReadMappedAxis(engine, axis_map, Settings::NativeAnalog::RStick, false) ==
                Catch::Approx(-1000.0 / 1500.0));
        REQUIRE(ReadMappedAxis(engine, axis_map, Settings::NativeAnalog::RStick, true) ==
                Catch::Approx(1000.0 / 1500.0));
    }
}

TEST_CASE("Switch2Controllers handles FireDisconnect, early disconnect, and post-Stop callbacks",
          "[switch2]") {
    const bool prev_enable = Settings::values.enable_switch2_driver.GetValue();
    const bool prev_combine = Settings::values.switch2_combine_joycons.GetValue();
    SCOPE_EXIT {
        Settings::values.enable_switch2_driver = prev_enable;
        Settings::values.switch2_combine_joycons = prev_combine;
    };
    Settings::values.enable_switch2_driver = false;
    Settings::values.switch2_combine_joycons = true;

    SECTION("FireDisconnect unregisters controller and splits dual pair cleanly") {
        InputCommon::Switch2Controllers engine{"switch2", nullptr};

        auto left_transport = std::make_shared<MockSwitch2Transport>(
            ProductId::JoyCon2Left, "AA:BB:CC:DD:EE:21", TransportType::Ble);
        auto right_transport = std::make_shared<MockSwitch2Transport>(
            ProductId::JoyCon2Right, "AA:BB:CC:DD:EE:22", TransportType::Ble);

        engine.RegisterDriver(std::make_shared<Switch2Driver>(left_transport));
        engine.RegisterDriver(std::make_shared<Switch2Driver>(right_transport));
        REQUIRE(engine.GetConnectedDevices().size() == 1U);
        REQUIRE(engine.GetConnectedDevices()[0].type == ControllerType::Dual);

        right_transport->Disconnect();
        right_transport->FireDisconnect();
        auto devices = engine.GetConnectedDevices();
        REQUIRE(devices.size() == 1U);
        REQUIRE(devices[0].type == ControllerType::Left);

        left_transport->Disconnect();
        left_transport->FireDisconnect();
        REQUIRE(engine.GetConnectedDevices().empty());
    }

    SECTION("Disconnect during Initialize before BindDriverCallbacks does not leave a dead slot") {
        InputCommon::Switch2Controllers engine{"switch2", nullptr};

        auto transport = std::make_shared<MockSwitch2Transport>(
            ProductId::ProController2, "AA:BB:CC:DD:EE:23", TransportType::Ble);
        transport->on_write_vibration = [raw = transport.get()]() {
            raw->Disconnect();
            raw->FireDisconnect();
        };

        auto driver = std::make_shared<Switch2Driver>(transport);
        engine.RegisterDriver(driver);
        REQUIRE(engine.GetConnectedDevices().empty());
        REQUIRE(engine.GetDeviceBindings().empty());
    }

    SECTION("Switch2Driver::Stop suppresses subsequent FireInputReport and FireDisconnect") {
        auto transport = std::make_shared<MockSwitch2Transport>(
            ProductId::ProController2, "AA:BB:CC:DD:EE:24", TransportType::Ble);
        auto driver = std::make_shared<Switch2Driver>(transport);

        int input_count = 0;
        int disconnect_count = 0;
        driver->SetCallbacks(Switch2Callbacks{
            .on_input_report = [&input_count](const Switch2InputReport&) { ++input_count; },
            .on_controller_info = {},
            .on_disconnect = [&disconnect_count]() { ++disconnect_count; },
        });
        REQUIRE(driver->Initialize(1, false) == Common::Input::DriverResult::Success);

        const auto report = MakeNativeInputReport(static_cast<u32>(PadButton::A), 2048U, 2048U,
                                                  2048U, 2048U);
        transport->FireInputReport(report);
        REQUIRE(input_count == 1);

        driver->Stop();
        transport->FireInputReport(report);
        transport->FireDisconnect();
        REQUIRE(input_count == 1);
        REQUIRE(disconnect_count == 0);
    }

    SECTION("Side button press during Initialize prevents Joy-Con auto-combine") {
        InputCommon::Switch2Controllers engine{"switch2", nullptr};

        auto left_transport = std::make_shared<MockSwitch2Transport>(
            ProductId::JoyCon2Left, "AA:BB:CC:DD:EE:51", TransportType::Ble);
        auto right_transport = std::make_shared<MockSwitch2Transport>(
            ProductId::JoyCon2Right, "AA:BB:CC:DD:EE:52", TransportType::Ble);

        right_transport->on_write_vibration = [&] {
            const auto report = MakeNativeInputReport(
                static_cast<u32>(PadButton::RightSL), 2048U, 2048U, 2048U, 2048U);
            right_transport->FireInputReport(report);
        };

        auto left_driver = std::make_shared<Switch2Driver>(left_transport);
        auto right_driver = std::make_shared<Switch2Driver>(right_transport);

        engine.RegisterDriver(left_driver);
        engine.RegisterDriver(right_driver);

        REQUIRE(right_driver->WasSideButtonPressed());
        const auto devices = engine.GetConnectedDevices();
        REQUIRE(devices.size() == 2U);
        REQUIRE(devices[0].type == ControllerType::Left);
        REQUIRE(devices[1].type == ControllerType::Right);
    }
}

TEST_CASE("Switch2Controllers GyroMouseMode::Roll in Dual Joy-Con mode drives LeftStickX",
          "[switch2]") {
    const bool prev_enable = Settings::values.enable_switch2_driver.GetValue();
    const bool prev_combine = Settings::values.switch2_combine_joycons.GetValue();
    const auto prev_gyro_mode = Settings::values.switch2_gyro_mouse_mode.GetValue();
    const auto prev_gyro_act = Settings::values.switch2_gyro_activation_mode.GetValue();
    const auto prev_gyro_sens = Settings::values.switch2_gyro_sensitivity.GetValue();
    SCOPE_EXIT {
        Settings::values.enable_switch2_driver = prev_enable;
        Settings::values.switch2_combine_joycons = prev_combine;
        Settings::values.switch2_gyro_mouse_mode = prev_gyro_mode;
        Settings::values.switch2_gyro_activation_mode = prev_gyro_act;
        Settings::values.switch2_gyro_sensitivity = prev_gyro_sens;
    };

    Settings::values.enable_switch2_driver = false;
    Settings::values.switch2_combine_joycons = true;
    Settings::values.switch2_gyro_mouse_mode = Settings::Switch2GyroMouseMode::Roll;
    Settings::values.switch2_gyro_activation_mode = Settings::Switch2GyroActivationMode::Hold;
    Settings::values.switch2_gyro_sensitivity = 10;

    InputCommon::Switch2Controllers engine{"switch2", nullptr};
    auto left_transport = std::make_shared<MockSwitch2Transport>(
        ProductId::JoyCon2Left, "AA:BB:CC:DD:EE:61", TransportType::Ble);
    auto right_transport = std::make_shared<MockSwitch2Transport>(
        ProductId::JoyCon2Right, "AA:BB:CC:DD:EE:62", TransportType::Ble);

    engine.RegisterDriver(std::make_shared<Switch2Driver>(left_transport));
    engine.RegisterDriver(std::make_shared<Switch2Driver>(right_transport));

    const auto bindings = engine.GetDeviceBindings();
    REQUIRE(bindings.size() == 1U);
    REQUIRE(bindings[0].info.type == ControllerType::Dual);
    const auto axis_map = engine.GetAnalogMappingForDevice(bindings[0].params);

    auto right_roll = MakeNativeInputReport(static_cast<u32>(PadButton::RightSL), 2048U, 2048U,
                                            2048U, 2048U);
    WriteS16LE(right_roll, 48, 1000);
    right_transport->FireInputReport(right_roll);
    REQUIRE(ReadMappedAxis(engine, axis_map, Settings::NativeAnalog::LStick, false) ==
            Catch::Approx(-0.5));

    // Subsequent Left Joy-Con reports must preserve active roll steering on LeftStickX.
    const auto left_stick = MakeNativeInputReport(0U, 3048U, 2048U, 2048U, 2048U);
    left_transport->FireInputReport(left_stick);
    REQUIRE(ReadMappedAxis(engine, axis_map, Settings::NativeAnalog::LStick, false) ==
            Catch::Approx(-0.5));

    // Releasing the gyro trigger immediately restores LeftStickX to Left's last stick reading.
    const auto right_idle = MakeNativeInputReport(0U, 2048U, 2048U, 2048U, 2048U);
    right_transport->FireInputReport(right_idle);
    REQUIRE(ReadMappedAxis(engine, axis_map, Settings::NativeAnalog::LStick, false) ==
            Catch::Approx(1000.0 / 1500.0));

    // Splitting the Dual slot while roll steering is active also restores LeftStickX immediately.
    right_transport->FireInputReport(right_roll);
    REQUIRE(ReadMappedAxis(engine, axis_map, Settings::NativeAnalog::LStick, false) ==
            Catch::Approx(-0.5));
    REQUIRE(engine.SplitSlot(0U));
    REQUIRE(ReadMappedAxis(engine, axis_map, Settings::NativeAnalog::LStick, false) ==
            Catch::Approx(1000.0 / 1500.0));
}

TEST_CASE("Switch2Controllers isolates mouse ownership across multiple controllers", "[switch2]") {
    const bool prev_enable = Settings::values.enable_switch2_driver.GetValue();
    const bool prev_combine = Settings::values.switch2_combine_joycons.GetValue();
    const auto prev_gyro_mode = Settings::values.switch2_gyro_mouse_mode.GetValue();
    const auto prev_gyro_act = Settings::values.switch2_gyro_activation_mode.GetValue();
    const bool prev_optical = Settings::values.switch2_optical_mouse.GetValue();
    SCOPE_EXIT {
        Settings::values.enable_switch2_driver = prev_enable;
        Settings::values.switch2_combine_joycons = prev_combine;
        Settings::values.switch2_gyro_mouse_mode = prev_gyro_mode;
        Settings::values.switch2_gyro_activation_mode = prev_gyro_act;
        Settings::values.switch2_optical_mouse = prev_optical;
    };

    Settings::values.enable_switch2_driver = false;
    Settings::values.switch2_combine_joycons = false;
    Settings::values.switch2_gyro_mouse_mode = Settings::Switch2GyroMouseMode::Yaw;
    Settings::values.switch2_gyro_activation_mode = Settings::Switch2GyroActivationMode::Hold;
    Settings::values.switch2_optical_mouse = false;

    InputCommon::Mouse mouse_engine{"mouse"};
    InputCommon::Switch2Controllers engine{"switch2", &mouse_engine};

    auto t1 = std::make_shared<MockSwitch2Transport>(ProductId::ProController2, "usb:1:1",
                                                     TransportType::Usb);
    auto t2 = std::make_shared<MockSwitch2Transport>(ProductId::ProController2, "usb:1:2",
                                                     TransportType::Usb);
    engine.RegisterDriver(std::make_shared<Switch2Driver>(t1));
    engine.RegisterDriver(std::make_shared<Switch2Driver>(t2));

    const PadIdentifier real_mouse_id{
        .guid = Common::UUID{},
        .port = 1,
        .pad = 0,
    };
    const int left_btn = static_cast<int>(InputCommon::MouseButton::Left);

    const auto active_click = MakeNativeInputReport(
        static_cast<u32>(PadButton::GR) | static_cast<u32>(PadButton::ZR), 2048U, 2048U, 2048U,
        2048U);
    const auto active_no_click =
        MakeNativeInputReport(static_cast<u32>(PadButton::GR), 2048U, 2048U, 2048U, 2048U);
    const auto idle_report = MakeNativeInputReport(0U, 2048U, 2048U, 2048U, 2048U);

    t1->FireInputReport(active_click);
    REQUIRE(mouse_engine.GetButton(real_mouse_id, left_btn));

    // A second controller activating mouse while t1 owns it must not steal ownership mid-drag.
    t2->FireInputReport(active_no_click);
    REQUIRE(mouse_engine.GetButton(real_mouse_id, left_btn));

    // Idle input or disconnect from a non-owner controller must not release the active mouse.
    t2->FireInputReport(idle_report);
    REQUIRE(mouse_engine.GetButton(real_mouse_id, left_btn));
    engine.UnregisterDriver("usb:1:2");
    REQUIRE(mouse_engine.GetButton(real_mouse_id, left_btn));

    // Only the owner going idle or disconnecting clears the mouse state.
    t1->FireInputReport(idle_report);
    REQUIRE_FALSE(mouse_engine.GetButton(real_mouse_id, left_btn));

    t1->FireInputReport(active_click);
    REQUIRE(mouse_engine.GetButton(real_mouse_id, left_btn));
    engine.UnregisterDriver("usb:1:1");
    REQUIRE_FALSE(mouse_engine.GetButton(real_mouse_id, left_btn));

    SECTION("Dual Joy-Con Left optical mouse releases ownership and buttons when lifted") {
        Settings::values.switch2_combine_joycons = true;
        Settings::values.switch2_optical_mouse = true;

        auto left_t = std::make_shared<MockSwitch2Transport>(
            ProductId::JoyCon2Left, "AA:BB:CC:DD:EE:71", TransportType::Ble);
        auto right_t = std::make_shared<MockSwitch2Transport>(
            ProductId::JoyCon2Right, "AA:BB:CC:DD:EE:72", TransportType::Ble);
        engine.RegisterDriver(std::make_shared<Switch2Driver>(left_t));
        engine.RegisterDriver(std::make_shared<Switch2Driver>(right_t));
        REQUIRE(engine.GetConnectedDevices().size() == 1U);
        REQUIRE(engine.GetConnectedDevices()[0].type == ControllerType::Dual);

        auto left_optical = MakeNativeInputReport(static_cast<u32>(PadButton::L), 2048U, 2048U,
                                                  2048U, 2048U);
        WriteU16LE(left_optical, 16, 100U);
        WriteU16LE(left_optical, 18, 200U);
        WriteU16LE(left_optical, 20, 500U);
        WriteU16LE(left_optical, 22, 300U);

        left_t->FireInputReport(left_optical);
        REQUIRE(mouse_engine.GetButton(real_mouse_id, left_btn));

        // Lifting the Left Joy-Con (distance = 0) in Dual mode must immediately release the
        // mouse button and clear ownership so Right can claim mouse next.
        left_t->FireInputReport(idle_report);
        REQUIRE_FALSE(mouse_engine.GetButton(real_mouse_id, left_btn));

        const auto right_gyro_click = MakeNativeInputReport(
            static_cast<u32>(PadButton::RightSL) | static_cast<u32>(PadButton::ZR), 2048U, 2048U,
            2048U, 2048U);
        right_t->FireInputReport(right_gyro_click);
        REQUIRE(mouse_engine.GetButton(real_mouse_id, left_btn));
        right_t->FireInputReport(idle_report);
        REQUIRE_FALSE(mouse_engine.GetButton(real_mouse_id, left_btn));
    }
}

TEST_CASE("Switch2Controllers GetUIName recognizes analog axis_x and axis_y mappings",
          "[switch2]") {
    const bool prev_enable = Settings::values.enable_switch2_driver.GetValue();
    SCOPE_EXIT {
        Settings::values.enable_switch2_driver = prev_enable;
    };
    Settings::values.enable_switch2_driver = false;

    InputCommon::Switch2Controllers engine{"switch2", nullptr};
    auto transport = std::make_shared<MockSwitch2Transport>(ProductId::ProController2, "usb:1:3",
                                                            TransportType::Usb);
    engine.RegisterDriver(std::make_shared<Switch2Driver>(transport));

    const auto bindings = engine.GetDeviceBindings();
    REQUIRE(bindings.size() == 1U);
    const auto analog_map = engine.GetAnalogMappingForDevice(bindings[0].params);
    REQUIRE(engine.GetUIName(analog_map.at(Settings::NativeAnalog::LStick)) ==
            Common::Input::ButtonNames::Value);
    REQUIRE(engine.GetUIName(analog_map.at(Settings::NativeAnalog::RStick)) ==
            Common::Input::ButtonNames::Value);
    REQUIRE(engine.GetUIName(Common::ParamPackage{{"engine", "switch2"}, {"axis", "0"}}) ==
            Common::Input::ButtonNames::Value);
    REQUIRE(engine.GetUIName(Common::ParamPackage{{"engine", "switch2"}, {"axis_y", "1"}}) ==
            Common::Input::ButtonNames::Value);
    REQUIRE(engine.GetUIName(Common::ParamPackage{{"engine", "switch2"}}) ==
            Common::Input::ButtonNames::Invalid);
}

TEST_CASE("Switch2Controllers Dual Joy-Con Roll steering handles disconnect, MergeSlots, and mouse "
          "isolation",
          "[switch2]") {
    const bool prev_enable = Settings::values.enable_switch2_driver.GetValue();
    const bool prev_combine = Settings::values.switch2_combine_joycons.GetValue();
    const auto prev_gyro_mode = Settings::values.switch2_gyro_mouse_mode.GetValue();
    const auto prev_gyro_act = Settings::values.switch2_gyro_activation_mode.GetValue();
    const auto prev_gyro_sens = Settings::values.switch2_gyro_sensitivity.GetValue();
    const bool prev_optical = Settings::values.switch2_optical_mouse.GetValue();
    SCOPE_EXIT {
        Settings::values.enable_switch2_driver = prev_enable;
        Settings::values.switch2_combine_joycons = prev_combine;
        Settings::values.switch2_gyro_mouse_mode = prev_gyro_mode;
        Settings::values.switch2_gyro_activation_mode = prev_gyro_act;
        Settings::values.switch2_gyro_sensitivity = prev_gyro_sens;
        Settings::values.switch2_optical_mouse = prev_optical;
    };

    Settings::values.enable_switch2_driver = false;
    Settings::values.switch2_combine_joycons = true;
    Settings::values.switch2_gyro_mouse_mode = Settings::Switch2GyroMouseMode::Roll;
    Settings::values.switch2_gyro_activation_mode = Settings::Switch2GyroActivationMode::Hold;
    Settings::values.switch2_gyro_sensitivity = 10;
    Settings::values.switch2_optical_mouse = false;

    InputCommon::Mouse mouse_engine{"mouse"};
    InputCommon::Switch2Controllers engine{"switch2", &mouse_engine};

    const PadIdentifier real_mouse_id{
        .guid = Common::UUID{},
        .port = 1,
        .pad = 0,
    };
    const int left_btn = static_cast<int>(InputCommon::MouseButton::Left);

    const auto left_stick = MakeNativeInputReport(0U, 3048U, 2048U, 2048U, 2048U);
    auto right_roll = MakeNativeInputReport(
        static_cast<u32>(PadButton::RightSL) | static_cast<u32>(PadButton::ZR), 2048U, 2048U,
        2048U, 2048U);
    WriteS16LE(right_roll, 48, 1000);
    const auto idle_report = MakeNativeInputReport(0U, 2048U, 2048U, 2048U, 2048U);

    SECTION("Right Joy-Con disconnect during active Roll steering restores LeftStickX and never "
            "presses mouse buttons") {
        auto left_transport = std::make_shared<MockSwitch2Transport>(
            ProductId::JoyCon2Left, "AA:BB:CC:DD:EE:81", TransportType::Ble);
        auto right_transport = std::make_shared<MockSwitch2Transport>(
            ProductId::JoyCon2Right, "AA:BB:CC:DD:EE:82", TransportType::Ble);

        engine.RegisterDriver(std::make_shared<Switch2Driver>(left_transport));
        engine.RegisterDriver(std::make_shared<Switch2Driver>(right_transport));

        const auto bindings = engine.GetDeviceBindings();
        REQUIRE(bindings.size() == 1U);
        REQUIRE(bindings[0].info.type == ControllerType::Dual);
        const auto axis_map = engine.GetAnalogMappingForDevice(bindings[0].params);

        left_transport->FireInputReport(left_stick);
        REQUIRE(ReadMappedAxis(engine, axis_map, Settings::NativeAnalog::LStick, false) ==
                Catch::Approx(1000.0 / 1500.0));

        right_transport->FireInputReport(right_roll);
        REQUIRE(ReadMappedAxis(engine, axis_map, Settings::NativeAnalog::LStick, false) ==
                Catch::Approx(-0.5));
        REQUIRE_FALSE(mouse_engine.GetButton(real_mouse_id, left_btn));

        engine.UnregisterDriver(right_transport->GetAddress());
        const auto post_bindings = engine.GetDeviceBindings();
        REQUIRE(post_bindings.size() == 1U);
        REQUIRE(post_bindings[0].info.type == ControllerType::Left);
        const auto post_axis_map = engine.GetAnalogMappingForDevice(post_bindings[0].params);
        REQUIRE(ReadMappedAxis(engine, post_axis_map, Settings::NativeAnalog::LStick, false) ==
                Catch::Approx(1000.0 / 1500.0));
        REQUIRE_FALSE(mouse_engine.GetButton(real_mouse_id, left_btn));
    }

    SECTION("Right Joy-Con disconnect with slot reorder restores LeftStickX on the new slot") {
        Settings::values.switch2_combine_joycons = false;

        auto pro_transport = std::make_shared<MockSwitch2Transport>(
            ProductId::ProController2, "usb:pro", TransportType::Usb);
        auto left_transport = std::make_shared<MockSwitch2Transport>(
            ProductId::JoyCon2Left, "AA:BB:CC:DD:EE:83", TransportType::Ble);
        auto right_transport = std::make_shared<MockSwitch2Transport>(
            ProductId::JoyCon2Right, "AA:BB:CC:DD:EE:84", TransportType::Ble);

        engine.RegisterDriver(std::make_shared<Switch2Driver>(pro_transport));
        engine.RegisterDriver(std::make_shared<Switch2Driver>(left_transport));
        engine.RegisterDriver(std::make_shared<Switch2Driver>(right_transport));
        REQUIRE(engine.MergeSlots(1U, 2U));

        const auto bindings = engine.GetDeviceBindings();
        REQUIRE(bindings.size() == 2U);
        REQUIRE(bindings[0].info.type == ControllerType::Pro);
        REQUIRE(bindings[1].info.type == ControllerType::Dual);
        REQUIRE(bindings[1].info.slot_index == 1U);
        const auto dual_axis_map = engine.GetAnalogMappingForDevice(bindings[1].params);

        left_transport->FireInputReport(left_stick);
        REQUIRE(ReadMappedAxis(engine, dual_axis_map, Settings::NativeAnalog::LStick, false) ==
                Catch::Approx(1000.0 / 1500.0));

        right_transport->FireInputReport(right_roll);
        REQUIRE(ReadMappedAxis(engine, dual_axis_map, Settings::NativeAnalog::LStick, false) ==
                Catch::Approx(-0.5));
        REQUIRE_FALSE(mouse_engine.GetButton(real_mouse_id, left_btn));

        engine.UnregisterDriver("usb:pro");
        const auto reordered_bindings = engine.GetDeviceBindings();
        REQUIRE(reordered_bindings.size() == 1U);
        REQUIRE(reordered_bindings[0].info.type == ControllerType::Dual);
        REQUIRE(reordered_bindings[0].info.slot_index == 0U);

        engine.UnregisterDriver(right_transport->GetAddress());
        const auto post_bindings = engine.GetDeviceBindings();
        REQUIRE(post_bindings.size() == 1U);
        REQUIRE(post_bindings[0].info.type == ControllerType::Left);
        REQUIRE(post_bindings[0].info.slot_index == 0U);
        const auto slot0_axis_map = engine.GetAnalogMappingForDevice(post_bindings[0].params);
        REQUIRE(ReadMappedAxis(engine, slot0_axis_map, Settings::NativeAnalog::LStick, false) ==
                Catch::Approx(1000.0 / 1500.0));
    }

    SECTION("MergeSlots and SplitSlot preserve pre-merge LeftStickX across Right-then-Left "
            "registration") {
        Settings::values.switch2_combine_joycons = false;

        auto right_transport = std::make_shared<MockSwitch2Transport>(
            ProductId::JoyCon2Right, "AA:BB:CC:DD:EE:85", TransportType::Ble);
        auto left_transport = std::make_shared<MockSwitch2Transport>(
            ProductId::JoyCon2Left, "AA:BB:CC:DD:EE:86", TransportType::Ble);

        engine.RegisterDriver(std::make_shared<Switch2Driver>(right_transport));
        engine.RegisterDriver(std::make_shared<Switch2Driver>(left_transport));

        // ReorderSlotsLocked() sorts Left (priority 3) ahead of Right (priority 4).
        const auto pre_bindings = engine.GetDeviceBindings();
        REQUIRE(pre_bindings.size() == 2U);
        REQUIRE(pre_bindings[0].info.type == ControllerType::Left);
        REQUIRE(pre_bindings[1].info.type == ControllerType::Right);
        const auto left_pre_axis_map = engine.GetAnalogMappingForDevice(pre_bindings[0].params);

        left_transport->FireInputReport(left_stick);
        REQUIRE(ReadMappedAxis(engine, left_pre_axis_map, Settings::NativeAnalog::LStick, false) ==
                Catch::Approx(1000.0 / 1500.0));

        REQUIRE(engine.MergeSlots(0U, 1U));
        const auto dual_bindings = engine.GetDeviceBindings();
        REQUIRE(dual_bindings.size() == 1U);
        REQUIRE(dual_bindings[0].info.type == ControllerType::Dual);
        const auto dual_axis_map = engine.GetAnalogMappingForDevice(dual_bindings[0].params);

        right_transport->FireInputReport(right_roll);
        REQUIRE(ReadMappedAxis(engine, dual_axis_map, Settings::NativeAnalog::LStick, false) ==
                Catch::Approx(-0.5));
        REQUIRE_FALSE(mouse_engine.GetButton(real_mouse_id, left_btn));

        right_transport->FireInputReport(idle_report);
        REQUIRE(ReadMappedAxis(engine, dual_axis_map, Settings::NativeAnalog::LStick, false) ==
                Catch::Approx(1000.0 / 1500.0));

        right_transport->FireInputReport(right_roll);
        REQUIRE(ReadMappedAxis(engine, dual_axis_map, Settings::NativeAnalog::LStick, false) ==
                Catch::Approx(-0.5));

        REQUIRE(engine.SplitSlot(0U));
        const auto split_bindings = engine.GetDeviceBindings();
        REQUIRE(split_bindings.size() == 2U);
        const auto split_left =
            std::find_if(split_bindings.begin(), split_bindings.end(),
                         [](const auto& b) { return b.info.type == ControllerType::Left; });
        REQUIRE(split_left != split_bindings.end());
        const auto split_left_axis_map = engine.GetAnalogMappingForDevice(split_left->params);
        REQUIRE(
            ReadMappedAxis(engine, split_left_axis_map, Settings::NativeAnalog::LStick, false) ==
            Catch::Approx(1000.0 / 1500.0));
    }

    SECTION("Auto-combined Right-primary Dual Joy-Con restores LeftStickX on SplitSlot with slot "
            "reorder") {
        auto right1 = std::make_shared<MockSwitch2Transport>(
            ProductId::JoyCon2Right, "AA:BB:CC:DD:EE:87", TransportType::Ble);
        auto left1 = std::make_shared<MockSwitch2Transport>(
            ProductId::JoyCon2Left, "AA:BB:CC:DD:EE:88", TransportType::Ble);
        auto left2 = std::make_shared<MockSwitch2Transport>(
            ProductId::JoyCon2Left, "AA:BB:CC:DD:EE:89", TransportType::Ble);
        auto right2 = std::make_shared<MockSwitch2Transport>(
            ProductId::JoyCon2Right, "AA:BB:CC:DD:EE:8A", TransportType::Ble);

        engine.RegisterDriver(std::make_shared<Switch2Driver>(right1));
        engine.RegisterDriver(std::make_shared<Switch2Driver>(left1));
        engine.RegisterDriver(std::make_shared<Switch2Driver>(left2));
        engine.RegisterDriver(std::make_shared<Switch2Driver>(right2));

        const auto bindings = engine.GetDeviceBindings();
        REQUIRE(bindings.size() == 2U);
        REQUIRE(bindings[0].info.type == ControllerType::Dual);
        REQUIRE(bindings[0].info.address == "AA:BB:CC:DD:EE:87 + AA:BB:CC:DD:EE:88");
        const auto dual_axis_map = engine.GetAnalogMappingForDevice(bindings[0].params);

        left1->FireInputReport(left_stick);
        right1->FireInputReport(right_roll);
        REQUIRE(ReadMappedAxis(engine, dual_axis_map, Settings::NativeAnalog::LStick, false) ==
                Catch::Approx(-0.5));
        REQUIRE_FALSE(mouse_engine.GetButton(real_mouse_id, left_btn));

        REQUIRE(engine.SplitSlot(0U));
        const auto split_bindings = engine.GetDeviceBindings();
        REQUIRE(split_bindings.size() == 3U);
        const auto split_left =
            std::find_if(split_bindings.begin(), split_bindings.end(),
                         [](const auto& b) { return b.info.address == "AA:BB:CC:DD:EE:88"; });
        REQUIRE(split_left != split_bindings.end());
        REQUIRE(split_left->info.type == ControllerType::Left);
        REQUIRE(split_left->info.slot_index == 1U);
        const auto split_left_axis_map = engine.GetAnalogMappingForDevice(split_left->params);
        REQUIRE(
            ReadMappedAxis(engine, split_left_axis_map, Settings::NativeAnalog::LStick, false) ==
            Catch::Approx(1000.0 / 1500.0));
    }
}

TEST_CASE("Switch2 protocol codecs match Rust FFI parity", "[switch2]") {
    const auto exercise_codecs = [] {
        REQUIRE(FeatureMaskForProduct(ProductId::JoyCon2Left) == FeatureFlags::JoyCon);
        REQUIRE(FeatureMaskForProduct(ProductId::ProController2) == FeatureFlags::Standard);

        std::array<u8, MinimumInputReportSize + 1> usb_raw{};
        usb_raw[0] = CommonReportId;
        WriteU32LE(usb_raw, 1, 0x11223344U);
        const auto stripped = StripHidReportId(usb_raw);
        REQUIRE(stripped.has_value());
        REQUIRE(stripped->size() == MinimumInputReportSize);

        std::array<u8, 16> adv{};
        WriteU16LE(adv, 3, NintendoVendorId);
        WriteU16LE(adv, 5, static_cast<u16>(ProductId::JoyCon2Right));
        adv[10] = 0x11;
        adv[15] = 0x66;
        const auto ble = ParseBleManufacturerData(adv);
        REQUIRE(ble.has_value());
        REQUIRE(ble->product_id == ProductId::JoyCon2Right);
        REQUIRE_FALSE(ble->is_pairing_mode);

        std::array<u8, UserCalibrationBlockSize> user_cal{};
        WriteU16LE(user_cal, 0, UserCalibrationMagic);
        const auto c_pack = PackStickXY(2050U, 2040U);
        const auto max_pack = PackStickXY(1450U, 1550U);
        const auto min_pack = PackStickXY(1400U, 1300U);
        std::copy(c_pack.begin(), c_pack.end(), user_cal.begin() + 2);
        std::copy(max_pack.begin(), max_pack.end(), user_cal.begin() + 5);
        std::copy(min_pack.begin(), min_pack.end(), user_cal.begin() + 8);
        const auto parsed_user = ParseUserStickCalibration(user_cal);
        REQUIRE(parsed_user.has_value());
        REQUIRE(parsed_user->center_x == 2050U);
        REQUIRE(parsed_user->min_y == 1300U);

        std::array<u8, 37> cinfo_buf{};
        cinfo_buf[2] = 'S';
        cinfo_buf[3] = 'W';
        cinfo_buf[4] = '2';
        WriteU16LE(cinfo_buf, 18, NintendoVendorId);
        WriteU16LE(cinfo_buf, 20, static_cast<u16>(ProductId::NsoGameCube));
        WriteRgb24(cinfo_buf, 25, 0x123456U);
        const auto cinfo = ParseControllerInfo(cinfo_buf);
        REQUIRE(cinfo.has_value());
        REQUIRE(cinfo->serial_number == "SW2");
        REQUIRE(cinfo->product_id == ProductId::NsoGameCube);
        REQUIRE(cinfo->body_color == 0x123456U);

        const VibrationPacketData vib = VibrationPacketData::FromVibrationStatus(
            {.low_amplitude = 0.6f,
             .low_frequency = 120.0f,
             .high_amplitude = 0.4f,
             .high_frequency = 240.0f,
             .type = Common::Input::VibrationAmplificationType::Linear});
        const auto enc5 = vib.Encode5Bytes();
        const auto ble_vib = BuildVibrationReport(0x03U, true, vib);
        REQUIRE(ble_vib.size() == 33U);
        REQUIRE(std::equal(enc5.begin(), enc5.end(), ble_vib.begin() + 2));

        const auto usb_pro = BuildUsbVibrationReport(ProductId::ProController2, 0x05U, vib);
        REQUIRE(usb_pro.size() == 64U);
        REQUIRE(usb_pro[0] == 0x02U);
        REQUIRE(usb_pro[1] == 0x55U);
        REQUIRE(std::equal(enc5.begin(), enc5.end(), usb_pro.begin() + 2));

        const auto usb_gc = BuildUsbVibrationReport(ProductId::NsoGameCube, 0x05U, vib);
        REQUIRE(usb_gc == std::vector<u8>{0x03, 0x55, 0x01, 0, 0, 0, 0});

        const auto usb_jc = BuildUsbVibrationReport(ProductId::JoyCon2Left, 0x05U, vib);
        REQUIRE(usb_jc.size() == 7U);
        REQUIRE(usb_jc[0] == 0x01U);

        auto input_raw = MakeNativeInputReport(static_cast<u32>(PadButton::A), 2600U, 1500U,
                                               1800U, 2400U);
        WriteS16LE(input_raw, 48, 2048);
        WriteS16LE(input_raw, 54, 100);
        const auto parsed_input =
            ParseInputReport(input_raw, *parsed_user, *parsed_user, DefaultStickDeadzone);
        REQUIRE(parsed_input.has_value());
        const auto motion = ConvertImuToMotion(*parsed_input, {0.0f, 0.0f, 0.0f}, 4000ULL);
        REQUIRE(motion.accel_x == Catch::Approx(0.5));
        REQUIRE(motion.delta_timestamp == 4000ULL);

        const auto mem_req = BuildMemoryReadPayload(0x40U, MemoryAddress::ControllerInfo);
        const auto cmd_pkt =
            BuildCommandPacket(CommandId::Memory, SubCommandId::MemoryRead, mem_req, true);
        REQUIRE(cmd_pkt.size() == 16U);

        REQUIRE(ReverseLowerBits4(0x01U) == 0x08U);
        REQUIRE(SignedLoopingDifference16(65530U, 5U) == 11);

        GyroMouseProcessor gyro_proc;
        const auto t0 = std::chrono::steady_clock::time_point{10s};
        const auto g_out = gyro_proc.Process(
            *parsed_input, ControllerType::Pro, false, true, Settings::Switch2GyroMouseMode::Yaw,
            Settings::Switch2GyroActivationMode::Hold, 1.0f, t0);
        REQUIRE(g_out.active);

        OpticalMouseProcessor opt_proc;
        const auto valid_mouse = MakeValidMouseReport(100U, 200U, 300U, 400U);
        REQUIRE(opt_proc.Process(valid_mouse, ControllerType::Left, true).active);
    };

    exercise_codecs();

#ifdef EGLANTINE_ENABLE_RUST_SWITCH2_IO
    const auto set_env = [](const char* name, const char* value) {
#ifdef _WIN32
        _putenv_s(name, value != nullptr ? value : "");
#else
        if (value != nullptr) {
            setenv(name, value, 1);
        } else {
            unsetenv(name);
        }
#endif
    };
    const auto get_env = [](const char* name) -> std::pair<bool, std::string> {
        const char* val = std::getenv(name);
        return val != nullptr ? std::pair<bool, std::string>{true, std::string{val}}
                              : std::pair<bool, std::string>{false, std::string{}};
    };
    const auto prev_mode = get_env("EDEN_NX2_RUST_SWITCH2_IO");
    const auto prev_verify = get_env("EDEN_NX2_RUST_SWITCH2_IO_VERIFY");
    SCOPE_EXIT {
        set_env("EDEN_NX2_RUST_SWITCH2_IO", prev_mode.first ? prev_mode.second.c_str() : nullptr);
        set_env("EDEN_NX2_RUST_SWITCH2_IO_VERIFY",
                prev_verify.first ? prev_verify.second.c_str() : nullptr);
    };

    set_env("EDEN_NX2_RUST_SWITCH2_IO_VERIFY", "1");
    set_env("EDEN_NX2_RUST_SWITCH2_IO", nullptr);
    exercise_codecs();

    set_env("EDEN_NX2_RUST_SWITCH2_IO_VERIFY", nullptr);
    set_env("EDEN_NX2_RUST_SWITCH2_IO", "1");
    exercise_codecs();

    // Check the BLE UUIDs exported by the C ABI.
    std::size_t uuid_len = 0;
    const char* uuid_ptr = egl_sw2_ble_uuid(EGL_SW2_UUID_INPUT_REPORT, &uuid_len);
    REQUIRE(std::string_view{uuid_ptr, uuid_len} == InputReportUuid);
    uuid_ptr = egl_sw2_ble_uuid(EGL_SW2_UUID_COMMAND_WRITE, &uuid_len);
    REQUIRE(std::string_view{uuid_ptr, uuid_len} == CommandWriteUuid);
    uuid_ptr = egl_sw2_ble_uuid(EGL_SW2_UUID_COMMAND_RESPONSE, &uuid_len);
    REQUIRE(std::string_view{uuid_ptr, uuid_len} == CommandResponseUuid);
#endif
}
