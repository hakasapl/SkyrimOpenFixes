#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <memory>

/**
 * @brief Small helpers used by more than one fix
 */
namespace OpenFixes::Util {

/**
 * @brief The address an instruction of length bytes at instruction refers to through the rip-relative 32-bit
 * displacement in its last four bytes (also the destination of a rel32 call or jump)
 */
[[nodiscard]] inline std::uintptr_t ripTarget(std::uintptr_t instruction,
                                              std::size_t length)
{
    std::int32_t displacement = 0;
    std::memcpy(&displacement, reinterpret_cast<const void*>(instruction + length - 4), sizeof(displacement));
    return instruction + length + static_cast<std::intptr_t>(displacement);
}

/**
 * @brief The address an instruction of length bytes at instruction refers to through the rip-relative 32-bit
 * displacement at dispOffset, for instructions with an immediate after it (e.g. cmp qword ptr [rip+disp32], imm8)
 */
[[nodiscard]] inline std::uintptr_t ripTarget(std::uintptr_t instruction,
                                              std::size_t dispOffset,
                                              std::size_t length)
{
    std::int32_t displacement = 0;
    std::memcpy(&displacement, reinterpret_cast<const void*>(instruction + dispOffset), sizeof(displacement));
    return instruction + length + static_cast<std::intptr_t>(displacement);
}

/**
 * @brief Takes a BSSpinLock the way the game's BSSpinLock::Lock does, but gives up after spins pauses instead of
 * sleeping until the lock is free
 *
 * BSSpinLock is { owning thread id at +0, lock count at +4 } on every runtime. Like the game, the owning thread
 * re-enters it by incrementing the count; any other thread swaps the count from 0 to 1 and then stores itself as the
 * owner. Release it with RE::BSSpinLock::Unlock, which is the game's own unlock (it clears the owner before dropping
 * the count back to 0). For code that must never wait on the lock, such as a destructor that can run while another
 * thread holds it.
 */
[[nodiscard]] inline bool tryLockSpinLock(RE::BSSpinLock& lock,
                                          std::uint32_t spins = 256)
{
    auto* const fields = reinterpret_cast<volatile long*>(std::addressof(lock));
    const auto thread = static_cast<long>(REX::W32::GetCurrentThreadId());
    _mm_lfence();
    if (fields[0] == thread) {
        _InterlockedIncrement(&fields[1]);
        return true;
    }
    for (std::uint32_t attempt = 0; attempt < spins; ++attempt) {
        if (_InterlockedCompareExchange(&fields[1], 1, 0) == 0) {
            fields[0] = thread;
            _mm_sfence();
            return true;
        }
        _mm_pause();
    }
    return false;
}

/**
 * @brief Holds a BSSpinLock taken with tryLockSpinLock until the guard goes away, if it could be taken
 */
class SpinLockTry {
public:
    explicit SpinLockTry(RE::BSSpinLock& lock)
        : m_lock(lock)
        , m_held(tryLockSpinLock(lock))
    {
    }

    ~SpinLockTry()
    {
        if (m_held) {
            m_lock.Unlock();
        }
    }

