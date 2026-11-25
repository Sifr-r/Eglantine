// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum EglAndroidAction {
    EGL_ANDROID_ACTION_NONE = 0,
    EGL_ANDROID_ACTION_OPEN_DOCUMENT = 1,
    EGL_ANDROID_ACTION_LAUNCH = 2,
    EGL_ANDROID_ACTION_ADD_FOLDER = 3,
    EGL_ANDROID_ACTION_INSTALL_KEYS = 4,
    EGL_ANDROID_ACTION_INSTALL_DRIVER = 5,
    EGL_ANDROID_ACTION_CLEAR_DRIVER = 6,
    EGL_ANDROID_ACTION_APPLY_SETTINGS = 7,
};

enum EglAndroidDrawKind {
    EGL_ANDROID_DRAW_FILL_RECT = 0,
    EGL_ANDROID_DRAW_TEXT = 1,
};

enum EglAndroidKey {
    EGL_ANDROID_KEY_CONFIRM = 1,
    EGL_ANDROID_KEY_LAUNCH = 2,
};

enum EglAndroidOverlayEventKind {
    EGL_ANDROID_OVERLAY_NONE = 0,
    EGL_ANDROID_OVERLAY_BUTTON = 1,
    EGL_ANDROID_OVERLAY_STICK = 2,
    EGL_ANDROID_OVERLAY_PAUSE_MENU = 3,
};

typedef struct EglAndroidOverlayEvent {
    int32_t kind;
    bool consumed;
    int32_t id;
    bool pressed;
    float x;
    float y;
} EglAndroidOverlayEvent;

typedef struct EglAndroidSettings {
    bool use_docked_mode;
    bool async_gpu;
    bool disk_shader_cache;
    bool use_vsync;
    int32_t resolution_index;
    int32_t gpu_accuracy;
} EglAndroidSettings;

typedef struct EglAndroidColor {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
} EglAndroidColor;

typedef struct EglAndroidDrawCmd {
    int32_t kind;
    float x;
    float y;
    float w;
    float h;
    EglAndroidColor color;
    const char* text;
} EglAndroidDrawCmd;

typedef struct EglAndroidGameEntry {
    const char* uri;
    const char* title;
    uint64_t program_id;
} EglAndroidGameEntry;

void* eglantine_android_create(void);
void eglantine_android_destroy(void* app);

void eglantine_android_viewport_size(int32_t* width, int32_t* height);

const char* eglantine_android_title(void);
const char* eglantine_android_instruction(void);
const char* eglantine_android_picker_label(void);
const char* eglantine_android_launch_label(void);
const char* eglantine_android_status(const void* app);

void eglantine_android_set_storage_dir(void* app, const char* path);
void eglantine_android_set_has_prod_keys(void* app, bool has_keys);
void eglantine_android_set_suspended(void* app, bool suspended);

bool eglantine_android_begin_selection(void* app);
void eglantine_android_select_result(void* app, int status, const char* detail);

bool eglantine_android_begin_folder_selection(void* app);
void eglantine_android_folder_scan_result(void* app, const char* tree_uri,
                                          const EglAndroidGameEntry* games, size_t game_count,
                                          const char* error);

bool eglantine_android_begin_keys_selection(void* app);
void eglantine_android_keys_install_result(void* app, const char* error, bool has_keys);

void eglantine_android_set_settings(void* app, EglAndroidSettings settings, const char* driver_name);
EglAndroidSettings eglantine_android_get_settings(const void* app);
bool eglantine_android_begin_driver_selection(void* app);
void eglantine_android_driver_install_result(void* app, const char* error, const char* driver_name);

size_t eglantine_android_game_count(const void* app);
bool eglantine_android_can_launch(const void* app);
const char* eglantine_android_begin_launch(void* app);
void eglantine_android_launch_result(void* app, const char* error);
void eglantine_android_session_ended(void* app, const char* error);

int32_t eglantine_android_pointer_up(void* app, float x, float y);
int32_t eglantine_android_key_down(void* app, int32_t key);
size_t eglantine_android_build_draw_list(void* app, const EglAndroidDrawCmd** out_cmds);

EglAndroidOverlayEvent eglantine_android_overlay_touch_down(void* app, int64_t finger_id,
                                                          float norm_x, float norm_y);
EglAndroidOverlayEvent eglantine_android_overlay_touch_move(void* app, int64_t finger_id,
                                                          float norm_x, float norm_y);
EglAndroidOverlayEvent eglantine_android_overlay_touch_up(void* app, int64_t finger_id);
bool eglantine_android_overlay_set_controller_active(void* app, bool active);
void eglantine_android_overlay_reset(void* app);

#ifdef __cplusplus
}
#endif
