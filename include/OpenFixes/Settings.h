#pragma once

#include <string_view>

/**
 * @brief Read access to Data/SKSE/Plugins/OpenFixes.ini, for the framework
 *
 * The INI holds [General] options and every fix's toggle, "<FixId> = true|false" in its section; fixes have no other
 * settings and don't read the INI themselves. Keys and sections are case-insensitive, and anything after ';' or '#' on
 * a line is a comment.
 */
namespace OpenFixes::Settings {

/**
 * @brief Loads the INI file; called once by the framework before any fix is installed
 */
void load();

[[nodiscard]] bool getBool(std::string_view section, std::string_view key, bool defaultValue);

} // namespace OpenFixes::Settings
