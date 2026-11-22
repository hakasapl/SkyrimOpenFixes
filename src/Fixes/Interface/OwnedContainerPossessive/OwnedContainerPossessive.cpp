// English possessive ("Roggvir's Coffin") hardcoded in owned container names in other languages
//
// Ported from "Language Fixes SKSE" by powerofthree (powerofthree), https://github.com/powerof3/LanguageFixes
// (commit ce9c554)
// Original code: MIT, Copyright (c) 2022 powerofthree. Full text in LICENSE in this folder.
// SPDX-License-Identifier: MIT

#include "PCH.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace {

/**
 * TESObjectCONT::GetActivateText names an owned container by joining the owner's name, a hardcoded English "'s " and
 * the container's name, in every language. The fix rewrites that line with the game language's own possessive (e.g.
 * "<container> de <owner>" in Spanish, "<owner>s <container>" in German), or "<container> - <owner>" for languages
 * without a rule. The language is the game's sLanguage setting. Left out of the
 * original: its NPC name rewrite (outside English the engine already writes NPC names as "Name - Owner" on purpose)
 * and its language override setting.
 */

constexpr std::size_t GET_ACTIVATE_TEXT = 0x4C;

enum class Language : std::uint8_t {
    ENGLISH,
    CHINESE,
    FRENCH,
    GERMAN,
    JAPANESE,
    KOREAN,
    POLISH,
    PORTUGUESE,
    SPANISH,
    OTHER,
};

// English (nothing to rewrite) until the data has loaded and sLanguage is read
Language s_language = Language::ENGLISH;

std::string toLower(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return out;
}

std::string toUpper(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return out;
}

