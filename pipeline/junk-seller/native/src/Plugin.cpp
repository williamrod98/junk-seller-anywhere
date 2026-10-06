#include "Probe.h"
#include "Install.h"
#include "receipt/Log.h"
// JS v0.9.0: E1 shared runtime gate, one DLL for
// 1.6.1170 and 1.7.104 (the E1 design r4 sections 2 and
// 3; the SR v1.9.0 pattern, pipeline/soul-recharge/native/src/Plugin.cpp).
// SkseGate.h binds CommonLibSSE-NG v9.1.0 and defines SKSEAPI, which v9 no
// longer does (v9.1.0 survey errors C2146, C2059, C2143, C2447).
#include "agh/runtime/SkseGate.h"
#include <filesystem>
#include <optional>
#include <string>

// SKSE's exported SKSEPlugin_Version has the SKSEPluginVersionData layout
// (CommonLib's name for the exact structure is SKSE::PluginVersionData).
// Design 2.2: the runtime part of the declaration comes from the shared
// runtime table only (CompatibleVersions = both table rows, SKSE minimum = the
// lowest floor, UsesAddressLibrary(), UsesUpdatedStructs(), v9's default
// Address Library V5 bit). The v9 setters take no bool and UsesStructsPost629
// is UsesUpdatedStructs in v9 (v9.1.0 survey errors C2660, C2039). The plugin
// version stays 0.0.1.0: the MCM only tests GetPluginVersion > 0.
extern "C" __declspec(dllexport) constinit SKSE::PluginVersionData SKSEPlugin_Version = [] {
    SKSE::PluginVersionData data;
    data.PluginName("AGH_JunkSeller");
    data.PluginVersion(REL::Version{ 0, 0, 1, 0 });
    data.AuthorName("AGH");
    agh::rt::skse::DeclareRuntimes(data);
    return data;
}();

namespace js
{
    namespace
    {
        // The CommonLib environment of the install ledger (Install.h InstallCore
        // Env contract). Nothing here decides; each member does one thing and
        // reports whether it worked.
        struct LiveEnv
        {
            const SKSE::LoadInterface* skse{};
            agh::rt::skse::GateResult gate{};
            std::optional<std::filesystem::path> directory{};

            // Before a pass nothing may touch SKSE::Init, REL::ID/Relocation/IDDB
            // or any RE:: symbol (design 2.3); Evaluate() is the only call before it.
            agh::rt::Decision Gate() noexcept
            {
                gate = agh::rt::skse::Evaluate(skse);
                return gate.decision;
            }
            // log = false: CommonLib v9's default logger would open a truncating
            // "AGH_JunkSeller.log" at every load (R7); the product receipt is
            // AGH_JunkSeller.jsonl, written by receipt/Log.cpp.
            bool Init() noexcept
            {
                SKSE::Init(skse, agh::rt::skse::InitOptions());
                return true;
            }
            bool LoadAddressLibrary()
            {
                agh::rt::skse::LoadAddressLibraryNow();
                return true;
            }
            // Q4 (Install.h kProofId): RE::ScriptEventSourceHolder::GetSingleton,
            // the id CommonLib v9 resolves for the container / tracked-stats sinks.
            // On the first table row this returns kOk without any lookup or read.
            agh::rt::Reason ProveAlUse(agh::rt::AlProof& proof) noexcept
            {
                return agh::rt::skse::ProveAlUse(gate, REL::RelocationID(e1::kProofSeId, e1::kProofId),
                    std::span<const agh::rt::ProofPin>{ e1::kProofPins }, proof);
            }
            bool LogDirectory()
            {
                directory = SKSE::log::log_directory();
                return directory.has_value();
            }
            // Set first, so every later refusal in this body has somewhere durable
            // to be recorded even when the receipt itself never opens (w-probe-r14).
            bool DiagnosticDirectory()
            {
                if (!directory) { return false; }
                SetDiagnosticDirectory(*directory);
                return true;
            }
            bool TaskInterface() noexcept { return SKSE::GetTaskInterface() != nullptr; }
            bool LoadSettings() { return js::LoadSettings(); }
            bool MessagingInterface() noexcept { return SKSE::GetMessagingInterface() != nullptr; }
            // The process identity has to be set before the first row is written.
            bool ProcessRun()
            {
                InitProcessRun();
                return true;
            }
            // The js_startup row is OpenLog's durable first row (w-probe-r13).
            bool OpenReceipt(const e1::StartupLabels& labels)
            {
                if (!directory) { return false; }
                SetRuntimeLabels(labels.runtime.c_str(), labels.skse.c_str());
                Row row;
                row.type = "js_startup";
                row.origin = "startup";
                ConfigureStartupRow(row, labels);
                return OpenLog(*directory / ReceiptBasename, row);
            }
            bool InstallListener()
            {
                auto* messaging = SKSE::GetMessagingInterface();
                return messaging && messaging->RegisterListener(OnMessage);
            }
            void LogRaw(const std::string& json) noexcept { (void)LogRawLine(json); }
            // Before receipt_open the row is one synchronous append to the
            // receipt. SKSE::log::log_directory() is Win32-only and allowed before
            // the gate passes (design 2.3); the path is never copied into a row.
            bool AppendRaw(const std::string& json) noexcept
            {
                try {
                    const auto target = directory ? directory : SKSE::log::log_directory();
                    return target && AppendRawLine(*target / ReceiptBasename, json);
                } catch (...) {
                    return false;
                }
            }
        };

        struct InstallOps : e1::InstallCore<LiveEnv>
        {
            explicit InstallOps(const SKSE::LoadInterface* a_skse) noexcept { env.skse = a_skse; }
        };
    }
}

// Ruling D1 (design 2.4): the whole body is the install ledger. Any refusal,
// false step, throw or tail exception leaves the plugin inert (latch
// disarmed), writes one receipt row and returns true: never SKSE's "Exit
// game?" box because of this DLL.
extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* skse) noexcept
{
    js::InstallOps ops{ skse };
    return agh::rt::RunInstall(std::span<const agh::rt::StepSpec>{ js::e1::kInstallPlan }, ops, js::latch).loadReturn;
}
