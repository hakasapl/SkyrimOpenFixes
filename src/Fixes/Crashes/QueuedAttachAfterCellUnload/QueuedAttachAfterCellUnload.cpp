// Background-loaded 3D attached to a reference whose cell is no longer loaded
//
// Ported from "Shadow Scene Node Crash Fix" by Bottle (InTheBottle), https://github.com/InTheBottle/SCN-Crash-fix
// (commit b54378f)
// Original code: GNU GPL v3.0, Copyright (c) 2026 InTheBottle. Full text in LICENSE at the repository root.
// SPDX-License-Identifier: GPL-3.0-only

#include "PCH.h"

#include <atomic>
#include <cstdint>

namespace {

/**
 * When a reference's model finishes loading in the background, QueuedReference's finish step (12880 / 13023) checks
 * TES::IsCellLoaded (13163 / 13308: cell state 3 to 7) for the reference's parent cell and then hands the model to
 * the attach step (13209 / 13355), which creates the cell's 3D and attaches the model to the scene. The original mod
 * found crashes in LOD cell transitions where the cell had left those states by the time of the attach. The fix tests
 * the same states again, on the cell actually passed to the attach, and skips the attach when they no longer hold.
 * Skipping it is what the game does when its own check fails: the reference gets no 3D, the finish step still marks
 * it done, and the loaded model is released when the load task is destroyed (QueuedReference's destructor drops its
 * reference at +0x2C8).
 *
 * VR 1.4.15 has the same code as 1.5.97, with the attach call at the same offset and the cell state at the same place;
 * the load task's members sit 8 bytes further on (the model at +0x2D0). Its Address Library lacks 12880 and 13209, so
 * they are given by their offsets in SkyrimVR.exe. The original mod didn't hook this call on VR.
 */

constexpr std::uint8_t FIRST_LOADED_STATE = 3;
constexpr std::uint8_t LAST_LOADED_STATE = 7;

constexpr std::uint64_t LOG_CAP = 16;
std::atomic<std::uint64_t> s_logged { 0 };

/**
 * Wraps the attach step's call in the finish step
 */
struct AttachQueuedModel {
    static void thunk(RE::TES* tes,
        RE::TESObjectREFR* ref,
        RE::TESObjectCELL* cell,
        void* task,
        bool reload,
        RE::NiAVObject* model)
    {
        const auto state = cell != nullptr ? cell->cellState.underlying() : std::uint8_t { 0 };
        if (ref != nullptr && (state < FIRST_LOADED_STATE || state > LAST_LOADED_STATE)) {
            if (s_logged.fetch_add(1) < LOG_CAP) {
                spdlog::debug("[QueuedAttachAfterCellUnload] skipped attaching {:08X}: cell {:08X} is in state {}",
                    ref->GetFormID(),
                    cell != nullptr ? cell->GetFormID() : 0,
                    state);
            }
            return;
        }
        func(tes, ref, cell, task, reload, model);
    }

    static inline REL::Relocation<decltype(thunk)> func;
};

class QueuedAttachAfterCellUnload final : public OpenFixes::Fix {
public:
    QueuedAttachAfterCellUnload()
        : Fix({
              .id = "QueuedAttachAfterCellUnload",
              .section = "Crashes",
              .description = "Background-loaded 3D attached to a reference whose cell has already unloaded",
              .details = "Off by default: right before a background-loaded model is attached, repeats the game's own "
                         "check that the reference's cell is still loaded, and if the cell unloaded in between, skips "
                         "the attach the way the game does when its check fails (no 3D, the model is released with "
                         "its load task). The game makes the same check a few instructions earlier on the same "
                         "thread, so this only matters if the cell unloads in that window, which the review could not "
                         "show happens.",
              .origin = "Shadow Scene Node Crash Fix by Bottle",
              .runtimes = OpenFixes::Runtime::ALL,
              .enabledByDefault = false,
              .skipIfInstalled = { "ShadowSceneNodeCrashFix.dll" },
          })
    {
    }

    bool install() override
    {
        // Finish step 12880 (SE) / 13023 (AE, 1.7) / VR 0x156AB0 calling the attach step 13209 / 13355 / VR 0x167A80
        const REL::Relocation<std::uintptr_t> finish { REL::VariantID(12880, 13023, 0x156AB0) };
        const auto callSite = finish.address() + OpenFixes::select<std::uintptr_t>(0x179, 0x1AB, 0x1AB, 0x179);
        const REL::Relocation<std::uintptr_t> attach { REL::VariantID(13209, 13355, 0x167A80) };
        if (!OpenFixes::Hooks::expectCall(callSite, attach.address())) {
            return false;
        }
        OpenFixes::Hooks::call<AttachQueuedModel>(callSite);
        return true;
    }
};

} // namespace

OPENFIXES_REGISTER(QueuedAttachAfterCellUnload)
