// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum EglSw2ProductId {
    EGL_SW2_PRODUCT_NONE = 0x0000,
    EGL_SW2_PRODUCT_JOYCON2_RIGHT = 0x2066,
    EGL_SW2_PRODUCT_JOYCON2_LEFT = 0x2067,
    EGL_SW2_PRODUCT_PRO_CONTROLLER2 = 0x2069,
    EGL_SW2_PRODUCT_NSO_GAMECUBE = 0x2073,
} EglSw2ProductId;

typedef enum EglSw2ControllerType {
    EGL_SW2_CONTROLLER_NONE = 0x00,
    EGL_SW2_CONTROLLER_LEFT = 0x01,
    EGL_SW2_CONTROLLER_RIGHT = 0x02,
    EGL_SW2_CONTROLLER_PRO = 0x03,
    EGL_SW2_CONTROLLER_DUAL = 0x05,
    EGL_SW2_CONTROLLER_GAMECUBE = 0x08,
} EglSw2ControllerType;

typedef enum EglSw2BleUuidKind {
    EGL_SW2_UUID_INPUT_REPORT = 0,
    EGL_SW2_UUID_VIBRATION_JOYCON_R = 1,
    EGL_SW2_UUID_VIBRATION_JOYCON_L = 2,
    EGL_SW2_UUID_VIBRATION_PRO_CONTROLLER = 3,
    EGL_SW2_UUID_COMMAND_WRITE = 4,
    EGL_SW2_UUID_COMMAND_RESPONSE = 5,
} EglSw2BleUuidKind;

typedef struct EglSw2BleAdvertisementInfo {
    uint16_t vendor_id;
    uint16_t product_id;
    uint32_t _pad;
    uint64_t reconnect_mac;
    uint8_t is_pairing_mode;
    uint8_t _pad2[7];
} EglSw2BleAdvertisementInfo;

typedef struct EglSw2StickCalibration {
    uint16_t center_x;
    uint16_t center_y;
    uint16_t max_x;
    uint16_t max_y;
    uint16_t min_x;
    uint16_t min_y;
} EglSw2StickCalibration;

typedef struct EglSw2ControllerInfo {
    char serial_number[16];
    uint8_t serial_len;
    uint8_t _pad;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t _pad2;
    uint32_t body_color;
    uint32_t buttons_color;
    uint32_t left_grip_color;
    uint32_t right_grip_color;
} EglSw2ControllerInfo;

typedef struct EglSw2VibrationPacketData {
    uint16_t lf_freq;
    uint16_t lf_amp;
    uint16_t hf_freq;
    uint16_t hf_amp;
} EglSw2VibrationPacketData;

typedef struct EglSw2InputReport {
    uint32_t timestamp;
    uint32_t buttons;
    uint16_t raw_left_x;
    uint16_t raw_left_y;
    uint16_t raw_right_x;
    uint16_t raw_right_y;
    float left_x;
    float left_y;
    float right_x;
    float right_y;
    uint16_t mouse_x;
    uint16_t mouse_y;
    uint16_t mouse_roughness;
    uint16_t mouse_distance;
    int16_t magnetometer[3];
    int16_t _pad_mag;
    float battery_voltage;
    float battery_current;
    float temperature;
    int16_t accelerometer[3];
    int16_t gyroscope[3];
} EglSw2InputReport;

typedef struct EglSw2MotionOutput {
    float gyro_x;
    float gyro_y;
    float gyro_z;
    float accel_x;
    float accel_y;
    float accel_z;
    uint64_t delta_timestamp;
} EglSw2MotionOutput;

typedef struct EglSw2GyroMouseState {
    uint64_t calibration_end_ns;
    float gyro_bias[3];
    uint8_t is_calibrating;
    uint8_t gyro_mouse_enabled;
    uint8_t prev_trigger;
    uint8_t _pad;
} EglSw2GyroMouseState;

typedef struct EglSw2GyroMouseOutput {
    uint8_t active;
    uint8_t has_roll_steer_x;
    uint8_t left_click;
    uint8_t right_click;
    float velocity_x;
    float velocity_y;
    float roll_steer_x;
} EglSw2GyroMouseOutput;

typedef struct EglSw2OpticalMouseState {
    uint8_t has_previous;
    uint8_t _pad;
    uint16_t prev_x;
    uint16_t prev_y;
} EglSw2OpticalMouseState;

typedef struct EglSw2OpticalMouseOutput {
    uint8_t active;
    uint8_t left_button;
    uint8_t middle_button;
    uint8_t right_button;
    int32_t delta_x;
    int32_t delta_y;
    float scroll_y;
} EglSw2OpticalMouseOutput;

