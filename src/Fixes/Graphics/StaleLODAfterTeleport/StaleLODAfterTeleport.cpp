// Distant LOD of the previous location never unloads after fast travel, a long teleport or a worldspace change
//
// Ported from "LOD Refresh Bug Fix" (SkyrimLODFixes) by Bottle (InTheBottle),
// https://github.com/InTheBottle/SkyrimLODFixes (commit e76efd1)
// Original code: GNU GPL v3.0, Copyright (c) 2026 InTheBottle. Full text in LICENSE at the repository root.
// SPDX-License-Identifier: GPL-3.0-only

#include "PCH.h"

#include "OpenFixes/Util.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace {

/**
 * The terrain manager (BGSTerrainManager) refreshes its LOD quadtree a little each frame by walking down from the root
 * towards the player's position. After a long jump the walk no longer reaches the subtree the player left, so the LOD
 * loaded there is never swapped out and the old location's LOD stays drawn. The engine only resets the whole tree with
 * its full teardown (Detach) when it switches to another terrain manager, which doesn't happen after fast travel within
 * a worldspace or when moving between worldspaces that share one manager (a child worldspace using its parent's LOD).
 * After each loading screen, if the player arrived more than uGridsToLoad cells from where they left, or in another
 * worldspace served by the same manager, the fix runs that teardown on the active manager so the LOD rebuilds.
 */

constexpr float CELL_SIZE = 4096.0F;
constexpr std::int32_t DEFAULT_GRIDS_TO_LOAD = 5;
constexpr int MAX_PARENT_HOPS = 16;

struct Waypoint {
    RE::BGSTerrainManager* manager = nullptr;
    RE::FormID worldspace = 0;
    std::int32_t cellX = 0;
    std::int32_t cellY = 0;
    bool valid = false;
};

RE::BGSTerrainManager** s_activeManager = nullptr;
const std::uint32_t* s_gridsToLoad = nullptr;
void (*s_detach)(RE::BGSTerrainManager* manager,
                 bool clearLoaded,
                 bool releaseAll) = nullptr;
Waypoint s_departure;

// The manager a worldspace draws with: its parent's while it uses the parent's LOD
RE::BGSTerrainManager* terrainManagerOf(RE::TESWorldSpace* world)
{
    for (int hops = 0; hops < MAX_PARENT_HOPS && world->parentWorld != nullptr
         && world->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseLODData);
         ++hops) {
        world = world->parentWorld;
    }
    return world->terrainManager;
}

// Cells go negative, so this floors rather than truncates
std::int32_t cellOf(float coordinate) { return static_cast<std::int32_t>(std::floor(coordinate / CELL_SIZE)); }

bool sample(Waypoint& out)
{
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (player == nullptr) {
        return false;
    }
    auto* world = player->GetWorldspace();
    if (world == nullptr) {
        return false;
    }
    auto* manager = terrainManagerOf(world);
    if (manager == nullptr) {
        return false;
    }
    const auto position = player->GetPosition();
    out = {manager, world->GetFormID(), cellOf(position.x), cellOf(position.y), true};
    return true;
}

// Cells within this radius are fully loaded, so moving inside it strands nothing
std::int32_t gridsToLoad()
{
    const auto value = *s_gridsToLoad;
    return value != 0 ? static_cast<std::int32_t>(value) : DEFAULT_GRIDS_TO_LOAD;
}

void resetActiveManager(const char* reason)
{
    auto* manager = *s_activeManager;
    if (manager == nullptr || !manager->hasLOD || manager->rootNode == nullptr) {
        return;
    }
    // Clears the loaded state and releases every LOD handle, the engine's own complete teardown
    s_detach(manager, true, true);
    manager->staticDataLoaded = false;
    spdlog::info("[StaleLODAfterTeleport] {}: reset the LOD of terrain manager {:#x}",
                 reason,
                 reinterpret_cast<std::uintptr_t>(manager));
}

void onLoadingScreenOpened()
{
    // Where the player leaves from; interiors keep the last exterior position
    Waypoint here;
    if (sample(here)) {
        s_departure = here;
    }
}

void onLoadingScreenClosed()
{
    Waypoint arrival;
    if (!sample(arrival)) {
        return;
    }
    const auto departure = s_departure;
    s_departure = arrival;

    // First exterior of the session, or another manager (the engine's switch resets that one itself)
    if (!departure.valid || arrival.manager != departure.manager) {
        return;
    }

    const auto moved = std::max(std::abs(arrival.cellX - departure.cellX), std::abs(arrival.cellY - departure.cellY));
    const char* reason = nullptr;
    if (arrival.worldspace != departure.worldspace) {
        reason = "worldspace changed";
    } else if (moved > gridsToLoad()) {
        reason = "teleport";
    } else {
        return;
    }

    if (auto* tasks = SKSE::GetTaskInterface(); tasks != nullptr) {
        tasks->AddTask([reason] { resetActiveManager(reason); });
    }
}

