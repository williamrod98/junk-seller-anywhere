#pragma once
// JS v0.9.0: the E1 install ledger of
// AGH_JunkSeller, one DLL for 1.6.1170 and 1.7.104.
//
// Design: the E1 design r4 sections 2.3-2.5 and 3 (JS is
// the third adopter after SW and SR; the SR pilot pattern,
// pipeline/soul-recharge/native/src/receipt/Install.h).
// Engine free on purpose (no CommonLib, no Windows header): Plugin.cpp binds
// the CommonLib environment, tests/js_e1_test.cpp binds a fake one to the SAME
// plan and the SAME InstallCore, and forces a failure after every step.
// ASCII only (MSVC reads UTF-8 comments as cp932).
//
// SKSEPlugin_Load is exactly RunInstall(kInstallPlan, InstallCore<Env>, latch):
// any refusal, false step, throw or tail exception disarms the plugin latch,
// writes one receipt row (runtime_gate or load_failure) and returns true, so
// SKSE never shows its "Exit game?" box because of this DLL (ruling D1). The
// v0.8.0 body returned false on six of its refusals (SKSE's box) and gated on
// the exact pair 1.6.1170 + SKSE 2.2.6.
#include "agh/runtime/ImageRange.h"
#include "agh/runtime/InstallLedger.h"
#include "agh/runtime/RuntimeGate.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace js
{
    // The plugin latch (plan step "arm"; the v0.8.0 `enabled` flag), and the
    // commit latch of the sink compound (latch_gate.py LATCH_CONTRACTS["JS"]).
    // Defined in Probe.cpp. Every resident callback reads `latch` as its first
    // statement; a sink also reads `sinksLive`.
    extern agh::rt::Latch latch;
    extern agh::rt::Latch sinksLive;  // compound "listener_sinks" (kDataLoaded / kPostLoadGame / kNewGame)
}

namespace js::e1
{
    inline constexpr std::string_view kPluginName = "AGH_JunkSeller";
    // The product receipt (receipt/Log.h ReceiptBasename). A refusal or a
    // failure before the writer opened is one synchronous append to it.
    inline constexpr std::string_view kReceiptBasename = "AGH_JunkSeller.jsonl";

    inline constexpr std::string_view kWriterIdle = "writer thread idle; receipt file open in append mode; writes only rows";
    inline constexpr std::string_view kListenerLatched = "listener stays registered; returns at once unless the latch is armed";

    // The 13-step plan: pipeline/shared/runtime/tests/e1_plans.h kJunkSeller,
    // step for step (tests/js_e1_test.cpp compares them). receipt_open writes
    // the js_startup row (OpenLog's durable first row). The Papyrus bridge and
    // the event sinks are registered later by the latched listener at
    // kDataLoaded, as in v0.8.0.
    inline constexpr std::array kInstallPlan{
        agh::rt::StepSpec{ "gate", agh::rt::StepKind::kCheck, agh::rt::Undo::kNone, {} },
        agh::rt::StepSpec{ "skse_init", agh::rt::StepKind::kCheck, agh::rt::Undo::kNone, {} },
        agh::rt::StepSpec{ "al_load", agh::rt::StepKind::kCheck, agh::rt::Undo::kNone, {} },
        agh::rt::StepSpec{ "al_proof", agh::rt::StepKind::kCheck, agh::rt::Undo::kNone, {} },  // a no-op on 1.6.1170
        agh::rt::StepSpec{ "log_dir", agh::rt::StepKind::kCheck, agh::rt::Undo::kNone, {} },
        agh::rt::StepSpec{ "diagnostic_dir", agh::rt::StepKind::kReceipt, agh::rt::Undo::kNone,
            "startup-note path set; synchronous append only on failure" },
        agh::rt::StepSpec{ "task_interface", agh::rt::StepKind::kCheck, agh::rt::Undo::kNone, {} },
        agh::rt::StepSpec{ "settings", agh::rt::StepKind::kCheck, agh::rt::Undo::kNone, {} },
        agh::rt::StepSpec{ "messaging_interface", agh::rt::StepKind::kCheck, agh::rt::Undo::kNone, {} },
        agh::rt::StepSpec{ "process_run", agh::rt::StepKind::kCheck, agh::rt::Undo::kNone, {} },
        agh::rt::StepSpec{ "receipt_open", agh::rt::StepKind::kReceipt, agh::rt::Undo::kNone, kWriterIdle },
        agh::rt::StepSpec{ "listener", agh::rt::StepKind::kInstall, agh::rt::Undo::kResident, kListenerLatched },
        agh::rt::StepSpec{ "arm", agh::rt::StepKind::kArm, agh::rt::Undo::kNone, {} },
    };
    static_assert(agh::rt::ValidatePlan(kInstallPlan) == agh::rt::PlanError::kNone, "JS plan breaks ruling D1");
    // receipt_open is plan step 10: a failure at or before it has no writer.
    inline constexpr std::size_t kReceiptOpenStep = 10;
    static_assert(kInstallPlan[kReceiptOpenStep].name == "receipt_open");

