// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <utility>

#include <SDL3/SDL.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <jni.h>
#include <sys/system_properties.h>

#ifdef ARCHITECTURE_arm64
#include <adrenotools/driver.h>
#endif

#include "common/android/android_settings.h"
#include "common/device_power_state.h"
#include "common/dynamic_library.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/crypto/key_manager.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "hid_core/hid_core.h"
#include "input_common/drivers/touch_screen.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/main.h"
#include "video_core/gpu.h"
#include "video_core/shader_notify.h"

#include "core_session.h"
#include "document_vfs.h"
#include "native_config.h"

extern "C" jint InitFFmpegOnLoad(JavaVM* vm);

namespace Eglantine::AndroidPrototype {
namespace {

using SurfaceReference = std::unique_ptr<ANativeWindow, decltype(&ANativeWindow_release)>;

struct Surface {
    SurfaceReference window{nullptr, ANativeWindow_release};
    int width{};
    int height{};

    [[nodiscard]] bool IsValid() const {
        return window && width > 0 && height > 0;
    }

    bool operator==(const Surface& other) const {
        return window.get() == other.window.get() && width == other.width && height == other.height;
    }
};

Surface GetSurface(SDL_Window* window) {
    if (!window) {
        return {};
    }
    const auto properties = SDL_GetWindowProperties(window);
    if (!SDL_LockProperties(properties)) {
        return {};
    }
    SCOPE_EXIT { SDL_UnlockProperties(properties); };
    auto* native_window = static_cast<ANativeWindow*>(SDL_GetPointerProperty(
        properties, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr));
    if (!native_window) {
        return {};
    }
    ANativeWindow_acquire(native_window);
    return {SurfaceReference{native_window, ANativeWindow_release},
            ANativeWindow_getWidth(native_window),
            ANativeWindow_getHeight(native_window)};
}

void UpdatePowerStatus() {
    int percentage{};
    const auto power = SDL_GetPowerInfo(nullptr, &percentage);
    if (power != SDL_POWERSTATE_ERROR && power != SDL_POWERSTATE_UNKNOWN) {
        Common::SetAndroidPowerStatus({percentage, power == SDL_POWERSTATE_CHARGING,
                                      power != SDL_POWERSTATE_NO_BATTERY});
    }
}

void ConfigureCpuBackend() {
#if defined(__aarch64__)
    char primary_abi[PROP_VALUE_MAX]{};
    __system_property_get("ro.product.cpu.abi", primary_abi);
    const std::string_view abi{primary_abi};
    if (abi == "x86_64" || abi == "x86") {
        // Android's ARM translation layer cannot act as the native guest execution backend.
        Settings::values.cpu_backend.SetValue(Settings::CpuBackend::Dynarmic);
        Settings::values.cpu_accuracy.SetValue(Settings::CpuAccuracy::Debugging);
        Settings::values.cpuopt_fastmem.SetValue(false);
        SDL_Log("Android prototype using Dynarmic without fastmem under ARM translation");
    }
#endif
}

class VulkanContext final : public Core::Frontend::GraphicsContext {
public:
    explicit VulkanContext(std::shared_ptr<Common::DynamicLibrary> library_)
        : library{std::move(library_)} {}

    std::shared_ptr<Common::DynamicLibrary> GetDriverLibrary() override {
        return library;
    }

private:
    std::shared_ptr<Common::DynamicLibrary> library;
};

class NativeWindow final : public Core::Frontend::EmuWindow {
public:
    NativeWindow(Surface surface_, std::shared_ptr<Common::DynamicLibrary> library_)
        : library{std::move(library_)} {
        window_info.type = Core::Frontend::WindowSystemType::Android;
        OnSurfaceChanged(std::move(surface_));
    }

    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return std::make_unique<VulkanContext>(library);
    }

    bool IsShown() const override {
        return visible.load(std::memory_order_relaxed);
    }

    void SetShown(bool shown) {
        visible.store(shown, std::memory_order_relaxed);
    }

