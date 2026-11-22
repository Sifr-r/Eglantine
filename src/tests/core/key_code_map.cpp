// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <limits>
#include <utility>

#include "core/hle/service/set/key_code_map.h"
#include "core/hle/service/set/settings_types.h"

namespace Service::Set {
namespace {

u16 ReadU16(const KeyCodeMap& map, std::size_t offset) {
    return static_cast<u16>(map[offset]) | (static_cast<u16>(map[offset + 1U]) << 8U);
}

u32 ReadU32(const KeyCodeMap& map, std::size_t offset) {
    return static_cast<u32>(ReadU16(map, offset)) |
           (static_cast<u32>(ReadU16(map, offset + 2U)) << 16U);
}

u16 ReadEntryWord(const KeyCodeMap& map, u8 hid, u32 column) {
    const u32 columns = ReadU32(map, 0x04U);
    REQUIRE(hid >= KEY_CODE_MAP_HID_BASE);
    REQUIRE(column < columns);
    const std::size_t index = static_cast<std::size_t>(hid - KEY_CODE_MAP_HID_BASE);
    const std::size_t offset =
        KEY_CODE_MAP_HEADER_SIZE + (index * columns + column) * sizeof(u16);
    REQUIRE(offset + sizeof(u16) <= map.size());
    return ReadU16(map, offset);
}

u32 ExpectedColumnsForMode(u32 mode_mask) {
    u32 cols = 1U;
    // Bits 0..6 are 2-column (normal, shift) script planes.
    for (u32 bit = 0; bit < 7U; ++bit) {
        if ((mode_mask & (1U << bit)) != 0U) {
            cols += 2U;
        }
    }
    if ((mode_mask & KEY_CODE_MAP_MODE_ALTGR) != 0U) {
        cols += 1U;
    }
    if ((mode_mask & KEY_CODE_MAP_MODE_ALTGR_SHIFT) != 0U) {
        cols += 1U;
    }
    return cols;
}

} // Anonymous namespace

TEST_CASE("All 14 KeyCodeMap tables satisfy nn::kpr wire header and size invariants",
          "[core][set][key_code_map]") {
    struct MapSpec {
        const KeyCodeMap* map;
        u32 expected_mode;
        u32 expected_columns;
        u32 expected_count;
        u32 expected_comp;
    };

    const std::array<MapSpec, 14> specs{{
        {&KeyCodeMapEnglishUsInternational,
         KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_ALTGR | KEY_CODE_MAP_MODE_ALTGR_SHIFT, 5U,
         KEY_CODE_MAP_COUNT_STANDARD, KEY_CODE_MAP_COMP_DEAD_KEYS},
        {&KeyCodeMapEnglishUk,
         KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_ALTGR | KEY_CODE_MAP_MODE_ALTGR_SHIFT, 5U,
         KEY_CODE_MAP_COUNT_STANDARD, KEY_CODE_MAP_COMP_DEAD_KEYS},
        {&KeyCodeMapFrench, KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_ALTGR, 4U,
         KEY_CODE_MAP_COUNT_STANDARD, KEY_CODE_MAP_COMP_DEAD_KEYS},
        {&KeyCodeMapFrenchCa, KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_ALTGR, 4U,
         KEY_CODE_MAP_COUNT_STANDARD, KEY_CODE_MAP_COMP_DEAD_KEYS},
        {&KeyCodeMapSpanish, KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_ALTGR, 4U,
         KEY_CODE_MAP_COUNT_STANDARD, KEY_CODE_MAP_COMP_DEAD_KEYS},
        {&KeyCodeMapSpanishLatin, KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_ALTGR, 4U,
         KEY_CODE_MAP_COUNT_STANDARD, KEY_CODE_MAP_COMP_DEAD_KEYS},
        {&KeyCodeMapGerman, KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_ALTGR, 4U,
         KEY_CODE_MAP_COUNT_STANDARD, KEY_CODE_MAP_COMP_DEAD_KEYS},
        {&KeyCodeMapItalian,
         KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_ALTGR | KEY_CODE_MAP_MODE_ALTGR_SHIFT, 5U,
         KEY_CODE_MAP_COUNT_STANDARD, KEY_CODE_MAP_COMP_DEAD_KEYS},
        {&KeyCodeMapPortuguese, KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_ALTGR, 4U,
         KEY_CODE_MAP_COUNT_STANDARD, KEY_CODE_MAP_COMP_DEAD_KEYS},
        {&KeyCodeMapRussian, KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_CYRILLIC, 5U,
         KEY_CODE_MAP_COUNT_STANDARD, KEY_CODE_MAP_COMP_NONE},
        {&KeyCodeMapKorean, KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_HANGUL, 5U,
         KEY_CODE_MAP_COUNT_STANDARD, KEY_CODE_MAP_COMP_NONE},
        {&KeyCodeMapChineseSimplified, KEY_CODE_MAP_MODE_LATIN, 3U, KEY_CODE_MAP_COUNT_STANDARD,
         KEY_CODE_MAP_COMP_NONE},
        {&KeyCodeMapChineseTraditional,
         KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_ZHUYIN | KEY_CODE_MAP_MODE_CANGJIE, 7U,
         KEY_CODE_MAP_COUNT_STANDARD, KEY_CODE_MAP_COMP_NONE},
        {&KeyCodeMapJapanese,
         KEY_CODE_MAP_MODE_LATIN | KEY_CODE_MAP_MODE_HIRAGANA | KEY_CODE_MAP_MODE_KATAKANA, 7U,
         KEY_CODE_MAP_COUNT_JAPANESE, KEY_CODE_MAP_COMP_NONE},
    }};

    for (const auto& spec : specs) {
        const auto& map = *spec.map;
        CHECK(map.size() == 0x1000U);

        const u32 mode = ReadU32(map, 0x00U);
        const u32 cols = ReadU32(map, 0x04U);
        const u32 count = ReadU32(map, 0x08U);
        const u32 comp = ReadU32(map, 0x0CU);

        CHECK(mode == spec.expected_mode);
        CHECK(cols == spec.expected_columns);
        CHECK(cols == ExpectedColumnsForMode(mode));
        CHECK(count == spec.expected_count);
        CHECK(comp == spec.expected_comp);

        for (std::size_t off = 0x10U; off < KEY_CODE_MAP_HEADER_SIZE; ++off) {
            CHECK(map[off] == 0U);
        }

        const std::size_t table_end =
            KEY_CODE_MAP_HEADER_SIZE + static_cast<std::size_t>(count) * cols * sizeof(u16);
        REQUIRE(table_end <= map.size());
        for (std::size_t off = table_end; off < map.size(); ++off) {
            CHECK(map[off] == 0U);
        }

        // Control and navigation HID keys carry zero flags and zero code points.
        for (const u8 control_hid : std::array<u8, 6>{{0x28U, 0x29U, 0x2AU, 0x2BU, 0x39U, 0x58U}}) {
            for (u32 col = 0; col < cols; ++col) {
                CHECK(ReadEntryWord(map, control_hid, col) == 0U);
            }
        }

        // Space and keypad arithmetic operators are present across every layout.
        CHECK(ReadEntryWord(map, 0x2CU, 0U) == KEY_CODE_FLAG_PRINTABLE);
        CHECK(ReadEntryWord(map, 0x2CU, 1U) == ' ');
        CHECK(ReadEntryWord(map, 0x54U, 0U) == KEY_CODE_FLAG_PRINTABLE);
        CHECK(ReadEntryWord(map, 0x54U, 1U) == '/');
        CHECK(ReadEntryWord(map, 0x55U, 1U) == '*');
        CHECK(ReadEntryWord(map, 0x56U, 1U) == '-');
        CHECK(ReadEntryWord(map, 0x57U, 1U) == '+');
        CHECK(ReadEntryWord(map, 0x67U, 1U) == '=');

        // Keypad digits obey NumLock: unshifted NUL, shifted digit.
        for (u8 offset = 0; offset < 9U; ++offset) {
            const u8 hid = static_cast<u8>(0x59U + offset);
            CHECK(ReadEntryWord(map, hid, 0U) == KEY_CODE_FLAG_NUMLOCK);
            CHECK(ReadEntryWord(map, hid, 1U) == 0U);
            CHECK(ReadEntryWord(map, hid, 2U) == static_cast<u16>('1' + offset));
        }
        CHECK(ReadEntryWord(map, 0x62U, 0U) == KEY_CODE_FLAG_NUMLOCK);
        CHECK(ReadEntryWord(map, 0x62U, 1U) == 0U);
        CHECK(ReadEntryWord(map, 0x62U, 2U) == '0');
        CHECK(ReadEntryWord(map, 0x63U, 0U) == KEY_CODE_FLAG_NUMLOCK);
        CHECK(ReadEntryWord(map, 0x63U, 1U) == 0U);
        CHECK(ReadEntryWord(map, 0x63U, 2U) == '.');
    }
}

TEST_CASE("KeyCodeMap layouts encode modifier flags, dead keys, AltGr, and national characters",
          "[core][set][key_code_map]") {
    // US-International: CapsLock+AltGr letters, AltGr-only CapsLock on c-cedilla, and dead keys.
    CHECK(ReadEntryWord(KeyCodeMapEnglishUsInternational, 0x04U, 0U) ==
          KEY_CODE_FLAG_CAPSLOCK_LATIN_ALTGR);
    CHECK(ReadEntryWord(KeyCodeMapEnglishUsInternational, 0x04U, 1U) == 'a');
    CHECK(ReadEntryWord(KeyCodeMapEnglishUsInternational, 0x04U, 2U) == 'A');
    CHECK(ReadEntryWord(KeyCodeMapEnglishUsInternational, 0x04U, 3U) == 0x00E1U); // á
    CHECK(ReadEntryWord(KeyCodeMapEnglishUsInternational, 0x04U, 4U) == 0x00C1U); // Á
    CHECK(ReadEntryWord(KeyCodeMapEnglishUsInternational, 0x05U, 0U) ==
          KEY_CODE_FLAG_CAPSLOCK_LATIN);
    CHECK(ReadEntryWord(KeyCodeMapEnglishUsInternational, 0x36U, 0U) == KEY_CODE_FLAG_CAPSLOCK_ALTGR);
    CHECK(ReadEntryWord(KeyCodeMapEnglishUsInternational, 0x36U, 3U) == 0x00E7U); // ç
    CHECK(ReadEntryWord(KeyCodeMapEnglishUsInternational, 0x36U, 4U) == 0x00C7U); // Ç
    CHECK(ReadEntryWord(KeyCodeMapEnglishUsInternational, 0x34U, 1U) == DEAD_KEY_APOSTROPHE);
    CHECK(ReadEntryWord(KeyCodeMapEnglishUsInternational, 0x34U, 2U) == DEAD_KEY_QUOTATION);
    CHECK(ReadEntryWord(KeyCodeMapEnglishUsInternational, 0x35U, 1U) == DEAD_KEY_GRAVE);
    CHECK(ReadEntryWord(KeyCodeMapEnglishUsInternational, 0x35U, 2U) == DEAD_KEY_TILDE);

    // UK ISO: shifted 3 = £, shifted ' = @, ISO key 0x64 = \/|.
    CHECK(ReadEntryWord(KeyCodeMapEnglishUk, 0x20U, 2U) == 0x00A3U);
    CHECK(ReadEntryWord(KeyCodeMapEnglishUk, 0x34U, 2U) == '@');
    CHECK(ReadEntryWord(KeyCodeMapEnglishUk, 0x64U, 1U) == '\\');
    CHECK(ReadEntryWord(KeyCodeMapEnglishUk, 0x64U, 2U) == '|');

    // French AZERTY: Q/A and Z/W swapped, M at 0x33, unshifted digits produce symbols.
    CHECK(ReadEntryWord(KeyCodeMapFrench, 0x14U, 1U) == 'a');
    CHECK(ReadEntryWord(KeyCodeMapFrench, 0x04U, 1U) == 'q');
    CHECK(ReadEntryWord(KeyCodeMapFrench, 0x1AU, 1U) == 'z');
    CHECK(ReadEntryWord(KeyCodeMapFrench, 0x1DU, 1U) == 'w');
    CHECK(ReadEntryWord(KeyCodeMapFrench, 0x33U, 1U) == 'm');
    CHECK(ReadEntryWord(KeyCodeMapFrench, 0x1EU, 1U) == '&');
    CHECK(ReadEntryWord(KeyCodeMapFrench, 0x1EU, 2U) == '1');
    CHECK(ReadEntryWord(KeyCodeMapFrench, 0x08U, 3U) == 0x20ACU); // €

    // French Canadian: é on 0x38 and guillemets on 0x64.
    CHECK(ReadEntryWord(KeyCodeMapFrenchCa, 0x38U, 1U) == 0x00E9U);
    CHECK(ReadEntryWord(KeyCodeMapFrenchCa, 0x38U, 2U) == 0x00C9U);
    CHECK(ReadEntryWord(KeyCodeMapFrenchCa, 0x64U, 1U) == 0x00ABU);
    CHECK(ReadEntryWord(KeyCodeMapFrenchCa, 0x64U, 2U) == 0x00BBU);

    // German QWERTZ: Z/Y swapped, umlauts and sharp s.
    CHECK(ReadEntryWord(KeyCodeMapGerman, 0x1CU, 1U) == 'z');
    CHECK(ReadEntryWord(KeyCodeMapGerman, 0x1DU, 1U) == 'y');
    CHECK(ReadEntryWord(KeyCodeMapGerman, 0x2DU, 1U) == 0x00DFU); // ß
    CHECK(ReadEntryWord(KeyCodeMapGerman, 0x2FU, 1U) == 0x00FCU); // ü
    CHECK(ReadEntryWord(KeyCodeMapGerman, 0x33U, 1U) == 0x00F6U); // ö
    CHECK(ReadEntryWord(KeyCodeMapGerman, 0x34U, 1U) == 0x00E4U); // ä
    CHECK(ReadEntryWord(KeyCodeMapGerman, 0x14U, 3U) == '@');

    // Italian, Spanish, Latin American Spanish, and Portuguese distinct keys.
    CHECK(ReadEntryWord(KeyCodeMapItalian, 0x2FU, 1U) == 0x00E8U);     // è
    CHECK(ReadEntryWord(KeyCodeMapSpanish, 0x33U, 1U) == 0x00F1U);     // ñ
    CHECK(ReadEntryWord(KeyCodeMapSpanishLatin, 0x33U, 1U) == 0x00F1U); // ñ
    CHECK(ReadEntryWord(KeyCodeMapSpanishLatin, 0x14U, 3U) == '@');
    CHECK(ReadEntryWord(KeyCodeMapPortuguese, 0x33U, 1U) == 0x00E7U); // ç

    // Russian ЙЦУКЕН: Cyrillic caps flags and letter/punctuation assignments.
    CHECK(ReadEntryWord(KeyCodeMapRussian, 0x04U, 0U) == KEY_CODE_FLAG_CAPSLOCK_LATIN_SECONDARY);
    CHECK(ReadEntryWord(KeyCodeMapRussian, 0x04U, 3U) == 0x0444U); // ф
    CHECK(ReadEntryWord(KeyCodeMapRussian, 0x04U, 4U) == 0x0424U); // Ф
    CHECK(ReadEntryWord(KeyCodeMapRussian, 0x2FU, 0U) == KEY_CODE_FLAG_CAPSLOCK_SECONDARY);
    CHECK(ReadEntryWord(KeyCodeMapRussian, 0x2FU, 3U) == 0x0445U); // х

    // Korean 2-Bulsik: Latin paired with Hangul jamo + shifted tense consonants.
    CHECK(ReadEntryWord(KeyCodeMapKorean, 0x04U, 3U) == 0x3141U); // ㅁ
    CHECK(ReadEntryWord(KeyCodeMapKorean, 0x14U, 3U) == 0x3142U); // ㅂ
    CHECK(ReadEntryWord(KeyCodeMapKorean, 0x14U, 4U) == 0x3143U); // ㅃ

    // Chinese Simplified (3-column US Latin) vs Chinese Traditional (Zhuyin + Cangjie).
    CHECK(ReadEntryWord(KeyCodeMapChineseSimplified, 0x04U, 1U) == 'a');
    CHECK(ReadEntryWord(KeyCodeMapChineseTraditional, 0x04U, 3U) == 0x3107U); // ㄇ
    CHECK(ReadEntryWord(KeyCodeMapChineseTraditional, 0x04U, 5U) == 0x65E5U); // 日

    // Japanese JIS: Latin, Hiragana, Katakana, plus Ro (0x87) and Yen (0x89).
    CHECK(ReadEntryWord(KeyCodeMapJapanese, 0x04U, 3U) == 0x3061U); // ち
    CHECK(ReadEntryWord(KeyCodeMapJapanese, 0x04U, 5U) == 0x30C1U); // チ
    CHECK(ReadEntryWord(KeyCodeMapJapanese, 0x87U, 1U) == '\\');
    CHECK(ReadEntryWord(KeyCodeMapJapanese, 0x87U, 2U) == '_');
    CHECK(ReadEntryWord(KeyCodeMapJapanese, 0x87U, 3U) == 0x308DU); // ろ
    CHECK(ReadEntryWord(KeyCodeMapJapanese, 0x89U, 1U) == 0x00A5U); // ¥
    CHECK(ReadEntryWord(KeyCodeMapJapanese, 0x89U, 3U) == 0x30FCU); // ー
}

TEST_CASE("KeyCodeMap selection honors locale overrides and falls back safely",
          "[core][set][key_code_map]") {
    for (std::size_t idx = 0; idx < available_language_codes.size(); ++idx) {
        bool found = false;
        const auto& map = GetKeyCodeMapForLanguageIndex(idx, &found);
        CHECK(found);
        CHECK(map.size() == 0x1000U);
        CHECK(&map == &GetKeyCodeMapForLanguage(available_language_codes[idx]));
    }

    // EnglishUs layout dispatches to localized Asian layouts when matched with KO/ZH.
    CHECK(&GetKeyCodeMapForLayout(KeyboardLayout::EnglishUs, LanguageCode::EN_US) ==
          &KeyCodeMapEnglishUsInternational);
    CHECK(&GetKeyCodeMapForLayout(KeyboardLayout::EnglishUs, LanguageCode::KO) ==
          &KeyCodeMapKorean);
    CHECK(&GetKeyCodeMapForLayout(KeyboardLayout::EnglishUs, LanguageCode::ZH_HANS) ==
          &KeyCodeMapChineseSimplified);
    CHECK(&GetKeyCodeMapForLayout(KeyboardLayout::EnglishUs, LanguageCode::ZH_HANT) ==
          &KeyCodeMapChineseTraditional);

    // Every explicit KeyboardLayout resolves to its dedicated map.
    CHECK(&GetKeyCodeMapForLayout(KeyboardLayout::Japanese, LanguageCode::JA) ==
          &KeyCodeMapJapanese);
    CHECK(&GetKeyCodeMapForLayout(KeyboardLayout::EnglishUk, LanguageCode::EN_GB) ==
          &KeyCodeMapEnglishUk);
    CHECK(&GetKeyCodeMapForLayout(KeyboardLayout::French, LanguageCode::FR) == &KeyCodeMapFrench);
    CHECK(&GetKeyCodeMapForLayout(KeyboardLayout::FrenchCa, LanguageCode::FR_CA) ==
          &KeyCodeMapFrenchCa);
    CHECK(&GetKeyCodeMapForLayout(KeyboardLayout::Spanish, LanguageCode::ES) == &KeyCodeMapSpanish);
    CHECK(&GetKeyCodeMapForLayout(KeyboardLayout::SpanishLatin, LanguageCode::ES_419) ==
          &KeyCodeMapSpanishLatin);
    CHECK(&GetKeyCodeMapForLayout(KeyboardLayout::German, LanguageCode::DE) == &KeyCodeMapGerman);
    CHECK(&GetKeyCodeMapForLayout(KeyboardLayout::Italian, LanguageCode::IT) == &KeyCodeMapItalian);
    CHECK(&GetKeyCodeMapForLayout(KeyboardLayout::Portuguese, LanguageCode::PT) ==
          &KeyCodeMapPortuguese);
    CHECK(&GetKeyCodeMapForLayout(KeyboardLayout::Russian, LanguageCode::RU) == &KeyCodeMapRussian);
    CHECK(&GetKeyCodeMapForLayout(KeyboardLayout::Korean, LanguageCode::KO) == &KeyCodeMapKorean);
    CHECK(&GetKeyCodeMapForLayout(KeyboardLayout::ChineseSimplified, LanguageCode::ZH_CN) ==
          &KeyCodeMapChineseSimplified);
    CHECK(&GetKeyCodeMapForLayout(KeyboardLayout::ChineseTraditional, LanguageCode::ZH_TW) ==
          &KeyCodeMapChineseTraditional);

    // Invalid layout, unknown language code, and out-of-range indices fall back cleanly.
    CHECK(&GetKeyCodeMapForLayout(static_cast<KeyboardLayout>(999), LanguageCode::EN_US) ==
          &KeyCodeMapEnglishUsInternational);

    bool found = true;
    CHECK(&GetKeyCodeMapForLanguage(static_cast<LanguageCode>(0), &found) ==
          &KeyCodeMapEnglishUsInternational);
    CHECK_FALSE(found);

    found = true;
    CHECK(&GetKeyCodeMapForLanguageIndex(available_language_codes.size(), &found) ==
          &KeyCodeMapEnglishUsInternational);
    CHECK_FALSE(found);

    found = true;
    CHECK(&GetKeyCodeMapForLanguageIndex(std::numeric_limits<std::size_t>::max(), &found) ==
          &KeyCodeMapEnglishUsInternational);
    CHECK_FALSE(found);
}

} // namespace Service::Set
