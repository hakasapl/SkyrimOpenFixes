// Crash when the light-queue pass evaluates a light that was already destroyed, or one without an NiLight
//
// Ported from "Shadow Scene Node Crash Fix" by Bottle (InTheBottle), https://github.com/InTheBottle/SCN-Crash-fix
// (commit b54378f)
// Original code: GNU GPL v3.0, Copyright (c) 2026 InTheBottle. Full text in LICENSE at the repository root.
// SPDX-License-Identifier: GPL-3.0-only

#include "PCH.h"

#include "OpenFixes/Util.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <intrin.h>
#include <span>

namespace {

/**
 * ShadowSceneNode's light-queue pass (99701 / 106335) evaluates every queued, dirty and active light against the
 * portal graph (99708 / 106342). That evaluation reads the light's NiLight and writes the light's portal graph and
 * room/portal lists, so a list entry left pointing at a destroyed light turns into writes to freed memory, and a
 * light without an NiLight is a null dereference. Before each evaluation the fix checks the light: a destroyed one
 * (not one of the five light virtual tables, or no references left) is taken out of this scene node's lists and
 * skipped, and one without an NiLight is skipped.
 *
 * The original set every list entry of a destroyed light to nullptr. The queues and the shadow accumulation slots
 * take that, but the active lists don't: the pass's last loop over the active lights never moves past an empty entry
 * (1.5.97 99746+0x49, 1.6.1170 and 1.7.99 106335+0x67C jump back to the loop test without advancing), so the next
 * pass would hang. The fix removes the light from the active lists instead, closing the gap, except when it is called
 * from that loop itself, which walks the active lights with a pointer that the removal could move past the end.
 *
 * 1.6 and 1.7 inline the add and active-light steps into the pass, so it calls the evaluation three times; 1.5.97
 * keeps them in 99694 and 99746 (each called only by the pass), so the same three steps are hooked there.
 *
 * VR 1.4.15 has the same code as 1.5.97 at the same call offsets, including the active-light loop that doesn't move
 * past an empty entry; only the scene node's members sit 0x28 bytes further on (CommonLib's GetRuntimeData() has
 * that). Its Address Library lacks 99701, 99694 and 99746, so they are given by their offsets in SkyrimVR.exe. The
 * original mod didn't hook these calls on VR.
 */

struct CallSite {
    std::uint64_t function; // address-library ID; on VR, the function's offset in SkyrimVR.exe
    std::uintptr_t offset;
    bool original;     // hooked by the original mod; the others are the same steps where SE doesn't inline them
    bool activeLoop;   // inside the pass's loop over the active lights
};

// Dirty-queue step in the pass, the add step (two calls) and the active-light step
constexpr std::array<CallSite, 4> SE_SITES { {
    { 99701, 0xDC, true, false },
    { 99694, 0xD8, false, false },
    { 99694, 0x135, false, false },
    { 99746, 0xB3, false, true },
} };

// Add step, dirty-queue step and active-light step, all inside the pass
constexpr std::array<CallSite, 3> AE_SITES { {
    { 106335, 0x16D, true, false },
    { 106335, 0x1CA, true, false },
    { 106335, 0x6E2, true, true },
} };

// The 1.5.97 steps in SkyrimVR.exe: the pass (99701), the add step (99694) and the active-light step (99746)
constexpr std::array<CallSite, 4> VR_SITES { {
    { 0x12F77E0, 0xDC, true, false },
    { 0x12F70E0, 0xD8, false, false },
    { 0x12F70E0, 0x135, false, false },
    { 0x12FC210, 0xB3, false, true },
} };

constexpr std::size_t CALL_SIZE = 5;

// Return address of the evaluation call in the active-light loop
std::uintptr_t s_activeLoopReturn = 0;

constexpr std::uint64_t LOG_CAP = 16;
std::atomic<std::uint64_t> s_logged { 0 };

/**
 * Wraps the light evaluation called from the light-queue pass, which holds the scene node's lightQueueLock
 */
struct EvaluateLight {
    static void thunk(RE::ShadowSceneNode* node, RE::BSLight* light)
    {
        if (light == nullptr) {
            return;
        }

        if (OpenFixes::Util::isDeadLight(light)) {
            std::size_t cleared = 0;
            if (const OpenFixes::Util::SpinLockTry lock(node->GetRuntimeData().lightQueueLock); lock) {
                const auto isLight = [light](const void* entry) { return entry == light; };
                cleared = OpenFixes::Util::clearSceneLightEntries(node, isLight);
                if (reinterpret_cast<std::uintptr_t>(_ReturnAddress()) != s_activeLoopReturn) {
                    cleared += OpenFixes::Util::eraseActiveLightEntries(node, isLight);
                }
            }
            if (s_logged.fetch_add(1) < LOG_CAP) {
                spdlog::debug("[DeadLightInLightQueue] skipped destroyed light {:#x}, took out {} list entries",
                    reinterpret_cast<std::uintptr_t>(light),
                    cleared);
            }
            return;
        }

        if (light->light == nullptr) {
            if (s_logged.fetch_add(1) < LOG_CAP) {
                spdlog::debug("[DeadLightInLightQueue] skipped light {:#x} without an NiLight",
                    reinterpret_cast<std::uintptr_t>(light));
            }
            return;
        }

        func(node, light);
    }

