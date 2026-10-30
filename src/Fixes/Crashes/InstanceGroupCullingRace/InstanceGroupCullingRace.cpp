// Crash in tree LOD and grass culling (BSMultiStreamInstanceTriShape::OnVisible) racing instance group changes
//
// Ported from "LOD Refresh Bug Fix" (SkyrimLODFixes) by Bottle (InTheBottle),
// https://github.com/InTheBottle/SkyrimLODFixes (commit e76efd1)
// Original code: GNU GPL v3.0, Copyright (c) 2026 InTheBottle. Full text in LICENSE at the repository root.
// SPDX-License-Identifier: GPL-3.0-only

#include "PCH.h"

#include "OpenFixes/Util.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace {

/**
 * Tree LOD and grass are drawn as BSMultiStreamInstanceTriShape: one shape holding an array of instance groups that
 * loader threads add and remove (AddGroup/RemoveGroup) while holding a global spin lock; RemoveGroup frees the group
 * and AddGroup may reallocate the array. OnVisible walks the same array on the culling thread without taking that
 * lock, so it can read a freed group or a stale array and crash. The fix holds the engine's own lock while OnVisible
 * tests the groups, and releases it right before OnVisible hands the shape to the base class for render
 * registration, so the lock is never held across work that doesn't touch the group array.
 */

// OnVisible's vtable slot; VR has one more NiAVObject virtual ahead of it
constexpr std::size_t ON_VISIBLE = 0x34;
constexpr std::size_t ON_VISIBLE_VR = 0x35;

// The engine's lock is a bare, non-recursive 0/1 flag taken with lock cmpxchg and released with a plain store
std::uint32_t* s_lock = nullptr;
thread_local bool t_held = false;

void acquire()
{
    std::atomic_ref<std::uint32_t> lock(*s_lock);
    for (std::uint32_t expected = 0; !lock.compare_exchange_weak(expected, 1); expected = 0) {
        std::this_thread::yield();
    }
    t_held = true;
}

void release()
{
    if (t_held) {
        t_held = false;
        std::atomic_ref<std::uint32_t>(*s_lock).store(0);
    }
}

/**
 * Wraps BSMultiStreamInstanceTriShape::OnVisible so the group tests run under the instance group lock
 */
struct OnVisible {
    static void thunk(RE::BSMultiStreamInstanceTriShape* shape, RE::NiCullingProcess& process, std::int32_t alphaGroup)
    {
        acquire();
        func(shape, process, alphaGroup);
        release();
    }

    static inline REL::Relocation<decltype(thunk)> func;
};

/**
 * Replaces OnVisible's call to BSInstanceTriShape::OnVisible: the groups are tested by now, so let go of the lock
 */
struct BaseOnVisible {
    static void thunk(RE::BSMultiStreamInstanceTriShape* shape, RE::NiCullingProcess& process, std::int32_t alphaGroup)
    {
        release();
        func(shape, process, alphaGroup);
    }

    static inline REL::Relocation<decltype(thunk)> func;
};

/**
 * Checks that a `lock cmpxchg dword ptr [rip+disp32], r32` at address takes the lock at expected
 */
bool takesLock(std::uintptr_t address, std::uintptr_t expected)
{
    if (!OpenFixes::Hooks::expect(address, { 0xF0, 0x0F, 0xB1 })) {
        return false;
    }
    const auto modrm = *reinterpret_cast<const std::uint8_t*>(address + 3);
    if ((modrm & 0xC7) != 0x05) {
        spdlog::error("[InstanceGroupCullingRace] {:#x} is not a rip-relative lock cmpxchg", address);
        return false;
    }
    const auto target = OpenFixes::Util::ripTarget(address, 8);
    if (target != expected) {
        spdlog::error("[InstanceGroupCullingRace] lock at {:#x} is {:#x}, expected {:#x}", address, target, expected);
        return false;
    }
    return true;
}

class InstanceGroupCullingRace final : public OpenFixes::Fix {
public:
    InstanceGroupCullingRace()
        : Fix({
              .id = "InstanceGroupCullingRace",
              .section = "Crashes",
              .description = "Crash in tree LOD and grass culling while instance groups are added or removed",
              .details = "Holds the game's own instance-group lock while tree LOD and grass culling read the group "
                         "list, so loader threads can't free a group mid-read. The lock is released before the shape "
                         "is passed on for rendering.",
              .origin = "LOD Refresh Bug Fix by Bottle",
              .runtimes = OpenFixes::Runtime::ALL,
              .skipIfInstalled = { "SkyrimLODFixes.dll" },
          })
    {
    }

    bool install() override
    {
        // The VR Address Library lacks these IDs, so VR uses the offsets of the same code in SkyrimVR.exe 1.4.15

        // Instance group spin lock: 524610 (SE) / 411250 (AE, 1.7) / 0x317C990 (VR)
        const REL::Relocation<std::uintptr_t> lock { REL::VariantID(524610, 411250, 0x317C990) };
        if (lock.address() % alignof(std::uint32_t) != 0) {
            spdlog::error("[InstanceGroupCullingRace] lock at {:#x} is misaligned", lock.address());
            return false;
        }

        // AddGroup (vtable 0x3C, VR 0x3D) and RemoveGroup (0x3D, VR 0x3E) take that lock: 74595 / 76323 / 0xD8F080
        // and 74600 / 76328 / 0xD8FBE0
        const REL::Relocation<std::uintptr_t> addGroup { REL::VariantID(74595, 76323, 0xD8F080) };
        const REL::Relocation<std::uintptr_t> removeGroup { REL::VariantID(74600, 76328, 0xD8FBE0) };
        if (!takesLock(addGroup.address() + OpenFixes::select<std::uintptr_t>(0x264, 0x254, 0x254, 0x264),
                lock.address())
            || !takesLock(removeGroup.address() + OpenFixes::select<std::uintptr_t>(0x31, 0x25, 0x25, 0x31),
                lock.address())) {
            return false;
        }

        // OnVisible (vtable 0x34, VR 0x35): 74602 (SE) / 76330 (AE, 1.7) / 0xD8FDA0 (VR); calls
        // BSInstanceTriShape::OnVisible 69542 / 70926 / 0xCB7A80
        const REL::Relocation<std::uintptr_t> vtable { RE::VTABLE_BSMultiStreamInstanceTriShape[0] };
        const REL::Relocation<std::uintptr_t> onVisible { REL::VariantID(74602, 76330, 0xD8FDA0) };
        const REL::Relocation<std::uintptr_t> baseOnVisible { REL::VariantID(69542, 70926, 0xCB7A80) };
        const auto slot = OpenFixes::select(ON_VISIBLE, ON_VISIBLE, ON_VISIBLE, ON_VISIBLE_VR);
        if (!OpenFixes::Hooks::expectVfunc(vtable.address(), slot, onVisible.address())) {
            return false;
        }
        const auto callSite = onVisible.address() + 0x1CF;
        if (!OpenFixes::Hooks::expectCall(callSite, baseOnVisible.address())) {
            return false;
        }

        s_lock = reinterpret_cast<std::uint32_t*>(lock.address());
        OpenFixes::Hooks::call<BaseOnVisible>(callSite);
        OpenFixes::Hooks::vfunc<OnVisible>(vtable.address(), slot);
        return true;
    }
};

} // namespace

OPENFIXES_REGISTER(InstanceGroupCullingRace)
