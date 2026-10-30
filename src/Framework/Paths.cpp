#include "PCH.h"

#include "Framework/Paths.h"

#include <Windows.h>

#include <array>
#include <filesystem>

namespace OpenFixes::Paths {

std::filesystem::path pluginDirectory()
{
    static const std::filesystem::path directory = [] {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&pluginDirectory),
            &self);

        std::array<wchar_t, 32768> buffer {};
        const auto length = GetModuleFileNameW(self, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (self == nullptr || length == 0 || length >= buffer.size()) {
            return std::filesystem::path(L"Data/SKSE/Plugins");
        }
        return std::filesystem::path(std::wstring_view(buffer.data(), length)).parent_path();
    }();
    return directory;
}

} // namespace OpenFixes::Paths
