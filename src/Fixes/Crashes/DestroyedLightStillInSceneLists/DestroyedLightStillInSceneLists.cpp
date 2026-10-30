// Crash when a destroyed light, room or portal is still listed by the shadow scene nodes or by room and portal light
// links
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
#include <mutex>
#include <optional>
#include <span>
#include <unordered_set>

namespace {

/**
 * The four ShadowSceneNodes keep lights in their add, remove and dirty queues, active and active shadow lists and the
 * shadow light accumulation list; interior rooms and portal nodes keep the lights that shine through them, and each
 * light keeps its rooms, portals and portal nodes. None of the destructors of BSLight (and the four shadow-light
 * classes), BSMultiBoundRoom, BSPortalSharedNode or BSPortal take themselves out of those lists (and the sky's sun and
 * cloud lights are deleted outright rather than released), so a list that still points at one leads the next light
 * pass or portal walk into freed memory.
 *
 * The fix hooks those destructors and removes the object from every list before it is freed, without releasing the
 * entries (their reference is no longer real). It never waits for a scene node's lightQueueLock, since a destructor
 * can run on a thread the lock holder is waiting for: if the lock stays busy for a few hundred spins, the cleanup is
 * queued and run, under the lock, at the start of that scene node's next light pass. The original drained the queue
 * before 2 of the light pass's 9 callers; here it runs before every light pass. Lights are tracked from SetLight
 * (called on every new light before it is queued) to their destructor, so links are only edited on lights that still
 * exist, and a queued cleanup is dropped when a new light takes the same address.
 *
 * Queue entries and shadow accumulation slots are set to nullptr as in the original. The original did the same to
 * the active and active shadow light lists, but the light pass's last loop over the active lights never moves past an
 * empty entry and hangs, so here those entries are removed instead (see Util::eraseActiveLightEntries). A destructor
 * can't do that safely: the game releases an active entry before closing its gap, and some game loops walk the active
 * lists without the lock, so a light still found there is queued and removed at the start of the next light pass,
 * where nothing up the stack is walking them (the pass itself rewrites them). Usually the entry is gone by then.
 *
 * VR 1.4.15 has the same code as 1.5.97, with the scene node's members and the room and portal node members 0x28
 * bytes further on (NiAVObject is bigger there); lights, portals and portal graphs have the same layout. Its Address
 * Library lacks most of these IDs, so the functions are given by their offsets in SkyrimVR.exe. It calls the light
 * pass from 8 places (the same ones as 1.5.97 but 39657, which doesn't call it there); the original mod hooked only
 * the destructors on VR and dropped a cleanup it could not run at once.
 */

constexpr std::size_t DESTRUCTOR = 0;
constexpr std::size_t SET_LIGHT = 2;
constexpr std::size_t SCENE_NODES = 4;

constexpr std::uint64_t LOG_CAP = 16;
std::atomic<std::uint64_t> s_logged { 0 };

std::uintptr_t s_roomVtable = 0;
std::uintptr_t s_sharedNodeVtable = 0;
std::uintptr_t s_portalVtable = 0;

std::uintptr_t vtableOf(const void* object)
{
    return *static_cast<const std::uintptr_t*>(object);
}

// CommonLib declares the members of BSMultiBoundRoom and BSPortalSharedNode right after their NiNode base, but a build
// for every runtime leaves NiNode's (and BSMultiBoundNode's) own members out of those bases, so the declared members
// land 0x18 to 0x28 bytes before the real ones on every runtime. These are the offsets the game's destructors use:
// 1.5.97 74159 / 74716, 1.6.1170 and 1.7.99 75898 / 76456 the same, VR 0x28 further on (NiAVObject is bigger there).
// BSPortal (not an NiAVObject) has the same layout on every runtime, as CommonLib declares it.
using PortalList = decltype(RE::BSMultiBoundRoom::portalList);
using LightList = decltype(RE::BSMultiBoundRoom::lights);

PortalList& roomPortals(RE::BSMultiBoundRoom* room)
{
    return REL::RelocateMember<PortalList>(room, OpenFixes::select<std::ptrdiff_t>(0x138, 0x138, 0x138, 0x160));
}

LightList& roomLights(RE::BSMultiBoundRoom* room)
{
    return REL::RelocateMember<LightList>(room, OpenFixes::select<std::ptrdiff_t>(0x180, 0x180, 0x180, 0x1A8));
}

LightList& sharedNodeLights(RE::BSPortalSharedNode* node)
{
    return REL::RelocateMember<LightList>(node, OpenFixes::select<std::ptrdiff_t>(0x128, 0x128, 0x128, 0x150));
}

RE::BSPortal* sharedNodePortal(RE::BSPortalSharedNode* node)
{
    return REL::RelocateMember<RE::BSPortal*>(node, OpenFixes::select<std::ptrdiff_t>(0x140, 0x140, 0x140, 0x168));
}

/**
 * Lights between SetLight and their destructor
 */
struct LiveLights {
    std::mutex lock;
    std::unordered_set<RE::BSLight*> lights;
};
LiveLights s_live;

/**
 * Cleanups that could not take a scene node's lock, by scene node (room and portal links belong to node 0, the world)
 */
struct Pending {
    using Set = std::unordered_set<const void*>;

