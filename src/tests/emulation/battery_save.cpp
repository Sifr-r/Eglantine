// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "emulation/classic/battery_save.h"

TEST_CASE("Classic battery saves replace existing payloads", "[classic-battery]") {
    using Emulation::Classic::ReadBatterySave;
    using Emulation::Classic::WriteBatterySave;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
                      ("eglantine-classic-battery-" + std::to_string(stamp));
    const auto path = root / "game-boy" / "game.sav";
    SCOPE_EXIT {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    };

    std::vector<std::uint8_t> data{5};
    std::string error;
    REQUIRE(ReadBatterySave(path, data, error));
    REQUIRE(data.empty());
    REQUIRE(WriteBatterySave(path, std::array<std::uint8_t, 4>{1, 2, 3, 4}, error));
    REQUIRE(WriteBatterySave(path, std::array<std::uint8_t, 2>{9, 8}, error));
    REQUIRE(ReadBatterySave(path, data, error));
    CHECK(data == std::vector<std::uint8_t>{9, 8});

    REQUIRE_FALSE(WriteBatterySave(path, std::vector<std::uint8_t>(1024 * 1024 + 1), error));
    REQUIRE_FALSE(error.empty());
    REQUIRE(ReadBatterySave(path, data, error));
    CHECK(data == std::vector<std::uint8_t>{9, 8});
    CHECK(std::distance(std::filesystem::directory_iterator(path.parent_path()),
                        std::filesystem::directory_iterator()) == 1);
}
