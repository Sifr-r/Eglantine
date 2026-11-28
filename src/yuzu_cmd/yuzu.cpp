// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <iterator>
#include <fstream>
#include <vector>
#include <cstdlib>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include "common/fs/fs_util.h"
#include "common/settings_enums.h"
#include "core/launch_params.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL_main.h>

#include <fmt/ostream.h>

#include "common/driver_shader_cache.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/nx2_sampler.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/cpu_manager.h"
#include "core/crypto/key_manager.h"
#include "core/file_sys/common_funcs.h"
#include "core/file_sys/nup_writer.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/am/frontend/applets.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "core/loader/loader.h"
#include "emulation/classic/cartridge.h"
#include "frontend_common/config.h"
#include "frontend_common/sentry_crash_reporter.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "input_common/drivers/keyboard.h"
#include "input_common/drivers/switch2_auto_pair.h"
#include "input_common/main.h"
#include "network/network.h"
#include "sdl_config.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"
#include "video_core/shader_warm/shader_warm_builder.h"
#ifdef EGLANTINE_CLASSIC_RUNTIME
#include "yuzu_cmd/classic_session.h"
#endif
#include "yuzu_cmd/emu_window/emu_window_sdl3.h"
#ifdef HAS_OPENGL
#include "yuzu_cmd/emu_window/emu_window_sdl3_gl.h"
#endif
#include "yuzu_cmd/emu_window/emu_window_sdl3_null.h"
#include "yuzu_cmd/emu_window/emu_window_sdl3_vk.h"
#include "yuzu_cmd/gpui_content.h"
#include "yuzu_cmd/gpui_controller.h"
#include "yuzu_cmd/gpui_library.h"
#include "yuzu_cmd/gpui_session.h"
#include "yuzu_cmd/software_keyboard_applet.h"
#ifdef YUZU_ENABLE_MCP
#include "mcp/server.h"
#include "yuzu_cmd/mcp_host_cli.h"
#endif

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

#ifdef _WIN32
// windows.h needs to be included before shellapi.h
#include <windows.h>
#include <shellapi.h>
#include "common/windows/timer_resolution.h"
#endif

#ifdef _WIN32
extern "C" {
// tells Nvidia and AMD drivers to use the dedicated GPU by default on laptops with switchable
// graphics
__declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

// EDEN_NX2_CRASH_DUMP=<dir>: on an unhandled exception (a real crash; fastmem faults are handled
// before this), write <dir>/eden-crash-<pid>.dmp. A debugger changes the timing enough to hide
// some crashes, so this catches them at full speed. dbghelp.dll is loaded only then.
// Prints a code address as module+offset (resolve with llvm-symbolizer and the module's PDB).
static void PrintCodeAddress(const char* label, const void* address) {
    HMODULE module{};
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module)) {
        GetModuleFileNameA(module, name, sizeof(name));
    }
    const char* base_name = std::strrchr(name, '\\');
    std::fprintf(stderr, "  %s %p %s+%#llx\n", label, address, base_name ? base_name + 1 : name,
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                                 reinterpret_cast<uintptr_t>(module)));
}

static LONG WINAPI WriteCrashDump(EXCEPTION_POINTERS* exception) {
    static std::atomic<bool> written{};
    const char* directory = std::getenv("EDEN_NX2_CRASH_DUMP");
    if (directory == nullptr || written.exchange(true)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // The faulting address and stack first: they survive even if the dump fails.
    const EXCEPTION_RECORD& record = *exception->ExceptionRecord;
    std::fprintf(stderr, "Unhandled exception %#lx on thread %lu, info %#llx %#llx\n",
                 record.ExceptionCode, GetCurrentThreadId(),
                 static_cast<unsigned long long>(record.NumberParameters > 0 ? record.ExceptionInformation[0] : 0),
                 static_cast<unsigned long long>(record.NumberParameters > 1 ? record.ExceptionInformation[1] : 0));
    PrintCodeAddress("at", record.ExceptionAddress);
    void* frames[48];
    const USHORT count = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        PrintCodeAddress("frame", frames[i]);
    }
    std::fflush(stderr);
    struct ExceptionInformation {
        DWORD thread_id;
        EXCEPTION_POINTERS* exception_pointers;
        BOOL client_pointers;
    };
    using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int, const ExceptionInformation*,
                                              const void*, const void*);
    const HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    const auto write = dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(
                                     GetProcAddress(dbghelp, "MiniDumpWriteDump"))
                               : nullptr;
    if (write == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    char path[MAX_PATH];
    std::snprintf(path, sizeof(path), "%s\\eden-crash-%lu.dmp", directory, GetCurrentProcessId());
    const HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        const ExceptionInformation information{GetCurrentThreadId(), exception, FALSE};
        // MiniDumpWithThreadInfo; the larger MiniDumpWithIndirectlyReferencedMemory failed on
        // Eden's address space and left an empty file.
        const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file, 0x1000,
                              &information, nullptr, nullptr);
        const DWORD error = ok ? 0 : GetLastError();
        CloseHandle(file);
        std::fprintf(stderr, ok ? "Crash dump written to %s\n" : "Crash dump to %s failed (%#lx)\n",
                     path, error);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static void OnStateChanged(const Network::RoomMember::State& state) {
    switch (state) {
    case Network::RoomMember::State::Idle:
        LOG_DEBUG(Network, "Network is idle");
        break;
    case Network::RoomMember::State::Joining:
        LOG_DEBUG(Network, "Connection sequence to room started");
        break;
    case Network::RoomMember::State::Joined:
        LOG_DEBUG(Network, "Successfully joined to the room");
        break;
    case Network::RoomMember::State::Moderator:
        LOG_DEBUG(Network, "Successfully joined the room as a moderator");
        break;
    default:
        break;
    }
}

static void OnNetworkError(const Network::RoomMember::Error& error) {
    switch (error) {
    case Network::RoomMember::Error::LostConnection:
        LOG_DEBUG(Network, "Lost connection to the room");
        break;
    case Network::RoomMember::Error::CouldNotConnect:
        LOG_ERROR(Network, "Error: Could not connect");
        exit(1);
        break;
    case Network::RoomMember::Error::NameCollision:
        LOG_ERROR(
            Network,
            "You tried to use the same nickname as another user that is connected to the Room");
        exit(1);
        break;
    case Network::RoomMember::Error::IpCollision:
        LOG_ERROR(Network, "You tried to use the same fake IP-Address as another user that is "
                           "connected to the Room");
        exit(1);
        break;
    case Network::RoomMember::Error::WrongPassword:
        LOG_ERROR(Network, "Room replied with: Wrong password");
        exit(1);
        break;
    case Network::RoomMember::Error::WrongVersion:
        LOG_ERROR(Network,
                  "You are using a different version than the room you are trying to connect to");
        exit(1);
        break;
    case Network::RoomMember::Error::RoomIsFull:
        LOG_ERROR(Network, "The room is full");
        exit(1);
        break;
    case Network::RoomMember::Error::HostKicked:
        LOG_ERROR(Network, "You have been kicked by the host");
        break;
    case Network::RoomMember::Error::HostBanned:
        LOG_ERROR(Network, "You have been banned by the host");
        break;
    case Network::RoomMember::Error::UnknownError:
        LOG_ERROR(Network, "UnknownError");
        break;
    case Network::RoomMember::Error::PermissionDenied:
        LOG_ERROR(Network, "PermissionDenied");
        break;
    case Network::RoomMember::Error::NoSuchUser:
        LOG_ERROR(Network, "NoSuchUser");
        break;
    }
}

static void OnMessageReceived(const Network::ChatEntry& msg) {
    std::cout << std::endl << msg.nickname << ": " << msg.message << std::endl << std::endl;
}

