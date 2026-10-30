#include "PCH.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OpenFixes::Hooks {

namespace {

struct Change {
    std::uintptr_t address;
    std::size_t size;
    std::string kind;
    std::string fixId;
};

std::mutex s_lock;
std::vector<Change> s_changes;
std::string s_currentFix;

std::string toHex(const std::uint8_t* bytes, std::size_t size)
{
    std::string out;
    out.reserve(size * 3);
    for (std::size_t i = 0; i < size; ++i) {
        out += std::format("{:02X} ", bytes[i]);
    }
    if (!out.empty()) {
        out.pop_back();
    }
    return out;
}

std::uintptr_t offsetOf(std::uintptr_t address)
{
    return address - REL::Module::get().base();
}

bool expectRel32(std::uintptr_t address, std::uint8_t opcode, std::uintptr_t target, std::string_view what)
{
    const auto* code = reinterpret_cast<const std::uint8_t*>(address);
    if (code[0] != opcode) {
        spdlog::error("[{}] expected a {} at +{:X} but found {}", s_currentFix, what, offsetOf(address),
            toHex(code, 5));
        return false;
    }
    if (target != 0) {
        std::int32_t displacement = 0;
        std::memcpy(&displacement, code + 1, sizeof(displacement));
        const auto destination = address + 5 + static_cast<std::intptr_t>(displacement);
        if (destination != target) {
            spdlog::error("[{}] {} at +{:X} goes to +{:X} instead of +{:X} (already patched by another mod?)",
                s_currentFix,
                what,
                offsetOf(address),
                offsetOf(destination),
                offsetOf(target));
            return false;
        }
    }
    return true;
}

bool isChainable(std::string_view kind)
{
    return kind == "call" || kind == "branch" || kind == "vfunc" || kind == "inline";
}

bool fitsRel32(std::intptr_t displacement)
{
    return displacement >= std::numeric_limits<std::int32_t>::min()
        && displacement <= std::numeric_limits<std::int32_t>::max();
}

// Trampoline stubs already written, by destination, for 5-byte (absolute jump) and 6-byte (address) sites
std::map<std::uintptr_t, std::byte*> s_jumpStubs;
std::map<std::uintptr_t, std::byte*> s_addressSlots;
std::size_t s_trampolineBytes = 0;

/**
 * @brief Exactly size bytes of executable memory within rel32 range of the game
 */
std::byte* allocateTrampoline(std::size_t size)
{
    void* memory = nullptr;
    if (const auto* intfc = SKSE::GetTrampolineInterface(); intfc != nullptr) {
        memory = intfc->AllocateFromBranchPool(size);
    }
    if (memory == nullptr) {
        // No SKSE branch pool: CommonLib maps a block of this size near the game instead. The trampolines are kept
        // alive because releasing one would free code the game now jumps into.
        static std::deque<SKSE::Trampoline> s_fallbacks;
        auto& fallback = s_fallbacks.emplace_back("OpenFixes");
        fallback.create(size, reinterpret_cast<void*>(REL::Module::get().base()));
        memory = fallback.allocate(size);
    }
    s_trampolineBytes += size;
    return static_cast<std::byte*>(memory);
}

} // namespace