    void OnSurfaceChanged(Surface new_surface) {
        std::scoped_lock lock{surface_mutex};
        surface = std::move(new_surface);
        window_info.render_surface = surface.IsValid() ? surface.window.get() : nullptr;
        if (surface.IsValid()) {
            UpdateCurrentFramebufferLayout(static_cast<u32>(surface.width),
                                           static_cast<u32>(surface.height));
        }
    }

    [[nodiscard]] bool Matches(const Surface& current) const {
        std::scoped_lock lock{surface_mutex};
        return surface == current;
    }

    [[nodiscard]] std::pair<float, float> MapTouch(float x, float y) const {
        std::scoped_lock lock{surface_mutex};
        if (surface.width <= 1 || surface.height <= 1) {
            return {0.0f, 0.0f};
        }
        const auto pixel_x = static_cast<u32>(std::clamp(x, 0.0f, 1.0f) *
                                              static_cast<float>(surface.width - 1));
        const auto pixel_y = static_cast<u32>(std::clamp(y, 0.0f, 1.0f) *
                                              static_cast<float>(surface.height - 1));
        return MapToTouchScreen(pixel_x, pixel_y);
    }

private:
    mutable std::mutex surface_mutex;
    Surface surface;
    std::shared_ptr<Common::DynamicLibrary> library;
    std::atomic<bool> visible{true};
};

std::string DescribeLoadError(Core::SystemResultStatus status) {
    using Core::SystemResultStatus;
    if (status == SystemResultStatus::ErrorGetLoader) {
        return "The selected document is not a recognized game file.";
    }
    if (status == SystemResultStatus::ErrorVideoCore) {
        return "The game renderer could not initialize on this device.";
    }
    const auto value = static_cast<u32>(status);
    if (const auto loader_base = static_cast<u32>(SystemResultStatus::ErrorLoader);
        value > loader_base) {
        return "Game load failed: " +
               Loader::GetResultStatusString(static_cast<Loader::ResultStatus>(value - loader_base));
    }
    return "Game load failed (core status " + std::to_string(value) + ").";
}

struct RunningGame {
    RunningGame(Surface surface, std::shared_ptr<Common::DynamicLibrary> library)
        : window{std::move(surface), std::move(library)} {}

    ~RunningGame() {
        window.SetShown(false);
        if (system.IsPoweredOn()) {
            system.DetachDebugger();
            system.ShutdownMainProcess();
            system.HIDCore().UnloadInputDevices();
        }
        Settings::RestoreGlobalState(false);
    }