    // Compound "listener_sinks" (e1_plans.h kJsSinks): the registrations of
    // Probe.cpp SinkOps::Add, in this order. input and mod_callback are
    // optional sources: an absent source adds nothing and is not a failure
    // (v0.8.0 behaviour: the hotkey is skipped / a NO_MODEVENT_SOURCE warning).
    inline constexpr std::array kSinkSubs{
        agh::rt::SubSpec{ "menu", agh::rt::SubUndo::kReversible },           // UI MenuOpenCloseEvent
        agh::rt::SubSpec{ "container", agh::rt::SubUndo::kReversible },      // TESContainerChangedEvent
        agh::rt::SubSpec{ "tracked_stats", agh::rt::SubUndo::kReversible },  // TESTrackedStatsEvent
        agh::rt::SubSpec{ "input", agh::rt::SubUndo::kReversible },          // BSInputDeviceManager (optional)
        agh::rt::SubSpec{ "mod_callback", agh::rt::SubUndo::kReversible },   // SKSE ModCallbackEvent (optional)
    };

    // The Papyrus native batch of AGH_JS_Native (API 9, 62 natives), run by
    // the VM callback the latched listener queues at kDataLoaded
    // (PapyrusBridge.cpp NativeOps::Add, case j = kNativeSubs[j]). Natives
    // cannot be unregistered: resident, each reads the plugin latch first.
    inline constexpr std::array kNativeSubs{
        agh::rt::SubSpec{ "GetAPI", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetStatus", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "IsEffectivelyEnabled", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetRuntime", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetReleaseId", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetVersion", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetConfigGeneration", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetEnabled", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "SetEnabled", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetConfirmBeforeSell", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "SetConfirmBeforeSell", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetSellIngredients", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "SetSellIngredients", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetSellAnywhere", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "SetSellAnywhere", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetJunkValueOnly", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "SetJunkValueOnly", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetJunkMaxValue", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "SetJunkMaxValue", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetSellStolen", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "SetSellStolen", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetSellWeakWeapons", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "SetSellWeakWeapons", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetSellWeakArmor", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "SetSellWeakArmor", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetSellClothing", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "SetSellClothing", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetSellEnchantedLoot", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "SetSellEnchantedLoot", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetEnchantedLootMaxValue", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "SetEnchantedLootMaxValue", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetKnownEnchantmentsOnly", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "SetKnownEnchantmentsOnly", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetKeepCategory", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "SetKeepCategory", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetKeepLimit", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "SetKeepLimit", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetLastKept", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetAnywhereRatePercent", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetLastMerchantCheck", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetPerkPrice", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "SetPerkPrice", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "RequestRatePreview", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetRatePreview", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetLastPriceSource", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetHotkey", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "SetHotkey", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetMaxValue", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "SetMaxValue", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetMinWeight", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "SetMinWeight", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetMaxValueWeight", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "SetMaxValueWeight", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetMaxItems", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "SetMaxItems", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetProtectedNames", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "SetProtectedNames", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetProtectedForms", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "SetProtectedForms", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetLastSaveError", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "GetLastRunSummary", agh::rt::SubUndo::kResident },
        agh::rt::SubSpec{ "CheckAPI", agh::rt::SubUndo::kResident },
    };
    static_assert(kNativeSubs.size() == 62, "AGH_JS_Native API 9 has 62 natives");

