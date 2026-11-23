# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.31)

#[=======================================================================[
AddRustCrate
------------

Builds a Rust Cargo staticlib crate and exposes it as an IMPORTED CMake
STATIC library target with its C header include directory and required
platform native static libraries.

Usage:
  add_rust_static_library(
      TARGET <cmake-target-name>
      MANIFEST <path/to/Cargo.toml>
      LIB_NAME <crate-lib-name>
      [PACKAGE <cargo-package-name>]
      [INCLUDE_DIR <header-include-dir>]
      [TARGET_DIR <cargo-target-dir>]
      [OFFLINE]
  )
#]=======================================================================]

option(EGLANTINE_RUST_OFFLINE "Pass --offline --frozen to Cargo invocations" OFF)

function(add_rust_static_library)
    set(options OFFLINE)
    set(oneValueArgs TARGET MANIFEST LIB_NAME PACKAGE INCLUDE_DIR TARGET_DIR)
    cmake_parse_arguments(RUST "${options}" "${oneValueArgs}" "" ${ARGN})

    if(NOT RUST_TARGET)
        message(FATAL_ERROR "add_rust_static_library: TARGET is required")
    endif()
    if(NOT RUST_MANIFEST)
        message(FATAL_ERROR "add_rust_static_library: MANIFEST is required")
    endif()
    if(NOT RUST_LIB_NAME)
        message(FATAL_ERROR "add_rust_static_library: LIB_NAME is required")
    endif()

    find_program(CARGO_EXECUTABLE cargo REQUIRED)

    get_filename_component(_manifest_abs "${RUST_MANIFEST}" ABSOLUTE)
    get_filename_component(_manifest_dir "${_manifest_abs}" DIRECTORY)

    if(RUST_TARGET_DIR)
        get_filename_component(_cargo_target_dir "${RUST_TARGET_DIR}" ABSOLUTE)
    else()
        set(_cargo_target_dir "${CMAKE_BINARY_DIR}/rust-target")
    endif()

    if(MSVC)
        set(_lib_filename "${RUST_LIB_NAME}.lib")
    else()
        set(_lib_filename "lib${RUST_LIB_NAME}.a")
    endif()

    if(NOT DEFINED EGLANTINE_RUST_TARGET)
        set(EGLANTINE_RUST_TARGET "" CACHE STRING "Explicit Cargo --target triple override")
    endif()

    set(_cargo_triple "")
    if(NOT "${EGLANTINE_RUST_TARGET}" STREQUAL "")
        set(_cargo_triple "${EGLANTINE_RUST_TARGET}")
    elseif(MINGW)
        set(_use_default_mingw_triple TRUE)
        if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
            get_filename_component(_cargo_dir "${CARGO_EXECUTABLE}" DIRECTORY)
            find_program(RUSTC_EXECUTABLE rustc HINTS "${_cargo_dir}")
            if(RUSTC_EXECUTABLE)
                execute_process(
                    COMMAND "${RUSTC_EXECUTABLE}" -vV
                    OUTPUT_VARIABLE _rustc_vv
                    ERROR_QUIET
                    RESULT_VARIABLE _rustc_rc
                )
                if(_rustc_rc EQUAL 0 AND _rustc_vv MATCHES "host: ([^\r\n]+)")
                    string(STRIP "${CMAKE_MATCH_1}" _rustc_host)
                    # Native MSYS2 UCRT64/CLANG64 rustc targets MinGW directly without --target.
                    if(ARCHITECTURE_x86_64 AND _rustc_host MATCHES "^x86_64-pc-windows-(gnu|gnullvm)$")
                        set(_use_default_mingw_triple FALSE)
                    elseif(ARCHITECTURE_arm64 AND _rustc_host MATCHES "^aarch64-pc-windows-(gnu|gnullvm)$")
                        set(_use_default_mingw_triple FALSE)
                    endif()
                endif()
            endif()
        endif()
        if(_use_default_mingw_triple)
            if(ARCHITECTURE_x86_64)
                set(_cargo_triple "x86_64-pc-windows-gnu")
            elseif(ARCHITECTURE_arm64)
                set(_cargo_triple "aarch64-pc-windows-gnullvm")
            endif()
        endif()
    elseif(APPLE)
        if(ARCHITECTURE_arm64)
            set(_cargo_triple "aarch64-apple-darwin")
        elseif(ARCHITECTURE_x86_64)
            set(_cargo_triple "x86_64-apple-darwin")
        endif()
    endif()

    if(_cargo_triple)
        set(_triple_dir "${_cargo_target_dir}/${_cargo_triple}")
        set(_triple_args --target "${_cargo_triple}")
    else()
        set(_triple_dir "${_cargo_target_dir}")
        set(_triple_args "")
    endif()

    set(_lib_debug "${_triple_dir}/debug/${_lib_filename}")
    set(_lib_release "${_triple_dir}/release/${_lib_filename}")

    set(_cargo_args build --manifest-path "${_manifest_abs}")
    if(RUST_PACKAGE)
        list(APPEND _cargo_args -p "${RUST_PACKAGE}")
    endif()
    if(RUST_OFFLINE OR EGLANTINE_RUST_OFFLINE)
        list(APPEND _cargo_args --offline --frozen)
    endif()
    if(_triple_args)
        list(APPEND _cargo_args ${_triple_args})
    endif()

    get_property(_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(_is_multi_config)
        list(APPEND _cargo_args "$<$<NOT:$<CONFIG:Debug>>:--release>")
        set(_stamp_file "${_cargo_target_dir}/${RUST_TARGET}_$<CONFIG>.stamp")
        set(_byproduct_libs "$<$<CONFIG:Debug>:${_lib_debug}>$<$<NOT:$<CONFIG:Debug>>:${_lib_release}>")
        set(_comment_config "$<CONFIG>")
        set(_default_location "${_lib_release}")
    else()
        string(TOUPPER "${CMAKE_BUILD_TYPE}" _build_type_upper)
        if(_build_type_upper STREQUAL "DEBUG")
            set(_default_location "${_lib_debug}")
            set(_stamp_config "Debug")
        else()
            list(APPEND _cargo_args --release)
            set(_default_location "${_lib_release}")
            if(CMAKE_BUILD_TYPE)
                set(_stamp_config "${CMAKE_BUILD_TYPE}")
            else()
                set(_stamp_config "Release")
            endif()
        endif()
        set(_stamp_file "${_cargo_target_dir}/${RUST_TARGET}_${_stamp_config}.stamp")
        set(_byproduct_libs "${_default_location}")
        set(_comment_config "${_stamp_config}")
    endif()

    set(_env_args "CARGO_TARGET_DIR=${_cargo_target_dir}")
    if(MSVC)
        # Keep Rust CRT linkage aligned with CMake's CMAKE_MSVC_RUNTIME_LIBRARY (/MD vs /MT).
        set(_rustflags "$ENV{RUSTFLAGS}")
        if(CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "^MultiThreaded(\\$<.*>)?$")
            set(_crt_flag "-C target-feature=+crt-static")
        else()
            set(_crt_flag "-C target-feature=-crt-static")
        endif()
        if(_rustflags)
            set(_rustflags "${_rustflags} ${_crt_flag}")
        else()
            set(_rustflags "${_crt_flag}")
        endif()
        list(APPEND _env_args "RUSTFLAGS=${_rustflags}")
    endif()

    file(GLOB_RECURSE _rust_sources CONFIGURE_DEPENDS
        "${_manifest_dir}/Cargo.toml"
        "${_manifest_dir}/Cargo.lock"
        "${_manifest_dir}/src/*.rs"
        "${_manifest_dir}/crates/*/Cargo.toml"
        "${_manifest_dir}/crates/**/*.rs"
    )

    add_custom_command(
        OUTPUT "${_stamp_file}"
        BYPRODUCTS "${_byproduct_libs}"
        COMMAND "${CMAKE_COMMAND}" -E env ${_env_args} "${CARGO_EXECUTABLE}" ${_cargo_args}
        COMMAND "${CMAKE_COMMAND}" -E touch "${_stamp_file}"
        WORKING_DIRECTORY "${_manifest_dir}"
        DEPENDS ${_rust_sources}
        COMMENT "Building Rust static library ${RUST_TARGET} (${_comment_config})"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )

    set(_build_target "${RUST_TARGET}_cargo_build")
    add_custom_target("${_build_target}" DEPENDS "${_stamp_file}")

    add_library("${RUST_TARGET}" STATIC IMPORTED GLOBAL)
    add_dependencies("${RUST_TARGET}" "${_build_target}")

    set_target_properties("${RUST_TARGET}" PROPERTIES
        IMPORTED_LOCATION "${_default_location}"
        IMPORTED_LOCATION_RELEASE "${_lib_release}"
        IMPORTED_LOCATION_RELWITHDEBINFO "${_lib_release}"
        IMPORTED_LOCATION_MINSIZEREL "${_lib_release}"
        IMPORTED_LOCATION_DEBUG "${_lib_debug}"
    )

    if(RUST_INCLUDE_DIR)
        get_filename_component(_include_abs "${RUST_INCLUDE_DIR}" ABSOLUTE)
        set_target_properties("${RUST_TARGET}" PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${_include_abs}"
        )
    endif()

    if(WIN32)
        set(_native_libs kernel32 ntdll userenv ws2_32 dbghelp bcrypt advapi32)
    elseif(APPLE)
        set(_native_libs pthread dl m "-framework Security" "-framework CoreFoundation")
    else()
        set(_native_libs pthread dl m)
    endif()

    set_target_properties("${RUST_TARGET}" PROPERTIES
        INTERFACE_LINK_LIBRARIES "${_native_libs}"
    )
endfunction()