    NativeWindow window;
    std::atomic<bool> exit_requested{};
    bool paused{};
    Core::System system;
};

#ifdef ARCHITECTURE_arm64
std::string QueryNativeLibraryDir() {
    auto* const env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (env == nullptr || env->PushLocalFrame(16) != JNI_OK) {
        if (env != nullptr && env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        return {};
    }
    SCOPE_EXIT { env->PopLocalFrame(nullptr); };

    const auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    const jclass context_class = env->FindClass("android/content/Context");
    const jclass app_info_class = env->FindClass("android/content/pm/ApplicationInfo");
    if (env->ExceptionCheck() || !activity || !context_class || !app_info_class) {
        env->ExceptionClear();
        return {};
    }
    const jmethodID get_app_info = env->GetMethodID(
        context_class, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
    const jfieldID native_lib_dir_field =
        env->GetFieldID(app_info_class, "nativeLibraryDir", "Ljava/lang/String;");
    if (env->ExceptionCheck() || !get_app_info || !native_lib_dir_field) {
        env->ExceptionClear();
        return {};
    }
    const jobject app_info = env->CallObjectMethod(activity, get_app_info);
    const auto jdir = app_info
                          ? static_cast<jstring>(env->GetObjectField(app_info, native_lib_dir_field))
                          : nullptr;
    if (env->ExceptionCheck() || !jdir) {
        env->ExceptionClear();
        return {};
    }
    const char* utf = env->GetStringUTFChars(jdir, nullptr);
    if (!utf) {
        return {};
    }
    std::string result{utf};
    env->ReleaseStringUTFChars(jdir, utf);
    if (!result.empty() && result.back() != '/') {
        result.push_back('/');
    }
    return result;
}
#endif

std::shared_ptr<Common::DynamicLibrary> OpenVulkanDriver() {
#ifdef ARCHITECTURE_arm64
    const std::string hook_lib_dir = QueryNativeLibraryDir();
    if (!hook_lib_dir.empty()) {
        void* handle = nullptr;
        const std::string& driver_path_str = AndroidSettings::values.driver_path.GetValue();
        if (!driver_path_str.empty()) {
            std::error_code ec;
            const std::filesystem::path driver_path{driver_path_str};
            if (std::filesystem::is_regular_file(driver_path, ec)) {
                std::string custom_dir = Common::FS::PathToUTF8String(driver_path.parent_path());
                if (!custom_dir.empty() && custom_dir.back() != '/') {
                    custom_dir.push_back('/');
                }
                const std::string custom_name = Common::FS::PathToUTF8String(driver_path.filename());
                handle = adrenotools_open_libvulkan(
                    RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr, hook_lib_dir.c_str(),
                    custom_dir.c_str(), custom_name.c_str(), nullptr, nullptr);
                if (!handle) {
                    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                                "Custom Vulkan driver failed to load (%s), falling back to system",
                                custom_name.c_str());
                }
            }
        }
        if (!handle) {
            handle = adrenotools_open_libvulkan(RTLD_NOW, 0, nullptr, hook_lib_dir.c_str(), nullptr,
                                                nullptr, nullptr, nullptr);
        }
        if (handle) {
            return std::make_shared<Common::DynamicLibrary>(handle);
        }
    }
#endif
    return std::make_shared<Common::DynamicLibrary>("libvulkan.so");
}

int ResolutionSetupToIndex(Settings::ResolutionSetup setup) {
    switch (setup) {
    case Settings::ResolutionSetup::Res1_2X:
        return 0;
    case Settings::ResolutionSetup::Res3_4X:
        return 1;
    case Settings::ResolutionSetup::Res1X:
        return 2;
    case Settings::ResolutionSetup::Res3_2X:
        return 3;
    case Settings::ResolutionSetup::Res2X:
        return 4;
    default:
        return 2;
    }
}

Settings::ResolutionSetup IndexToResolutionSetup(int index) {
    switch (index) {
    case 0:
        return Settings::ResolutionSetup::Res1_2X;
    case 1:
        return Settings::ResolutionSetup::Res3_4X;
    case 2:
        return Settings::ResolutionSetup::Res1X;
    case 3:
        return Settings::ResolutionSetup::Res3_2X;
    case 4:
        return Settings::ResolutionSetup::Res2X;
    default:
        return Settings::ResolutionSetup::Res1X;
    }
}

std::string CurrentDriverLabel() {
    const std::string& driver_path = AndroidSettings::values.driver_path.GetValue();
    if (driver_path.empty()) {
        return "System default";
    }
    const auto filename =
        Common::FS::PathToUTF8String(std::filesystem::path{driver_path}.filename());
    return filename.empty() ? "System default" : filename;
}

} // namespace

struct CoreSession::State {
    InputCommon::InputSubsystem input;
    std::unique_ptr<NativeAndroidConfig> config;
    std::optional<RunningGame> game;
    std::string user_data_dir;
    bool initialized{};
};

CoreSession::CoreSession() : state{std::make_unique<State>()} {}

CoreSession::~CoreSession() {
    Stop();
    if (state->initialized) {
        state->input.Shutdown();
        Common::Log::Stop();
    }
}

std::string CoreSession::EnsureStorageReady() {
    if (state->initialized) {
        return {};
    }
    const char* storage = SDL_GetAndroidExternalStoragePath();
    if (!storage || *storage == '\0') {
        storage = SDL_GetAndroidInternalStoragePath();
    }
    if (!storage || *storage == '\0') {
        return "Android did not provide a storage directory.";
    }
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    JavaVM* vm{};
    if (!env || env->GetJavaVM(&vm) != JNI_OK || InitFFmpegOnLoad(vm) != 0) {
        return "Android media services could not initialize.";
    }
    Common::FS::SetAppDirectory(storage);
    Common::FS::CreateEdenPaths();
    state->config = std::make_unique<NativeAndroidConfig>();
    Common::Log::Initialize();
    Common::Log::Start();
    Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Vulkan);
    ConfigureCpuBackend();
    Settings::values.udp_input_servers.SetValue({});
    state->input.Initialize();
    Core::Crypto::KeyManager::Instance().ReloadKeys();
    state->user_data_dir =
        Common::FS::PathToUTF8String(Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir));
    state->initialized = true;
    return {};
}