static void OnStatusMessageReceived(const Network::StatusMessageEntry& msg) {
    std::string message = [&]() {
        switch (msg.type) {
        case Network::IdMemberJoin:
            return fmt::format("{} has joined", msg.nickname);
        case Network::IdMemberLeave:
            return fmt::format("{} has left", msg.nickname);
        case Network::IdMemberKicked:
            return fmt::format("{} has been kicked", msg.nickname);
        case Network::IdMemberBanned:
            return fmt::format("{} has been banned", msg.nickname);
        case Network::IdAddressUnbanned:
            return fmt::format("{} has been unbanned", msg.nickname);
        default:
            return std::string{};
        }
    }();
    if (!message.empty())
        std::cout << std::endl << "* " << message << std::endl << std::endl;
}

struct SdlState {
    Frontend::SentryCrashReporter crash_reporter;
    Core::System system{};
    // Must outlive emu_window, which keeps a pointer to it.
    InputCommon::InputSubsystem input_subsystem{};
    std::unique_ptr<EmuWindow_SDL3> emu_window;
    std::shared_ptr<YuzuCmd::SoftwareKeyboardController> swkbd_controller;
    bool network_initialized{};
    std::unique_ptr<Gpui::Session> gpui_session;
#ifdef EGLANTINE_CLASSIC_RUNTIME
    std::unique_ptr<DesktopClassic::Session> classic_session;
#endif
    SDL_Scancode gpui_home_key{SDL_SCANCODE_F10};
    SDL_GamepadButton gpui_home_button{SDL_GAMEPAD_BUTTON_GUIDE};
    bool gpui_key_held{};
    std::optional<SDL_JoystickID> gpui_controller_held;
    bool gpui_mapped_home_held{};
    bool gpui_home_down{};
    bool gpui_home_announced{};
    bool gpui_start_error{};
    std::atomic_bool gpui_game_exit{};
    // The configuration of this session. It is owned by the state because SDL_AppMain returns
    // before SDL_AppIterate runs, so a local would already be destroyed when the Switch 2
    // auto-pair pass saves the bindings it makes.
    std::unique_ptr<SdlConfig> owned_config;
    // The configuration of this session, null until SDL_AppMain created it.
    SdlConfig* config{nullptr};
    // When the last Switch 2 auto-pair pass ran.
    std::chrono::steady_clock::time_point last_switch2_auto_pair{};
    // The Switch 2 auto-pair pass keeps the state of the controllers it saw: a controller is
    // only reported as gone after it was missing for several passes. The pass has to run on
    // the same object every time, a local would lose that state on every poll.
    InputCommon::Switch2::Switch2AutoPair switch2_auto_pair;
#ifdef YUZU_ENABLE_MCP
    // Built-in MCP server (only with --mcp): destroyed before the system shuts down.
    std::unique_ptr<CliMcpHost> mcp_host;
    std::unique_ptr<Mcp::Server> mcp_server;
#endif
};

constexpr int kRestartExitCode = 75;
static std::atomic_bool restart_requested{false};

// ExecuteProgram state for the restarted process: what the Qt frontend keeps in-process when it
// reboots a game. Only read when the launcher sets EDEN_CLI_RESTART_HANDOFF, so a leftover file
// never changes a normal launch.
struct RestartHandoff {
    u64 program_id{};
    s32 program_index{};
    s32 previous_program_index{-1};
    std::vector<std::vector<u8>> user_channel;
};

static std::filesystem::path RestartHandoffPath() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "restart-handoff.bin";
}

template <typename T>
static void PutRaw(std::ostream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

template <typename T>
static void GetRaw(std::istream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(value));
}

// Bounds keep a damaged handoff from requesting huge allocations; the writer refuses anything the
// reader would reject.
constexpr u32 kHandoffMaxEntries = 256;
constexpr u64 kHandoffMaxTotalSize = 64u << 20;

static bool WriteRestartHandoff(const RestartHandoff& handoff) {
    u64 total = 0;
    for (const auto& entry : handoff.user_channel) {
        total += entry.size();
    }
    if (handoff.user_channel.size() > kHandoffMaxEntries || total > kHandoffMaxTotalSize) {
        return false;
    }
    std::ofstream out{RestartHandoffPath(), std::ios::binary | std::ios::trunc};
    PutRaw(out, handoff.program_id);
    PutRaw(out, handoff.program_index);
    PutRaw(out, handoff.previous_program_index);
    PutRaw(out, static_cast<u32>(handoff.user_channel.size()));
    for (const auto& entry : handoff.user_channel) {
        PutRaw(out, static_cast<u32>(entry.size()));
        out.write(reinterpret_cast<const char*>(entry.data()), std::streamsize(entry.size()));
    }
    out.close();
    return !out.fail();
}

// Leaves the file in place; the caller removes it once the game has loaded.
static std::optional<RestartHandoff> ReadRestartHandoff() {
    std::ifstream in{RestartHandoffPath(), std::ios::binary};
    RestartHandoff handoff;
    u32 count{};
    GetRaw(in, handoff.program_id);
    GetRaw(in, handoff.program_index);
    GetRaw(in, handoff.previous_program_index);
    GetRaw(in, count);
    if (in.fail() || count > kHandoffMaxEntries) {
        return std::nullopt;
    }
    u64 total = 0;
    for (u32 i = 0; i < count; ++i) {
        u32 size{};
        GetRaw(in, size);
        total += size;
        if (in.fail() || total > kHandoffMaxTotalSize) {
            return std::nullopt;
        }
        auto& entry = handoff.user_channel.emplace_back(size);
        in.read(reinterpret_cast<char*>(entry.data()), std::streamsize(size));
        if (in.fail()) {
            return std::nullopt;
        }
    }
    return handoff;
}

static std::atomic_bool pack_nup_cancel{false};
static std::atomic_bool unpack_nup_cancel{false};
static std::atomic_bool edit_nup_cancel{false};

// eden-cli --pack-nup <dump> [<out.nup>]; the output defaults to <dump>.nup next to the dump.
static bool PackNup(const std::string& source, const std::string& output) {
    const auto to_path = [](const std::string& utf8) {
        return std::filesystem::path{std::u8string{utf8.begin(), utf8.end()}};
    };
    const auto dump_root = to_path(source);
    const auto archive =
        output.empty() ? FileSys::Nup::DefaultArchivePath(dump_root) : to_path(output);
    std::signal(SIGINT, [](int) { pack_nup_cancel = true; });

    const auto start = std::chrono::steady_clock::now();
    auto last_report = start;
    std::string error;
    const auto result = FileSys::Nup::PackDump(
        dump_root, archive, {},
        [&](const FileSys::Nup::PackProgress& p) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_report < std::chrono::seconds(1) && p.files_done != p.files_total) {
                return;
            }
            last_report = now;
            fmt::print(stderr, "\r{} / {} MiB, {} / {} files", p.bytes_done >> 20,
                       p.bytes_total >> 20, p.files_done, p.files_total);
        },
        &pack_nup_cancel, &error);
    const auto seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    fmt::print(stderr, "\n");
    switch (result) {
    case FileSys::Nup::PackResult::Success:
        fmt::print(stderr, "Wrote {} in {:.1f} s\n", Common::FS::PathToUTF8String(archive),
                   seconds);
        return true;
    case FileSys::Nup::PackResult::Cancelled:
        fmt::print(stderr, "Cancelled\n");
        return false;
    default:
        fmt::print(stderr, "Packing failed: {}\n", error);
        return false;
    }
}

// eden-cli --unpack-nup <file.nup> [<out_dir>]; the output defaults to <name>/ next to the NUP.
static bool UnpackNup(const std::string& source, const std::string& output) {
    const auto to_path = [](const std::string& utf8) {
        return std::filesystem::path{std::u8string{utf8.begin(), utf8.end()}};
    };
    const auto archive = to_path(source);
    const auto out_dir =
        output.empty() ? FileSys::Nup::DefaultExtractPath(archive) : to_path(output);
    std::signal(SIGINT, [](int) { unpack_nup_cancel = true; });

    const auto start = std::chrono::steady_clock::now();
    auto last_report = start;
    std::string error;
    const auto result = FileSys::Nup::ExtractNup(
        archive, out_dir,
        [&](const FileSys::Nup::PackProgress& p) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_report < std::chrono::seconds(1) && p.files_done != p.files_total) {
                return;
            }
            last_report = now;
            fmt::print(stderr, "\r{} / {} MiB, {} / {} files", p.bytes_done >> 20,
                       p.bytes_total >> 20, p.files_done, p.files_total);
        },
        &unpack_nup_cancel, &error);
    const auto seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    fmt::print(stderr, "\n");
    switch (result) {
    case FileSys::Nup::ExtractResult::Success:
        fmt::print(stderr, "Extracted to {} in {:.1f} s\n", Common::FS::PathToUTF8String(out_dir),
                   seconds);
        return true;
    case FileSys::Nup::ExtractResult::Cancelled:
        fmt::print(stderr, "Cancelled\n");
        return false;
    default:
        fmt::print(stderr, "Extraction failed: {}\n", error);
        return false;
    }
}

