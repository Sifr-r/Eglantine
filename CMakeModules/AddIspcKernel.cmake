# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

include_guard(GLOBAL)

find_program(ISPC_EXECUTABLE NAMES ispc DOC "Path to the Intel ISPC compiler")

if(ISPC_EXECUTABLE)
    set(EGLANTINE_HAS_ISPC TRUE)
else()
    set(EGLANTINE_HAS_ISPC FALSE)
endif()

# Compiles one or more `.ispc` sources into a multi-ISA static library and generates `*_ispc.h`
# headers for C/C++ consumers. When `ispc` is absent, warns once cleanly and creates an empty
# INTERFACE target without `EGLANTINE_ENABLE_ISPC`, preserving scalar C++ fallbacks.
function(add_ispc_kernel target)
    cmake_parse_arguments(
        ARG
        ""
        "HEADER_DIR"
        "SOURCES;TARGETS;INCLUDE_DIRECTORIES;DEFINITIONS;FLAGS"
        ${ARGN}
    )

    if(NOT ARG_SOURCES)
        message(FATAL_ERROR "add_ispc_kernel(${target}): SOURCES must not be empty")
    endif()

    if(NOT ISPC_EXECUTABLE)
        get_property(_warned GLOBAL PROPERTY EGLANTINE_ISPC_ABSENT_WARNED)
        if(NOT _warned)
            message(WARNING "ISPC compiler (ispc) not found; skipping ISPC SIMD kernels (${target}) and keeping scalar C++ fallbacks.")
            set_property(GLOBAL PROPERTY EGLANTINE_ISPC_ABSENT_WARNED TRUE)
        endif()
        if(NOT TARGET "${target}")
            add_library("${target}" INTERFACE)
        endif()
        set("${target}_FOUND" FALSE PARENT_SCOPE)
        return()
    endif()

    if(NOT ARG_HEADER_DIR)
        set(ARG_HEADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/include/simd_kernels")
    endif()

    if(NOT ARG_TARGETS)
        if(ARCHITECTURE_arm64 OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64|AARCH64)$")
            set(ARG_TARGETS "neon-i32x4")
        else()
            set(ARG_TARGETS "sse4-i32x4,avx2-i32x8,avx512skx-i32x16")
        endif()
    endif()

    string(REPLACE "," ";" _target_list "${ARG_TARGETS}")
    list(LENGTH _target_list _target_count)

    set(_isa_suffixes "")
    if(_target_count GREATER 1)
        foreach(_isa IN LISTS _target_list)
            string(REGEX REPLACE "-.*$" "" _family "${_isa}")
            list(APPEND _isa_suffixes "_${_family}")
        endforeach()
    endif()

    if(ARCHITECTURE_arm64 OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64|AARCH64)$")
        set(_ispc_arch "aarch64")
    else()
        set(_ispc_arch "x86-64")
    endif()

    set(_common_flags
        "--arch=${_ispc_arch}"
        "--target=${ARG_TARGETS}"
        "-O2"
        "--opt=fast-math"
        "-I${CMAKE_CURRENT_SOURCE_DIR}"
    )

    if(WIN32)
        list(APPEND _common_flags "--target-os=windows")
    elseif(APPLE)
        list(APPEND _common_flags "--target-os=macos" "--pic")
    else()
        list(APPEND _common_flags "--target-os=linux" "--pic")
    endif()

    foreach(_inc IN LISTS ARG_INCLUDE_DIRECTORIES)
        list(APPEND _common_flags "-I${_inc}")
    endforeach()
    foreach(_def IN LISTS ARG_DEFINITIONS)
        list(APPEND _common_flags "-D${_def}")
    endforeach()
    if(ARG_FLAGS)
        list(APPEND _common_flags ${ARG_FLAGS})
    endif()

    file(MAKE_DIRECTORY "${ARG_HEADER_DIR}")
    set(_obj_dir "${CMAKE_CURRENT_BINARY_DIR}/ispc_objs/${target}")
    file(MAKE_DIRECTORY "${_obj_dir}")

    set(_obj_ext "${CMAKE_C_OUTPUT_EXTENSION}")
    if(NOT _obj_ext)
        if(WIN32)
            set(_obj_ext ".obj")
        else()
            set(_obj_ext ".o")
        endif()
    endif()

    set(_all_objects "")
    set(_all_headers "")

    foreach(_src IN LISTS ARG_SOURCES)
        get_filename_component(_src_abs "${_src}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        get_filename_component(_stem "${_src}" NAME_WE)

        set(_out_header "${ARG_HEADER_DIR}/${_stem}_ispc.h")
        set(_out_obj "${_obj_dir}/${_stem}_ispc${_obj_ext}")
        set(_outputs "${_out_obj}")

        foreach(_suffix IN LISTS _isa_suffixes)
            list(APPEND _outputs "${_obj_dir}/${_stem}_ispc${_suffix}${_obj_ext}")
        endforeach()

        add_custom_command(
            OUTPUT ${_outputs} "${_out_header}"
            COMMAND "${ISPC_EXECUTABLE}" ${_common_flags} -h "${_out_header}" -o "${_out_obj}" "${_src_abs}"
            MAIN_DEPENDENCY "${_src_abs}"
            DEPENDS "${_src_abs}"
            COMMENT "Compiling ISPC kernel ${_stem}.ispc (${ARG_TARGETS})"
            VERBATIM
        )

        list(APPEND _all_objects ${_outputs})
        list(APPEND _all_headers "${_out_header}")
    endforeach()

    set_source_files_properties(${_all_objects} PROPERTIES
        EXTERNAL_OBJECT TRUE
        GENERATED TRUE
    )
    set_source_files_properties(${_all_headers} PROPERTIES
        GENERATED TRUE
        HEADER_FILE_ONLY TRUE
    )

    add_library("${target}" STATIC ${_all_objects} ${_all_headers})
    set_target_properties("${target}" PROPERTIES
        LINKER_LANGUAGE C
    )

    get_filename_component(_header_parent "${ARG_HEADER_DIR}" DIRECTORY)
    target_include_directories("${target}" PUBLIC "${_header_parent}" "${ARG_HEADER_DIR}")
    target_compile_definitions("${target}" INTERFACE EGLANTINE_ENABLE_ISPC=1)

    set("${target}_FOUND" TRUE PARENT_SCOPE)
endfunction()
