#include "PCH.h"

#include "Framework/Conflicts.h"
#include "Framework/Framework.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OpenFixes {

namespace {

constexpr std::uint32_t SERIALIZATION_ID = 'OPFX';

std::vector<std::unique_ptr<Fix>>& registry()
{
    static std::vector<std::unique_ptr<Fix>> fixes;
    return fixes;
}

std::vector<Fix*> s_installed;

std::string_view runtimeName(Runtime runtime)
{
    switch (runtime) {
    case Runtime::SE:
        return "SE";
    case Runtime::AE:
        return "AE";
    case Runtime::AE17:
        return "AE 1.7";
    case Runtime::VR:
        return "VR";
    default:
        return "unknown";
    }
}

/**
 * @brief Runs callback for every installed fix, attributing any patches it makes to that fix
 */
template <class F>
void forEachInstalled(F&& callback)
{
    for (auto* fix : s_installed) {
        Hooks::setCurrentFix(fix->info().id);
        try {
            callback(*fix);
        } catch (const std::exception& e) {
            spdlog::error("[{}] {}", fix->info().id, e.what());
        }
    }
    Hooks::setCurrentFix({});
}

void onMessage(SKSE::MessagingInterface::Message* message)
{
    forEachInstalled([message](Fix& fix) { fix.onMessage(message); });

    if (message->type == SKSE::MessagingInterface::kDataLoaded) {
        // Most fixes have patched everything by now
        Hooks::reportOverlaps();
        spdlog::info("{} trampoline bytes used", Hooks::trampolineBytes());
    }
}

Fix* ownerOf(std::uint32_t type)
{
    for (auto* fix : s_installed) {
        if (std::ranges::contains(fix->recordTypes(), type)) {
            return fix;
        }
    }
    return nullptr;
}

void registerSerialization()
{
    std::set<std::uint32_t> types;
    for (const auto* fix : s_installed) {
        for (const auto type : fix->recordTypes()) {
            if (!types.insert(type).second) {
                spdlog::error("Co-save record type {:08X} is claimed by more than one fix", type);
            }
        }
    }
    if (types.empty()) {
        return;
    }

    const auto* serialization = SKSE::GetSerializationInterface();
    serialization->SetUniqueID(SERIALIZATION_ID);
    serialization->SetSaveCallback([](SKSE::SerializationInterface* intfc) {
        forEachInstalled([intfc](Fix& fix) { fix.onSave(intfc); });
    });
    serialization->SetRevertCallback([](SKSE::SerializationInterface* intfc) {
        forEachInstalled([intfc](Fix& fix) { fix.onRevert(intfc); });
    });
    serialization->SetLoadCallback([](SKSE::SerializationInterface* intfc) {
        std::uint32_t type = 0;
        std::uint32_t version = 0;
        std::uint32_t length = 0;
        while (intfc->GetNextRecordInfo(type, version, length)) {
            auto* owner = ownerOf(type);
            if (owner == nullptr) {
                // A fix that wrote this record is turned off now; SKSE skips the unread bytes
                spdlog::info("Skipping co-save record {:08X} with no installed owner", type);
                continue;
            }
            Hooks::setCurrentFix(owner->info().id);
            try {
                owner->onLoad(intfc, type, version, length);
            } catch (const std::exception& e) {
                spdlog::error("[{}] {}", owner->info().id, e.what());
            }
            Hooks::setCurrentFix({});
        }
    });
}

} // namespace

Runtime currentRuntime() noexcept
{
    static const Runtime runtime = [] {
        // VR (1.4.15) has to be told apart before the version checks, which would take it for SE
        if (REL::Module::IsVR()) {
            return Runtime::VR;
        }
        const auto version = REL::Module::get().version();
        if (version < SKSE::RUNTIME_SSE_1_6_317) {
            return Runtime::SE;
        }
        if (version < SKSE::RUNTIME_SSE_1_7_99) {
            return Runtime::AE;
        }
        return Runtime::AE17;
    }();
    return runtime;
}

bool isInstalled(std::string_view fixId) noexcept
{
    return std::ranges::any_of(s_installed, [fixId](const Fix* fix) { return fix->info().id == fixId; });
}

void registerFix(std::unique_ptr<Fix> fix)
{
    registry().push_back(std::move(fix));
}

namespace Framework {

void load()
{
    // Registration order depends on static initialisation, so sort for a stable install order
    auto& fixes = registry();
    std::ranges::sort(fixes, [](const auto& lhs, const auto& rhs) {
        return std::pair(lhs->info().section, lhs->info().id) < std::pair(rhs->info().section, rhs->info().id);
    });

    const auto runtime = currentRuntime();
    std::set<std::string_view> seen;
    std::vector<Fix*> enabled;
    for (const auto& fix : fixes) {
        const auto& info = fix->info();
        if (!seen.insert(info.id).second) {
            spdlog::error("Duplicate fix id {} ignored", info.id);
            continue;
        }
        if (!Settings::getBool(info.section, info.id, info.enabledByDefault)) {
            spdlog::info("[{}] off in the INI", info.id);
        } else if (!hasRuntime(info.runtimes, runtime)) {
            spdlog::info("[{}] not available on {} {}", info.id, runtimeName(runtime),
                REL::Module::get().version().string("."));
        } else if (const auto providers = Conflicts::installedFor(*fix); !providers.empty()) {
            // The original mod (or another mod with the same fix) is installed, so leave it to that mod
            std::string names;
            for (const auto& provider : providers) {
                names += names.empty() ? provider : ", " + provider;
            }
            spdlog::info("[{}] skipped, {} is installed", info.id, names);
        } else {
            enabled.push_back(fix.get());
        }
    }

    for (auto* fix : enabled) {
        const auto& info = fix->info();
        Hooks::setCurrentFix(info.id);
        bool installed = false;
        try {
            installed = fix->install();
        } catch (const std::exception& e) {
            spdlog::error("[{}] install failed: {}", info.id, e.what());
        }
        if (installed) {
            s_installed.push_back(fix);
            spdlog::info("[{}] installed", info.id);
        } else {
            spdlog::warn("[{}] could not be installed", info.id);
        }
    }
    Hooks::setCurrentFix({});

    spdlog::info("{} of {} fix(es) installed", s_installed.size(), fixes.size());

    SKSE::GetMessagingInterface()->RegisterListener(onMessage);
    registerSerialization();
}

} // namespace Framework

} // namespace OpenFixes
