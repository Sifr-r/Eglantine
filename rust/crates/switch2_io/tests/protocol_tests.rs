// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![deny(warnings)]

use eglantine_switch2_io::ffi::{
    egl_sw2_apply_stick_calibration_axis, egl_sw2_ble_uuid, egl_sw2_build_command_packet,
    egl_sw2_build_memory_read_payload, egl_sw2_build_usb_vibration_report,
    egl_sw2_build_vibration_report, egl_sw2_convert_imu_to_motion, egl_sw2_feature_mask_for_product,
    egl_sw2_free_bytes, egl_sw2_gyro_mouse_process, egl_sw2_is_supported_product_id,
    egl_sw2_optical_mouse_process, egl_sw2_parse_ble_manufacturer_data,
    egl_sw2_parse_controller_info, egl_sw2_parse_input_report, egl_sw2_parse_stick_calibration,
    egl_sw2_parse_user_stick_calibration, egl_sw2_product_id_to_controller_type,
    egl_sw2_product_name, egl_sw2_reverse_lower_bits4, egl_sw2_signed_looping_difference16,
    egl_sw2_strip_hid_report_id, egl_sw2_unpack_stick_xy, egl_sw2_vibration_encode_5bytes,
    egl_sw2_vibration_from_status, EglSw2BleAdvertisementInfo, EglSw2Bytes, EglSw2ControllerInfo,
    EglSw2GyroMouseOutput, EglSw2GyroMouseState, EglSw2InputReport, EglSw2MotionOutput,
    EglSw2OpticalMouseOutput, EglSw2OpticalMouseState, EglSw2StickCalibration,
    EglSw2VibrationPacketData,
};
use eglantine_switch2_io::{
    apply_stick_calibration_axis, ble_uuids, build_command_packet, build_memory_read_payload,
    build_usb_vibration_report, build_vibration_report, command_id, convert_imu_to_motion,
    feature_flags, feature_mask_for_product, is_supported_product_id, memory_address,
    parse_ble_manufacturer_data, parse_controller_info, parse_input_report,
    parse_stick_calibration, parse_user_stick_calibration, product_id_to_controller_type,
    product_name, reverse_lower_bits4, signed_looping_difference16, strip_hid_report_id,
    sub_command_id, unpack_stick_xy, ControllerType, GyroActivationMode, GyroMouseMode,
    GyroMouseProcessor, OpticalMouseProcessor, PadButton, ProductId, StickCalibration,
    Switch2InputReport, VibrationPacketData, MINIMUM_INPUT_REPORT_SIZE, NINTENDO_VENDOR_ID,
};
use std::ptr;

fn write_u16_le(dst: &mut [u8], offset: usize, value: u16) {
    dst[offset] = (value & 0xFF) as u8;
    dst[offset + 1] = ((value >> 8) & 0xFF) as u8;
}

fn write_s16_le(dst: &mut [u8], offset: usize, value: i16) {
    write_u16_le(dst, offset, value as u16);
}

fn write_u32_le(dst: &mut [u8], offset: usize, value: u32) {
    dst[offset] = (value & 0xFF) as u8;
    dst[offset + 1] = ((value >> 8) & 0xFF) as u8;
    dst[offset + 2] = ((value >> 16) & 0xFF) as u8;
    dst[offset + 3] = ((value >> 24) & 0xFF) as u8;
}

fn pack_stick_xy(x: u16, y: u16) -> [u8; 3] {
    let packed = ((x & 0xFFF) as u32) | (((y & 0xFFF) as u32) << 12);
    [
        (packed & 0xFF) as u8,
        ((packed >> 8) & 0xFF) as u8,
        ((packed >> 16) & 0xFF) as u8,
    ]
}

fn write_rgb24(dst: &mut [u8], offset: usize, rgb: u32) {
    dst[offset] = ((rgb >> 16) & 0xFF) as u8;
    dst[offset + 1] = ((rgb >> 8) & 0xFF) as u8;
    dst[offset + 2] = (rgb & 0xFF) as u8;
}

fn approx_eq(a: f32, b: f32) -> bool {
    (a - b).abs() < 1e-4
}

