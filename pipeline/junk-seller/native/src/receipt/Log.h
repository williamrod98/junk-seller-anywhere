#pragma once
// JSONL receipt contract. Engine free on purpose: every field is a scalar or a
// fixed byte array copied by the producer, so the offline harness can emit and
// diff the exact bytes the runtime would write.
#include "Names.h"
#include "../CandidateRows.h"
#include "../OracleVerdict.h"
#include "../FrameWindow.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <limits>
#include <memory>
#include <string>
namespace js {
// Receipt revision. The static gate pins this against the INI, the README and
// the design document so a schema change cannot ship unannounced.
// w-probe-r4 (W-probe design sections 4.1 and 10): schema stays 1 and every O
// field keeps its meaning; the added fields are told apart by this revision
// alone. r4 adds the read-only W identification rows and retains the prior
// argument evidence and per-session oracle visit budget.
// w-probe-r48 adds a VENC slot origin to rule-run balance selection.
// w-probe-r7 (W-4): post-load issuance and cached before-sample fallback.
// w-probe-r8 (W-5): the base count the issuance path needs is a dedicated
// entry lookup instead of a by-product of the gold walk, and the entry visit
// cap follows the INI VisitCap like the other two caps.
// w-probe-r9 (R9-1..R9-4): the issue gate's foreground term no longer
// requires an unpaused game (the BarterMenu pauses the game itself), and the
// failed-term names are recorded on the js_sale_attempt row.
// w-probe-r10 (R10-1..R10-4): the `topIsBarter` term is dropped from the
// foreground gate entirely (GetTopMostMenu returned null while the
// BarterMenu was the only and topmost menu, R-W5), and the js_sale_attempt
// row carries its own phase snapshot (topmostMenu/negativeTopmost/
// openMenus/paused) taken at gate time.
// w-probe-r11 (R11-1..R11-5): sale validation names adapter, quote, and
// quantity refusals separately; pre-dispatch refusals do not latch; and the
// attempt row carries the populated gate-time W phase snapshot.
// w-probe-r12 (R12-1/R12-2): the attempt deadline runs from the settle
// request and is long enough for the six-stage sample a paused BarterMenu
// needs; a deferred oracle request is coalesced into the queued sample instead
// of overwriting it, so the id a settle was handed always publishes. The
// attempt row states settleSampleState/settleElapsedMs and the js_settle row
// states coalescedInto.
// w-probe-r13 (R13-1): startup writes the first receipt row synchronously and
// every subsequent receipt row is flushed by the sink before it is counted as
// written. This makes a silent writer visible at plugin startup.
// w-probe-r14 (R14-1..R14-3): the startup sequence can no longer fail in
// silence. A receipt path that cannot be opened or written falls back to a per
// process file, every refusal and every swallowed exception is recorded in a
// separate startup note, and the module is only kept resident once the receipt
// is proven open.
// w-probe-r16 (R16-2): a second GFx observation row, `js_gfx_list`, reads
// every ItemList entry's rendered GFx members (baseId/eslId/count/infoValue/
// infoValueWeight/infoWeight/name) instead of only the selected one, so the
// list-wide price read can be compared against a manual sale. Additive only:
// `js_gfx_entry` keeps its r15 shape.
// w-probe-r17 (R17-3): each `js_gfx_list` entry adds the side/index members
// already proven on `js_gfx_entry` (filterFlag/filteredIndex/itemIndex/type/
// formType/equipState) and the display name now comes from the GFx object's
// `text` member (the r15 `str` map showed `text` holds the rendered item
// name; `name` was never a real member on the live object). Cap raised
// 128 -> 256; `entryCountTotal`/`truncated` keep their r16 meaning.
// w-probe-r18 (R18-4): a third GFx observation row, `js_gfx_root`, reads the
// BarterMenu root object's own members (not an ItemList entry) once at the
// snapshot key, right after `js_gfx_list`. Fail-closed to `rootPresent=false`
// when the root path is not an object. One nested level only for a member
// whose GFx type is object and whose name contains "ategory" or "List".
// w-probe-r20 (R21-4): the four GFx arrays (js_gfx_entry members, js_gfx_list
// entries, js_gfx_root members, js_gfx_root nested groups) moved off `Row`
// into a heap `GfxPayload`, `Log` takes `const Row&`, and `sizeof(Row)` is
// gated at 8192 bytes. This is a stack-overflow fix (R-W15 CTD,
// a live test log); no row kind, no
// JSON field name/order/value and no parser behaviour changed.
// w-probe-r22 (R23-3): the bind handler refreshes selectionFacts from
// its own read-back before a hand-off (R22-3 a), and js_sale_attempt carries
// the facts it compared (factsReadOk/factsBaseObj/factsMenuGeneration/
// factsLoadGeneration/factsInventoryGeneration/factsAgeMs/factsOwnerForm/
// factsOwnerReadable), appended after the existing fields. No existing field
// changed name, position or meaning.
// w-probe-r23 (R24-3 a): the hand-off gate refuses only on `factsCurrent`
// (facts_mismatch). The row cap and the visit cap no longer refuse; they are
// recorded on the product sale row as `factsRowCap` / `factsVisitCap` (R24-2: the
// facts are already published unconditionally before either cap is checked,
// so a cap stop was never a stale-facts condition and was over-refusing a
// long-running session).
// w-probe-r25 (R27-2 a/b, W-5 fix): the rule path's protection facts get a
// real ini-driven source (the required ProtectedNames/ProtectedForms keys,
// resolved once at load), Misc bases with no non-junk vendor keyword classify
// as junk so the rule path can issue at all, and the config block echoes both
// raw values plus the resolution state. The `js_rule_run`/`js_rule_candidate`
// rows, their existing fields and the event-pair confirmation are unchanged;
// w-probe-r32 (R37-1): rule-run rows split the run terminal reason from the
// per-attempt terminal list, and HEALTH funds counters have one rule-run path.
// w-probe-r34 (D-W08): the run row still carries candidatesSelectable from
// w-probe-r33, and the settings stop row adds its confirmation fields.
// w-probe-r59 (v0.1.0, the v0.1.0 design r3 section 4.4): the
// sell-anywhere press writes one js_convert_run row (full shape, a
// run-terminal row that may use the reserved slots) and at most 256 slim
// js_convert_row rows. No existing row changed.
// w-probe-r60 (v0.1.0 instance units, design addendum IU): js_convert_run
// gains notPlainKinds, notPlainOtherTypes, the instance counts and
// soldByCategory, and its categories object names junk (always on);
// js_convert_row gains plainUnits, instanceUnits, lists and category.
// w-probe-r61 (addendum IU.10): no row changed; a scale and a reference
// handle naming no reference became inert extras, so `reference` in
// notPlainKinds now counts live reference links only.
// w-probe-r62 (v0.2.0, the v0.2 update plan r2): js_convert_run's
// categories and soldByCategory name junk, ingredients, weakWeapons,
// weakArmor and clothing (the v0.1.0 food / misc / gear switches are
// retired); excluded gains builtin; new: junkRule, junkMaxValue, stolenRows,
// stolenUnits, stolenUnitsSold, rowsCleared, kept (the five "why kept"
// counters) and catalog (the kDataLoaded list sizes). js_convert_row gains
// stolenUnits. Every other row is unchanged.
// w-probe-r63 (v0.3.0, the v0.2 update plan r2 section 6 row v0.3.0):
// js_convert_run's categories and soldByCategory gain enchantedLoot; the run
// row gains knownEnchantmentsOnly and enchantedLootMaxValue, and catalog gains
// enchantLevels and artifactKeywords (the kDataLoaded sizes). kept.enchantUnknown
// is now live. Every other row is unchanged.
// w-probe-r64 (v0.4.0, the v0.2 update plan r2 section 3 and section 6 row
// v0.4.0, keep-N): js_convert_run's categories and soldByCategory gain
// alchemyIngredients, cookedFood, rawFood, drinks, ore, gems, pelts and tools;
// the run row gains keep (the eight keep per kind), partialRowsSold and
// partialUnitsSold, and catalog gains cookedForms, cookInputs, drinkForms,
// drinkKeywords and questFoods. speechXpCalls now counts one call per sold
// row. js_convert_row gains partial and keep. Every other row is unchanged.
// w-probe-r65 (v0.5.0, the v0.2 update plan r2 section 2.2 rows "Potions"
// and "Poisons", section 6 row v0.5.0): js_convert_run's categories,
// soldByCategory and keep gain restorePotions, skillPotions, otherPotions and
// poisons (twelve keep-limit categories; a pooled row's keep is its kind's
// share of the effect pool). New observe-only row js_frame_window (slim, only
// with the base INI key DevFrameWindow=1): the +5 s to +7 s task sampler of
// FrameWindow.h. Every other row is unchanged.
// w-probe-r66 (v0.5.1, fix build, least load): js_convert_run gains
// removeCalls (the engine removal calls of the press), goldCalls (its gold
// credits), stackRows / stackFallbackRows (rows removed with one call for
// the whole stack / whose stack call left units the per-list path removed)
// and containerEvents (the player's container-change events the sink
// counted during the press); js_convert_row gains removeCalls. Every other
// row is unchanged.
// w-probe-r67 (v0.6.0, the v0.2 update plan r2 section 6 row v0.6.0):
// js_convert_run's categories, soldByCategory and keep gain midGear, highGear
// and jewelry (fifteen keep-limit categories; mid / high enchanted loot counts
// under its tier). Every other row is unchanged.
// w-probe-r68 (v0.7.0): no receipt row changes; the material table names
// the Stormcloak officer, Blades and Penitus Oculatus sets (Convert.h).
// w-probe-r69 (v0.8.0, the barter-price design r2 section 6): js_convert_run
// and js_batch gain priceSource, the price snapshot of the press or batch
// (PerkPrice.h): source (perks / formula / fallback), reason, the switch, the
// walk counters, perkFactor, allPlayer, engineSelf, evalUs and the first 16
// walked entries. priceModel and shadow keep their shapes (priceModel's
// speech / f / m / mPrime now come from the snapshot's price). Every other
// row is unchanged.
// w-probe-r71 (v0.9.0: one DLL for 1.6.1170 and 1.7.104,
// the E1 design): js_startup takes its reason, runtime and
// skse from the shared runtime gate's labels (byte-identical on 1.6.1170 +
// SKSE 2.2.6) and, off 1.6.1170 only, carries the Q4 proof fields (alFormat,
// alProofId, alProofRva, alProofResolved, alProofRead, alProofBytes) before
// its closing brace. Two new line shapes come from the shared header, never
// from Row: runtime_gate (a refused setup) and load_failure (a failed start,
// the plugin inert). Every other row is unchanged.
constexpr const char* Revision = "w-probe-r71";
// W-probe design section 4.1: the observation build writes its own basename so
// an r5 log can never be renamed into evidence for this one. The DLL, the ESP
// and the INI keep their AGH_JunkSeller names.
constexpr const char* ReceiptBasename = "AGH_JunkSeller.jsonl";
// w-probe-r14 (R14-2): the startup note. One JSONL line per refused stage and
// per swallowed exception, written on its own stream so it survives a receipt
// that cannot be opened at all. It is never the receipt and never parsed as
// one; it exists so a dead startup has a durable trace.
constexpr const char* StartupNoteBasename = "AGH_JunkSeller.startup.jsonl";
// w-probe-r14 (R14-2): the stem the per process fallback receipt is built from,
// "<stem>.<processRunId>.jsonl", used only when the primary receipt refuses.
constexpr const char* FallbackReceiptStem = "AGH_JunkSeller";
constexpr unsigned MaxArgs = 8;
constexpr unsigned ArgTextBytes = 48;
constexpr unsigned TextBytes = 96;
// w-probe-r71: room for the Q4 suffix (about 210 bytes, RuntimeGate.h AlProofJson).
constexpr unsigned AlProofBytes = 256;
// heavy-fix (w-probe-r56): the receipt ring. One press can queue up to
// CandidateRowSummary::ExcludeRowCap (256) planning EXCLUDE rows and, on a
// same-task end (NO_CANDIDATES or a plan-time ERROR), up to BatchMaxRows (256)
// js_batch_row rows plus its js_batch and js_rule_run rows, all inside one UI
// task while the writer thread drains 64 rows per 10 ms. With 256 slots the
// heavy L1b r55 press filled the ring with its 256 EXCLUDE rows and its
// js_batch / js_rule_run rows were dropped. The ring now holds 512 rows, and
// the last LogTerminalReserveRows slots only take the run-terminal rows
// (LogRowUsesReserve), so a press can never lose its run row to a full ring.
constexpr unsigned LogRows = 512;
constexpr unsigned LogTerminalReserveRows = 8;
static_assert(LogTerminalReserveRows < LogRows, "the reserve must leave room for other rows");
// The run-terminal rows: the one js_batch and the one js_rule_run row of a
// press. Only these may use the reserved slots, and only these wait for the
// writer lock instead of being dropped on contention (Log in Log.cpp).
inline bool LogRowUsesReserve(const char* type) noexcept {
    if (!type) return false;
    constexpr const char* terminal[] = {"js_rule_run", "js_batch", "js_convert_run"};
    for (const char* name : terminal) {
        const char* a = type;
        const char* b = name;
        while (*a && *a == *b) { ++a; ++b; }
        if (*a == '\0' && *b == '\0') return true;
    }
    return false;
}
// How many queued rows a row of this kind may find and still be accepted.
inline unsigned LogRowAdmitLimit(bool reserved) noexcept {
    return reserved ? LogRows : LogRows - LogTerminalReserveRows;
}
constexpr std::uint64_t RotateBytes = 20ull * 1024 * 1024;
constexpr unsigned RetainedGenerations = 1;
inline std::int64_t MainTaskGapMs(std::uint64_t before, std::uint64_t after) noexcept {
    return before && after >= before ? static_cast<std::int64_t>(after - before) : -1;
}
constexpr float Missing = std::numeric_limits<float>::quiet_NaN();

// Thread kind of the producer, recorded instead of assumed. r4 / probe-js02
// anomaly 4: an SKSE UI task is not the same thread as an SKSE task, so a row
// produced on it used to read "other" and count as a wrong thread. The two
// expected threads are now told apart and only a third thread is unexpected.
// w-probe-r1 adds kLifecycle: the SKSE messaging callback is neither of the
// two task threads, and it is not an unexpected thread either. Without a name
// of its own it would have counted as wrongThreadCount, which would have
// changed what that r4 counter means.
enum class ThreadKind : unsigned {
    kMainTask = 0,
    kOther = 1,
    kUnknown = 2,
    kUiTask = 3,
    kLifecycle = 4
};

// Where a row was produced, as a measurement point and nothing more (W-probe
// design section 2.2). These are the places this code actually runs; none of
// them is a claim about a Flash internal phase.
enum class Phase : unsigned {
    kUnknown = 0,
    kHookCapture = 1,
    kHookPublish = 2,
    kMainTask = 3,
    kUiTask = 4,
    kMenuOpen = 5,
    kMenuClose = 6,
    kPreload = 7,
    kPostload = 8,
    kCount = 9
};
inline const char* PhaseName(Phase phase) noexcept {
    switch (phase) {
    case Phase::kHookCapture: return "hook_capture";
    case Phase::kHookPublish: return "hook_publish";
    case Phase::kMainTask: return "main_task";
    case Phase::kUiTask: return "ui_task";
    case Phase::kMenuOpen: return "menu_open";
    case Phase::kMenuClose: return "menu_close";
    case Phase::kPreload: return "preload";
    case Phase::kPostload: return "postload";
    default: return "unknown";
    }
}

// The prerequisites W still needs before a dispatch could even be discussed.
// A bit set here means "not observed", so the array the receipt prints is the
// list of things that are missing, never the list of things that are proven.
constexpr std::uint32_t kPrereqResponseIdSource = 1u << 0;
constexpr std::uint32_t kPrereqResponseLifetime = 1u << 1;
constexpr std::uint32_t kPrereqPreDispatchQuote = 1u << 2;
constexpr std::uint32_t kPrereqNullArgMeaning = 1u << 3;
constexpr std::uint32_t kPrereqInstanceBinding = 1u << 4;
constexpr std::uint32_t kPrereqForegroundGate = 1u << 5;
constexpr std::uint32_t kPrereqModalDetection = 1u << 6;
constexpr std::uint32_t kPrereqThreadPhase = 1u << 7;
constexpr std::uint32_t kPrereqFundsQuote = 1u << 8;
constexpr std::uint32_t kPrereqStableSample = 1u << 9;
constexpr unsigned PrerequisiteCount = 10;
inline const char* PrerequisiteName(unsigned index) noexcept {
    switch (index) {
    case 0: return "RESPONSE_ID_SOURCE";
    case 1: return "RESPONSE_LIFETIME";
    case 2: return "PRE_DISPATCH_QUOTE";
    case 3: return "NULL_ARG_MEANING";
    case 4: return "INSTANCE_BINDING";
    case 5: return "FOREGROUND_GATE";
    case 6: return "MODAL_DETECTION";
    case 7: return "THREAD_PHASE";
    case 8: return "FUNDS_QUOTE";
    case 9: return "STABLE_SAMPLE";
    default: return "UNKNOWN";
    }
}

// The W side of one observation, attached to the js_selected row it belongs to
// (design section 4.2). Every string is a fixed literal and every number is a
// value the probe read; nothing here is an instruction to anything.
struct WObservation {
    std::uint64_t observationId{};
    // The nearby manual trace this observation quotes, or -1 for "unknown",
    // which the receipt prints as null. It is evidence, never an argument to
    // send anywhere.
    std::int64_t argsEvidenceCallIndex{-1};
    const char* quoteSource{"none"};
    // Always null by contract (design section 2.3): no pre-dispatch quote path
    // has been proven, so there is nothing to name.
    const char* preDispatchQuoteSource{nullptr};
    const char* responseLifetime{"unknown"};
    const char* foregroundEvidence{"unknown"};
    const char* foregroundSource{"none"};
    const char* modalEvidence{"unknown"};
    const char* modalSource{"none"};
    std::int32_t proposedQuantity{-1};
    const char* quantitySource{"unknown"};
    // w-probe-r4 retains the R-F1 evidence: the fourth raw argument of the quoted manual trace,
    // as observed. arg3Type is the raw GFxValue type and -1 means no trace was
    // quoted; arg3Bool is the value when that type was a Boolean, -1 when it
    // was not read. Neither is given a meaning here: NULL_ARG_MEANING stays in
    // missingPrerequisites on every row.
    std::int64_t arg3Type{-1};
    std::int32_t arg3Bool{-1};
    const char* fundsCheck{"FUNDS_UNKNOWN"};
    const char* dispatchEligibility{"UNPROVEN"};
    std::uint32_t missingPrerequisites{};
    // The reads this build refused for want of a defensible phase.
    bool readPhaseUnproven{};
    const char* readPhaseReason{"none"};
};

// One Scaleform argument exactly as it arrived. Object and array arguments keep
// their type only; no engine call is made to walk them from a hook thread.
struct ArgValue {
    std::uint32_t type{0xFFFFFFFFu};
    double number{0.0};
    bool boolean{false};
    // w-probe-r4: false is also the default, so the receipt says whether the boolean
    // was read at all instead of printing a default as an observation.
    bool hasBool{false};
    bool hasNumber{false};
    bool hasText{false};
    std::array<char, ArgTextBytes> text{};
};

// Which oracle candidates a budget stop left unsampled (B-02).
constexpr std::uint32_t kUnsampledPlayerGold = 1u << 0;
constexpr std::uint32_t kUnsampledMerchantActor = 1u << 1;
constexpr std::uint32_t kUnsampledMerchantContainer = 1u << 2;
constexpr std::uint32_t kUnsampledMerchantInventory = 1u << 3;
constexpr std::uint32_t kUnsampledSpeech = 1u << 4;
// r5: the six stages a sample is built from, one per task (Stages.h). Slot 0 is
// the merchant resolve stage and slots 1-5 are the candidates in the order the
// design document lists them.
constexpr unsigned OracleStageSlots = 6;

// w-probe-r8 (design ruling R8-1): where a per-base count came from. The count the
// issuance path compares is read from the player's InventoryEntryData for the
// quoted base (countDelta) plus that object's rows in the reference's base
// TESContainer. No extra data list is enumerated for it, so a long inventory
// costs one pass over the entry list and nothing per stack.
//   entry_delta - the object was found in one of those two records
//   absent      - both records were readable and neither held the object, so
//                 the player owns none of it. That is a measurement of zero,
//                 never a truncation (R8-1).
//   none        - neither record could be read at all; the count stays -1.
constexpr const char* CountSourceEntryDelta = "entry_delta";
constexpr const char* CountSourceAbsent = "absent";
constexpr const char* CountSourceNone = "none";

// Candidate pay sources sampled together. B-02 forbids calling any one of them
// "the" merchant balance, so all four are recorded side by side, each as its own
// budgeted stage whose omission is stated rather than silently defaulted.
struct OracleSample {
    std::uint64_t sampleId{}, sampleNs{}, costUs{}, epoch{};
    // r5: costUs is the measured probe cost summed over the stages, spanUs is
    // the wall clock from the request to the publish (a sample built across six
    // tasks is more post-hoc than one built inline, and says by how much), and
    // prepareUs is what the merchant resolve stage cost on its own.
    std::uint64_t spanUs{}, prepareUs{};
    std::array<std::uint32_t, OracleStageSlots> stageCostUs{};
    std::array<std::uint32_t, OracleStageSlots> stageChunks{};
    std::uint32_t walkRestarts{};
    const char* walkAbandonReason{"none"};
    std::uint32_t merchantActorID{}, merchantBaseID{}, merchantContainerID{};
    // r5 / probe-js05 anomaly 3: the raw BarterMenu target handle and what it
    // resolved to, plus the two dialogue candidates recorded beside it. Nothing
    // here is adopted unless merchantSource says it was.
    std::uint32_t merchantHandle{}, handleRefID{}, handleBaseID{};
    std::uint32_t speakerHandle{}, speakerID{}, lastSpeakerHandle{}, lastSpeakerID{};
    const char* merchantSource{"none"};
    std::int32_t playerGold{-1}, merchantActorGold{-1}, merchantContainerGold{-1}, merchantInventoryGold{-1};
    std::int32_t playerItemCount{-1};
    // w-probe-r6 (W-3 ruling 3): the base a sale request named, counted in the
    // player and VENC walks next to gold. -1 = not measured.
    std::uint32_t targetBase{};
    std::int32_t playerBaseCount{-1}, vendorBaseCount{-1};
    // w-probe-r8 (R8-1): where each of those two counts came from, and what the
    // dedicated lookup cost. countSource names the player side count, the one
    // issue_before / issue_after compare; vendorCountSource names the VENC side.
    const char* countSource{CountSourceNone};
    const char* vendorCountSource{CountSourceNone};
    std::uint32_t baseCountVisits{};
    float speechLevel{Missing}, speechXp{Missing}, speechThreshold{Missing};
    std::uint32_t unsampled{}, scannedEntries{};
    // w-probe-r4 retains the count of entries the walk refused to dereference (null entry, null object, or
    // an object pointer VirtualQuery says is not readable). A non-zero count is
    // the receipt's way of saying the engine table was not trustworthy.
    std::uint32_t rejectedEntries{};
    // r5: walks that stopped because they had found gold (a completion, not a
    // truncation), the slots whose walk was cut by its deadline, and how many
    // stages actually ran.
    std::uint32_t earlyExits{}, truncatedStages{}, stagesRun{};
    bool merchantResolved{}, containerResolved{}, speechResolved{}, truncated{}, attached{};
    bool handleIsPlayer{}, fallbackUsed{};
    // w-probe-r1 (design section 4.2): the window the sample was built in. A
    // sample spans six tasks, so the generations and the event sequence are
    // recorded at the request and again at the publish; a change between them
    // invalidates the whole sample instead of producing a "simultaneous"
    // reading of two different moments.
    std::uint64_t startMenuGeneration{}, endMenuGeneration{};
    std::uint64_t startLoadGeneration{}, endLoadGeneration{};
    std::uint64_t startInventoryGeneration{}, endInventoryGeneration{};
    std::uint64_t firstEventSeq{}, lastEventSeq{};
    std::uint32_t startMerchantID{}, endMerchantID{};
    // Where each reading came from, so a reader never has to guess which code
    // path produced a balance (design section 8).
    const char* playerGoldSource{"none"};
    const char* merchantActorGoldSource{"none"};
    const char* merchantContainerGoldSource{"none"};
    const char* merchantInventorySource{"none"};
    const char* speechSource{"none"};
    // Entry visits this sample charged against the cumulative run cap.
    std::uint32_t entryVisits{};
    bool visitCapReached{};
    bool sampleValid{};
    const char* invalidationReason{"none"};
};

inline OracleVerdict SummarizeSample(const OracleSample& sample) noexcept {
    OracleVerdict out;
    out.lastSampleId = sample.sampleId;
    out.lastSampleValid = sample.sampleValid;
    out.lastInvalidReason = sample.invalidationReason;
    out.lastTargeted = sample.targetBase != 0;
    out.lastStageCostUs = sample.stageCostUs;
    out.lastChunks = sample.stageChunks;
    out.lastRestarts = sample.walkRestarts;
    out.lastScanned = sample.scannedEntries;
    out.lastBaseCountVisits = sample.baseCountVisits;
    out.lastTruncatedStages = sample.truncatedStages;
    out.lastUnsampled = sample.unsampled;
    out.lastSpanUs = sample.spanUs;
    return out;
}

// Whether every stage of a sample ran inside one unchanged window. Returns the
// reason a sample is not usable, or nullptr when it is clean. Engine free on
// purpose: the offline harness drives exactly this rule.
inline const char* OracleSampleInvalidReason(const OracleSample& sample) noexcept {
    if (!sample.attached) return "NOT_ATTACHED";
    if (sample.startLoadGeneration != sample.endLoadGeneration) return "LOAD_GENERATION_CHANGED";
    if (sample.startMenuGeneration != sample.endMenuGeneration) return "MENU_GENERATION_CHANGED";
    if (sample.startInventoryGeneration != sample.endInventoryGeneration) return "INVENTORY_CHANGED";
    if (sample.startMerchantID != sample.endMerchantID) return "MERCHANT_CHANGED";
    if (sample.visitCapReached) return "VISIT_CAP";
    if (sample.stagesRun != OracleStageSlots) return "STAGES_INCOMPLETE";
    if (sample.unsampled) return "UNSAMPLED_CANDIDATE";
    if (sample.truncated) return "TRUNCATED";
    if (!sample.merchantResolved) return "MERCHANT_UNRESOLVED";
    return nullptr;
}

// One budgeted oracle stage. Returns false when the deadline has already passed,
// in which case the candidate stays at its "not sampled" default and says so.
inline bool TakeStage(std::uint64_t nowNs, std::uint64_t deadlineNs, std::uint32_t bit,
                      OracleSample& sample) noexcept {
    if (deadlineNs && nowNs >= deadlineNs) {
        sample.unsampled |= bit;
        sample.truncated = true;
        return false;
    }
    return true;
}

// B-04: a record captured before a load must never be paired with a balance read
// after it. Stale records are still emitted, but without any oracle at all.
struct DrainDecision {
    bool stale{false};
    bool attachOracle{true};
    const char* reason{"OK"};
};
inline DrainDecision ClassifyRecord(std::uint64_t recordEpoch, std::uint64_t currentEpoch) noexcept {
    if (recordEpoch == currentEpoch) return {};
    return {true, false, "EPOCH_STALE"};
}

// Highlighted row as read through the standard ItemList only.
struct SelectedInfo {
    std::uint32_t listSize{}, baseObj{}, ownerForm{}, xListCount{};
    std::int32_t selectedIndexRead{-1}, countDelta{}, soulLevel{-1}, value{-1};
    bool indexRead{}, questObject{}, favorite{}, worn{}, enchanted{}, canBeTaken{}, owned{};
    bool ownerReadable{}, ownerIsPlayer{};
    // r4 identification only.  ExtraUniqueID is copied while the selected
    // InventoryEntryData is live; it is never retained across a UI task.
    bool extraDataListPresent{}, extraUniqueIdPresent{}, stolenKnown{};
    std::uint32_t extraUniqueId{}, mixedExtraListStacks{};
    std::array<char, TextBytes> displayName{};
    // w-probe-r1 (design section 4.2): the identity of the highlighted row and
    // the moment it was read are two different things and are kept apart. The
    // sample bounds are the read itself, not the surrounding task.
    std::uint64_t observationId{}, sampleStartNs{}, sampleEndNs{};
    // The index was read again after the entry: a change in between means the
    // two halves of this row describe two different selections.
    std::int32_t selectedIndexAfter{-1};
    bool selectionValid{};
    const char* source{"none"};
    const char* indexSource{"none"};
    // "unextended" / "extended" / "unknown", derived from the xList count that
    // was already read. It is a shape, never an individual item's identity.
    const char* instanceKind{"unknown"};
    // "base_only" / "base_and_uid" / "unknown". A uniqueID read would need a
    // phase this build cannot defend, so it stays base_only at best.
    const char* identityQuality{"unknown"};
    // Entries the read walked, charged to the cumulative run cap. When the cap
    // stopped the walk the count is known-incomplete and says so, instead of
    // under-reporting as if the stack had fewer extra lists.
    std::uint32_t visits{};
    bool visitCapReached{}, xListTruncated{};
};

// r4 rows are deliberately separate from js_selected and js_callback_trace:
// their names make the three W exit-condition evidence streams joinable.
struct WPhaseInfo {
    bool capture{}, mainUiThread{}, barterOpen{}, uiMoviePresent{}, itemSelectPresent{};
    bool handlerUnchanged{}, firstObservation{}, negativeTopmost{}, modalOpen{}, gamePaused{};
    std::uint64_t firstUiTaskThread{}, currentThread{}, handlerPointer{}, previousHandlerPointer{};
    unsigned reentryDepth{};
    std::array<char, TextBytes> topmostMenu{}, openMenus{};
};
struct WArgsInfo {
    std::uint64_t responseId{};
    bool responseMatchesPrevious{}, responseKnown{}, unitPriceKnown{};
    unsigned handlerArgCount{};
    double unitPrice{};
    const char* unitPriceSource{"PUBLIC_PATH_UNAVAILABLE"};
};

// Inventory cost split demanded by B-06, one stage per task (B-01).
struct PerfInfo {
    const char* stage{"none"};
    std::uint64_t prepareUs{}, formScanUs{}, xListScanUs{}, totalUs{}, budgetUs{}, estimateUs{};
    std::uint32_t entryCount{}, formCount{}, xListCount{};
    bool truncated{}, prechecked{}, latched{};
    // w-probe-r1 (design section 4.2): the added reads are metered apart from
    // the three inventory stages, and the run's cumulative visits travel with
    // the row so a repeated read is visibly charged.
    std::uint64_t selectionReadUs{}, protectionUs{};
    std::uint32_t selectionVisits{}, protectionVisits{};
    std::uint64_t entryVisitsTotal{}, xListVisitsTotal{}, oracleVisitsTotal{};
    std::uint64_t visitAllowanceShort{};
    // w-probe-r4 retains the oracle visits charged to the menu session this row belongs to.
    // oracleVisitsTotal stays the process wide total, so the two together show
    // both that the session budget reset and that the valve did not move.
    std::uint64_t oracleSessionVisits{};
    bool visitCapReached{};
};

// The unimplemented W stages. They are named in every js_perf row as
// notMeasured, because a stage that does not exist must never read as a stage
// that cost 0 us (design section 4.2).
constexpr unsigned NotMeasuredCount = 4;
inline const char* NotMeasuredName(unsigned index) noexcept {
    switch (index) {
    case 0: return "selector";
    case 1: return "quote";
    case 2: return "binding";
    case 3: return "ui_update";
    default: return "unknown";
    }
}

// Receipt completeness (design section 4.3). generated = every row a producer
// made, accepted = rows the bounded queue took, delivered = rows the writer
// put in the file, pending = accepted rows not yet written.
struct Delivery {
    std::uint64_t generated{}, accepted{}, dropped{}, delivered{}, pending{}, fileDropped{};
};
inline bool DeliveryConsistent(const Delivery& d) noexcept {
    if (d.generated != d.accepted + d.dropped) return false;
    if (d.accepted < d.delivered) return false;
    // The second identity only holds while the file writer has not failed.
    if (!d.fileDropped && d.accepted != d.delivered + d.pending) return false;
    return true;
}

// The tail rule the snapshot parser and this harness share (design section 5):
// a receipt line counts only once its newline is there. A snapshot taken while
// the writer is mid-line ends in a partial line that is counted apart and
// completed by the re-collection after the process exits.
inline std::size_t CompleteLineCount(const char* text, std::size_t length) noexcept {
    std::size_t lines = 0;
    for (std::size_t i = 0; i < length; ++i) {
        if (text[i] == '\n') ++lines;
    }
    return lines;
}
inline bool TailIncomplete(const char* text, std::size_t length) noexcept {
    return length != 0 && text[length - 1] != '\n';
}

// w-probe-r20 (R21-4): the GFx observation payload. This used to be four
// std::array members inline inside struct Row (js_gfx_entry's gfxMembers,
// js_gfx_list's gfxListEntries, js_gfx_root's gfxRootMembers and
// gfxRootNested), which made one Row several hundred KB. Log(Row) taking Row
// by value and NewRow returning Row by value then stacked several such
// frames on the SKSE UI task thread until __chkstk walked past the guard
// page (a live test log). The
// payload now lives on the heap, reached only through Row::gfx, and is
// non-null only for js_gfx_entry / js_gfx_list / js_gfx_root rows. The JSON
// field names, order and values it produces are unchanged.
struct GfxMember {
    std::array<char, 65> name{};
    std::uint32_t gfxType{};
    double number{};
    std::array<char, 65> text{};
    unsigned valueKind{};
};
// w-probe-r16/r17 (R16-2/R17-3): one `js_gfx_list` entry.
struct GfxListEntry {
    std::int32_t index{};
    std::uint32_t baseId{}; bool baseIdKnown{};
    double eslId{}; bool eslIdKnown{};
    double count{}; bool countKnown{};
    double infoValue{}; bool infoValueKnown{};
    double infoValueWeight{}; bool infoValueWeightKnown{};
    double infoWeight{}; bool infoWeightKnown{};
    // w-probe-r17 (R17-3): the same numeric members already proven on
    // js_gfx_entry (a live test log, rw12), read the same
    // GetMember-only way. A missing or non-numeric member stays unknown.
    double filterFlag{}; bool filterFlagKnown{};
    double filteredIndex{}; bool filteredIndexKnown{};
    double itemIndex{}; bool itemIndexKnown{};
    double type{}; bool typeKnown{};
    double formType{}; bool formTypeKnown{};
    double equipState{}; bool equipStateKnown{};
    std::array<char, 65> name{}; bool nameKnown{};
};
// w-probe-r18 (R18-4): one nested group under a `js_gfx_root` member (a root
// member whose GFx type is object and whose name contains "ategory" or
// "List"). Same GfxMember shape and 64-member cap as the root's own members,
// its own truncated flag, never recurses further.
struct GfxRootNested {
    std::array<char, 65> parentName{};
    std::array<GfxMember, 64> members{};
    unsigned memberCount{}, memberCountTotal{};
    bool truncated{};
};
struct GfxPayload {
    static constexpr unsigned MaxGfxMembers = 64;
    std::array<GfxMember, MaxGfxMembers> gfxMembers{};
    unsigned gfxMemberCount{}, gfxMemberCountTotal{};
    bool gfxTruncated{};