std::uintptr_t writeBranch(std::uintptr_t source, std::uintptr_t destination, std::size_t size, bool isCall)
{
    const std::scoped_lock guard(s_lock);

    // Same rule as CommonLib: the old target is the rel32 at the end of the instruction, unless it was a NOP
    const auto* code = reinterpret_cast<const std::uint8_t*>(source);
    std::int32_t oldDisplacement = 0;
    std::memcpy(&oldDisplacement, code + size - 4, sizeof(oldDisplacement));
    const auto original = code[0] == 0x90 ? 0 : source + size + static_cast<std::intptr_t>(oldDisplacement);

    // A 5-byte site whose destination is within rel32 range (code in SKSE's branch pool, or a thunk in this DLL when
    // it happens to load near the game) is pointed straight at it, so no trampoline memory is needed
    if (const auto direct = static_cast<std::intptr_t>(destination) - static_cast<std::intptr_t>(source + 5);
        size == 5 && fitsRel32(direct)) {
        const auto rel32 = static_cast<std::int32_t>(direct);
        std::array<std::uint8_t, 5> instruction { static_cast<std::uint8_t>(isCall ? 0xE8 : 0xE9) };
        std::memcpy(&instruction[1], &rel32, sizeof(rel32));
        REL::safe_write(source, instruction.data(), instruction.size());
        return original;
    }

    std::byte* stub = nullptr;
    if (size == 5) {
        auto& existing = s_jumpStubs[destination];
        if (existing == nullptr) {
            // jmp [rip+0]; dq destination
            existing = allocateTrampoline(14);
            const std::array<std::uint8_t, 6> jump { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
            std::memcpy(existing, jump.data(), jump.size());
            std::memcpy(existing + jump.size(), &destination, sizeof(destination));
        }
        stub = existing;
    } else {
        auto& existing = s_addressSlots[destination];
        if (existing == nullptr) {
            existing = allocateTrampoline(sizeof(destination));
            std::memcpy(existing, &destination, sizeof(destination));
        }
        stub = existing;
    }

    const auto displacement = reinterpret_cast<std::intptr_t>(stub) - static_cast<std::intptr_t>(source + size);
    if (!fitsRel32(displacement)) {
        SKSE::stl::report_and_fail("OpenFixes: trampoline memory is out of rel32 range of the game");
    }
    const auto rel32 = static_cast<std::int32_t>(displacement);

    std::array<std::uint8_t, 6> instruction {};
    if (size == 5) {
        instruction[0] = isCall ? 0xE8 : 0xE9; // call/jmp rel32
        std::memcpy(&instruction[1], &rel32, sizeof(rel32));
    } else {
        instruction[0] = 0xFF;
        instruction[1] = isCall ? 0x15 : 0x25; // call/jmp [rip+disp32]
        std::memcpy(&instruction[2], &rel32, sizeof(rel32));
    }
    REL::safe_write(source, instruction.data(), size);
    return original;
}

std::uintptr_t allocateCode(Xbyak::CodeGenerator& code)
{
    const std::scoped_lock guard(s_lock);
    code.ready();
    auto* memory = allocateTrampoline(code.getSize());
    std::memcpy(memory, code.getCode(), code.getSize());
    return reinterpret_cast<std::uintptr_t>(memory);
}

std::size_t trampolineBytes() noexcept
{
    return s_trampolineBytes;
}

void setCurrentFix(std::string_view fixId)
{
    const std::scoped_lock guard(s_lock);
    s_currentFix = fixId;
}

void track(std::uintptr_t address, std::size_t size, std::string_view kind)
{
    const std::scoped_lock guard(s_lock);
    s_changes.push_back({ address, size, std::string(kind), s_currentFix });
    spdlog::debug("[{}] {} at +{:X} ({} bytes)", s_currentFix, kind, offsetOf(address), size);
}

bool expect(std::uintptr_t address, std::span<const std::uint8_t> bytes)
{
    const auto* code = reinterpret_cast<const std::uint8_t*>(address);
    if (std::memcmp(code, bytes.data(), bytes.size()) == 0) {
        return true;
    }
    spdlog::error("[{}] unexpected code at +{:X}: expected {}, found {}",
        s_currentFix,
        offsetOf(address),
        toHex(bytes.data(), bytes.size()),
        toHex(code, bytes.size()));
    return false;
}

bool expect(std::uintptr_t address, std::initializer_list<std::uint8_t> bytes)
{
    return expect(address, std::span(bytes.begin(), bytes.size()));
}

bool expectCall(std::uintptr_t address, std::uintptr_t target)
{
    return expectRel32(address, 0xE8, target, "call");
}

bool expectJump(std::uintptr_t address, std::uintptr_t target)
{
    return expectRel32(address, 0xE9, target, "jump");
}

bool expectVfunc(std::uintptr_t vtable, std::size_t index, std::uintptr_t expected)
{
    const auto entry = *reinterpret_cast<const std::uintptr_t*>(vtable + (index * sizeof(std::uintptr_t)));
    if (entry == expected) {
        return true;
    }
    const auto text = REL::Module::get().segment(REL::Segment::textx);
    const bool inGame = entry >= text.address() && entry < text.address() + text.size();
    spdlog::error("[{}] vtable +{:X} entry {:#x} is {} instead of +{:X} (replaced by another mod?)",
        s_currentFix,
        offsetOf(vtable),
        index,
        inGame ? std::format("+{:X}", offsetOf(entry)) : std::format("{:#x} outside the game", entry),
        offsetOf(expected));
    return false;
}

void patch(std::uintptr_t address, std::span<const std::uint8_t> bytes)
{
    REL::safe_write(address, bytes.data(), bytes.size());
    track(address, bytes.size(), "patch");
}

void patch(std::uintptr_t address, std::initializer_list<std::uint8_t> bytes)
{
    patch(address, std::span(bytes.begin(), bytes.size()));
}

void nop(std::uintptr_t address, std::size_t size)
{
    REL::safe_fill(address, 0x90, size);
    track(address, size, "nop");
}

SafetyHookInline inlineHook(std::uintptr_t target, void* destination)
{
    auto hook = safetyhook::create_inline(reinterpret_cast<void*>(target), destination);
    if (!hook) {
        spdlog::error("[{}] failed to create inline hook at +{:X}", s_currentFix, offsetOf(target));
    } else {
        track(target, 5, "inline");
    }
    return hook;
}

SafetyHookMid midHook(std::uintptr_t target, safetyhook::MidHookFn destination)
{
    auto hook = safetyhook::create_mid(reinterpret_cast<void*>(target), destination);
    if (!hook) {
        spdlog::error("[{}] failed to create mid hook at +{:X}", s_currentFix, offsetOf(target));
    } else {
        track(target, 5, "mid");
    }
    return hook;
}

void reportOverlaps()
{
    const std::scoped_lock guard(s_lock);
    auto changes = s_changes;
    std::ranges::sort(changes, {}, &Change::address);

    for (std::size_t i = 0; i < changes.size(); ++i) {
        for (std::size_t j = i + 1; j < changes.size() && changes[j].address < changes[i].address + changes[i].size;
             ++j) {
            const auto& first = changes[i];
            const auto& second = changes[j];
            if (first.fixId == second.fixId) {
                continue;
            }
            // Redirecting the same call, jump, vtable slot or function twice chains the two thunks
            if (first.address == second.address && first.kind == second.kind && isChainable(first.kind)) {
                spdlog::info("{} and {} both hook the {} at +{:X} (chained)",
                    first.fixId,
                    second.fixId,
                    first.kind,
                    offsetOf(first.address));
            } else {
                spdlog::warn("{} ({} at +{:X}) overlaps {} ({} at +{:X})",
                    first.fixId,
                    first.kind,
                    offsetOf(first.address),
                    second.fixId,
                    second.kind,
                    offsetOf(second.address));
            }
        }
    }
}

} // namespace OpenFixes::Hooks
