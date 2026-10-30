#pragma once

#include <filesystem>

namespace OpenFixes::Paths {

/**
 * @brief Folder OpenFixes.dll was loaded from (Data/SKSE/Plugins, as the game sees it)
 */
[[nodiscard]] std::filesystem::path pluginDirectory();

} // namespace OpenFixes::Paths
