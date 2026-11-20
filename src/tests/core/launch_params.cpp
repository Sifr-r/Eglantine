// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "common/uuid.h"
#include "core/core.h"
#include "core/hle/result.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/launch_params.h"
#include "network/room.h"
#include "tests/core/system_test_context.h"

namespace {

Core::LaunchParams RunParse(Core::System& system, std::initializer_list<const char*> raw_args) {
    std::vector<std::string> storage;
    storage.reserve(raw_args.size());
    for (const char* arg : raw_args) {
        storage.emplace_back(arg);
    }

    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& arg : storage) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    const int argc = static_cast<int>(storage.size());
    return Core::ParseLaunchParams(system, argc, argv.data(), nullptr);
}

} // Anonymous namespace

TEST_CASE("ParseLaunchParams handles null or empty arguments", "[core][launch_params]") {
    const ScopedSystemTestContext context;
    Core::System system;

    const auto check_defaults = [](const Core::LaunchParams& lp) {
        CHECK(lp.argv0.empty());
        CHECK(lp.filepath.empty());
        CHECK_FALSE(lp.enable_gdbstub);
        CHECK_FALSE(lp.override_gdb_port.has_value());
        CHECK_FALSE(lp.mcp_enable);
        CHECK_FALSE(lp.mcp_port.has_value());
        CHECK_FALSE(lp.mcp_allow_writes);
        CHECK(lp.port == Network::DefaultRoomPort);
        CHECK_FALSE(lp.use_multiplayer);
        CHECK_FALSE(lp.fullscreen);
        CHECK_FALSE(lp.force_null_render);
        CHECK_FALSE(lp.force_single_core);
        CHECK_FALSE(lp.print_version);
        CHECK_FALSE(lp.print_help);
        CHECK_FALSE(lp.launch_hlaunch);
        CHECK_FALSE(lp.launch_qlaunch);
        CHECK_FALSE(lp.launch_setup);
    };

    check_defaults(Core::ParseLaunchParams(system, 0, nullptr, nullptr));
    check_defaults(Core::ParseLaunchParams(system, -1, nullptr, nullptr));
    check_defaults(Core::ParseLaunchParams(system, 1, nullptr, nullptr));

    std::string arg0 = "eglantine";
    char* empty_argv[] = {arg0.data(), nullptr};
    check_defaults(Core::ParseLaunchParams(system, 0, empty_argv, nullptr));
}

TEST_CASE("ParseLaunchParams parses --debug optional port and --mcp-port bounds",
          "[core][launch_params]") {
    const ScopedSystemTestContext context;
    Core::System system;

    {
        const auto lp = RunParse(system, {"eglantine", "-d"});
        CHECK(lp.argv0 == "eglantine");
        CHECK(lp.enable_gdbstub);
        CHECK_FALSE(lp.override_gdb_port.has_value());
    }

    {
        const auto lp = RunParse(system, {"eglantine", "--debug"});
        CHECK(lp.enable_gdbstub);
        CHECK_FALSE(lp.override_gdb_port.has_value());
    }

    {
        const auto lp = RunParse(system, {"eglantine", "-d1234"});
        CHECK(lp.enable_gdbstub);
        REQUIRE(lp.override_gdb_port.has_value());
        CHECK(*lp.override_gdb_port == u16{1234});
    }

    {
        const auto lp = RunParse(system, {"eglantine", "--debug=65535"});
        CHECK(lp.enable_gdbstub);
        REQUIRE(lp.override_gdb_port.has_value());
        CHECK(*lp.override_gdb_port == u16{65535});
    }

    for (const char* invalid_debug : {"-d0", "-d65536", "-dabc", "-d123trailing"}) {
        CAPTURE(invalid_debug);
        const auto lp = RunParse(system, {"eglantine", invalid_debug});
        CHECK(lp.enable_gdbstub);
        CHECK_FALSE(lp.override_gdb_port.has_value());
    }

    {
        const auto lp = RunParse(system, {"eglantine", "--mcp"});
        CHECK(lp.mcp_enable);
        CHECK_FALSE(lp.mcp_port.has_value());
        CHECK_FALSE(lp.mcp_allow_writes);
    }

    {
        const auto lp = RunParse(system, {"eglantine", "--mcp-port", "6545", "--mcp-allow-writes"});
        CHECK(lp.mcp_enable);
        REQUIRE(lp.mcp_port.has_value());
        CHECK(*lp.mcp_port == u16{6545});
        CHECK(lp.mcp_allow_writes);
    }

    for (const char* invalid_mcp_port : {"0", "70000", "bad"}) {
        CAPTURE(invalid_mcp_port);
        const auto lp = RunParse(system, {"eglantine", "--mcp-port", invalid_mcp_port});
        CHECK(lp.mcp_enable);
        CHECK_FALSE(lp.mcp_port.has_value());
    }
}

