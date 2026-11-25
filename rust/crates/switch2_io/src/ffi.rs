// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

use std::ffi::c_char;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;

use crate::protocol::{
    apply_stick_calibration_axis, build_command_packet, build_memory_read_payload,
    convert_imu_to_motion, parse_ble_manufacturer_data, parse_controller_info, parse_input_report,
    parse_stick_calibration, parse_user_stick_calibration, strip_hid_report_id, unpack_stick_xy,
    GyroMouseProcessor, OpticalMouseProcessor,
};
use crate::rumble::{build_usb_vibration_report, build_vibration_report, VibrationPacketData};
use crate::types::{
    ble_uuids, feature_mask_for_product, is_supported_product_id, product_id_to_controller_type,
    product_name_c_str, reverse_lower_bits4, signed_looping_difference16, ControllerType,
    GyroActivationMode, GyroMouseMode, ProductId, StickCalibration, Switch2InputReport,
};

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct EglSw2BleAdvertisementInfo {
    pub vendor_id: u16,
    pub product_id: u16,
    pub _pad: u32,
    pub reconnect_mac: u64,
    pub is_pairing_mode: u8,
    pub _pad2: [u8; 7],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EglSw2StickCalibration {
    pub center_x: u16,
    pub center_y: u16,
    pub max_x: u16,
    pub max_y: u16,
    pub min_x: u16,
    pub min_y: u16,
}

impl Default for EglSw2StickCalibration {
    fn default() -> Self {
        let d = StickCalibration::default();
        Self::from(d)
    }
}

impl From<StickCalibration> for EglSw2StickCalibration {
    fn from(cal: StickCalibration) -> Self {
        Self {
            center_x: cal.center_x,
            center_y: cal.center_y,
            max_x: cal.max_x,
            max_y: cal.max_y,
            min_x: cal.min_x,
            min_y: cal.min_y,
        }
    }
}

impl From<EglSw2StickCalibration> for StickCalibration {
    fn from(cal: EglSw2StickCalibration) -> Self {
        Self {
            center_x: cal.center_x,
            center_y: cal.center_y,
            max_x: cal.max_x,
            max_y: cal.max_y,
            min_x: cal.min_x,
            min_y: cal.min_y,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct EglSw2ControllerInfo {
    pub serial_number: [c_char; 16],
    pub serial_len: u8,
    pub _pad: u8,
    pub vendor_id: u16,
    pub product_id: u16,
    pub _pad2: u16,
    pub body_color: u32,
    pub buttons_color: u32,
    pub left_grip_color: u32,
    pub right_grip_color: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EglSw2VibrationPacketData {
    pub lf_freq: u16,
    pub lf_amp: u16,
    pub hf_freq: u16,
    pub hf_amp: u16,
}

impl Default for EglSw2VibrationPacketData {
    fn default() -> Self {
        let d = VibrationPacketData::default();
        Self {
            lf_freq: d.lf_freq,
            lf_amp: d.lf_amp,
            hf_freq: d.hf_freq,
            hf_amp: d.hf_amp,
        }
    }
}

impl From<EglSw2VibrationPacketData> for VibrationPacketData {
    fn from(v: EglSw2VibrationPacketData) -> Self {
        Self {
            lf_freq: v.lf_freq,
            lf_amp: v.lf_amp,
            hf_freq: v.hf_freq,
            hf_amp: v.hf_amp,
        }
    }
}

impl From<VibrationPacketData> for EglSw2VibrationPacketData {
    fn from(v: VibrationPacketData) -> Self {
        Self {
            lf_freq: v.lf_freq,
            lf_amp: v.lf_amp,
            hf_freq: v.hf_freq,
            hf_amp: v.hf_amp,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct EglSw2InputReport {
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
    pub _pad_mag: i16,
    pub battery_voltage: f32,
    pub battery_current: f32,
    pub temperature: f32,
    pub accelerometer: [i16; 3],
    pub gyroscope: [i16; 3],
}

impl From<Switch2InputReport> for EglSw2InputReport {
    fn from(r: Switch2InputReport) -> Self {
        Self {
            timestamp: r.timestamp,
            buttons: r.buttons,
            raw_left_x: r.raw_left_x,
            raw_left_y: r.raw_left_y,
            raw_right_x: r.raw_right_x,
            raw_right_y: r.raw_right_y,
            left_x: r.left_x,
            left_y: r.left_y,
            right_x: r.right_x,
            right_y: r.right_y,
            mouse_x: r.mouse_x,
            mouse_y: r.mouse_y,
            mouse_roughness: r.mouse_roughness,
            mouse_distance: r.mouse_distance,
            magnetometer: r.magnetometer,
            _pad_mag: 0,
            battery_voltage: r.battery_voltage,
            battery_current: r.battery_current,
            temperature: r.temperature,
            accelerometer: r.accelerometer,
            gyroscope: r.gyroscope,
        }
    }
}

impl From<EglSw2InputReport> for Switch2InputReport {
    fn from(r: EglSw2InputReport) -> Self {
        Self {
            timestamp: r.timestamp,
            buttons: r.buttons,
            raw_left_x: r.raw_left_x,
            raw_left_y: r.raw_left_y,
            raw_right_x: r.raw_right_x,
            raw_right_y: r.raw_right_y,
            left_x: r.left_x,
            left_y: r.left_y,
            right_x: r.right_x,
            right_y: r.right_y,
            mouse_x: r.mouse_x,
            mouse_y: r.mouse_y,
            mouse_roughness: r.mouse_roughness,
            mouse_distance: r.mouse_distance,
            magnetometer: r.magnetometer,
            battery_voltage: r.battery_voltage,
            battery_current: r.battery_current,
            temperature: r.temperature,
            accelerometer: r.accelerometer,
            gyroscope: r.gyroscope,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct EglSw2MotionOutput {
    pub gyro_x: f32,
    pub gyro_y: f32,
    pub gyro_z: f32,
    pub accel_x: f32,
    pub accel_y: f32,
    pub accel_z: f32,
    pub delta_timestamp: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct EglSw2GyroMouseState {
    pub calibration_end_ns: u64,
    pub gyro_bias: [f32; 3],
    pub is_calibrating: u8,
    pub gyro_mouse_enabled: u8,
    pub prev_trigger: u8,
    pub _pad: u8,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct EglSw2GyroMouseOutput {
    pub active: u8,
    pub has_roll_steer_x: u8,
    pub left_click: u8,
    pub right_click: u8,
    pub velocity_x: f32,
    pub velocity_y: f32,
    pub roll_steer_x: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct EglSw2OpticalMouseState {
    pub has_previous: u8,
    pub _pad: u8,
    pub prev_x: u16,
    pub prev_y: u16,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct EglSw2OpticalMouseOutput {
    pub active: u8,
    pub left_button: u8,
    pub middle_button: u8,
    pub right_button: u8,
    pub delta_x: i32,
    pub delta_y: i32,
    pub scroll_y: f32,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct EglSw2Bytes {
    pub data: *mut u8,
    pub len: usize,
}

impl Default for EglSw2Bytes {
    fn default() -> Self {
        Self {
            data: ptr::null_mut(),
            len: 0,
        }
    }
}

unsafe fn slice_from_raw<'a, T>(ptr: *const T, count: usize) -> Option<&'a [T]> {
    if count == 0 {
        return Some(&[]);
    }
    if ptr.is_null() || !ptr.is_aligned() {
        return None;
    }
    let elem_size = std::mem::size_of::<T>();
    if elem_size > 0 && count > (isize::MAX as usize) / elem_size {
        return None;
    }
    Some(std::slice::from_raw_parts(ptr, count))
}

fn vec_into_raw_parts(vec: Vec<u8>) -> (*mut u8, usize) {
    if vec.is_empty() {
        (ptr::null_mut(), 0)
    } else {
        let boxed = vec.into_boxed_slice();
        let len = boxed.len();
        (Box::into_raw(boxed) as *mut u8, len)
    }
}

unsafe fn write_bytes(out_bytes: *mut EglSw2Bytes, bytes: Vec<u8>) {
    let (data, len) = vec_into_raw_parts(bytes);
    ptr::write(out_bytes, EglSw2Bytes { data, len });
}

fn static_c_str(s_nul: &'static [u8], out_len: *mut usize) -> *const c_char {
    if !out_len.is_null() && out_len.is_aligned() {
        unsafe {
            ptr::write(out_len, s_nul.len().saturating_sub(1));
        }
    }
    s_nul.as_ptr() as *const c_char
}

#[no_mangle]
pub extern "C" fn egl_sw2_is_supported_product_id(raw_pid: u16) -> u8 {
    u8::from(is_supported_product_id(raw_pid))
}

#[no_mangle]
pub extern "C" fn egl_sw2_product_id_to_controller_type(pid: u16) -> u8 {
    let parsed = ProductId::from_raw(pid).unwrap_or(ProductId::None);
    product_id_to_controller_type(parsed) as u8
}

/// Returns the NUL-terminated product name for `pid`, optionally writing its byte length.
///
/// # Safety
///
/// `out_len` must be null or point to a valid, aligned, writable `usize`.
#[no_mangle]
pub unsafe extern "C" fn egl_sw2_product_name(pid: u16, out_len: *mut usize) -> *const c_char {
    let parsed = ProductId::from_raw(pid).unwrap_or(ProductId::None);
    static_c_str(product_name_c_str(parsed).to_bytes_with_nul(), out_len)
}

#[no_mangle]
pub extern "C" fn egl_sw2_feature_mask_for_product(pid: u16) -> u8 {
    let parsed = ProductId::from_raw(pid).unwrap_or(ProductId::None);
    feature_mask_for_product(parsed)
}

/// Returns the NUL-terminated Switch 2 BLE GATT UUID string for `kind`.
///
/// # Safety
///
/// `out_len` must be null or point to a valid, aligned, writable `usize`.
#[no_mangle]
pub unsafe extern "C" fn egl_sw2_ble_uuid(kind: u32, out_len: *mut usize) -> *const c_char {
    let _ = (
        ble_uuids::INPUT_REPORT,
        ble_uuids::VIBRATION_JOYCON_R,
        ble_uuids::VIBRATION_JOYCON_L,
        ble_uuids::VIBRATION_PRO_CONTROLLER,
        ble_uuids::COMMAND_WRITE,
        ble_uuids::COMMAND_RESPONSE,
    );
    match kind {
        0 => static_c_str(b"ab7de9be-89fe-49ad-828f-118f09df7fd2\0", out_len),
        1 => static_c_str(b"fa19b0fb-cd1f-46a7-84a1-bbb09e00c149\0", out_len),
        2 => static_c_str(b"289326cb-a471-485d-a8f4-240c14f18241\0", out_len),
        3 => static_c_str(b"cc483f51-9258-427d-a939-630c31f72b05\0", out_len),
        4 => static_c_str(b"649d4ac9-8eb7-4e6c-af44-1ea54fe5f005\0", out_len),
        5 => static_c_str(b"c765a961-d9d8-4d36-a20a-5315b111836a\0", out_len),
        _ => static_c_str(b"\0", out_len),
    }
}

/// Strips the USB HID report ID byte from `usb_report[..len]`, writing `out_offset` and `out_len`.
///
/// # Safety
///
/// If `len > 0`, `usb_report` must point to `len` readable bytes; `out_offset` and `out_len` must
/// be null or point to valid, aligned, writable `usize` values.
#[no_mangle]
pub unsafe extern "C" fn egl_sw2_strip_hid_report_id(
    usb_report: *const u8,
    len: usize,
    out_offset: *mut usize,
    out_len: *mut usize,
) -> u8 {
    if out_offset.is_null() || !out_offset.is_aligned() || out_len.is_null() || !out_len.is_aligned()
    {
        return 0;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let Some(slice) = (unsafe { slice_from_raw(usb_report, len) }) else {
            return 0;
        };
        let Some(stripped) = strip_hid_report_id(slice) else {
            return 0;
        };
        unsafe {
            ptr::write(out_offset, 1);
            ptr::write(out_len, stripped.len());
        }
        1
    }));
    result.unwrap_or(0)
}

/// Parses a BLE manufacturer advertisement payload into `out_info`.
///
/// # Safety
///
/// If `len > 0`, `data` must point to `len` readable bytes; `out_info` must be null or point to
/// a valid, aligned, writable [`EglSw2BleAdvertisementInfo`].
#[no_mangle]
pub unsafe extern "C" fn egl_sw2_parse_ble_manufacturer_data(
    data: *const u8,
    len: usize,
    out_info: *mut EglSw2BleAdvertisementInfo,
) -> u8 {
    if out_info.is_null() || !out_info.is_aligned() {
        return 0;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let Some(slice) = (unsafe { slice_from_raw(data, len) }) else {
            return 0;
        };
        let Some(info) = parse_ble_manufacturer_data(slice) else {
            return 0;
        };
        unsafe {
            ptr::write(
                out_info,
                EglSw2BleAdvertisementInfo {
                    vendor_id: info.vendor_id,
                    product_id: info.product_id.as_u16(),
                    _pad: 0,
                    reconnect_mac: info.reconnect_mac,
                    is_pairing_mode: u8::from(info.is_pairing_mode),
                    _pad2: [0; 7],
                },
            );
        }
        1
    }));
    result.unwrap_or(0)
}

/// Unpacks 12-bit X/Y stick axes from `three_bytes[..len]` into `out_x` and `out_y`.
///
/// # Safety
///
/// If `len > 0`, `three_bytes` must point to `len` readable bytes; `out_x` and `out_y` must be
/// null or point to valid, aligned, writable `u16` values.
#[no_mangle]
pub unsafe extern "C" fn egl_sw2_unpack_stick_xy(
    three_bytes: *const u8,
    len: usize,
    out_x: *mut u16,
    out_y: *mut u16,
) {
    if out_x.is_null() || !out_x.is_aligned() || out_y.is_null() || !out_y.is_aligned() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let slice = unsafe { slice_from_raw(three_bytes, len) }.unwrap_or(&[]);
        let (x, y) = unpack_stick_xy(slice);
        unsafe {
            ptr::write(out_x, x);
            ptr::write(out_y, y);
        }
    }));
}

/// Parses factory stick calibration bytes into `out_cal`.
///
/// # Safety
///
/// If `len > 0`, `data` must point to `len` readable bytes; `out_cal` must be null or point to a
/// valid, aligned, writable [`EglSw2StickCalibration`].
#[no_mangle]
pub unsafe extern "C" fn egl_sw2_parse_stick_calibration(
    data: *const u8,
    len: usize,
    out_cal: *mut EglSw2StickCalibration,
) {
    if out_cal.is_null() || !out_cal.is_aligned() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let slice = unsafe { slice_from_raw(data, len) }.unwrap_or(&[]);
        let cal = parse_stick_calibration(slice);
        unsafe {
            ptr::write(out_cal, EglSw2StickCalibration::from(cal));
        }
    }));
}

/// Parses a user stick calibration block into `out_cal`.
///
/// # Safety
///
/// If `len > 0`, `block` must point to `len` readable bytes; `out_cal` must be null or point to
/// a valid, aligned, writable [`EglSw2StickCalibration`].
#[no_mangle]
pub unsafe extern "C" fn egl_sw2_parse_user_stick_calibration(
    block: *const u8,
    len: usize,
    out_cal: *mut EglSw2StickCalibration,
) -> u8 {
    if out_cal.is_null() || !out_cal.is_aligned() {
        return 0;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let Some(slice) = (unsafe { slice_from_raw(block, len) }) else {
            return 0;
        };
        let Some(cal) = parse_user_stick_calibration(slice) else {
            return 0;
        };
        unsafe {
            ptr::write(out_cal, EglSw2StickCalibration::from(cal));
        }
        1
    }));
    result.unwrap_or(0)
}

#[no_mangle]
pub extern "C" fn egl_sw2_apply_stick_calibration_axis(
    raw_value: u16,
    center: u16,
    max_offset: u16,
    min_offset: u16,
    deadzone: u16,
) -> f32 {
    apply_stick_calibration_axis(raw_value, center, max_offset, min_offset, deadzone)
}

/// Parses a controller info block from `data[..len]` into `out_info`.
///
/// # Safety
///
/// If `len > 0`, `data` must point to `len` readable bytes; `out_info` must be null or point to
/// a valid, aligned, writable [`EglSw2ControllerInfo`].
#[no_mangle]
pub unsafe extern "C" fn egl_sw2_parse_controller_info(
    data: *const u8,
    len: usize,
    out_info: *mut EglSw2ControllerInfo,
) -> u8 {
    if out_info.is_null() || !out_info.is_aligned() {
        return 0;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let Some(slice) = (unsafe { slice_from_raw(data, len) }) else {
            return 0;
        };
        let Some(info) = parse_controller_info(slice) else {
            return 0;
        };
        let mut c_info = EglSw2ControllerInfo::default();
        let raw_serial = &slice[2..16];
        let serial_len = raw_serial.iter().position(|&b| b == 0).unwrap_or(14);
        for (i, &b) in raw_serial[..serial_len].iter().enumerate() {
            c_info.serial_number[i] = b as c_char;
        }
        c_info.serial_len = serial_len as u8;
        c_info.vendor_id = info.vendor_id;
        c_info.product_id = info.product_id.as_u16();
        c_info.body_color = info.body_color;
        c_info.buttons_color = info.buttons_color;
        c_info.left_grip_color = info.left_grip_color;
        c_info.right_grip_color = info.right_grip_color;
        unsafe {
            ptr::write(out_info, c_info);
        }
        1
    }));
    result.unwrap_or(0)
}

/// Parses a Switch 2 input report `data[..len]` using `left_cal` and `right_cal` into `out_report`.
///
/// # Safety
///
/// If `len > 0`, `data` must point to `len` readable bytes; `left_cal`, `right_cal`, and
/// `out_report` must be null or point to valid, aligned structs (`out_report` writable).
#[no_mangle]
pub unsafe extern "C" fn egl_sw2_parse_input_report(
    data: *const u8,
    len: usize,
    left_cal: *const EglSw2StickCalibration,
    right_cal: *const EglSw2StickCalibration,
    deadzone: u16,
    out_report: *mut EglSw2InputReport,
) -> u8 {
    if left_cal.is_null()
        || !left_cal.is_aligned()
        || right_cal.is_null()
        || !right_cal.is_aligned()
        || out_report.is_null()
        || !out_report.is_aligned()
    {
        return 0;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let Some(slice) = (unsafe { slice_from_raw(data, len) }) else {
            return 0;
        };
        let lc = StickCalibration::from(unsafe { *left_cal });
        let rc = StickCalibration::from(unsafe { *right_cal });
        let Some(parsed) = parse_input_report(slice, &lc, &rc, deadzone) else {
            return 0;
        };
        unsafe {
            ptr::write(out_report, EglSw2InputReport::from(parsed));
        }
        1
    }));
    result.unwrap_or(0)
}