typedef struct EglSw2Bytes {
    uint8_t* data;
    size_t len;
} EglSw2Bytes;

uint8_t egl_sw2_is_supported_product_id(uint16_t raw_pid);

uint8_t egl_sw2_product_id_to_controller_type(uint16_t pid);

const char* egl_sw2_product_name(uint16_t pid, size_t* out_len);

uint8_t egl_sw2_feature_mask_for_product(uint16_t pid);

const char* egl_sw2_ble_uuid(uint32_t kind, size_t* out_len);

uint8_t egl_sw2_strip_hid_report_id(const uint8_t* usb_report,
                                      size_t len,
                                      size_t* out_offset,
                                      size_t* out_len);

uint8_t egl_sw2_parse_ble_manufacturer_data(const uint8_t* data,
                                            size_t len,
                                            EglSw2BleAdvertisementInfo* out_info);

void egl_sw2_unpack_stick_xy(const uint8_t* three_bytes,
                             size_t len,
                             uint16_t* out_x,
                             uint16_t* out_y);

void egl_sw2_parse_stick_calibration(const uint8_t* data,
                                     size_t len,
                                     EglSw2StickCalibration* out_cal);

uint8_t egl_sw2_parse_user_stick_calibration(const uint8_t* block,
                                             size_t len,
                                             EglSw2StickCalibration* out_cal);

float egl_sw2_apply_stick_calibration_axis(uint16_t raw_value,
                                           uint16_t center,
                                           uint16_t max_offset,
                                           uint16_t min_offset,
                                           uint16_t deadzone);

uint8_t egl_sw2_parse_controller_info(const uint8_t* data,
                                      size_t len,
                                      EglSw2ControllerInfo* out_info);

uint8_t egl_sw2_parse_input_report(const uint8_t* data,
                                   size_t len,
                                   const EglSw2StickCalibration* left_cal,
                                   const EglSw2StickCalibration* right_cal,
                                   uint16_t deadzone,
                                   EglSw2InputReport* out_report);

void egl_sw2_convert_imu_to_motion(const EglSw2InputReport* report,
                                   const float gyro_bias[3],
                                   uint64_t delta_timestamp_us,
                                   EglSw2MotionOutput* out_motion);

void egl_sw2_vibration_encode_5bytes(const EglSw2VibrationPacketData* pkt, uint8_t out_bytes[5]);

void egl_sw2_vibration_from_status(float low_amplitude,
                                   float low_frequency,
                                   float high_amplitude,
                                   float high_frequency,
                                   EglSw2VibrationPacketData* out_pkt);

int32_t egl_sw2_build_command_packet(uint8_t command_id,
                                     uint8_t subcommand_id,
                                     const uint8_t* payload,
                                     size_t payload_len,
                                     uint8_t is_ble,
                                     EglSw2Bytes* out_bytes);

void egl_sw2_build_memory_read_payload(uint8_t length, uint32_t address, uint8_t out_payload[8]);

int32_t egl_sw2_build_vibration_report(uint8_t packet_id,
                                       uint8_t is_pro_controller,
                                       const EglSw2VibrationPacketData* motor1,
                                       const EglSw2VibrationPacketData* motor2,
                                       const EglSw2VibrationPacketData* motor3,
                                       EglSw2Bytes* out_bytes);

int32_t egl_sw2_build_usb_vibration_report(uint16_t pid,
                                           uint8_t sequence,
                                           const EglSw2VibrationPacketData* motor,
                                           EglSw2Bytes* out_bytes);

uint8_t egl_sw2_reverse_lower_bits4(uint8_t value);

int32_t egl_sw2_signed_looping_difference16(uint16_t previous, uint16_t current);

int32_t egl_sw2_gyro_mouse_process(EglSw2GyroMouseState* state,
                                   const int16_t* cal_samples_xyz,
                                   size_t cal_sample_count,
                                   const EglSw2InputReport* report,
                                   uint8_t controller_type,
                                   uint8_t horizontal_hold,
                                   uint8_t trigger_pressed,
                                   uint32_t mode,
                                   uint32_t activation_mode,
                                   float sensitivity,
                                   uint64_t now_ns,
                                   uint8_t* out_sample_action,
                                   EglSw2GyroMouseOutput* out_output);

int32_t egl_sw2_optical_mouse_process(EglSw2OpticalMouseState* state,
                                      const EglSw2InputReport* report,
                                      uint8_t controller_type,
                                      uint8_t enabled,
                                      EglSw2OpticalMouseOutput* out_output);

/// Idempotent: frees owned bytes and zeroes fields.
void egl_sw2_free_bytes(EglSw2Bytes* bytes);

#ifdef __cplusplus
} // extern "C"
#endif
