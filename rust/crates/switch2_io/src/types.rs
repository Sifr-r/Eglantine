// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![forbid(unsafe_code)]

use std::ffi::CStr;

pub const NINTENDO_VENDOR_ID: u16 = 0x057E;
pub const NINTENDO_BLE_MANUFACTURER_ID: u16 = 0x0553;

pub const DEVICE_ID_KEY: &str = "device_id";

pub mod ble_uuids {
    pub const INPUT_REPORT: &str = "ab7de9be-89fe-49ad-828f-118f09df7fd2";
    pub const VIBRATION_JOYCON_R: &str = "fa19b0fb-cd1f-46a7-84a1-bbb09e00c149";
    pub const VIBRATION_JOYCON_L: &str = "289326cb-a471-485d-a8f4-240c14f18241";
    pub const VIBRATION_PRO_CONTROLLER: &str = "cc483f51-9258-427d-a939-630c31f72b05";
    pub const COMMAND_WRITE: &str = "649d4ac9-8eb7-4e6c-af44-1ea54fe5f005";
    pub const COMMAND_RESPONSE: &str = "c765a961-d9d8-4d36-a20a-5315b111836a";
}

pub mod command_id {
    pub const MEMORY: u8 = 0x02;
    pub const USB_INIT: u8 = 0x03;
    pub const LEDS: u8 = 0x09;
    pub const VIBRATION: u8 = 0x0A;
    pub const FEATURE: u8 = 0x0C;
    pub const PAIR: u8 = 0x15;
}

pub mod sub_command_id {
    pub const LEDS_SET_PLAYER: u8 = 0x07;
    pub const VIBRATION_PLAY_PRESET: u8 = 0x02;
    pub const MEMORY_READ: u8 = 0x04;
    pub const PAIR_SET_MAC: u8 = 0x01;
    pub const PAIR_LTK2: u8 = 0x02;
    pub const PAIR_FINISH: u8 = 0x03;
    pub const PAIR_LTK1: u8 = 0x04;
    pub const FEATURE_INIT: u8 = 0x02;
    pub const FEATURE_ENABLE: u8 = 0x04;
    pub const SELECT_INPUT_REPORT: u8 = 0x0A;
}

pub mod feature_flags {
    pub const BUTTONS: u8 = 0x01;
    pub const ANALOG_STICKS: u8 = 0x02;
    pub const MOTION: u8 = 0x04;
    pub const MOUSE: u8 = 0x10;
    pub const RUMBLE: u8 = 0x20;
    pub const MAGNETOMETER: u8 = 0x80;

    pub const STANDARD: u8 = BUTTONS | ANALOG_STICKS | MOTION | RUMBLE;
    pub const JOYCON: u8 = STANDARD | MOUSE;
    pub const STARTUP: u8 = MOTION | MOUSE | MAGNETOMETER;
}

pub mod memory_address {
    pub const CONTROLLER_INFO: u32 = 0x0001_3000;
    pub const FACTORY_JOYSTICK1: u32 = 0x01_30A8;
    pub const FACTORY_JOYSTICK2: u32 = 0x01_30E8;
    pub const USER_JOYSTICK1: u32 = 0x1F_C040;
    pub const USER_JOYSTICK2: u32 = 0x1F_C080;
}

pub const USER_CALIBRATION_MAGIC: u16 = 0xA1B2;
pub const USER_CALIBRATION_BLOCK_SIZE: u8 = 11;
pub const FACTORY_CALIBRATION_SIZE: u8 = 9;

pub const LED_PATTERNS: [u8; 8] = [0x01, 0x03, 0x07, 0x0F, 0x09, 0x05, 0x0D, 0x06];

pub const DEFAULT_STICK_DEADZONE: u16 = 50;
pub const MINIMUM_INPUT_REPORT_SIZE: usize = 60;
pub const NATIVE_INPUT_REPORT_SIZE: usize = 63;
pub const COMMON_REPORT_ID: u8 = 0x05;

#[repr(u16)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ProductId {
    #[default]
    None = 0x0000,
    JoyCon2Right = 0x2066,
    JoyCon2Left = 0x2067,
    ProController2 = 0x2069,
    NsoGameCube = 0x2073,
}