TEST_CASE("ParseLaunchParams parses NUP packing, unpacking, and metadata editing options",
          "[core][launch_params]") {
    const ScopedSystemTestContext context;
    Core::System system;

    {
        const auto lp = RunParse(system, {"eglantine", "--pack-nup", "/path/to/dump"});
        REQUIRE(lp.pack_nup_source.has_value());
        CHECK(*lp.pack_nup_source == "/path/to/dump");
        CHECK_FALSE(lp.unpack_nup_source.has_value());
        CHECK_FALSE(lp.edit_nup_source.has_value());
    }

    {
        const auto lp = RunParse(system, {"eglantine", "--unpack-nup", "/path/to/archive.nup"});
        REQUIRE(lp.unpack_nup_source.has_value());
        CHECK(*lp.unpack_nup_source == "/path/to/archive.nup");
        CHECK_FALSE(lp.pack_nup_source.has_value());
        CHECK_FALSE(lp.edit_nup_source.has_value());
    }

    {
        const auto lp = RunParse(
            system, {"eglantine", "--edit-nup", "/path/to/edit.nup", "--nup-name", "MyGame",
                     "--nup-icon", "icon.png", "--nup-version", "2.0.1", "--nup-publisher",
                     "Studio"});
        REQUIRE(lp.edit_nup_source.has_value());
        CHECK(*lp.edit_nup_source == "/path/to/edit.nup");
        REQUIRE(lp.edit_nup_name.has_value());
        CHECK(*lp.edit_nup_name == "MyGame");
        REQUIRE(lp.edit_nup_icon.has_value());
        CHECK(*lp.edit_nup_icon == "icon.png");
        REQUIRE(lp.edit_nup_version.has_value());
        CHECK(*lp.edit_nup_version == "2.0.1");
        REQUIRE(lp.edit_nup_publisher.has_value());
        CHECK(*lp.edit_nup_publisher == "Studio");
    }

    {
        const auto lp =
            RunParse(system, {"eglantine", "--edit-nup", "/path/to/edit.nup", "--clear-nup-name",
                              "--clear-nup-icon", "--clear-nup-version", "--clear-nup-publisher"});
        REQUIRE(lp.edit_nup_source.has_value());
        CHECK(*lp.edit_nup_source == "/path/to/edit.nup");
        REQUIRE(lp.edit_nup_name.has_value());
        CHECK(lp.edit_nup_name->empty());
        REQUIRE(lp.edit_nup_icon.has_value());
        CHECK(lp.edit_nup_icon->empty());
        REQUIRE(lp.edit_nup_version.has_value());
        CHECK(lp.edit_nup_version->empty());
        REQUIRE(lp.edit_nup_publisher.has_value());
        CHECK(lp.edit_nup_publisher->empty());
    }
}

