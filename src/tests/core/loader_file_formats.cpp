// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "core/loader/loader.h"

TEST_CASE("Loader file extensions preserve frontend scan policies", "[loader-formats]") {
    using Loader::FileExtensionUse;
    using Loader::FileType;

    const std::vector<std::string> desktop{
        "nro", "nso", "nca", "xci", "xcz", "nsp", "nsz", "nup", "kip",
    };
    const std::vector<std::string> android_games{"nro", "nca", "xci", "xcz", "nsp", "nsz", "nup"};
    const std::vector<std::string> android_content{"xci", "xcz", "nsp", "nsz"};

    CHECK(Loader::GetFileExtensions(FileExtensionUse::DesktopGame) == desktop);
    CHECK(Loader::GetFileExtensions(FileExtensionUse::AndroidGame) == android_games);
    CHECK(Loader::GetFileExtensions(FileExtensionUse::AndroidExternalContent) == android_content);

    const std::pair<const char*, FileType> recognized[]{
        {"title.nro", FileType::NRO}, {"title.nso", FileType::NSO}, {"title.nca", FileType::NCA},
        {"title.xci", FileType::XCI}, {"title.xcz", FileType::XCZ}, {"title.nsp", FileType::NSP},
        {"title.nsz", FileType::NSZ}, {"title.nup", FileType::NUP}, {"title.kip", FileType::KIP},
    };
    for (const auto& [filename, type] : recognized) {
        CHECK(Loader::GuessFromFilename(filename) == type);
    }
}

TEST_CASE("Loader filename guesses retain special names and ignore extension case",
          "[loader-formats]") {
    using Loader::FileType;

    CHECK(Loader::GuessFromFilename("title.NuP") == FileType::NUP);
    CHECK(Loader::GuessFromFilename("main") == FileType::DeconstructedRomDirectory);
    CHECK(Loader::GuessFromFilename("00") == FileType::NCA);
    CHECK(Loader::GuessFromFilename("title.nax") == FileType::Unknown);
    CHECK(Loader::GuessFromFilename("title.unknown") == FileType::Unknown);
}