std::string CoreSession::GetUserDataDir() const {
    return state->user_data_dir;
}

bool CoreSession::HasProdKeys() const {
    return state->initialized &&
           Common::FS::Exists(Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir) /
                              "prod.keys");
}

AndroidSettingsSnapshot CoreSession::GetSettingsSnapshot() const {
    return {
        .use_docked_mode =
            Settings::values.use_docked_mode.GetValue() == Settings::ConsoleMode::Docked,
        .async_gpu = Settings::values.use_asynchronous_gpu_emulation.GetValue(),
        .disk_shader_cache = Settings::values.use_disk_shader_cache.GetValue(),
        .use_vsync = Settings::values.vsync_mode.GetValue() != Settings::VSyncMode::Immediate,
        .resolution_index = ResolutionSetupToIndex(Settings::values.resolution_setup.GetValue()),
        .gpu_accuracy = Settings::values.gpu_accuracy.GetValue() == Settings::GpuAccuracy::High ? 1 : 0,
        .driver_name = CurrentDriverLabel(),
    };
}

void CoreSession::ApplySettingsSnapshot(const AndroidSettingsSnapshot& snapshot) {
    Settings::values.use_docked_mode.SetValue(
        snapshot.use_docked_mode ? Settings::ConsoleMode::Docked : Settings::ConsoleMode::Handheld);
    Settings::values.use_asynchronous_gpu_emulation.SetValue(snapshot.async_gpu);
    Settings::values.use_disk_shader_cache.SetValue(snapshot.disk_shader_cache);
    Settings::values.vsync_mode.SetValue(snapshot.use_vsync ? Settings::VSyncMode::Fifo
                                                            : Settings::VSyncMode::Immediate);
    Settings::values.resolution_setup.SetValue(IndexToResolutionSetup(snapshot.resolution_index));
    Settings::values.gpu_accuracy.SetValue(snapshot.gpu_accuracy != 0 ? Settings::GpuAccuracy::High
                                                                  : Settings::GpuAccuracy::Low);
    if (state->config) {
        state->config->SaveAllValues();
    }
}