/// Converts raw IMU samples in `report` and `gyro_bias[..3]` into `out_motion`.
///
/// # Safety
///
/// Non-null `report`, `gyro_bias` (3 readable `f32` elements), and `out_motion` (writable) must be
/// valid and properly aligned.
#[no_mangle]
pub unsafe extern "C" fn egl_sw2_convert_imu_to_motion(
    report: *const EglSw2InputReport,
    gyro_bias: *const f32,
    delta_timestamp_us: u64,
    out_motion: *mut EglSw2MotionOutput,
) {
    if report.is_null()
        || !report.is_aligned()
        || gyro_bias.is_null()
        || !gyro_bias.is_aligned()
        || out_motion.is_null()
        || !out_motion.is_aligned()
    {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let rep = Switch2InputReport::from(unsafe { *report });
        let bias_slice = unsafe { std::slice::from_raw_parts(gyro_bias, 3) };
        let bias = [bias_slice[0], bias_slice[1], bias_slice[2]];
        let m = convert_imu_to_motion(&rep, bias, delta_timestamp_us);
        unsafe {
            ptr::write(
                out_motion,
                EglSw2MotionOutput {
                    gyro_x: m.gyro_x,
                    gyro_y: m.gyro_y,
                    gyro_z: m.gyro_z,
                    accel_x: m.accel_x,
                    accel_y: m.accel_y,
                    accel_z: m.accel_z,
                    delta_timestamp: m.delta_timestamp,
                },
            );
        }
    }));
}