TEST_CASE("ParseLaunchParams parses game, user, multiplayer, filter, and positional arguments",
          "[core][launch_params]") {
    const ScopedSystemTestContext context;
    const s32 prev_current_user = Settings::values.current_user.GetValue();
    SCOPE_EXIT {
        Settings::values.current_user.SetValue(prev_current_user);
    };

    Core::System system;
    auto& profiles = system.GetProfileManager();
    REQUIRE(profiles.UserExistsIndex(0));
    REQUIRE(profiles.CreateNewUser(Common::UUID::MakeRandom(), "PlayerTwo") == ResultSuccess);
    REQUIRE(profiles.UserExistsIndex(1));

    {
        const auto lp = RunParse(
            system, {"eglantine", "-c", "custom.ini", "-f", "-h", "-v", "-n", "-s", "-i",
                     "custom_profile", "-p", "--arg=1", "-g", "title.nsp"});
        CHECK(lp.argv0 == "eglantine");
        REQUIRE(lp.config_path.has_value());
        CHECK(*lp.config_path == "custom.ini");
        CHECK(lp.fullscreen);
        CHECK(lp.print_help);
        CHECK(lp.print_version);
        CHECK(lp.force_null_render);
        CHECK(lp.force_single_core);
        CHECK(lp.input_profile == "custom_profile");
        CHECK(lp.program_args == "--arg=1");
        CHECK(lp.filepath == "title.nsp");
    }

    {
        const auto lp = RunParse(system, {"eglantine", "--program=--flag=2"});
        CHECK(lp.program_args == "--flag=2");
    }

    {
        const auto lp = RunParse(system, {"eglantine", "--program"});
        CHECK(lp.program_args.empty());
    }

    {
        const auto lp = RunParse(system, {"eglantine", "-x", "*:Debug"});
        REQUIRE(lp.log_filter.has_value());
        CHECK(*lp.log_filter == "*:Debug");
    }

    {
        const auto lp = RunParse(system, {"eglantine", "-m", "Tester_1:pass123@10.0.0.5:25555"});
        CHECK(lp.use_multiplayer);
        CHECK(lp.nickname == "Tester_1");
        CHECK(lp.password == "pass123");
        CHECK(lp.address == "10.0.0.5");
        CHECK(lp.port == u16{25555});
    }

    {
        const auto lp = RunParse(system, {"eglantine", "-m", "Tester_2@10.0.0.5"});
        CHECK(lp.use_multiplayer);
        CHECK(lp.nickname == "Tester_2");
        CHECK(lp.password.empty());
        CHECK(lp.address == "10.0.0.5");
        CHECK(lp.port == Network::DefaultRoomPort);
    }

    {
        const auto lp = RunParse(system, {"eglantine", "-u", "PlayerTwo"});
        REQUIRE(lp.selected_user.has_value());
        CHECK(*lp.selected_user == 1);
        CHECK(Settings::values.current_user.GetValue() == 1);
    }

    {
        const auto lp = RunParse(system, {"eglantine", "-u", "0"});
        REQUIRE(lp.selected_user.has_value());
        CHECK(*lp.selected_user == 0);
        CHECK(Settings::values.current_user.GetValue() == 0);
    }

    {
        const auto lp = RunParse(system, {"eglantine", "-u", "MissingUser"});
        CHECK_FALSE(lp.selected_user.has_value());
        CHECK(Settings::values.current_user.GetValue() == 0);
    }

    {
        const auto lp = RunParse(system, {"eglantine", "-u", "7"});
        REQUIRE(lp.selected_user.has_value());
        CHECK(*lp.selected_user == 7);
        CHECK(Settings::values.current_user.GetValue() == 0);
    }

    {
        const auto lp =
            RunParse(system, {"eglantine", "--", "-hlaunch", "-qlaunch", "-setup", "rom.nro"});
        CHECK(lp.launch_hlaunch);
        CHECK(lp.launch_qlaunch);
        CHECK(lp.launch_setup);
        CHECK(lp.filepath == "rom.nro");
    }

    {
        const auto lp = RunParse(system, {"eglantine", "rom.nro"});
        CHECK(lp.filepath == "rom.nro");
        CHECK_FALSE(lp.launch_hlaunch);
        CHECK_FALSE(lp.launch_qlaunch);
        CHECK_FALSE(lp.launch_setup);
    }

#ifdef _WIN32
    {
        std::vector<std::string> narrow_storage{"eglantine.exe", "-f", "-g", "ignored.nsp"};
        std::vector<std::wstring> wide_storage{L"eglantine.exe", L"-f", L"-g", L"wide_game.nsp"};
        std::vector<char*> argv;
        std::vector<wchar_t*> argv_w;
        argv.reserve(narrow_storage.size() + 1);
        argv_w.reserve(wide_storage.size() + 1);
        for (auto& s : narrow_storage) {
            argv.push_back(s.data());
        }
        for (auto& ws : wide_storage) {
            argv_w.push_back(ws.data());
        }
        argv.push_back(nullptr);
        argv_w.push_back(nullptr);

        const auto lp = Core::ParseLaunchParams(
            system, static_cast<int>(wide_storage.size()), argv.data(), argv_w.data());
        CHECK(lp.argv0 == "eglantine.exe");
        CHECK(lp.fullscreen);
        CHECK(lp.filepath == "wide_game.nsp");
    }
#endif
}