static bool EditNup(const Core::LaunchParams& lp) {
    const auto to_path = [](const std::string& utf8) {
        return std::filesystem::path{std::u8string{utf8.begin(), utf8.end()}};
    };
    const auto archive = to_path(*lp.edit_nup_source);
    FileSys::Nup::MetadataEdit edit;
    if (lp.edit_nup_name) {
        edit.title_name = *lp.edit_nup_name;
    }
    if (lp.edit_nup_publisher) {
        edit.publisher = *lp.edit_nup_publisher;
    }
    if (lp.edit_nup_version) {
        edit.display_version = *lp.edit_nup_version;
    }
    if (lp.edit_nup_icon) {
        if (lp.edit_nup_icon->empty()) {
            edit.icon = std::vector<u8>{};
        } else {
            const auto icon_path = to_path(*lp.edit_nup_icon);
            std::ifstream in(icon_path, std::ios::binary);
            if (!in) {
                fmt::print(stderr, "Cannot read icon {}\n",
                           Common::FS::PathToUTF8String(icon_path));
                return false;
            }
            edit.icon = std::vector<u8>((std::istreambuf_iterator<char>(in)), {});
            if (edit.icon->empty()) {
                fmt::print(stderr, "Icon file is empty\n");
                return false;
            }
        }
    }
    if (!edit.title_name && !edit.icon && !edit.display_version && !edit.publisher) {
        fmt::print(stderr, "Nothing to edit: pass --nup-name, --nup-icon, --nup-version or "
                           "--nup-publisher (or a --clear-* flag)\n");
        return false;
    }
    std::signal(SIGINT, [](int) { edit_nup_cancel = true; });
    std::string error;
    switch (FileSys::Nup::EditNupMetadata(archive, edit, &error, &edit_nup_cancel)) {
    case FileSys::Nup::EditResult::Success:
        fmt::print(stderr, "Updated {}\n", Common::FS::PathToUTF8String(archive));
        return true;
    case FileSys::Nup::EditResult::NoChange:
        fmt::print(stderr, "No changes to apply\n");
        return true;
    case FileSys::Nup::EditResult::Cancelled:
        fmt::print(stderr, "Cancelled\n");
        return false;
    default:
        fmt::print(stderr, "Edit failed: {}\n", error);
        return false;
    }
}

/// Hidden test hook (not in --help): builds a game's shader precache file without booting it.
///   yuzu-cmd --build-warm <game file> <title id hex> <output dir>
static bool BuildWarm(Core::System& system, int argc, char** argv, wchar_t** argv_w) {
    namespace ShaderWarm = VideoCommon::ShaderWarm;
    if (argc < 5) {
        fmt::print(stderr, "usage: --build-warm <game> <title id> <output dir>\n");
        return false;
    }
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    const u64 title_id = std::strtoull(argv[3], nullptr, 16);
#ifdef _WIN32
    const std::string game_arg =
        argv_w != nullptr && argv_w[2] != nullptr ? Common::UTF16ToUTF8(argv_w[2]) : argv[2];
    const std::string out_arg =
        argv_w != nullptr && argv_w[4] != nullptr ? Common::UTF16ToUTF8(argv_w[4]) : argv[4];
#else
    (void)argv_w;
    const std::string game_arg = argv[2];
    const std::string out_arg = argv[4];
#endif
    ShaderWarm::BuildOptions options;
    options.output_dir = Common::FS::ToU8String(out_arg);
    std::string last_stage;
    auto last_print = std::chrono::steady_clock::now();
    const auto begin = last_print;
    const auto on_progress = [&](const ShaderWarm::Progress& progress) {
        const auto now = std::chrono::steady_clock::now();
        if (progress.stage == last_stage && now - last_print < std::chrono::seconds{5}) {
            return;
        }
        last_stage = progress.stage;
        last_print = now;
        fmt::print("{:7.1f} s  {}: {} / {}\n",
                   std::chrono::duration<double>(now - begin).count(), progress.stage,
                   progress.done, progress.total);
        std::fflush(stdout);
    };
    const auto result = ShaderWarm::BuildWarmFilesWith(
        system, Common::FS::ToU8String(game_arg), title_id, options, on_progress, {});
    // The error can hold a Windows path: escape it so the summary stays valid JSON.
    std::string error;
    for (const char c : result.error) {
        switch (c) {
        case '"':
            error += "\\\"";
            break;
        case '\\':
            error += "\\\\";
            break;
        case '\n':
            error += "\\n";
            break;
        case '\r':
            error += "\\r";
            break;
        case '\t':
            error += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                error += fmt::format("\\u{:04x}", static_cast<unsigned>(c));
            } else {
                error += c;
            }
            break;
        }
    }
    fmt::print("{{\"ok\": {}, \"error\": \"{}\", \"archives\": {}, \"compute\": {}, "
               "\"skipped\": {}, \"seconds\": {:.1f}}}\n",
               result.ok, error, result.archives, result.compute_programs, result.skipped,
               std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count());
    std::fflush(stdout);
    return result.ok;
}

enum class GpuiTool { None, Library, Content, SwkbdTest, Controller };

static bool RunGpuiSwkbdTest(Gpui::Session& session, std::string_view scenario) {
    using namespace Service::AM::Frontend;
    auto controller = std::make_shared<YuzuCmd::SoftwareKeyboardController>();
    YuzuCmd::HostSoftwareKeyboardApplet applet{controller};

    struct ResultRecord {
        int final_completions{};
        int text_check_calls{};
        SwkbdResult last_result{SwkbdResult::Cancel};
        std::u16string last_text;
    } record;

    Core::Frontend::KeyboardInitializeParameters params{};
    params.header_text = u"Enter Name";
    params.sub_text = u"Profile";
    params.guide_text = u"Type here";
    params.ok_text = u"Done";
    params.initial_text = u"Églantine";
    params.min_text_length = 2;
    params.max_text_length = 16;
    params.password_mode = SwkbdPasswordMode::Disabled;
    params.enable_return_button = false;
    params.disable_cancel_button = false;

    const bool use_text_check = scenario == "text_check";
    if (use_text_check) {
        params.min_text_length = 3;
        params.max_text_length = 8;
    } else if (scenario == "disable_cancel") {
        params.disable_cancel_button = true;
    }

    applet.InitializeKeyboard(
        false, std::move(params),
        [&](SwkbdResult result, std::u16string text, bool confirmed) {
            if (use_text_check && result == SwkbdResult::Ok && !confirmed) {
                ++record.text_check_calls;
                if (text == u"bad") {
                    applet.ShowTextCheckDialog(SwkbdTextCheckResult::Failure, u"Name reserved");
                    return;
                }
                if (text == u"warn") {
                    applet.ShowTextCheckDialog(SwkbdTextCheckResult::Confirm, u"Use warn?");
                    return;
                }
            }
            ++record.final_completions;
            record.last_result = result;
            record.last_text = std::move(text);
            applet.ExitKeyboard();
        },
        {});

    session.SetState(Gpui::SessionState::Running);
    applet.ShowNormalKeyboard();
    controller->DrainGpuiEvents(session, nullptr);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (record.final_completions == 0 && std::chrono::steady_clock::now() < deadline) {
        std::vector<Gpui::Request> requests;
        if (!session.Poll(requests)) {
            applet.Close();
            break;
        }
        for (const auto& request : requests) {
            if (request.command == Gpui::Command::Stop) {
                session.SetState(Gpui::SessionState::Stopping);
                applet.Close();
                session.Reply(request.id);
                break;
            }
            if (request.command == Gpui::Command::SwkbdSubmit) {
                std::string error;
                if (!controller->Submit(request.swkbd_request_id, request.swkbd_text,
                                        request.swkbd_confirmed, error)) {
                    session.Reply(request.id, error);
                } else {
                    session.Reply(request.id);
                }
            } else if (request.command == Gpui::Command::SwkbdCancel) {
                std::string error;
                if (!controller->Cancel(request.swkbd_request_id, error)) {
                    session.Reply(request.id, error);
                } else {
                    session.Reply(request.id);
                }
            } else {
                session.Reply(request.id, "Unsupported command in swkbd test");
            }
        }
        controller->DrainGpuiEvents(session, nullptr);
        if (record.final_completions == 0) {
            SDL_Delay(5);
        }
    }

    // Calling Close() again after completion must never trigger a second completion.
    applet.Close();
    controller->DrainGpuiEvents(session, nullptr);

    const std::string result_utf8 = Common::UTF16ToUTF8(record.last_text);
    const std::string summary = fmt::format(
        "swkbd_test_done:{}:{}:{}:{}", record.final_completions, record.text_check_calls,
        record.last_result == SwkbdResult::Ok ? "ok" : "cancel", result_utf8);
    session.SendEvent(summary);
    session.SetState(Gpui::SessionState::Stopped);
    return record.final_completions == 1;
}