class LoadingScreenWatcher final : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
public:
    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event,
                                          RE::BSTEventSource<RE::MenuOpenCloseEvent>* /*source*/) override
    {
        if (event != nullptr && event->menuName == RE::LoadingMenu::MENU_NAME) {
            if (event->opening) {
                onLoadingScreenOpened();
            } else {
                onLoadingScreenClosed();
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

class StaleLODAfterTeleport final : public OpenFixes::Fix {
public:
    StaleLODAfterTeleport()
        : Fix({
              .id = "StaleLODAfterTeleport",
              .section = "Graphics",
              .description = "Old distant LOD never unloads after fast travel, a long teleport or a worldspace change",
              .details = "After a loading screen that moved the player more than uGridsToLoad cells, or into another "
                         "worldspace drawn by the same terrain LOD manager, runs the engine's own full LOD reset on "
                         "that manager, so distant terrain and object LOD rebuild for the new position over about a "
                         "second.",
              .origin = "LOD Refresh Bug Fix by Bottle",
              .runtimes = OpenFixes::Runtime::ALL,
              .skipIfInstalled = {"SkyrimLODFixes.dll"},
          })
    {
    }

    bool install() override
    {
        // The VR Address Library lacks these IDs, so VR uses the offsets of the same code in SkyrimVR.exe 1.4.15.
        // BGSTerrainManager::Update 31017 (SE) / 31802 (AE, 1.7) / 0x4C2A30 (VR) switches managers by calling Detach
        // 31026 / 31812 / 0x4C3260 on the old one and storing the new one to the active-manager global
        // 516111 / 402262 / 0x2FE8BA8
        const REL::Relocation<std::uintptr_t> update {REL::VariantID(31017, 31802, 0x4C2A30)};
        const REL::Relocation<std::uintptr_t> detach {REL::VariantID(31026, 31812, 0x4C3260)};
        const REL::Relocation<std::uintptr_t> activeManager {REL::VariantID(516111, 402262, 0x2FE8BA8)};
        const auto detachCall = update.address() + OpenFixes::select<std::uintptr_t>(0x1AB, 0x1E2, 0x1E2, 0x1AB);
        const auto activeStore = detachCall + 0x9; // past the 4-byte store to old+58h
        if (!OpenFixes::Hooks::expectCall(detachCall, detach.address())
            || !OpenFixes::Hooks::expect(activeStore, {0x48, 0x89, 0x3D})) {
            return false;
        }
        if (OpenFixes::Util::ripTarget(activeStore, 7) != activeManager.address()) {
            spdlog::error("[StaleLODAfterTeleport] terrain update stores the active manager elsewhere");
            return false;
        }

        // The terrain update's setup 31067 / 31859 / 0x4C5DA0 copies the uGridsToLoad setting to the global
        // 516188 / 402339 / 0x2FE8DEC
        const REL::Relocation<std::uintptr_t> setup {REL::VariantID(31067, 31859, 0x4C5DA0)};
        const REL::Relocation<std::uintptr_t> grids {REL::VariantID(516188, 402339, 0x2FE8DEC)};
        const auto gridsStore = setup.address() + 0x4A;
        if (!OpenFixes::Hooks::expect(gridsStore, {0x89, 0x05})) {
            return false;
        }
        if (OpenFixes::Util::ripTarget(gridsStore, 6) != grids.address()) {
            spdlog::error("[StaleLODAfterTeleport] terrain update copies uGridsToLoad elsewhere");
            return false;
        }

        s_activeManager = reinterpret_cast<RE::BGSTerrainManager**>(activeManager.address());
        s_gridsToLoad = reinterpret_cast<const std::uint32_t*>(grids.address());
        s_detach = reinterpret_cast<void (*)(RE::BGSTerrainManager*, bool, bool)>(detach.address());
        return true;
    }

    void onMessage(SKSE::MessagingInterface::Message* message) override
    {
        switch (message->type) {
        case SKSE::MessagingInterface::kDataLoaded: {
            static LoadingScreenWatcher watcher;
            if (auto* ui = RE::UI::GetSingleton(); ui != nullptr) {
                ui->AddEventSink<RE::MenuOpenCloseEvent>(&watcher);
            } else {
                spdlog::error("[StaleLODAfterTeleport] UI not available; loading screens not watched");
            }
            break;
        }
        case SKSE::MessagingInterface::kPostLoadGame:
        case SKSE::MessagingInterface::kNewGame: {
            // Re-seed, so the first trip of a session compares against where the player actually is
            s_departure = {};
            Waypoint here;
            if (sample(here)) {
                s_departure = here;
            }
            break;
        }
        default:
            break;
        }
    }
};

} // namespace

OPENFIXES_REGISTER(StaleLODAfterTeleport)
