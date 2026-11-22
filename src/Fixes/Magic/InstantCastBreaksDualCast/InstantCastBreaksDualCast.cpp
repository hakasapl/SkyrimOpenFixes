// Scripted or instant spells cast while dual casting break dual-cast effects and perks
//
// Ported from "Dual Casting Fix" by powerofthree (powerofthree), https://github.com/powerof3/DualCastingFix
// (commit a368327)
// Original code: GNU GPL v3.0, Copyright (c) 2023-2026 powerofthree. Full text in LICENSE at the repository root.
// SPDX-License-Identifier: GPL-3.0-only

#include "PCH.h"

#include <cstdint>

namespace {

/**
 * When one of an actor's magic casters finishes a cast, ActorMagicCaster::FinishCastImpl clears that source's casting
 * state and also turns off the actor's dual casting, whatever the source. So a spell cast by script (Cast, RemoteCast,
 * AddSpell) or by a perk ability while the actor is dual casting turns the dual cast off, and the dual-cast effects
 * and perks that depend on it (e.g. Impact) don't apply. Survival Mode's scripted spells do this in a vanilla game.
 * The fix, unchanged from the original, replaces the function with the same code that only turns off dual casting
 * when the left-hand caster finishes. Scripted and instant spells can't be dual cast in the first place.
 */

constexpr std::size_t FINISH_CAST_IMPL = 0x07;

bool (*s_setWantCasting)(RE::Actor* actor,
                         RE::MagicSystem::CastingSource source,
                         bool enable) = nullptr;
void (*s_setWantDualCasting)(RE::Actor* actor,
                             bool enable) = nullptr;

// Replaces the game's function entirely; it differs only in the left-hand check
struct FinishCastImpl {
    static void thunk(RE::ActorMagicCaster* caster)
    {
        if (auto* actor = caster->actor; actor != nullptr) {
            s_setWantCasting(actor, caster->castingSource, false);
            if (caster->castingSource == RE::MagicSystem::CastingSource::kLeftHand) {
                s_setWantDualCasting(actor, false);
                caster->flags.reset(RE::ActorMagicCaster::Flags::kDualCasting);
            }
        }
        caster->flags.reset(RE::ActorMagicCaster::Flags::kSkipCheckCast);
        caster->costCharged = 0.0F;
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

class InstantCastBreaksDualCast final : public OpenFixes::Fix {
public:
    InstantCastBreaksDualCast()
        : Fix({
              .id = "InstantCastBreaksDualCast",
              .section = "Magic",
              .description = "Scripted or instant spells cast while dual casting break dual-cast effects and perks",
              .origin = "Dual Casting Fix by powerofthree",
              .runtimes = OpenFixes::Runtime::ALL,
              .skipIfInstalled = {"po3_DualCastingFix.dll"},
          })
    {
    }

    bool install() override
    {
        // The game's ActorMagicCaster::FinishCastImpl, checked so another mod's replacement isn't overwritten
        const REL::Relocation<std::uintptr_t> finishCastImpl {REL::VariantID(33360, 34141, 0x545750)};
        if (!OpenFixes::Hooks::expectVfunc<RE::ActorMagicCaster>(FINISH_CAST_IMPL, finishCastImpl.address())) {
            return false;
        }

        // Actor::SetWantCasting and Actor::SetWantDualCasting, the two functions FinishCastImpl calls
        s_setWantCasting = REL::Relocation<decltype(s_setWantCasting)> {RELOCATION_ID(36925, 37950)}.get();
        s_setWantDualCasting = REL::Relocation<decltype(s_setWantDualCasting)> {RELOCATION_ID(37814, 38763)}.get();

        OpenFixes::Hooks::vfunc<RE::ActorMagicCaster, FinishCastImpl>(FINISH_CAST_IMPL);
        return true;
    }
};

} // namespace

OPENFIXES_REGISTER(InstantCastBreaksDualCast)