static bool RunGpuiTool(GpuiTool tool, Gpui::Session& session, Core::System& system, int argc,
                        char** argv, [[maybe_unused]] wchar_t** wide_argv) {
    if (tool == GpuiTool::SwkbdTest) {
        return RunGpuiSwkbdTest(session, argc >= 3 ? std::string_view{argv[2]} : "submit");
    }
    if (tool == GpuiTool::Controller) {
        return Gpui::RunControllerBridge(session);
    }
    const char* config = SDL_GetEnvironmentVariable(SDL_GetEnvironment(), "EGLANTINE_GPUI_CONFIG");
    if (config && *config && !SdlConfig::ReadToolValues(config)) {
        session.Error("The GPUI emulator configuration could not be read");
        return false;
    }
    const auto path_argument = [&](int index) {
#ifdef _WIN32
        if (wide_argv) {
            return Common::UTF16ToUTF8(wide_argv[index]);
        }
#endif
        return std::string{argv[index]};
    };

    if (tool == GpuiTool::Library) {
        const bool shallow = argc == 4 && std::string_view{argv[3]} == "--shallow";
        if ((argc != 3 && !shallow) || std::string_view{argv[2]}.empty()) {
            session.Error("A library folder is required for discovery");
            return false;
        }
        const auto directory = std::filesystem::path{Common::FS::ToU8String(path_argument(2))};
        return Gpui::ScanLibrary(session, system, directory, !shallow);
    }

    if (argc != 3 && argc != 4) {
        session.Error("A content operation is required");
        return false;
    }
    const auto error = Gpui::RunContentCommand(system, argv[2], argc == 4 ? path_argument(3) : "");
    if (!error.empty()) {
        session.Error(error);
        return false;
    }
    session.SendEvent("content_done");
    return session.IsConnected();
}

extern "C" SDL_AppResult SDL_AppInit(void **appstate, int argc, char **argv) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(WriteCrashDump);
    const char* crash_dump_directory = std::getenv("EDEN_NX2_CRASH_DUMP");
    const bool local_crash_dump = crash_dump_directory != nullptr && *crash_dump_directory != '\0';
#else
    constexpr bool local_crash_dump = false;
#endif
    Nx2Sampler::StartFromEnvironment();
    // SDL calls SDL_AppIterate back to back by default, which kept a host core at 100% polling
    // input; 240 Hz is plenty for input and the key script.
    SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "240");
    SdlState* state = new SdlState();
    *appstate = state;
    const std::string_view first_option = argc > 1 ? argv[1] : "";
    const bool gpui_requested = first_option == "--gpui-session";
    const auto gpui_tool = first_option == "--gpui-library"      ? GpuiTool::Library
                           : first_option == "--gpui-content"    ? GpuiTool::Content
                           : first_option == "--gpui-swkbd-test" ? GpuiTool::SwkbdTest
                           : first_option == "--gpui-controller" ? GpuiTool::Controller
                                                                 : GpuiTool::None;
    std::unique_ptr<Gpui::Session> gpui_tool_session;
    if (gpui_requested || gpui_tool != GpuiTool::None) {
        auto session = Gpui::Session::Open();
        if (!session) {
            std::fputs("GPUI session mode requires inherited stdin, stdout and stderr.\n", stderr);
            return SDL_APP_FAILURE;
        }
        if (gpui_requested) {
            state->gpui_session = std::move(session);
        } else {
            gpui_tool_session = std::move(session);
        }
    }
    if (gpui_requested) {
        if (const char* name = std::getenv("EGLANTINE_GPUI_HOME_KEY")) {
            const auto key = SDL_GetScancodeFromName(name);
            if (key != SDL_SCANCODE_UNKNOWN) {
                state->gpui_home_key = key;
            }
        }
        if (const char* name = std::getenv("EGLANTINE_GPUI_HOME_BUTTON")) {
            const auto button = SDL_GetGamepadButtonFromString(name);
            if (button != SDL_GAMEPAD_BUTTON_INVALID) {
                state->gpui_home_button = button;
            }
        }
    }

    const auto fail_session = [state](std::string_view message) {
        if (state->gpui_session) {
            state->gpui_session->Error(message);
            state->gpui_start_error = true;
        }
        return SDL_APP_FAILURE;
    };

#ifdef _WIN32
    if (!gpui_requested && !gpui_tool_session && AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "wb", stdout);
        freopen("CONOUT$", "wb", stderr);
    }
    // Raise the CRT's open-file limit like the Qt frontend: loose RomFS folders keep thousands of
    // files open through the VFS cache.
    _setmaxstdio(8192);
#elif defined(__unix__) || defined(__APPLE__)
    // Same for the open file limit (macOS shells default to 256).
    if (struct rlimit rl; getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = std::min<rlim_t>(8192, rl.rlim_max);
        setrlimit(RLIMIT_NOFILE, &rl);
    }
#endif

    if (!local_crash_dump && !gpui_tool_session) {
        state->crash_reporter.Start(Frontend::SentryFrontend::Sdl);
    }

    if (!gpui_tool_session) {
        Common::Log::Initialize();
        Common::Log::SetColorConsoleBackendEnabled(true);
        Common::Log::Start();
    }

#ifdef _WIN32
    int argc_w = 0;
    auto argv_w = CommandLineToArgvW(GetCommandLineW(), &argc_w);
    if (argv_w == nullptr) {
        LOG_CRITICAL(Frontend, "Failed to get command line arguments");
        return fail_session("Failed to read game launch arguments");
    }
    wchar_t** matched_argv_w = argc_w == argc ? argv_w : nullptr;
#else
    wchar_t** matched_argv_w = nullptr;
#endif

    if (gpui_tool_session) {
        const bool success = RunGpuiTool(gpui_tool, *gpui_tool_session, state->system, argc, argv,
                                         matched_argv_w);
#ifdef _WIN32
        LocalFree(argv_w);
#endif
        return success ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
    }

    // The private switch belongs to SDL only. Preserve both UTF-8 and wide arguments for Core.
    if (gpui_requested) {
        argv[1] = argv[0];
        ++argv;
        --argc;
        if (matched_argv_w) {
            matched_argv_w[1] = matched_argv_w[0];
            ++matched_argv_w;
        }
    }

    if (!gpui_requested && argc > 1 && std::string_view{argv[1]} == "--build-warm") {
        const bool ok = BuildWarm(state->system, argc, argv, matched_argv_w);
#ifdef _WIN32
        LocalFree(argv_w);
#endif
        return ok ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
    }

    Core::LaunchParams lp = Core::ParseLaunchParams(state->system, argc, argv, matched_argv_w);
