#pragma once

namespace OpenFixes::Framework {

/**
 * @brief Checks for conflicting mods and installs every enabled fix; called from SKSEPluginLoad after
 * Settings::load()
 */
void load();

} // namespace OpenFixes::Framework