    std::mutex lock;
    std::array<Set, SCENE_NODES> lights;
    Set lightLinks;
    Set rooms;
    Set portals;
    Set sharedNodes;
    std::atomic<std::uint32_t> count { 0 };

    void recount()
    {
        std::size_t total = lightLinks.size() + rooms.size() + portals.size() + sharedNodes.size();
        for (const auto& set : lights) {
            total += set.size();
        }
        count.store(static_cast<std::uint32_t>(total), std::memory_order_release);
    }

    void deferLight(std::size_t index, const void* light, bool withLinks)
    {
        const std::scoped_lock guard(lock);
        lights[index].insert(light);
        if (index == 0 && withLinks) {
            lightLinks.insert(light);
        }
        recount();
    }

    void deferRoom(const void* room)
    {
        const std::scoped_lock guard(lock);
        rooms.insert(room);
        recount();
    }

    void deferPortal(const void* portal)
    {
        const std::scoped_lock guard(lock);
        portals.insert(portal);
        recount();
    }

    void deferSharedNode(const void* node, const void* portal)
    {
        const std::scoped_lock guard(lock);
        sharedNodes.insert(node);
        if (portal != nullptr) {
            portals.insert(portal);
        }
        recount();
    }

    // A new light now lives at this address, so a queued cleanup would hit it instead of the destroyed one
    void forgetLight(const void* light)
    {
        if (count.load(std::memory_order_acquire) == 0) {
            return;
        }
        const std::scoped_lock guard(lock);
        for (auto& set : lights) {
            set.erase(light);
        }
        lightLinks.erase(light);
        recount();
    }
};
Pending s_pending;

RE::ShadowSceneNode* sceneNode(std::size_t index)
{
    return RE::BSShaderManager::State::GetSingleton().shadowSceneNode[index];
}

std::optional<std::size_t> indexOf(const RE::ShadowSceneNode* node)
{
    for (std::size_t i = 0; i < SCENE_NODES; ++i) {
        if (sceneNode(i) == node) {
            return i;
        }
    }
    return std::nullopt;
}

template <class T, class Pred>
std::size_t eraseIf(RE::BSTArray<T*>& list, const Pred& matches)
{
    std::size_t erased = 0;
    for (auto it = list.begin(); it != list.end();) {
        if (*it != nullptr && matches(static_cast<const void*>(*it))) {
            it = list.erase(it);
            ++erased;
        } else {
            ++it;
        }
    }
    return erased;
}

/**
 * Removes matching lights from the light lists of the world's rooms and of their portals' shared nodes; call with the
 * world scene node's lightQueueLock held
 */
template <class Pred>
std::size_t eraseLightLinks(RE::ShadowSceneNode* world, const Pred& matches)
{
    auto* graph = world->GetRuntimeData().portalGraph;
    if (graph == nullptr) {
        return 0;
    }
    std::size_t erased = 0;
    for (auto& roomPointer : graph->rooms) {
        auto* room = roomPointer.get();
        if (room == nullptr) {
            continue;
        }
        erased += eraseIf(roomLights(room), matches);
        for (auto* portal : roomPortals(room)) {
            if (portal == nullptr) {
                continue;
            }
            if (auto* node = portal->portalSharedNode.get(); node != nullptr) {
                erased += eraseIf(sharedNodeLights(node), matches);
            }
        }
    }
    return erased;
}

void onLightDestroyed(RE::BSLight* light)
{
    {
        const std::scoped_lock guard(s_live.lock);
        s_live.lights.erase(light);
    }

    const auto isLight = [light](const void* entry) { return entry == light; };
    for (std::size_t i = 0; i < SCENE_NODES; ++i) {
        auto* node = sceneNode(i);
        if (node == nullptr) {
            continue;
        }
        if (const OpenFixes::Util::SpinLockTry lock(node->GetRuntimeData().lightQueueLock); lock) {
            OpenFixes::Util::clearSceneLightEntries(node, isLight);
            if (OpenFixes::Util::countActiveLightEntries(node, isLight) != 0) {
                // Usually the entry the game is releasing right now; whatever is still there goes before the next pass
                s_pending.deferLight(i, light, false);
            }
            if (i == 0) {
                eraseLightLinks(node, isLight);
            }
        } else {
            s_pending.deferLight(i, light, true);
        }
    }
}

// Neither the room's unload nor its destructor clears the room pointers its portals hold, so a portal that outlives
// the room would lead the room-visibility walk into freed memory. Null is the game's own "leads outside" value (a new
// portal starts with both rooms null).
void unlinkPortalsFrom(RE::BSMultiBoundRoom* room)
{
    for (auto* portal : roomPortals(room)) {
        if (portal == nullptr || vtableOf(portal) != s_portalVtable) {
            continue;
        }
        if (portal->entranceRoom == room) {
            portal->entranceRoom = nullptr;
        }
        if (portal->exitRoom == room) {
            portal->exitRoom = nullptr;
        }
    }
}

/**
 * Runs scan under the world scene node's lightQueueLock, or queues the cleanup with defer if the lock stays busy
 */
template <class Scan, class Defer>
void withWorldLock(const Scan& scan, const Defer& defer)
{
    auto* world = sceneNode(0);
    if (world == nullptr) {
        scan();
        return;
    }
    if (const OpenFixes::Util::SpinLockTry lock(world->GetRuntimeData().lightQueueLock); lock) {
        scan();
    } else {
        defer();
    }
}

void onRoomDestroyed(RE::BSMultiBoundRoom* room)
{
    unlinkPortalsFrom(room);
    withWorldLock(
        [room]() {
            const auto isRoom = [room](const void* entry) { return entry == room; };
            const std::scoped_lock guard(s_live.lock);
            for (auto* light : roomLights(room)) {
                if (light != nullptr && s_live.lights.contains(light)) {
                    eraseIf(light->rooms, isRoom);
                }
            }
        },
        [room]() { s_pending.deferRoom(room); });
}

void onSharedNodeDestroyed(RE::BSPortalSharedNode* node)
{
    auto* portal = sharedNodePortal(node);
    withWorldLock(
        [node, portal]() {
            const auto isNode = [node](const void* entry) { return entry == node; };
            const auto isPortal = [portal](const void* entry) { return entry == portal; };
            const std::scoped_lock guard(s_live.lock);
            for (auto* light : sharedNodeLights(node)) {
                if (light == nullptr || !s_live.lights.contains(light)) {
                    continue;
                }
                eraseIf(light->portalSharedNodes, isNode);
                if (portal != nullptr) {
                    eraseIf(light->portals, isPortal);
                }
            }
        },
        [node, portal]() { s_pending.deferSharedNode(node, portal); });
}

void onPortalDestroyed(RE::BSPortal* portal)
{
    auto* node = portal->portalSharedNode.get();
    if (node == nullptr) {
        return;
    }
    withWorldLock(
        [portal, node]() {
            const auto isPortal = [portal](const void* entry) { return entry == portal; };
            const std::scoped_lock guard(s_live.lock);
            for (auto* light : sharedNodeLights(node)) {
                if (light != nullptr && s_live.lights.contains(light)) {
                    eraseIf(light->portals, isPortal);
                }
            }
        },
        [portal]() { s_pending.deferPortal(portal); });
}

/**
 * Runs the cleanups queued for this scene node, under its lightQueueLock, before its light pass
 */
void drain(RE::ShadowSceneNode* node)
{
    const auto index = indexOf(node);
    if (!index) {
        return;
    }

    Pending::Set lights;
    Pending::Set links;
    Pending::Set rooms;
    Pending::Set portals;
    Pending::Set nodes;
    {
        const std::scoped_lock guard(s_pending.lock);
        lights.swap(s_pending.lights[*index]);
        if (*index == 0) {
            links.swap(s_pending.lightLinks);
            rooms.swap(s_pending.rooms);
            portals.swap(s_pending.portals);
            nodes.swap(s_pending.sharedNodes);
        }
        s_pending.recount();
    }
    if (lights.empty() && links.empty() && rooms.empty() && portals.empty() && nodes.empty()) {
        return;
    }

    // A new room, portal or portal node at the same address is alive and keeps its links
    std::erase_if(rooms, [](const void* room) { return vtableOf(room) == s_roomVtable; });
    std::erase_if(portals, [](const void* portal) { return vtableOf(portal) == s_portalVtable; });
    std::erase_if(nodes, [](const void* shared) { return vtableOf(shared) == s_sharedNodeVtable; });

    const auto deadIn = [](const Pending::Set& set) {
        return [&set](const void* entry) {
            return set.contains(entry) && OpenFixes::Util::isDeadLight(static_cast<const RE::BSLight*>(entry));
        };
    };
    const auto in = [](const Pending::Set& set) {
        return [&set](const void* entry) { return set.contains(entry); };
    };

    std::size_t cleared = 0;
    {
        // Called right before the light pass, so no game code is walking these lists further up the stack
        const RE::BSSpinLockGuard lock(node->GetRuntimeData().lightQueueLock);
        if (!lights.empty()) {
            cleared += OpenFixes::Util::clearSceneLightEntries(node, deadIn(lights));
            cleared += OpenFixes::Util::eraseActiveLightEntries(node, deadIn(lights));
        }
        if (!links.empty()) {
            cleared += eraseLightLinks(node, deadIn(links));
        }
        if (!rooms.empty() || !portals.empty() || !nodes.empty()) {
            const std::scoped_lock guard(s_live.lock);
            for (auto* light : s_live.lights) {
                if (!rooms.empty()) {
                    cleared += eraseIf(light->rooms, in(rooms));
                }
                if (!portals.empty()) {
                    cleared += eraseIf(light->portals, in(portals));
                }
                if (!nodes.empty()) {
                    cleared += eraseIf(light->portalSharedNodes, in(nodes));
                }
            }
        }
    }

    if (cleared != 0 && s_logged.fetch_add(1) < LOG_CAP) {
        spdlog::debug("[DestroyedLightStillInSceneLists] scene node {}: removed {} entries still pointing at destroyed "
                      "lights, rooms or portals",
            *index,
            cleared);
    }
}

template <std::size_t I>
struct LightDestructor {
    static RE::BSLight* thunk(RE::BSLight* light, std::uint32_t flags)
    {
        onLightDestroyed(light);
        return func(light, flags);
    }

