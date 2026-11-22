// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>

#include "common/common_types.h"
#include "core/hle/service/set/settings_types.h"

namespace Service::Set {

// Wire format for nn::kpr::KeyCodeMap: a 0x40-byte little-endian header followed
// by a fixed-stride table indexed by USB HID Usage Page 0x07 codes starting at
// usage 0x04 (KeyA), zero-padded to 0x1000 bytes.
using KeyCodeMap = std::array<u8, 0x1000>;

inline constexpr std::size_t KEY_CODE_MAP_HEADER_SIZE = 0x40;
inline constexpr u8 KEY_CODE_MAP_HID_BASE = 0x04;
inline constexpr u32 KEY_CODE_MAP_COUNT_STANDARD = 0x64;
inline constexpr u32 KEY_CODE_MAP_COUNT_JAPANESE = 0x88;

// Mode bitmasks stored at header offset 0x00. Each active 2-column mode adds
// (unshifted, shifted) UTF-16 code units per entry; AltGr modes add 1 column each.
inline constexpr u32 KEY_CODE_MAP_MODE_LATIN = 0x00000001U;
inline constexpr u32 KEY_CODE_MAP_MODE_HIRAGANA = 0x00000002U;
inline constexpr u32 KEY_CODE_MAP_MODE_KATAKANA = 0x00000004U;
inline constexpr u32 KEY_CODE_MAP_MODE_CYRILLIC = 0x00000008U;
inline constexpr u32 KEY_CODE_MAP_MODE_HANGUL = 0x00000010U;
inline constexpr u32 KEY_CODE_MAP_MODE_ZHUYIN = 0x00000020U;
inline constexpr u32 KEY_CODE_MAP_MODE_CANGJIE = 0x00000040U;
inline constexpr u32 KEY_CODE_MAP_MODE_ALTGR = 0x01000000U;
inline constexpr u32 KEY_CODE_MAP_MODE_ALTGR_SHIFT = 0x02000000U;

// Dead-key composition flag stored at header offset 0x0C.
inline constexpr u32 KEY_CODE_MAP_COMP_NONE = 0x00000000U;
inline constexpr u32 KEY_CODE_MAP_COMP_DEAD_KEYS = 0x00000003U;

// Per-entry modifier/lock behavior flags stored at column 0 of each key entry.
inline constexpr u16 KEY_CODE_FLAG_NONE = 0x0000U;
inline constexpr u16 KEY_CODE_FLAG_PRINTABLE = 0x1000U;
inline constexpr u16 KEY_CODE_FLAG_CAPSLOCK_LATIN = 0x1001U;
inline constexpr u16 KEY_CODE_FLAG_CAPSLOCK_ALTGR = 0x1002U;
inline constexpr u16 KEY_CODE_FLAG_CAPSLOCK_LATIN_ALTGR = 0x1003U;
inline constexpr u16 KEY_CODE_FLAG_CAPSLOCK_SECONDARY = 0x1010U;
inline constexpr u16 KEY_CODE_FLAG_CAPSLOCK_LATIN_SECONDARY = 0x1011U;
inline constexpr u16 KEY_CODE_FLAG_NUMLOCK = 0x20FFU;

// Unicode combining characters used as dead-key tokens in nn::kpr.
inline constexpr u16 DEAD_KEY_GRAVE = 0x0300U;
inline constexpr u16 DEAD_KEY_ACUTE = 0x0301U;
inline constexpr u16 DEAD_KEY_CIRCUMFLEX = 0x0302U;
inline constexpr u16 DEAD_KEY_TILDE = 0x0303U;
inline constexpr u16 DEAD_KEY_DIAERESIS = 0x0308U;
inline constexpr u16 DEAD_KEY_APOSTROPHE = 0x030DU;
inline constexpr u16 DEAD_KEY_QUOTATION = 0x030EU;
inline constexpr u16 DEAD_KEY_CEDILLA = 0x0327U;

namespace detail {

struct KeyCodeMapBuilder {
    KeyCodeMap data{};
    u32 columns{};
    u32 key_count{};

    constexpr KeyCodeMapBuilder(u32 mode_mask, u32 cols, u32 keys, u32 comp_flags)
        : columns{cols}, key_count{keys} {
        WriteU32(0x00U, mode_mask);
        WriteU32(0x04U, cols);
        WriteU32(0x08U, keys);
        WriteU32(0x0CU, comp_flags);
    }

    constexpr void WriteU16(std::size_t byte_offset, u16 value) {
        if (byte_offset + 1U >= data.size()) {
            return;
        }
        data[byte_offset] = static_cast<u8>(value & 0xFFU);
        data[byte_offset + 1U] = static_cast<u8>((value >> 8U) & 0xFFU);
    }

    constexpr void WriteU32(std::size_t byte_offset, u32 value) {
        WriteU16(byte_offset, static_cast<u16>(value & 0xFFFFU));
        WriteU16(byte_offset + 2U, static_cast<u16>((value >> 16U) & 0xFFFFU));
    }

    constexpr void SetKey(u8 hid, u16 flags, u16 c0 = 0, u16 c1 = 0, u16 c2 = 0, u16 c3 = 0,
                          u16 c4 = 0, u16 c5 = 0) {
        if (hid < KEY_CODE_MAP_HID_BASE) {
            return;
        }
        const u32 index = static_cast<u32>(hid - KEY_CODE_MAP_HID_BASE);
        if (index >= key_count) {
            return;
        }
        const std::size_t base =
            KEY_CODE_MAP_HEADER_SIZE + static_cast<std::size_t>(index) * columns * sizeof(u16);
        const std::array<u16, 7> words{{flags, c0, c1, c2, c3, c4, c5}};
        for (u32 col = 0; col < columns && col < words.size(); ++col) {
            WriteU16(base + static_cast<std::size_t>(col) * sizeof(u16), words[col]);
        }
    }

    // Populate a key whose symbol repeats across every active shift/mode column.
    constexpr void SetKeyUniform(u8 hid, u16 flags, u16 normal, u16 shift) {
        switch (columns) {
        case 3U:
            SetKey(hid, flags, normal, shift);
            break;
        case 4U:
            SetKey(hid, flags, normal, shift, normal);
            break;
        case 5U:
            SetKey(hid, flags, normal, shift, normal, shift);
            break;
        case 7U:
            SetKey(hid, flags, normal, shift, normal, shift, normal, shift);
            break;
        default:
            SetKey(hid, flags, normal, shift);
            break;
        }
    }

    constexpr void SetAlphaAscii(u8 hid, char lower, char upper, u16 altgr = 0,
                                 u16 altgr_shift = 0, u16 flags = KEY_CODE_FLAG_CAPSLOCK_LATIN) {
        SetKey(hid, flags, static_cast<u16>(static_cast<u8>(lower)),
               static_cast<u16>(static_cast<u8>(upper)), altgr, altgr_shift);
    }

    constexpr void PopulateUsLatinBase() {
        for (u8 offset = 0; offset < 26U; ++offset) {
            const u8 hid = static_cast<u8>(0x04U + offset);
            const u16 lower = static_cast<u16>('a' + offset);
            const u16 upper = static_cast<u16>('A' + offset);
            SetKey(hid, KEY_CODE_FLAG_CAPSLOCK_LATIN, lower, upper);
        }

        constexpr std::array<u16, 10> digit_shift{{'!', '@', '#', '$', '%', '^', '&', '*', '(', ')'}};
        for (u8 offset = 0; offset < 10U; ++offset) {
            const u8 hid = static_cast<u8>(0x1EU + offset);
            const u16 digit = static_cast<u16>(offset < 9U ? ('1' + offset) : '0');
            SetKey(hid, KEY_CODE_FLAG_PRINTABLE, digit, digit_shift[offset]);
        }

        SetKey(0x2DU, KEY_CODE_FLAG_PRINTABLE, '-', '_');
        SetKey(0x2EU, KEY_CODE_FLAG_PRINTABLE, '=', '+');
        SetKey(0x2FU, KEY_CODE_FLAG_PRINTABLE, '[', '{');
        SetKey(0x30U, KEY_CODE_FLAG_PRINTABLE, ']', '}');
        SetKey(0x31U, KEY_CODE_FLAG_PRINTABLE, '\\', '|');
        SetKey(0x33U, KEY_CODE_FLAG_PRINTABLE, ';', ':');
        SetKey(0x34U, KEY_CODE_FLAG_PRINTABLE, '\'', '"');
        SetKey(0x35U, KEY_CODE_FLAG_PRINTABLE, '`', '~');
        SetKey(0x36U, KEY_CODE_FLAG_PRINTABLE, ',', '<');
        SetKey(0x37U, KEY_CODE_FLAG_PRINTABLE, '.', '>');
        SetKey(0x38U, KEY_CODE_FLAG_PRINTABLE, '/', '?');
    }

