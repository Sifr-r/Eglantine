// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use crate::types::{
    is_supported_product_id, signed_looping_difference16, BleAdvertisementInfo, ControllerInfo,
    ControllerType, GyroActivationMode, GyroMouseMode, GyroMouseOutput, MotionOutput,
    OpticalMouseOutput, PadButton, ProductId, StickCalibration, Switch2InputReport,
    FACTORY_CALIBRATION_SIZE, MINIMUM_INPUT_REPORT_SIZE, NINTENDO_VENDOR_ID,
    USER_CALIBRATION_BLOCK_SIZE, USER_CALIBRATION_MAGIC,
};

#[inline]
const fn decode_u16_le(data: &[u8], offset: usize) -> u16 {
    (data[offset] as u16) | ((data[offset + 1] as u16) << 8)
}

#[inline]
const fn decode_s16_le(data: &[u8], offset: usize) -> i16 {
    decode_u16_le(data, offset) as i16
}

#[inline]
const fn decode_u24_le(data: &[u8], offset: usize) -> u32 {
    (data[offset] as u32) | ((data[offset + 1] as u32) << 8) | ((data[offset + 2] as u32) << 16)
}

#[inline]
const fn decode_u32_le(data: &[u8], offset: usize) -> u32 {
    (data[offset] as u32)
        | ((data[offset + 1] as u32) << 8)
        | ((data[offset + 2] as u32) << 16)
        | ((data[offset + 3] as u32) << 24)
}

#[inline]
const fn decode_rgb24(data: &[u8], offset: usize) -> u32 {
    ((data[offset] as u32) << 16) | ((data[offset + 1] as u32) << 8) | (data[offset + 2] as u32)
}

#[inline]
fn apply_soft_deadzone(value: f32, deadzone: f32) -> f32 {
    if value.abs() < deadzone {
        0.0
    } else if value > 0.0 {
        value - deadzone
    } else {
        value + deadzone
    }
}

#[inline]
pub fn strip_hid_report_id(usb_report: &[u8]) -> Option<&[u8]> {
    if usb_report.len() < MINIMUM_INPUT_REPORT_SIZE + 1 {
        return None;
    }
    Some(&usb_report[1..])
}

pub fn parse_ble_manufacturer_data(data: &[u8]) -> Option<BleAdvertisementInfo> {
    if data.len() < 16 {
        return None;
    }

    let vendor_id = decode_u16_le(data, 3);
    let raw_pid = decode_u16_le(data, 5);
    if vendor_id != NINTENDO_VENDOR_ID || !is_supported_product_id(raw_pid) {
        return None;
    }
    let product_id = ProductId::from_raw(raw_pid)?;

    let mut reconnect_mac = 0u64;
    for (i, &byte) in data[10..16].iter().enumerate() {
        reconnect_mac |= (byte as u64) << (i * 8);
    }

    Some(BleAdvertisementInfo {
        vendor_id,
        product_id,
        reconnect_mac,
        is_pairing_mode: reconnect_mac == 0,
    })
}

#[inline]
pub fn unpack_stick_xy(three_bytes: &[u8]) -> (u16, u16) {
    if three_bytes.len() < 3 {
        return (2048, 2048);
    }
    let packed = decode_u24_le(three_bytes, 0);
    (
        (packed & 0xFFF) as u16,
        ((packed >> 12) & 0xFFF) as u16,
    )
}

pub fn parse_stick_calibration(data: &[u8]) -> StickCalibration {
    let mut cal = StickCalibration::default();
    if data.len() < 9 {
        return cal;
    }

    let (cx, cy) = unpack_stick_xy(&data[0..3]);
    let (maxx, maxy) = unpack_stick_xy(&data[3..6]);
    let (minx, miny) = unpack_stick_xy(&data[6..9]);

    if cx == 0 && cy == 0 && maxx == 0 && maxy == 0 {
        return cal;
    }

    cal.center_x = cx;
    cal.center_y = cy;
    cal.max_x = if maxx > 0 { maxx } else { 1500 };
    cal.max_y = if maxy > 0 { maxy } else { 1500 };
    cal.min_x = if minx > 0 { minx } else { 1500 };
    cal.min_y = if miny > 0 { miny } else { 1500 };
    cal
}

pub fn parse_user_stick_calibration(block: &[u8]) -> Option<StickCalibration> {
    if block.len() < USER_CALIBRATION_BLOCK_SIZE as usize {
        return None;
    }
    if decode_u16_le(block, 0) != USER_CALIBRATION_MAGIC {
        return None;
    }
    Some(parse_stick_calibration(
        &block[2..2 + FACTORY_CALIBRATION_SIZE as usize],
    ))
}

