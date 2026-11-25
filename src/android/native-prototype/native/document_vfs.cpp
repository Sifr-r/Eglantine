// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

#include <SDL3/SDL.h>
#include <jni.h>

#include "common/scope_exit.h"
#include "common/string_util.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "document_vfs.h"

namespace Eglantine::AndroidPrototype {
namespace {

// The loader interprets some path components such as "00". Document IDs are opaque.
constexpr std::string_view DocumentPath = "/__eglantine_android_document__/selected";
constexpr std::string_view ContentUriPrefix = "content://";
constexpr std::size_t MaxUriSize = 4095;

using Stream = std::unique_ptr<SDL_IOStream, decltype(&SDL_CloseIO)>;

bool ClearJavaException(JNIEnv& env) {
    if (!env.ExceptionCheck()) {
        return false;
    }
    env.ExceptionClear();
    return true;
}

std::string ReadDisplayName(JNIEnv& env, jobject cursor) {
    const jclass cursor_class = env.FindClass("android/database/Cursor");
    if (ClearJavaException(env) || cursor_class == nullptr) {
        return {};
    }
    const jmethodID close = env.GetMethodID(cursor_class, "close", "()V");
    if (ClearJavaException(env) || close == nullptr) {
        return {};
    }
    SCOPE_EXIT {
        env.CallVoidMethod(cursor, close);
        ClearJavaException(env);
    };

    const jmethodID move_to_first = env.GetMethodID(cursor_class, "moveToFirst", "()Z");
    if (ClearJavaException(env) || move_to_first == nullptr) {
        return {};
    }
    const jboolean has_row = env.CallBooleanMethod(cursor, move_to_first);
    if (ClearJavaException(env) || has_row != JNI_TRUE) {
        return {};
    }
    const jmethodID get_string = env.GetMethodID(cursor_class, "getString", "(I)Ljava/lang/String;");
    if (ClearJavaException(env) || get_string == nullptr) {
        return {};
    }
    const auto value = static_cast<jstring>(env.CallObjectMethod(cursor, get_string, 0));
    if (ClearJavaException(env) || value == nullptr) {
        return {};
    }
    const auto length = static_cast<std::size_t>(env.GetStringLength(value));
    if (length == 0 || length > MaxUriSize) {
        return {};
    }
    const jchar* data = env.GetStringChars(value, nullptr);
    if (ClearJavaException(env) || data == nullptr) {
        return {};
    }
    SCOPE_EXIT { env.ReleaseStringChars(value, data); };
    try {
        return Common::UTF16ToUTF8({reinterpret_cast<const char16_t*>(data), length});
    } catch (const std::range_error&) {
        return {};
    }
}

std::string QueryDisplayName(std::u16string_view uri) {
    auto* const env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (env == nullptr) {
        return {};
    }
    if (env->PushLocalFrame(16) != JNI_OK) {
        ClearJavaException(*env);
        return {};
    }
    SCOPE_EXIT { env->PopLocalFrame(nullptr); };

    const auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (ClearJavaException(*env) || activity == nullptr) {
        return {};
    }
    const jclass context_class = env->FindClass("android/content/Context");
    if (ClearJavaException(*env) || context_class == nullptr) {
        return {};
    }
    const jmethodID get_resolver = env->GetMethodID(
        context_class, "getContentResolver", "()Landroid/content/ContentResolver;");
    if (ClearJavaException(*env) || get_resolver == nullptr) {
        return {};
    }
    const jobject resolver = env->CallObjectMethod(activity, get_resolver);
    if (ClearJavaException(*env) || resolver == nullptr) {
        return {};
    }
    const jclass uri_class = env->FindClass("android/net/Uri");
    if (ClearJavaException(*env) || uri_class == nullptr) {
        return {};
    }
    const jmethodID parse_uri =
        env->GetStaticMethodID(uri_class, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    if (ClearJavaException(*env) || parse_uri == nullptr) {
        return {};
    }
    const jstring uri_string = env->NewString(reinterpret_cast<const jchar*>(uri.data()),
                                               static_cast<jsize>(uri.size()));
    if (ClearJavaException(*env) || uri_string == nullptr) {
        return {};
    }
    const jobject parsed_uri = env->CallStaticObjectMethod(uri_class, parse_uri, uri_string);
    if (ClearJavaException(*env) || parsed_uri == nullptr) {
        return {};
    }
    const jclass string_class = env->FindClass("java/lang/String");
    if (ClearJavaException(*env) || string_class == nullptr) {
        return {};
    }
    const jstring display_name_column = env->NewStringUTF("_display_name");
    if (ClearJavaException(*env) || display_name_column == nullptr) {
        return {};
    }
    const jobjectArray projection = env->NewObjectArray(1, string_class, display_name_column);
    if (ClearJavaException(*env) || projection == nullptr) {
        return {};
    }
    const jclass resolver_class = env->FindClass("android/content/ContentResolver");
    if (ClearJavaException(*env) || resolver_class == nullptr) {
        return {};
    }
    const jmethodID query = env->GetMethodID(
        resolver_class, "query",
        "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/"
        "String;)Landroid/database/Cursor;");
    if (ClearJavaException(*env) || query == nullptr) {
        return {};
    }
    const jobject cursor = env->CallObjectMethod(resolver, query, parsed_uri, projection,
                                                  static_cast<jobject>(nullptr),
                                                  static_cast<jobject>(nullptr),
                                                  static_cast<jobject>(nullptr));
    if (ClearJavaException(*env) || cursor == nullptr) {
        return {};
    }
    return ReadDisplayName(*env, cursor);
}

class DocumentFile final : public FileSys::VfsFile {
public:
    DocumentFile(Stream stream, std::string name, std::size_t size)
        : stream{std::move(stream)}, name{std::move(name)}, size{size} {}

    std::string GetName() const override {
        return name;
    }

    std::string GetFullPath() const override {
        return std::string{DocumentPath};
    }

    std::size_t GetSize() const override {
        return size;
    }

    bool Resize(std::size_t) override {
        return false;
    }

    FileSys::VirtualDir GetContainingDirectory() const override {
        return {};
    }

    bool IsWritable() const override {
        return false;
    }

    bool IsReadable() const override {
        return true;
    }

    std::size_t Read(u8* data, std::size_t length, std::size_t offset) const override {
        if (data == nullptr || length == 0 || offset >= size) {
            return 0;
        }
        const auto position = static_cast<Sint64>(offset);
        const auto count = std::min(length, size - offset);
        std::scoped_lock lock{stream_mutex};
        if (SDL_SeekIO(stream.get(), position, SDL_IO_SEEK_SET) != position) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Selected document seek failed: %s",
                         SDL_GetError());
            return 0;
        }
        const auto read = SDL_ReadIO(stream.get(), data, count);
        if (read < count && SDL_GetIOStatus(stream.get()) != SDL_IO_STATUS_EOF) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Selected document read failed: %s",
                         SDL_GetError());
        }
        return read;
    }

    std::size_t Write(const u8*, std::size_t, std::size_t) override {
        return 0;
    }

    bool Rename(std::string_view) override {
        return false;
    }