std::string CoreSession::InstallDriverDocument(std::string_view uri) {
    if (const auto init_err = EnsureStorageReady(); !init_err.empty()) {
        return init_err;
    }
    if (!uri.starts_with("content://") || uri.find('\0') != std::string_view::npos) {
        return "Select a valid Vulkan driver (.so) document.";
    }
    auto name = QueryDocumentDisplayName(uri);
    std::string clean_name;
    clean_name.reserve(name.size());
    for (unsigned char ch : name) {
        if (ch < 0x20 || ch == 0x7f || ch == '/' || ch == '\\') {
            return "The selected driver filename is invalid.";
        }
        clean_name.push_back(static_cast<char>(ch));
    }
    std::string lower_name = clean_name;
    std::ranges::transform(lower_name, lower_name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!lower_name.ends_with(".so") || lower_name == ".so") {
        return "Select an extracted Vulkan driver .so file.";
    }

    const std::string uri_string{uri};
    std::unique_ptr<SDL_IOStream, decltype(&SDL_CloseIO)> stream{
        SDL_IOFromFile(uri_string.c_str(), "rb"), SDL_CloseIO};
    if (!stream) {
        return "Could not open the selected driver file.";
    }
    constexpr Sint64 MaxDriverBytes = 64 * 1024 * 1024;
    const Sint64 size = SDL_GetIOSize(stream.get());
    if (size <= 0 || size > MaxDriverBytes) {
        return "The selected driver file is empty or too large.";
    }

    const auto drivers_dir =
        Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "gpu" / "drivers";
    std::error_code ec;
    std::filesystem::create_directories(drivers_dir, ec);
    if (ec) {
        return "Could not access the GPU drivers directory.";
    }
    const auto dest_path = drivers_dir / clean_name;
    const auto temp_path = drivers_dir / (clean_name + ".tmp");
    {
        std::ofstream out{temp_path, std::ios::binary | std::ios::trunc};
        if (!out) {
            return "Could not write the driver file.";
        }
        std::array<char, 8192> buffer{};
        Sint64 total = 0;
        while (total < size) {
            const auto to_read =
                static_cast<std::size_t>(std::min<Sint64>(buffer.size(), size - total));
            const auto read = SDL_ReadIO(stream.get(), buffer.data(), to_read);
            if (read == 0) {
                break;
            }
            out.write(buffer.data(), static_cast<std::streamsize>(read));
            if (!out) {
                std::filesystem::remove(temp_path, ec);
                return "Failed while writing the driver file.";
            }
            total += static_cast<Sint64>(read);
        }
        out.flush();
        if (!out || total != size) {
            std::filesystem::remove(temp_path, ec);
            return "Failed while copying the driver file.";
        }
    }
    std::filesystem::rename(temp_path, dest_path, ec);
    if (ec) {
        std::filesystem::remove(temp_path, ec);
        return "Could not finalize the driver file.";
    }
    AndroidSettings::values.driver_path.SetValue(Common::FS::PathToUTF8String(dest_path));
    if (state->config) {
        state->config->SaveAllValues();
    }
    return {};
}

void CoreSession::ClearCustomDriver() {
    AndroidSettings::values.driver_path.SetValue("");
    if (state->config) {
        state->config->SaveAllValues();
    }
}

std::string CoreSession::InstallKeysDocument(std::string_view uri) {
    if (const auto init_err = EnsureStorageReady(); !init_err.empty()) {
        return init_err;
    }
    if (!uri.starts_with("content://") || uri.find('\0') != std::string_view::npos) {
        return "Select a valid keys document.";
    }
    auto name = QueryDocumentDisplayName(uri);
    std::string lower_name;
    lower_name.reserve(name.size());
    for (unsigned char ch : name) {
        if (ch < 0x20 || ch == 0x7f || ch == '/' || ch == '\\') {
            return "The selected key filename is invalid.";
        }
        lower_name.push_back(static_cast<char>(std::tolower(ch)));
    }
    if (!lower_name.ends_with(".keys") || lower_name == ".keys") {
        return "Select a .keys file (such as prod.keys).";
    }

    const std::string uri_string{uri};
    std::unique_ptr<SDL_IOStream, decltype(&SDL_CloseIO)> stream{
        SDL_IOFromFile(uri_string.c_str(), "rb"), SDL_CloseIO};
    if (!stream) {
        return "Could not open the selected keys file.";
    }
    constexpr Sint64 MaxKeysBytes = 1024 * 1024;
    const Sint64 size = SDL_GetIOSize(stream.get());
    if (size <= 0 || size > MaxKeysBytes) {
        return "The selected keys file is empty or too large.";
    }

    const auto keys_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::KeysDir);
    std::error_code ec;
    std::filesystem::create_directories(keys_dir, ec);
    if (ec) {
        return "Could not access the keys directory.";
    }
    const auto dest_path = keys_dir / lower_name;
    const auto temp_path = keys_dir / (lower_name + ".tmp");
    {
        std::ofstream out{temp_path, std::ios::binary | std::ios::trunc};
        if (!out) {
            return "Could not write the keys file.";
        }
        std::array<char, 8192> buffer{};
        Sint64 total = 0;
        while (total < size) {
            const auto to_read =
                static_cast<std::size_t>(std::min<Sint64>(buffer.size(), size - total));
            const auto read = SDL_ReadIO(stream.get(), buffer.data(), to_read);
            if (read == 0) {
                break;
            }
            out.write(buffer.data(), static_cast<std::streamsize>(read));
            if (!out) {
                std::filesystem::remove(temp_path, ec);
                return "Failed while writing the keys file.";
            }
            total += static_cast<Sint64>(read);
        }
        out.flush();
        if (!out || total != size) {
            std::filesystem::remove(temp_path, ec);
            return "Failed while copying the keys file.";
        }
    }
    std::filesystem::rename(temp_path, dest_path, ec);
    if (ec) {
        std::filesystem::remove(temp_path, ec);
        return "Could not finalize the keys file.";
    }
    Core::Crypto::KeyManager::Instance().ReloadKeys();
    return {};
}