    // Q4 proof (design 2.3, 6.2 pattern): RE::ScriptEventSourceHolder::GetSingleton,
    // RELOCATION_ID(14108, 14298) in CommonLib v9 (src/RE/S/ScriptEventSourceHolder.cpp),
    // which JS resolves and calls at kDataLoaded to register its container and
    // tracked-stats sinks (Probe.cpp TryRegister). 1.6.1170 and 1.7.104 are both
    // AE in v9, so the AE id 14298 is the one looked up on both. Offline pins
    // from AL v11/v13 and the 1.7.104 copy exe (the Address Library id table
    // row 14298; the 16 bytes: the development notes).
    // The proof runs off the first table row only; on that row nothing is emitted.
    inline constexpr std::uint64_t kProofSeId = 14108;
    inline constexpr std::uint64_t kProofId = 14298;
    inline constexpr std::array kProofPins{
        agh::rt::ProofPin{ agh::rt::RuntimeId::k1_6_1170, kProofId, 0x1D5230 },
        agh::rt::ProofPin{ agh::rt::RuntimeId::k1_7_104, kProofId, 0x1DA8F0 },
    };

    // The runtime strings of the js_startup row and of the Diagnostics
    // GetRuntime native, from the gate decision (never a literal). On
    // 1.6.1170 + SKSE 2.2.6 each is byte-identical to the v0.8.0 literal:
    // reason "RUNTIME_1.6.1170_SKSE_2.2.6", runtime "1.6.1170.0", skse "2.2.6.0".
    struct StartupLabels
    {
        std::string reason;
        std::string runtime;
        std::string skse;
        std::string alProof;  // the Q4 suffix: empty on 1.6.1170
    };
    [[nodiscard]] inline StartupLabels LabelsFor(const agh::rt::Decision& decision, const agh::rt::AlProof& proof)
    {
        StartupLabels out;
        out.reason = agh::rt::ReceiptToken(decision);
        out.runtime = agh::rt::RuntimeLabel4(decision);
        out.skse = agh::rt::SkseLabel4(decision);
        out.alProof = agh::rt::AlProofJson(decision, proof);
        return out;
    }

    // The GetRuntime text: "<release>; <runtime> + SKSE <skse>; receiptFallback=<bool>"
    // (v0.8.0: "w-probe-r69; 1.6.1170.0 + SKSE 2.2.6.0; receiptFallback=false").
    [[nodiscard]] inline std::string RuntimeNameText(std::string_view release, std::string_view runtime,
        std::string_view skse, bool fallback)
    {
        return std::string(release) + "; " + std::string(runtime) + " + SKSE " + std::string(skse) +
            "; receiptFallback=" + (fallback ? "true" : "false");
    }

    // Ops of RunInstall over kInstallPlan (InstallLedger.h Ops contract). Env
    // contract (Plugin.cpp: CommonLib; tests/js_e1_test.cpp: a fake):
    //   agh::rt::Decision Gate();                       skse::Evaluate, keeps its AL facts
    //   bool Init(); bool LoadAddressLibrary();         SKSE::Init(InitOptions()), IDDB now
    //   agh::rt::Reason ProveAlUse(agh::rt::AlProof&);  Q4, kOk without work on 1.6.1170
    //   bool LogDirectory();                            SKSE::log::log_directory()
    //   bool DiagnosticDirectory();                     js::SetDiagnosticDirectory
    //   bool TaskInterface(); bool MessagingInterface();
    //   bool LoadSettings();                            js::LoadSettings (strict base INI)
    //   bool ProcessRun();                              js::InitProcessRun
    //   bool OpenReceipt(const StartupLabels&);         js::OpenLog with the js_startup row
    //   bool InstallListener();                         RegisterListener(OnMessage) (resident)
    //   void LogRaw(const std::string&) noexcept;       one line through the open writer
    //   bool AppendRaw(const std::string&) noexcept;    one synchronous append, no writer
    // Every step either decides or does exactly one thing; nothing here catches
    // a step failure (RunInstall owns the one failure path).
    template <class Env>
    struct InstallCore
    {
        Env env{};
        agh::rt::Decision decision{};
        agh::rt::AlProof proof{};
        bool refused{};     // the gate or the Q4 proof refused: the row is GateRefusalJson
        bool writerOpen{};  // receipt_open ran: rows go through the writer