private:
    const Stream stream;
    const std::string name;
    const std::size_t size;
    mutable std::mutex stream_mutex;
};

class DocumentFilesystem final : public FileSys::RealVfsFilesystem {
public:
    explicit DocumentFilesystem(FileSys::VirtualFile document) : document{std::move(document)} {}

    FileSys::VfsEntryType GetEntryType(std::string_view path) const override {
        if (path == DocumentPath) {
            return FileSys::VfsEntryType::File;
        }
        return RealVfsFilesystem::GetEntryType(path);
    }

    FileSys::VirtualFile OpenFile(std::string_view path, FileSys::OpenMode permissions) override {
        if (path == DocumentPath) {
            return permissions == FileSys::OpenMode::Read ? document : nullptr;
        }
        return RealVfsFilesystem::OpenFile(path, permissions);
    }

private:
    const FileSys::VirtualFile document;
};

} // namespace

DocumentVfs OpenDocumentVfs(std::string_view uri) {
    if (!uri.starts_with(ContentUriPrefix) || uri.size() <= ContentUriPrefix.size() ||
        uri.size() > MaxUriSize || uri.find('\0') != std::string_view::npos ||
        std::string_view{"/?#"}.find(uri[ContentUriPrefix.size()]) != std::string_view::npos) {
        return {.error = "Select a valid Android document."};
    }
    std::u16string uri_utf16;
    try {
        uri_utf16 = Common::UTF8ToUTF16(uri);
    } catch (const std::range_error&) {
        return {.error = "The document URI is not valid UTF-8."};
    }
    const std::string uri_string{uri};
    Stream stream{SDL_IOFromFile(uri_string.c_str(), "rb"), SDL_CloseIO};
    if (!stream) {
        return {.error = "Could not open the selected document."};
    }
    const auto size = SDL_GetIOSize(stream.get());
    if (size < 0 || static_cast<Uint64>(size) > std::numeric_limits<std::size_t>::max() ||
        SDL_SeekIO(stream.get(), 0, SDL_IO_SEEK_SET) != 0) {
        return {.error = "The selected document does not support random access."};
    }

    auto name = QueryDisplayName(uri_utf16);
    std::replace_if(name.begin(), name.end(), [](unsigned char character) {
        return character < 0x20 || character == 0x7f || character == '/' || character == '\\';
    }, '_');
    if (name.empty() || name == "." || name == "..") {
        name = "selected";
    }
    auto file = std::make_shared<DocumentFile>(std::move(stream), std::move(name),
                                               static_cast<std::size_t>(size));
    return {.filesystem = std::make_shared<DocumentFilesystem>(std::move(file)),
            .path = std::string{DocumentPath}};
}

} // namespace Eglantine::AndroidPrototype