/// Encodes `pkt` into a 5-byte HD rumble payload written to `out_bytes[..5]`.
///
/// # Safety
///
/// Non-null `pkt` must be valid and aligned, and `out_bytes` must point to 5 writable bytes.
#[no_mangle]
pub unsafe extern "C" fn egl_sw2_vibration_encode_5bytes(
    pkt: *const EglSw2VibrationPacketData,
    out_bytes: *mut u8,
) {
    if pkt.is_null() || !pkt.is_aligned() || out_bytes.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let rust_pkt = VibrationPacketData::from(unsafe { *pkt });
        let encoded = rust_pkt.encode_5_bytes();
        unsafe {
            ptr::copy_nonoverlapping(encoded.as_ptr(), out_bytes, 5);
        }
    }));
}

/// Builds a vibration packet from amplitude/frequency parameters into `out_pkt`.
///
/// # Safety
///
/// `out_pkt` must be null or point to a valid, aligned, writable [`EglSw2VibrationPacketData`].
#[no_mangle]
pub unsafe extern "C" fn egl_sw2_vibration_from_status(
    low_amplitude: f32,
    low_frequency: f32,
    high_amplitude: f32,
    high_frequency: f32,
    out_pkt: *mut EglSw2VibrationPacketData,
) {
    if out_pkt.is_null() || !out_pkt.is_aligned() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let pkt = VibrationPacketData::from_vibration_status(
            low_amplitude,
            low_frequency,
            high_amplitude,
            high_frequency,
        );
        unsafe {
            ptr::write(out_pkt, EglSw2VibrationPacketData::from(pkt));
        }
    }));
}

