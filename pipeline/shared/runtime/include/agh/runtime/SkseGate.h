#pragma once
// agh/runtime/SkseGate.h -- CommonLib adapter for RuntimeGate.h (E1 design r3.1).
//
// Shipped shared header, moved from the design prototype
// (E1 design r3.1). CommonLibSSE-NG v9.1.0 only
// (vendor/commonlibsse-ng-v9.1.0); it cannot be host compiled for real, so
// every CommonLib name is cited to its v9 header line. A host syntax check
// against the real v9 PluginVersionData / InitInfo / REL::Version and
// line-cited stubs for the rest: tests/skse_gate_decl_check.py. ASCII only.
//
// Contract (design section 1.3):
// - Evaluate() touches only the LoadInterface accessors, REL::Module (Win32
//   file-version read, no Address Library) and std file IO. It never calls
//   SKSE::Init, REL::ID, REL::Relocation, IDDB or any RE:: symbol.
// - A refusal is returned, never thrown; the install ledger (InstallLedger.h)
//   writes the receipt row and SKSEPlugin_Load returns true (inert).

#include "agh/runtime/ImageRange.h"
#include "agh/runtime/InstallLedger.h"
#include "agh/runtime/RuntimeGate.h"

#include "SKSE/SKSE.h"  // v9 umbrella: SKSE/Impl/PCH.h (REL, REX::W32), API.h, Interfaces.h

#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <system_error>

// SKSEAPI. The pinned 800d tree defines it in SKSE/API.h:11 as
// __cdecl; CommonLibSSE-NG v9.1.0 defines it nowhere (grep of the whole v9
// tree). Every DLL declares `bool SKSEAPI SKSEPlugin_Load(...)`, so under v9
// the word is parsed as a declarator name and MSVC reports C2146, C2059,
// C2143 and C2447 at that line (the v9.1.0 survey, all 7 DLLs). The
// shared header keeps the one spelling working on both pins; __cdecl is the
// only calling convention on x64 anyway.
#ifndef SKSEAPI
#    define SKSEAPI __cdecl
#endif

