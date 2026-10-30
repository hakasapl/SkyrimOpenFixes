#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace OpenFixes {

/**
 * @brief Runtime families a fix can run on
 *
 * CommonLibSSE-NG resolves function addresses per runtime, but offsets inside functions, patched bytes and struct
 * layouts can still differ between runtimes, so every fix states the runtimes it was checked against. A fix is never
 * installed on a runtime it doesn't list.
 *
 * On VR, RELOCATION_ID(se, ae) resolves the SE ID through the VR Address Library, which only holds a small part of the
 * SE IDs; an ID it doesn't have stops the game. A fix lists VR only once every address it uses resolves on VR.
 */
enum class Runtime : std::uint8_t {
    NONE = 0,
    SE = 1U << 0,          /**< 1.5.97 */
    AE = 1U << 1,          /**< 1.6.x, checked against 1.6.1170 */
    AE17 = 1U << 2,        /**< 1.7.x, checked against 1.7.99 */
    VR = 1U << 3,          /**< Skyrim VR 1.4.15 */
    FLAT = SE | AE | AE17, /**< Every non-VR runtime */
    ALL = FLAT | VR,
};

[[nodiscard]] constexpr Runtime operator|(Runtime lhs, Runtime rhs) noexcept
{
    return static_cast<Runtime>(std::to_underlying(lhs) | std::to_underlying(rhs));
}

[[nodiscard]] constexpr bool hasRuntime(Runtime set, Runtime runtime) noexcept
{
    return (std::to_underlying(set) & std::to_underlying(runtime)) != 0;
}

/**
 * @brief The runtime family the game is running
 */
[[nodiscard]] Runtime currentRuntime() noexcept;

/**
 * @brief Picks a per-runtime value, such as an offset inside a function or a patched byte, for a fix that runs on VR
 */
template <class T>
[[nodiscard]] T select(T se, T ae, T ae17, T vr) noexcept
{
    switch (currentRuntime()) {
    case Runtime::SE:
        return se;
    case Runtime::AE:
        return ae;
    case Runtime::VR:
        return vr;
    default:
        return ae17;
    }
}

/**
 * @brief Picks a per-runtime value for a fix that doesn't run on VR
 *
 * Only fixes that list VR are installed there, and those use the four-value overload (the lint checks it), so this one
 * never runs on VR.
 */
template <class T>
[[nodiscard]] T select(T se, T ae, T ae17) noexcept
{
    return select(se, ae, ae17, se);
}

/**
 * @brief Static description of a fix, used for the INI toggle, runtime gating, conflict checks and the log
 *
 * Write every field as a literal: tools/gen_ini.py reads them from the source to generate package/OpenFixes.ini, one
 * line per fix with "<description> - <origin>" as its comment. A fix has only that on/off toggle and no settings of
 * its own (CONTRIBUTING.md).
 */
struct FixInfo {
    std::string_view id;          /**< INI key, PascalCase and unique, e.g. "AIPathingFreeze" */
    std::string_view section;     /**< INI section, one of the sections in package/OpenFixes.ini */
    std::string_view description; /**< One line describing the bug that is fixed */
    std::string_view details;     /**< One or two sentences on what the fix changes in the game, for the README */
    std::string_view origin;      /**< Mod the fix comes from and its authors, e.g. "AI Pathing Freeze Fix by cashboxs" */
    Runtime runtimes = Runtime::FLAT; /**< Runtimes the fix was checked on; VR must be listed explicitly */
    bool enabledByDefault = true;
    std::vector<std::string_view> skipIfInstalled; /**< SKSE plugin DLL file names (e.g. "po3_Tweaks.dll") of the
                                                        original mod and of any other mod that applies the same fix;
                                                        if one of them is installed, this fix is quietly skipped and
                                                        that mod keeps it */
};

/**
 * @brief One bug fix
 *
 * A fix that is turned off in the INI, unsupported on the running runtime, or provided by an installed mod listed in
 * its skipIfInstalled is never installed: none of its methods except info() are called, so it must not patch, hook or
 * register anything outside install() and the callbacks below. In particular, no namespace-scope or static objects may
 * resolve game addresses.
 */
class Fix {
public:
    explicit Fix(FixInfo info)
        : m_info(std::move(info))
    {
    }

    virtual ~Fix() = default;
    Fix(const Fix&) = delete;
    Fix(Fix&&) = delete;
    Fix& operator=(const Fix&) = delete;
    Fix& operator=(Fix&&) = delete;

    [[nodiscard]] const FixInfo& info() const noexcept { return m_info; }

    /**
     * @brief Applies the fix, called once from SKSEPluginLoad
     *
     * @return false if the fix could not be applied; it must then have changed nothing
     */
    virtual bool install() = 0;

    /**
     * @brief Receives every SKSE message (kPostLoad, kDataLoaded, kNewGame, kPostLoadGame, ...) once installed
     */
    virtual void onMessage(SKSE::MessagingInterface::Message* /*message*/) { }

    /**
     * @brief Co-save record types this fix owns; leave empty if it saves nothing
     */
    [[nodiscard]] virtual std::span<const std::uint32_t> recordTypes() const { return {}; }

    virtual void onSave(SKSE::SerializationInterface* /*intfc*/) { }
    virtual void onLoad(SKSE::SerializationInterface* /*intfc*/,
        std::uint32_t /*type*/,
        std::uint32_t /*version*/,
        std::uint32_t /*length*/)
    {
    }
    virtual void onRevert(SKSE::SerializationInterface* /*intfc*/) { }

private:
    FixInfo m_info;
};

/**
 * @brief Whether the fix with this id was installed (enabled, supported and not provided by another mod)
 *
 * Only complete once SKSEPluginLoad has returned, so ask from a message (kPostLoad or later) or at runtime.
 */
[[nodiscard]] bool isInstalled(std::string_view fixId) noexcept;

/**
 * @brief Adds a fix to the registry; use OPENFIXES_REGISTER instead of calling this directly
 */
void registerFix(std::unique_ptr<Fix> fix);

template <class T>
struct Registrar {
    Registrar() { registerFix(std::make_unique<T>()); }
};

} // namespace OpenFixes

#define OPENFIXES_CONCAT_IMPL(a, b) a##b
#define OPENFIXES_CONCAT(a, b) OPENFIXES_CONCAT_IMPL(a, b)

/**
 * @brief Registers a Fix subclass; place once at namespace scope in the fix's .cpp file
 */
#define OPENFIXES_REGISTER(TYPE)                                                                                       \
    namespace {                                                                                                        \
    const ::OpenFixes::Registrar<TYPE> OPENFIXES_CONCAT(s_registrar, __LINE__);                                        \
    }
