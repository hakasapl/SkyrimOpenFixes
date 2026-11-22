// Previous region's weather persists through a load door or fast travel (surface weather in Blackreach)
//
// Ported from "Baked Goods" by ItzIvy05, https://github.com/ItzIvy05/Baked-Goods
// (commit 7cfc632)
// Original code: GNU GPL v3.0, Copyright (c) 2026 Ivy. Full text in LICENSE at the repository root.
// SPDX-License-Identifier: GPL-3.0-only

#include "PCH.h"

namespace
{
    std::atomic<bool> teleported{ false };

    bool NeedsResync(RE::Sky* a_sky)
    {
        static REL::Relocation<bool (*)(RE::TESRegion*, RE::TESWeather*)> hasWeather{ RELOCATION_ID(16204, 16450) };  // TESRegion::HasWeather
        if (!a_sky->region || !a_sky->currentWeather || a_sky->overrideWeather || !a_sky->region->dataList || hasWeather(a_sky->region, a_sky->currentWeather)) {
            return false;
        }
        for (const auto& data : a_sky->region->dataList->regionDataList) {
            if (data && data->GetType() == RE::TESRegionData::Type::kWeather) {
                return data->IsLoaded();
            }
        }
        return false;
    }

    // doors, coc, fast travel and MoveTo all teleport the player through here
    struct TeleportSetParentCell
    {
        static void thunk(RE::TESObjectREFR* a_this, RE::TESObjectCELL* a_cell)
        {
            func(a_this, a_cell);
            teleported = true;
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    // the new region is committed before a running blend swallows the switch
    struct SkyUpdateWeather
    {
        static void thunk(RE::Sky* a_sky)
        {
            func(a_sky);
            if (teleported.exchange(false) && NeedsResync(a_sky)) {
                a_sky->region = nullptr;
                a_sky->flags.set(RE::Sky::Flags::kFastTravel);
            }
        }
        static inline REL::Relocation<decltype(thunk)> func;
    };

    class RegionWeather final : public OpenFixes::Fix
    {
    public:
        RegionWeather() : Fix({ .id = "RegionWeather", .section = "Graphics", .description = "Previous region's weather persists through a load door or fast travel", .origin = "Baked Goods by Ivy", .skipIfInstalled = { "BakedGoods.dll", "RegionWeatherFix.dll" } }) {}

        bool install() override
        {
            REL::Relocation<std::uintptr_t> teleport{ RELOCATION_ID(39657, 40744), REL::VariantOffset(0x134, 0x131, 0x0) };  // PlayerCharacter teleport
            OpenFixes::Hooks::call<TeleportSetParentCell>(teleport.address());
            REL::Relocation<std::uintptr_t> skyUpdate{ RELOCATION_ID(25682, 26229), REL::VariantOffset(0x29C, 0x3E6, 0x0) };  // Sky::Update
            OpenFixes::Hooks::call<SkyUpdateWeather>(skyUpdate.address());
            return true;
        }
    };
}

OPENFIXES_REGISTER(RegionWeather)
