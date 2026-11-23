// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![deny(warnings)]

pub use eglantine_ampere_rs::ffi::*;
pub use eglantine_ampere_spirv::ffi::*;
pub use eglantine_nup_fs::ffi::*;
pub use eglantine_switch2_io::ffi::*;

#[repr(transparent)]
#[allow(dead_code)]
pub struct FfiSymbolAnchor(*const ());

// SAFETY: Function pointers are immutable process-wide code addresses.
unsafe impl Sync for FfiSymbolAnchor {}

/// Anchor every `#[no_mangle] pub extern "C"` symbol from the `rlib` workspace
/// crates so `rustc` retains and exports all C ABI entrypoints in `eglantine_rs`.
#[used]
#[no_mangle]
pub static EGLANTINE_EXPORTED_FFI_SYMBOLS: [FfiSymbolAnchor; 57] = [
    // eglantine_ampere.h (11)
    FfiSymbolAnchor(egl_ampere_capture_init as *const ()),
    FfiSymbolAnchor(egl_ampere_capture_observe as *const ()),
    FfiSymbolAnchor(egl_ampere_native_program_key as *const ()),
    FfiSymbolAnchor(egl_ampere_has_native_address_map as *const ()),
    FfiSymbolAnchor(egl_ampere_native_program_address as *const ()),
    FfiSymbolAnchor(egl_ampere_override_source_size as *const ()),
    FfiSymbolAnchor(egl_ampere_encode_override as *const ()),
    FfiSymbolAnchor(egl_ampere_decode_override as *const ()),
    FfiSymbolAnchor(egl_ampere_lower_to_maxwell as *const ()),
    FfiSymbolAnchor(egl_ampere_free_lowered as *const ()),
    FfiSymbolAnchor(egl_ampere_free_bytes as *const ()),
    // eglantine_ampere_spirv.h (3)
    FfiSymbolAnchor(egl_ampere_spirv_compile as *const ()),
    FfiSymbolAnchor(egl_ampere_spirv_compile_batch as *const ()),
    FfiSymbolAnchor(egl_ampere_spirv_free as *const ()),
    // eglantine_nup_fs.h (18)
    FfiSymbolAnchor(egl_nup_is_nup_magic as *const ()),
    FfiSymbolAnchor(egl_nup_container_parse as *const ()),
    FfiSymbolAnchor(egl_nup_container_free as *const ()),
    FfiSymbolAnchor(egl_nup_read_extent as *const ()),
    FfiSymbolAnchor(egl_nup_verify_integrity as *const ()),
    FfiSymbolAnchor(egl_nup_is_loose_dump as *const ()),
    FfiSymbolAnchor(egl_nup_dump_root_from_main as *const ()),
    FfiSymbolAnchor(egl_nup_probe_metadata as *const ()),
    FfiSymbolAnchor(egl_nup_probe_nup_metadata as *const ()),
    FfiSymbolAnchor(egl_nup_default_archive_path as *const ()),
    FfiSymbolAnchor(egl_nup_default_extract_path as *const ()),
    FfiSymbolAnchor(egl_nup_has_resumable_journal as *const ()),
    FfiSymbolAnchor(egl_nup_pack_dump as *const ()),
    FfiSymbolAnchor(egl_nup_extract as *const ()),
    FfiSymbolAnchor(egl_nup_edit_metadata as *const ()),
    FfiSymbolAnchor(egl_nup_free_bytes as *const ()),
    FfiSymbolAnchor(egl_nup_free_string as *const ()),
    FfiSymbolAnchor(egl_nup_free_metadata as *const ()),
    // eglantine_switch2_io.h (25)
    FfiSymbolAnchor(egl_sw2_is_supported_product_id as *const ()),
    FfiSymbolAnchor(egl_sw2_product_id_to_controller_type as *const ()),
    FfiSymbolAnchor(egl_sw2_product_name as *const ()),
    FfiSymbolAnchor(egl_sw2_feature_mask_for_product as *const ()),
    FfiSymbolAnchor(egl_sw2_ble_uuid as *const ()),
    FfiSymbolAnchor(egl_sw2_strip_hid_report_id as *const ()),
    FfiSymbolAnchor(egl_sw2_parse_ble_manufacturer_data as *const ()),
    FfiSymbolAnchor(egl_sw2_unpack_stick_xy as *const ()),
    FfiSymbolAnchor(egl_sw2_parse_stick_calibration as *const ()),
    FfiSymbolAnchor(egl_sw2_parse_user_stick_calibration as *const ()),
    FfiSymbolAnchor(egl_sw2_apply_stick_calibration_axis as *const ()),
    FfiSymbolAnchor(egl_sw2_parse_controller_info as *const ()),
    FfiSymbolAnchor(egl_sw2_parse_input_report as *const ()),
    FfiSymbolAnchor(egl_sw2_convert_imu_to_motion as *const ()),
    FfiSymbolAnchor(egl_sw2_vibration_encode_5bytes as *const ()),
    FfiSymbolAnchor(egl_sw2_vibration_from_status as *const ()),
    FfiSymbolAnchor(egl_sw2_build_command_packet as *const ()),
    FfiSymbolAnchor(egl_sw2_build_memory_read_payload as *const ()),
    FfiSymbolAnchor(egl_sw2_build_vibration_report as *const ()),
    FfiSymbolAnchor(egl_sw2_build_usb_vibration_report as *const ()),
    FfiSymbolAnchor(egl_sw2_reverse_lower_bits4 as *const ()),
    FfiSymbolAnchor(egl_sw2_signed_looping_difference16 as *const ()),
    FfiSymbolAnchor(egl_sw2_gyro_mouse_process as *const ()),
    FfiSymbolAnchor(egl_sw2_optical_mouse_process as *const ()),
    FfiSymbolAnchor(egl_sw2_free_bytes as *const ()),
];
