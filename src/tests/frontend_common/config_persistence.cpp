// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/path_util.h"
#include "common/scope_exit.h"
#include "frontend_common/config.h"

namespace {

class TestConfig final : public Config {
public:
    explicit TestConfig(const std::filesystem::path& path) {
        config_loc = Common::FS::PathToUTF8String(path);
        SetUpIni();
    }

    void SetValue(const char* value) {
        REQUIRE(config->SetValue("Test", "Value", value) >= 0);
        WriteToIni();
    }

private:
    void ReloadAllValues() override {}
    void SaveAllValues() override {}
    void ReadHidbusValues() override {}
    void ReadDebugControlValues() override {}
    void ReadPathValues() override {}
    void ReadShortcutValues() override {}
    void ReadUIValues() override {}
    void ReadUIGamelistValues() override {}
    void ReadUILayoutValues() override {}
    void ReadMultiplayerValues() override {}
    void SaveHidbusValues() override {}
    void SaveDebugControlValues() override {}
    void SavePathValues() override {}
    void SaveShortcutValues() override {}
    void SaveUIValues() override {}
    void SaveUIGamelistValues() override {}
    void SaveUILayoutValues() override {}
    void SaveMultiplayerValues() override {}
    std::vector<Settings::BasicSetting*>& FindRelevantList(Settings::Category) override {
        return settings;
    }

    std::vector<Settings::BasicSetting*> settings;
};

std::string ReadBinaryFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>{input}, {}};
}

std::ptrdiff_t CountEntries(const std::filesystem::path& directory) {
    return std::distance(std::filesystem::directory_iterator(directory),
                         std::filesystem::directory_iterator{});
}

} // namespace

TEST_CASE("Config saves and overwrites INI atomically without leftover temporary files",
          "[frontend_common][config_persistence]") {
    const auto root = std::filesystem::temp_directory_path() /
                      ("eglantine-config-persistence-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(root));
    SCOPE_EXIT {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    };

    const auto path = root / "persistence.ini";
    TestConfig config{path};

    config.SetValue("before");
    const auto first = ReadBinaryFile(path);
    CHECK(first.find("before") != std::string::npos);
    CHECK(CountEntries(root) == 1);

    config.SetValue("after");
    const auto second = ReadBinaryFile(path);
    CHECK(second.find("after") != std::string::npos);
    CHECK(second.find("before") == std::string::npos);
    CHECK(CountEntries(root) == 1);

    REQUIRE(std::filesystem::remove(path));
    config.SetValue("recreated");
    CHECK(ReadBinaryFile(path).find("recreated") != std::string::npos);
    CHECK(CountEntries(root) == 1);
}

TEST_CASE("Config leaves a read-only INI untouched and leaves no temporary files behind",
          "[frontend_common][config_persistence]") {
    const auto root = std::filesystem::temp_directory_path() /
                      ("eglantine-config-read-only-" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(root));
    const auto path = root / "read-only.ini";
    SCOPE_EXIT {
        std::error_code error;
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, error);
        std::filesystem::remove_all(root, error);
    };

    TestConfig config{path};
    config.SetValue("kept");
    const auto previous = ReadBinaryFile(path);
    REQUIRE(previous.find("kept") != std::string::npos);

    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_write |
                                     std::filesystem::perms::group_write |
                                     std::filesystem::perms::others_write,
                                 std::filesystem::perm_options::remove);
    if (std::fstream{path, std::ios::binary | std::ios::in | std::ios::out}.is_open()) {
        std::error_code error;
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, error);
        WARN("file permissions do not block writes on this host");
        return;
    }

    config.SetValue("changed");

    CHECK(ReadBinaryFile(path) == previous);
    CHECK(CountEntries(root) == 1);
}