#[test]
fn product_ids_and_feature_masks() {
    assert!(is_supported_product_id(0x2066));
    assert!(is_supported_product_id(0x2067));
    assert!(is_supported_product_id(0x2069));
    assert!(is_supported_product_id(0x2073));
    assert!(!is_supported_product_id(0x2009));

    assert_eq!(
        product_id_to_controller_type(ProductId::JoyCon2Left),
        ControllerType::Left
    );
    assert_eq!(
        product_id_to_controller_type(ProductId::JoyCon2Right),
        ControllerType::Right
    );
    assert_eq!(
        product_id_to_controller_type(ProductId::ProController2),
        ControllerType::Pro
    );
    assert_eq!(
        product_id_to_controller_type(ProductId::NsoGameCube),
        ControllerType::GameCube
    );

    assert_eq!(
        feature_mask_for_product(ProductId::JoyCon2Left),
        feature_flags::JOYCON
    );
    assert_eq!(
        feature_mask_for_product(ProductId::JoyCon2Right),
        feature_flags::JOYCON
    );
    assert_eq!(
        feature_mask_for_product(ProductId::ProController2),
        feature_flags::STANDARD
    );
    assert_eq!(
        product_name(ProductId::ProController2),
        "Switch 2 Pro Controller"
    );
    assert_eq!(
        ble_uuids::INPUT_REPORT,
        "ab7de9be-89fe-49ad-828f-118f09df7fd2"
    );
}

#[test]
fn ble_manufacturer_data_and_hid_strip() {
    assert!(parse_ble_manufacturer_data(&[]).is_none());
    assert!(parse_ble_manufacturer_data(&[0u8; 15]).is_none());

    let mut data = [0u8; 16];
    write_u16_le(&mut data, 3, NINTENDO_VENDOR_ID);
    write_u16_le(&mut data, 5, ProductId::ProController2 as u16);
    let parsed = parse_ble_manufacturer_data(&data).unwrap();
    assert_eq!(parsed.vendor_id, NINTENDO_VENDOR_ID);
    assert_eq!(parsed.product_id, ProductId::ProController2);
    assert_eq!(parsed.reconnect_mac, 0);
    assert!(parsed.is_pairing_mode);

    write_u16_le(&mut data, 5, ProductId::JoyCon2Left as u16);
    data[10..16].copy_from_slice(&[0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF]);
    let paired = parse_ble_manufacturer_data(&data).unwrap();
    assert_eq!(paired.product_id, ProductId::JoyCon2Left);
    assert_eq!(paired.reconnect_mac, 0xFFEEDDCCBBAA);
    assert!(!paired.is_pairing_mode);

    assert!(strip_hid_report_id(&[0u8; MINIMUM_INPUT_REPORT_SIZE]).is_none());
    let usb = vec![0x05u8; MINIMUM_INPUT_REPORT_SIZE + 1];
    assert_eq!(
        strip_hid_report_id(&usb).unwrap().len(),
        MINIMUM_INPUT_REPORT_SIZE
    );
}

#[test]
fn stick_unpacking_calibration_and_normalization() {
    assert_eq!(unpack_stick_xy(&[]), (2048, 2048));
    assert_eq!(unpack_stick_xy(&[0x12, 0x34]), (2048, 2048));

    let packed = pack_stick_xy(0x123, 0xABC);
    assert_eq!(packed, [0x23, 0xC1, 0xAB]);
    assert_eq!(unpack_stick_xy(&packed), (0x123, 0xABC));

    let zero_cal = parse_stick_calibration(&[0u8; 9]);
    assert_eq!(zero_cal, StickCalibration::default());

    let mut asym = [0u8; 9];
    asym[0..3].copy_from_slice(&pack_stick_xy(2060, 2030));
    asym[3..6].copy_from_slice(&pack_stick_xy(1400, 1600));
    asym[6..9].copy_from_slice(&pack_stick_xy(1200, 1000));
    let parsed = parse_stick_calibration(&asym);
    assert_eq!(parsed.center_x, 2060);
    assert_eq!(parsed.center_y, 2030);
    assert_eq!(parsed.max_x, 1400);
    assert_eq!(parsed.max_y, 1600);
    assert_eq!(parsed.min_x, 1200);
    assert_eq!(parsed.min_y, 1000);

    let mut user_block = [0u8; 11];
    write_u16_le(&mut user_block, 0, 0xA1B2);
    user_block[2..11].copy_from_slice(&asym);
    assert_eq!(parse_user_stick_calibration(&user_block), Some(parsed));
    user_block[0] = 0;
    assert_eq!(parse_user_stick_calibration(&user_block), None);

    assert_eq!(
        apply_stick_calibration_axis(2050, 2000, 1000, 500, 50),
        0.0
    );
    assert!(approx_eq(
        apply_stick_calibration_axis(2500, 2000, 1000, 500, 50),
        0.5
    ));
    assert!(approx_eq(
        apply_stick_calibration_axis(1750, 2000, 1000, 500, 50),
        -0.5
    ));
    assert_eq!(apply_stick_calibration_axis(2100, 2000, 0, 500, 50), 1.0);
    assert_eq!(apply_stick_calibration_axis(1900, 2000, 1000, 0, 50), -1.0);
}