/// Builds a Switch 2 command packet from `payload[..payload_len]` into `out_bytes`.
///
/// # Safety
///
/// If `payload_len > 0`, `payload` must point to `payload_len` readable bytes; `out_bytes` must
/// be null or point to a valid, aligned, writable [`EglSw2Bytes`].
#[no_mangle]
pub unsafe extern "C" fn egl_sw2_build_command_packet(
    command_id: u8,
    subcommand_id: u8,
    payload: *const u8,
    payload_len: usize,
    is_ble: u8,
    out_bytes: *mut EglSw2Bytes,
) -> i32 {
    if out_bytes.is_null() || !out_bytes.is_aligned() {
        return -1;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let Some(slice) = (unsafe { slice_from_raw(payload, payload_len) }) else {
            return -1;
        };
        let packet = build_command_packet(command_id, subcommand_id, slice, is_ble != 0);
        unsafe {
            write_bytes(out_bytes, packet);
        }
        0
    }));
    result.unwrap_or(-1)
}

/// Writes an 8-byte SPI/memory read command payload to `out_payload[..8]`.
///
/// # Safety
///
/// Non-null `out_payload` must point to 8 writable bytes.
#[no_mangle]
pub unsafe extern "C" fn egl_sw2_build_memory_read_payload(
    length: u8,
    address: u32,
    out_payload: *mut u8,
) {
    if out_payload.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let payload = build_memory_read_payload(length, address);
        unsafe {
            ptr::copy_nonoverlapping(payload.as_ptr(), out_payload, 8);
        }
    }));
}

