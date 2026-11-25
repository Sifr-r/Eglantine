// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#![deny(warnings)]

#[cfg(feature = "ampere-spirv")]
pub use eglantine_ampere_spirv::ffi::*;

#[repr(transparent)]
#[allow(dead_code)]
pub struct FfiSymbolAnchor(*const ());

// SAFETY: Function pointers are immutable process-wide code addresses.
unsafe impl Sync for FfiSymbolAnchor {}

/// Anchor every `#[no_mangle] pub extern "C"` symbol from enabled `rlib` workspace
/// crates so `rustc` retains and exports their C ABI entrypoints in `eglantine_rs`.
#[cfg(feature = "ampere-spirv")]
#[used]
#[no_mangle]
pub static EGLANTINE_EXPORTED_AMPERE_SPIRV_SYMBOLS: [FfiSymbolAnchor; 3] = [
    // eglantine_ampere_spirv.h (3)
    FfiSymbolAnchor(egl_ampere_spirv_compile as *const ()),
    FfiSymbolAnchor(egl_ampere_spirv_compile_batch as *const ()),
    FfiSymbolAnchor(egl_ampere_spirv_free as *const ()),
];
