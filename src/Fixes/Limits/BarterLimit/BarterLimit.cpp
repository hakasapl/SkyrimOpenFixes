// No gold or items received from a merchant holding more than 32,767 of them
//
// Ported from "Barter Limit Fix" by powerofthree and KernalsEgg (powerofthree),
// https://github.com/powerof3/BarterLimitFix (commit 72b8262), itself a port of KernalsEgg's original,
// https://github.com/KernalsEgg/SKSE64Plugins/tree/main/BarterLimitFix
// Original code: GNU GPL v3.0, Copyright (c) 2022-2026 KernalsEgg and powerofthree. Full text in LICENSE at the
// repository root.
// SPDX-License-Identifier: GPL-3.0-only

#include "PCH.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace {

/**
 * Item stacks keep their count in a 16-bit ExtraCount (at most 32,767). When a trade moves items, the game reads those
 * counts as 16-bit values and does its arithmetic on them in 16 bits, so with more than 32,767 gold (or of an item)
 * the count wraps and nothing is handed over; a loop elsewhere in the transfer also stops after its second pass. The
 * fix, unchanged from the original: the counts are read and subtracted as 32-bit values, that loop is no longer cut
 * short, and each count written back to an ExtraCount is capped at 32,767.
 */

// Five 16-bit count reads in 15895 (SE) / 16135 (AE, 1.7), each followed by a call to ExtraDataList::SetCount
constexpr std::size_t SITES = 5;

// Counts read as 16-bit values, and the 32-bit replacements (NOP-padded to the same length)
struct GetCountSite {
    std::uintptr_t se;
    std::uintptr_t ae;
    std::array<std::uint8_t, 6> original;
    std::array<std::uint8_t, 6> patched;
    std::size_t size;
};

constexpr std::array<GetCountSite, SITES> GET_COUNT {{
    {0x4F6,
     0x5D5,
     {0x41, 0x0F, 0xB7, 0x54, 0x24, 0x10},
     {0x41, 0x8B, 0x54, 0x24, 0x10, 0x90},
     6}, // movzx edx, word ptr [r12+10] -> mov edx, [r12+10]
    {0xA99, 0xB29, {0x66, 0x41, 0x2B, 0xD6}, {0x44, 0x29, 0xF2, 0x90}, 4}, // sub dx, r14w -> sub edx, r14d
    {0xB76, 0xC06, {0x41, 0x0F, 0xB7, 0x55, 0x10}, {0x41, 0x8B, 0x55, 0x10, 0x90}, 5}, // movzx edx, word ptr [r13+10]
                                                                                       // -> mov edx, [r13+10]
    {0xC76, 0xD06, {0x41, 0x0F, 0xB7, 0x57, 0x10}, {0x41, 0x8B, 0x57, 0x10, 0x90}, 5}, // movzx edx, word ptr [r15+10]
                                                                                       // -> mov edx, [r15+10]
    {0xE36, 0xE49, {0x66, 0x41, 0x2B, 0xD4}, {0x44, 0x29, 0xE2, 0x90}, 4}, // sub dx, r12w -> sub edx, r12d
}};

// Calls to ExtraDataList::SetCount right after each count above
constexpr std::array<std::uintptr_t, SITES> SET_COUNT_SE {0x4FF, 0xAA0, 0xB7E, 0xC7E, 0xE3D};
constexpr std::array<std::uintptr_t, SITES> SET_COUNT_AE {0x5DE, 0xB30, 0xC0E, 0xD0E, 0xE50};

// Stores the count, capped to what an ExtraCount holds (32,767); one per call site so each keeps its own original
template <std::size_t N> struct SetCount {
    static void thunk(RE::ExtraDataList* extraDataList,
                      std::int32_t count)
    {
        func(extraDataList,
             static_cast<std::int16_t>(
                 std::min(count, static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::max()))));
    }
    static inline REL::Relocation<decltype(thunk)> func;
};

class BarterLimit final : public OpenFixes::Fix {
public:
    BarterLimit()
        : Fix({
              .id = "BarterLimit",
              .section = "Limits",
              .description = "No gold or items received from a merchant holding more than 32,767 of them",
              .origin = "Barter Limit Fix by powerofthree and KernalsEgg",
              .runtimes = OpenFixes::Runtime::FLAT,
              .skipIfInstalled = {"BarterLimitFix.dll"},
          })
    {
    }

    bool install() override
    {
        const REL::Relocation<std::uintptr_t> transfer {RELOCATION_ID(15895, 16135)};
        // Exit from the loop after its second pass in 13661 (SE) / 13771 (AE, 1.7): jnz +0x45D (SE) / +0x45F (AE, 1.7)
        const REL::Relocation<std::uintptr_t> loop {RELOCATION_ID(13661, 13771), 0x124};
        const auto base = transfer.address();
        const auto& setCount = OpenFixes::select(SET_COUNT_SE, SET_COUNT_AE, SET_COUNT_AE);

        const std::uint8_t loopTarget = OpenFixes::select<std::uint8_t>(0x5D, 0x5F, 0x5F);
        if (!OpenFixes::Hooks::expect(loop.address(), {0x0F, 0x85, loopTarget, 0x04, 0x00, 0x00})) {
            return false;
        }
        for (std::size_t i = 0; i < SITES; ++i) {
            const auto& site = GET_COUNT[i];
            const auto offset = OpenFixes::select(site.se, site.ae, site.ae);
            if (!OpenFixes::Hooks::expect(base + offset, std::span(site.original.data(), site.size))
                || !OpenFixes::Hooks::expectCall(base + setCount[i])) {
                return false;
            }
        }

        // jnz +0: the exit is never taken
        OpenFixes::Hooks::patch(loop.address() + 2, {0x00, 0x00, 0x00, 0x00});
        for (const auto& site : GET_COUNT) {
            const auto offset = OpenFixes::select(site.se, site.ae, site.ae);
            OpenFixes::Hooks::patch(base + offset, std::span(site.patched.data(), site.size));
        }
        OpenFixes::Hooks::call<SetCount<0>>(base + setCount[0]);
        OpenFixes::Hooks::call<SetCount<1>>(base + setCount[1]);
        OpenFixes::Hooks::call<SetCount<2>>(base + setCount[2]);
        OpenFixes::Hooks::call<SetCount<3>>(base + setCount[3]);
        OpenFixes::Hooks::call<SetCount<4>>(base + setCount[4]);
        return true;
    }
};

} // namespace

OPENFIXES_REGISTER(BarterLimit)