    static inline REL::Relocation<decltype(thunk)> func;
};

template <std::size_t I>
struct LightSetLight {
    static bool thunk(RE::BSLight* light, RE::NiLight* niLight)
    {
        s_pending.forgetLight(light);
        {
            const std::scoped_lock guard(s_live.lock);
            s_live.lights.insert(light);
        }
        return func(light, niLight);
    }

    static inline REL::Relocation<decltype(thunk)> func;
};

struct RoomDestructor {
    static RE::BSMultiBoundRoom* thunk(RE::BSMultiBoundRoom* room, std::uint32_t flags)
    {
        onRoomDestroyed(room);
        return func(room, flags);
    }

    static inline REL::Relocation<decltype(thunk)> func;
};

struct SharedNodeDestructor {
    static RE::BSPortalSharedNode* thunk(RE::BSPortalSharedNode* node, std::uint32_t flags)
    {
        onSharedNodeDestroyed(node);
        return func(node, flags);
    }

    static inline REL::Relocation<decltype(thunk)> func;
};

struct PortalDestructor {
    static RE::BSPortal* thunk(RE::BSPortal* portal, std::uint32_t flags)
    {
        onPortalDestroyed(portal);
        return func(portal, flags);
    }

    static inline REL::Relocation<decltype(thunk)> func;
};

/**
 * Wraps every call to ShadowSceneNode's light-queue pass (99701 / 106335) to run the queued cleanups first
 */
struct ProcessLightQueues {
    static std::uintptr_t thunk(RE::ShadowSceneNode* node, std::uintptr_t arg)
    {
        if (node != nullptr && s_pending.count.load(std::memory_order_acquire) != 0) {
            drain(node);
        }
        return func(node, arg);
    }

