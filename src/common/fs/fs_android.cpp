// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/android/android_common.h"
#include "common/android/id_cache.h"
#include "common/assert.h"
#include "common/fs/fs_android.h"
#include "common/string_util.h"

namespace Common::FS::Android {
namespace {

class ScopedLocalJString {
public:
    ScopedLocalJString(JNIEnv* env_, jstring ref_) noexcept : env{env_}, ref{ref_} {}
    ~ScopedLocalJString() {
        if (ref != nullptr && env != nullptr) {
            env->DeleteLocalRef(ref);
        }
    }

    ScopedLocalJString(const ScopedLocalJString&) = delete;
    ScopedLocalJString& operator=(const ScopedLocalJString&) = delete;

    [[nodiscard]] jstring get() const noexcept {
        return ref;
    }

    explicit operator bool() const noexcept {
        return ref != nullptr;
    }

private:
    JNIEnv* env;
    jstring ref;
};

std::string CallStringMethod(jmethodID method, const std::string& filepath) {
    if (method == nullptr) {
        return {};
    }
    auto env = Common::Android::GetEnvForThread();
    if (env == nullptr) {
        return {};
    }
    const ScopedLocalJString j_filepath{env, Common::Android::ToJString(env, filepath)};
    if (!j_filepath) {
        return {};
    }
    const ScopedLocalJString j_return{
        env, static_cast<jstring>(
                 env->CallStaticObjectMethod(native_library, method, j_filepath.get()))};
    if (!j_return) {
        return {};
    }
    return Common::Android::GetJString(env, j_return.get());
}

} // namespace

void RegisterCallbacks(JNIEnv* env, jclass clazz) {
    env->GetJavaVM(&g_jvm);
    native_library = clazz;

    s_get_parent_directory = env->GetStaticMethodID(native_library, "getParentDirectory",
                                                    "(Ljava/lang/String;)Ljava/lang/String;");
    s_get_filename = env->GetStaticMethodID(native_library, "getFilename",
                                            "(Ljava/lang/String;)Ljava/lang/String;");
    s_get_size = env->GetStaticMethodID(native_library, "getSize", "(Ljava/lang/String;)J");
    s_is_directory = env->GetStaticMethodID(native_library, "isDirectory", "(Ljava/lang/String;)Z");
    s_file_exists = env->GetStaticMethodID(native_library, "exists", "(Ljava/lang/String;)Z");
    s_open_content_uri = env->GetStaticMethodID(native_library, "openContentUri",
                                                "(Ljava/lang/String;Ljava/lang/String;)I");
}

void UnRegisterCallbacks() {
    s_get_parent_directory = nullptr;
    s_get_filename = nullptr;

    s_get_size = nullptr;
    s_is_directory = nullptr;
    s_file_exists = nullptr;

    s_open_content_uri = nullptr;
}

bool IsContentUri(const std::string& path) {
    constexpr std::string_view prefix = "content://";
    if (path.size() < prefix.size()) [[unlikely]] {
        return false;
    }

    return path.find(prefix) == 0;
}

s32 OpenContentUri(const std::string& filepath, OpenMode openmode) {
    if (s_open_content_uri == nullptr)
        return -1;

    const char* mode = "";
    switch (openmode) {
    case OpenMode::Read:
        mode = "r";
        break;
    default:
        UNIMPLEMENTED();
        return -1;
    }
    auto env = Common::Android::GetEnvForThread();
    if (env == nullptr) {
        return -1;
    }
    const ScopedLocalJString j_filepath{env, Common::Android::ToJString(env, filepath)};
    const ScopedLocalJString j_mode{env, Common::Android::ToJString(env, mode)};
    if (!j_filepath || !j_mode) {
        return -1;
    }
    return env->CallStaticIntMethod(native_library, s_open_content_uri, j_filepath.get(),
                                    j_mode.get());
}

u64 GetSize(const std::string& filepath) {
    if (s_get_size == nullptr) {
        return 0;
    }
    auto env = Common::Android::GetEnvForThread();
    if (env == nullptr) {
        return 0;
    }
    const ScopedLocalJString j_filepath{env, Common::Android::ToJString(env, filepath)};
    if (!j_filepath) {
        return 0;
    }
    return static_cast<u64>(
        env->CallStaticLongMethod(native_library, s_get_size, j_filepath.get()));
}

bool IsDirectory(const std::string& filepath) {
    if (s_is_directory == nullptr) {
        return false;
    }
    auto env = Common::Android::GetEnvForThread();
    if (env == nullptr) {
        return false;
    }
    const ScopedLocalJString j_filepath{env, Common::Android::ToJString(env, filepath)};
    if (!j_filepath) {
        return false;
    }
    return env->CallStaticBooleanMethod(native_library, s_is_directory, j_filepath.get()) !=
           JNI_FALSE;
}

bool Exists(const std::string& filepath) {
    if (s_file_exists == nullptr) {
        return false;
    }
    auto env = Common::Android::GetEnvForThread();
    if (env == nullptr) {
        return false;
    }
    const ScopedLocalJString j_filepath{env, Common::Android::ToJString(env, filepath)};
    if (!j_filepath) {
        return false;
    }
    return env->CallStaticBooleanMethod(native_library, s_file_exists, j_filepath.get()) !=
           JNI_FALSE;
}

std::string GetParentDirectory(const std::string& filepath) {
    return CallStringMethod(s_get_parent_directory, filepath);
}

std::string GetFilename(const std::string& filepath) {
    return CallStringMethod(s_get_filename, filepath);
}

} // namespace Common::FS::Android