#[test]
fn controller_info_and_input_report() {
    assert!(parse_controller_info(&[0u8; 36]).is_none());

    let mut data = [0u8; 37];
    let serial = b"HAC1234567";
    data[2..2 + serial.len()].copy_from_slice(serial);
    write_u16_le(&mut data, 18, NINTENDO_VENDOR_ID);
    write_u16_le(&mut data, 20, ProductId::ProController2 as u16);
    write_rgb24(&mut data, 25, 0x112233);
    write_rgb24(&mut data, 28, 0x445566);
    write_rgb24(&mut data, 31, 0x778899);
    write_rgb24(&mut data, 34, 0xAABBCC);

    let info = parse_controller_info(&data).unwrap();
    assert_eq!(info.serial_number, "HAC1234567");
    assert_eq!(info.vendor_id, NINTENDO_VENDOR_ID);
    assert_eq!(info.product_id, ProductId::ProController2);
    assert_eq!(info.body_color, 0x00112233);
    assert_eq!(info.buttons_color, 0x00445566);
    assert_eq!(info.left_grip_color, 0x00778899);
    assert_eq!(info.right_grip_color, 0x00AABBCC);

    let mut raw = [0u8; MINIMUM_INPUT_REPORT_SIZE];
    write_u32_le(&mut raw, 0, 0x89AB_CDEF);
    let buttons = (PadButton::A as u32) | (PadButton::ZR as u32) | (PadButton::C as u32);
    write_u32_le(&mut raw, 4, buttons);
    raw[10..13].copy_from_slice(&pack_stick_xy(2500, 1750));
    raw[13..16].copy_from_slice(&pack_stick_xy(2020, 3200));
    write_u16_le(&mut raw, 16, 0x1234);
    write_u16_le(&mut raw, 18, 0xABCD);
    write_u16_le(&mut raw, 20, 1234);
    write_u16_le(&mut raw, 22, 567);
    write_s16_le(&mut raw, 25, -120);
    write_s16_le(&mut raw, 27, 0);
    write_s16_le(&mut raw, 29, 320);
    write_u16_le(&mut raw, 31, 4150);
    write_u16_le(&mut raw, 34, 325);
    write_u16_le(&mut raw, 46, 635);
    write_s16_le(&mut raw, 48, -4096);
    write_s16_le(&mut raw, 50, 2048);
    write_s16_le(&mut raw, 52, -32768);
    write_s16_le(&mut raw, 54, -500);
    write_s16_le(&mut raw, 56, 1234);
    write_s16_le(&mut raw, 58, 32767);

    let asym_cal = StickCalibration {
        center_x: 2000,
        center_y: 2000,
        max_x: 1000,
        max_y: 1000,
        min_x: 500,
        min_y: 500,
    };
    let report = parse_input_report(&raw, &asym_cal, &asym_cal, 50).unwrap();
    assert_eq!(report.timestamp, 0x89AB_CDEF);
    assert_eq!(report.buttons, buttons);
    assert!(approx_eq(report.left_x, 0.5));
    assert!(approx_eq(report.left_y, -0.5));
    assert_eq!(report.right_x, 0.0);
    assert!(approx_eq(report.right_y, 1.0));
    assert_eq!(report.magnetometer, [-120, 0, 320]);
    assert!(approx_eq(report.battery_voltage, 4.15));
    assert!(approx_eq(report.battery_current, 3.25));
    assert!(approx_eq(report.temperature, 30.0));
    assert_eq!(report.accelerometer, [-4096, 2048, -32768]);
    assert_eq!(report.gyroscope, [-500, 1234, 32767]);

    let motion = convert_imu_to_motion(&report, [0.0, 0.0, 0.0], 0);
    assert_eq!(motion.delta_timestamp, 4000);
    assert!(approx_eq(motion.accel_x, -1.0));
    assert!(approx_eq(motion.accel_y, -0.5));
}