namespace agh::rt::skse {

// Section 2 (Declarations): the runtime part of SKSEPlugin_Version, from
// kRuntimeTable only. Every DLL calls it inside its constinit initializer:
//   CompatibleVersions  {1.6.1170.0, 1.7.104.0}  (v9 Interfaces.h:581-589)
//   MinimumRequiredXSEVersion  MinimumSkseDeclared() = 2.2.6  (Interfaces.h:579)
//   UsesAddressLibrary()  no bool parameter in v9  (Interfaces.h:573; survey C2660)
//   UsesUpdatedStructs()  v9 name of UsesStructsPost629  (Interfaces.h:575;
//                         survey C2039). Never drop it: without bit 2 SKSE
//                         refuses the DLL on runtimes >= 1.6.629.
//   versionIndependenceEx keeps its v9 default kVersionIndependentEx_AddressLibraryV5
//                         (Interfaces.h:598); there is no setter.
// Every call is constexpr in v9, so the DLL's constinit initializer stays a
// constant expression (survey C2127 was the consequence of the two errors above).
// declareSkseMinimum = false is Mod Doctor's form: its runtime declares no SKSE
// minimum today and E1 keeps that (design section 2).
inline constexpr void DeclareRuntimes(SKSE::PluginVersionData& data, bool declareSkseMinimum = true) noexcept
{
    static_assert(kRuntimeTable.size() == 2, "a new runtime row needs its entry here and in the tests");
    data.CompatibleVersions({ REL::Version::unpack(kRuntimeTable[0].runtime),
        REL::Version::unpack(kRuntimeTable[1].runtime) });
    if (declareSkseMinimum) { data.MinimumRequiredXSEVersion(REL::Version::unpack(MinimumSkseDeclared())); }
    data.UsesAddressLibrary();
    data.UsesUpdatedStructs();
}

// SKSE::Init options for every DLL (ruling D2). v9.1.0 InitInfo defaults to
// log = true (include/SKSE/API.h:24-35), and API::InitLog() then opens a
// TRUNCATING basic_file_sink on "<My Games>/SKSE/<PluginName>.log" and makes
// it the spdlog default logger (src/SKSE/API.cpp:101-129). Two of our DLLs
// write exactly that file themselves: Soul Recharge "AGH_SoulRecharge_G0.log"
// (plugin name AGH_SoulRecharge_G0) and Mod Doctor "AGH_ModDoctor.log"
// (plugin name AGH_ModDoctor, ReceiptWriter_win.cpp:124,152-160). So logging
// stays off. trampoline = false keeps each DLL's own lazy AllocTrampoline
// (UC HookBudget.cpp:11-12, EU P6Patch.cpp:102, Shout CooldownCallsiteInstall.h:59).
inline SKSE::InitInfo InitOptions() noexcept
{
    SKSE::InitInfo info{};
    info.log = false;
    info.trampoline = false;
    return info;
}

// The CheckAlHeader() reader over the real file, relative to the process
// working directory (the game root), exactly like CommonLib's own open
// (include/REL/IDDB.h:274-280). Every call is noexcept; an IO error reads as
// "absent" or "short", which the gate refuses.
struct FileReader {
    std::filesystem::path path;
    [[nodiscard]] bool Exists() const noexcept
    {
        std::error_code ec;
        return std::filesystem::is_regular_file(path, ec) && !ec;
    }
    [[nodiscard]] bool Size(std::uint64_t& out) const noexcept
    {
        std::error_code ec;
        out = static_cast<std::uint64_t>(std::filesystem::file_size(path, ec));
        return !ec;
    }
    [[nodiscard]] bool ReadAt(std::uint64_t at, std::uint8_t* dst, std::size_t n) const noexcept
    {
        try {
            std::ifstream in(path, std::ios::binary);
            if (!in) { return false; }
            in.seekg(static_cast<std::streamoff>(at));
            if (!in) { return false; }
            in.read(reinterpret_cast<char*>(dst), static_cast<std::streamsize>(n));
            return static_cast<std::size_t>(in.gcount()) == n;
        } catch (...) {
            return false;
        }
    }
};

struct GateResult {
    Decision decision;
    AlFacts al;  // filled when the AL header was read
};

// The whole gate. Order: interface -> editor -> runtime row -> SKSE floor ->
// module version -> AL header. Nothing before this call may touch CommonLib.
[[nodiscard]] inline GateResult Evaluate(const SKSE::LoadInterface* a_skse) noexcept
{
    GateResult result;
    LoadFacts facts;
    facts.hasInterface = a_skse != nullptr;
    if (a_skse) {
        facts.isEditor = a_skse->IsEditor();                  // Interfaces.h:159
        facts.runtime = a_skse->RuntimeVersion().pack();      // Interfaces.h:160, Version.h:69-75
        facts.skse = a_skse->SKSEVersion();                   // Interfaces.h:155 (already packed)
    }
    result.decision = DecideLoad(facts);
    if (!result.decision.Passed()) { return result; }
    // REL::Module::get() reads SkyrimSE.exe's file version (include/REL/Module.h:330-352);
    // no Address Library access. The pinned CommonLib called it inside SKSE::Init anyway.
    result.decision = CheckModule(result.decision, REL::Module::get().version().pack());
    if (!result.decision.Passed()) { return result; }
    const FileReader file{ std::filesystem::path{ result.decision.row->alBin } };
    const Reason al = CheckAlHeader(*result.decision.row, file, &result.al);
    if (al != Reason::kOk) { result.decision.reason = al; }
    return result;
}

// After SKSE::Init on a pass: load the Address Library now, at a known point,
// as the pinned 800d61ee SKSE::Init did (vendor/commonlibsse-ng/src/SKSE/API.cpp:78-79).
// v9.1.0 loads it lazily on the first REL lookup (include/REL/IDDB.h:78-90);
// without this call a residual AL failure would surface mid-load at the first
// hook instead of at plugin load. IDDB::get() is public in v9.1.0.
inline void LoadAddressLibraryNow() { (void)REL::IDDB::get(); }

// Non-fatal id -> offset lookup for per-runtime feature rows (design section 4)
// and for the Q4 proof on a format 1/2 bin. REL::ID::offset() calls
// report_id_lookup_failure -> MessageBox + terminate when the id is absent
// (include/REL/ID.h:27, src/REL/IDDB.cpp:29-37,150-178). REL::Offset2ID
// (include/REL/Offset2ID.h:7-38, src/REL/Offset2ID.cpp:5-35) is the only public
// non-fatal view; it copies the whole map, so build it once per install pass
// and query it for every row.
[[nodiscard]] inline std::size_t FindIdOffset(const REL::Offset2ID& map, std::uint64_t id) noexcept
{
    for (const auto& m : map) {
        if (m.id == id) { return static_cast<std::size_t>(m.offset); }
    }
    return 0;
}

// r3: the running image for ImageRange.h. Query() is one
// VirtualQuery (REX::W32::VirtualQuery, v9 include/REX/W32/KERNEL32.h:506;
// constants KERNEL32.h:15-29) classified by ClassifyPage(); Copy() is a plain
// memcpy that ImageRange.h calls ONLY after RangeReadable() proved every page
// of the range committed and readable. No catch is relied on for memory
// safety: an access violation is SEH, and /EHsc catch (...) does not see it.
struct LiveImage {
    [[nodiscard]] PageInfo Query(std::uint64_t address) const noexcept
    {
        REX::W32::MEMORY_BASIC_INFORMATION info{};
        const bool ok = REX::W32::VirtualQuery(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)),
                            &info, sizeof(info)) == sizeof(info);
        return ClassifyPage(ok, reinterpret_cast<std::uintptr_t>(info.baseAddress), info.regionSize, info.state,
            info.protect);
    }
    void Copy(std::uint64_t address, std::uint8_t* dst, std::size_t n) const noexcept
    {
        std::memcpy(dst, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), n);
    }
};

