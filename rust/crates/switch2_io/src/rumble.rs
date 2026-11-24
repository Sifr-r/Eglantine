// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use crate::types::ProductId;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct VibrationPacketData {
    pub lf_freq: u16,
    pub lf_amp: u16,
    pub hf_freq: u16,
    pub hf_amp: u16,
}

impl Default for VibrationPacketData {
    #[inline]
    fn default() -> Self {
        Self {
            lf_freq: 0x0E1,
            lf_amp: 0x000,
            hf_freq: 0x1E1,
            hf_amp: 0x000,
        }
    }
}

impl VibrationPacketData {
    #[inline]
    pub const fn encode_5_bytes(&self) -> [u8; 5] {
        [
            (self.hf_freq & 0xFF) as u8,
            ((self.hf_freq >> 8) | (self.hf_amp << 2)) as u8,
            ((self.hf_amp >> 6) | (self.lf_freq << 4)) as u8,
            ((self.lf_freq >> 4) | (self.lf_amp << 6)) as u8,
            (self.lf_amp >> 2) as u8,
        ]
    }

    pub fn from_vibration_status(
        low_amplitude: f32,
        low_frequency: f32,
        high_amplitude: f32,
        high_frequency: f32,
    ) -> Self {
        let mut data = Self::default();
        let clamped_lf = low_amplitude.clamp(0.0, 1.0);
        let clamped_hf = high_amplitude.clamp(0.0, 1.0);
        data.lf_amp = (clamped_lf * 800.0).round() as u16;
        data.hf_amp = (clamped_hf * 800.0).round() as u16;

        if low_frequency > 0.0 {
            let encoded_lf = low_frequency.clamp(40.0, 320.0);
            data.lf_freq = (encoded_lf * 1.4).round().clamp(1.0, 0x1FF as f32) as u16;
        }
        if high_frequency > 0.0 {
            let encoded_hf = high_frequency.clamp(80.0, 640.0);
            data.hf_freq = (encoded_hf * 1.5).round().clamp(1.0, 0x1FF as f32) as u16;
        }
        data
    }
}

pub fn build_vibration_report(
    packet_id: u8,
    is_pro_controller: bool,
    motor1: &VibrationPacketData,
    motor2: &VibrationPacketData,
    motor3: &VibrationPacketData,
) -> Vec<u8> {
    let mut motor_block = [0u8; 16];
    motor_block[0] = 0x50u8.wrapping_add(packet_id & 0x0F);
    motor_block[1..6].copy_from_slice(&motor1.encode_5_bytes());
    motor_block[6..11].copy_from_slice(&motor2.encode_5_bytes());
    motor_block[11..16].copy_from_slice(&motor3.encode_5_bytes());

    let mut report = Vec::with_capacity(if is_pro_controller { 33 } else { 17 });
    report.push(0x00);
    report.extend_from_slice(&motor_block);
    if is_pro_controller {
        report.extend_from_slice(&motor_block);
    }
    report
}

pub fn build_usb_vibration_report(
    pid: ProductId,
    sequence: u8,
    motor: &VibrationPacketData,
) -> Vec<u8> {
    let frame = motor.encode_5_bytes();
    let sequence_byte = 0x50u8 | (sequence & 0x0F);

    match pid {
        ProductId::ProController2 => {
            let mut report = vec![0u8; 64];
            report[0] = 0x02;
            report[1] = sequence_byte;
            report[2..7].copy_from_slice(&frame);
            report[0x11] = sequence_byte;
            report[0x12..0x17].copy_from_slice(&frame);
            report
        }
        ProductId::NsoGameCube => {
            let mut report = vec![0u8; 7];
            report[0] = 0x03;
            report[1] = sequence_byte;
            report[2] = if motor.lf_amp != 0 || motor.hf_amp != 0 {
                0x01
            } else {
                0x02
            };
            report
        }
        ProductId::JoyCon2Left | ProductId::JoyCon2Right | ProductId::None => {
            let mut report = vec![0u8; 7];
            report[0] = 0x01;
            report[1] = sequence_byte;
            report[2..7].copy_from_slice(&frame);
            report
        }
    }
}