std::string CoreSession::Launch(SDL_Window* window, std::string_view uri) {
    if (state->game) {
        return "An emulation session is already open.";
    }
    try {
        auto document = OpenDocumentVfs(uri);
        if (!document.filesystem) {
            return document.error;
        }
        if (const auto init_err = EnsureStorageReady(); !init_err.empty()) {
            return init_err;
        }
        auto vulkan = OpenVulkanDriver();
        if (!vulkan->IsOpen()) {
            return "The Android Vulkan driver is unavailable.";
        }

        UpdatePowerStatus();
        auto surface = GetSurface(window);
        if (!surface.IsValid()) {
            return "Android did not provide a valid game surface.";
        }
        auto& game = state->game.emplace(std::move(surface), std::move(vulkan));
        game.system.SetFilesystem(std::move(document.filesystem));
        game.system.Initialize();
        game.system.ApplySettings();
        game.system.HIDCore().ReloadInputDevices();
        game.system.GetFileSystemController().CreateFactories(*game.system.GetFilesystem());

        Service::AM::FrontendAppletParameters parameters{
            .applet_id = Service::AM::AppletId::Application,
            .applet_type = Service::AM::AppletType::Application,
            .launch_type = Service::AM::LaunchType::FrontendInitiated,
        };
        const auto load = game.system.Load(game.window, document.path, parameters);
        if (load != Core::SystemResultStatus::Success) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Android prototype core load failed (%u)",
                        static_cast<unsigned int>(load));
            state->game.reset();
            return DescribeLoadError(load);
        }
        const auto request_exit = [exit = &game.exit_requested] {
            exit->store(true, std::memory_order_relaxed);
        };
        game.system.RegisterExitCallback([request_exit] { request_exit(); });
        game.system.RegisterExecuteProgramCallback([request_exit](std::size_t) { request_exit(); });
        game.system.GPU().Start();
        game.system.GetCpuManager().OnGpuReady();
        game.system.Run();
        SDL_Log("Android prototype core started");
        return {};
    } catch (const std::exception& error) {
        state->game.reset();
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Android prototype core startup failed: %s",
                     error.what());
        return "The emulation session could not start.";
    }
}

void CoreSession::Pause() {
    if (!state->game || state->game->paused) {
        return;
    }
    auto& game = *state->game;
    game.window.SetShown(false);
    state->input.GetTouchScreen()->ReleaseAllTouch();
    if (auto* vpad = state->input.GetVirtualGamepad()) {
        vpad->ResetControllers();
    }
    SDL_Log("Android prototype core pausing");
    game.system.Pause();
    game.window.OnSurfaceChanged({});
    game.system.GPU().WaitForIdle();
    game.paused = true;
    SDL_Log("Android prototype core paused");
}

std::string CoreSession::Resume(SDL_Window* window) {
    if (!state->game) {
        return {};
    }
    auto current = GetSurface(window);
    if (!current.IsValid()) {
        return {};
    }
    auto& game = *state->game;
    if (!game.window.Matches(current)) {
        game.window.OnSurfaceChanged(std::move(current));
    }
    if (game.paused) {
        UpdatePowerStatus();
        game.window.SetShown(true);
        game.system.Run();
        game.paused = false;
        SDL_Log("Android prototype core resumed");
    }
    return {};
}

