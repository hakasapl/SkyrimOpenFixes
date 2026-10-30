#include "PCH.h"

#include "Framework/Paths.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <string>
#include <string_view>

namespace OpenFixes::Settings {

namespace {

std::map<std::string, std::string> s_values; /**< "section.key" (lower case) -> raw value */

std::string lower(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string_view trim(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string makeKey(std::string_view section, std::string_view key)
{
    return lower(section) + "." + lower(key);
}

const std::string* find(std::string_view section, std::string_view key)
{
    const auto it = s_values.find(makeKey(section, key));
    return it == s_values.end() ? nullptr : &it->second;
}

} // namespace

void load()
{
    const auto path = Paths::pluginDirectory() / (std::string(PLUGIN_NAME) + ".ini");
    std::ifstream file(path);
    if (!file) {
        spdlog::warn("{} not found, every fix uses its default on/off state", path.string());
        return;
    }

    std::string section;
    std::string line;
    std::size_t lineNumber = 0;
    while (std::getline(file, line)) {
        ++lineNumber;
        std::string_view view = line;

        // Everything after ';' or '#' is a comment, including comments to the right of a value
        if (const auto comment = view.find_first_of(";#"); comment != std::string_view::npos) {
            view = view.substr(0, comment);
        }
        view = trim(view);
        if (view.empty()) {
            continue;
        }

        if (view.front() == '[') {
            if (view.back() != ']') {
                spdlog::warn("{}:{}: malformed section header ignored", path.filename().string(), lineNumber);
                continue;
            }
            section = std::string(trim(view.substr(1, view.size() - 2)));
            continue;
        }

        const auto equals = view.find('=');
        if (equals == std::string_view::npos) {
            spdlog::warn("{}:{}: line without '=' ignored", path.filename().string(), lineNumber);
            continue;
        }
        s_values[makeKey(section, trim(view.substr(0, equals)))] = std::string(trim(view.substr(equals + 1)));
    }
    spdlog::info("Loaded {} setting(s) from {}", s_values.size(), path.string());
}

bool getBool(std::string_view section, std::string_view key, bool defaultValue)
{
    const auto* value = find(section, key);
    if (value == nullptr) {
        return defaultValue;
    }
    const auto text = lower(*value);
    if (text == "true" || text == "1" || text == "yes" || text == "on") {
        return true;
    }
    if (text == "false" || text == "0" || text == "no" || text == "off") {
        return false;
    }
    spdlog::warn("[{}] {} = {} is not a boolean, using {}", section, key, *value, defaultValue);
    return defaultValue;
}

} // namespace OpenFixes::Settings