pub fn apply_stick_calibration_axis(
    raw_value: u16,
    center: u16,
    max_offset: u16,
    min_offset: u16,
    deadzone: u16,
) -> f32 {
    let signed_val = (raw_value as i32) - (center as i32);
    let dz = deadzone as i32;
    if signed_val > dz {
        if max_offset == 0 {
            return 1.0;
        }
        ((signed_val as f32) / (max_offset as f32)).min(1.0)
    } else if signed_val < -dz {
        if min_offset == 0 {
            return -1.0;
        }
        -((-signed_val as f32) / (min_offset as f32)).min(1.0)
    } else {
        0.0
    }
}

pub fn parse_controller_info(data: &[u8]) -> Option<ControllerInfo> {
    if data.len() < 37 {
        return None;
    }

    let serial_bytes = &data[2..16];
    let serial_len = serial_bytes
        .iter()
        .position(|&b| b == 0)
        .unwrap_or(14);
    let serial_number = String::from_utf8_lossy(&serial_bytes[..serial_len]).into_owned();
    let vendor_id = decode_u16_le(data, 18);
    let raw_pid = decode_u16_le(data, 20);
    let product_id = ProductId::from_raw(raw_pid).unwrap_or(ProductId::None);

    Some(ControllerInfo {
        serial_number,
        vendor_id,
        product_id,
        body_color: decode_rgb24(data, 25),
        buttons_color: decode_rgb24(data, 28),
        left_grip_color: decode_rgb24(data, 31),
        right_grip_color: decode_rgb24(data, 34),
    })
}

pub fn parse_input_report(
    data: &[u8],
    left_cal: &StickCalibration,
    right_cal: &StickCalibration,
    deadzone: u16,
) -> Option<Switch2InputReport> {
    if data.len() < MINIMUM_INPUT_REPORT_SIZE {
        return None;
    }

    let timestamp = decode_u32_le(data, 0);
    let buttons = decode_u32_le(data, 4);

    let (raw_left_x, raw_left_y) = unpack_stick_xy(&data[10..13]);
    let (raw_right_x, raw_right_y) = unpack_stick_xy(&data[13..16]);

    let left_x = apply_stick_calibration_axis(
        raw_left_x,
        left_cal.center_x,
        left_cal.max_x,
        left_cal.min_x,
        deadzone,
    );
    let left_y = apply_stick_calibration_axis(
        raw_left_y,
        left_cal.center_y,
        left_cal.max_y,
        left_cal.min_y,
        deadzone,
    );
    let right_x = apply_stick_calibration_axis(
        raw_right_x,
        right_cal.center_x,
        right_cal.max_x,
        right_cal.min_x,
        deadzone,
    );
    let right_y = apply_stick_calibration_axis(
        raw_right_y,
        right_cal.center_y,
        right_cal.max_y,
        right_cal.min_y,
        deadzone,
    );

    Some(Switch2InputReport {
        timestamp,
        buttons,
        raw_left_x,
        raw_left_y,
        raw_right_x,
        raw_right_y,
        left_x,
        left_y,
        right_x,
        right_y,
        mouse_x: decode_u16_le(data, 16),
        mouse_y: decode_u16_le(data, 18),
        mouse_roughness: decode_u16_le(data, 20),
        mouse_distance: decode_u16_le(data, 22),
        magnetometer: [
            decode_s16_le(data, 25),
            decode_s16_le(data, 27),
            decode_s16_le(data, 29),
        ],
        battery_voltage: (decode_u16_le(data, 31) as f32) / 1000.0,
        battery_current: (decode_u16_le(data, 34) as f32) / 100.0,
        temperature: 25.0 + (decode_u16_le(data, 46) as f32) / 127.0,
        accelerometer: [
            decode_s16_le(data, 48),
            decode_s16_le(data, 50),
            decode_s16_le(data, 52),
        ],
        gyroscope: [
            decode_s16_le(data, 54),
            decode_s16_le(data, 56),
            decode_s16_le(data, 58),
        ],
    })
}

