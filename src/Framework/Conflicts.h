#pragma once

#include <string>
#include <vector>

namespace OpenFixes {
class Fix;
}

namespace OpenFixes::Conflicts {

/**
 * @brief The installed SKSE plugins, among the fix's skipIfInstalled list, that already provide its fix
 *
 * The plugin folder is scanned once; a DLL that is already loaded counts as installed too.
 */
[[nodiscard]] std::vector<std::string> installedFor(const Fix& fix);

} // namespace OpenFixes::Conflicts
