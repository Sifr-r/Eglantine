// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <jni.h>

extern "C" {
#include <libavcodec/jni.h>

jint InitFFmpegOnLoad(JavaVM* vm) {
    return av_jni_set_java_vm(vm, nullptr);
}
}