// Q4 proof (design ruling Q4; design r2 ruling 3).
// Runs ONLY when EmitsAlProof(): on 1.6.1170 it returns kOk without doing
// anything, so nothing changes there. The id is the one the running runtime
// resolves: `rid.id()` of the same REL::RelocationID the DLL uses (SW:
// RELOCATION_ID(14108, 14298) of RE::ScriptEventSourceHolder::GetSingleton,
// v9 src/RE/S/ScriptEventSourceHolder.cpp:12; SE id first, AE id second,
// v9 include/SKSE/Impl/PCH.h:664 and include/REL/ID.h:35-100; 1.6.1170 and
// 1.7.104 are both Runtime::AE because minor >= 6, include/REL/Module.h:337-349,
// so the proof id is 14298 on both). The pure sequence is ProveAlUseCore():
//   1. non-fatal presence (format 5: the dense entry from the file; formats
//      1/2: one REL::Offset2ID snapshot); absent -> AL_ID_ABSENT;
//   2. the DLL's offline pin for this runtime, when one exists; a different
//      RVA -> AL_PIN_MISMATCH before any game memory is touched;
//   3. CommonLib agrees: REL::ID(id).offset() == that RVA (safe: present);
//   4. the loaded image's PE headers (page-checked reads), then the range
//      [rva, rva + 16) inside one readable + executable section, no
//      overflow, below SizeOfImage -> else AL_PROOF_RANGE;
//   5. the 16 bytes copied only after every page is proven readable.
// The live oracle records the bytes; comparing them with the 1.7.104 exe
// stays UNVERIFIED until a copy of that exe is available.
[[nodiscard]] inline Reason ProveAlUse(const GateResult& gate, const REL::RelocationID& rid,
    std::span<const ProofPin> pins, AlProof& proof) noexcept
{
    if (!EmitsAlProof(gate.decision)) { return Reason::kOk; }
    const std::uint64_t id = rid.id();
    if (id == 0) { return Reason::kAlIdAbsent; }
    const auto lookup = [&](std::uint64_t want) noexcept -> std::uint64_t {
        if (gate.al.format == 5) {
            const FileReader file{ std::filesystem::path{ gate.decision.row->alBin } };
            return LookupDenseOffset(file, gate.al, want);
        }
        try {  // a C++ allocation failure only; no memory-safety role
            const REL::Offset2ID snapshot;
            return FindIdOffset(snapshot, want);
        } catch (...) {
            return 0;
        }
    };
    const auto commonLib = [](std::uint64_t want) noexcept -> std::uint64_t {
        return static_cast<std::uint64_t>(REL::ID(want).offset());
    };
    const LiveImage image{};
    return ProveAlUseCore(gate.decision, gate.al.format, id, pins.data(), pins.size(), lookup, commonLib, image,
        static_cast<std::uint64_t>(REL::Module::get().base()), proof);
}

}  // namespace agh::rt::skse