void CoreSession::Stop() {
    if (state->game) {
        SDL_Log("Android prototype core stopping");
        state->input.GetTouchScreen()->ReleaseAllTouch();
        if (auto* vpad = state->input.GetVirtualGamepad()) {
            vpad->ResetControllers();
        }
        state->game.reset();
        SDL_Log("Android prototype core stopped");
    }
}

bool CoreSession::IsRunning() const {
    return state->game.has_value();
}

bool CoreSession::ExitRequested() const {
    return state->game && state->game->exit_requested.load(std::memory_order_relaxed);
}

void CoreSession::TouchDown(int id, float x, float y) {
    if (!state->game || state->game->paused || id < 0 || !std::isfinite(x) || !std::isfinite(y)) {
        return;
    }
    const auto [mapped_x, mapped_y] = state->game->window.MapTouch(x, y);
    state->input.GetTouchScreen()->TouchPressed(mapped_x, mapped_y, static_cast<std::size_t>(id));
}

void CoreSession::TouchMove(int id, float x, float y) {
    if (!state->game || state->game->paused || id < 0 || !std::isfinite(x) || !std::isfinite(y)) {
        return;
    }
    const auto [mapped_x, mapped_y] = state->game->window.MapTouch(x, y);
    state->input.GetTouchScreen()->TouchMoved(mapped_x, mapped_y, static_cast<std::size_t>(id));
}

void CoreSession::TouchUp(int id) {
    if (state->game && id >= 0) {
        state->input.GetTouchScreen()->TouchReleased(static_cast<std::size_t>(id));
    }
}

void CoreSession::SetVirtualButton(int button_id, bool pressed) {
    if (!state->game || state->game->paused) {
        return;
    }
    if (auto* vpad = state->input.GetVirtualGamepad()) {
        vpad->SetButtonState(0, button_id, pressed);
        vpad->SetButtonState(8, button_id, pressed);
    }
}

void CoreSession::SetVirtualStick(int stick_id, float x, float y) {
    if (!state->game || state->game->paused || !std::isfinite(x) || !std::isfinite(y)) {
        return;
    }
    const float clamped_x = std::clamp(x, -1.0f, 1.0f);
    const float clamped_y = std::clamp(y, -1.0f, 1.0f);
    if (auto* vpad = state->input.GetVirtualGamepad()) {
        vpad->SetStickPosition(0, stick_id, clamped_x, clamped_y);
        vpad->SetStickPosition(8, stick_id, clamped_x, clamped_y);
    }
}

void CoreSession::SetVirtualMotion(u64 delta_timestamp_us, float gyro_x, float gyro_y, float gyro_z,
                                   float accel_x, float accel_y, float accel_z) {
    if (!state->game || state->game->paused || !std::isfinite(gyro_x) ||
        !std::isfinite(gyro_y) || !std::isfinite(gyro_z) || !std::isfinite(accel_x) ||
        !std::isfinite(accel_y) || !std::isfinite(accel_z)) {
        return;
    }
    if (auto* vpad = state->input.GetVirtualGamepad()) {
        vpad->SetMotionState(0, delta_timestamp_us, gyro_x, gyro_y, gyro_z, accel_x, accel_y,
                             accel_z);
        vpad->SetMotionState(8, delta_timestamp_us, gyro_x, gyro_y, gyro_z, accel_x, accel_y,
                             accel_z);
    }
}

void CoreSession::ResetVirtualGamepad() {
    if (auto* vpad = state->input.GetVirtualGamepad()) {
        vpad->ResetControllers();
    }
}

double CoreSession::GetCurrentFps() {
    if (!state->game || state->game->paused) {
        return 0.0;
    }
    return state->game->system.GetAndResetPerfStats().average_game_fps;
}

int CoreSession::GetShadersBuilding() const {
    if (!state->game) {
        return 0;
    }
    return state->game->system.GPU().ShaderNotify().ShadersBuilding();
}

} // namespace Eglantine::AndroidPrototype

#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"