    static inline REL::Relocation<decltype(thunk)> func;
};

struct CallSite {
    std::uint64_t function; // address-library ID; on VR, the function's offset in SkyrimVR.exe
    std::uintptr_t offset;
    bool original; // drained by the original mod (once-per-frame wrapper and SetPortalGraph)
};

// Every call to the light-queue pass in each executable
constexpr std::array<CallSite, 9> SE_PASS_CALLS { {
    { 13140, 0x376, false },
    { 13171, 0x285, false },
    { 15514, 0x19E, false },
    { 19000, 0x2CA, false },
    { 34870, 0x104, false },
    { 39657, 0x3DC, false },
    { 51855, 0x215, false },
    { 99703, 0x2B, true },
    { 99741, 0x530, true },
} };

constexpr std::array<CallSite, 9> AE_PASS_CALLS { {
    { 13280, 0x545, false },
    { 13316, 0x38A, false },
    { 15691, 0x265, false },
    { 19411, 0x1B3, false },
    { 35780, 0x10E, false },
    { 40744, 0x3D2, false },
    { 52727, 0x20E, false },
    { 106337, 0x2B, true },
    { 106385, 0x517, true },
} };

// The same calls in SkyrimVR.exe, as 1.5.97's 13140, 13171, 15514, 19000, 34870, 51855, 99703 and 99741
constexpr std::array<CallSite, 8> VR_PASS_CALLS { {
    { 0x161E70, 0x376, false },
    { 0x1649D0, 0x285, false },
    { 0x1D9810, 0x19E, false },
    { 0x28EA60, 0x2CA, false },
    { 0x5920B0, 0x104, false },
    { 0x8FF930, 0x3DB, false },
    { 0x12F81D0, 0x2B, true },
    { 0x12FB9E0, 0x530, true },
} };

class DestroyedLightStillInSceneLists final : public OpenFixes::Fix {
public:
    DestroyedLightStillInSceneLists()
        : Fix({
              .id = "DestroyedLightStillInSceneLists",
              .section = "Crashes",
              .description = "Crash when a destroyed light, room or portal is still in the scene's light lists",
              .details = "When a light, interior room, portal or portal node is destroyed, removes it from the "
                         "shadow scene nodes' light lists and from the room and portal light links, so later light "
                         "passes and portal walks don't use freed memory. It never waits for the scene's light lock: "
                         "if that is busy, and always for the active-light lists (which the original emptied in place, "
                         "hanging the next light pass), the cleanup runs at the start of the next light pass, so until "
                         "then other code can still see the stale entry as in vanilla.",
              .origin = "Shadow Scene Node Crash Fix by Bottle",
              .runtimes = OpenFixes::Runtime::ALL,
              .skipIfInstalled = { "ShadowSceneNodeCrashFix.dll" },
          })
    {
    }