        agh::rt::StepResult Do(std::size_t i)
        {
            using agh::rt::StepResult;
            const std::string_view step = kInstallPlan[i].name;
            const auto ok = [](bool v) { return v ? StepResult::kOk : StepResult::kFail; };
            if (step == "gate") {
                decision = env.Gate();
                refused = !decision.Passed();
                return ok(!refused);
            }
            if (step == "skse_init") { return ok(env.Init()); }
            if (step == "al_load") { return ok(env.LoadAddressLibrary()); }
            if (step == "al_proof") {
                const agh::rt::Reason reason = env.ProveAlUse(proof);
                if (reason != agh::rt::Reason::kOk) {
                    decision.reason = reason;
                    refused = true;
                }
                return ok(!refused);
            }
            if (step == "log_dir") { return ok(env.LogDirectory()); }
            if (step == "diagnostic_dir") { return ok(env.DiagnosticDirectory()); }
            if (step == "task_interface") { return ok(env.TaskInterface()); }
            if (step == "settings") { return ok(env.LoadSettings()); }
            if (step == "messaging_interface") { return ok(env.MessagingInterface()); }
            if (step == "process_run") { return ok(env.ProcessRun()); }
            if (step == "receipt_open") {
                writerOpen = env.OpenReceipt(LabelsFor(decision, proof));
                return ok(writerOpen);
            }
            if (step == "listener") { return ok(env.InstallListener()); }
            return StepResult::kFail;  // a plan step without a binding: fail closed
        }
        // No plan step is reversible (the listener is resident).
        void Undo(std::size_t) noexcept {}
        void Receipt(std::string_view step, std::string_view reason, std::string_view sub) noexcept
        {
            try {
                const std::string json = refused ? agh::rt::GateRefusalJson(decision, kPluginName)
                                                 : agh::rt::LoadFailureJson(kPluginName, step, reason, sub);
                if (writerOpen) {
                    env.LogRaw(json);
                } else {
                    (void)env.AppendRaw(json);
                }
            } catch (...) {
                // Only an allocation failure lands here; no evidence can be written
                // (design 2.4, the one receipt-less case). The plugin is inert anyway.
            }
        }
        // No plan step is a compound (the sink and native compounds run later,
        // in the latched listener and the VM callback, and report through LateFail).
        [[nodiscard]] agh::rt::SubFailure FailedSub() const noexcept { return {}; }
    };

    // A failure after SKSEPlugin_Load returned (the sink compound in the
    // latched listener's task, the native batch in the VM callback): the Ops
    // of agh::rt::LateFail. Both act only while the latch is armed, i.e. after
    // every plan step ran and the writer is open.
    struct LateOps
    {
        void (*write)(const std::string&){};
        void Undo(std::size_t) noexcept {}
        void Receipt(std::string_view step, std::string_view reason, std::string_view sub) noexcept
        {
            try {
                if (write) { write(agh::rt::LoadFailureJson(kPluginName, step, reason, sub)); }
            } catch (...) {
            }
        }
    };

    // Disarm, one load_failure row naming the compound and its sub-call; a
    // second late failure disarms again and writes nothing (Latch::ClaimFailure).
    inline void LateFailure(std::string_view step, const agh::rt::CompoundOutcome& outcome, agh::rt::Latch& plugin,
        void (*write)(const std::string&)) noexcept
    {
        LateOps ops{ write };
        (void)agh::rt::LateFail(std::span<const agh::rt::StepSpec>{ kInstallPlan }, kInstallPlan.size(), ops, plugin,
            step, outcome.reason, outcome.failedSubName);
    }
}
