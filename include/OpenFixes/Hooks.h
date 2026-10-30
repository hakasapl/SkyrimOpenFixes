#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>

#include <safetyhook.hpp>

/**
 * @brief Patching helpers shared by every fix
 *
 * Each helper records the bytes it changes against the fix being installed, so the log can show which fixes touch the
 * same code. Two fixes that both redirect the same call or virtual function chain correctly (the second one calls the
 * first); anything else overlapping is logged as a warning.
 */
namespace OpenFixes::Hooks {

/**
 * @brief Records that the current fix changed size bytes at address
 */
void track(std::uintptr_t address, std::size_t size, std::string_view kind);

/**
 * @brief Checks the game code at address before patching it; logs the difference and returns false on mismatch
 */
bool expect(std::uintptr_t address, std::span<const std::uint8_t> bytes);
bool expect(std::uintptr_t address, std::initializer_list<std::uint8_t> bytes);

/**
 * @brief Checks that address holds a 5-byte rel32 call (E8) or jump (E9), optionally to a specific target
 */
bool expectCall(std::uintptr_t address, std::uintptr_t target = 0);
bool expectJump(std::uintptr_t address, std::uintptr_t target = 0);

/**
 * @brief Checks that entry index of the virtual table at vtable still holds the game's function expected; logs and
 * returns false if it doesn't (e.g. another mod replaced it)
 */
bool expectVfunc(std::uintptr_t vtable, std::size_t index, std::uintptr_t expected);

/**
 * @brief Checks that entry index of Class's virtual table number table still holds the game's function expected
 */
template <class Class>
bool expectVfunc(std::size_t index, std::uintptr_t expected, std::size_t table = 0)
{
    return expectVfunc(Class::VTABLE[table].address(), index, expected);
}

/**
 * @brief Overwrites game code
 */
void patch(std::uintptr_t address, std::span<const std::uint8_t> bytes);
void patch(std::uintptr_t address, std::initializer_list<std::uint8_t> bytes);
void nop(std::uintptr_t address, std::size_t size);

/**
 * @brief Writes a call or jump at source that reaches destination through the trampoline, like CommonLib's
 * Trampoline::write_call / write_branch
 *
 * N = 5 writes E8/E9 rel32: straight to destination when it is within rel32 range (always for code from
 * allocateCode), otherwise to a 14-byte absolute jump. N = 6 writes FF 15/FF 25 [rip+disp32] to an 8-byte address.
 * Trampoline memory is taken from SKSE's branch pool only when a stub is needed, and one stub is shared by every site
 * that reaches the same destination, so OpenFixes never reserves more than its installed hooks use.
 *
 * @return The previous rel32 target at source (0 if source was a NOP), as CommonLib returns it
 */
std::uintptr_t writeBranch(std::uintptr_t source, std::uintptr_t destination, std::size_t size, bool isCall);

/**
 * @brief Copies the finished Xbyak code into trampoline memory of exactly its size and returns its address
 */
std::uintptr_t allocateCode(Xbyak::CodeGenerator& code);

/**
 * @brief Total trampoline bytes taken so far
 */
[[nodiscard]] std::size_t trampolineBytes() noexcept;

/**
 * @brief Redirects the rel32 call at source to T::thunk, storing the original target in T::func
 */
template <class T, std::size_t N = 5>
void call(std::uintptr_t source)
{
    static_assert(N == 5 || N == 6);
    T::func = writeBranch(source, SKSE::stl::unrestricted_cast<std::uintptr_t>(T::thunk), N, true);
    track(source, N, "call");
}

/**
 * @brief Redirects the rel32 jump at source to T::thunk, storing the original target in T::func
 */
template <class T, std::size_t N = 5>
void branch(std::uintptr_t source)
{
    static_assert(N == 5 || N == 6);
    T::func = writeBranch(source, SKSE::stl::unrestricted_cast<std::uintptr_t>(T::thunk), N, false);
    track(source, N, "branch");
}

/**
 * @brief Copies the finished Xbyak code into the trampoline and replaces the N bytes at source with a jump to it
 *
 * The code must jump back to the game itself.
 *
 * @return Address the code was copied to
 */
template <std::size_t N = 5>
std::uintptr_t branchToCode(std::uintptr_t source, Xbyak::CodeGenerator& code)
{
    static_assert(N == 5 || N == 6);
    const auto destination = allocateCode(code);
    writeBranch(source, destination, N, false);
    track(source, N, "branch");
    return destination;
}

/**
 * @brief Replaces entry index of the virtual table at vtable with T::thunk, storing the original in T::func
 */
template <class T>
void vfunc(std::uintptr_t vtable, std::size_t index)
{
    REL::Relocation<std::uintptr_t> table { vtable };
    T::func = table.write_vfunc(index, T::thunk);
    track(vtable + (index * sizeof(std::uintptr_t)), sizeof(std::uintptr_t), "vfunc");
}

/**
 * @brief Replaces entry index of Class's virtual table number table with T::thunk
 */
template <class Class, class T>
void vfunc(std::size_t index, std::size_t table = 0)
{
    vfunc<T>(Class::VTABLE[table].address(), index);
}

/**
 * @brief Detours the function starting at target; call the original through the returned hook
 */
SafetyHookInline inlineHook(std::uintptr_t target, void* destination);

/**
 * @brief Runs destination with the full register context when execution reaches target
 */
SafetyHookMid midHook(std::uintptr_t target, safetyhook::MidHookFn destination);

/**
 * @brief Logs every address patched by more than one fix; called by the framework after installation
 */
void reportOverlaps();

/**
 * @brief Sets the fix that track() attributes changes to; managed by the framework
 */
void setCurrentFix(std::string_view fixId);

} // namespace OpenFixes::Hooks
