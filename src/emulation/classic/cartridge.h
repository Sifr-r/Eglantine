// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Emulation::Classic {

enum class ClassicCartridge { GameBoy, GameBoyColor, GameBoyAdvance };

struct ClassicCartridgeMetadata {
    ClassicCartridge system;
    std::string internal_title;
    std::string game_code;
    std::string publisher;
    std::string region;
    std::string version;
    std::string hardware;
    std::uint64_t file_size{};
};

using ReadCartridgeHeader = std::function<std::size_t(std::span<std::uint8_t>)>;

[[nodiscard]] constexpr bool IsClassicCartridgeExtension(std::string_view extension) {
    return extension == "gb" || extension == "gbc" || extension == "gba";
}

[[nodiscard]] std::optional<ClassicCartridgeMetadata> ReadClassicCartridgeMetadata(
    std::string_view extension, std::size_t size, const ReadCartridgeHeader& read_header);

[[nodiscard]] std::optional<ClassicCartridge> IdentifyClassicCartridge(
    std::string_view extension, std::size_t size, const ReadCartridgeHeader& read_header);

} // namespace Emulation::Classic