pub fn convert_imu_to_motion(
    report: &Switch2InputReport,
    gyro_bias: [f32; 3],
    delta_timestamp_us: u64,
) -> MotionOutput {
    const ACCEL_SCALE: f32 = 1.0 / 4096.0;
    const GYRO_RAD_PER_COUNT: f32 = 40.0 / 32767.0;
    const GYRO_SCALE: f32 = GYRO_RAD_PER_COUNT / (2.0 * std::f32::consts::PI);

    let gx = (report.gyroscope[0] as f32) - gyro_bias[0];
    let gy = (report.gyroscope[1] as f32) - gyro_bias[1];
    let gz = (report.gyroscope[2] as f32) - gyro_bias[2];

    MotionOutput {
        gyro_x: -gx * GYRO_SCALE,
        gyro_y: gy * GYRO_SCALE,
        gyro_z: gz * GYRO_SCALE,
        accel_x: (report.accelerometer[0] as f32) * ACCEL_SCALE,
        accel_y: -(report.accelerometer[1] as f32) * ACCEL_SCALE,
        accel_z: -(report.accelerometer[2] as f32) * ACCEL_SCALE,
        delta_timestamp: if delta_timestamp_us > 0 {
            delta_timestamp_us
        } else {
            4000
        },
    }
}

pub fn build_command_packet(
    command_id: u8,
    subcommand_id: u8,
    payload: &[u8],
    is_ble: bool,
) -> Vec<u8> {
    let mut packet = Vec::with_capacity(8 + payload.len());
    packet.push(command_id);
    packet.push(0x91);
    packet.push(if is_ble { 0x01 } else { 0x00 });
    packet.push(subcommand_id);
    packet.push(0x00);
    packet.push(payload.len() as u8);
    packet.push(0x00);
    packet.push(0x00);
    packet.extend_from_slice(payload);
    packet
}