/// Builds a BLE vibration report from three motor packets into `out_bytes`.
///
/// # Safety
///
/// Non-null `motor1`, `motor2`, `motor3`, and `out_bytes` must be valid and properly aligned
/// (`out_bytes` writable).
#[no_mangle]
pub unsafe extern "C" fn egl_sw2_build_vibration_report(
    packet_id: u8,
    is_pro_controller: u8,
    motor1: *const EglSw2VibrationPacketData,
    motor2: *const EglSw2VibrationPacketData,
    motor3: *const EglSw2VibrationPacketData,
    out_bytes: *mut EglSw2Bytes,
) -> i32 {
    if motor1.is_null()
        || !motor1.is_aligned()
        || motor2.is_null()
        || !motor2.is_aligned()
        || motor3.is_null()
        || !motor3.is_aligned()
        || out_bytes.is_null()
        || !out_bytes.is_aligned()
    {
        return -1;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let m1 = VibrationPacketData::from(unsafe { *motor1 });
        let m2 = VibrationPacketData::from(unsafe { *motor2 });
        let m3 = VibrationPacketData::from(unsafe { *motor3 });
        let report = build_vibration_report(packet_id, is_pro_controller != 0, &m1, &m2, &m3);
        unsafe {
            write_bytes(out_bytes, report);
        }
        0
    }));
    result.unwrap_or(-1)
}