impl ProductId {
    #[inline]
    pub const fn from_raw(raw: u16) -> Option<Self> {
        match raw {
            0x0000 => Some(Self::None),
            0x2066 => Some(Self::JoyCon2Right),
            0x2067 => Some(Self::JoyCon2Left),
            0x2069 => Some(Self::ProController2),
            0x2073 => Some(Self::NsoGameCube),
            _ => None,
        }
    }

    #[inline]
    pub const fn as_u16(self) -> u16 {
        self as u16
    }
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ControllerType {
    #[default]
    None = 0x00,
    Left = 0x01,
    Right = 0x02,
    Pro = 0x03,
    Dual = 0x05,
    GameCube = 0x08,
}

impl ControllerType {
    #[inline]
    pub const fn from_raw(raw: u8) -> Option<Self> {
        match raw {
            0x00 => Some(Self::None),
            0x01 => Some(Self::Left),
            0x02 => Some(Self::Right),
            0x03 => Some(Self::Pro),
            0x05 => Some(Self::Dual),
            0x08 => Some(Self::GameCube),
            _ => None,
        }
    }
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum TransportType {
    #[default]
    None = 0,
    Ble = 1,
    Usb = 2,
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum PadButton {
    #[default]
    None = 0,
    Y = 0x0000_0001,
    X = 0x0000_0002,
    B = 0x0000_0004,
    A = 0x0000_0008,
    RightSR = 0x0000_0010,
    RightSL = 0x0000_0020,
    R = 0x0000_0040,
    ZR = 0x0000_0080,
    Minus = 0x0000_0100,
    Plus = 0x0000_0200,
    StickR = 0x0000_0400,
    StickL = 0x0000_0800,
    Home = 0x0000_1000,
    Capture = 0x0000_2000,
    C = 0x0000_4000,
    Down = 0x0001_0000,
    Up = 0x0002_0000,
    Right = 0x0004_0000,
    Left = 0x0008_0000,
    LeftSR = 0x0010_0000,
    LeftSL = 0x0020_0000,
    L = 0x0040_0000,
    ZL = 0x0080_0000,
    GR = 0x0100_0000,
    GL = 0x0200_0000,
}

#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PadAxes {
    LeftStickX = 0,
    LeftStickY = 1,
    RightStickX = 2,
    RightStickY = 3,
}

#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PadMotion {
    LeftMotion = 0,
    RightMotion = 1,
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum GyroMouseMode {
    #[default]
    Off = 0,
    Yaw = 1,
    Roll = 2,
}

impl GyroMouseMode {
    #[inline]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(Self::Off),
            1 => Some(Self::Yaw),
            2 => Some(Self::Roll),
            _ => None,
        }
    }
}

#[repr(u32)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum GyroActivationMode {
    #[default]
    Hold = 0,
    Toggle = 1,
}