    static constexpr unsigned MaxGfxListEntries = 256;
    std::array<GfxListEntry, MaxGfxListEntries> gfxListEntries{};
    unsigned gfxListEntryCount{}, gfxListEntryCountTotal{};
    bool gfxListTruncated{};

    static constexpr unsigned MaxGfxRootMembers = 64;
    std::array<GfxMember, MaxGfxRootMembers> gfxRootMembers{};
    unsigned gfxRootMemberCount{}, gfxRootMemberCountTotal{};
    bool gfxRootTruncated{};

    static constexpr unsigned MaxGfxRootNested = 8;
    std::array<GfxRootNested, MaxGfxRootNested> gfxRootNested{};
    unsigned gfxRootNestedCount{};
};

// stage S2c (the r2 design sections 2.10 / 2.11 / 3.6): the
// one js_batch row every press writes and the slim js_batch_row rows (one per
// planned row, at most 256) of a batch. Plain scalars and literal pointers
// only, so the host harness emits the exact bytes the runtime writes. Names
// that the shared row block already prints (reason, beforeSampleId,
// afterSampleId) are carried there, never twice.
struct BatchReceipt {
    const char* outcome{"none"};
    // BatchStopName of the pure layer, or "none".
    const char* stop{"none"};
    // A stop the engine side decided outside the commit loop
    // (BEFORE_UNAVAILABLE, LIST_CHANGED, PLAN_INVALID, BATCH_TIMEOUT, the
    // cancel reasons), or "none".
    const char* abort{"none"};
    const char* eventMode{"unknown"};
    const char* goldSource{"none"};
    const char* surface{"none"};
    std::int64_t unitsEligible{}, unitsPlanned{}, unitsSold{};
    std::int64_t goldPlanned{}, goldDispatched{}, goldConfirmed{}, goldReceived{};
    std::int64_t playerGoldDelta{}, vencGoldDelta{};
    std::uint32_t rowsPlanned{}, rowsDispatched{}, rowsConfirmed{}, rowsUnconfirmed{}, rowsDeferred{};
    std::uint32_t commitFrames{}, pairInCallRows{}, listRebuiltInCallRows{}, listRemaps{};
    std::uint32_t lateEvents{}, lateDropped{}, slimRows{}, unknownPlaceholders{};
    // bindfix: rows bound across the batch's own update-pending flag,
    // and the first bind refusal as a static token ("none" if none).
    std::uint32_t pendingBypassRows{};
    const char* bindFail{"none"};
    // bindfix2: binds repeated after a read-back the list UI moved.
    std::uint32_t bindRewrites{};
    // r57: rows decided on more than one item or gold event, and the
    // first container event of the batch that was attributed to no row
    // (foreignKnown false: none; the other fields are then 0).
    std::uint32_t splitEventRows{};
    bool foreignKnown{};
    std::uint32_t foreignOld{}, foreignNew{}, foreignBase{};
    std::int32_t foreignCount{};
    // r58: the per-press row cap on this list and the selected rows it
    // cut from the plan (0 when it cut none).
    std::uint32_t pressRowCap{}, rowsCapped{};
    std::uint64_t commitTaskUsMax{}, commitTaskUsTotal{}, edgeToLineMs{};
    std::int64_t postCommitGapMs{-1};
    bool latched{}, balanceLimited{}, runLimited{}, balanceUnknown{};
    bool samplesValid{}, reconciled{}, reconcileOk{}, textFallback{};
    // Receipt-only shadow quote inputs (design-r2 3.6).
    bool shadowKnown{}, shadowMerchantPerk{};
    double shadowMultiplier{};
    std::int32_t shadowHagglingRank{-1};
    float shadowSpeech{Missing}, shadowAv107{Missing}, shadowAv146{Missing};
    float shadowBarterMin{Missing}, shadowBarterMax{Missing};
};
struct BatchRowReceipt {
    std::uint32_t token{}, baseObj{}, listIndex{};
    std::int32_t stackCount{}, quantity{}, unitPrice{}, baseValue{};
    std::int64_t total{}, shadowPrice{-1};
    const char* state{"planned"};
    bool pairInCall{}, listRebuiltInCall{}, merchantPerk{};
    // r57: the in-call (or late) event counts the row was decided on;
    // 0 for a row never decided.
    std::uint32_t itemEvents{}, goldEvents{}, foreignEvents{};
};

// v0.1.0 (design r3 section 4.4): the one js_convert_run row of a
// sell-anywhere press and its slim js_convert_row rows (one per planned row,
// at most 256). Plain scalars and literal pointers only, like BatchReceipt.
struct ConvertReceipt {
    const char* outcome{"none"};
    const char* gate{"OK"};
    const char* stop{"none"};
    const char* surface{"none"};
    std::int64_t unitsEligible{}, unitsPlanned{}, unitsSold{};
    std::int64_t goldPlanned{}, goldPaid{}, goldBefore{-1}, goldAfter{-1};
    std::uint32_t entriesRead{}, rowsEligible{}, rowsPlanned{}, rowsSold{}, rowsRefused{};
    std::uint32_t rowsRolledBack{}, rowsMismatch{}, rowsLost{}, slimRows{};
    std::uint32_t exType{}, exCategoryOff{}, exRule{}, exAbsolute{}, exOwner{}, exInvalid{};
    std::uint32_t exNotPlain{}, exZeroGold{}, exRunLimit{};
    std::uint64_t xpCalls{};
    std::int32_t speechLevelBefore{-1}, speechLevelAfter{-1};
    float speechXpBefore{Missing}, speechXpAfter{Missing};
    bool priceKnown{}, clamped{};
    std::int32_t hagglingRank{-1}, ratePercent{-1};
    float speech{Missing}, factor{Missing}, multiplier{Missing}, mPrime{Missing};
    float av107{Missing}, av146{Missing}, barterMin{Missing}, barterMax{Missing};
    std::uint64_t readUs{}, readCountsUs{}, sliceMaxUs{}, sliceTotalUs{}, edgeToLineMs{};
    std::uint32_t slices{};
    bool latched{}, runLimited{}, textFallback{};
    std::uint32_t unknownPlaceholders{};
    // SellIngredients of the run's snapshot; v0.2.0: the Categories page
    // (the v0.1.0 SellFood / SellMisc / SellGear are retired).
    bool sellIngredients{};
    bool junkValueOnly{}, sellStolen{}, sellWeakWeapons{}, sellWeakArmor{}, sellClothing{};
    std::int32_t junkMaxValue{50};
    // v0.3.0: weak enchanted loot of the run's snapshot, and the catalog
    // sizes of its facts.
    bool sellEnchantedLoot{}, knownEnchantmentsOnly{true};
    std::int32_t enchantedLootMaxValue{300};
    std::uint32_t soldEnchantedLoot{}, catalogEnchantLevels{}, catalogArtifactKeywords{};
    // v0.4.0: the keep-limit categories of the run's snapshot (the
    // Settings.h SettingsKeepCategories order), the rows sold per category,
    // the keep-N rows that left units behind, and the catalog sizes of the
    // food facts.
    // v0.5.0: twelve (the four potion and poison categories appended).
    // v0.6.0: fifteen (mid-tier gear, high-tier gear, jewelry appended).
    std::array<bool, 15> keepOn{};
    std::array<std::int32_t, 15> keepLimit{};
    std::array<std::uint32_t, 15> soldKeep{};
    std::uint32_t partialRowsSold{};
    std::int64_t partialUnitsSold{};
    std::uint32_t catalogCookedForms{}, catalogCookInputs{}, catalogDrinkForms{}, catalogDrinkKeywords{};
    std::uint32_t catalogQuestFoods{};
    // v0.2.0: built-in never-sell rows, the five "why kept" counters,
    // the stolen part of the plan and of the sale, the rows the press
    // emptied, and the catalog sizes built at kDataLoaded.
    std::uint32_t exBuiltin{};
    std::uint32_t keptAtLimit{}, keptProtected{}, keptStolen{}, keptEnchantUnknown{}, keptCategoryOff{};
    std::uint32_t stolenRows{}, rowsCleared{};
    std::int64_t stolenUnits{}, stolenUnitsSold{};
    std::uint32_t catalogGuardGear{}, catalogGuardNpcs{}, catalogGuardSourcesMissing{}, catalogUniques{};
    std::uint32_t catalogCarpenterInputs{}, catalogToolForms{}, catalogNeverForms{};
    // w-probe-r60 (design addendum IU): the rows sold per category (junk is
    // always on; the categories object names it so it never reads all false
    // while junk rows sell), the instance units of the plan, and the kinds
    // of the kept lists that made rows notPlain (order: Convert.h
    // ConvertKeptKindName) with the first raw types of "other" extras.
    std::uint32_t soldJunk{}, soldIngredients{}, soldWeakWeapons{}, soldWeakArmor{}, soldClothing{};
    std::uint32_t instanceRows{}, instanceRowsSold{};
    std::int64_t instanceUnits{}, instanceLists{}, instanceUnitsSold{};
    std::array<std::uint32_t, 9> keptKinds{};
    std::array<std::uint32_t, 8> otherTypes{};
    std::uint32_t otherTypeCount{};
    // Audit B-03 (a review): the first 16 notPlain rows as
    // {baseObj, kept-kind bits, first "other" raw type}.
    std::array<std::array<std::uint32_t, 3>, 16> keptRows{};
    std::uint32_t keptRowCount{};
    // v0.5.1: the removal and gold calls, the stack rows and the
    // container events of the press.
    std::uint64_t removeCalls{};
    std::uint32_t goldCalls{}, stackRows{}, stackFallbackRows{}, containerEvents{};
};
// v0.8.0 (w-probe-r69): the price snapshot of one press (js_convert_run)
// or one in-menu batch (js_batch). Plain scalars and literal pointers only.
inline constexpr std::size_t PriceSourceEntryRows = 16;
struct PriceEntryReceipt {
    std::uint32_t perk{};
    std::uint8_t function{};
    std::int8_t dataType{-1};
    bool merchantSide{}, passed{}, supported{};
    float value0{Missing}, value1{Missing}, avRead{Missing}, term{Missing};
};
struct PriceSourceReceipt {
    bool present{};
    const char* source{"formula"};
    const char* reason{"off"};
    bool switchOn{}, walked{}, tableOk{}, overflow{}, engineCalled{};
    std::uint32_t visits{}, collected{}, dropped{};
    std::uint32_t playerSidePassing{}, merchantSidePassing{}, unsupportedPassing{};
    float perkFactor{Missing}, allPlayer{Missing}, engineSelf{Missing};
    std::uint64_t evalUs{};
    std::uint32_t entryRows{};
    std::array<PriceEntryReceipt, PriceSourceEntryRows> entries{};
};
struct ConvertRowReceipt {
    std::uint32_t token{}, baseObj{}, slice{}, xpCalls{};
    std::int32_t value{}, price{}, units{}, countBefore{}, countAfter{-1};
    // w-probe-r60: the row's plain and instance units, its instance lists and
    // its category ("junk", "ingredients", "food", "misc", "gear").
    std::int32_t plainUnits{}, instanceUnits{}, lists{};
    // v0.2.0: the units of the row's stolen lists (part of instanceUnits).
    std::int32_t stolenUnits{};
    // v0.4.0: a keep-N row that leaves units behind, and its keep.
    bool partial{};
    std::int32_t keep{};
    // v0.5.1: the engine removal calls of the row.
    std::uint32_t removeCalls{};
    const char* category{"junk"};
    const char* state{"PLANNED"};
};

struct Row {
    const char* type{"js_warning"};
    const char* reason{"OBSERVE_ONLY"};
    const char* origin{"none"};
    std::uint64_t seq{}, ms{}, epoch{}, monotonicNs{}, elapsedUs{};
    ThreadKind thread{ThreadKind::kUnknown};
    // Registry rows.
    std::array<char, NameBytes> name{};
    std::uint64_t moduleRelOffset{}, handlerRelOffset{}, acceptOffset{}, hashOffset{};
    unsigned callbackCount{}, hashCount{}, onlyInAccept{}, onlyInHash{};
    unsigned offsetDifferCount{}, handlerDifferCount{};
    bool handlerIsMenu{}, presentInHash{}, fromAccept{}, offsetDiffers{}, handlerDiffers{};
    // Call trace rows.
    unsigned argCount{}, capturedArgs{};
    std::array<ArgValue, MaxArgs> args{};
    std::uint64_t callIndex{}, originalNs{}, delegateRelOffset{};
    bool barterOpen{}, delegateMatched{};
    // Container and stat rows.
    std::uint32_t oldContainer{}, newContainer{}, baseObj{}, referenceID{}, uniqueID{};
    std::int32_t itemCount{}, statValue{};
    unsigned batchIndex{}, batchSize{}, attempt{};
    std::uint64_t attemptToken{};
    const char* responseIdSource{"none"};
    const char* responseValueSource{"none"};
    unsigned dispatchCount{};
    const char* terminalReason{"none"};
    const char* unitPriceSource{"none"};
    const char* refusalTerm{"none"};
    std::int32_t quantity{}, count{}, merchantGold{};
    double price{};
    // w-probe-r9 (R9-3): failed foreground-gate term names, comma joined,
    // empty when the gate passed.
    std::array<char, TextBytes> foregroundTerms{};
    // w-probe-r10 (R10-2/R10-3): the attempt's own phase snapshot at gate
    // time, same encoding as WPhaseInfo's topmostMenu/negativeTopmost/
    // openMenus/gamePaused. `topmostMenu` is still recorded but never gates.
    std::array<char, TextBytes> topmostMenu{}, openMenus{};
    bool negativeTopmost{}, paused{};
    const char* beforeSource{"none"};
    bool itemEvent{}, goldEvent{};
    std::array<std::int32_t, 4> balanceDeltas{};
    std::array<std::int32_t, 2> inventoryDeltas{};
    bool balanceDeltasValid{};
    const char* deltaInvalidReason{"NOT_SETTLED"};
    std::uint64_t beforeSampleId{}, afterSampleId{}, settleSampleId{};
    // w-probe-r12 (R12-1): what happened to the sample the settle asked for
    // ("none"/"pending"/"published"/"lost") and how long the attempt waited,
    // measured from the settle request when there was one.
    const char* settleSampleState{"none"};
    std::uint64_t settleElapsedMs{};
    std::int64_t mainTaskGapMs{-1};
    // w-probe-r12 (R12-2): the already queued sample this request was folded
    // into, or 0 when the request got a sample of its own.
    std::uint64_t coalescedInto{};
    std::uint64_t itemEventAttemptToken{}, goldEventAttemptToken{};
    std::uint64_t itemEventSeq{}, goldEventSeq{};
    std::int32_t itemEventCount{}, goldEventCount{};
    // w-probe-r24 fix round 3 (A-03): same-shaped events rejected for
    // arriving after AttemptDeadlineMs (never a pair member, never a gate).
    std::uint64_t lateEvents{};
    // w-probe-r24 fix round 5 (A-01): container events this attempt lost
    // before they could be examined (row cap, full ring, or an
    // observed/handled gap). Non-zero can never be CONFIRMED.
    std::uint64_t eventsLost{};
    std::int32_t attemptQuantity{}, attemptUnitPrice{};
    std::uint32_t goldOldContainer{}, goldNewContainer{}, goldBaseObj{};
    std::uint32_t quoteBaseObj{};
    const char* protectionRule{"none"};
    // w-probe-r6 (W-3 rulings 1-3): the measured before/after pairs
    // {playerBase, vendorBase, playerGold, vendorGold}, whether both were
    // measured, and the age of the key-edge request when it was decided.
    std::array<std::int32_t, 4> measuredBefore{-1, -1, -1, -1};
    std::array<std::int32_t, 4> measuredAfter{-1, -1, -1, -1};
    bool measuredPairsValid{};
    std::uint64_t requestAgeMs{};
    // Papyrus rows (r4): the three values a ModCallbackEvent carries besides the
    // event name, so "the quest ran" is readable without the Papyrus log.
    double numArg{};
    std::uint32_t senderFormID{};
    // r5: the oracle sample this row belongs to. The sample is built by its own
    // stages and written on its own js_oracle row, so a row that asked for one
    // states the id instead of carrying a half filled copy. Zero means no
    // sample was requested for this row.
    std::uint64_t oracleRequestId{};
    // A request placed while another oracle job owned the single slot. This
    // lets the receipt tie a missing settle or close sample to that exact
    // deferred request instead of inferring it from the process-wide count.
    bool oracleDeferredRequest{};
    bool modEventSource{}, nameCounters{};
    std::array<char, TextBytes> text{};
    OracleSample oracle{};
    SelectedInfo selected{};
    PerfInfo perf{};
    // Product configuration contract: the two user-settings lists and the
    // protection resolution facts are present on every row.
    bool bindReadbackMatch{};
    unsigned bindWrites{};
    // w-probe-r25 (R27-2 a): both protection keys as the user settings spell them,
    // plus the resolution state ("pending" before the data handler is ready,
    // "ok" once every FormKey resolved, "unresolved:<FormKey>" on the refusal
    // that turns the probe OFF), so the config block never leaves a
    // configured-but-inoperative protection key unstated.
    std::array<char, TextBytes> cfgProtectedNames{}, cfgProtectedForms{}, cfgProtectionFacts{};
    // w-probe-r20 (R21-4): GfxMember / GfxListEntry / GfxRootNested and the
    // four arrays they used to sit in inline are now GfxPayload, reached
    // through `gfx` below. The type names and the Max* caps stay available
    // on Row so existing call sites (Row::GfxMember, Row::MaxGfxListEntries,
    // ...) do not have to change.
    using GfxMember = js::GfxMember;
    using GfxListEntry = js::GfxListEntry;
    using GfxRootNested = js::GfxRootNested;
    static constexpr unsigned MaxGfxMembers = GfxPayload::MaxGfxMembers;
    static constexpr unsigned MaxGfxListEntries = GfxPayload::MaxGfxListEntries;
    static constexpr unsigned MaxGfxRootMembers = GfxPayload::MaxGfxRootMembers;
    static constexpr unsigned MaxGfxRootNested = GfxPayload::MaxGfxRootNested;
    const char* gfxTrigger{"snapshot_key"};
    const char* gfxReason{"OK"};
    std::int32_t gfxSelectedIndex{-1};
    double gfxArgsUnitPrice{};
    bool gfxArgsUnitPriceKnown{};
    bool gfxListSellSideKnown{}, gfxListSellSide{};
    // w-probe-r18 (R18-4): the BarterMenu root object's own members, read once
    // at the snapshot key (GFxMovieView::GetVariable("_root.Menu_mc") then
    // VisitMembers), fail-closed to gfxRootPresent=false when the path is not
    // an object -- never a throw. Numbers and booleans both use valueKind 1 on
    // the shared GfxMember shape (a boolean is stored as 0/1 in `number`; the
    // member's own `gfxType` still says which one it was); strings use
    // valueKind 2. `gfxTrigger`/`gfxReason` above are shared with js_gfx_entry
    // and js_gfx_list (one row instance is only ever one type).
    bool gfxRootPresent{};
    // w-probe-r20 (R21-4): null for every row kind except js_gfx_entry /
    // js_gfx_list / js_gfx_root. MutableGfx() allocates it lazily for a
    // producer about to fill it; the JSON writer and every other reader goes
    // through the pointer with a null check (an absent payload reads exactly
    // like an empty one: count 0, truncated false, empty array).
    std::shared_ptr<const GfxPayload> gfx;
    GfxPayload& MutableGfx() {
        if (!gfx) gfx = std::make_shared<GfxPayload>();
        return const_cast<GfxPayload&>(*gfx);
    }
    // ---------------------------------------------------------------------
    // w-probe-r1 additions. Every one of them is a new name: no O field is
    // given a second meaning (design section 8).
    // ---------------------------------------------------------------------
    // On every row: which generations were current. The process identity is a
    // property of the process, not of the row, so it comes from ProcessRunId()
    // and is not copied into 256 queue slots.
    std::uint64_t menuGeneration{}, loadGeneration{}, inventoryGeneration{};
    // The comparison window a row belongs to.
    std::uint64_t firstEventSeq{}, lastEventSeq{};
    Phase phase{Phase::kUnknown};
    // Trace rows: the thread the producer really ran on, the kind of task, the
    // capture and publish instants and the re-entry depth of the chain thunk.
    std::uint64_t producerThreadId{}, captureNs{}, publishNs{};
    unsigned reentryDepth{};
    const char* taskKind{"none"};
    // Whether every raw argument and its type were captured.
    bool argsComplete{};
    // The argument shape verdict of a traced ItemSelect, recorded and never
    // acted on: ARGS_SHAPE_MATCH / ARGS_REJECTED / ARGS_UNKNOWN.
    const char* argsShape{"ARGS_UNKNOWN"};
    const char* argsShapeReason{"none"};
    // Product provenance is embedded by the release build and emitted at startup.
    std::array<char, TextBytes> releaseId{};
    std::array<char, TextBytes> runtime{};
    std::array<char, TextBytes> skse{};
    std::array<char, TextBytes> addressLibrary{};
    std::array<char, TextBytes> baseIniSha256{};
    std::array<char, TextBytes> espSha256{};
    std::array<char, TextBytes> acceptanceRun{};
    std::array<char, TextBytes> receiptBasename{};
    // w-probe-r71 (v0.9.0): the Q4 proof suffix of the js_startup row
    // (agh::rt::AlProofJson, a ",\"alFormat\":..." fragment written verbatim
    // before the closing brace). Empty on 1.6.1170, so that row keeps its bytes.
    std::array<char, AlProofBytes> alProof{};
    // r36: the receipt destination and its bounded-file contract are stated
    // in startup evidence. The fallback bit is also available to Diagnostics.
    bool receiptFallback{};
    bool menuOpen{};
    std::uint64_t rotateBytes{};
    unsigned retainedGenerations{};
    std::array<char, TextBytes> rotatedFrom{};
    std::uint64_t rotatedBytes{};
    unsigned generation{};
    // Close and warning rows: what the invalidation left behind.
    unsigned pendingObservations{};
    // This is elapsed time since invalidation at the delayed idle report, not
    // a shutdown-latency measurement (live/lc finding, r4 lifecycle fix).
    std::uint64_t msSinceInvalidationAtReport{};
    WObservation wObservation{};
    WPhaseInfo wPhase{};
    WArgsInfo wArgs{};
    // w-probe-r22 (R23-3): append-only attempt evidence.
    // attempt, so a STALE_TARGET refusal names the stale member without a
    // join to a nearby js_w_binding/js_w_phase row. Appended after every
    // existing field; existing field order is unchanged.
    bool factsReadOk{}, factsOwnerReadable{};
    std::uint32_t factsBaseObj{}, factsOwnerForm{};
    std::uint64_t factsMenuGeneration{}, factsLoadGeneration{}, factsInventoryGeneration{};
    std::uint64_t factsAgeMs{};
    std::uint64_t settleRequestedMs{}, settlePublishedMs{};
    bool settleDeferred{}, latchCleared{};
    // w-probe-r27 (R28-1, design ERROR_LATCH row): the BarterMenu open row
    // states whether a rule-run task was still queued from the previous menu
    // generation when the new session started. Like latchCleared it is
    // append-only and carried by every row kind, so an older row kind is
    // unchanged except for the two new keys.
    bool queuedRunDropped{};
    const char* oracleBlockSource{"none"};
    // w-probe-r24 (W-5, R25-4): the rule-selected multi-candidate issuance
    // path. runToken/issueSource are append-only, carried by the shared
    // writer on every row kind (same rule as the r21 facts fields above);
    // an attempt not part of a rule run keeps runToken 0 and issueSource
    // "bind". The remaining fields are js_rule_run / js_rule_candidate only.
    std::uint64_t runToken{};
    const char* issueSource{"bind"};
    std::int32_t ruleCandidatesRead{}, ruleCandidatesEligible{};
    // w-probe-r33 (R38-1): the rows that passed every exclusion except the run
    // limit (= selected + run_limit-excluded), written right after
    // candidatesEligible. candidatesEligible is unchanged: it stays the count
    // of rows that survived the read/type filter.
    std::int32_t ruleCandidatesSelectable{};
    CandidateRowSummary ruleCandidateRows{};
    std::int32_t ruleExcludedNotDisplayed{}, ruleExcludedNoQuote{}, ruleExcludedIdentityUnresolved{},
        ruleExcludedFactsUnreadable{};
    std::int32_t ruleIssued{}, ruleConfirmed{};
    bool ruleCapReached{};
    const char* ruleRunReason{"none"};
    std::array<char, TextBytes> ruleAttemptTerminals{};
    std::array<char, TextBytes> ruleTerminalReasons{};
    std::uint64_t ruleHealthDropped{};
    std::uint64_t ruleListEntries{}, ruleReadUs{};
    const char* ruleReadReason{"none"};
    OracleVerdict ruleOracleVerdict{};
    // w-probe-r26 (R27d-2): the rule run's own merchant-balance fact, taken
    // from cachedBeforeSample or the last sample under the menu/load
    // generation test in WSelect.h. Never a per-candidate read; value -1
    // means unknown, never a merchant with less than one gold.
    const char* ruleBalanceSource{"none"};
    std::int64_t ruleBalanceValue{-1};
    std::int64_t ruleBalanceInventoryDelta{-1};
    // w-probe-r27 (R28-1): the one gate reason this run was decided with
    // ("OK" when it passed the D-W07 foreground gate) and the hotkey down
    // edges the sink dropped between this run's start and its terminal.
    const char* ruleGateReason{"none"};
    std::uint64_t ruleEdgesDroppedDuringRun{};
    std::int32_t ruleCandidateIndex{-1};
    std::uint32_t ruleCandidateBaseObj{};
    bool ruleCandidateHasIndividual{};
    std::int32_t ruleCandidateFilteredIndex{-1};
    bool ruleCandidateIndexDrift{};
    std::int32_t ruleCandidatePlannedIndex{-1};
    double ruleCandidateUnitPrice{};
    std::array<char, TextBytes> ruleCandidateName{};
    const char* ruleCandidateDecision{"EXCLUDE"};
    const char* ruleCandidateReason{"none"};
    // w-probe-r28 (R29-1/R29-3): the D-W11 settings echo, append-only and
    // carried by every row kind (same rule as runToken above). The seven
    // cfg* keys are the live snapshot's values, settingsSource names where
    // they came from, configGeneration is the atomic generation the save
    // path bumps, and effectiveOn/effectiveReason are the R29-3 verdict.
    unsigned cfgEnabled{}, cfgConfirmBeforeSell{}, cfgHotkey{}, cfgMaxValue{}, cfgMaxItems{};
    float cfgMinWeight{}, cfgMaxValueWeight{};
    // w-probe-r29 (B-001 of a review): the fixed,
    // display-only `Mode` value of the v0.1 key table is echoed too, so a
    // receipt never leaves the eighth D-W11 key unstated. It is a constant
    // (Settings.h SettingsModeName, "Hotkey"), never stored and never parsed.
    const char* cfgMode{"Hotkey"};
    const char* settingsSource{"defaults"};
    std::uint64_t configGeneration{};
    bool effectiveOn{};
    const char* effectiveReason{"DISABLED"};
    // js_rule_run only: the R29-3 settings verdict of the run's snapshot
    // ("OK"/"DISABLED"/"RUNTIME_MISMATCH"/"ADAPTER_UNPROVEN"/
    // "CONFIG_CORRUPT"/"API_MISMATCH"). gateReason stays the D-W07 gate.
    const char* ruleSettingsReason{"none"};
    // js_settings_apply only: the one key a native Set* applied, and how the
    // save went. `accepted`/`reason` come from ApplySetting, `saved` says the
    // file was written and `readbackOk` that the readback matched (R29-2).
    const char* settingsApplyKey{"none"};
    bool settingsApplyValue{};
    bool settingsApplyAccepted{}, settingsApplySaved{}, settingsApplyReadbackOk{};
    const char* settingsApplyReason{"none"};
    // w-probe-r34 (D-W08): the settings stop confirmation is emitted after
    // older-generation rule work has drained through the existing UI queue.
    unsigned stopPendingBefore{}, stopPendingAfter{}, stopCancelled{};
    std::uint64_t stopIdleUs{};
    bool stopIdle{}, stopLatched{};
    // S2c: js_batch and js_batch_row only (append-only, never printed
    // by any other row kind).
    BatchReceipt batch{};
    BatchRowReceipt batchRow{};
    // v0.1.0: js_convert_run and js_convert_row only.
    ConvertReceipt convert{};
    ConvertRowReceipt convertRow{};
    // v0.5.0: js_frame_window only (observe-only, DevFrameWindow=1).
    FrameWindow frame{};
    // v0.8.0: js_convert_run and js_batch only (printed when present).
    PriceSourceReceipt priceSource{};
};
// R21-4: the ceiling that keeps js::Row a stack frame that a game/UI task
// thread can afford. Never raise this; move the next largest member into
// GfxPayload (or an equivalent heap payload) instead.
static_assert(sizeof(Row) <= 8192, "js::Row must stay small enough for game-thread stack frames (R21-4)");

std::uint64_t NowNs();
std::uint64_t NowUs();
std::uint64_t NowMs();
// Writes one complete row and flushes it. OpenLog uses this for the startup
// row and the worker uses it for every queued row.
bool WriteRow(std::ostream& out, const Row& row);
// w-probe-r14 (R14-2): where the startup note is written. Set from the SKSE log
// directory before anything else in the load body can refuse, so a refusal that
// happens before the receipt opens still has somewhere to go.
void SetDiagnosticDirectory(const std::filesystem::path& directory) noexcept;
// w-probe-r14 (R14-2): one startup note line. Never throws and never fails the
// caller; a note that cannot be written leaves the caller unchanged.
void ReportStartupFailure(const char* stage, const char* detail) noexcept;
// Opens the receipt and writes the startup row before returning. The startup
// row states the file it was actually written to, which is the fallback path
// when the primary receipt refused (w-probe-r14 / R14-2). "opened" receives
// that path when the caller wants it.
bool OpenLog(const std::filesystem::path& path, Row startup,
             std::filesystem::path* opened = nullptr);
// Host-only seam: production always calls OpenLog and therefore uses
// RotateBytes. This takes a value rather than a mutable global.
bool AppendReceiptForTest(const std::filesystem::path& path, const Row& row,
                          std::uint64_t rotateBytes);
bool ReceiptFallback() noexcept;
void Log(const Row& row) noexcept;
// w-probe-r71 (v0.9.0, E1 install ledger): one complete JSON line of the
// shared runtime gate (agh::rt::GateRefusalJson / LoadFailureJson), queued for
// the writer thread once the receipt is open (rotation as for rows). At most
// RawLines lines wait; a line that does not fit is dropped and counted.
inline constexpr unsigned RawLines = 4;
inline constexpr unsigned RawLineBytes = 512;
bool LogRawLine(const std::string& json) noexcept;
// The same line before the writer exists: one synchronous append to `path`
// (the receipt the writer would open). Never throws.
bool AppendRawLine(const std::filesystem::path& path, const std::string& json) noexcept;
void MarkTask(std::uint64_t ms) noexcept;
void SetPump(void (*pump)()) noexcept;
// w-probe-r1: the process identity every row carries. The caller supplies the
// PID and the wall clock start, because this file stays Windows free so the
// offline harness can link it.
void SetProcessRun(std::uint64_t processId, std::uint64_t startEpochMs) noexcept;
const char* ProcessRunId() noexcept;
// The current receipt accounting, for the row that states it and for the
// harness that checks the identity.
Delivery CurrentDelivery() noexcept;

extern std::atomic<std::uint64_t> drops, budgetOverruns, wrongThreadCount, truncated;
// r4: what the trace filter refused, plus the three thread ids the receipt now
// states instead of leaving the reader to infer them from "other".
extern std::atomic<std::uint64_t> traceExcluded;
extern std::atomic<std::uint64_t> taskThreadId, uiThreadId, hookThreadId;
extern NameCounters traceExcludedByName, truncatedByName;
extern std::atomic<std::uint64_t> callbackTruncated, ringDrops, queueFailures, hookExceptions, rotations;
// w-probe-r27 (R28-1): hotkey down edges the input sink dropped because a
// rule-run task was already queued, a run was active or an attempt was in
// flight. The sink owns the increments; HEALTH states the session total and
// every js_rule_run row its own delta.
extern std::atomic<std::uint64_t> edgesDropped;
extern std::atomic<std::uint64_t> mutations;
extern std::atomic<std::uint64_t> epochStale, registerAttempts, perfLatches, oracleTruncations, continuations;
// r5: oracle jobs that had to wait because one was already being built, jobs a
// load abandoned, and every tracked stat name the sink saw (O-4 is answerable
// from this table even when the row cap has been reached).
extern std::atomic<std::uint64_t> oracleDeferred, oracleAborted;
extern NameCounters statByName;
// ---------------------------------------------------------------------------
// w-probe-r1 counters (design section 4.3). Every one of them has a stated
// denominator beside it in HEALTH, because an unreached zero is not a pass.
// ---------------------------------------------------------------------------
// The one counter that exists purely to state a zero: nothing in this probe
// increments it, exactly like mutations.
extern std::atomic<std::uint64_t> dispatchCount;
// Every call the chain thunk passed to the original, and the subset of those
// that were a BarterMenu ItemSelect. The two are deliberately different
// measurements (design section 2.1).
extern std::atomic<std::uint64_t> originalPassCount, manualItemSelectCount;
extern std::atomic<std::uint64_t> reentryCount, hookCallCount;
extern std::atomic<std::uint64_t> observationCount, observationRequests, staleObservationCount;
extern std::atomic<std::uint64_t> argsShapeMatchCount, argsRejectedCount, argsUnknownCount;
extern std::atomic<std::uint64_t> fundsKnownCount, fundsInsufficientCount, fundsUnknownCount;
extern std::atomic<std::uint64_t> stableSampleCount, invalidSampleCount, sampleCount;
extern std::atomic<std::uint64_t> phaseUnknownCount, readPhaseUnprovenCount;
extern std::atomic<unsigned> pendingObservationTasks;
// The generations every row is stamped with. loadGeneration is the counter the
// O rows have always called "epoch"; Probe.cpp binds that name to it so there
// is one counter and not two.
extern std::atomic<std::uint64_t> loadGeneration, menuGeneration, inventoryGeneration;
extern std::atomic<std::uint64_t> lastEventSeqSeen;
// Cumulative visit accounting (design section 4.3). A repeated read is charged
// again; past a cap the probe stops reading instead of quietly paying more.
// w-probe-r8 (design ruling R8-2): there is one configured cap. The INI VisitCap
// bounds the entry walk, the xList walk and the oracle entry walk alike, so a
// run can no longer hit a hard coded 8192 entry cap while the INI says 262144.
// MaxXListVisits stays only as the value configuredVisitCap holds before the
// INI has been read; R-W3 measured the old fixed entry cap being reported as
// entryVisitCap=8192 next to xListVisitCap=262144 (anomaly A-02).
constexpr std::uint64_t MaxXListVisits = 262144;
// w-probe-r7: VisitCap is the configured per-menu oracle cap; per-stage
// deadlines remain independent of this visit cap. w-probe-r56: it is no
// longer also a process-wide valve (WObserve.h OracleStageAllowance), so
// the process cap constant is gone.
constexpr std::uint64_t MaxOracleSessionVisits = 262144;
// visitCapHits counts only reads actually stopped by an exhausted cap. A short
// stage allowance that completes its read is counted separately.
extern std::atomic<std::uint64_t> entryVisits, xListVisits, oracleEntryVisits;
extern std::atomic<std::uint64_t> visitCapHits, visitAllowanceShort;
extern std::atomic<std::uint64_t> configuredVisitCap;
}