// Usage in every DLL (the ShieldWatch pilot shown; design sections 1.6, 2 and 3.4):
//
// extern "C" __declspec(dllexport) constinit SKSE::PluginVersionData SKSEPlugin_Version = [] {
//     SKSE::PluginVersionData data;
//     data.PluginName("AGH_ShieldWatch");
//     data.PluginVersion(REL::Version{ 0, 0, 2, 0 });
//     data.AuthorName("AGH");
//     agh::rt::skse::DeclareRuntimes(data);   // section 2; replaces the 800d setter lines
//     return data;
// }();
// Mod Doctor: agh::rt::skse::DeclareRuntimes(data, false) (no SKSE minimum, as today).
//
// namespace sw {
// constinit agh::rt::Latch latch;            // OnMessage, the sinks and the drain task read latch.Armed() first
// constinit agh::rt::Latch sinksLive;        // r3: the compound's commit latch; armed only after all 4 sinks
// inline constexpr std::array kInstallPlan{ ... see pipeline/shared/runtime/tests/e1_plans.h kShieldWatch ... };
// inline constexpr std::array kSinkSubs{ ... e1_plans.h kSwSinks: hit, equip, container, magic_effect ... };
// inline constexpr std::array kProofPins{ agh::rt::ProofPin{ agh::rt::RuntimeId::k1_6_1170, 14298, 0x1D5230 },
//                                         agh::rt::ProofPin{ agh::rt::RuntimeId::k1_7_104, 14298, <AL id row> } };
// struct SinkOps {                           // RunCompound SubOps; NO try/catch here (the latch gate fails one)
//     RE::ScriptEventSourceHolder* holder;
//     bool Add(std::size_t j);               // j-th AddEventSink<T>(&sinks); returns true (AddEventSink is void)
//     void Remove(std::size_t j) noexcept;   // j-th RemoveEventSink<T>(&sinks); no-op when absent (BSTEvent.h:106-128)
// };
// struct InstallOps {
//     const SKSE::LoadInterface* skse; agh::rt::skse::GateResult gate; agh::rt::AlProof proof; bool writerOpen{ false };
//     agh::rt::CompoundOutcome sinks{};      // the last compound result, for Receipt()
//     agh::rt::StepResult Do(std::size_t i);          // one case per plan step, each atomic or a RunCompound
//     void Undo(std::size_t i) noexcept;              // "sinks": RemoveEventSink x4 (reverse), sinksLive.Disarm()
//     void Receipt(std::string_view step, std::string_view reason, std::string_view sub) noexcept;
//          // writerOpen ? sw::Log(failure row) : one synchronous append of GateRefusalJson / a
//          // load_failure row to <log_directory()>/AGH_ShieldWatch.jsonl (log_directory() is Win32-only)
//     agh::rt::SubFailure FailedSub() const noexcept; // {sinks.failedSubName, sinks.reason} after a failed compound
// };
// }
//
// Every sink body, the listener and the drain task start with the latch check (latch gate L1):
//   RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* e, RE::BSTEventSource<RE::TESHitEvent>*) override {
//       if (!sw::latch.Armed() || !sw::sinksLive.Armed()) return RE::BSEventNotifyControl::kContinue;
//       ...
//   void OnMessage(SKSE::MessagingInterface::Message* m) {
//       if (!sw::latch.Armed()) return;
//       ... retry: RunCompound(kSinkSubs, sinkOps, &sinksLive); a failure -> agh::rt::LateFail(kInstallPlan,
//           loadDone, ops, latch, "sinks", outcome.reason, outcome.failedSubName)
// A hook thunk (UC, EU, Shout) falls through to the original when disarmed (latch gate L1, thunk form):
//   static float Thunk(void* a, float b) { if (!uc::latch.Armed()) return g_original(a, b); ... }
//
// extern "C" __declspec(dllexport) bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* skse) noexcept {
//     sw::InstallOps ops{ skse };
//     return agh::rt::RunInstall(sw::kInstallPlan, ops, sw::latch).loadReturn;   // always true
// }
//
// Step "gate": ops.gate = Evaluate(skse); refused -> kFail (the receipt row is GateRefusalJson).
// Step "skse_init": SKSE::Init(skse, agh::rt::skse::InitOptions()).
// Step "al_load": LoadAddressLibraryNow().
// Step "al_proof": ProveAlUse(ops.gate, REL::RelocationID(14108, 14298), sw::kProofPins, ops.proof)
//   (1.6.1170: no-op, kOk; a refusal sets ops.gate.decision.reason and fails the step, so the row is
//   GateRefusalJson with AL_ID_ABSENT / AL_PIN_MISMATCH / AL_PROOF_RANGE).
// Step "sinks": ops.sinks = RunCompound(sw::kSinkSubs, sinkOps, &sw::sinksLive) when the holder exists
//   (kFail on failure, the sub-call named in the row); no holder yet = kOk, nothing added, listener retries.
// Step "startup_row": the existing row, plus AlProofJson(ops.gate.decision, ops.proof) before its
//   closing brace (empty on 1.6.1170, so that row keeps its bytes).
// Mod Doctor keeps its SEH wrapper: __except -> ops.FailFromSeh() = the same disarm + receipt path.