/// Builds a USB vibration report from `motor` into `out_bytes`.
///
/// # Safety
///
/// Non-null `motor` and `out_bytes` must be valid and properly aligned (`out_bytes` writable).
#[no_mangle]
pub unsafe extern "C" fn egl_sw2_build_usb_vibration_report(
    pid: u16,
    sequence: u8,
    motor: *const EglSw2VibrationPacketData,
    out_bytes: *mut EglSw2Bytes,
) -> i32 {
    if motor.is_null()
        || !motor.is_aligned()
        || out_bytes.is_null()
        || !out_bytes.is_aligned()
    {
        return -1;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let parsed_pid = ProductId::from_raw(pid).unwrap_or(ProductId::None);
        let m = VibrationPacketData::from(unsafe { *motor });
        let report = build_usb_vibration_report(parsed_pid, sequence, &m);
        unsafe {
            write_bytes(out_bytes, report);
        }
        0
    }));
    result.unwrap_or(-1)
}

#[no_mangle]
pub extern "C" fn egl_sw2_reverse_lower_bits4(value: u8) -> u8 {
    reverse_lower_bits4(value)
}

#[no_mangle]
pub extern "C" fn egl_sw2_signed_looping_difference16(previous: u16, current: u16) -> i32 {
    signed_looping_difference16(previous, current)
}