    constexpr void PopulateCommonSpaceAndKeypad(u16 decimal_char = '.') {
        SetKeyUniform(0x2CU, KEY_CODE_FLAG_PRINTABLE, ' ', ' ');
        SetKeyUniform(0x54U, KEY_CODE_FLAG_PRINTABLE, '/', '/');
        SetKeyUniform(0x55U, KEY_CODE_FLAG_PRINTABLE, '*', '*');
        SetKeyUniform(0x56U, KEY_CODE_FLAG_PRINTABLE, '-', '-');
        SetKeyUniform(0x57U, KEY_CODE_FLAG_PRINTABLE, '+', '+');
        SetKeyUniform(0x67U, KEY_CODE_FLAG_PRINTABLE, '=', '=');

        for (u8 offset = 0; offset < 9U; ++offset) {
            const u8 hid = static_cast<u8>(0x59U + offset);
            const u16 digit = static_cast<u16>('1' + offset);
            SetKeyUniform(hid, KEY_CODE_FLAG_NUMLOCK, 0x0000U, digit);
        }
        SetKeyUniform(0x62U, KEY_CODE_FLAG_NUMLOCK, 0x0000U, '0');
        SetKeyUniform(0x63U, KEY_CODE_FLAG_NUMLOCK, 0x0000U, decimal_char);
    }
};

constexpr KeyCodeMap BuildEnglishUsInternational() {
    KeyCodeMapBuilder b{KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_ALTGR |
                            KEY_CODE_MAP_MODE_ALTGR_SHIFT,
                        5U, KEY_CODE_MAP_COUNT_STANDARD, KEY_CODE_MAP_COMP_DEAD_KEYS};
    b.PopulateUsLatinBase();
    b.PopulateCommonSpaceAndKeypad();

    // US-International dead keys on 6/^ (circumflex), '/" (acute/diaeresis), and `/~ (grave/tilde).
    b.SetKey(0x23U, KEY_CODE_FLAG_PRINTABLE, '6', DEAD_KEY_CIRCUMFLEX, 0x00BCU, 0x0000U);
    b.SetKey(0x34U, KEY_CODE_FLAG_PRINTABLE, DEAD_KEY_APOSTROPHE, DEAD_KEY_QUOTATION, 0x00B4U,
             0x00A8U);
    b.SetKey(0x35U, KEY_CODE_FLAG_PRINTABLE, DEAD_KEY_GRAVE, DEAD_KEY_TILDE, 0x0000U, 0x0000U);

    // US-International AltGr / AltGr+Shift characters (CapsLock also affects accented letters).
    b.SetKey(0x04U, KEY_CODE_FLAG_CAPSLOCK_LATIN_ALTGR, 'a', 'A', 0x00E1U, 0x00C1U);
    b.SetKey(0x06U, KEY_CODE_FLAG_CAPSLOCK_LATIN, 'c', 'C', 0x00A9U, 0x00A2U);
    b.SetKey(0x07U, KEY_CODE_FLAG_CAPSLOCK_LATIN_ALTGR, 'd', 'D', 0x00F0U, 0x00D0U);
    b.SetKey(0x08U, KEY_CODE_FLAG_CAPSLOCK_LATIN_ALTGR, 'e', 'E', 0x00E9U, 0x00C9U);
    b.SetKey(0x0CU, KEY_CODE_FLAG_CAPSLOCK_LATIN_ALTGR, 'i', 'I', 0x00EDU, 0x00CDU);
    b.SetKey(0x0FU, KEY_CODE_FLAG_CAPSLOCK_LATIN_ALTGR, 'l', 'L', 0x00F8U, 0x00D8U);
    b.SetKey(0x10U, KEY_CODE_FLAG_CAPSLOCK_LATIN, 'm', 'M', 0x00B5U, 0x0000U);
    b.SetKey(0x11U, KEY_CODE_FLAG_CAPSLOCK_LATIN_ALTGR, 'n', 'N', 0x00F1U, 0x00D1U);
    b.SetKey(0x12U, KEY_CODE_FLAG_CAPSLOCK_LATIN_ALTGR, 'o', 'O', 0x00F3U, 0x00D3U);
    b.SetKey(0x13U, KEY_CODE_FLAG_CAPSLOCK_LATIN_ALTGR, 'p', 'P', 0x00F6U, 0x00D6U);
    b.SetKey(0x14U, KEY_CODE_FLAG_CAPSLOCK_LATIN_ALTGR, 'q', 'Q', 0x00E4U, 0x00C4U);
    b.SetKey(0x15U, KEY_CODE_FLAG_CAPSLOCK_LATIN, 'r', 'R', 0x00AEU, 0x0000U);
    b.SetKey(0x16U, KEY_CODE_FLAG_CAPSLOCK_LATIN, 's', 'S', 0x00DFU, 0x00A7U);
    b.SetKey(0x17U, KEY_CODE_FLAG_CAPSLOCK_LATIN_ALTGR, 't', 'T', 0x00FEU, 0x00DEU);
    b.SetKey(0x18U, KEY_CODE_FLAG_CAPSLOCK_LATIN_ALTGR, 'u', 'U', 0x00FAU, 0x00DAU);
    b.SetKey(0x1AU, KEY_CODE_FLAG_CAPSLOCK_LATIN_ALTGR, 'w', 'W', 0x00E5U, 0x00C5U);
    b.SetKey(0x1CU, KEY_CODE_FLAG_CAPSLOCK_LATIN_ALTGR, 'y', 'Y', 0x00FCU, 0x00DCU);
    b.SetKey(0x1DU, KEY_CODE_FLAG_CAPSLOCK_LATIN_ALTGR, 'z', 'Z', 0x00E6U, 0x00C6U);

    b.SetKey(0x1EU, KEY_CODE_FLAG_PRINTABLE, '1', '!', 0x00A1U, 0x00B9U);
    b.SetKey(0x1FU, KEY_CODE_FLAG_PRINTABLE, '2', '@', 0x00B2U, 0x0000U);
    b.SetKey(0x20U, KEY_CODE_FLAG_PRINTABLE, '3', '#', 0x00B3U, 0x0000U);
    b.SetKey(0x21U, KEY_CODE_FLAG_PRINTABLE, '4', '$', 0x00A4U, 0x00A3U);
    b.SetKey(0x22U, KEY_CODE_FLAG_PRINTABLE, '5', '%', 0x20ACU, 0x0000U);
    b.SetKey(0x24U, KEY_CODE_FLAG_PRINTABLE, '7', '&', 0x00BDU, 0x0000U);
    b.SetKey(0x25U, KEY_CODE_FLAG_PRINTABLE, '8', '*', 0x00BEU, 0x0000U);
    b.SetKey(0x26U, KEY_CODE_FLAG_PRINTABLE, '9', '(', 0x2018U, 0x0000U);
    b.SetKey(0x27U, KEY_CODE_FLAG_PRINTABLE, '0', ')', 0x2019U, 0x0000U);
    b.SetKey(0x2DU, KEY_CODE_FLAG_PRINTABLE, '-', '_', 0x00A5U, 0x0000U);
    b.SetKey(0x2EU, KEY_CODE_FLAG_PRINTABLE, '=', '+', 0x00D7U, 0x00F7U);
    b.SetKey(0x2FU, KEY_CODE_FLAG_PRINTABLE, '[', '{', 0x00ABU, 0x0000U);
    b.SetKey(0x30U, KEY_CODE_FLAG_PRINTABLE, ']', '}', 0x00BBU, 0x0000U);
    b.SetKey(0x31U, KEY_CODE_FLAG_PRINTABLE, '\\', '|', 0x00ACU, 0x00A6U);
    b.SetKey(0x33U, KEY_CODE_FLAG_PRINTABLE, ';', ':', 0x00B6U, 0x00B0U);
    b.SetKey(0x36U, KEY_CODE_FLAG_CAPSLOCK_ALTGR, ',', '<', 0x00E7U, 0x00C7U);
    b.SetKey(0x38U, KEY_CODE_FLAG_PRINTABLE, '/', '?', 0x00BFU, 0x0000U);
    b.SetKey(0x64U, KEY_CODE_FLAG_PRINTABLE, '\\', '|', 0x0000U, 0x0000U);
    return b.data;
}

constexpr KeyCodeMap BuildEnglishUk() {
    KeyCodeMapBuilder b{KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_ALTGR |
                            KEY_CODE_MAP_MODE_ALTGR_SHIFT,
                        5U, KEY_CODE_MAP_COUNT_STANDARD, KEY_CODE_MAP_COMP_DEAD_KEYS};
    b.PopulateUsLatinBase();
    b.PopulateCommonSpaceAndKeypad();

    b.SetKey(0x1FU, KEY_CODE_FLAG_PRINTABLE, '2', '"', 0x0000U, 0x0000U);
    b.SetKey(0x20U, KEY_CODE_FLAG_PRINTABLE, '3', 0x00A3U, 0x0000U, 0x0000U);
    b.SetKey(0x21U, KEY_CODE_FLAG_PRINTABLE, '4', '$', 0x20ACU, 0x20ACU);
    b.SetKey(0x31U, KEY_CODE_FLAG_PRINTABLE, '#', '~', 0x0000U, 0x0000U);
    b.SetKey(0x32U, KEY_CODE_FLAG_PRINTABLE, '#', '~', 0x0000U, 0x0000U);
    b.SetKey(0x34U, KEY_CODE_FLAG_PRINTABLE, '\'', '@', 0x0000U, 0x0000U);
    b.SetKey(0x35U, KEY_CODE_FLAG_PRINTABLE, '`', 0x00ACU, 0x00A6U, 0x00A6U);
    b.SetKey(0x64U, KEY_CODE_FLAG_PRINTABLE, '\\', '|', 0x0000U, 0x0000U);

    b.SetKey(0x04U, KEY_CODE_FLAG_CAPSLOCK_LATIN_ALTGR, 'a', 'A', 0x00E1U, 0x00C1U);
    b.SetKey(0x08U, KEY_CODE_FLAG_CAPSLOCK_LATIN_ALTGR, 'e', 'E', 0x00E9U, 0x00C9U);
    b.SetKey(0x0CU, KEY_CODE_FLAG_CAPSLOCK_LATIN_ALTGR, 'i', 'I', 0x00EDU, 0x00CDU);
    b.SetKey(0x12U, KEY_CODE_FLAG_CAPSLOCK_LATIN_ALTGR, 'o', 'O', 0x00F3U, 0x00D3U);
    b.SetKey(0x18U, KEY_CODE_FLAG_CAPSLOCK_LATIN_ALTGR, 'u', 'U', 0x00FAU, 0x00DAU);
    return b.data;
}

constexpr KeyCodeMap BuildFrench() {
    KeyCodeMapBuilder b{KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_ALTGR, 4U,
                        KEY_CODE_MAP_COUNT_STANDARD, KEY_CODE_MAP_COMP_DEAD_KEYS};
    b.PopulateUsLatinBase();
    b.PopulateCommonSpaceAndKeypad();

    // French AZERTY swaps A/Q, Z/W, moves M to ';' key, and places digits on Shift.
    b.SetAlphaAscii(0x14U, 'a', 'A');
    b.SetAlphaAscii(0x04U, 'q', 'Q');
    b.SetAlphaAscii(0x1AU, 'z', 'Z');
    b.SetAlphaAscii(0x1DU, 'w', 'W');
    b.SetAlphaAscii(0x08U, 'e', 'E', 0x20ACU);
    b.SetAlphaAscii(0x33U, 'm', 'M');
    b.SetKey(0x10U, KEY_CODE_FLAG_PRINTABLE, ',', '?');
    b.SetKey(0x36U, KEY_CODE_FLAG_PRINTABLE, ';', '.');
    b.SetKey(0x37U, KEY_CODE_FLAG_PRINTABLE, ':', '/');
    b.SetKey(0x38U, KEY_CODE_FLAG_PRINTABLE, '!', 0x00A7U);

    b.SetKey(0x1EU, KEY_CODE_FLAG_PRINTABLE, '&', '1');
    b.SetKey(0x1FU, KEY_CODE_FLAG_PRINTABLE, 0x00E9U, '2', '~');
    b.SetKey(0x20U, KEY_CODE_FLAG_PRINTABLE, '"', '3', '#');
    b.SetKey(0x21U, KEY_CODE_FLAG_PRINTABLE, '\'', '4', '{');
    b.SetKey(0x22U, KEY_CODE_FLAG_PRINTABLE, '(', '5', '[');
    b.SetKey(0x23U, KEY_CODE_FLAG_PRINTABLE, '-', '6', '|');
    b.SetKey(0x24U, KEY_CODE_FLAG_PRINTABLE, 0x00E8U, '7', '`');
    b.SetKey(0x25U, KEY_CODE_FLAG_PRINTABLE, '_', '8', '\\');
    b.SetKey(0x26U, KEY_CODE_FLAG_PRINTABLE, 0x00E7U, '9', '^');
    b.SetKey(0x27U, KEY_CODE_FLAG_PRINTABLE, 0x00E0U, '0', '@');

    b.SetKey(0x2DU, KEY_CODE_FLAG_PRINTABLE, ')', 0x00B0U, ']');
    b.SetKey(0x2EU, KEY_CODE_FLAG_PRINTABLE, '=', '+', '}');
    b.SetKey(0x2FU, KEY_CODE_FLAG_PRINTABLE, DEAD_KEY_CIRCUMFLEX, DEAD_KEY_DIAERESIS);
    b.SetKey(0x30U, KEY_CODE_FLAG_PRINTABLE, '$', 0x00A3U, 0x00A4U);
    b.SetKey(0x31U, KEY_CODE_FLAG_PRINTABLE, '*', 0x00B5U);
    b.SetKey(0x32U, KEY_CODE_FLAG_PRINTABLE, '*', 0x00B5U);
    b.SetKey(0x34U, KEY_CODE_FLAG_PRINTABLE, 0x00F9U, '%');
    b.SetKey(0x35U, KEY_CODE_FLAG_PRINTABLE, 0x00B2U, 0x0000U);
    b.SetKey(0x64U, KEY_CODE_FLAG_PRINTABLE, '<', '>');
    return b.data;
}

constexpr KeyCodeMap BuildFrenchCa() {
    KeyCodeMapBuilder b{KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_ALTGR, 4U,
                        KEY_CODE_MAP_COUNT_STANDARD, KEY_CODE_MAP_COMP_DEAD_KEYS};
    b.PopulateUsLatinBase();
    b.PopulateCommonSpaceAndKeypad();

    b.SetKey(0x1EU, KEY_CODE_FLAG_PRINTABLE, '1', '!', 0x00B1U);
    b.SetKey(0x1FU, KEY_CODE_FLAG_PRINTABLE, '2', '"', '@');
    b.SetKey(0x20U, KEY_CODE_FLAG_PRINTABLE, '3', '/', 0x00A3U);
    b.SetKey(0x21U, KEY_CODE_FLAG_PRINTABLE, '4', '$', 0x00A2U);
    b.SetKey(0x22U, KEY_CODE_FLAG_PRINTABLE, '5', '%', 0x00A4U);
    b.SetKey(0x23U, KEY_CODE_FLAG_PRINTABLE, '6', '?', 0x00ACU);
    b.SetKey(0x24U, KEY_CODE_FLAG_PRINTABLE, '7', '&', 0x00A6U);
    b.SetKey(0x25U, KEY_CODE_FLAG_PRINTABLE, '8', '*', 0x00B2U);
    b.SetKey(0x26U, KEY_CODE_FLAG_PRINTABLE, '9', '(', 0x00B3U);
    b.SetKey(0x27U, KEY_CODE_FLAG_PRINTABLE, '0', ')', 0x00BCU);

    b.SetKey(0x2DU, KEY_CODE_FLAG_PRINTABLE, '-', '_', 0x00BDU);
    b.SetKey(0x2EU, KEY_CODE_FLAG_PRINTABLE, '=', '+', 0x00BEU);
    b.SetKey(0x2FU, KEY_CODE_FLAG_PRINTABLE, DEAD_KEY_CIRCUMFLEX, DEAD_KEY_DIAERESIS, '[');
    b.SetKey(0x30U, KEY_CODE_FLAG_PRINTABLE, DEAD_KEY_CEDILLA, DEAD_KEY_DIAERESIS, ']');
    b.SetKey(0x31U, KEY_CODE_FLAG_PRINTABLE, '<', '>', '}');
    b.SetKey(0x32U, KEY_CODE_FLAG_PRINTABLE, '<', '>', '}');
    b.SetKey(0x33U, KEY_CODE_FLAG_PRINTABLE, ';', ':', '~');
    b.SetKey(0x34U, KEY_CODE_FLAG_PRINTABLE, DEAD_KEY_GRAVE, DEAD_KEY_GRAVE, '{');
    b.SetKey(0x35U, KEY_CODE_FLAG_PRINTABLE, '#', '|', '\\');
    b.SetKey(0x36U, KEY_CODE_FLAG_PRINTABLE, ',', '\'');
    b.SetKey(0x37U, KEY_CODE_FLAG_PRINTABLE, '.', '"');
    b.SetKey(0x38U, KEY_CODE_FLAG_CAPSLOCK_LATIN, 0x00E9U, 0x00C9U);
    b.SetKey(0x64U, KEY_CODE_FLAG_PRINTABLE, 0x00ABU, 0x00BBU, 0x00B0U);
    return b.data;
}

constexpr KeyCodeMap BuildGerman() {
    KeyCodeMapBuilder b{KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_ALTGR, 4U,
                        KEY_CODE_MAP_COUNT_STANDARD, KEY_CODE_MAP_COMP_DEAD_KEYS};
    b.PopulateUsLatinBase();
    b.PopulateCommonSpaceAndKeypad();

    // German QWERTZ swaps Y/Z and puts sharp s / umlauts / ISO symbols on punctuation keys.
    b.SetAlphaAscii(0x1CU, 'z', 'Z');
    b.SetAlphaAscii(0x1DU, 'y', 'Y');
    b.SetAlphaAscii(0x08U, 'e', 'E', 0x20ACU);
    b.SetAlphaAscii(0x10U, 'm', 'M', 0x00B5U);
    b.SetAlphaAscii(0x14U, 'q', 'Q', '@');

    b.SetKey(0x1EU, KEY_CODE_FLAG_PRINTABLE, '1', '!');
    b.SetKey(0x1FU, KEY_CODE_FLAG_PRINTABLE, '2', '"', 0x00B2U);
    b.SetKey(0x20U, KEY_CODE_FLAG_PRINTABLE, '3', 0x00A7U, 0x00B3U);
    b.SetKey(0x21U, KEY_CODE_FLAG_PRINTABLE, '4', '$');
    b.SetKey(0x22U, KEY_CODE_FLAG_PRINTABLE, '5', '%');
    b.SetKey(0x23U, KEY_CODE_FLAG_PRINTABLE, '6', '&');
    b.SetKey(0x24U, KEY_CODE_FLAG_PRINTABLE, '7', '/', '{');
    b.SetKey(0x25U, KEY_CODE_FLAG_PRINTABLE, '8', '(', '[');
    b.SetKey(0x26U, KEY_CODE_FLAG_PRINTABLE, '9', ')', ']');
    b.SetKey(0x27U, KEY_CODE_FLAG_PRINTABLE, '0', '=', '}');

    b.SetKey(0x2DU, KEY_CODE_FLAG_PRINTABLE, 0x00DFU, '?', '\\');
    b.SetKey(0x2EU, KEY_CODE_FLAG_PRINTABLE, DEAD_KEY_ACUTE, DEAD_KEY_GRAVE);
    b.SetKey(0x2FU, KEY_CODE_FLAG_CAPSLOCK_LATIN, 0x00FCU, 0x00DCU);
    b.SetKey(0x30U, KEY_CODE_FLAG_PRINTABLE, '+', '*', '~');
    b.SetKey(0x31U, KEY_CODE_FLAG_PRINTABLE, '#', '\'');
    b.SetKey(0x32U, KEY_CODE_FLAG_PRINTABLE, '#', '\'');
    b.SetKey(0x33U, KEY_CODE_FLAG_CAPSLOCK_LATIN, 0x00F6U, 0x00D6U);
    b.SetKey(0x34U, KEY_CODE_FLAG_CAPSLOCK_LATIN, 0x00E4U, 0x00C4U);
    b.SetKey(0x35U, KEY_CODE_FLAG_PRINTABLE, DEAD_KEY_CIRCUMFLEX, 0x00B0U);
    b.SetKey(0x36U, KEY_CODE_FLAG_PRINTABLE, ',', ';');
    b.SetKey(0x37U, KEY_CODE_FLAG_PRINTABLE, '.', ':');
    b.SetKey(0x38U, KEY_CODE_FLAG_PRINTABLE, '-', '_');
    b.SetKey(0x64U, KEY_CODE_FLAG_PRINTABLE, '<', '>', '|');
    return b.data;
}

constexpr KeyCodeMap BuildItalian() {
    KeyCodeMapBuilder b{KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_ALTGR |
                            KEY_CODE_MAP_MODE_ALTGR_SHIFT,
                        5U, KEY_CODE_MAP_COUNT_STANDARD, KEY_CODE_MAP_COMP_DEAD_KEYS};
    b.PopulateUsLatinBase();
    b.PopulateCommonSpaceAndKeypad();

    b.SetAlphaAscii(0x08U, 'e', 'E', 0x20ACU);
    b.SetKey(0x1FU, KEY_CODE_FLAG_PRINTABLE, '2', '"');
    b.SetKey(0x20U, KEY_CODE_FLAG_PRINTABLE, '3', 0x00A3U);
    b.SetKey(0x23U, KEY_CODE_FLAG_PRINTABLE, '6', '&');
    b.SetKey(0x24U, KEY_CODE_FLAG_PRINTABLE, '7', '/');
    b.SetKey(0x25U, KEY_CODE_FLAG_PRINTABLE, '8', '(');
    b.SetKey(0x26U, KEY_CODE_FLAG_PRINTABLE, '9', ')');
    b.SetKey(0x27U, KEY_CODE_FLAG_PRINTABLE, '0', '=');

    b.SetKey(0x2DU, KEY_CODE_FLAG_PRINTABLE, '\'', '?');
    b.SetKey(0x2EU, KEY_CODE_FLAG_PRINTABLE, 0x00ECU, '^');
    b.SetKey(0x2FU, KEY_CODE_FLAG_PRINTABLE, 0x00E8U, 0x00E9U, '[', '{');
    b.SetKey(0x30U, KEY_CODE_FLAG_PRINTABLE, '+', '*', ']', '}');
    b.SetKey(0x31U, KEY_CODE_FLAG_PRINTABLE, 0x00F9U, 0x00A7U);
    b.SetKey(0x32U, KEY_CODE_FLAG_PRINTABLE, 0x00F9U, 0x00A7U);
    b.SetKey(0x33U, KEY_CODE_FLAG_PRINTABLE, 0x00F2U, 0x00E7U, '@');
    b.SetKey(0x34U, KEY_CODE_FLAG_PRINTABLE, 0x00E0U, 0x00B0U, '#');
    b.SetKey(0x35U, KEY_CODE_FLAG_PRINTABLE, '\\', '|');
    b.SetKey(0x36U, KEY_CODE_FLAG_PRINTABLE, ',', ';');
    b.SetKey(0x37U, KEY_CODE_FLAG_PRINTABLE, '.', ':');
    b.SetKey(0x38U, KEY_CODE_FLAG_PRINTABLE, '-', '_');
    b.SetKey(0x64U, KEY_CODE_FLAG_PRINTABLE, '<', '>');
    return b.data;
}

constexpr KeyCodeMap BuildSpanish() {
    KeyCodeMapBuilder b{KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_ALTGR, 4U,
                        KEY_CODE_MAP_COUNT_STANDARD, KEY_CODE_MAP_COMP_DEAD_KEYS};
    b.PopulateUsLatinBase();
    b.PopulateCommonSpaceAndKeypad();

    b.SetAlphaAscii(0x08U, 'e', 'E', 0x20ACU);
    b.SetKey(0x1EU, KEY_CODE_FLAG_PRINTABLE, '1', '!', '|');
    b.SetKey(0x1FU, KEY_CODE_FLAG_PRINTABLE, '2', '"', '@');
    b.SetKey(0x20U, KEY_CODE_FLAG_PRINTABLE, '3', 0x00B7U, '#');
    b.SetKey(0x21U, KEY_CODE_FLAG_PRINTABLE, '4', '$', '~');
    b.SetKey(0x22U, KEY_CODE_FLAG_PRINTABLE, '5', '%', 0x20ACU);
    b.SetKey(0x23U, KEY_CODE_FLAG_PRINTABLE, '6', '&', 0x00ACU);
    b.SetKey(0x24U, KEY_CODE_FLAG_PRINTABLE, '7', '/');
    b.SetKey(0x25U, KEY_CODE_FLAG_PRINTABLE, '8', '(');
    b.SetKey(0x26U, KEY_CODE_FLAG_PRINTABLE, '9', ')');
    b.SetKey(0x27U, KEY_CODE_FLAG_PRINTABLE, '0', '=');

    b.SetKey(0x2DU, KEY_CODE_FLAG_PRINTABLE, '\'', '?');
    b.SetKey(0x2EU, KEY_CODE_FLAG_PRINTABLE, 0x00A1U, 0x00BFU);
    b.SetKey(0x2FU, KEY_CODE_FLAG_PRINTABLE, DEAD_KEY_GRAVE, DEAD_KEY_CIRCUMFLEX, '[');
    b.SetKey(0x30U, KEY_CODE_FLAG_PRINTABLE, '+', '*', ']');
    b.SetKey(0x31U, KEY_CODE_FLAG_CAPSLOCK_LATIN, 0x00E7U, 0x00C7U, '}');
    b.SetKey(0x32U, KEY_CODE_FLAG_CAPSLOCK_LATIN, 0x00E7U, 0x00C7U, '}');
    b.SetKey(0x33U, KEY_CODE_FLAG_CAPSLOCK_LATIN, 0x00F1U, 0x00D1U);
    b.SetKey(0x34U, KEY_CODE_FLAG_PRINTABLE, DEAD_KEY_ACUTE, DEAD_KEY_DIAERESIS, '{');
    b.SetKey(0x35U, KEY_CODE_FLAG_PRINTABLE, 0x00BAU, 0x00AAU, '\\');
    b.SetKey(0x36U, KEY_CODE_FLAG_PRINTABLE, ',', ';');
    b.SetKey(0x37U, KEY_CODE_FLAG_PRINTABLE, '.', ':');
    b.SetKey(0x38U, KEY_CODE_FLAG_PRINTABLE, '-', '_');
    b.SetKey(0x64U, KEY_CODE_FLAG_PRINTABLE, '<', '>');
    return b.data;
}

constexpr KeyCodeMap BuildSpanishLatin() {
    KeyCodeMapBuilder b{KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_ALTGR, 4U,
                        KEY_CODE_MAP_COUNT_STANDARD, KEY_CODE_MAP_COMP_DEAD_KEYS};
    b.PopulateUsLatinBase();
    b.PopulateCommonSpaceAndKeypad();

    b.SetAlphaAscii(0x14U, 'q', 'Q', '@');
    b.SetKey(0x1FU, KEY_CODE_FLAG_PRINTABLE, '2', '"');
    b.SetKey(0x20U, KEY_CODE_FLAG_PRINTABLE, '3', '#');
    b.SetKey(0x23U, KEY_CODE_FLAG_PRINTABLE, '6', '&');
    b.SetKey(0x24U, KEY_CODE_FLAG_PRINTABLE, '7', '/');
    b.SetKey(0x25U, KEY_CODE_FLAG_PRINTABLE, '8', '(');
    b.SetKey(0x26U, KEY_CODE_FLAG_PRINTABLE, '9', ')');
    b.SetKey(0x27U, KEY_CODE_FLAG_PRINTABLE, '0', '=');

    b.SetKey(0x2DU, KEY_CODE_FLAG_PRINTABLE, '\'', '?', '\\');
    b.SetKey(0x2EU, KEY_CODE_FLAG_PRINTABLE, 0x00BFU, 0x00A1U);
    b.SetKey(0x2FU, KEY_CODE_FLAG_PRINTABLE, DEAD_KEY_ACUTE, DEAD_KEY_DIAERESIS);
    b.SetKey(0x30U, KEY_CODE_FLAG_PRINTABLE, '+', '*', '~');
    b.SetKey(0x31U, KEY_CODE_FLAG_PRINTABLE, '}', ']', '`');
    b.SetKey(0x32U, KEY_CODE_FLAG_PRINTABLE, '}', ']', '`');
    b.SetKey(0x33U, KEY_CODE_FLAG_CAPSLOCK_LATIN, 0x00F1U, 0x00D1U);
    b.SetKey(0x34U, KEY_CODE_FLAG_PRINTABLE, '{', '[', '^');
    b.SetKey(0x35U, KEY_CODE_FLAG_PRINTABLE, '|', 0x00B0U, 0x00ACU);
    b.SetKey(0x36U, KEY_CODE_FLAG_PRINTABLE, ',', ';');
    b.SetKey(0x37U, KEY_CODE_FLAG_PRINTABLE, '.', ':');
    b.SetKey(0x38U, KEY_CODE_FLAG_PRINTABLE, '-', '_');
    b.SetKey(0x64U, KEY_CODE_FLAG_PRINTABLE, '<', '>');
    return b.data;
}

constexpr KeyCodeMap BuildPortuguese() {
    KeyCodeMapBuilder b{KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_ALTGR, 4U,
                        KEY_CODE_MAP_COUNT_STANDARD, KEY_CODE_MAP_COMP_DEAD_KEYS};
    b.PopulateUsLatinBase();
    b.PopulateCommonSpaceAndKeypad();

    b.SetAlphaAscii(0x08U, 'e', 'E', 0x20ACU);
    b.SetKey(0x1FU, KEY_CODE_FLAG_PRINTABLE, '2', '"', '@');
    b.SetKey(0x20U, KEY_CODE_FLAG_PRINTABLE, '3', '#', 0x00A3U);
    b.SetKey(0x21U, KEY_CODE_FLAG_PRINTABLE, '4', '$', 0x00A7U);
    b.SetKey(0x22U, KEY_CODE_FLAG_PRINTABLE, '5', '%', 0x20ACU);
    b.SetKey(0x23U, KEY_CODE_FLAG_PRINTABLE, '6', '&');
    b.SetKey(0x24U, KEY_CODE_FLAG_PRINTABLE, '7', '/', '{');
    b.SetKey(0x25U, KEY_CODE_FLAG_PRINTABLE, '8', '(', '[');
    b.SetKey(0x26U, KEY_CODE_FLAG_PRINTABLE, '9', ')', ']');
    b.SetKey(0x27U, KEY_CODE_FLAG_PRINTABLE, '0', '=', '}');

    b.SetKey(0x2DU, KEY_CODE_FLAG_PRINTABLE, '\'', '?');
    b.SetKey(0x2EU, KEY_CODE_FLAG_PRINTABLE, 0x00ABU, 0x00BBU);
    b.SetKey(0x2FU, KEY_CODE_FLAG_PRINTABLE, '+', '*', DEAD_KEY_DIAERESIS);
    b.SetKey(0x30U, KEY_CODE_FLAG_PRINTABLE, DEAD_KEY_ACUTE, DEAD_KEY_GRAVE);
    b.SetKey(0x31U, KEY_CODE_FLAG_PRINTABLE, DEAD_KEY_TILDE, DEAD_KEY_CIRCUMFLEX);
    b.SetKey(0x32U, KEY_CODE_FLAG_PRINTABLE, DEAD_KEY_TILDE, DEAD_KEY_CIRCUMFLEX);
    b.SetKey(0x33U, KEY_CODE_FLAG_CAPSLOCK_LATIN, 0x00E7U, 0x00C7U);
    b.SetKey(0x34U, KEY_CODE_FLAG_PRINTABLE, 0x00BAU, 0x00AAU);
    b.SetKey(0x35U, KEY_CODE_FLAG_PRINTABLE, '\\', '|');
    b.SetKey(0x36U, KEY_CODE_FLAG_PRINTABLE, ',', ';');
    b.SetKey(0x37U, KEY_CODE_FLAG_PRINTABLE, '.', ':');
    b.SetKey(0x38U, KEY_CODE_FLAG_PRINTABLE, '-', '_');
    b.SetKey(0x64U, KEY_CODE_FLAG_PRINTABLE, '<', '>');
    return b.data;
}

constexpr KeyCodeMap BuildRussian() {
    KeyCodeMapBuilder b{KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_CYRILLIC, 5U,
                        KEY_CODE_MAP_COUNT_STANDARD, KEY_CODE_MAP_COMP_NONE};
    b.PopulateCommonSpaceAndKeypad();

    // Standard US Latin paired with PC Russian ЙЦУКЕН Cyrillic.
    struct DualLetter {
        u8 hid;
        char lower;
        u16 cyr_lower;
        u16 cyr_upper;
    };
    constexpr std::array<DualLetter, 26> letters{{
        {0x04U, 'a', 0x0444U, 0x0424U}, // ф
        {0x05U, 'b', 0x0438U, 0x0418U}, // и
        {0x06U, 'c', 0x0441U, 0x0421U}, // с
        {0x07U, 'd', 0x0432U, 0x0412U}, // в
        {0x08U, 'e', 0x0443U, 0x0423U}, // у
        {0x09U, 'f', 0x0430U, 0x0410U}, // а
        {0x0AU, 'g', 0x043FU, 0x041FU}, // п
        {0x0BU, 'h', 0x0440U, 0x0420U}, // р
        {0x0CU, 'i', 0x0448U, 0x0428U}, // ш
        {0x0DU, 'j', 0x043EU, 0x041EU}, // о
        {0x0EU, 'k', 0x043BU, 0x041BU}, // л
        {0x0FU, 'l', 0x0434U, 0x0414U}, // д
        {0x10U, 'm', 0x044CU, 0x042CU}, // ь
        {0x11U, 'n', 0x0442U, 0x0422U}, // т
        {0x12U, 'o', 0x0449U, 0x0429U}, // щ
        {0x13U, 'p', 0x0437U, 0x0417U}, // з
        {0x14U, 'q', 0x0439U, 0x0419U}, // й
        {0x15U, 'r', 0x043AU, 0x041AU}, // к
        {0x16U, 's', 0x044BU, 0x042BU}, // ы
        {0x17U, 't', 0x0435U, 0x0415U}, // е
        {0x18U, 'u', 0x0433U, 0x0413U}, // г
        {0x19U, 'v', 0x043CU, 0x041CU}, // м
        {0x1AU, 'w', 0x0446U, 0x0426U}, // ц
        {0x1BU, 'x', 0x0447U, 0x0427U}, // ч
        {0x1CU, 'y', 0x043DU, 0x041DU}, // н
        {0x1DU, 'z', 0x044FU, 0x042FU}, // я
    }};
    for (const auto& entry : letters) {
        const u16 lower = static_cast<u16>(static_cast<u8>(entry.lower));
        const u16 upper = static_cast<u16>(static_cast<u8>(entry.lower - 'a' + 'A'));
        b.SetKey(entry.hid, KEY_CODE_FLAG_CAPSLOCK_LATIN_SECONDARY, lower, upper, entry.cyr_lower,
                 entry.cyr_upper);
    }

    constexpr std::array<u16, 10> us_shift{{'!', '@', '#', '$', '%', '^', '&', '*', '(', ')'}};
    constexpr std::array<u16, 10> ru_shift{{'!', '"', 0x2116U, ';', '%', ':', '?', '*', '(', ')'}};
    for (u8 offset = 0; offset < 10U; ++offset) {
        const u8 hid = static_cast<u8>(0x1EU + offset);
        const u16 digit = static_cast<u16>(offset < 9U ? ('1' + offset) : '0');
        b.SetKey(hid, KEY_CODE_FLAG_PRINTABLE, digit, us_shift[offset], digit, ru_shift[offset]);
    }

    b.SetKey(0x2DU, KEY_CODE_FLAG_PRINTABLE, '-', '_', '-', '_');
    b.SetKey(0x2EU, KEY_CODE_FLAG_PRINTABLE, '=', '+', '=', '+');
    b.SetKey(0x2FU, KEY_CODE_FLAG_CAPSLOCK_SECONDARY, '[', '{', 0x0445U, 0x0425U); // х/Х
    b.SetKey(0x30U, KEY_CODE_FLAG_CAPSLOCK_SECONDARY, ']', '}', 0x044AU, 0x042AU); // ъ/Ъ
    b.SetKey(0x31U, KEY_CODE_FLAG_PRINTABLE, '\\', '|', '\\', '/');
    b.SetKey(0x33U, KEY_CODE_FLAG_CAPSLOCK_SECONDARY, ';', ':', 0x0436U, 0x0416U); // ж/Ж
    b.SetKey(0x34U, KEY_CODE_FLAG_CAPSLOCK_SECONDARY, '\'', '"', 0x044DU, 0x042DU); // э/Э
    b.SetKey(0x35U, KEY_CODE_FLAG_CAPSLOCK_SECONDARY, '`', '~', 0x0451U, 0x0401U); // ё/Ё
    b.SetKey(0x36U, KEY_CODE_FLAG_CAPSLOCK_SECONDARY, ',', '<', 0x0431U, 0x0411U); // б/Б
    b.SetKey(0x37U, KEY_CODE_FLAG_CAPSLOCK_SECONDARY, '.', '>', 0x044EU, 0x042EU); // ю/Ю
    b.SetKey(0x38U, KEY_CODE_FLAG_PRINTABLE, '/', '?', '.', ',');
    return b.data;
}

constexpr KeyCodeMap BuildKorean() {
    KeyCodeMapBuilder b{KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_HANGUL, 5U,
                        KEY_CODE_MAP_COUNT_STANDARD, KEY_CODE_MAP_COMP_NONE};
    b.PopulateCommonSpaceAndKeypad();

    // Standard US Latin paired with 2-Bulsik Hangul Compatibility Jamo (KS X 5002).
    struct DualLetter {
        u8 hid;
        char lower;
        u16 jamo;
        u16 jamo_shift;
    };
    constexpr std::array<DualLetter, 26> letters{{
        {0x04U, 'a', 0x3141U, 0x3141U}, // ㅁ
        {0x05U, 'b', 0x3160U, 0x3160U}, // ㅠ
        {0x06U, 'c', 0x314AU, 0x314AU}, // ㅊ
        {0x07U, 'd', 0x3147U, 0x3147U}, // ㅇ
        {0x08U, 'e', 0x3137U, 0x3138U}, // ㄷ/ㄸ
        {0x09U, 'f', 0x3139U, 0x3139U}, // ㄹ
        {0x0AU, 'g', 0x314EU, 0x314EU}, // ㅎ
        {0x0BU, 'h', 0x3157U, 0x3157U}, // ㅗ
        {0x0CU, 'i', 0x3151U, 0x3151U}, // ㅑ
        {0x0DU, 'j', 0x3153U, 0x3153U}, // ㅓ
        {0x0EU, 'k', 0x314FU, 0x314FU}, // ㅏ
        {0x0FU, 'l', 0x3163U, 0x3163U}, // ㅣ
        {0x10U, 'm', 0x3161U, 0x3161U}, // ㅡ
        {0x11U, 'n', 0x315CU, 0x315CU}, // ㅜ
        {0x12U, 'o', 0x3150U, 0x3152U}, // ㅐ/ㅒ
        {0x13U, 'p', 0x3154U, 0x3156U}, // ㅔ/ㅖ
        {0x14U, 'q', 0x3142U, 0x3143U}, // ㅂ/ㅃ
        {0x15U, 'r', 0x3131U, 0x3132U}, // ㄱ/ㄲ
        {0x16U, 's', 0x3134U, 0x3134U}, // ㄴ
        {0x17U, 't', 0x3145U, 0x3146U}, // ㅅ/ㅆ
        {0x18U, 'u', 0x3155U, 0x3155U}, // ㅕ
        {0x19U, 'v', 0x314DU, 0x314DU}, // ㅍ
        {0x1AU, 'w', 0x3148U, 0x3149U}, // ㅈ/ㅉ
        {0x1BU, 'x', 0x314CU, 0x314CU}, // ㅌ
        {0x1CU, 'y', 0x315BU, 0x315BU}, // ㅛ
        {0x1DU, 'z', 0x314BU, 0x314BU}, // ㅋ
    }};
    for (const auto& entry : letters) {
        const u16 lower = static_cast<u16>(static_cast<u8>(entry.lower));
        const u16 upper = static_cast<u16>(static_cast<u8>(entry.lower - 'a' + 'A'));
        b.SetKey(entry.hid, KEY_CODE_FLAG_CAPSLOCK_LATIN, lower, upper, entry.jamo,
                 entry.jamo_shift);
    }

    constexpr std::array<u16, 10> shift_sym{{'!', '@', '#', '$', '%', '^', '&', '*', '(', ')'}};
    for (u8 offset = 0; offset < 10U; ++offset) {
        const u8 hid = static_cast<u8>(0x1EU + offset);
        const u16 digit = static_cast<u16>(offset < 9U ? ('1' + offset) : '0');
        b.SetKeyUniform(hid, KEY_CODE_FLAG_PRINTABLE, digit, shift_sym[offset]);
    }
    b.SetKeyUniform(0x2DU, KEY_CODE_FLAG_PRINTABLE, '-', '_');
    b.SetKeyUniform(0x2EU, KEY_CODE_FLAG_PRINTABLE, '=', '+');
    b.SetKeyUniform(0x2FU, KEY_CODE_FLAG_PRINTABLE, '[', '{');
    b.SetKeyUniform(0x30U, KEY_CODE_FLAG_PRINTABLE, ']', '}');
    b.SetKeyUniform(0x31U, KEY_CODE_FLAG_PRINTABLE, '\\', '|');
    b.SetKeyUniform(0x33U, KEY_CODE_FLAG_PRINTABLE, ';', ':');
    b.SetKeyUniform(0x34U, KEY_CODE_FLAG_PRINTABLE, '\'', '"');
    b.SetKeyUniform(0x35U, KEY_CODE_FLAG_PRINTABLE, '`', '~');
    b.SetKeyUniform(0x36U, KEY_CODE_FLAG_PRINTABLE, ',', '<');
    b.SetKeyUniform(0x37U, KEY_CODE_FLAG_PRINTABLE, '.', '>');
    b.SetKeyUniform(0x38U, KEY_CODE_FLAG_PRINTABLE, '/', '?');
    return b.data;
}

constexpr KeyCodeMap BuildChineseSimplified() {
    KeyCodeMapBuilder b{KEY_CODE_MAP_MODE_LATIN, 3U, KEY_CODE_MAP_COUNT_STANDARD,
                        KEY_CODE_MAP_COMP_NONE};
    b.PopulateUsLatinBase();
    b.PopulateCommonSpaceAndKeypad();
    return b.data;
}

constexpr KeyCodeMap BuildChineseTraditional() {
    KeyCodeMapBuilder b{KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_ZHUYIN |
                            KEY_CODE_MAP_MODE_CANGJIE,
                        7U, KEY_CODE_MAP_COUNT_STANDARD, KEY_CODE_MAP_COMP_NONE};
    b.PopulateCommonSpaceAndKeypad();

    // US Latin paired with Standard Taiwanese Zhuyin/Bopomofo and Cangjie.
    struct TripleLetter {
        u8 hid;
        char lower;
        u16 zhuyin;
        u16 cangjie;
    };
    constexpr std::array<TripleLetter, 26> letters{{
        {0x04U, 'a', 0x3107U, 0x65E5U}, // ㄇ / 日
        {0x05U, 'b', 0x3116U, 0x6708U}, // ㄖ / 月
        {0x06U, 'c', 0x310FU, 0x91D1U}, // ㄏ / 金
        {0x07U, 'd', 0x310EU, 0x6728U}, // ㄎ / 木
        {0x08U, 'e', 0x310DU, 0x6C34U}, // ㄍ / 水
        {0x09U, 'f', 0x3111U, 0x706BU}, // ㄑ / 火
        {0x0AU, 'g', 0x3115U, 0x571FU}, // ㄕ / 土
        {0x0BU, 'h', 0x3118U, 0x7AF9U}, // ㄘ / 竹
        {0x0CU, 'i', 0x311BU, 0x6208U}, // ㄛ / 戈
        {0x0DU, 'j', 0x3128U, 0x5341U}, // ㄨ / 十
        {0x0EU, 'k', 0x311CU, 0x5927U}, // ㄜ / 大
        {0x0FU, 'l', 0x3120U, 0x4E2DU}, // ㄠ / 中
        {0x10U, 'm', 0x3129U, 0x4E00U}, // ㄩ / 一
        {0x11U, 'n', 0x3119U, 0x5F13U}, // ㄙ / 弓
        {0x12U, 'o', 0x311FU, 0x4EBAU}, // ㄟ / 人
        {0x13U, 'p', 0x3123U, 0x5FC3U}, // ㄣ / 心
        {0x14U, 'q', 0x3106U, 0x624BU}, // ㄆ / 手
        {0x15U, 'r', 0x3110U, 0x53E3U}, // ㄐ / 口
        {0x16U, 's', 0x310BU, 0x5C38U}, // ㄋ / 尸
        {0x17U, 't', 0x3114U, 0x5EFFU}, // ㄔ / 廿
        {0x18U, 'u', 0x3127U, 0x5C71U}, // ㄧ / 山
        {0x19U, 'v', 0x3112U, 0x5973U}, // ㄒ / 女
        {0x1AU, 'w', 0x310AU, 0x7530U}, // ㄊ / 田
        {0x1BU, 'x', 0x310CU, 0x96E3U}, // ㄌ / 難
        {0x1CU, 'y', 0x3117U, 0x535CU}, // ㄗ / 卜
        {0x1DU, 'z', 0x3108U, 'Z'},     // ㄈ / Z
    }};
    for (const auto& entry : letters) {
        const u16 lower = static_cast<u16>(static_cast<u8>(entry.lower));
        const u16 upper = static_cast<u16>(static_cast<u8>(entry.lower - 'a' + 'A'));
        b.SetKey(entry.hid, KEY_CODE_FLAG_CAPSLOCK_LATIN, lower, upper, entry.zhuyin, 0x0000U,
                 entry.cangjie, 0x0000U);
    }

    struct DigitKey {
        u8 hid;
        u16 normal;
        u16 shift;
        u16 zhuyin;
    };
    constexpr std::array<DigitKey, 11> num_row{{
        {0x1EU, '1', '!', 0x3105U}, // ㄅ
        {0x1FU, '2', '@', 0x3109U}, // ㄉ
        {0x20U, '3', '#', 0x02C7U}, // ˇ (3rd tone)
        {0x21U, '4', '$', 0x02CBU}, // ˋ (4th tone)
        {0x22U, '5', '%', 0x3113U}, // ㄓ
        {0x23U, '6', '^', 0x02CAU}, // ˊ (2nd tone)
        {0x24U, '7', '&', 0x02D9U}, // ˙ (neutral tone)
        {0x25U, '8', '*', 0x311AU}, // ㄚ
        {0x26U, '9', '(', 0x311EU}, // ㄞ
        {0x27U, '0', ')', 0x3122U}, // ㄢ
        {0x2DU, '-', '_', 0x3126U}, // ㄦ
    }};
    for (const auto& entry : num_row) {
        b.SetKey(entry.hid, KEY_CODE_FLAG_PRINTABLE, entry.normal, entry.shift, entry.zhuyin,
                 0x0000U, entry.normal, entry.shift);
    }

    b.SetKey(0x2EU, KEY_CODE_FLAG_PRINTABLE, '=', '+', 0x0000U, 0x0000U, '=', '+');
    b.SetKey(0x2FU, KEY_CODE_FLAG_PRINTABLE, '[', '{', 0x0000U, 0x0000U, '[', '{');
    b.SetKey(0x30U, KEY_CODE_FLAG_PRINTABLE, ']', '}', 0x0000U, 0x0000U, ']', '}');
    b.SetKey(0x31U, KEY_CODE_FLAG_PRINTABLE, '\\', '|', 0x0000U, 0x0000U, '\\', '|');
    b.SetKey(0x33U, KEY_CODE_FLAG_PRINTABLE, ';', ':', 0x3124U, 0x0000U, ';', ':'); // ㄤ
    b.SetKey(0x34U, KEY_CODE_FLAG_PRINTABLE, '\'', '"', 0x0000U, 0x0000U, '\'', '"');
    b.SetKey(0x35U, KEY_CODE_FLAG_PRINTABLE, '`', '~', 0x0000U, 0x0000U, '`', '~');
    b.SetKey(0x36U, KEY_CODE_FLAG_PRINTABLE, ',', '<', 0x311DU, 0x0000U, ',', '<'); // ㄝ
    b.SetKey(0x37U, KEY_CODE_FLAG_PRINTABLE, '.', '>', 0x3121U, 0x0000U, '.', '>'); // ㄡ
    b.SetKey(0x38U, KEY_CODE_FLAG_PRINTABLE, '/', '?', 0x3125U, 0x0000U, '/', '?'); // ㄥ
    return b.data;
}

constexpr KeyCodeMap BuildJapanese() {
    KeyCodeMapBuilder b{KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_HIRAGANA |
                            KEY_CODE_MAP_MODE_KATAKANA,
                        7U, KEY_CODE_MAP_COUNT_JAPANESE, KEY_CODE_MAP_COMP_NONE};
    b.PopulateCommonSpaceAndKeypad();

    // Standard JIS 106/109 keyboard: Latin paired with Hiragana and Katakana.
    struct JisLetter {
        u8 hid;
        char lower;
        u16 hira;
        u16 hira_shift;
        u16 kata;
        u16 kata_shift;
    };
    constexpr std::array<JisLetter, 26> letters{{
        {0x04U, 'a', 0x3061U, 0x3061U, 0x30C1U, 0x30C1U}, // ち/チ
        {0x05U, 'b', 0x3053U, 0x3053U, 0x30B3U, 0x30B3U}, // こ/コ
        {0x06U, 'c', 0x305DU, 0x305DU, 0x30BDU, 0x30BDU}, // そ/ソ
        {0x07U, 'd', 0x3057U, 0x3057U, 0x30B7U, 0x30B7U}, // し/シ
        {0x08U, 'e', 0x3044U, 0x3043U, 0x30A4U, 0x30A3U}, // い/ぃ イ/ィ
        {0x09U, 'f', 0x306FU, 0x306FU, 0x30CFU, 0x30CFU}, // は/ハ
        {0x0AU, 'g', 0x304DU, 0x304DU, 0x30ADU, 0x30ADU}, // き/キ
        {0x0BU, 'h', 0x304FU, 0x304FU, 0x30AFU, 0x30AFU}, // く/ク
        {0x0CU, 'i', 0x306BU, 0x306BU, 0x30CBU, 0x30CBU}, // に/ニ
        {0x0DU, 'j', 0x307EU, 0x307EU, 0x30DEU, 0x30DEU}, // ま/マ
        {0x0EU, 'k', 0x306EU, 0x306EU, 0x30CEU, 0x30CEU}, // の/ノ
        {0x0FU, 'l', 0x308AU, 0x308AU, 0x30EAU, 0x30EAU}, // り/リ
        {0x10U, 'm', 0x3082U, 0x3082U, 0x30E2U, 0x30E2U}, // も/モ
        {0x11U, 'n', 0x307FU, 0x307FU, 0x30DFU, 0x30DFU}, // み/ミ
        {0x12U, 'o', 0x3089U, 0x3089U, 0x30E9U, 0x30E9U}, // ら/ラ
        {0x13U, 'p', 0x305BU, 0x305BU, 0x30BBU, 0x30BBU}, // せ/セ
        {0x14U, 'q', 0x305FU, 0x305FU, 0x30BFU, 0x30BFU}, // た/タ
        {0x15U, 'r', 0x3059U, 0x3059U, 0x30B9U, 0x30B9U}, // す/ス
        {0x16U, 's', 0x3068U, 0x3068U, 0x30C8U, 0x30C8U}, // と/ト
        {0x17U, 't', 0x304BU, 0x304BU, 0x30ABU, 0x30ABU}, // か/カ
        {0x18U, 'u', 0x306AU, 0x306AU, 0x30CAU, 0x30CAU}, // な/ナ
        {0x19U, 'v', 0x3072U, 0x3072U, 0x30D2U, 0x30D2U}, // ひ/ヒ
        {0x1AU, 'w', 0x3066U, 0x3066U, 0x30C6U, 0x30C6U}, // て/テ
        {0x1BU, 'x', 0x3055U, 0x3055U, 0x30B5U, 0x30B5U}, // さ/サ
        {0x1CU, 'y', 0x3093U, 0x3093U, 0x30F3U, 0x30F3U}, // ん/ン
        {0x1DU, 'z', 0x3064U, 0x3063U, 0x30C4U, 0x30C3U}, // つ/っ ツ/ッ
    }};
    for (const auto& entry : letters) {
        const u16 lower = static_cast<u16>(static_cast<u8>(entry.lower));
        const u16 upper = static_cast<u16>(static_cast<u8>(entry.lower - 'a' + 'A'));
        b.SetKey(entry.hid, KEY_CODE_FLAG_CAPSLOCK_LATIN, lower, upper, entry.hira, entry.hira_shift,
                 entry.kata, entry.kata_shift);
    }

    struct JisSym {
        u8 hid;
        u16 lat;
        u16 lat_shift;
        u16 hira;
        u16 hira_shift;
        u16 kata;
        u16 kata_shift;
    };
    constexpr std::array<JisSym, 22> syms{{
        {0x1EU, '1', '!', 0x306CU, 0x306CU, 0x30CCU, 0x30CCU}, // ぬ/ヌ
        {0x1FU, '2', '"', 0x3075U, 0x3075U, 0x30D5U, 0x30D5U}, // ふ/フ
        {0x20U, '3', '#', 0x3042U, 0x3041U, 0x30A2U, 0x30A1U}, // あ/ぁ
        {0x21U, '4', '$', 0x3046U, 0x3045U, 0x30A6U, 0x30A5U}, // う/ぅ
        {0x22U, '5', '%', 0x3048U, 0x3047U, 0x30A8U, 0x30A7U}, // え/ぇ
        {0x23U, '6', '&', 0x304AU, 0x3049U, 0x30AAU, 0x30A9U}, // お/ぉ
        {0x24U, '7', '\'', 0x3084U, 0x3083U, 0x30E4U, 0x30E3U}, // や/ゃ
        {0x25U, '8', '(', 0x3086U, 0x3085U, 0x30E6U, 0x30E5U}, // ゆ/ゅ
        {0x26U, '9', ')', 0x3088U, 0x3087U, 0x30E8U, 0x30E7U}, // よ/ょ
        {0x27U, '0', 0x0000U, 0x308FU, 0x3092U, 0x30EFU, 0x30F2U}, // わ/を
        {0x2DU, '-', '=', 0x307BU, 0x307BU, 0x30DBU, 0x30DBU}, // ほ/ホ
        {0x2EU, '^', '~', 0x3078U, 0x3078U, 0x30D8U, 0x30D8U}, // へ/ヘ
        {0x2FU, '@', '`', 0x309BU, 0x309BU, 0x309BU, 0x309BU}, // ゛
        {0x30U, '[', '{', 0x309CU, 0x300CU, 0x309CU, 0x300CU}, // ゜/「
        {0x32U, ']', '}', 0x3080U, 0x300DU, 0x30E0U, 0x300DU}, // む/」
        {0x33U, ';', '+', 0x308CU, 0x308CU, 0x30ECU, 0x30ECU}, // れ/レ
        {0x34U, ':', '*', 0x3051U, 0x3051U, 0x30B1U, 0x30B1U}, // け/ケ
        {0x36U, ',', '<', 0x306DU, 0x3001U, 0x30CDU, 0x3001U}, // ね/、
        {0x37U, '.', '>', 0x308BU, 0x3002U, 0x30EBU, 0x3002U}, // る/。
        {0x38U, '/', '?', 0x3081U, 0x30FBU, 0x30E1U, 0x30FBU}, // め/・
        {0x87U, '\\', '_', 0x308DU, 0x308DU, 0x30EDU, 0x30EDU}, // ろ/ロ
        {0x89U, 0x00A5U, '|', 0x30FCU, 0x30FCU, 0x30FCU, 0x30FCU}, // ー
    }};
    for (const auto& entry : syms) {
        b.SetKey(entry.hid, KEY_CODE_FLAG_PRINTABLE, entry.lat, entry.lat_shift, entry.hira,
                 entry.hira_shift, entry.kata, entry.kata_shift);
    }
    return b.data;
}

} // namespace detail

inline constexpr KeyCodeMap KeyCodeMapEnglishUsInternational =
    detail::BuildEnglishUsInternational();
inline constexpr KeyCodeMap KeyCodeMapEnglishUk = detail::BuildEnglishUk();
inline constexpr KeyCodeMap KeyCodeMapFrench = detail::BuildFrench();
inline constexpr KeyCodeMap KeyCodeMapFrenchCa = detail::BuildFrenchCa();
inline constexpr KeyCodeMap KeyCodeMapSpanish = detail::BuildSpanish();
inline constexpr KeyCodeMap KeyCodeMapSpanishLatin = detail::BuildSpanishLatin();
inline constexpr KeyCodeMap KeyCodeMapGerman = detail::BuildGerman();
inline constexpr KeyCodeMap KeyCodeMapItalian = detail::BuildItalian();
inline constexpr KeyCodeMap KeyCodeMapPortuguese = detail::BuildPortuguese();
inline constexpr KeyCodeMap KeyCodeMapRussian = detail::BuildRussian();
inline constexpr KeyCodeMap KeyCodeMapKorean = detail::BuildKorean();
inline constexpr KeyCodeMap KeyCodeMapChineseSimplified = detail::BuildChineseSimplified();
inline constexpr KeyCodeMap KeyCodeMapChineseTraditional = detail::BuildChineseTraditional();
inline constexpr KeyCodeMap KeyCodeMapJapanese = detail::BuildJapanese();

constexpr const KeyCodeMap& GetKeyCodeMapForLayout(KeyboardLayout keyboard_layout,
                                                   LanguageCode language_code) {
    switch (keyboard_layout) {
    case KeyboardLayout::Japanese:
        return KeyCodeMapJapanese;
    case KeyboardLayout::EnglishUs:
        if (language_code == LanguageCode::KO) {
            return KeyCodeMapKorean;
        }
        if (language_code == LanguageCode::ZH_HANS) {
            return KeyCodeMapChineseSimplified;
        }
        if (language_code == LanguageCode::ZH_HANT) {
            return KeyCodeMapChineseTraditional;
        }
        return KeyCodeMapEnglishUsInternational;
    case KeyboardLayout::EnglishUk:
        return KeyCodeMapEnglishUk;
    case KeyboardLayout::French:
        return KeyCodeMapFrench;
    case KeyboardLayout::FrenchCa:
        return KeyCodeMapFrenchCa;
    case KeyboardLayout::Spanish:
        return KeyCodeMapSpanish;
    case KeyboardLayout::SpanishLatin:
        return KeyCodeMapSpanishLatin;
    case KeyboardLayout::German:
        return KeyCodeMapGerman;
    case KeyboardLayout::Italian:
        return KeyCodeMapItalian;
    case KeyboardLayout::Portuguese:
        return KeyCodeMapPortuguese;
    case KeyboardLayout::Russian:
        return KeyCodeMapRussian;
    case KeyboardLayout::Korean:
        return KeyCodeMapKorean;
    case KeyboardLayout::ChineseSimplified:
        return KeyCodeMapChineseSimplified;
    case KeyboardLayout::ChineseTraditional:
        return KeyCodeMapChineseTraditional;
    case KeyboardLayout::EnglishUsInternational:
    default:
        return KeyCodeMapEnglishUsInternational;
    }
}

constexpr const KeyCodeMap& GetKeyCodeMapForLanguage(LanguageCode language_code,
                                                     bool* out_found = nullptr) {
    for (const auto& entry : language_to_layout) {
        if (entry.first == language_code) {
            if (out_found != nullptr) {
                *out_found = true;
            }
            return GetKeyCodeMapForLayout(entry.second, entry.first);
        }
    }
    if (out_found != nullptr) {
        *out_found = false;
    }
    return KeyCodeMapEnglishUsInternational;
}

constexpr const KeyCodeMap& GetKeyCodeMapForLanguageIndex(std::size_t language_index,
                                                          bool* out_found = nullptr) {
    if (language_index >= available_language_codes.size()) {
        if (out_found != nullptr) {
            *out_found = false;
        }
        return KeyCodeMapEnglishUsInternational;
    }
    return GetKeyCodeMapForLanguage(available_language_codes[language_index], out_found);
}

} // namespace Service::Set
