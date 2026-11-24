# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

include(AddIspcKernel)

add_ispc_kernel(simd_kernels
    HEADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/include/simd_kernels"
    SOURCES
        tex_etc2.ispc
        tex_astc_scan.ispc
        tex_swizzle.ispc
)

if(NOT TARGET eglantine::simd_kernels)
    add_library(eglantine::simd_kernels ALIAS simd_kernels)
endif()
