// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "frontend_common/game_list_metadata.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

#include "core/file_sys/control_metadata.h"
#include "core/loader/loader.h"

namespace GameListDiscovery {
namespace {

std::string TrimWhitespace(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return std::string{text};
}

std::vector<std::string> ExtractLanguages(u32 mask) {
    using FileSys::SupportedLanguage;
    constexpr std::array<std::pair<SupportedLanguage, std::string_view>, 18> Map{{
        {SupportedLanguage::AmericanEnglish, "English"},
        {SupportedLanguage::BritishEnglish, "English"},
        {SupportedLanguage::Japanese, "Japanese"},
        {SupportedLanguage::French, "French"},
        {SupportedLanguage::CanadianFrench, "French"},
        {SupportedLanguage::German, "German"},
        {SupportedLanguage::Spanish, "Spanish"},
        {SupportedLanguage::LatinAmericanSpanish, "Spanish"},
        {SupportedLanguage::Italian, "Italian"},
        {SupportedLanguage::Dutch, "Dutch"},
        {SupportedLanguage::Portuguese, "Portuguese"},
        {SupportedLanguage::BrazilianPortuguese, "Portuguese"},
        {SupportedLanguage::Russian, "Russian"},
        {SupportedLanguage::Korean, "Korean"},
        {SupportedLanguage::TraditionalChinese, "Chinese"},
        {SupportedLanguage::SimplifiedChinese, "Chinese"},
        {SupportedLanguage::Polish, "Polish"},
        {SupportedLanguage::Thai, "Thai"},
    }};
    std::vector<std::string> languages;
    for (const auto& [flag, label] : Map) {
        if ((mask & static_cast<u32>(flag)) == 0) {
            continue;
        }
        if (std::ranges::find(languages, label) == languages.end()) {
            languages.emplace_back(label);
        }
    }
    return languages;
}

} // namespace

TitleMetadata ReadTitleMetadata(Loader::AppLoader& loader) {
    TitleMetadata metadata{.title = " "};
    loader.ReadIcon(metadata.icon);
    metadata.title_valid = loader.ReadTitle(metadata.title) == Loader::ResultStatus::Success;

    FileSys::NACP nacp;
    if (loader.ReadControlData(nacp) == Loader::ResultStatus::Success) {
        metadata.publisher = TrimWhitespace(nacp.GetDeveloperName());
        metadata.version = TrimWhitespace(nacp.GetVersionString());
        metadata.languages = ExtractLanguages(nacp.GetSupportedLanguages());
        if (!metadata.title_valid || TrimWhitespace(metadata.title).empty()) {
            const auto app_name = TrimWhitespace(nacp.GetApplicationName());
            if (!app_name.empty()) {
                metadata.title = app_name;
                metadata.title_valid = true;
            }
        }
    }
    if (metadata.version.empty()) {
        std::string version;
        if (loader.ReadDisplayVersion(version) == Loader::ResultStatus::Success) {
            metadata.version = TrimWhitespace(version);
        }
    }
    return metadata;
}

} // namespace GameListDiscovery