impl GyroActivationMode {
    #[inline]
    pub const fn from_raw(raw: u32) -> Option<Self> {
        match raw {
            0 => Some(Self::Hold),
            1 => Some(Self::Toggle),
            _ => None,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct BleAdvertisementInfo {
    pub vendor_id: u16,
    pub product_id: ProductId,
    pub reconnect_mac: u64,
    pub is_pairing_mode: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StickCalibration {
    pub center_x: u16,
    pub center_y: u16,
    pub max_x: u16,
    pub max_y: u16,
    pub min_x: u16,
    pub min_y: u16,
}

impl Default for StickCalibration {
    #[inline]
    fn default() -> Self {
        Self {
            center_x: 2048,
            center_y: 2048,
            max_x: 1500,
            max_y: 1500,
            min_x: 1500,
            min_y: 1500,
        }
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ControllerInfo {
    pub serial_number: String,
    pub vendor_id: u16,
    pub product_id: ProductId,
    pub body_color: u32,
    pub buttons_color: u32,
    pub left_grip_color: u32,
    pub right_grip_color: u32,
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Switch2InputReport {
    pub timestamp: u32,
    pub buttons: u32,
    pub raw_left_x: u16,
    pub raw_left_y: u16,
    pub raw_right_x: u16,
    pub raw_right_y: u16,
    pub left_x: f32,
    pub left_y: f32,
    pub right_x: f32,
    pub right_y: f32,
    pub mouse_x: u16,
    pub mouse_y: u16,
    pub mouse_roughness: u16,
    pub mouse_distance: u16,
    pub magnetometer: [i16; 3],
    pub battery_voltage: f32,
    pub battery_current: f32,
    pub temperature: f32,
    pub accelerometer: [i16; 3],
    pub gyroscope: [i16; 3],
}

impl Default for Switch2InputReport {
    #[inline]
    fn default() -> Self {
        Self {
            timestamp: 0,
            buttons: 0,
            raw_left_x: 2048,
            raw_left_y: 2048,
            raw_right_x: 2048,
            raw_right_y: 2048,
            left_x: 0.0,
            left_y: 0.0,
            right_x: 0.0,
            right_y: 0.0,
            mouse_x: 0,
            mouse_y: 0,
            mouse_roughness: 0,
            mouse_distance: 0,
            magnetometer: [0; 3],
            battery_voltage: 0.0,
            battery_current: 0.0,
            temperature: 0.0,
            accelerometer: [0; 3],
            gyroscope: [0; 3],
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct MotionOutput {
    pub gyro_x: f32,
    pub gyro_y: f32,
    pub gyro_z: f32,
    pub accel_x: f32,
    pub accel_y: f32,
    pub accel_z: f32,
    pub delta_timestamp: u64,
}

impl Default for MotionOutput {
    #[inline]
    fn default() -> Self {
        Self {
            gyro_x: 0.0,
            gyro_y: 0.0,
            gyro_z: 0.0,
            accel_x: 0.0,
            accel_y: 0.0,
            accel_z: 0.0,
            delta_timestamp: 4000,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct GyroMouseOutput {
    pub active: bool,
    pub velocity_x: f32,
    pub velocity_y: f32,
    pub roll_steer_x: Option<f32>,
    pub left_click: bool,
    pub right_click: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct OpticalMouseOutput {
    pub active: bool,
    pub delta_x: i32,
    pub delta_y: i32,
    pub left_button: bool,
    pub middle_button: bool,
    pub right_button: bool,
    pub scroll_y: f32,
}

#[inline]
pub const fn is_supported_product_id(raw_pid: u16) -> bool {
    matches!(
        raw_pid,
        0x2066 | 0x2067 | 0x2069 | 0x2073
    )
}

#[inline]
pub const fn product_id_to_controller_type(pid: ProductId) -> ControllerType {
    match pid {
        ProductId::JoyCon2Left => ControllerType::Left,
        ProductId::JoyCon2Right => ControllerType::Right,
        ProductId::ProController2 => ControllerType::Pro,
        ProductId::NsoGameCube => ControllerType::GameCube,
        ProductId::None => ControllerType::None,
    }
}

#[inline]
pub const fn product_name(pid: ProductId) -> &'static str {
    match std::str::from_utf8(product_name_c_str(pid).to_bytes()) {
        Ok(name) => name,
        Err(_) => panic!("controller product names must be UTF-8"),
    }
}

pub(crate) const fn product_name_c_str(pid: ProductId) -> &'static CStr {
    match pid {
        ProductId::JoyCon2Right => c"Switch 2 Joy-Con (Right)",
        ProductId::JoyCon2Left => c"Switch 2 Joy-Con (Left)",
        ProductId::ProController2 => c"Switch 2 Pro Controller",
        ProductId::NsoGameCube => c"NSO GameCube Controller",
        ProductId::None => c"Unknown Switch 2 Controller",
    }
}

#[inline]
pub const fn feature_mask_for_product(pid: ProductId) -> u8 {
    match pid {
        ProductId::JoyCon2Left | ProductId::JoyCon2Right => feature_flags::JOYCON,
        ProductId::None | ProductId::ProController2 | ProductId::NsoGameCube => {
            feature_flags::STANDARD
        }
    }
}

#[inline]
pub const fn reverse_lower_bits4(mut value: u8) -> u8 {
    let mut result = 0u8;
    let mut i = 0;
    while i < 4 {
        result = (result << 1) | (value & 1);
        value >>= 1;
        i += 1;
    }
    result
}

#[inline]
pub const fn signed_looping_difference16(previous: u16, current: u16) -> i32 {
    let mut diff = ((current as i32) - (previous as i32)) & 0xFFFF;
    if diff > 32768 {
        diff -= 65536;
    }
    diff
}