#[test]
fn rumble_encoding_and_packets() {
    let known = VibrationPacketData {
        lf_freq: 0x155,
        lf_amp: 0x2AA,
        hf_freq: 0x0F0,
        hf_amp: 0x301,
    };
    assert_eq!(known.encode_5_bytes(), [0xF0, 0x04, 0x5C, 0x95, 0xAA]);

    let mid = VibrationPacketData::from_vibration_status(0.5, 100.0, 0.25, 200.0);
    assert_eq!(mid.lf_amp, 400);
    assert_eq!(mid.hf_amp, 200);
    assert_eq!(mid.lf_freq, 140);
    assert_eq!(mid.hf_freq, 300);

    let jc = build_vibration_report(0x13, false, &known, &mid, &VibrationPacketData::default());
    assert_eq!(jc.len(), 17);
    assert_eq!(jc[0], 0x00);
    assert_eq!(jc[1], 0x53);

    let pro = build_vibration_report(0x05, true, &known, &mid, &VibrationPacketData::default());
    assert_eq!(pro.len(), 33);
    assert_eq!(&pro[1..17], &pro[17..33]);

    let usb_pro = build_usb_vibration_report(ProductId::ProController2, 0x07, &known);
    assert_eq!(usb_pro.len(), 64);
    assert_eq!(usb_pro[0], 0x02);
    assert_eq!(usb_pro[1], 0x57);
    assert_eq!(usb_pro[0x11], 0x57);

    let usb_gc = build_usb_vibration_report(ProductId::NsoGameCube, 0x01, &known);
    assert_eq!(usb_gc, vec![0x03, 0x51, 0x01, 0, 0, 0, 0]);

    let mem = build_memory_read_payload(0x40, memory_address::CONTROLLER_INFO);
    assert_eq!(mem, [0x40, 0x7E, 0x00, 0x00, 0x00, 0x30, 0x01, 0x00]);
    let cmd = build_command_packet(command_id::MEMORY, sub_command_id::MEMORY_READ, &mem, true);
    assert_eq!(
        cmd,
        vec![0x02, 0x91, 0x01, 0x04, 0x00, 0x08, 0x00, 0x00, 0x40, 0x7E, 0x00, 0x00, 0x00, 0x30, 0x01, 0x00]
    );
}