    SpinLockTry(const SpinLockTry&) = delete;
    SpinLockTry(SpinLockTry&&) = delete;
    SpinLockTry& operator=(const SpinLockTry&) = delete;
    SpinLockTry& operator=(SpinLockTry&&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept { return m_held; }

private:
    RE::BSSpinLock& m_lock;
    bool m_held;
};

/**
 * @brief Virtual tables of every light class the game has: BSLight, BSShadowLight and the directional, frustum and
 * parabolic shadow lights (the only classes derived from BSLight on 1.5.97, 1.6.1170, 1.7.99 and VR 1.4.15)
 *
 * Only call this from code that runs after a fix was installed.
 */
[[nodiscard]] inline const std::array<std::uintptr_t, 5>& lightVtables()
{
    static const std::array<std::uintptr_t, 5> s_vtables {
        REL::Relocation<std::uintptr_t> { RE::VTABLE_BSLight[0] }.address(),
        REL::Relocation<std::uintptr_t> { RE::VTABLE_BSShadowLight[0] }.address(),
        REL::Relocation<std::uintptr_t> { RE::VTABLE_BSShadowDirectionalLight[0] }.address(),
        REL::Relocation<std::uintptr_t> { RE::VTABLE_BSShadowFrustumLight[0] }.address(),
        REL::Relocation<std::uintptr_t> { RE::VTABLE_BSShadowParabolicLight[0] }.address(),
    };
    return s_vtables;
}

/**
 * @brief Whether a light pointer taken from a scene list no longer points at a live light
 *
 * A destroyed light can't keep one of the five light virtual tables (its destructors leave NiRefObject's, and the
 * allocator then writes its free-list link over the start of the block), and a light whose last reference is gone has
 * a zero reference count. A block the allocator has already handed to a new light that is referenced again passes:
 * it is a live light by then.
 */
[[nodiscard]] inline bool isDeadLight(const RE::BSLight* light)
{
    const auto vtable = *reinterpret_cast<const std::uintptr_t*>(light);
    const auto& vtables = lightVtables();
    if (std::ranges::find(vtables, vtable) == vtables.end()) {
        return true;
    }
    return light->GetRefCount() == 0;
}

/**
 * @brief Sets to nullptr, without releasing them, the entries that matches returns true for in the ShadowSceneNode
 * lists that may hold empty entries: the add, remove and dirty queues and the shadow light accumulation slots
 *
 * For entries that point at a light that is being or has been destroyed, whose reference is no longer real. Every
 * loop over the queues skips empty entries (and the light pass empties them, releasing only the entries left), and
 * an empty accumulation slot is how the game itself takes a shadow light out of it. The active lists must not get
 * empty entries: see eraseActiveLightEntries. Call with the node's lightQueueLock held.
 *
 * @return Number of entries cleared
 */
template <class Pred>
std::size_t clearSceneLightEntries(RE::ShadowSceneNode* node,
                                   const Pred& matches)
{
    auto& data = node->GetRuntimeData();
    std::size_t cleared = 0;
    const auto clear = [&](auto& list) {
        auto** const entries = reinterpret_cast<void**>(list.data());
        for (std::uint32_t i = 0; i < list.size(); ++i) {
            if (entries[i] != nullptr && matches(static_cast<const void*>(entries[i]))) {
                entries[i] = nullptr;
                ++cleared;
            }
        }
    };
    clear(data.lightQueueAdd);
    clear(data.lightQueueRemove);
    clear(data.unk190);
    clear(data.shadowLightsAccum);
    return cleared;
}

/**
 * @brief Number of entries that matches returns true for in a ShadowSceneNode's active and active shadow light lists
 */
template <class Pred>
[[nodiscard]] std::size_t countActiveLightEntries(RE::ShadowSceneNode* node,
                                                  const Pred& matches)
{
    auto& data = node->GetRuntimeData();
    std::size_t count = 0;
    const auto countIn = [&](auto& list) {
        auto** const entries = reinterpret_cast<void**>(list.data());
        for (std::uint32_t i = 0; i < list.size(); ++i) {
            if (entries[i] != nullptr && matches(static_cast<const void*>(entries[i]))) {
                ++count;
            }
        }
    };
    countIn(data.activeLights);
    countIn(data.activeShadowLights);
    return count;
}

/**
 * @brief Removes, without releasing them, the entries that matches returns true for from a ShadowSceneNode's active and
 * active shadow light lists, closing the gaps in order as the game's own removal does
 *
 * The game never leaves an empty entry in these lists: the light pass's last loop over the active lights doesn't move
 * past one and spins forever (1.5.97, 1.6.1170, 1.7.99 and VR 1.4.15), and some readers of the active shadow lights
 * don't check for one. Only call this with the node's lightQueueLock held while no game code is walking or removing
 * from these lists further up the stack: not from inside the light pass's active-light loop, and not from a destructor
 * that runs while the game releases an entry of these lists (it releases before closing the gap).
 *
 * @return Number of entries removed
 */
template <class Pred>
std::size_t eraseActiveLightEntries(RE::ShadowSceneNode* node,
                                    const Pred& matches)
{
    auto& data = node->GetRuntimeData();
    std::size_t erased = 0;
    const auto erase = [&](auto& list) {
        auto** const entries = reinterpret_cast<void**>(list.data());
        const auto size = list.size();
        std::uint32_t kept = 0;
        for (std::uint32_t i = 0; i < size; ++i) {
            if (entries[i] != nullptr && matches(static_cast<const void*>(entries[i]))) {
                ++erased;
            } else {
                entries[kept++] = entries[i];
            }
        }
        // The vacated tail holds no reference any more; pop_back then destroys an empty NiPointer
        for (auto i = kept; i < size; ++i) {
            entries[i] = nullptr;
        }
        while (list.size() > kept) {
            list.pop_back();
        }
    };
    erase(data.activeLights);
    erase(data.activeShadowLights);
    return erased;
}

} // namespace OpenFixes::Util