#[inline]
pub const fn build_memory_read_payload(length: u8, address: u32) -> [u8; 8] {
    [
        length,
        0x7E,
        0x00,
        0x00,
        (address & 0xFF) as u8,
        ((address >> 8) & 0xFF) as u8,
        ((address >> 16) & 0xFF) as u8,
        ((address >> 24) & 0xFF) as u8,
    ]
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct GyroMouseProcessor {
    pub is_calibrating: bool,
    pub calibration_end_ns: u64,
    pub calibration_samples: Vec<[i16; 3]>,
    pub gyro_bias: [f32; 3],
    pub gyro_mouse_enabled: bool,
    pub prev_trigger: bool,
}

impl GyroMouseProcessor {
    pub fn start_calibration(&mut self, now_ns: u64) {
        self.is_calibrating = true;
        self.calibration_end_ns = now_ns.saturating_add(2_000_000_000);
        self.calibration_samples.clear();
    }

    pub fn reset(&mut self) {
        self.is_calibrating = false;
        self.calibration_samples.clear();
        self.gyro_mouse_enabled = false;
        self.prev_trigger = false;
    }

    #[allow(clippy::too_many_arguments)]
    pub fn process(
        &mut self,
        report: &Switch2InputReport,
        controller_type: ControllerType,
        horizontal_hold: bool,
        trigger_pressed: bool,
        mode: GyroMouseMode,
        activation_mode: GyroActivationMode,
        sensitivity: f32,
        now_ns: u64,
    ) -> GyroMouseOutput {
        if self.is_calibrating {
            if now_ns < self.calibration_end_ns {
                self.calibration_samples.push(report.gyroscope);
                return GyroMouseOutput::default();
            }
            self.is_calibrating = false;
            if !self.calibration_samples.is_empty() {
                let mut sx = 0.0f64;
                let mut sy = 0.0f64;
                let mut sz = 0.0f64;
                for sample in &self.calibration_samples {
                    sx += sample[0] as f64;
                    sy += sample[1] as f64;
                    sz += sample[2] as f64;
                }
                let count = self.calibration_samples.len() as f64;
                self.gyro_bias = [(sx / count) as f32, (sy / count) as f32, (sz / count) as f32];
            }
            self.calibration_samples.clear();
        }

        self.process_post_calibration(
            report,
            controller_type,
            horizontal_hold,
            trigger_pressed,
            mode,
            activation_mode,
            sensitivity,
        )
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn process_post_calibration(
        &mut self,
        report: &Switch2InputReport,
        controller_type: ControllerType,
        horizontal_hold: bool,
        trigger_pressed: bool,
        mode: GyroMouseMode,
        activation_mode: GyroActivationMode,
        sensitivity: f32,
    ) -> GyroMouseOutput {
        let mut output = GyroMouseOutput::default();

        if mode == GyroMouseMode::Off {
            self.gyro_mouse_enabled = false;
            self.prev_trigger = trigger_pressed;
            return output;
        }

        if activation_mode == GyroActivationMode::Hold {
            self.gyro_mouse_enabled = trigger_pressed;
        } else if trigger_pressed && !self.prev_trigger {
            self.gyro_mouse_enabled = !self.gyro_mouse_enabled;
        }
        self.prev_trigger = trigger_pressed;

        if !self.gyro_mouse_enabled {
            return output;
        }

        output.active = true;

        if mode == GyroMouseMode::Roll {
            let tilt_normalized = (report.accelerometer[0] as f32) / 4000.0;
            output.roll_steer_x = Some((tilt_normalized * (sensitivity * -2.0)).clamp(-1.0, 1.0));
            return output;
        }

        output.left_click = (report.buttons & (PadButton::ZR as u32)) != 0;
        output.right_click = (report.buttons & (PadButton::ZL as u32)) != 0;

        const BIAS_THRESHOLD: f32 = 5.0;
        let bx = if self.gyro_bias[0].abs() > BIAS_THRESHOLD {
            self.gyro_bias[0]
        } else {
            0.0
        };
        let by = if self.gyro_bias[1].abs() > BIAS_THRESHOLD {
            self.gyro_bias[1]
        } else {
            0.0
        };
        let bz = if self.gyro_bias[2].abs() > BIAS_THRESHOLD {
            self.gyro_bias[2]
        } else {
            0.0
        };

        const SOFT_DEADZONE: f32 = 8.0;
        let gx = apply_soft_deadzone((report.gyroscope[0] as f32) - bx, SOFT_DEADZONE);
        let gy = apply_soft_deadzone((report.gyroscope[1] as f32) - by, SOFT_DEADZONE);
        let gz = apply_soft_deadzone((report.gyroscope[2] as f32) - bz, SOFT_DEADZONE);

        const GYRO_DEADZONE: f32 = 0.2;
        if gx.abs() > GYRO_DEADZONE || gy.abs() > GYRO_DEADZONE || gz.abs() > GYRO_DEADZONE {
            let eff_h = apply_soft_deadzone(-gz, GYRO_DEADZONE);
            let raw_v = if controller_type == ControllerType::Pro || !horizontal_hold {
                gx
            } else if controller_type == ControllerType::Left {
                -gy
            } else {
                gy
            };
            let eff_v = apply_soft_deadzone(raw_v, GYRO_DEADZONE);

            const ACCEL_FACTOR: f32 = 0.002;
            output.velocity_x = eff_h * sensitivity * ACCEL_FACTOR;
            output.velocity_y = eff_v * -sensitivity * ACCEL_FACTOR;
        }

        output
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct OpticalMouseProcessor {
    pub has_previous: bool,
    pub prev_x: u16,
    pub prev_y: u16,
}

impl OpticalMouseProcessor {
    pub fn reset(&mut self) {
        self.has_previous = false;
        self.prev_x = 0;
        self.prev_y = 0;
    }

    pub fn process(
        &mut self,
        report: &Switch2InputReport,
        controller_type: ControllerType,
        enabled: bool,
    ) -> OpticalMouseOutput {
        let mut output = OpticalMouseOutput::default();
        let is_joycon =
            controller_type == ControllerType::Left || controller_type == ControllerType::Right;
        if !enabled || !is_joycon {
            self.reset();
            return output;
        }

        if report.mouse_distance == 0
            || report.mouse_distance >= 1000
            || report.mouse_roughness >= 4000
        {
            self.reset();
            return output;
        }

        output.active = true;
        if controller_type == ControllerType::Left {
            output.left_button = (report.buttons & (PadButton::L as u32)) != 0;
            output.middle_button = (report.buttons & (PadButton::StickL as u32)) != 0;
            output.right_button = (report.buttons & (PadButton::ZL as u32)) != 0;
            output.scroll_y = if report.left_y.abs() > 0.2 {
                report.left_y
            } else {
                0.0
            };
        } else {
            output.left_button = (report.buttons & (PadButton::R as u32)) != 0;
            output.middle_button = (report.buttons & (PadButton::StickR as u32)) != 0;
            output.right_button = (report.buttons & (PadButton::ZR as u32)) != 0;
            output.scroll_y = if report.right_y.abs() > 0.2 {
                report.right_y
            } else {
                0.0
            };
        }

        if self.has_previous {
            output.delta_x = signed_looping_difference16(self.prev_x, report.mouse_x);
            output.delta_y = signed_looping_difference16(self.prev_y, report.mouse_y);
        }
        self.prev_x = report.mouse_x;
        self.prev_y = report.mouse_y;
        self.has_previous = true;
        output
    }
}
