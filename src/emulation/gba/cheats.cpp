// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "emulation/gba/cheats.h"

#include <charconv>

namespace Emulation::Gba {
namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.remove_suffix(1);
    }
    return s;
}

bool ParseHexU32(std::string_view token, std::uint32_t& out) {
    if (token.empty() || token.size() > 8) {
        return false;
    }
    std::uint32_t value = 0;
    const auto* begin = token.data();
    const auto* end = begin + token.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value, 16);
    if (ec != std::errc{} || ptr != end) {
        return false;
    }
    out = value;
    return true;
}

bool ParseSingleLine(std::string_view line, CheatEntry& out, std::string& error) {
    if (const auto colon = line.find(':'); colon != std::string_view::npos) {
        const auto left = Trim(line.substr(0, colon));
        const auto right = Trim(line.substr(colon + 1));
        std::uint32_t addr = 0;
        if (!ParseHexU32(left, addr)) {
            error = "Invalid cheat code: Invalid address in '" + std::string(line) + "'";
            return false;
        }
        std::uint32_t val = 0;
        if (right.size() <= 4) {
            if (!ParseHexU32(right, val) || val > 0xFFFFu) {
                error = "Invalid cheat code: Invalid value in '" + std::string(line) + "'";
                return false;
            }
            out = CheatEntry{CheatEntry::Kind::Raw16, addr, val};
            return true;
        }
        if (!ParseHexU32(right, val)) {
            error = "Invalid cheat code: Invalid value in '" + std::string(line) + "'";
            return false;
        }
        out = CheatEntry{CheatEntry::Kind::Raw32, addr, val};
        return true;
    }

    std::string_view parts[3];
    std::size_t count = 0;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
            ++i;
        }
        if (i >= line.size()) {
            break;
        }
        const std::size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') {
            ++i;
        }
        if (count < 3) {
            parts[count] = line.substr(start, i - start);
        }
        ++count;
        if (count > 2) {
            break;
        }
    }

    if (count != 2) {
        error = "Invalid cheat code: Expected 'XXXXXXXX YYYYYYYY', 'XXXXXXXX YYYY', or 'ADDR:VAL', got '" +
                std::string(line) + "'";
        return false;
    }

    std::uint32_t op = 0;
    if (!ParseHexU32(parts[0], op)) {
        error = "Invalid cheat code: Invalid hex token '" + std::string(parts[0]) + "'";
        return false;
    }
    std::uint32_t val = 0;
    if (!ParseHexU32(parts[1], val)) {
        error = "Invalid cheat code: Invalid hex token '" + std::string(parts[1]) + "'";
        return false;
    }

    if (parts[1].size() <= 4) {
        // CodeBreaker format: TAAAAAAA YYYY
        const std::uint32_t code_type = (op >> 28) & 0xFu;
        const std::uint32_t addr = op & 0x0FFFFFFFu;
        const std::uint16_t v16 = static_cast<std::uint16_t>(val & 0xFFFFu);
        switch (code_type) {
        case 0x3:
            out = CheatEntry{CheatEntry::Kind::CodeBreakerWrite8, addr, v16 & 0xFFu};
            break;
        case 0x7:
            out = CheatEntry{CheatEntry::Kind::CodeBreakerIfEqual16, addr, v16};
            break;
        case 0x2:
            out = CheatEntry{CheatEntry::Kind::CodeBreakerAdd16, addr, v16};
            break;
        case 0x8:
        default:
            out = CheatEntry{CheatEntry::Kind::CodeBreakerWrite16, addr, v16};
            break;
        }
        return true;
    }

    // GameShark / Action Replay v3 raw format: TTAAAAAA VVVVVVVV
    const std::uint32_t code_type = (op >> 24) & 0xFFu;
    const std::uint32_t addr = 0x02000000u | (op & 0x00FFFFFFu);
    switch (code_type) {
    case 0x00:
        out = CheatEntry{CheatEntry::Kind::GameSharkWrite8, addr, val & 0xFFu};
        break;
    case 0x01:
        out = CheatEntry{CheatEntry::Kind::GameSharkWrite16, addr, val & 0xFFFFu};
        break;
    case 0x02:
        out = CheatEntry{CheatEntry::Kind::GameSharkWrite32, addr, val};
        break;
    case 0xD0:
        out = CheatEntry{CheatEntry::Kind::GameSharkIfEqual16, addr, val & 0xFFFFu};
        break;
    default:
        if (code_type == 0x03) {
            out = CheatEntry{CheatEntry::Kind::GameSharkWrite32,
                             0x03000000u | (op & 0x00FFFFFFu), val};
        } else {
            out = CheatEntry{CheatEntry::Kind::GameSharkWrite32, op & 0x0FFFFFFFu, val};
        }
        break;
    }
    return true;
}

} // namespace

bool CheatEngine::AddCode(std::string_view raw, std::string& error) {
    const auto trimmed = Trim(raw);
    if (trimmed.empty()) {
        error = "Invalid cheat code: Empty cheat code";
        return false;
    }

    std::vector<CheatEntry> parsed;
    std::size_t pos = 0;
    while (pos < trimmed.size()) {
        std::size_t end = trimmed.find('\n', pos);
        if (end == std::string_view::npos) {
            end = trimmed.size();
        }
        const auto line = Trim(trimmed.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line.starts_with('#') || line.starts_with("//")) {
            continue;
        }
        CheatEntry entry{};
        if (!ParseSingleLine(line, entry, error)) {
            return false;
        }
        parsed.push_back(entry);
    }
    codes_.insert(codes_.end(), parsed.begin(), parsed.end());
    error.clear();
    return true;
}

bool CheatEngine::ReplaceCodes(std::span<const std::string> codes, std::string& error) {
    CheatEngine next;
    for (const auto& code : codes) {
        if (!next.AddCode(code, error)) {
            return false;
        }
    }
    codes_ = std::move(next.codes_);
    error.clear();
    return true;
}

} // namespace Emulation::Gba