#ifdef _WIN32
    LocalFree(argv_w);
#endif
    if (gpui_requested && (lp.filepath.empty() || lp.print_help || lp.print_version ||
                           lp.pack_nup_source || lp.unpack_nup_source || lp.edit_nup_source)) {
        return fail_session("A game path is required for a GPUI gameplay session");
    }
    state->owned_config = std::make_unique<SdlConfig>(lp.config_path);
    state->config = state->owned_config.get();

    if (!lp.filepath.empty() && !lp.print_help && !lp.print_version && !lp.pack_nup_source &&
        !lp.unpack_nup_source && !lp.edit_nup_source) {
        const std::filesystem::path path{Common::FS::ToU8String(lp.filepath)};
        auto extension = Common::FS::PathToUTF8String(path.extension());
        if (!extension.empty()) {
            extension.erase(0, 1);
            std::ranges::transform(extension, extension.begin(), [](unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
        }
        if (Emulation::Classic::IsClassicCartridgeExtension(extension)) {
#ifdef EGLANTINE_CLASSIC_RUNTIME
            Core::ApplyLaunchParams(lp);
            const auto classic_dir =
                Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "classic";
            const auto profile_str = std::to_string(Settings::values.current_user.GetValue());
            const auto save_root = classic_dir / "saves" / profile_str;
            const auto state_root = classic_dir / "states" / profile_str;
            SDL_Environment* const sdl_env = SDL_GetEnvironment();
            std::filesystem::path bios_path = classic_dir / "bios" / "gba_bios.bin";
            bool bios_explicit = false;
            if (const char* env_bios =
                    sdl_env ? SDL_GetEnvironmentVariable(sdl_env, "EGLANTINE_GPUI_GBA_BIOS")
                            : nullptr;
                env_bios && env_bios[0] != '\0') {
                bios_path = std::filesystem::path{Common::FS::ToU8String(env_bios)};
                bios_explicit = true;
            }
            const auto env_flag = [sdl_env](const char* name) {
                const char* v = sdl_env ? SDL_GetEnvironmentVariable(sdl_env, name) : nullptr;
                return v && (std::string_view{v} == "1" || std::string_view{v} == "true");
            };
            std::vector<std::string> cheats;
            if (const char* env_cheats =
                    sdl_env ? SDL_GetEnvironmentVariable(sdl_env, "EGLANTINE_GPUI_GBA_CHEATS")
                            : nullptr;
                env_cheats && env_cheats[0] != '\0') {
                std::string_view sv{env_cheats};
                while (!sv.empty()) {
                    const auto sep = sv.find_first_of(";\n");
                    const auto item = sv.substr(0, sep);
                    if (!item.empty()) {
                        cheats.emplace_back(item);
                    }
                    if (sep == std::string_view::npos) {
                        break;
                    }
                    sv.remove_prefix(sep + 1);
                }
            }
            std::string error;
            state->classic_session = DesktopClassic::Session::Open(
                {.cartridge = path,
                 .save_root = save_root,
                 .state_root = state_root,
                 .bios_path = bios_path,
                 .bios_explicit = bios_explicit,
                 .extension = extension,
                 .fullscreen = lp.fullscreen,
                 .volume = Settings::values.audio_muted.GetValue()
                               ? 0.0f
                               : static_cast<float>(Settings::values.volume.GetValue()) / 100.0f,
                 .color_correction = env_flag("EGLANTINE_GPUI_GBA_COLOR_CORRECTION"),
                 .interframe_blending = env_flag("EGLANTINE_GPUI_GBA_INTERFRAME_BLENDING"),
                 .cheats = std::move(cheats),
                 .gpui = state->gpui_session.get(),
                 .home_key = state->gpui_home_key,
                 .home_button = state->gpui_home_button},
                error);
            if (state->classic_session) {
                return SDL_APP_CONTINUE;
            }
#else
            const std::string_view error = "Classic cartridge playback is disabled in this build";
#endif
            LOG_ERROR(Frontend, "{}", error);
            return fail_session(error);
        }
    }

    // Before anything loads a GPU driver.
    Common::ConfigureDriverShaderCache(Settings::values.private_driver_shader_cache.GetValue(),
                                       Settings::values.driver_shader_cache_size.GetValue());
    Common::CreateDriverShaderCacheFolder();

    Core::ApplyLaunchParams(lp);

    // Informational options already printed by ApplyLaunchParams; handle them before any
    // archive operation so --help/--version never packs, unpacks or edits.
    if (lp.print_help || lp.print_version) {
        return SDL_APP_SUCCESS;
    }

    if (lp.pack_nup_source) {
        return PackNup(*lp.pack_nup_source, lp.filepath) ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
    }

    if (lp.unpack_nup_source) {
        return UnpackNup(*lp.unpack_nup_source, lp.filepath) ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
    }

    if (lp.edit_nup_source) {
        return EditNup(lp) ? SDL_APP_SUCCESS : SDL_APP_FAILURE;
    }

    if (lp.filepath.empty()) {
        LOG_CRITICAL(Frontend, "Failed to load ROM: No ROM specified");
        return SDL_APP_FAILURE;
    }

    state->network_initialized = Network::Init();
    if (!state->network_initialized) {
        LOG_CRITICAL(Network, "Failed to initialize room networking");
        return fail_session("Failed to initialize room networking");
    }

    state->system.Initialize();
    state->swkbd_controller = std::make_shared<YuzuCmd::SoftwareKeyboardController>();
    {
        Service::AM::Frontend::FrontendAppletSet applets{};
        applets.software_keyboard =
            std::make_unique<YuzuCmd::HostSoftwareKeyboardApplet>(state->swkbd_controller);
        state->system.SetFrontendAppletSet(std::move(applets));
    }


    // Apply the command line arguments
    state->system.ApplySettings();

    switch (Settings::values.renderer_backend.GetValue()) {
#ifdef HAS_OPENGL
    case Settings::RendererBackend::OpenGL_GLSL:
    case Settings::RendererBackend::OpenGL_GLASM:
    case Settings::RendererBackend::OpenGL_SPIRV:
        state->emu_window = std::make_unique<EmuWindow_SDL3_GL>(&state->input_subsystem, state->system, lp.fullscreen);
        break;
#endif
    case Settings::RendererBackend::Vulkan:
        state->emu_window = std::make_unique<EmuWindow_SDL3_VK>(&state->input_subsystem, state->system, lp.fullscreen);
        break;
    case Settings::RendererBackend::Null:
        state->emu_window = std::make_unique<EmuWindow_SDL3_Null>(&state->input_subsystem, state->system, lp.fullscreen);
        break;
    default:
        LOG_CRITICAL(Frontend, "Invalid renderer backend");
        return fail_session("Invalid renderer backend");
    }

#ifdef _WIN32
    Common::Windows::SetCurrentTimerResolutionToMaximum();
    state->system.CoreTiming().SetTimerResolutionNs(Common::Windows::GetCurrentTimerResolution());
#endif

    state->system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    state->system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    state->system.GetFileSystemController().CreateFactories(*state->system.GetFilesystem());
    state->system.GetUserChannel().clear();

    Service::AM::FrontendAppletParameters load_parameters{
        .applet_id = Service::AM::AppletId::Application,
    };
    const char* const handoff_env = std::getenv("EDEN_CLI_RESTART_HANDOFF");
    const bool restarted = handoff_env != nullptr && handoff_env[0] != '\0';
    if (restarted) {
        auto handoff = ReadRestartHandoff();
        if (!handoff) {
            // Booting as a frontend launch would clear the game's temp storage.
            LOG_CRITICAL(Frontend, "Restart handoff {} is missing or damaged, not booting",
                         Common::FS::PathToUTF8String(RestartHandoffPath()));
            return fail_session("The game restart handoff is missing or damaged");
        }
        state->system.GetUserChannel() = std::move(handoff->user_channel);
        load_parameters.program_id = handoff->program_id;
        load_parameters.launch_type = Service::AM::LaunchType::ApplicationInitiated;
        load_parameters.program_index = handoff->program_index;
        load_parameters.previous_program_index = handoff->previous_program_index;
        LOG_INFO(Frontend, "Restarted by the game: program index {}, {} user channel entries",
                 handoff->program_index, state->system.GetUserChannel().size());
    }
    const Core::SystemResultStatus load_result = state->system.Load(*state->emu_window, lp.filepath, load_parameters);
    switch (load_result) {
    case Core::SystemResultStatus::Success:
        if (restarted) {
            std::error_code ec;
            std::filesystem::remove(RestartHandoffPath(), ec);
        }
        break; // Expected case
    case Core::SystemResultStatus::ErrorGetLoader:
        LOG_CRITICAL(Frontend, "Failed to obtain loader for {}!", lp.filepath);
        return fail_session("The game format was not recognized or the file could not be read");
    case Core::SystemResultStatus::ErrorLoader:
        LOG_CRITICAL(Frontend, "Failed to load ROM!");
        return fail_session("Failed to load the game");
    case Core::SystemResultStatus::ErrorNotInitialized:
        LOG_CRITICAL(Frontend, "CPUCore not initialized");
        return fail_session("The emulator CPU was not initialized");
    case Core::SystemResultStatus::ErrorVideoCore:
        LOG_CRITICAL(Frontend, "Failed to initialize VideoCore!");
        return fail_session("Failed to initialize the game renderer");
    default:
        const u16 loader_id = u16(Core::SystemResultStatus::ErrorLoader);
        const u16 error_id = u16(load_result) - loader_id;
        LOG_CRITICAL(Frontend,
            "While attempting to load the ROM requested, an error occurred. Please "
            "refer to the Eden wiki for more information or the Eden discord for "
            "additional help.\n\nError Code: {:04X}-{:04X}\nError Description: {}",
            loader_id, error_id, Loader::ResultStatus(error_id));
        return fail_session(fmt::format("Game load failed ({:04X}-{:04X}): {}", loader_id,
                                        error_id, Loader::ResultStatus(error_id)));
    }

    if (lp.use_multiplayer) {
        if (auto member = Network::GetRoomMember().lock()) {
            member->BindOnChatMessageReceived(OnMessageReceived);
            member->BindOnStatusMessageReceived(OnStatusMessageReceived);
            member->BindOnStateChanged(OnStateChanged);
            member->BindOnError(OnNetworkError);
            LOG_DEBUG(Network, "Start connection to {}:{} with nickname {}", lp.address, lp.port, lp.nickname);
            member->Join(lp.nickname, lp.address.c_str(), lp.port, 0, Network::NoPreferredIP, lp.password);
        } else {
            LOG_ERROR(Network, "Could not access RoomMember");
            return SDL_APP_FAILURE;
        }
    }

    // Core is loaded, start the GPU (makes the GPU contexts current to this thread)
    state->system.GPU().Start();
    state->system.GetCpuManager().OnGpuReady();

    if (Settings::values.use_disk_shader_cache.GetValue()) {
        VideoCommon::ShaderWarm::RefreshWarmFiles(
            state->system, state->system.GetApplicationProcessProgramID(), {});
        state->system.Renderer().ReadRasterizer()->LoadDiskResources(
            state->system.GetApplicationProcessProgramID(), std::stop_token{},
            [](VideoCore::LoadCallbackStage, size_t value, size_t total) {});
    }

    state->system.RegisterExitCallback([state, gpui_requested] {
        if (gpui_requested) {
            state->gpui_game_exit = true;
        }
    });

    // A game executing a program (e.g. restarting after a settings change) writes its launch state
    // to the restart handoff and quits with kRestartExitCode; the launcher boots the same file again
    // with EDEN_CLI_RESTART_HANDOFF set. Mirrors MainWindow::OnExecuteProgram.
    state->system.RegisterExecuteProgramCallback([system = &state->system](std::size_t program_index) {
        RestartHandoff handoff{
            .program_index = static_cast<s32>(program_index),
            .user_channel = system->GetUserChannel(),
        };
        const u64 previous_program_id = system->GetApplicationProcessProgramID();
        if (previous_program_id > static_cast<u64>(Service::AM::AppletProgramId::MaxProgramId)) {
            handoff.previous_program_index =
                static_cast<s32>(previous_program_id - FileSys::GetBaseTitleID(previous_program_id));
            handoff.program_id = previous_program_id;
        }
        // Without the handoff, a relaunch would boot as a frontend launch and clear temp storage.
        if (!WriteRestartHandoff(handoff)) {
            LOG_CRITICAL(Frontend, "ExecuteProgram: could not write {}, not restarting",
                         Common::FS::PathToUTF8String(RestartHandoffPath()));
            return;
        }
        LOG_INFO(Frontend, "ExecuteProgram: program index {}, exiting with code {} to restart",
                 program_index, kRestartExitCode);
        restart_requested = true;
    });

    // QLaunch launched applications (should) now reload shader cache.
    static std::mutex shader_cache_reload_mutex;
    state->system.RegisterApplicationChangedCallback([state](u64 changed_program_id) {
        if (!Settings::values.use_disk_shader_cache.GetValue()) {
            return;
        }

        std::scoped_lock lk{shader_cache_reload_mutex};

        LOG_INFO(Frontend, "Reloading disk shader cache for {:016X}", changed_program_id);

        state->system.Pause();
        state->system.GPU().WaitForIdle();
        state->system.GPU().ObtainContext();

        state->system.Renderer().ReadRasterizer()->LoadDiskResources(
            changed_program_id, std::stop_token{},
            [](VideoCore::LoadCallbackStage, size_t, size_t) {});

        state->system.GPU().ReleaseContext();
        state->system.Run();
    });

    if (state->gpui_session) {
        for (std::size_t index = 0; index < Settings::values.players.GetValue().size(); ++index) {
            state->system.HIDCore().GetEmulatedControllerByIndex(index)->SetHomeButtonEnabled(false);
        }
    }
    void(state->system.Run());
    if (state->system.DebuggerEnabled())
        state->system.InitializeDebugger();

#ifdef YUZU_ENABLE_MCP
    // eden-cli starts the MCP server only when asked (--mcp), so unattended runs never collide
    // with a desktop emulator holding the port.
    if (lp.mcp_enable) {
        Mcp::ServerConfig mcp_config;
        mcp_config.port = lp.mcp_port.value_or(Settings::values.mcp_port.GetValue());
        mcp_config.allow_writes = lp.mcp_allow_writes || Settings::values.mcp_allow_writes.GetValue();
        mcp_config.bearer_token = Settings::values.mcp_token.GetValue();
        state->mcp_host = std::make_unique<CliMcpHost>(state->system, state->input_subsystem);
        state->mcp_server =
            std::make_unique<Mcp::Server>(state->system, *state->mcp_host, std::move(mcp_config));
        if (const std::string error = state->mcp_server->Start(); !error.empty()) {
            LOG_ERROR(Frontend, "MCP server: {}", error);
            state->mcp_server.reset();
        }
    }
#endif
    if (state->gpui_session) {
        state->gpui_session->SetState(Gpui::SessionState::Running);
    }
    return SDL_APP_CONTINUE;
}
// EDEN_NX2_KEY_SCRIPT="seconds:code,seconds:-code,..." (or "@<file>"): press (code) or release (-code) keyboard
// keys, by SDL scancode, that many seconds after start. Lets hidden test runs drive menus.
double Nx2KeyClockSeconds(); // emu_window_sdl3.cpp

// EDEN_NX2_DIAG_DIR=<dir>: keys.txt there is read and removed while the game runs, for scripted
// control (tools/nx2/lab_ctl.py press). One entry per line: "<code>" press, "-<code>" release, or
// "<code> <milliseconds>" press and release after that long. Codes are SDL scancodes.
static void RunLiveKeys(InputCommon::InputSubsystem& input_subsystem, Core::System& system) {
    static const std::filesystem::path file = [] {
        const char* dir = std::getenv("EDEN_NX2_DIAG_DIR");
        return dir && *dir ? std::filesystem::path{dir} / "keys.txt" : std::filesystem::path{};
    }();
    if (file.empty()) {
        return;
    }
    static std::vector<std::pair<double, int>> releases;
    static double last_check = 0.0;
    const double now = Nx2KeyClockSeconds();
    std::erase_if(releases, [&](const std::pair<double, int>& release) {
        if (release.first > now) {
            return false;
        }
        input_subsystem.GetKeyboard()->ReleaseKey(static_cast<std::size_t>(release.second));
        return true;
    });
    if (now - last_check < 0.05) {
        return;
    }
    last_check = now;
    // Renamed first, so a writer replacing the file is never read halfway.
    const auto taken = std::filesystem::path{file}.replace_extension(".taken");
    std::error_code error;
    std::filesystem::rename(file, taken, error);
    if (error) {
        return;
    }
    std::ifstream in{taken};
    std::string line;
    while (std::getline(in, line)) {
        // "gpu low|high": sets the GPU accuracy, as the Qt app's Change GPU Mode hotkey.
        if (line.starts_with("gpu ")) {
            const bool high = line.substr(4) == "high";
            Settings::values.gpu_accuracy.SetValue(high ? Settings::GpuAccuracy::High
                                                        : Settings::GpuAccuracy::Low);
            system.ApplySettings();
            LOG_INFO(Frontend, "Live keys: GPU accuracy {}", high ? "high" : "low");
            continue;
        }
        int code = 0, milliseconds = -1;
        if (std::sscanf(line.c_str(), "%d %d", &code, &milliseconds) < 1 || code == 0) {
            continue;
        }
        LOG_INFO(Frontend, "Live keys: {} {}{}", code < 0 ? "release" : "press", code < 0 ? -code : code,
                 milliseconds >= 0 ? fmt::format(" for {} ms", milliseconds) : "");
        if (code < 0) {
            input_subsystem.GetKeyboard()->ReleaseKey(static_cast<std::size_t>(-code));
            continue;
        }
        input_subsystem.GetKeyboard()->PressKey(static_cast<std::size_t>(code));
        if (milliseconds >= 0) {
            releases.emplace_back(now + milliseconds / 1000.0, code);
        }
    }
    in.close();
    std::filesystem::remove(taken, error);
}

static void RunKeyScript(InputCommon::InputSubsystem& input_subsystem, Core::System& system) {
    struct KeyEvent {
        double seconds; // when frame is 0
        u64 frame;      // "f<frame>:code": after that many presents
        int code;
    };
    static const std::vector<KeyEvent> script = [] {
        std::vector<KeyEvent> events;
        if (const char* text = std::getenv("EDEN_NX2_KEY_SCRIPT")) {
            // "@<file>" reads the list from a file: long scripts in the environment slow down
            // every getenv in the process.
            std::string list{text};
            if (!list.empty() && list[0] == '@') {
                std::ifstream file{list.substr(1)};
                list.assign(std::istreambuf_iterator<char>{file}, {});
            }
            size_t start = 0;
            while (start < list.size()) {
                const size_t end = (std::min)(list.find(',', start), list.size());
                const std::string item = list.substr(start, end - start);
                if (const size_t colon = item.find(':'); colon != std::string::npos) {
                    const std::string when = item.substr(0, colon);
                    const int code = std::stoi(item.substr(colon + 1));
                    if (!when.empty() && when[0] == 'f') {
                        events.push_back({0.0, std::stoull(when.substr(1)), code});
                    } else {
                        events.push_back({std::stod(when), 0, code});
                    }
                }
                start = end + 1;
            }
        }
        return events;
    }();
    static size_t next = 0;
    const double elapsed = Nx2KeyClockSeconds();
    const u64 frame = Core::g_nx2_present_count.load();
    while (next < script.size() && (script[next].frame != 0 ? script[next].frame <= frame
                                                            : script[next].seconds <= elapsed)) {
        const int code = script[next].code;
        LOG_INFO(Frontend, "Key script: {} key {} at {:.1f} s (frame {})",
                 code < 0 ? "release" : "press", code < 0 ? -code : code, elapsed, frame);
        if (code < 0) {
            input_subsystem.GetKeyboard()->ReleaseKey(static_cast<std::size_t>(-code));
        } else {
            input_subsystem.GetKeyboard()->PressKey(static_cast<std::size_t>(code));
        }
        ++next;
    }
    RunLiveKeys(input_subsystem, system);
}

// Binds the Switch 2 controllers that the driver found to player slots, so a controller that is
// plugged in while the game is running is picked up as well. Runs about once per second:
// SDL_AppIterate runs up to 240 times per second and this pass touches the settings.
static void CheckSwitch2AutoPair(SdlState* state) {
    constexpr auto kInterval = std::chrono::seconds(1);
    const auto now = std::chrono::steady_clock::now();
    if (now - state->last_switch2_auto_pair < kInterval) {
        return;
    }
    state->last_switch2_auto_pair = now;

    auto* switch2 = state->input_subsystem.GetSwitch2();
    if (switch2 == nullptr) {
        // The driver is disabled (Settings::values.enable_switch2_driver).
        return;
    }

    static const auto keyboard_defaults = InputCommon::MakeDefaultKeyboardPlayer(
        SdlConfig::default_buttons, SdlConfig::default_analogs, SdlConfig::default_stick_mod,
        SdlConfig::default_motions);
    const auto changed = state->switch2_auto_pair.Apply(
        *switch2, InputCommon::PlayerOneUsesDefaultKeyboard(keyboard_defaults));
    if (changed.empty()) {
        return;
    }

    for (const auto& player : changed) {
        const std::string name = player.device_name.empty() ? player.device_id : player.device_name;
        if (player.disconnected) {
            LOG_INFO(Frontend, "Switch 2 auto-pair: {} disconnected from Player {}", name,
                     player.player_index + 1);
        } else if (player.reconnected) {
            LOG_INFO(Frontend, "Switch 2 auto-pair: {} reconnected to Player {}", name,
                     player.player_index + 1);
        } else {
            LOG_INFO(Frontend, "Switch 2 auto-pair: {} bound to Player {}", name,
                     player.player_index + 1);
        }
    }

    // Let the running game see the new binding.
    if (state->system.IsPoweredOn()) {
        state->system.HIDCore().ReloadInputDevices();
    }

    // Keep the binding for the next start.
    if (state->config != nullptr) {
        state->config->SaveAllValues();
    }
}

static void UpdateGpuiHome(SdlState& state) {
    auto& session = *state.gpui_session;
    const bool pressed = state.gpui_key_held || state.gpui_controller_held.has_value() ||
                         state.gpui_mapped_home_held;
    if (std::exchange(state.gpui_home_down, pressed) == pressed) {
        return;
    }
    if (pressed && session.GetState() == Gpui::SessionState::Running &&
        state.emu_window->HasInputFocus()) {
        state.gpui_home_announced = true;
        session.SendEvent("home_requested");
    } else if (!pressed && std::exchange(state.gpui_home_announced, false)) {
        session.SendEvent("home_released");
    }
}

static bool RunGpuiSession(SdlState& state) {
    auto& session = *state.gpui_session;
    using enum Gpui::SessionState;

    if (state.swkbd_controller) {
        state.swkbd_controller->DrainGpuiEvents(session, state.emu_window.get());
    }
    std::vector<Gpui::Request> requests;
    if (state.gpui_game_exit || !session.Poll(requests)) {
        if (state.swkbd_controller) {
            state.swkbd_controller->CloseNormal();
        }
        session.SetState(Stopping);
        return false;
    }
    state.gpui_mapped_home_held = false;
    for (std::size_t index = 0; index < Settings::values.players.GetValue().size(); ++index) {
        if (state.system.HIDCore().GetEmulatedControllerByIndex(index)->GetHomeButtonValue()) {
            state.gpui_mapped_home_held = true;
            break;
        }
    }
    UpdateGpuiHome(state);
    for (const auto& request : requests) {
        if (session.GetState() == Stopping) {
            session.Reply(request.id, request.command == Gpui::Command::Stop
                                          ? std::string_view{}
                                          : "The game is stopping");
            continue;
        }
        switch (request.command) {
        case Gpui::Command::Pause:
            if (session.GetState() == Running) {
                state.system.Pause();
                state.input_subsystem.GetKeyboard()->ReleaseAllKeys();
                if (!state.emu_window->SetSessionVisible(false)) {
                    LOG_WARNING(Frontend, "Could not hide the paused game window: {}", SDL_GetError());
                }
                session.SetState(Paused);
            }
            break;
        case Gpui::Command::Resume:
            if (session.GetState() == Paused) {
                if (!state.emu_window->SetSessionVisible(true)) {
                    session.Reply(request.id, "Could not restore the game window");
                    continue;
                }
                state.system.Run();
                session.SetState(Running);
            }
            break;
        case Gpui::Command::Stop:
            if (state.swkbd_controller) {
                state.swkbd_controller->CloseNormal();
            }
            session.SetState(Stopping);
            break;
        case Gpui::Command::SetViewport:
            if (!state.emu_window->SetViewport(request.viewport_x, request.viewport_y,
                                               request.viewport_width, request.viewport_height)) {
                session.Reply(request.id, "Could not resize the game viewport");
                continue;
            }
            break;
        case Gpui::Command::SwkbdSubmit: {
            if (!state.swkbd_controller) {
                session.Reply(request.id, "Software keyboard is not available");
                continue;
            }
            std::string error;
            if (!state.swkbd_controller->Submit(request.swkbd_request_id, request.swkbd_text,
                                                request.swkbd_confirmed, error)) {
                session.Reply(request.id, error);
                continue;
            }
            break;
        }
        case Gpui::Command::SwkbdCancel: {
            if (!state.swkbd_controller) {
                session.Reply(request.id, "Software keyboard is not available");
                continue;
            }
            std::string error;
            if (!state.swkbd_controller->Cancel(request.swkbd_request_id, error)) {
                session.Reply(request.id, error);
                continue;
            }
            break;
        }
        case Gpui::Command::SaveState:
        case Gpui::Command::LoadState:
        case Gpui::Command::SetFastForward:
        case Gpui::Command::RewindStep:
        case Gpui::Command::SetVideoOptions:
        case Gpui::Command::SetCheats:
        case Gpui::Command::Reset:
            session.Reply(request.id, "This command is only supported in GBA sessions");
            continue;
        }
        session.Reply(request.id);
    }
    if (state.swkbd_controller) {
        state.swkbd_controller->DrainGpuiEvents(session, state.emu_window.get());
    }
    return session.GetState() != Stopping;
}

static bool HandleGpuiHome(SdlState& state, const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
        if (event.key.scancode == state.gpui_home_key) {
            if (!event.key.repeat) {
                state.gpui_key_held = true;
                UpdateGpuiHome(state);
            }
            return true;
        }
        break;
    case SDL_EVENT_KEY_UP:
        if (event.key.scancode == state.gpui_home_key) {
            state.gpui_key_held = false;
            UpdateGpuiHome(state);
            return true;
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        state.gpui_key_held = false;
        UpdateGpuiHome(state);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (event.gbutton.button == state.gpui_home_button && !state.gpui_controller_held) {
            state.gpui_controller_held = event.gbutton.which;
            UpdateGpuiHome(state);
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (event.gbutton.button == state.gpui_home_button &&
            state.gpui_controller_held == event.gbutton.which) {
            state.gpui_controller_held.reset();
            UpdateGpuiHome(state);
        }
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (state.gpui_controller_held == event.gdevice.which) {
            state.gpui_controller_held.reset();
            UpdateGpuiHome(state);
        }
        break;
    default:
        break;
    }
    return false;
}

extern "C" SDL_AppResult SDL_AppIterate(void *appstate) {
    SdlState *state = (SdlState *)appstate;
#ifdef EGLANTINE_CLASSIC_RUNTIME
    if (state->classic_session) {
        return state->classic_session->Iterate();
    }
#endif
    if (state->gpui_session && !RunGpuiSession(*state)) {
        return SDL_APP_SUCCESS;
    }
    if (!state->gpui_session || state->gpui_session->GetState() == Gpui::SessionState::Running) {
        RunKeyScript(state->input_subsystem, state->system);
    }
    CheckSwitch2AutoPair(state);
#ifdef YUZU_ENABLE_MCP
    if (state->mcp_host) {
        state->mcp_host->DrainMainThread();
        if (state->mcp_host->QuitRequested()) {
            return SDL_APP_SUCCESS;
        }
    }
#endif
    if (!state->gpui_session && state->swkbd_controller && state->emu_window) {
        state->swkbd_controller->DrainSdlEvents(state->emu_window->GetRenderWindow());
    }
    if (state->emu_window &&
        !(state->swkbd_controller && state->swkbd_controller->IsSdlActive())) {
        state->emu_window->ApplyPendingTitle();
    }
    if (restart_requested) {
        return SDL_APP_SUCCESS;
    }
    return state->emu_window && state->emu_window->IsOpen() ? SDL_APP_CONTINUE : SDL_APP_SUCCESS;
}
extern "C" SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
    SdlState *state = (SdlState *)appstate;
#ifdef EGLANTINE_CLASSIC_RUNTIME
    if (state->classic_session) {
        return state->classic_session->Event(*event);
    }
#endif
    if (state->emu_window) {
        if (!state->gpui_session && state->swkbd_controller &&
            state->swkbd_controller->HandleSdlEvent(*event, state->emu_window->GetRenderWindow())) {
            return SDL_APP_CONTINUE;
        }
        if (state->gpui_session) {
            if (HandleGpuiHome(*state, *event)) {
                return SDL_APP_CONTINUE;
            }
            if (state->gpui_session->GetState() == Gpui::SessionState::Paused &&
                event->type >= SDL_EVENT_KEY_DOWN && event->type <= SDL_EVENT_FINGER_CANCELED) {
                return SDL_APP_CONTINUE;
            }
        }
        state->emu_window->OnEvent(*event);
    }
    return SDL_APP_CONTINUE;
}
extern "C" void SDL_AppQuit(void *appstate, SDL_AppResult result) {
    SdlState *state = (SdlState *)appstate;
    if (!state) {
        return;
    }
#ifdef EGLANTINE_CLASSIC_RUNTIME
    if (state->classic_session) {
        state->classic_session->Finish(result);
        delete state;
        return;
    }
#endif
    auto gpui_session = std::move(state->gpui_session);
    if (gpui_session) {
        if (result == SDL_APP_FAILURE && !state->gpui_start_error) {
            gpui_session->Error("The gameplay host failed. See the emulator log for details.");
        }
        gpui_session->SetState(Gpui::SessionState::Stopping);
        LOG_DEBUG(Frontend, "Stopping GPUI gameplay session");
    }
#ifdef YUZU_ENABLE_MCP
    state->mcp_server.reset(); // stops serving before the system goes away
    state->mcp_host.reset();
#endif
    if (state->swkbd_controller) {
        state->swkbd_controller->CloseNormal();
    }
    state->system.DetachDebugger();
    // SDL calls this even when SDL_AppInit never powered the system on (no ROM given, --help or
    // --version): the kernel is uninitialised then, and Pause()/ShutdownMainProcess() would
    // dereference it.
    if (state->system.IsPoweredOn()) {
        if (!gpui_session) {
            state->system.Pause();
        }
        // GPUI can stop immediately after boot. Core shutdown requests cancellation before
        // pausing, so startup work can release its waits before the emulation threads stop.
        state->system.ShutdownMainProcess();
    }
    if (gpui_session) {
        LOG_DEBUG(Frontend, "Releasing GPUI gameplay host resources");
    }
    const bool network_initialized = state->network_initialized;
    delete state;
    if (network_initialized) {
        Network::Shutdown();
    }
    if (gpui_session) {
        gpui_session->SetState(Gpui::SessionState::Stopped);
    }
    if (restart_requested) {
        // SDL3 maps SDL_APP_SUCCESS to exit code 0; quit SDL ourselves to return the restart code.
        SDL_Quit();
        std::exit(kRestartExitCode);
    }
}

#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"
