# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

include(${PROJECT_SOURCE_DIR}/CMakeModules/AddRustCrate.cmake)

add_rust_static_library(
    TARGET eglantine-android-frontend
    MANIFEST "${PROJECT_SOURCE_DIR}/rust/apps/eglantine_android/Cargo.toml"
    LIB_NAME eglantine_android
    OFFLINE
)

# The bundled Android SDL is static, but upstream SDLActivity loads libSDL3.so.
# Keep the full archive so its JNI entry points are retained in the shared library.
add_library(eglantine-android-sdl SHARED sdl_shared.c)
set_target_properties(eglantine-android-sdl PROPERTIES
    OUTPUT_NAME SDL3
    LINKER_LANGUAGE CXX
)
target_link_libraries(eglantine-android-sdl PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,SDL3::SDL3>"
    android dl EGL GLESv2 log m OpenSLES
)
target_include_directories(eglantine-android-sdl SYSTEM INTERFACE
    "$<TARGET_PROPERTY:SDL3::SDL3,INTERFACE_INCLUDE_DIRECTORIES>"
)

add_library(eglantine-android SHARED
    ${PROJECT_SOURCE_DIR}/src/common/android/ffmpeg.cpp
    core_session.cpp
    core_session.h
    document_vfs.cpp
    document_vfs.h
    main.cpp
    native_config.cpp
    native_config.h
)
set_target_properties(eglantine-android PROPERTIES OUTPUT_NAME main)
target_include_directories(eglantine-android PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/rust/apps/eglantine_android/include"
)
target_link_libraries(eglantine-android PRIVATE
    eglantine-android-sdl eglantine-android-frontend
    audio_core common core frontend_common input_common video_core android
)
if (ARCHITECTURE_arm64)
    target_link_libraries(eglantine-android PRIVATE adrenotools)
endif()
# Match the shipping Android link for the bundled FFmpeg assembly globals.
target_link_options(eglantine-android PRIVATE "-Wl,-Bsymbolic")