#[test]
fn gyro_and_optical_mouse_processors() {
    assert_eq!(reverse_lower_bits4(0x01), 0x08);
    assert_eq!(signed_looping_difference16(65530, 5), 11);
    assert_eq!(signed_looping_difference16(5, 65530), -11);

    let mut gyro = GyroMouseProcessor::default();
    let t0 = 10_000_000_000u64;
    gyro.start_calibration(t0);
    let mut s = Switch2InputReport::default();
    s.gyroscope = [10, -20, 30];
    assert!(
        !gyro
            .process(
                &s,
                ControllerType::Pro,
                false,
                true,
                GyroMouseMode::Yaw,
                GyroActivationMode::Hold,
                1.0,
                t0
            )
            .active
    );
    s.gyroscope = [20, -40, 60];
    assert!(
        !gyro
            .process(
                &s,
                ControllerType::Pro,
                false,
                true,
                GyroMouseMode::Yaw,
                GyroActivationMode::Hold,
                1.0,
                t0 + 1_000_000_000
            )
            .active
    );
    s.gyroscope = [30, -60, 90];
    assert!(
        !gyro
            .process(
                &s,
                ControllerType::Pro,
                false,
                true,
                GyroMouseMode::Yaw,
                GyroActivationMode::Hold,
                1.0,
                t0 + 1_999_000_000
            )
            .active
    );
    s.gyroscope = [20, -40, 60];
    let out = gyro.process(
        &s,
        ControllerType::Pro,
        false,
        true,
        GyroMouseMode::Yaw,
        GyroActivationMode::Hold,
        1.0,
        t0 + 2_000_000_000,
    );
    assert!(out.active);
    assert!(!gyro.is_calibrating);
    assert!(approx_eq(gyro.gyro_bias[0], 20.0));
    assert!(approx_eq(gyro.gyro_bias[1], -40.0));
    assert!(approx_eq(gyro.gyro_bias[2], 60.0));

    let mut opt = OpticalMouseProcessor::default();
    let mut r1 = Switch2InputReport::default();
    r1.mouse_x = 65530;
    r1.mouse_y = 10;
    r1.mouse_distance = 300;
    r1.mouse_roughness = 500;
    let o1 = opt.process(&r1, ControllerType::Right, true);
    assert!(o1.active);
    assert_eq!(o1.delta_x, 0);
    r1.mouse_x = 5;
    r1.mouse_y = 65530;
    let o2 = opt.process(&r1, ControllerType::Right, true);
    assert!(o2.active);
    assert_eq!(o2.delta_x, 11);
    assert_eq!(o2.delta_y, -16);
}

