// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "emulation/classic/cartridge.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <numeric>
#include <utility>

namespace Emulation::Classic {
namespace {

constexpr std::size_t GameBoyHeaderSize = 0x150;
constexpr std::size_t AdvanceHeaderSize = 0xC0;
constexpr std::array<std::uint8_t, 4> GameBoyLogoPrefix{0xCE, 0xED, 0x66, 0x66};
constexpr std::array<std::uint8_t, 4> AdvanceLogoPrefix{0x24, 0xFF, 0xAE, 0x51};

bool ValidHeaderChecksum(std::span<const std::uint8_t> bytes, std::uint8_t expected) {
    const auto sum = std::accumulate(bytes.begin(), bytes.end(), 0x19u);
    return static_cast<std::uint8_t>(0u - sum) == expected;
}

std::size_t GameBoyDeclaredSize(std::uint8_t size_code) {
    if (size_code <= 8) {
        return std::size_t{32 * 1024} << size_code;
    }
    constexpr std::array<std::size_t, 3> ExtraBankCounts{72, 80, 96};
    if (size_code >= 0x52 && size_code <= 0x54) {
        return ExtraBankCounts[size_code - 0x52] * 16 * 1024;
    }
    return 0;
}

std::string CleanAscii(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (const std::uint8_t byte : bytes) {
        if (byte == 0x00) {
            break;
        }
        if (byte >= 0x20 && byte <= 0x7E) {
            out.push_back(static_cast<char>(byte));
        }
    }
    while (!out.empty() && out.front() == ' ') {
        out.erase(out.begin());
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

bool IsUpperAlnum(std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, [](unsigned char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
    });
}

std::string_view LookupNewLicensee(std::string_view code) {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 37> Table{{
        {"01", "Nintendo"},
        {"08", "Capcom"},
        {"13", "Electronic Arts"},
        {"18", "Hudson Soft"},
        {"19", "B-AI"},
        {"20", "KSS"},
        {"24", "PCM Complete"},
        {"25", "San-X"},
        {"28", "Kemco"},
        {"29", "SETA"},
        {"30", "Viacom"},
        {"33", "Ocean / Acclaim"},
        {"34", "Konami"},
        {"35", "HectorSoft"},
        {"37", "Taito"},
        {"38", "Hudson Soft"},
        {"39", "Banpresto"},
        {"41", "Ubisoft"},
        {"42", "Atlus"},
        {"44", "Malibu"},
        {"46", "Angel"},
        {"47", "Bullet-Proof Software"},
        {"49", "Irem"},
        {"52", "Activision"},
        {"54", "Konami"},
        {"5G", "Atlus"},
        {"64", "LucasArts"},
        {"69", "Electronic Arts"},
        {"78", "THQ"},
        {"7F", "Kemco"},
        {"8P", "Sega"},
        {"9B", "Tecmo"},
        {"A4", "Konami"},
        {"AF", "Namco"},
        {"B2", "Bandai"},
        {"C2", "Kemco"},
        {"GD", "Square Enix"},
    }};
    for (const auto& [key, name] : Table) {
        if (key == code) {
            return name;
        }
    }
    // Additional common 2-char maker codes shared across GB/GBA.
    constexpr std::array<std::pair<std::string_view, std::string_view>, 12> ExtraTable{{
        {"B4", "Enix"},
        {"B6", "HAL Laboratory"},
        {"B7", "SNK"},
        {"C0", "Taito"},
        {"C3", "Square"},
        {"C8", "Koei"},
        {"D9", "Banpresto"},
        {"DA", "Tomy"},
        {"E9", "Natsume"},
        {"EB", "Atlus"},
        {"EM", "Konami"},
        {"75", "SCi"},
    }};
    for (const auto& [key, name] : ExtraTable) {
        if (key == code) {
            return name;
        }
    }
    return {};
}

std::string_view LookupOldLicensee(std::uint8_t code) {
    switch (code) {
    case 0x01:
    case 0x31:
        return "Nintendo";
    case 0x08:
        return "Capcom";
    case 0x13:
    case 0x69:
        return "Electronic Arts";
    case 0x18:
    case 0x38:
        return "Hudson Soft";
    case 0x28:
    case 0x7F:
    case 0xC2:
        return "Kemco";
    case 0x34:
    case 0xA4:
        return "Konami";
    case 0x41:
        return "Ubisoft";
    case 0x52:
        return "Activision";
    case 0x64:
        return "LucasArts";
    case 0x78:
        return "THQ";
    case 0x8B:
        return "Bullet-Proof Software";
    case 0x9B:
        return "Tecmo";
    case 0xAF:
        return "Namco";
    case 0xB2:
        return "Bandai";
    case 0xB4:
        return "Enix";
    case 0xB6:
        return "HAL Laboratory";
    case 0xB7:
        return "SNK";
    case 0xC0:
        return "Taito";
    case 0xC3:
        return "Square";
    case 0xC8:
        return "Koei";
    case 0xD9:
        return "Banpresto";
    case 0xDA:
        return "Tomy";
    case 0xE9:
        return "Natsume";
    case 0xEB:
        return "Atlus";
    default:
        return {};
    }
}

std::string_view LookupAdvanceRegion(char region_code) {
    switch (region_code) {
    case 'E':
        return "USA";
    case 'J':
        return "Japan";
    case 'P':
    case 'X':
    case 'Y':
    case 'Z':
        return "Europe";
    case 'D':
        return "Germany";
    case 'F':
        return "France";
    case 'I':
        return "Italy";
    case 'S':
        return "Spain";
    case 'K':
        return "Korea";
    case 'C':
        return "China";
    case 'W':
        return "World";
    default:
        return {};
    }
}

std::string_view LookupGameBoyHardware(std::uint8_t type) {
    switch (type) {
    case 0x00:
        return "ROM Only";
    case 0x01:
        return "MBC1";
    case 0x02:
        return "MBC1 + RAM";
    case 0x03:
        return "MBC1 + RAM + Battery";
    case 0x05:
        return "MBC2";
    case 0x06:
        return "MBC2 + Battery";
    case 0x08:
        return "ROM + RAM";
    case 0x09:
        return "ROM + RAM + Battery";
    case 0x0B:
        return "MMM01";
    case 0x0C:
        return "MMM01 + RAM";
    case 0x0D:
        return "MMM01 + RAM + Battery";
    case 0x0F:
        return "MBC3 + Timer + Battery";
    case 0x10:
        return "MBC3 + Timer + RAM + Battery";
    case 0x11:
        return "MBC3";
    case 0x12:
        return "MBC3 + RAM";
    case 0x13:
        return "MBC3 + RAM + Battery";
    case 0x19:
        return "MBC5";
    case 0x1A:
        return "MBC5 + RAM";
    case 0x1B:
        return "MBC5 + RAM + Battery";
    case 0x1C:
        return "MBC5 + Rumble";
    case 0x1D:
        return "MBC5 + Rumble + RAM";
    case 0x1E:
        return "MBC5 + Rumble + RAM + Battery";
    case 0x20:
        return "MBC6";
    case 0x22:
        return "MBC7 + Sensor + Rumble + RAM + Battery";
    case 0xFC:
        return "Pocket Camera";
    case 0xFD:
        return "Bandai TAMA5";
    case 0xFE:
        return "HuC3";
    case 0xFF:
        return "HuC1 + RAM + Battery";
    default:
        return {};
    }
}

} // namespace

std::optional<ClassicCartridgeMetadata> ReadClassicCartridgeMetadata(
    std::string_view extension, std::size_t size, const ReadCartridgeHeader& read_header) {
    if (!IsClassicCartridgeExtension(extension) || !read_header) {
        return std::nullopt;
    }
    const bool advance = extension == "gba";
    const auto header_size = advance ? AdvanceHeaderSize : GameBoyHeaderSize;
    const auto max_size = advance ? 64 * 1024 * 1024u : 8 * 1024 * 1024u;
    if (size < header_size || size > max_size) {
        return std::nullopt;
    }

    std::array<std::uint8_t, GameBoyHeaderSize> header{};
    if (read_header(std::span{header}.first(header_size)) != header_size) {
        return std::nullopt;
    }
    const std::span<const std::uint8_t> bytes{header.data(), header_size};
    if (advance) {
        if (!std::ranges::equal(bytes.subspan<0x04, 4>(), AdvanceLogoPrefix) ||
            header[0xB2] != 0x96 ||
            !ValidHeaderChecksum(bytes.subspan<0xA0, 0x1D>(), header[0xBD])) {
            return std::nullopt;
        }
        ClassicCartridgeMetadata metadata{
            .system = ClassicCartridge::GameBoyAdvance,
            .internal_title = CleanAscii(bytes.subspan<0xA0, 12>()),
            .version = "1." + std::to_string(header[0xBC]),
            .file_size = static_cast<std::uint64_t>(size),
        };
        const auto code = CleanAscii(bytes.subspan<0xAC, 4>());
        if (code.size() == 4 && IsUpperAlnum(code)) {
            metadata.game_code = code;
            metadata.region = std::string{LookupAdvanceRegion(code[3])};
        }
        const auto maker = CleanAscii(bytes.subspan<0xB0, 2>());
        if (const auto publisher = LookupNewLicensee(maker); !publisher.empty()) {
            metadata.publisher = std::string{publisher};
        }
        return metadata;
    }

    const auto declared_size = GameBoyDeclaredSize(header[0x148]);
    if (declared_size == 0 || size < declared_size ||
        !std::ranges::equal(bytes.subspan<0x104, 4>(), GameBoyLogoPrefix) ||
        !ValidHeaderChecksum(bytes.subspan<0x134, 0x19>(), header[0x14D])) {
        return std::nullopt;
    }
    const std::uint8_t color_flag = header[0x143];
    const bool is_cgb = color_flag == 0x80 || color_flag == 0xC0;
    ClassicCartridgeMetadata metadata{
        .system = is_cgb ? ClassicCartridge::GameBoyColor : ClassicCartridge::GameBoy,
        .internal_title =
            is_cgb ? CleanAscii(bytes.subspan<0x134, 11>()) : CleanAscii(bytes.subspan<0x134, 16>()),
        .version = "1." + std::to_string(header[0x14C]),
        .hardware = std::string{LookupGameBoyHardware(header[0x147])},
        .file_size = static_cast<std::uint64_t>(size),
    };
    if (is_cgb) {
        const auto code = CleanAscii(bytes.subspan<0x13F, 4>());
        if (code.size() == 4 && IsUpperAlnum(code)) {
            metadata.game_code = code;
        }
    }
    if (header[0x14B] == 0x33) {
        const auto licensee = CleanAscii(bytes.subspan<0x144, 2>());
        if (const auto publisher = LookupNewLicensee(licensee); !publisher.empty()) {
            metadata.publisher = std::string{publisher};
        }
    } else if (const auto publisher = LookupOldLicensee(header[0x14B]); !publisher.empty()) {
        metadata.publisher = std::string{publisher};
    }
    if (header[0x14A] == 0x00) {
        metadata.region = "Japan";
    } else if (header[0x14A] == 0x01) {
        metadata.region = "Overseas";
    }
    return metadata;
}

std::optional<ClassicCartridge> IdentifyClassicCartridge(std::string_view extension, std::size_t size,
                                                         const ReadCartridgeHeader& read_header) {
    const auto metadata = ReadClassicCartridgeMetadata(extension, size, read_header);
    if (!metadata) {
        return std::nullopt;
    }
    return metadata->system;
}

} // namespace Emulation::Classic
