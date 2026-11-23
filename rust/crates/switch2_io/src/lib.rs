// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![deny(warnings)]

pub mod ffi;
pub mod protocol;
pub mod rumble;
pub mod types;

pub use protocol::{
    apply_stick_calibration_axis, build_command_packet, build_memory_read_payload,
    convert_imu_to_motion, parse_ble_manufacturer_data, parse_controller_info, parse_input_report,
    parse_stick_calibration, parse_user_stick_calibration, strip_hid_report_id, unpack_stick_xy,
    GyroMouseProcessor, OpticalMouseProcessor,
};
pub use rumble::{build_usb_vibration_report, build_vibration_report, VibrationPacketData};
pub use types::{
    ble_uuids, command_id, feature_flags, feature_mask_for_product, is_supported_product_id,
    memory_address, product_id_to_controller_type, product_name, reverse_lower_bits4,
    signed_looping_difference16, sub_command_id, BleAdvertisementInfo, ControllerInfo,
    ControllerType, GyroActivationMode, GyroMouseMode, GyroMouseOutput, MotionOutput,
    OpticalMouseOutput, PadAxes, PadButton, PadMotion, ProductId, StickCalibration,
    Switch2InputReport, TransportType, COMMON_REPORT_ID, DEFAULT_STICK_DEADZONE, DEVICE_ID_KEY,
    FACTORY_CALIBRATION_SIZE, LED_PATTERNS, MINIMUM_INPUT_REPORT_SIZE, NATIVE_INPUT_REPORT_SIZE,
    NINTENDO_BLE_MANUFACTURER_ID, NINTENDO_VENDOR_ID, USER_CALIBRATION_BLOCK_SIZE,
    USER_CALIBRATION_MAGIC,
};