/// Processes one input report through the gyro-mouse state machine.
///
/// # Safety
///
/// Non-null `state`, `report`, `out_sample_action`, and `out_output` must be valid and properly
/// aligned (`state`, `out_sample_action`, and `out_output` writable); if `cal_sample_count > 0`,
/// `cal_samples_xyz` must point to `cal_sample_count * 3` readable `i16` values.
#[no_mangle]
pub unsafe extern "C" fn egl_sw2_gyro_mouse_process(
    state: *mut EglSw2GyroMouseState,
    cal_samples_xyz: *const i16,
    cal_sample_count: usize,
    report: *const EglSw2InputReport,
    controller_type: u8,
    horizontal_hold: u8,
    trigger_pressed: u8,
    mode: u32,
    activation_mode: u32,
    sensitivity: f32,
    now_ns: u64,
    out_sample_action: *mut u8,
    out_output: *mut EglSw2GyroMouseOutput,
) -> i32 {
    if state.is_null()
        || !state.is_aligned()
        || report.is_null()
        || !report.is_aligned()
        || out_sample_action.is_null()
        || out_output.is_null()
        || !out_output.is_aligned()
    {
        return -1;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let Some(sample_elem_count) = cal_sample_count.checked_mul(3) else {
            return -1;
        };
        let Some(samples_flat) = (unsafe { slice_from_raw(cal_samples_xyz, sample_elem_count) })
        else {
            return -1;
        };
        let Some(ctype) = ControllerType::from_raw(controller_type) else {
            return -1;
        };
        let Some(gmode) = GyroMouseMode::from_raw(mode) else {
            return -1;
        };
        let Some(amode) = GyroActivationMode::from_raw(activation_mode) else {
            return -1;
        };
        let c_state = unsafe { &mut *state };
        let rep = Switch2InputReport::from(unsafe { *report });

        let mut proc_state = GyroMouseProcessor {
            is_calibrating: c_state.is_calibrating != 0,
            calibration_end_ns: c_state.calibration_end_ns,
            calibration_samples: Vec::new(),
            gyro_bias: c_state.gyro_bias,
            gyro_mouse_enabled: c_state.gyro_mouse_enabled != 0,
            prev_trigger: c_state.prev_trigger != 0,
        };

        let mut sample_action = 0u8;
        let out = if proc_state.is_calibrating {
            if now_ns < proc_state.calibration_end_ns {
                sample_action = 1; // push current report gyroscope sample
                crate::types::GyroMouseOutput::default()
            } else {
                proc_state.is_calibrating = false;
                if cal_sample_count > 0 {
                    let mut sx = 0.0f64;
                    let mut sy = 0.0f64;
                    let mut sz = 0.0f64;
                    for chunk in samples_flat.as_chunks::<3>().0 {
                        sx += chunk[0] as f64;
                        sy += chunk[1] as f64;
                        sz += chunk[2] as f64;
                    }
                    let count = cal_sample_count as f64;
                    proc_state.gyro_bias =
                        [(sx / count) as f32, (sy / count) as f32, (sz / count) as f32];
                }
                sample_action = 2; // clear samples
                proc_state.process_post_calibration(
                    &rep,
                    ctype,
                    horizontal_hold != 0,
                    trigger_pressed != 0,
                    gmode,
                    amode,
                    sensitivity,
                )
            }
        } else {
            proc_state.process_post_calibration(
                &rep,
                ctype,
                horizontal_hold != 0,
                trigger_pressed != 0,
                gmode,
                amode,
                sensitivity,
            )
        };

        c_state.calibration_end_ns = proc_state.calibration_end_ns;
        c_state.gyro_bias = proc_state.gyro_bias;
        c_state.is_calibrating = u8::from(proc_state.is_calibrating);
        c_state.gyro_mouse_enabled = u8::from(proc_state.gyro_mouse_enabled);
        c_state.prev_trigger = u8::from(proc_state.prev_trigger);

        unsafe {
            ptr::write(out_sample_action, sample_action);
            ptr::write(
                out_output,
                EglSw2GyroMouseOutput {
                    active: u8::from(out.active),
                    has_roll_steer_x: u8::from(out.roll_steer_x.is_some()),
                    left_click: u8::from(out.left_click),
                    right_click: u8::from(out.right_click),
                    velocity_x: out.velocity_x,
                    velocity_y: out.velocity_y,
                    roll_steer_x: out.roll_steer_x.unwrap_or(0.0),
                },
            );
        }
        0
    }));
    result.unwrap_or(-1)
}

