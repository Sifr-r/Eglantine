// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <jni.h>

#include "common/scope_exit.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "frontend_common/game_list_metadata.h"
#include "document_vfs.h"

namespace Eglantine::AndroidPrototype {
namespace {

// The loader interprets some path components such as "00". Document IDs are opaque.
constexpr std::string_view DocumentPath = "/__eglantine_android_document__/selected";
constexpr std::string_view ContentUriPrefix = "content://";
constexpr std::size_t MaxUriSize = 4095;
constexpr unsigned int MaxTreeDepth = 8;
constexpr std::size_t MaxTreeEntries = 512;
constexpr jint FlagGrantReadUriPermission = 1;

using Stream = std::unique_ptr<SDL_IOStream, decltype(&SDL_CloseIO)>;

bool ClearJavaException(JNIEnv& env) {
    if (!env.ExceptionCheck()) {
        return false;
    }
    env.ExceptionClear();
    return true;
}

bool IsValidContentUri(std::string_view uri) {
    return uri.starts_with(ContentUriPrefix) && uri.size() > ContentUriPrefix.size() &&
           uri.size() <= MaxUriSize && uri.find('\0') == std::string_view::npos &&
           std::string_view{"/?#"}.find(uri[ContentUriPrefix.size()]) == std::string_view::npos;
}

std::string JStringToUtf8(JNIEnv& env, jstring value) {
    if (value == nullptr) {
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

jstring Utf8ToJString(JNIEnv& env, std::string_view text) {
    std::u16string utf16;
    try {
        utf16 = Common::UTF8ToUTF16(text);
    } catch (const std::range_error&) {
        return nullptr;
    }
    jstring result = env.NewString(reinterpret_cast<const jchar*>(utf16.data()),
                                   static_cast<jsize>(utf16.size()));
    return ClearJavaException(env) ? nullptr : result;
}

std::string SanitizeTitle(std::string name) {
    std::replace_if(name.begin(), name.end(), [](unsigned char ch) {
        return ch < 0x20 || ch == 0x7f || ch == '\t' || ch == '\r' || ch == '\n';
    }, ' ');
    return name;
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
    return JStringToUtf8(env, value);
}

std::string QueryDisplayName(std::string_view uri) {
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
    const jstring uri_string = Utf8ToJString(*env, uri);
    if (uri_string == nullptr) {
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

bool HasMatchingExtension(std::string_view display_name,
                          const std::vector<std::string>& extensions) {
    const auto dot = display_name.rfind('.');
    if (dot == std::string_view::npos || dot + 1 >= display_name.size()) {
        return false;
    }
    std::string ext;
    ext.reserve(display_name.size() - dot - 1);
    for (unsigned char ch : display_name.substr(dot + 1)) {
        ext.push_back(static_cast<char>(std::tolower(ch)));
    }
    return std::ranges::find(extensions, ext) != extensions.end();
}

bool NeedsBootableContainer(Loader::FileType type) {
    using enum Loader::FileType;
    return type == XCI || type == XCZ || type == NSP || type == NSZ || type == NUP;
}

DiscoveredGame InspectGameMetadata(Core::System& system, std::string uri,
                                   std::string fallback_name) {
    DiscoveredGame game{.uri = std::move(uri), .title = SanitizeTitle(std::move(fallback_name))};
    auto doc = OpenDocumentVfs(game.uri);
    if (!doc.filesystem) {
        return game;
    }
    system.SetFilesystem(doc.filesystem);
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system.GetFileSystemController().CreateFactories(*doc.filesystem);
    const auto file = doc.filesystem->OpenFile(doc.path, FileSys::OpenMode::Read);
    const auto loader = file ? Loader::GetLoader(system, file) : nullptr;
    if (!loader) {
        return game;
    }
    const auto type = loader->GetFileType();
    if (type == Loader::FileType::Unknown || type == Loader::FileType::Error ||
        (NeedsBootableContainer(type) && !Loader::IsBootableGameContainer(file, type))) {
        return game;
    }
    const auto metadata = GameListDiscovery::ReadTitleMetadata(*loader);
    if (metadata.title_valid && !metadata.title.empty() && metadata.title != " ") {
        game.title = SanitizeTitle(metadata.title);
    }
    u64 id{};
    if (loader->ReadProgramId(id) == Loader::ResultStatus::Success) {
        game.program_id = id;
    }
    return game;
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

std::string QueryDocumentDisplayName(std::string_view uri) {
    if (!IsValidContentUri(uri)) {
        return {};
    }
    return QueryDisplayName(uri);
}

bool TakePersistableReadPermission(std::string_view tree_uri) {
    if (!IsValidContentUri(tree_uri)) {
        return false;
    }
    auto* const env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (env == nullptr || env->PushLocalFrame(16) != JNI_OK) {
        if (env != nullptr) {
            ClearJavaException(*env);
        }
        return false;
    }
    SCOPE_EXIT { env->PopLocalFrame(nullptr); };

    const auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    const jclass context_class = env->FindClass("android/content/Context");
    const jclass uri_class = env->FindClass("android/net/Uri");
    const jclass resolver_class = env->FindClass("android/content/ContentResolver");
    if (ClearJavaException(*env) || !activity || !context_class || !uri_class || !resolver_class) {
        return false;
    }
    const jmethodID get_resolver = env->GetMethodID(
        context_class, "getContentResolver", "()Landroid/content/ContentResolver;");
    const jmethodID parse_uri =
        env->GetStaticMethodID(uri_class, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    const jmethodID take_perm = env->GetMethodID(
        resolver_class, "takePersistableUriPermission", "(Landroid/net/Uri;I)V");
    if (ClearJavaException(*env) || !get_resolver || !parse_uri || !take_perm) {
        return false;
    }
    const jobject resolver = env->CallObjectMethod(activity, get_resolver);
    const jstring juri = Utf8ToJString(*env, tree_uri);
    const jobject parsed_uri =
        juri ? env->CallStaticObjectMethod(uri_class, parse_uri, juri) : nullptr;
    if (ClearJavaException(*env) || !resolver || !parsed_uri) {
        return false;
    }
    env->CallVoidMethod(resolver, take_perm, parsed_uri, FlagGrantReadUriPermission);
    return !ClearJavaException(*env);
}

FolderScanResult ScanDocumentTree(std::string_view tree_uri) {
    if (!IsValidContentUri(tree_uri)) {
        return {.error = "Select a valid Android folder."};
    }
    auto* const env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (env == nullptr || env->PushLocalFrame(32) != JNI_OK) {
        if (env != nullptr) {
            ClearJavaException(*env);
        }
        return {.error = "Android JNI environment is unavailable."};
    }
    SCOPE_EXIT { env->PopLocalFrame(nullptr); };

    const auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    const jclass context_class = env->FindClass("android/content/Context");
    const jclass uri_class = env->FindClass("android/net/Uri");
    const jclass resolver_class = env->FindClass("android/content/ContentResolver");
    const jclass docs_class = env->FindClass("android/provider/DocumentsContract");
    const jclass cursor_class = env->FindClass("android/database/Cursor");
    const jclass string_class = env->FindClass("java/lang/String");
    if (ClearJavaException(*env) || !activity || !context_class || !uri_class ||
        !resolver_class || !docs_class || !cursor_class || !string_class) {
        return {.error = "Android document provider classes are unavailable."};
    }

    const jmethodID get_resolver = env->GetMethodID(
        context_class, "getContentResolver", "()Landroid/content/ContentResolver;");
    const jmethodID parse_uri =
        env->GetStaticMethodID(uri_class, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    const jmethodID uri_to_string =
        env->GetMethodID(uri_class, "toString", "()Ljava/lang/String;");
    const jmethodID query = env->GetMethodID(
        resolver_class, "query",
        "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/"
        "String;)Landroid/database/Cursor;");
    const jmethodID get_tree_doc_id = env->GetStaticMethodID(
        docs_class, "getTreeDocumentId", "(Landroid/net/Uri;)Ljava/lang/String;");
    const jmethodID build_children_uri = env->GetStaticMethodID(
        docs_class, "buildChildDocumentsUriUsingTree",
        "(Landroid/net/Uri;Ljava/lang/String;)Landroid/net/Uri;");
    const jmethodID build_doc_uri = env->GetStaticMethodID(
        docs_class, "buildDocumentUriUsingTree",
        "(Landroid/net/Uri;Ljava/lang/String;)Landroid/net/Uri;");
    const jmethodID move_to_next = env->GetMethodID(cursor_class, "moveToNext", "()Z");
    const jmethodID get_string =
        env->GetMethodID(cursor_class, "getString", "(I)Ljava/lang/String;");
    const jmethodID close_cursor = env->GetMethodID(cursor_class, "close", "()V");
    if (ClearJavaException(*env) || !get_resolver || !parse_uri || !uri_to_string || !query ||
        !get_tree_doc_id || !build_children_uri || !build_doc_uri || !move_to_next ||
        !get_string || !close_cursor) {
        return {.error = "Android document provider methods are unavailable."};
    }

    const jobject resolver = env->CallObjectMethod(activity, get_resolver);
    const jstring jtree_uri = Utf8ToJString(*env, tree_uri);
    const jobject parsed_tree =
        jtree_uri ? env->CallStaticObjectMethod(uri_class, parse_uri, jtree_uri) : nullptr;
    const auto root_doc_id = parsed_tree
                                 ? static_cast<jstring>(env->CallStaticObjectMethod(
                                       docs_class, get_tree_doc_id, parsed_tree))
                                 : nullptr;
    if (ClearJavaException(*env) || !resolver || !parsed_tree || !root_doc_id) {
        return {.error = "Could not open the selected Android folder."};
    }

    const jobjectArray projection = env->NewObjectArray(3, string_class, nullptr);
    const jstring col_doc_id = env->NewStringUTF("document_id");
    const jstring col_name = env->NewStringUTF("_display_name");
    const jstring col_mime = env->NewStringUTF("mime_type");
    if (ClearJavaException(*env) || !projection || !col_doc_id || !col_name || !col_mime) {
        return {.error = "Could not construct Android document query."};
    }
    env->SetObjectArrayElement(projection, 0, col_doc_id);
    env->SetObjectArrayElement(projection, 1, col_name);
    env->SetObjectArrayElement(projection, 2, col_mime);
    if (ClearJavaException(*env)) {
        return {.error = "Could not populate Android document query."};
    }

    const auto extensions = Loader::GetFileExtensions(Loader::FileExtensionUse::AndroidGame);
    std::vector<std::pair<std::string, unsigned int>> pending;
    pending.emplace_back(JStringToUtf8(*env, root_doc_id), 0U);
    if (pending.back().first.empty()) {
        return {.error = "The selected folder has no root document ID."};
    }

    struct RawCandidate {
        std::string uri;
        std::string name;
    };
    std::vector<RawCandidate> candidates;
    std::size_t visited = 0;

    while (!pending.empty() && visited < MaxTreeEntries) {
        const auto [doc_id, depth] = std::move(pending.back());
        pending.pop_back();
        if (env->PushLocalFrame(16) != JNI_OK) {
            ClearJavaException(*env);
            continue;
        }
        SCOPE_EXIT { env->PopLocalFrame(nullptr); };

        const jstring jdoc_id = Utf8ToJString(*env, doc_id);
        const jobject children_uri =
            jdoc_id ? env->CallStaticObjectMethod(docs_class, build_children_uri, parsed_tree,
                                                  jdoc_id)
                    : nullptr;
        const jobject cursor =
            children_uri
                ? env->CallObjectMethod(resolver, query, children_uri, projection,
                                        static_cast<jobject>(nullptr), static_cast<jobject>(nullptr),
                                        static_cast<jobject>(nullptr))
                : nullptr;
        if (ClearJavaException(*env) || cursor == nullptr) {
            continue;
        }
        SCOPE_EXIT {
            env->CallVoidMethod(cursor, close_cursor);
            ClearJavaException(*env);
        };

        while (visited < MaxTreeEntries) {
            const jboolean has_next = env->CallBooleanMethod(cursor, move_to_next);
            if (ClearJavaException(*env) || has_next != JNI_TRUE) {
                break;
            }
            if (env->PushLocalFrame(8) != JNI_OK) {
                ClearJavaException(*env);
                break;
            }
            SCOPE_EXIT { env->PopLocalFrame(nullptr); };

            const auto child_id =
                JStringToUtf8(*env, static_cast<jstring>(env->CallObjectMethod(cursor, get_string, 0)));
            const auto child_name =
                JStringToUtf8(*env, static_cast<jstring>(env->CallObjectMethod(cursor, get_string, 1)));
            const auto child_mime =
                JStringToUtf8(*env, static_cast<jstring>(env->CallObjectMethod(cursor, get_string, 2)));
            if (ClearJavaException(*env) || child_id.empty()) {
                continue;
            }
            ++visited;
            if (child_mime == "vnd.android.document/directory") {
                if (depth < MaxTreeDepth) {
                    pending.emplace_back(child_id, depth + 1);
                }
                continue;
            }
            if (!HasMatchingExtension(child_name, extensions)) {
                continue;
            }
            const jstring jchild_id = Utf8ToJString(*env, child_id);
            const jobject child_uri_obj =
                jchild_id ? env->CallStaticObjectMethod(docs_class, build_doc_uri, parsed_tree,
                                                        jchild_id)
                          : nullptr;
            const auto child_uri =
                child_uri_obj
                    ? JStringToUtf8(*env, static_cast<jstring>(
                                              env->CallObjectMethod(child_uri_obj, uri_to_string)))
                    : std::string{};
            if (!ClearJavaException(*env) && IsValidContentUri(child_uri)) {
                candidates.push_back({.uri = child_uri, .name = child_name});
            }
        }
    }

    FolderScanResult result;
    if (!candidates.empty()) {
        Core::System metadata_system;
        result.games.reserve(candidates.size());
        for (auto& candidate : candidates) {
            result.games.push_back(InspectGameMetadata(metadata_system, std::move(candidate.uri),
                                                       std::move(candidate.name)));
        }
    }
    return result;
}

DocumentVfs OpenDocumentVfs(std::string_view uri) {
    if (!IsValidContentUri(uri)) {
        return {.error = "Select a valid Android document."};
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

    auto name = QueryDisplayName(uri);
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