#[test]
fn ffi_surface_and_null_safety() {
    assert_eq!(egl_sw2_is_supported_product_id(0x2069), 1);
    assert_eq!(egl_sw2_product_id_to_controller_type(0x2073), 0x08);
    assert_eq!(egl_sw2_feature_mask_for_product(0x2066), 0x37);
    assert_eq!(egl_sw2_reverse_lower_bits4(0x01), 0x08);
    assert_eq!(egl_sw2_signed_looping_difference16(0, 65535), -1);

    let mut len = 0usize;
    let name_ptr = egl_sw2_product_name(0x2069, &mut len);
    assert!(!name_ptr.is_null());
    assert_eq!(len, "Switch 2 Pro Controller".len());
    let uuid_ptr = egl_sw2_ble_uuid(0, &mut len);
    assert!(!uuid_ptr.is_null());
    assert_eq!(len, ble_uuids::INPUT_REPORT.len());

    // Null / invalid pointers fail gracefully without UB or panics
    assert_eq!(
        egl_sw2_parse_ble_manufacturer_data(ptr::null(), 16, ptr::null_mut()),
        0
    );
    assert_eq!(
        egl_sw2_strip_hid_report_id(ptr::null(), 0, ptr::null_mut(), ptr::null_mut()),
        0
    );
    assert_eq!(
        egl_sw2_parse_user_stick_calibration(ptr::null(), 0, ptr::null_mut()),
        0
    );
    assert_eq!(
        egl_sw2_parse_controller_info(ptr::null(), 0, ptr::null_mut()),
        0
    );
    assert_eq!(
        egl_sw2_parse_input_report(
            ptr::null(),
            0,
            ptr::null(),
            ptr::null(),
            50,
            ptr::null_mut()
        ),
        0
    );

    // Valid FFI roundtrip + idempotent free
    let mut ble_info = EglSw2BleAdvertisementInfo::default();
    let mut adv = [0u8; 16];
    write_u16_le(&mut adv, 3, NINTENDO_VENDOR_ID);
    write_u16_le(&mut adv, 5, 0x2066);
    assert_eq!(
        egl_sw2_parse_ble_manufacturer_data(adv.as_ptr(), adv.len(), &mut ble_info),
        1
    );
    assert_eq!(ble_info.product_id, 0x2066);

    let mut bytes = EglSw2Bytes::default();
    let payload = [0xAAu8, 0xBB];
    assert_eq!(
        egl_sw2_build_command_packet(0x02, 0x04, payload.as_ptr(), payload.len(), 1, &mut bytes),
        0
    );
    assert_eq!(bytes.len, 10);
    egl_sw2_free_bytes(&mut bytes);
    assert!(bytes.data.is_null());
    assert_eq!(bytes.len, 0);
    egl_sw2_free_bytes(&mut bytes);
    egl_sw2_free_bytes(ptr::null_mut());

    // Smoke-check remaining FFI entrypoints compile & run cleanly
    let mut x = 0u16;
    let mut y = 0u16;
    let stick_bytes = pack_stick_xy(100, 200);
    egl_sw2_unpack_stick_xy(stick_bytes.as_ptr(), 3, &mut x, &mut y);
    assert_eq!((x, y), (100, 200));

    let mut cal = EglSw2StickCalibration::default();
    egl_sw2_parse_stick_calibration([0u8; 9].as_ptr(), 9, &mut cal);
    assert_eq!(cal.center_x, 2048);
    assert_eq!(
        egl_sw2_apply_stick_calibration_axis(2048, 2048, 1500, 1500, 50),
        0.0
    );

    let mut cinfo = EglSw2ControllerInfo::default();
    let mut cbuf = [0u8; 37];
    cbuf[2..5].copy_from_slice(b"ABC");
    assert_eq!(
        egl_sw2_parse_controller_info(cbuf.as_ptr(), cbuf.len(), &mut cinfo),
        1
    );
    assert_eq!(cinfo.serial_len, 3);

    let mut irep = EglSw2InputReport::default();
    let raw_rep = [0u8; MINIMUM_INPUT_REPORT_SIZE];
    assert_eq!(
        egl_sw2_parse_input_report(raw_rep.as_ptr(), raw_rep.len(), &cal, &cal, 50, &mut irep),
        1
    );
    let mut mot = EglSw2MotionOutput::default();
    let bias = [0.0f32; 3];
    egl_sw2_convert_imu_to_motion(&irep, bias.as_ptr(), 4000, &mut mot);
    assert_eq!(mot.delta_timestamp, 4000);

    let mut vpkt = EglSw2VibrationPacketData::default();
    egl_sw2_vibration_from_status(0.5, 100.0, 0.25, 200.0, &mut vpkt);
    let mut enc5 = [0u8; 5];
    egl_sw2_vibration_encode_5bytes(&vpkt, enc5.as_mut_ptr());

    let mut mread = [0u8; 8];
    egl_sw2_build_memory_read_payload(0x40, 0x0001_3000, mread.as_mut_ptr());
    assert_eq!(mread[0], 0x40);

    let mut vib_rep = EglSw2Bytes::default();
    assert_eq!(
        egl_sw2_build_vibration_report(0, 1, &vpkt, &vpkt, &vpkt, &mut vib_rep),
        0
    );
    assert_eq!(vib_rep.len, 33);
    egl_sw2_free_bytes(&mut vib_rep);

    assert_eq!(
        egl_sw2_build_usb_vibration_report(0x2069, 2, &vpkt, &mut vib_rep),
        0
    );
    assert_eq!(vib_rep.len, 64);
    egl_sw2_free_bytes(&mut vib_rep);

    let mut gstate = EglSw2GyroMouseState::default();
    let mut gout = EglSw2GyroMouseOutput::default();
    let mut action = 0u8;
    assert_eq!(
        egl_sw2_gyro_mouse_process(
            &mut gstate,
            ptr::null(),
            0,
            &irep,
            ControllerType::Pro as u8,
            0,
            1,
            1,
            0,
            1.0,
            0,
            &mut action,
            &mut gout
        ),
        0
    );
    assert_eq!(gout.active, 1);

    let mut ostate = EglSw2OpticalMouseState::default();
    let mut oout = EglSw2OpticalMouseOutput::default();
    assert_eq!(
        egl_sw2_optical_mouse_process(
            &mut ostate,
            &irep,
            ControllerType::Left as u8,
            1,
            &mut oout
        ),
        0
    );
    assert_eq!(oout.active, 0);

    // F0599: cal_sample_count * 3 overflow must be rejected instead of wrapping.
    let sentinel_samples = [0i16; 3];
    let overflow_samples = (usize::MAX / 3) + 1;
    assert_eq!(
        egl_sw2_gyro_mouse_process(
            &mut gstate,
            sentinel_samples.as_ptr(),
            overflow_samples,
            &irep,
            ControllerType::Pro as u8,
            0,
            1,
            1,
            0,
            1.0,
            0,
            &mut action,
            &mut gout
        ),
        -1
    );
}
