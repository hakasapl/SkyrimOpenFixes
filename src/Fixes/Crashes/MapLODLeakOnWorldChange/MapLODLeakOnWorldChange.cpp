// The previous worldspace's world-map LOD stays loaded after changing worldspace
//
// Ported from "LOD Refresh Bug Fix" (SkyrimLODFixes) by Bottle (InTheBottle),
// https://github.com/InTheBottle/SkyrimLODFixes (commit e76efd1)
// Original code: GNU GPL v3.0, Copyright (c) 2026 InTheBottle. Full text in LICENSE at the repository root.
// SPDX-License-Identifier: GPL-3.0-only

#include "PCH.h"

#include <cstdint>

namespace {

/**
 * When the player enters a worldspace drawn by another terrain manager, BGSTerrainManager::Update tears the old manager
 * down with Detach(old, clearLoaded = 1, releaseAll = 0). The third argument widens the set of LOD handles released on
 * each node from 0x2F to 0x3F, adding the pair that holds the world map's own LOD; with 0 those stay loaded although
 * the manager is no longer used (some of the engine's other teardowns pass 1). The fix makes the switch pass 1 by
 * turning its `xor r8d, r8d` into `mov r8b, 1` (Detach only reads r8b), so the old manager is released completely.
 */

class MapLODLeakOnWorldChange final : public OpenFixes::Fix {
public:
    MapLODLeakOnWorldChange()
        : Fix({
              .id = "MapLODLeakOnWorldChange",
              .section = "Crashes",
              .description = "Previous worldspace's world-map LOD stays loaded after changing worldspace (memory leak)",
              .details = "When the game switches to another worldspace's terrain LOD manager, it now releases the old "
                         "manager's world-map LOD along with the rest of its LOD (the engine's own full teardown) "
                         "instead of keeping it in memory.",
              .origin = "LOD Refresh Bug Fix by Bottle",
              .runtimes = OpenFixes::Runtime::ALL,
              .skipIfInstalled = {"SkyrimLODFixes.dll"},
          })
    {
    }

    bool install() override
    {
        // BGSTerrainManager::Update 31017 (SE) / 31802 (AE, 1.7) / 0x4C2A30 (VR): xor r8d, r8d; mov dl, 1;
        // mov rcx, rsi; call Detach 31026 / 31812 / 0x4C3260. The VR Address Library lacks both IDs, so VR uses the
        // offsets of the same code in SkyrimVR.exe 1.4.15.
        const REL::Relocation<std::uintptr_t> update {REL::VariantID(31017, 31802, 0x4C2A30)};
        const REL::Relocation<std::uintptr_t> detach {REL::VariantID(31026, 31812, 0x4C3260)};
        const auto site = update.address() + OpenFixes::select<std::uintptr_t>(0x1A3, 0x1DA, 0x1DA, 0x1A3);
        if (!OpenFixes::Hooks::expect(site, {0x45, 0x33, 0xC0, 0xB2, 0x01, 0x48, 0x8B, 0xCE})
            || !OpenFixes::Hooks::expectCall(site + 8, detach.address())) {
            return false;
        }

        OpenFixes::Hooks::patch(site, {0x41, 0xB0, 0x01}); // mov r8b, 1
        return true;
    }
};

} // namespace

OPENFIXES_REGISTER(MapLODLeakOnWorldChange)
