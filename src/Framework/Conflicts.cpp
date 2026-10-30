#include "PCH.h"

#include "Framework/Conflicts.h"
#include "Framework/Paths.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace OpenFixes::Conflicts {

namespace {

std::string lower(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/**
 * @brief Lower-case file name -> file name of every DLL in the plugin folder (seen through MO2's virtual file system
 * if used)
 */
const std::map<std::string, std::string>& installedPlugins()
{
    static const std::map<std::string, std::string> plugins = [] {
        std::map<std::string, std::string> found;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(Paths::pluginDirectory(), ec)) {
            if (entry.is_regular_file(ec) && lower(entry.path().extension().string()) == ".dll") {
                const auto name = entry.path().filename().string();
                found.emplace(lower(name), name);
            }
        }
        if (ec) {
            spdlog::warn("Could not list {}: {}", Paths::pluginDirectory().string(), ec.message());
        }
        return found;
    }();
    return plugins;
}

} // namespace

std::vector<std::string> installedFor(const Fix& fix)
{
    static const auto self = lower(std::string(PLUGIN_NAME) + ".dll");

    std::vector<std::string> installed;
    for (const auto dll : fix.info().skipIfInstalled) {
        const auto key = lower(dll);
        if (key == self) {
            continue;
        }
        if (const auto it = installedPlugins().find(key); it != installedPlugins().end()) {
            installed.push_back(it->second);
        } else if (GetModuleHandleA(std::string(dll).c_str()) != nullptr) {
            installed.emplace_back(dll);
        }
    }
    return installed;
}

} // namespace OpenFixes::Conflicts