bool isSpace(char ch) { return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\v' || ch == '\f' || ch == '\r'; }

Language languageFromSetting()
{
    auto* collection = RE::INISettingCollection::GetSingleton();
    auto* setting = collection != nullptr ? collection->GetSetting("sLanguage:General") : nullptr;
    const auto* value = setting != nullptr ? setting->GetString() : nullptr;
    const auto language = toUpper(value != nullptr ? value : "");
    if (language == "ENGLISH") {
        return Language::ENGLISH;
    }
    if (language == "CHINESE") {
        return Language::CHINESE;
    }
    if (language == "FRENCH") {
        return Language::FRENCH;
    }
    if (language == "GERMAN") {
        return Language::GERMAN;
    }
    if (language == "JAPANESE") {
        return Language::JAPANESE;
    }
    if (language == "KOREAN") {
        return Language::KOREAN;
    }
    if (language == "POLISH") {
        return Language::POLISH;
    }
    if (language == "PORTUGUESE") {
        return Language::PORTUGUESE;
    }
    if (language == "SPANISH") {
        return Language::SPANISH;
    }
    return Language::OTHER;
}

bool isVowel(char ch) { return ch == 'a' || ch == 'e' || ch == 'i' || ch == 'o' || ch == 'u'; }

// French uses "d'" before a vowel or a silent h followed by a vowel
bool frenchElides(std::string_view owner)
{
    if (owner.empty()) {
        return false;
    }
    const auto lower = toLower(owner);
    if (lower.front() == 'h' && lower.size() > 1) {
        return isVowel(lower[1]);
    }
    return isVowel(lower.front());
}

// Polish genitive of the owner's name. As in the original, the sex comes from the reference passed to GetActivateText,
// which for a container is the container itself, so it is always unknown there
std::string polishGenitive(std::string owner,
                           RE::TESObjectREFR* ownerRef)
{
    auto sex = RE::SEX::kNone;
    if (auto* actor = ownerRef != nullptr ? ownerRef->As<RE::Actor>() : nullptr; actor != nullptr) {
        if (auto* base = actor->GetActorBase(); base != nullptr) {
            sex = base->GetSex();
        }
    }
    switch (toLower(owner).back()) {
    case 'a':
        owner.back() = 'y';
        break;
    case 'y':
        if (sex == RE::SEX::kMale) {
            owner.pop_back();
            owner.append("'ego");
        }
        break;
    default:
        if (sex != RE::SEX::kFemale) {
            owner.append("a");
        }
        break;
    }
    return owner;
}

std::string possessive(std::string owner,
                       std::string object,
                       RE::TESObjectREFR* ownerRef)
{
    switch (s_language) {
    case Language::CHINESE:
        return owner + "的" + object;
    case Language::FRENCH:
        return object + (frenchElides(owner) ? " d'" : " de ") + owner;
    case Language::GERMAN:
        return owner + (toLower(owner).back() == 's' ? "' " : "s ") + object;
    case Language::JAPANESE:
        return owner + "の" + object;
    case Language::KOREAN:
        return owner + "의 " + object;
    case Language::POLISH:
        return object + " " + polishGenitive(std::move(owner), ownerRef);
    case Language::PORTUGUESE:
    case Language::SPANISH:
        return object + " de " + owner;
    default:
        return object + " - " + owner;
    }
}

// Splits a line at its first "'s" followed by whitespace, with text on both sides (the original's
// ([\S\s]+?)'s\s([\S\s]+))
std::optional<std::pair<std::string,
                        std::string>>
splitPossessive(std::string_view line)
{
    for (std::size_t i = 1; i + 3 < line.size(); ++i) {
        if (line[i] == '\'' && line[i + 1] == 's' && isSpace(line[i + 2])) {
            return std::pair(std::string(line.substr(0, i)), std::string(line.substr(i + 3)));
        }
    }
    return std::nullopt;
}

struct GetActivateText {
    static bool thunk(RE::TESObjectCONT* container,
                      RE::TESObjectREFR* activator,
                      RE::BSString& text)
    {
        const auto result = func(container, activator, text);
        if (s_language == Language::ENGLISH || text.c_str() == nullptr) {
            return result;
        }
        const std::string_view original = text.c_str();
        if (!original.contains("'s ")) {
            return result;
        }

        // Each line is the action, the name or the empty/locked note; only the name has the possessive
        std::string rewritten;
        std::size_t start = 0;
        while (start < original.size()) {
            const auto end = std::min(original.find('\n', start), original.size());
            const auto line = original.substr(start, end - start);
            if (auto parts = splitPossessive(line)) {
                rewritten += possessive(std::move(parts->first), std::move(parts->second), activator);
            } else {
                rewritten += line;
            }
            rewritten += '\n';
            start = end + 1;
        }

        const auto first = std::ranges::find_if_not(rewritten, isSpace);
        const auto last = std::ranges::find_if_not(rewritten.rbegin(), rewritten.rend(), isSpace).base();
        text = first < last ? std::string(first, last) : std::string();
        return result;
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

class OwnedContainerPossessive final : public OpenFixes::Fix {
public:
    OwnedContainerPossessive()
        : Fix({
              .id = "OwnedContainerPossessive",
              .section = "Interface",
              .description
              = "English possessive (\"Roggvir's Coffin\") hardcoded in owned container names in other languages",
              .origin = "Language Fixes SKSE by powerofthree",
              .runtimes = OpenFixes::Runtime::ALL,
              .skipIfInstalled = {"po3_LanguageFixes.dll"},
          })
    {
    }

    bool install() override
    {
        // The game's TESObjectCONT::GetActivateText, checked so another mod's replacement isn't overwritten
        const REL::Relocation<std::uintptr_t> getActivateText {REL::VariantID(17486, 17888, 0x23CCE0)};
        if (!OpenFixes::Hooks::expectVfunc<RE::TESObjectCONT>(GET_ACTIVATE_TEXT, getActivateText.address())) {
            return false;
        }
        OpenFixes::Hooks::vfunc<RE::TESObjectCONT, GetActivateText>(GET_ACTIVATE_TEXT);
        return true;
    }

    void onMessage(SKSE::MessagingInterface::Message* message) override
    {
        if (message->type == SKSE::MessagingInterface::kDataLoaded) {
            s_language = languageFromSetting();
            spdlog::info("[OwnedContainerPossessive] game language: {}",
                         s_language == Language::ENGLISH ? "English, nothing to rewrite" : "rewriting possessives");
        }
    }
};

} // namespace

OPENFIXES_REGISTER(OwnedContainerPossessive)
