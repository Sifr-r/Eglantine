// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <optional>
#include <utility>

#include <SDL3/SDL.h>
#include <android/native_window.h>
#include <jni.h>
#include <sys/system_properties.h>

#include "common/device_power_state.h"
#include "common/dynamic_library.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "hid_core/hid_core.h"
#include "input_common/drivers/touch_screen.h"
#include "input_common/main.h"
#include "video_core/gpu.h"

#include "core_session.h"
#include "document_vfs.h"

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
        : surface{std::move(surface_)}, library{std::move(library_)} {
        window_info.type = Core::Frontend::WindowSystemType::Android;
        window_info.render_surface = surface.window.get();
        UpdateCurrentFramebufferLayout(static_cast<u32>(surface.width),
                                       static_cast<u32>(surface.height));
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

    [[nodiscard]] bool Matches(const Surface& current) const {
        return surface == current;
    }

    [[nodiscard]] std::pair<float, float> MapTouch(float x, float y) const {
        const auto pixel_x = static_cast<u32>(std::clamp(x, 0.0f, 1.0f) *
                                              static_cast<float>(surface.width - 1));
        const auto pixel_y = static_cast<u32>(std::clamp(y, 0.0f, 1.0f) *
                                              static_cast<float>(surface.height - 1));
        return MapToTouchScreen(pixel_x, pixel_y);
    }

private:
    const Surface surface;
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

} // namespace

struct CoreSession::State {
    InputCommon::InputSubsystem input;
    std::optional<RunningGame> game;
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

std::string CoreSession::Launch(SDL_Window* window, std::string_view uri) {
    if (state->game) {
        return "An emulation session is already open.";
    }
    try {
        auto document = OpenDocumentVfs(uri);
        if (!document.filesystem) {
            return document.error;
        }
        auto vulkan = std::make_shared<Common::DynamicLibrary>("libvulkan.so");
        if (!vulkan->IsOpen()) {
            return "The Android Vulkan driver is unavailable.";
        }

        if (!state->initialized) {
            const char* storage = SDL_GetAndroidInternalStoragePath();
            if (!storage || *storage == '\0') {
                return "Android did not provide an app-private storage directory.";
            }
            auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
            JavaVM* vm{};
            if (!env || env->GetJavaVM(&vm) != JNI_OK || InitFFmpegOnLoad(vm) != 0) {
                return "Android media services could not initialize.";
            }
            Common::FS::SetAppDirectory(storage);
            Common::FS::CreateEdenPaths();
            Common::Log::Initialize();
            Common::Log::Start();
            Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Vulkan);
            ConfigureCpuBackend();
            Settings::values.udp_input_servers.SetValue({});
            state->input.Initialize();
            state->initialized = true;
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
    SDL_Log("Android prototype core pausing");
    game.system.Pause();
    game.paused = true;
    SDL_Log("Android prototype core paused");
}

std::string CoreSession::Resume(SDL_Window* window) {
    if (!state->game) {
        return {};
    }
    auto& game = *state->game;
    if (!game.window.Matches(GetSurface(window))) {
        Pause();
        return "The Android game surface changed. Reopen the game to continue.";
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

} // namespace Eglantine::AndroidPrototype

#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"