TEST_CASE("ApplyLaunchParams applies settings and clamps selected user", "[core][launch_params]") {
    const std::string prev_program_args = Settings::values.program_args.GetValue();
    const std::string prev_player0_profile = Settings::values.players.GetValue()[0].profile_name;
    const s32 prev_current_user = Settings::values.current_user.GetValue();
    const bool prev_use_gdbstub = Settings::values.use_gdbstub.GetValue();
    const u16 prev_gdbstub_port = Settings::values.gdbstub_port.GetValue();
    const bool prev_use_multi_core = Settings::values.use_multi_core.GetValue();
    const Settings::RendererBackend prev_renderer_backend =
        Settings::values.renderer_backend.GetValue();
    const std::string prev_log_filter = Settings::values.log_filter.GetValue();

    SCOPE_EXIT {
        Settings::values.program_args.SetValue(prev_program_args);
        Settings::values.players.GetValue()[0].profile_name = prev_player0_profile;
        Settings::values.current_user.SetValue(prev_current_user);
        Settings::values.use_gdbstub.SetValue(prev_use_gdbstub);
        Settings::values.gdbstub_port.SetValue(prev_gdbstub_port);
        Settings::values.use_multi_core.SetValue(prev_use_multi_core);
        Settings::values.renderer_backend.SetValue(prev_renderer_backend);
        Settings::values.log_filter.SetValue(prev_log_filter);

        Common::Log::Filter filter{};
        filter.ParseFilterString(prev_log_filter);
        Common::Log::SetGlobalFilter(filter);
    };

    Settings::values.program_args.SetValue({});
    Settings::values.players.GetValue()[0].profile_name.clear();
    Settings::values.current_user.SetValue(2);
    Settings::values.use_gdbstub.SetValue(false);
    Settings::values.gdbstub_port.SetValue(6543);
    Settings::values.use_multi_core.SetValue(true);
    Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Vulkan);
    Settings::values.log_filter.SetValue("*:Info");

    {
        const Core::LaunchParams empty_lp{};
        Core::ApplyLaunchParams(empty_lp);
        CHECK(Settings::values.program_args.GetValue().empty());
        CHECK(Settings::values.players.GetValue()[0].profile_name.empty());
        CHECK(Settings::values.current_user.GetValue() == 2);
        CHECK_FALSE(Settings::values.use_gdbstub.GetValue());
        CHECK(Settings::values.gdbstub_port.GetValue() == u16{6543});
        CHECK(Settings::values.use_multi_core.GetValue());
        CHECK(Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Vulkan);
    }

    {
        Core::LaunchParams lp{};
        lp.program_args = "--arg=1";
        lp.input_profile = "custom_profile";
        lp.selected_user = 99;
        lp.enable_gdbstub = true;
        lp.override_gdb_port = u16{7777};
        lp.force_single_core = true;
        lp.force_null_render = true;
        lp.log_filter = "*:Debug";

        Core::ApplyLaunchParams(lp);
        CHECK(Settings::values.program_args.GetValue() == "--arg=1");
        CHECK(Settings::values.players.GetValue()[0].profile_name == "custom_profile");
        CHECK(Settings::values.current_user.GetValue() == 7);
        CHECK(Settings::values.use_gdbstub.GetValue());
        CHECK(Settings::values.gdbstub_port.GetValue() == u16{7777});
        CHECK_FALSE(Settings::values.use_multi_core.GetValue());
        CHECK(Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Null);
    }

    {
        Core::LaunchParams lp{};
        lp.selected_user = -3;

        Core::ApplyLaunchParams(lp);
        CHECK(Settings::values.current_user.GetValue() == 0);
    }
}