    static inline REL::Relocation<decltype(thunk)> func;
};

class DeadLightInLightQueue final : public OpenFixes::Fix {
public:
    DeadLightInLightQueue()
        : Fix({
              .id = "DeadLightInLightQueue",
              .section = "Crashes",
              .description = "Crash when the light-queue pass evaluates a destroyed light or one without an NiLight",
              .details = "Before the shadow scene node evaluates a queued, dirty or active light, skips it if it was "
                         "already destroyed (no light virtual table or no references left; it is taken out of that "
                         "scene node's lists without releasing it) or has no NiLight. Unlike the original it removes "
                         "such a light from the active lists instead of leaving an empty entry there, which would hang "
                         "the next light pass; a destroyed light whose memory already holds a new, referenced light "
                         "can't be told apart, as in vanilla.",
              .origin = "Shadow Scene Node Crash Fix by Bottle",
              .runtimes = OpenFixes::Runtime::ALL,
              .skipIfInstalled = { "ShadowSceneNodeCrashFix.dll" },
          })
    {
    }

    bool install() override
    {
        // Light evaluation against the portal graph: 99708 (SE, and VR through its Address Library) / 106342 (AE,
        // 1.7). SSE Engine Fixes patches inside it (+0x16); this fix only redirects the calls to it, so both apply in
        // either load order.
        const REL::Relocation<std::uintptr_t> evaluate { RELOCATION_ID(99708, 106342) };
        const bool vr = OpenFixes::currentRuntime() == OpenFixes::Runtime::VR;
        const auto sites = OpenFixes::select<std::span<const CallSite>>(SE_SITES, AE_SITES, AE_SITES, VR_SITES);

        std::array<std::uintptr_t, 4> addresses {};
        std::size_t count = 0;
        std::uintptr_t activeLoopReturn = 0;
        for (const auto& site : sites) {
            const auto function = vr ? REL::Offset(site.function).address() : REL::ID(site.function).address();
            const auto address = function + site.offset;
            if (OpenFixes::Hooks::expectCall(address, evaluate.address())) {
                addresses[count++] = address;
                if (site.activeLoop) {
                    activeLoopReturn = address + CALL_SIZE;
                }
            } else if (site.original) {
                return false;
            } else if (vr) {
                spdlog::warn("[DeadLightInLightQueue] call at SkyrimVR.exe+{:X} +{:X} left alone",
                    site.function,
                    site.offset);
            } else {
                spdlog::warn("[DeadLightInLightQueue] call in {} +{:X} left alone", site.function, site.offset);
            }
        }

        s_activeLoopReturn = activeLoopReturn;
        for (std::size_t i = 0; i < count; ++i) {
            OpenFixes::Hooks::call<EvaluateLight>(addresses[i]);
        }
        return true;
    }
};

} // namespace

OPENFIXES_REGISTER(DeadLightInLightQueue)