    bool install() override
    {
        // Scalar deleting destructors (slot 0) and SetLight (slot 2) of the five light classes. The VR Address Library
        // has only 101302 of these IDs, so the others are given by their offsets in SkyrimVR.exe.
        const auto& vtables = OpenFixes::Util::lightVtables();
        const std::array<REL::Relocation<std::uintptr_t>, 5> destructors {
            REL::Relocation<std::uintptr_t> { REL::VariantID(101319, 108312, 0x1364C60) }, // BSLight
            REL::Relocation<std::uintptr_t> { REL::VariantID(100823, 107611, 0x134CEB0) }, // BSShadowLight
            REL::Relocation<std::uintptr_t> { REL::VariantID(101504, 108501, 0x135A520) }, // BSShadowDirectionalLight
            REL::Relocation<std::uintptr_t> { REL::VariantID(101616, 108619, 0x1370370) }, // BSShadowFrustumLight
            REL::Relocation<std::uintptr_t> { REL::VariantID(101626, 108631, 0x13714B0) }, // BSShadowParabolicLight
        };
        // BSLight::SetLight, shared by all but the frustum light, whose override calls it
        const REL::Relocation<std::uintptr_t> setLight { RELOCATION_ID(101302, 108289) };
        const REL::Relocation<std::uintptr_t> frustumSetLight { REL::VariantID(101614, 108617, 0x136FF70) };
        constexpr std::size_t FRUSTUM = 3;
        for (std::size_t i = 0; i < vtables.size(); ++i) {
            const auto expected = i == FRUSTUM ? frustumSetLight.address() : setLight.address();
            if (!OpenFixes::Hooks::expectVfunc(vtables[i], DESTRUCTOR, destructors[i].address())
                || !OpenFixes::Hooks::expectVfunc(vtables[i], SET_LIGHT, expected)) {
                return false;
            }
        }

        const REL::Relocation<std::uintptr_t> roomVtable { RE::VTABLE_BSMultiBoundRoom[0] };
        const REL::Relocation<std::uintptr_t> sharedNodeVtable { RE::VTABLE_BSPortalSharedNode[0] };
        const REL::Relocation<std::uintptr_t> portalVtable { RE::VTABLE_BSPortal[0] };
        const REL::Relocation<std::uintptr_t> roomDestructor { REL::VariantID(74159, 75898, 0xD7C6B0) };
        const REL::Relocation<std::uintptr_t> sharedNodeDestructor { REL::VariantID(74716, 76456, 0xD96130) };
        const REL::Relocation<std::uintptr_t> portalDestructor { REL::VariantID(15405, 15555, 0x1D2090) };
        if (!OpenFixes::Hooks::expectVfunc(roomVtable.address(), DESTRUCTOR, roomDestructor.address())
            || !OpenFixes::Hooks::expectVfunc(sharedNodeVtable.address(), DESTRUCTOR, sharedNodeDestructor.address())
            || !OpenFixes::Hooks::expectVfunc(portalVtable.address(), DESTRUCTOR, portalDestructor.address())) {
            return false;
        }

        // Calls to the light-queue pass: 99701 (SE) / 106335 (AE, 1.7), VR 0x12F77E0
        const REL::Relocation<std::uintptr_t> pass { REL::VariantID(99701, 106335, 0x12F77E0) };
        const bool vr = OpenFixes::currentRuntime() == OpenFixes::Runtime::VR;
        const auto calls = OpenFixes::select<std::span<const CallSite>>(SE_PASS_CALLS,
            AE_PASS_CALLS,
            AE_PASS_CALLS,
            VR_PASS_CALLS);
        std::array<std::uintptr_t, 9> sites {};
        std::size_t count = 0;
        for (const auto& call : calls) {
            const auto function = vr ? REL::Offset(call.function).address() : REL::ID(call.function).address();
            const auto site = function + call.offset;
            if (OpenFixes::Hooks::expectCall(site, pass.address())) {
                sites[count++] = site;
            } else if (call.original) {
                return false;
            } else if (vr) {
                spdlog::warn("[DestroyedLightStillInSceneLists] light pass call at SkyrimVR.exe+{:X} +{:X} left alone",
                    call.function,
                    call.offset);
            } else {
                spdlog::warn("[DestroyedLightStillInSceneLists] light pass call in {} +{:X} left alone",
                    call.function,
                    call.offset);
            }
        }

        s_roomVtable = roomVtable.address();
        s_sharedNodeVtable = sharedNodeVtable.address();
        s_portalVtable = portalVtable.address();

        for (std::size_t i = 0; i < count; ++i) {
            OpenFixes::Hooks::call<ProcessLightQueues>(sites[i]);
        }

        OpenFixes::Hooks::vfunc<LightDestructor<0>>(vtables[0], DESTRUCTOR);
        OpenFixes::Hooks::vfunc<LightDestructor<1>>(vtables[1], DESTRUCTOR);
        OpenFixes::Hooks::vfunc<LightDestructor<2>>(vtables[2], DESTRUCTOR);
        OpenFixes::Hooks::vfunc<LightDestructor<3>>(vtables[3], DESTRUCTOR);
        OpenFixes::Hooks::vfunc<LightDestructor<4>>(vtables[4], DESTRUCTOR);
        OpenFixes::Hooks::vfunc<LightSetLight<0>>(vtables[0], SET_LIGHT);
        OpenFixes::Hooks::vfunc<LightSetLight<1>>(vtables[1], SET_LIGHT);
        OpenFixes::Hooks::vfunc<LightSetLight<2>>(vtables[2], SET_LIGHT);
        OpenFixes::Hooks::vfunc<LightSetLight<3>>(vtables[3], SET_LIGHT);
        OpenFixes::Hooks::vfunc<LightSetLight<4>>(vtables[4], SET_LIGHT);
        OpenFixes::Hooks::vfunc<RoomDestructor>(s_roomVtable, DESTRUCTOR);
        OpenFixes::Hooks::vfunc<SharedNodeDestructor>(s_sharedNodeVtable, DESTRUCTOR);
        OpenFixes::Hooks::vfunc<PortalDestructor>(s_portalVtable, DESTRUCTOR);
        return true;
    }
};

} // namespace

OPENFIXES_REGISTER(DestroyedLightStillInSceneLists)
