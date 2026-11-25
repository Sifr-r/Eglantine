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
add_library(eglantine-android-prototype-sdl SHARED sdl_shared.c)
set_target_properties(eglantine-android-prototype-sdl PROPERTIES
    OUTPUT_NAME SDL3
    LINKER_LANGUAGE CXX
)
target_link_libraries(eglantine-android-prototype-sdl PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,SDL3::SDL3>"
    android dl EGL GLESv2 log m OpenSLES
)
target_include_directories(eglantine-android-prototype-sdl SYSTEM INTERFACE
    "$<TARGET_PROPERTY:SDL3::SDL3,INTERFACE_INCLUDE_DIRECTORIES>"
)

add_library(eglantine-android-prototype SHARED
    ${PROJECT_SOURCE_DIR}/src/common/android/ffmpeg.cpp
    ${PROJECT_SOURCE_DIR}/src/android/app/src/main/jni/android_settings.cpp
    core_session.cpp
    core_session.h
    document_vfs.cpp
    document_vfs.h
    main.cpp
)
set_target_properties(eglantine-android-prototype PROPERTIES OUTPUT_NAME main)
target_include_directories(eglantine-android-prototype PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(eglantine-android-prototype PRIVATE
    eglantine-android-prototype-sdl eglantine-android-frontend
    audio_core common core frontend_common input_common video_core android
)
# Match the shipping Android link for the bundled FFmpeg assembly globals.
target_link_options(eglantine-android-prototype PRIVATE "-Wl,-Bsymbolic")