/// Processes one input report through the optical-mouse state machine.
///
/// # Safety
///
/// Non-null `state`, `report`, and `out_output` must be valid and properly aligned (`state` and
/// `out_output` writable).
#[no_mangle]
pub unsafe extern "C" fn egl_sw2_optical_mouse_process(
    state: *mut EglSw2OpticalMouseState,
    report: *const EglSw2InputReport,
    controller_type: u8,
    enabled: u8,
    out_output: *mut EglSw2OpticalMouseOutput,
) -> i32 {
    if state.is_null()
        || !state.is_aligned()
        || report.is_null()
        || !report.is_aligned()
        || out_output.is_null()
        || !out_output.is_aligned()
    {
        return -1;
    }
    let result = catch_unwind(AssertUnwindSafe(|| {
        let Some(ctype) = ControllerType::from_raw(controller_type) else {
            return -1;
        };
        let c_state = unsafe { &mut *state };
        let rep = Switch2InputReport::from(unsafe { *report });
        let mut proc_state = OpticalMouseProcessor {
            has_previous: c_state.has_previous != 0,
            prev_x: c_state.prev_x,
            prev_y: c_state.prev_y,
        };
        let out = proc_state.process(&rep, ctype, enabled != 0);
        c_state.has_previous = u8::from(proc_state.has_previous);
        c_state.prev_x = proc_state.prev_x;
        c_state.prev_y = proc_state.prev_y;
        unsafe {
            ptr::write(
                out_output,
                EglSw2OpticalMouseOutput {
                    active: u8::from(out.active),
                    left_button: u8::from(out.left_button),
                    middle_button: u8::from(out.middle_button),
                    right_button: u8::from(out.right_button),
                    delta_x: out.delta_x,
                    delta_y: out.delta_y,
                    scroll_y: out.scroll_y,
                },
            );
        }
        0
    }));
    result.unwrap_or(-1)
}

/// Frees a byte buffer owned by [`EglSw2Bytes`] and zeroes its fields.
///
/// # Safety
///
/// `bytes` must be null or point to a valid, aligned [`EglSw2Bytes`] allocated by this crate (or
/// already zeroed).
#[no_mangle]
pub unsafe extern "C" fn egl_sw2_free_bytes(bytes: *mut EglSw2Bytes) {
    if bytes.is_null() || !bytes.is_aligned() {
        return;
    }
    unsafe {
        let b = &mut *bytes;
        if !b.data.is_null() && b.len > 0 {
            drop(Box::from_raw(ptr::slice_from_raw_parts_mut(b.data, b.len)));
        }
        *b = EglSw2Bytes::default();
    }
}
