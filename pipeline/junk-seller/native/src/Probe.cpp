#include "Probe.h"
#include "Batch.h"
#include "BuildInfo.h"
#include "Convert.h"
#include "Oracle.h"
#include "PerkPrice.h"
#include "PriceReceipt.h"
#include "Perf.h"
#include "Protection.h"
#include "Ring.h"
#include "SaleGate.h"
#include "Selection.h"
#include "Settings.h"
#include "PapyrusBridge.h"
#include "ResultText.h"
#include "Stages.h"
#include "WObserve.h"
#include "WIssue.h"
#include "WBind.h"
#include "WSelect.h"
#include "receipt/Log.h"
// v0.1.0: the one native file allowed to mutate the inventory (sell
// anywhere). Included after every header it builds on.
#include "ConvertEngine.h"
#include "PerkPriceEngine.h"
#include <RE/B/BSInputDeviceManager.h>
#include <RE/B/ButtonEvent.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <span>
#include <system_error>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace js {
// w-probe-r71 (v0.9.0, E1 design 1.6): the plugin latch (plan step "arm"
// of Install.h; it replaces the v0.8.0 `enabled` flag) and the commit latch of
// the sink compound "listener_sinks". Every resident callback (the listener,
// the sinks, the natives, the VM callback) reads `latch` as its first
// statement; a sink also reads `sinksLive` (latch_gate.py, contract JS).
agh::rt::Latch latch;
agh::rt::Latch sinksLive;
// Forward declaration at js scope: LiveSettingsCopy is defined after the
// anonymous namespace closes but is used by the input sink inside it (the
// host gates never compile this unit; the Windows builds caught it).
Settings LiveSettingsCopy();
namespace {
// Hard per-process caps. Past a cap the probe only increments a counter, so a
// long session cannot grow the receipt without bound.
constexpr unsigned MaxSessions = 16;
// r4 / probe-js02 anomaly 2: the per process cap was 512 and RequestItemCardInfo
// spent all of it in one menu open. With the noisy names excluded by name the
// cap is the ring size, so a whole session of the four remaining callbacks fits.
constexpr unsigned MaxContainerRows = 256;
// r5 / probe-js05 anomaly 5: the stat sink no longer waits for the barter menu,
// so a row cap that only had to cover one menu session is too small. Every name
// is counted in statByName whether or not a row survives this cap.
constexpr unsigned MaxStatRows = 512;
constexpr unsigned MaxSelectedRows = 256;
constexpr unsigned MaxPapyrusRows = 256;
constexpr unsigned MaxRegisterAttempts = 3;
constexpr unsigned DrainBatch = 16;
constexpr std::uint64_t SelectionPeriodMs = 500;
constexpr std::uint64_t SelectionHeartbeatMs = 4000;
constexpr std::uint64_t SettleMs = 200;
constexpr std::uint64_t SinkBudgetUs = 10;

// w-probe-r28 (R29-2): the optional user settings file, the sibling of the
// probe ini LoadSettings opens by the same relative literal. The save path
// writes "<path>.tmp" beside it and renames over it, so both always share one
// directory and a half-written file is never the target.
constexpr const char* UserSettingsPath = "Data/SKSE/Plugins/AGH_JunkSeller_user.ini";

struct ProbeIniSettings {
    std::array<char, TextBytes> protectionFacts{"live"};
    // The base INI has no settings; this field labels the retained protection
    // facts in the product config block.
} settings;

// w-probe-r28 (R29-1/R29-2/R29-3): the live D-W11 settings and the state the
// effective verdict reads. liveSettings is written only through
// ApplyAndSaveSetting (Papyrus VM thread) and LoadSettings (SKSE load thread),
// always under settingsMutex, and read as one copy.
std::mutex settingsMutex;
js::Settings liveSettings{};
// Where the live snapshot came from: 0 defaults, 1 the probe ini's two
// protection keys, 2 the user settings file. Append-only in that order.
std::atomic<unsigned> settingsSourceKind{};
// The atomic config generation the save path bumps once a readback matched.
std::atomic<std::uint64_t> configGeneration{};
// A user settings file that exists but does not parse (R29-2).
std::atomic<bool> configCorrupt{};
// Set by the CheckAPI native when a caller expected an API other than 1.
std::atomic<bool> apiMismatch{};
// The UI-owned adapter verdict. It is published by the BarterMenu open/close
// path and by the rule-run gate, so the cached effective state uses the same
// adapter fact that can refuse a run.
std::atomic<bool> adapterProvenCache{};
std::atomic<bool> barterMenuOpenCache{};
// The last save failure text ("none" when there is none), and the R29-3
// effective-state cache every row's echo reads. The reason token is a pointer
// to one of the Settings.h string literals, so it needs no lock.
std::mutex lastRunMutex;
std::array<char, TextBytes> lastSaveError{};
std::array<char, TextBytes> lastRunSummary{};
std::atomic<bool> effectiveOnCache{false};
std::atomic<const char*> effectiveReasonCache{js::EffectiveReasonDisabled};

// One staged UI task at a time (B-01). The open task no longer runs selection
// and the oracle stages inline; each is its own bounded task.
std::atomic<std::uint64_t> sequence{};
// One counter under two names. "epoch" is what every O row and ClassifyRecord
// have always called the load generation; loadGeneration is the explicit name
// the W rows and HEALTH print. Binding the name to the same object keeps the
// two from ever drifting apart.
std::atomic<std::uint64_t>& epoch = loadGeneration;
std::atomic<std::uint64_t> sessions{}, callRows{}, containerRows{}, statRows{}, selectedRows{}, papyrusRows{};
std::atomic<bool> sessionCapWarning{};
std::atomic<std::uint64_t> observationId{};
// w-probe-r1: the armed quiet-sample observation. At most one may be
// outstanding (design section 8) and it is dropped, not deferred, when the
// generation it belongs to is gone.
std::atomic<bool> observationArmed{};
std::atomic<std::uint64_t> armedLoadGeneration{}, armedMenuGeneration{};
// When a generation was invalidated and whether the idle that followed it has
// already been stated once.
std::atomic<std::uint64_t> invalidationMs{};
std::atomic<bool> idleReported{true};
std::atomic<std::uintptr_t> previousItemSelectHandler{}, currentItemSelectHandler{};
// A UI-task snapshot copied into observation rows. These atomics are
// observations, never permissions.
std::atomic<bool> phaseBarterOpen{}, phaseModalOpen{}, phaseGamePaused{}, phaseUiMovie{}, phaseItemSelect{};
std::atomic<bool> phaseQuantityOpen{}, phaseMessageOpen{}, phaseConsoleOpen{};
std::atomic<bool> phaseNegativeTopmost{}, phaseHandlerUnchanged{};
std::atomic<std::uintptr_t> phaseHandlerPointer{}, phasePreviousHandlerPointer{};
std::atomic<std::uintptr_t> activeItemList{};
std::atomic<bool> barterOpen{}, registered{}, registerInFlight{};
std::atomic<bool> taskQueued{}, uiQueued{}, stageQueued{}, settlePending{};
std::atomic<unsigned> stageMask{};
// r5 / probe-js04 anomaly 1: the poll clock and the row clock are two different
// things. lastSelectionMs only moves when a row is written, so gating the poll
// on it ran the poll on every pass of the receipt writer loop (82 per second
// measured) instead of twice per second.
std::atomic<std::uint64_t> lastEventMs{}, lastSelectionMs{}, lastPollMs{}, lastMainTaskCompletedMs{};
std::atomic<std::uint32_t> lastSelectedBase{};
std::int32_t lastSelectedIndex{-1};

// r5: the oracle job under construction. One job exists at a time. The request
// queue serializes request, publish handoff, and abort, so a settle snapshot is
// never displaced by another normal task requester.
OracleSample oracleSample{};
js::WalkCursor oracleWalk{};
std::atomic<std::uint64_t> oracleStageNotBeforeNs{};
const char* oracleOrigin{"none"};
OracleRequestQueue oracleRequests{};

std::uint64_t NextSeq() { return ++sequence; }

// r4 / probe-js02 anomaly 4: an SKSE task and an SKSE UI task are two different
// expected threads, and r3 kept overwriting one id with the other, so every UI
// side row read "other" and inflated wrongThreadCount. Both ids are recorded,
// and wrongThreadCount now means "neither of the two expected threads".
std::uint64_t CurrentThreadId() noexcept { return static_cast<std::uint64_t>(GetCurrentThreadId()); }
ThreadKind CurrentThread() noexcept {
    const auto current = CurrentThreadId();
    const auto task = taskThreadId.load(), ui = uiThreadId.load();
    if (task && current == task) return ThreadKind::kMainTask;
    if (ui && current == ui) return ThreadKind::kUiTask;
    if (!task && !ui) return ThreadKind::kUnknown;
    ++wrongThreadCount;
    return ThreadKind::kOther;
}
void EnterTask() noexcept {
    taskThreadId = CurrentThreadId();
    MarkTask(NowMs());
}
void EnterUiTask() noexcept {
    // The first UI task is a measured identity, not a rolling last-task value.
    if (!uiThreadId.load()) uiThreadId = CurrentThreadId();
    MarkTask(NowMs());
}
// w-probe-r28 (R29-3): where the live snapshot came from, as one token.
const char* SettingsSourceName(unsigned kind) noexcept {
    if (kind == 1) return "ini";
    if (kind == 2) return "user";
    return "defaults";
}

// The runtime/version test that decides whether this module may load at all is
// Plugin.cpp:18-19 (skse->RuntimeVersion() != REL::Version{1,6,1170,0} ||
// skse->SKSEVersion() != REL::Version{2,2,6,0}.pack()); reaching any code in
// this file means it passed, so it is a constant here rather than a second
// copy of the comparison.
bool RuntimeOk() noexcept { return true; }

void PublishAdapterVerdict(bool proven) noexcept {
    adapterProvenCache.store(proven);
}

// The r10 sale gate's own adapter readiness, the same two facts Probe.cpp
// sets on SaleInput at the gate (delegateAvailable / uiMovieAvailable, refused
// in WIssue.h:493-494 as kAdapterUnavailable delegate/uiMovie). Asked at a
// rule run's entry only, on the UI task.
bool AdapterProven() noexcept {
    auto* ui = RE::UI::GetSingleton();
    if (!ui) {
        PublishAdapterVerdict(false);
        return false;
    }
    auto menu = ui->GetMenu<RE::BarterMenu>();
    const bool proven = menu.get() && menu->fxDelegate.get() != nullptr && menu->uiMovie != nullptr;
    PublishAdapterVerdict(proven);
    return proven;
}

// The R29-3 verdict of the live snapshot. `adapterProven` is the UI-owned
// adapter verdict published by the BarterMenu lifecycle and the run gate.
js::EffectiveStateResult LiveEffectiveState(bool adapterProven) {
    js::Settings live;
    {
        std::lock_guard<std::mutex> lock(settingsMutex);
        live = liveSettings;
    }
    return js::EffectiveState(live, RuntimeOk(), adapterProven, configCorrupt.load(),
                              apiMismatch.load());
}

// Publish the current verdict into the two caches every row echo reads. Called
// whenever an input to it moves: after the load, after the user file, after a
// successful save and after the CheckAPI native.
void RefreshEffectiveState() {
    const auto state = LiveEffectiveState(adapterProvenCache.load());
    effectiveOnCache.store(state.on);
    effectiveReasonCache.store(state.reason);
}

void Configure(Row& row) {
    // w-probe-r40: the bounded product config block carries the two
    // user-settings lists and their protection-facts label. The D-W11 echo
    // below remains separate and carries the live MCM settings.
    // live snapshot is read as one copy under the mutex; the effective state
    // comes from the cache RefreshEffectiveState published, so NewRow never
    // calls an engine API.
    // w-probe-r29 (B-001 of a review): the two
    // protection echoes and the display-only Mode come from that one live
    // snapshot, not from the ini object: a receipt can say settingsSource=user
    // only together with the effective values that source holds.
    // S-001 of a review: that snapshot holds the two
    // lists as parsed vectors and js::Settings has no raw text field, so the
    // echo joins them back with the separator the ini and the user file spell
    // -- the exact expression PapyrusGetProtectedNames already uses.
    js::Settings live;
    {
        std::lock_guard<std::mutex> lock(settingsMutex);
        live = liveSettings;
    }
    const std::string protectedNamesEcho =
        js::JoinSettingsList(live.protectedNames, js::SettingsListSeparator);
    const std::string protectedFormsEcho =
        js::JoinSettingsList(live.protectedForms, js::SettingsListSeparator);
    CopyText(row.cfgProtectedNames, protectedNamesEcho.c_str());
    CopyText(row.cfgProtectedForms, protectedFormsEcho.c_str());
    row.cfgProtectionFacts = settings.protectionFacts;
    row.cfgMode = js::SettingsModeName;
    row.cfgEnabled = live.enabled ? 1u : 0u;
    row.cfgConfirmBeforeSell = live.confirmBeforeSell ? 1u : 0u;
    row.cfgHotkey = static_cast<unsigned>(live.hotkey);
    row.cfgMaxValue = static_cast<unsigned>(live.maxValue);
    row.cfgMinWeight = live.minWeight;
    row.cfgMaxValueWeight = live.maxValueWeight;
    row.cfgMaxItems = static_cast<unsigned>(live.maxItems);
    row.settingsSource = SettingsSourceName(settingsSourceKind.load());
    row.configGeneration = configGeneration.load();
    row.effectiveOn = effectiveOnCache.load();
    row.effectiveReason = effectiveReasonCache.load();
    row.menuOpen = barterMenuOpenCache.load();
}
// r4 U-W4 snapshot. UI::IsMenuOpen/GetTopMostMenu and FxDelegate::callbacks
// are public CommonLib APIs: UI.h:79-103 and FxDelegate.h:19-50. This records
// what is live now; AddUITask's name and a previous thread id are not evidence.
void FillWPhase(Row& row, bool capture, unsigned reentry) {
    auto& out = row.wPhase;
    out.capture = capture;
    out.currentThread = CurrentThreadId();
    out.firstUiTaskThread = taskThreadId.load();
    out.mainUiThread = out.firstUiTaskThread && out.currentThread == out.firstUiTaskThread;
    out.reentryDepth = reentry;
    auto* ui = RE::UI::GetSingleton();
    if (!ui) return;
    out.barterOpen = ui->IsMenuOpen(RE::BarterMenu::MENU_NAME);
    out.modalOpen = ui->IsModalMenuOpen();
    out.gamePaused = ui->GameIsPaused();
    const bool quantity = ui->IsMenuOpen("QuantityMenu");
    const bool message = ui->IsMenuOpen("MessageBoxMenu");
    const bool console = ui->IsMenuOpen("Console");
    std::string names = std::string("BarterMenu=") + (out.barterOpen ? "1" : "0") +
        ",QuantityMenu=" + (quantity ? "1" : "0") + ",MessageBoxMenu=" +
        (message ? "1" : "0") + ",Console=" + (console ? "1" : "0") +
        ",paused=" + (out.gamePaused ? "1" : "0");
    CopyText(out.openMenus, names.c_str());
    RE::IMenu* top = nullptr;
    ui->GetTopMostMenu(&top, 0);
    auto menu = ui->GetMenu<RE::BarterMenu>();
    barterMenuOpenCache.store(menu.get() != nullptr);
    out.negativeTopmost = top && top != menu.get();
    CopyText(out.topmostMenu, top ? (out.negativeTopmost ? "other_menu" : "BarterMenu") : "none");
    out.uiMoviePresent = menu && menu->uiMovie;
    auto* delegate = menu ? menu->fxDelegate.get() : nullptr;
    // GHash::GetAlt returns CallbackDefn* (GHash.h:127-136); no callback is made.
    auto* defn = delegate ? delegate->callbacks.GetAlt(RE::GString(ItemSelectName)) : nullptr;
    out.itemSelectPresent = defn && defn->handler;
    out.handlerPointer = out.itemSelectPresent ? reinterpret_cast<std::uintptr_t>(defn->handler.get()) : 0;
    const auto prior = previousItemSelectHandler.load();
    out.previousHandlerPointer = prior;
    out.firstObservation = prior == 0;
    out.handlerUnchanged = prior != 0 && prior == out.handlerPointer;
    std::uintptr_t none = 0;
    if (out.handlerPointer) currentItemSelectHandler.compare_exchange_strong(none, out.handlerPointer);
    phaseBarterOpen.store(out.barterOpen);
    phaseModalOpen.store(out.modalOpen);
    phaseGamePaused.store(out.gamePaused);
    phaseUiMovie.store(out.uiMoviePresent);
    phaseItemSelect.store(out.itemSelectPresent);
    phaseQuantityOpen.store(quantity);
    phaseMessageOpen.store(message);
    phaseConsoleOpen.store(console);
    phaseNegativeTopmost.store(out.negativeTopmost);
    phaseHandlerUnchanged.store(out.handlerUnchanged);
    phaseHandlerPointer.store(out.handlerPointer);
    phasePreviousHandlerPointer.store(out.previousHandlerPointer);
}
// The phase of the thread this row is being made on. A row produced somewhere
// the probe cannot name says unknown and is counted, because "the measurement
// point is unknown" is a result and not a detail to be smoothed over.
Phase ThreadPhase(ThreadKind kind) noexcept {
    switch (kind) {
    case ThreadKind::kMainTask: return Phase::kMainTask;
    case ThreadKind::kUiTask: return Phase::kUiTask;
    default: ++phaseUnknownCount; return Phase::kUnknown;
    }
}
// w-probe-r1: every row states the three generations and the event sequence
// the comparison window ends at, so nothing has to be inferred from file order.
void StampGenerations(Row& row) noexcept {
    row.loadGeneration = loadGeneration.load();
    row.menuGeneration = menuGeneration.load();
    row.inventoryGeneration = inventoryGeneration.load();
    row.lastEventSeq = lastEventSeqSeen.load();
}
Row NewRow(const char* type, const char* origin, Phase phase = Phase::kCount) {
    Row row; row.type = type; row.origin = origin;
    row.seq = NextSeq(); row.epoch = epoch.load(); row.monotonicNs = NowNs();
    row.producerThreadId = CurrentThreadId();
    if (phase == Phase::kPreload || phase == Phase::kPostload) {
        // The lifecycle callback is its own measurement point, so it gets its
        // own thread name instead of counting as an unexpected thread.
        row.thread = ThreadKind::kLifecycle;
        row.phase = phase;
    } else {
        row.thread = CurrentThread();
        row.phase = phase == Phase::kCount ? ThreadPhase(row.thread) : phase;
    }
    StampGenerations(row);
    Configure(row);
    return row;
}
void Warning(const char* reason, const char* origin = "probe") {
    auto row = NewRow("js_warning", origin);
    row.reason = reason;
    row.pendingObservations = pendingObservationTasks.load();
    Log(row);
}

struct EventRecord {
    enum class Kind : unsigned { kContainer = 0, kStat = 1 };
    std::array<char, TextBytes> text{};
    std::uint64_t seq{}, ns{}, epoch{}, menuGeneration{}, mainTaskBeforeMs{};
    std::uint32_t oldContainer{}, newContainer{}, baseObj{}, referenceID{}, uniqueID{};
    std::int32_t itemCount{}, statValue{};
    Kind kind{Kind::kContainer};
    ThreadKind thread{ThreadKind::kUnknown};
    // r5: whether the barter menu was open when the event arrived. The stat
    // sink no longer refuses events outside the menu, so every row has to say
    // for itself which side of the menu it belongs to.
    bool barterOpen{};
    // w-probe-r24 fix round 2 (A-07): the dispatch token active in
    // issuedToken at the moment this native event was captured. 0 means no
    // attempt was active when the event was captured; HandleIssueEvent
    // requires an exact, non-zero match against the active attempt's own
    // token before treating an event as belonging to it, so an accidental
    // same-shaped unrelated transfer (or a row with a missing/zero token)
    // can never be attributed to the active attempt.
    std::uint64_t attemptToken{};
};
Ring<EventRecord, 256> eventRing;

void Pump();
void PumpStage();
RE::ItemList* CurrentItemList();
std::uint32_t CaptureSelectedBase() noexcept {
    auto* list = reinterpret_cast<RE::ItemList*>(activeItemList.load());
    auto* item = list ? list->GetSelectedItem() : nullptr;
    auto* entry = item ? item->data.objDesc : nullptr;
    return entry && entry->object ? entry->object->GetFormID() : 0;
}
struct LastSample {
    std::atomic<std::uint64_t> sampleId{}, menuGeneration{}, loadGeneration{}, inventoryGeneration{};
    std::atomic<std::uint64_t> lastEventSeq{};
    std::atomic<std::uint32_t> playerID{}, merchantContainerID{};
    std::atomic<std::int32_t> playerGold{-1}, merchantActorGold{-1}, merchantContainerGold{-1}, merchantInventoryGold{-1};
    std::atomic<std::int32_t> vendorGold{-1};
    std::atomic<bool> valid{}, publishedValid{};
    std::atomic<const char*> origin{"none"}, invalidReason{"none"};
    std::atomic<bool> targeted{};
    std::array<std::atomic<std::uint32_t>, OracleStageSlots> stageCostUs{};
    std::array<std::atomic<std::uint32_t>, OracleStageSlots> stageChunks{};
    std::atomic<std::uint32_t> walkRestarts{};
    std::atomic<std::uint32_t> scanned{}, baseCountVisits{}, truncatedStages{}, unsampled{};
    std::atomic<std::uint64_t> spanUs{};
} lastSample;
// UI-task-only slot reading. It never substitutes for the complete before
// sample the batch plan reads (batch_before).
struct VencSlotBalance {
    std::int32_t gold{-1};
    std::uint64_t sampleId{}, startMenuGeneration{}, endMenuGeneration{};
    std::uint64_t startLoadGeneration{}, endLoadGeneration{};
    std::uint64_t startInventoryGeneration{}, endInventoryGeneration{};
    std::uint32_t merchantContainerID{};
    bool complete{};
} vencSlotBalance;
js::RuleVencMerchantWindow oracleMerchantWindow{};
MenuOracleTally menuOracleTally{};
// W-4: a valid menu_open/observation sample may serve as the before side when
// the fresh issue_before walk is truncated. It is invalidated by any container
// generation change and never substitutes for a missing measured pair.
OracleSample cachedBeforeSample{};
const char* cachedBeforeSource{"none"};
std::atomic<std::uint64_t> cachedBeforeContainerGeneration{};
std::atomic<std::uint64_t> issueToken{}, issuedToken{};
std::atomic<bool> issueInFlight{}, issueLatched{}, issueOracleMismatchLatched{}, issueRowEmitted{};
std::atomic<std::uint64_t> activeDispatchToken{}, activeDispatchBase{};
// w-probe-r6 (W-3 ruling 2): every container event the sink saw while the
// menu was open, counted before the row cap. The attempt compares this with
// the events it examined, so an event nobody checked cannot pass as silence.
std::atomic<std::uint64_t> containerEventTotal{};
// w-probe-r24 fix round 5 (A-01): events this attempt lost at a sink drop
// site while it was in flight -- the process-wide MaxContainerRows cap or a
// full event ring. Such an event never reaches HandleIssueEvent, so without
// this counter an exact-looking pair could confirm although a third,
// unexamined mutation was dropped. Reset to zero at every dispatch, read at
// the deadline by the sole terminal gate.
std::atomic<std::uint64_t> attemptEventsLost{};
// w-probe-r24 design hand review (w27 A-02): the event ring's own refusals. The
// shared health counter ringDrops also counts callback trace ring refusals,
// which are not lost container evidence, so the attempt loss baseline is
// taken from this counter alone.
std::atomic<std::uint64_t> eventRingDrops{};
AttemptMachine issueMachine;

struct IssueContext {
    std::uint64_t token{}, startNs{}, beforeSampleId{}, settleSampleId{}, afterSampleId{};
    // w-probe-r12 (R12-1): when the settle sample was asked for, how long the
    // attempt waited for it and what became of it. The deadline is measured
    // from settleRequestMs once it is set.
    std::uint64_t settleRequestMs{}, settlePublishedMs{}, settleElapsedMs{}, settleCoalescedInto{};
    const char* settleSampleState{"none"};
    std::uint64_t startMenuGeneration{}, startLoadGeneration{}, startInventoryGeneration{};
    std::uint64_t startEventSeq{}, itemEventSeq{}, goldEventSeq{};
    // w-probe-r6: the container event count at dispatch, the events the
    // attempt examined, and the age of the request when it was decided.
    std::uint64_t startContainerEvents{}, handledEvents{}, observedEvents{}, requestAgeMs{};
    // w-probe-r24 fix round 3 (A-03): same-shaped events rejected for
    // arriving after AttemptDeadlineMs (WIssue.h AttemptMachine::LateEvent).
    std::uint64_t lateEvents{};
    // w-probe-r24 fix round 5 (A-01): the event-ring drop counter at dispatch
    // (w27 A-02: the event ring's own, never the shared callback-ring health
    // counter), and the events this attempt is measured to have lost (sink
    // drops plus any observed/handled gap). Non-zero denies CONFIRMED.
    std::uint64_t startEventRingDrops{}, eventsLost{};
    std::int64_t mainTaskGapMs{-1};
    std::uint32_t baseObj{}, playerID{}, vencID{};
    bool fromBind{};
    const char* responseValueSource{"none"};
    const char* unitPriceSource{"none"};
    std::int32_t quantity{}, unitPrice{};
    std::int32_t goldBaseObj{};
    std::array<std::int32_t, 4> beforeBalances{};
    std::array<std::int32_t, 4> afterBalances{};
    std::array<std::int32_t, 4> balanceDeltas{};
    std::array<std::int32_t, 2> inventoryDeltas{};
    // w-probe-r6 (ruling 3): what the two oracle samples measured.
    MeasuredPair measuredBefore{}, measuredAfter{};
    std::int32_t itemEventCount{}, goldEventCount{};
    std::uint32_t goldOldContainer{}, goldNewContainer{};
    bool active{}, itemEvent{}, goldEvent{}, afterPending{}, afterValid{}, deltaMatch{}, settleDeferred{}, latchCleared{};
    bool measuredPairsValid{};
    const char* beforeSource{"none"};
    const char* protectionRule{"none"};
    const char* refusalTerm{"none"};
    std::int32_t count{}, merchantGold{};
    double price{};
    // w-probe-r9 (R9-3): the failed foreground-gate term names, comma joined,
    // empty when the gate passed or was never the deciding term.
    std::array<char, TextBytes> foregroundTerms{};
    // w-probe-r10 (R10-2/R10-3): the row's own phase snapshot at gate time,
    // same encoding as js_w_phase, so an explanation never needs a join to a
    // nearby js_w_phase row. `topmostMenu` is still recorded but never gates.
    std::array<char, TextBytes> topmostMenu{}, openMenus{};
    bool negativeTopmost{}, paused{};
    WPhaseInfo wPhase{};
    // w-probe-r21 (R22-3 b): the selectionFacts the gate compared, so a
    // STALE_TARGET refusal names the stale member on the attempt row itself.
    bool factsReadOk{}, factsOwnerReadable{};
    std::uint32_t factsBaseObj{}, factsOwnerForm{};
    std::uint64_t factsMenuGeneration{}, factsLoadGeneration{}, factsInventoryGeneration{};
    std::uint64_t factsAgeMs{};
    OracleSample beforeOracle{}, settleOracle{};
    // w-probe-r24 (W-5): 0 / "bind" for the BindKey and manual W2 paths
    // (unchanged), non-zero / "rule" for an attempt the rule-run sequencer
    // dispatched.
    std::uint64_t runToken{};
    const char* issueSource{"bind"};
};
IssueContext issueContext{};

// w-probe-r6 (W-3 ruling 1): the one recorded key-edge request. It is written
// and read by UI tasks only (the request task, the oracle publish stage, the
// poll and the menu close), so it needs no atomics.
SaleRequest pendingRequest{};

// w-probe-r6: the protection facts of the last selection read, published by
// LogSelection (a UI task that already pays for that read under its own
// allowance). The issuing path only compares them; it never reads the
// selection itself.
struct SelectionFacts {
    bool readOk{}, capReached{};
    std::uint32_t baseObj{}, ownerForm{};
    bool ownerReadable{}, stolenKnown{}, questObject{}, favorite{}, worn{};
    std::uint64_t menuGeneration{}, loadGeneration{}, inventoryGeneration{};
    // w-probe-r21 (R22-3 b): when this read happened, so a consumer can state
    // the age of the facts it compared instead of only their generations.
    std::uint64_t readMs{};
};
SelectionFacts selectionFacts{};

std::uint64_t IssueGeneration() noexcept {
    return (menuGeneration.load() * 0x9E3779B97F4A7C15ULL) ^ epoch.load();
}

void FinalizeIssue(SaleTerminal terminal, const char* invalidReason = "NOT_SETTLED");
void DrainAttemptEventsThroughDeadline();
std::uint64_t RequestOracle(const char* origin, bool* deferred = nullptr,
                            std::uint64_t* coalescedInto = nullptr);
// w-probe-r21 (R22-3 a): forward declared so the bind handler (defined above
// LogSelection in this file) can refresh selectionFacts from its own
// read-back before the hand-off.
void LogSelection(RE::ItemList* list, const char* origin, bool force,
                  std::uint64_t oracleRequestId = 0);

void EmitSaleTerminal(const IssueContext& context, SaleTerminal terminal, unsigned dispatched,
                      const char* source = "none") {
    // The row phase follows the thread: pre-dispatch drops and the dispatch
    // itself are decided on a UI task, event terminals on the main task.
    auto row = NewRow("js_sale_attempt", "w2");
    row.attemptToken = context.token; row.terminalReason = SaleTerminalName(terminal);
    row.dispatchCount = dispatched; row.unitPriceSource = context.unitPriceSource == std::string_view("none") ? source : context.unitPriceSource;
    row.refusalTerm = context.refusalTerm;
    row.quantity = context.quantity;
    row.count = context.count;
    row.price = context.price;
    row.merchantGold = context.merchantGold;
    row.foregroundTerms = context.foregroundTerms;
    row.topmostMenu = context.topmostMenu;
    row.negativeTopmost = context.negativeTopmost;
    row.openMenus = context.openMenus;
    row.paused = context.paused;
    row.beforeSource = context.beforeSource;
    row.barterOpen = barterOpen.load();
    row.responseIdSource = context.fromBind ? "rule_bind" : "none";
    row.responseValueSource = context.responseValueSource;
    row.menuGeneration = context.startMenuGeneration ? context.startMenuGeneration : menuGeneration.load();
    row.loadGeneration = context.startLoadGeneration ? context.startLoadGeneration : epoch.load();
    row.beforeSampleId = context.beforeSampleId;
    row.afterSampleId = context.afterSampleId;
    row.settleSampleId = context.settleSampleId;
    row.settleSampleState = context.settleSampleState;
    row.settleElapsedMs = context.settleElapsedMs;
    row.mainTaskGapMs = context.mainTaskGapMs;
    row.coalescedInto = context.settleCoalescedInto;
    row.baseObj = context.baseObj;
    row.quoteBaseObj = context.baseObj;
    row.itemEvent = context.itemEvent;
    row.goldEvent = context.goldEvent;
    row.itemEventAttemptToken = context.itemEvent ? context.token : 0;
    row.goldEventAttemptToken = context.goldEvent ? context.token : 0;
    row.itemEventSeq = context.itemEventSeq;
    row.goldEventSeq = context.goldEventSeq;
    row.itemEventCount = context.itemEventCount;
    row.goldEventCount = context.goldEventCount;
    row.lateEvents = context.lateEvents;
    row.eventsLost = context.eventsLost;
    row.attemptQuantity = context.quantity;
    row.attemptUnitPrice = context.unitPrice;
    row.oldContainer = context.playerID;
    row.newContainer = context.vencID;
    row.goldOldContainer = context.goldOldContainer;
    row.goldNewContainer = context.goldNewContainer;
    row.goldBaseObj = context.goldBaseObj;
    row.balanceDeltas = context.balanceDeltas;
    row.inventoryDeltas = context.inventoryDeltas;
    row.balanceDeltasValid = terminal == SaleTerminal::kConfirmed && context.afterValid;
    row.deltaInvalidReason = row.balanceDeltasValid ? "none" : source;
    row.protectionRule = context.protectionRule;
    row.measuredBefore = {context.measuredBefore.playerBase, context.measuredBefore.vendorBase,
                          context.measuredBefore.playerGold, context.measuredBefore.vendorGold};
    row.measuredAfter = {context.measuredAfter.playerBase, context.measuredAfter.vendorBase,
                         context.measuredAfter.playerGold, context.measuredAfter.vendorGold};
    row.measuredPairsValid = context.measuredPairsValid;
    row.requestAgeMs = context.requestAgeMs;
    row.wPhase = context.wPhase;
    // w-probe-r21 (R22-3 b): the facts the gate compared for this attempt.
    row.factsReadOk = context.factsReadOk;
    row.factsBaseObj = context.factsBaseObj;
    row.factsMenuGeneration = context.factsMenuGeneration;
    row.factsLoadGeneration = context.factsLoadGeneration;
    row.factsInventoryGeneration = context.factsInventoryGeneration;
    row.factsAgeMs = context.factsAgeMs;
    row.factsOwnerForm = context.factsOwnerForm;
    row.factsOwnerReadable = context.factsOwnerReadable;
    row.settleRequestedMs = context.settleRequestMs;
    row.settlePublishedMs = context.settlePublishedMs;
    row.settleDeferred = context.settleDeferred;
    row.latchCleared = context.latchCleared;
    row.runToken = context.runToken;
    row.issueSource = context.issueSource;
    if (context.settlePublishedMs) {
        row.oracle = context.settleOracle;
        row.oracleBlockSource = "settle";
    } else if (context.beforeOracle.sampleId) {
        row.oracle = context.beforeOracle;
        row.oracleBlockSource = "before";
    }
    Log(row);
}

void FinalizeIssue(SaleTerminal terminal, const char* invalidReason) {
    if (!issueContext.active || issueRowEmitted.exchange(true)) return;
    if (terminal == SaleTerminal::kMenuClosed || terminal == SaleTerminal::kLoadChanged) {
        issueMachine.Close(terminal == SaleTerminal::kLoadChanged);
    } else {
        issueMachine.Abort(terminal);
    }
    issueInFlight.store(false);
    // w-probe-r25 (loop2 B-01, a review): the token
    // the finished attempt carried must not outlive its terminal. The sink
    // stamps record.attemptToken = issuedToken.load() on any captured
    // container event, so a token retained past the terminal would label an
    // out-of-attempt event (a late event of this menu, or the first event of
    // the next one) as the finished attempt's. Cleared here, in the same
    // place issueInFlight drops, so a non-zero issuedToken always means an
    // attempt is in flight.
    issuedToken.store(0);
    if (TerminalLatches(terminal, true)) {
        issueLatched.store(true);
        issueOracleMismatchLatched.store(terminal == SaleTerminal::kOracleMismatch);
    }
    issueContext.afterValid = issueContext.afterValid && terminal == SaleTerminal::kConfirmed;
    EmitSaleTerminal(issueContext, terminal, 1, invalidReason);
    issueContext.active = false;
}

// A request that never reached the dispatch. Its row carries dispatchCount 0
// and it never touches the context of an attempt that may be in flight.
void DropRequest(const SaleRequest& request, SaleTerminal terminal, const char* source,
                 std::uint64_t ageMs, std::uint64_t sampleId = 0,
                 const OracleSample* before = nullptr) {
    IssueContext dropped{};
    dropped.token = request.token;
    dropped.baseObj = request.baseFormID;
    dropped.quantity = request.quantity;
    dropped.requestAgeMs = ageMs;
    dropped.beforeSampleId = sampleId;
    dropped.latchCleared = request.latchCleared;
    dropped.runToken = request.runToken;
    dropped.issueSource = request.runToken ? "rule" : "bind";
    if (before) dropped.beforeOracle = *before;
    EmitSaleTerminal(dropped, terminal, 0, source);
}

// W-3 ruling 1, the key edge: record {baseFormID, quantity, menuGeneration,
// tick} and ask for a fresh oracle job. Nothing here reads an inventory; the
// base is the one pointer ItemList already holds for its highlighted row.
// w-probe-r24 (W-5): a single js_gfx_list-shaped entry read, engine adapter
// half of js::GfxListCandidateInput. Read-only, the same GetMember pattern
// The rule request and ObserveGfxList already use; the protection facts
// follow Selection.h's ReadSelectedInner exactly (IsQuestObject/IsFavorited/
// IsWorn/IsEnchanted, GetValue, GetWeight), including its honesty about what
// this build cannot resolve (see the comments on the fields below).
struct RuleGfxRead {
    js::GfxListCandidateInput input{};
    std::int32_t filteredIndex{-1};
    std::array<char, TextBytes> name{};
    bool nameTruncated{};
    bool readDenied{};
    bool visitAllowanceShort{};
    // w-probe-r24 fix round 2 (A-04): the raw owner facts behind
    // input.candidate.ownerUnknown, kept here (not in the engine-free
    // header) so a rule dispatch can carry the same DecideProtection inputs
    // the BindKey path already uses onto its SaleRequest.
    std::uint32_t ownerForm{};
    bool ownerReadable{};
};

// heavy-fix (w-probe-r56): the candidate reader no longer draws on the
// process-lifetime TakeXListVisits allowance (WSelect.h RuleXListBudget says
// why); it takes the run's own budget and only adds its visits to the
// process total that the perf rows report.

// w-probe-r28 (R29-3): the one settings copy a rule run takes at its entry and
// never re-reads. Not noexcept: the copy owns two std::vectors, so a throwing
// allocator must reach the UI task's own try/catch (queueFailures) rather than
// call std::terminate. The Read* natives are not noexcept for the same reason. `values` are the v0.1 keys (the selector thresholds come
// from it, item 6); `names`/`forms` are the two protection sources derived
// from that same live copy (w31 fix round 2, audit A-001), and
// `configGeneration` is the generation this plan belongs to, so a save during
// the run stops it (R29-2).
struct RuleSettingsSnapshot {
    js::Settings values{};
    ProtectedNames names{};
    ProtectedFormSet forms{};
    bool protectionFactsKnown{};
    std::uint64_t configGeneration{};
};

// w31 fix round 2 (a review A-001): the FormKeys of the
// LIVE settings are resolved to runtime FormIDs here, through the same
// TESDataHandler::LookupFormID call the protection resolver uses for the probe
// ini's own keys. The native setters cannot do this themselves -- item 4
// requires them to touch no RE object -- so the resolution belongs to the
// run's own snapshot, taken on the UI task that already reads the item list.
// A key that resolves to 0, or a list whose items BuildProtectedFormKeys
// refuses, answers false and leaves the snapshot's protection facts unknown,
// so the candidates stay excluded facts_unreadable (fail-closed) instead of a
// protection the caller configured being ignored.
// hf-fix (w-probe-r55): the same call also merges the built-in protected
// forms (Protection.h BuiltinProtectedForms: HearthFires building materials and
// adoption gifts). A built-in key whose plugin is not loaded is skipped, never
// a refusal, and is reported once per process as one js_warning row.
std::atomic<bool> builtinProtectionSkipLogged{false};
bool ResolveLiveProtectedForms(const js::Settings& values, ProtectedFormSet& out) {
    out.Clear();
    auto* data = RE::TESDataHandler::GetSingleton();
    if (!data) return false;
    const auto keys = js::BuildProtectedFormKeys(values);
    // BuildProtectedFormKeys answers an empty set when one item refuses, which
    // must not read as "no forms are protected": ResolveProtectedFormSet's
    // count check (keys.Count() != values.protectedForms.size()) keeps the
    // refusal fail-closed.
    auto lookup = [data](std::uint32_t local, const char* plugin) -> std::uint32_t {
        return data->LookupFormID(local, plugin);
    };
    js::BuiltinProtectionResolution builtin{};
    const bool resolved =
        js::ResolveProtectedFormSet(keys, values.protectedForms.size(), lookup, out, builtin);
    if (builtin.skipped && !builtinProtectionSkipLogged.exchange(true)) {
        Warning("BUILTIN_PROTECTED_PLUGIN_ABSENT", "protection");
    }
    return resolved;
}

// The live v0.1 values and the two protection sources of one rule run (UI
// task). The values are copied under settingsMutex, and names/forms are built
// from that same copy, so a user-file value or a Papyrus Set* that reaches
// liveSettings reaches the selector too. settings.protectionFactsKnown stays a
// conjunct: it is the probe-wide R-W23 fail-closed state of the ini's own two
// keys, and a run must not outlive it.
RuleSettingsSnapshot TakeRuleSettingsSnapshot() {
    RuleSettingsSnapshot snapshot;
    {
        std::lock_guard<std::mutex> lock(settingsMutex);
        snapshot.values = liveSettings;
    }
    snapshot.names = js::BuildProtectedNames(snapshot.values);
    const bool namesBuilt = snapshot.names.Count() == snapshot.values.protectedNames.size();
    const bool formsResolved = ResolveLiveProtectedForms(snapshot.values, snapshot.forms);
    snapshot.protectionFactsKnown = namesBuilt && formsResolved;
    snapshot.configGeneration = configGeneration.load();
    return snapshot;
}

// w-probe-r25 (R27-2 b): the engine half of the Misc classification. The
// keyword names and the junk/not-junk decision live in Protection.h so the
// host test drives them; this only asks the base form's own keyword list
// (BGSKeywordForm::HasKeywordString, BGSKeywordForm.h:35; TESObjectMISC
// inherits BGSKeywordForm, TESObjectMISC.h:27). A base whose keyword list
// cannot be read is treated as NOT junk, so an unreadable form is never
// promoted to an issuable candidate.
bool MiscBaseIsJunk(RE::TESObjectMISC* form) noexcept {
    if (!form) return false;
    const int index = js::FirstNonJunkKeywordIndex(
        [form](const char* keyword) { return form->HasKeywordString(keyword); });
    return js::MiscTypeForKeywordIndex(index) == js::CandidateType::kJunk;
}

// w-probe-r45: `acceptance` is the rule run's own stolen-goods
// acceptance (RuleRunState::stolenAcceptance), decided once before the list
// read; the planning read and the pre-dispatch re-read pass the same value.
// heavy-fix (w-probe-r56): `xListBudget` is that run's own extra-list
// visit budget (RuleRunState::xListBudget), passed by both callers.
RuleGfxRead ReadRuleCandidateEntry(RE::ItemList::Item* item,
                                  const RuleSettingsSnapshot& snapshot,
                                  js::StolenAcceptance acceptance,
                                  js::RuleXListBudget& xListBudget) noexcept {
    RuleGfxRead out;
    if (!item || !item->obj.IsObject()) {
        out.readDenied = true;
        return out;
    }
    RE::GFxValue value;
    if (item->obj.GetMember("filteredIndex", &value) && value.IsNumber() && std::isfinite(value.GetNumber())) {
        out.input.filteredIndexKnown = true;
        out.filteredIndex = static_cast<std::int32_t>(value.GetNumber());
        out.input.filteredIndex = out.filteredIndex;
    }
    if (!out.input.filteredIndexKnown) return out;
    if (item->obj.GetMember("infoValue", &value) && value.IsNumber() && std::isfinite(value.GetNumber())) {
        out.input.infoValueKnown = true;
        out.input.infoValue = value.GetNumber();
    }
    if (item->obj.GetMember("text", &value) && value.IsString()) {
        const auto* displayName = value.GetString();
        out.nameTruncated = displayName && !js::CopyProtectedDisplayName(out.name, displayName);
    }
    auto* entry = item->data.objDesc;
    if (!entry || !entry->object) {
        out.readDenied = true;
        return out;
    }
    out.input.baseObjKnown = true;
    out.input.baseObj = entry->object->GetFormID();
    out.input.candidate.baseFormKey = out.input.baseObj;
    const auto formType = entry->object->GetFormType();
    // w-probe-r24 fix round 2 (A-01) plus w-probe-r25 (R27-2 b): Ingredient is
    // the one engine-verified, non-guessed type mapped positively. Misc is
    // junk unless the base carries one of the six non-junk vendor keywords
    // (Protection.h), which keeps gems, ore, hides, tools, animal parts and
    // Daedric artifacts out of the rule path through Selector.h's own type
    // term. Every other form type stays kUnknown, which DecideCandidate
    // already excludes fail-closed (kExcludedUnknown / SelectorTerm::kType).
    if (formType == RE::FormType::Ingredient) {
        out.input.candidate.type = js::CandidateType::kIngredient;
    } else if (formType == RE::FormType::Misc) {
        out.input.candidate.type =
            MiscBaseIsJunk(entry->object->As<RE::TESObjectMISC>()) ? js::CandidateType::kJunk
                                                                  : js::CandidateType::kUnknown;
    } else {
        out.input.candidate.type = js::CandidateType::kUnknown;
    }
    out.input.candidate.basePrice = entry->GetValue();
    out.input.candidate.weight = entry->GetWeight();
    out.input.candidate.count = entry->countDelta > 0 ? entry->countDelta : 1;
    out.input.candidate.questItem = entry->IsQuestObject();
    out.input.candidate.enchanted = entry->IsEnchanted();
    out.input.candidate.favorite = entry->IsFavorited();
    out.input.candidate.equipped = entry->IsWorn();
    // w-probe-r25 (R27-2 a): the two protection facts now have a real,
    // ini-driven source. protectionFactsKnown is the probe-wide state set by
    // protection resolution once both required keys parsed and every
    // ProtectedForms key resolved; while it is false, BuildRuleCandidate in
    // WSelect.h still excludes every candidate fail-closed
    // (facts_unreadable), never defaulting the facts themselves to "safe".
    // nameProtected compares the display name read above against
    // ProtectedNames (trimmed, case-insensitive, whole-name match);
    // formListProtected checks the base FormID against the resolved set.
    // w-probe-r28 (R29-3): both facts come from the run's own snapshot, taken
    // once at BeginRuleRun and never re-read mid-run, so a Set* during a run
    // cannot change the plan that run already decided.
    out.input.protectionFactsKnown = snapshot.protectionFactsKnown && !out.nameTruncated;
    if (out.input.protectionFactsKnown) {
        out.input.candidate.nameProtected = snapshot.names.Matches(out.name.data());
        out.input.candidate.formListProtected = snapshot.forms.Contains(out.input.baseObj);
    }
    // w-probe-r25 (R27-2 c): sameBaseMixedProtection is still unresolved by
    // design. Deciding whether the same base appears in the player's
    // inventory with more than one extra list needs a cross-stack sweep over
    // the whole inventory, which is out of scope for this stage; the field
    // keeps its false default rather than guessing, so Selector.h's
    // mixed_protection term never fires.
    out.input.candidate.sameBaseMixedProtection = false;
    // Real read, the same source and semantics DecideProtection (WIssue.h)
    // and Selection.h's ReadSelectedInner already use: an owner other than
    // the player is a protection fact, not a stolen verdict.
    auto* ownerForm = entry->GetOwner();
    out.ownerForm = ownerForm ? ownerForm->GetFormID() : 0;
    out.ownerReadable = true;
    auto* rulePlayer = RE::PlayerCharacter::GetSingleton();
    // w-probe-r45: a foreign owner is refused (owner_unknown, the
    // r42 term) only when this run's merchant does not accept stolen goods.
    // With StolenAcceptance::kRefused this is the r42 expression exactly
    // (StolenRule.h ForeignOwnerRefused, host-tested).
    out.input.candidate.ownerUnknown = js::ForeignOwnerRefused(
        out.ownerForm, rulePlayer ? rulePlayer->GetFormID() : 0, acceptance);
    // No public API here distinguishes a theft verdict from generic
    // ownership (Selection.h, same rule); never claimed true without a
    // source, so it stays false rather than excluding on an absence.
    out.input.candidate.stolen = false;
    if (!entry->extraLists) {
        // Positively no extra-data list at all: there cannot be an
        // individual. This is a real "no" answer, not a denied read.
        out.input.individualReadOk = true;
    } else if (xListBudget.Take()) {
        ++xListVisits;
        out.input.individualReadOk = true;
        for (auto* extra : *entry->extraLists) {
            if (!extra) continue;
            if (const auto* unique = extra->GetByType<RE::ExtraUniqueID>()) {
                out.input.hasIndividual = true;
                out.input.individualId = static_cast<std::uint32_t>(unique->uniqueID);
                break;
            }
        }
    } else {
        // A-03 (w-probe-r24 fix round 2): the visit budget was denied. Do
        // not conflate this with positively finding no individual -- leave
        // identity unresolved instead. Since w-probe-r56 only this run's own
        // budget can deny it.
        out.input.individualReadOk = false;
        out.visitAllowanceShort = true;
    }
    return out;
}

// w-probe-r24 (W-5): the rule run of one press: its read, its plan counters
// and the receipt fields of the js_rule_run / js_rule_candidate rows.
// S2c: the plan is sold as one batch (BatchRun below); the
// per-candidate sequencer and its IssueFromSnapshot hand-off are gone.
struct RuleRunState {
    bool active{};
    std::uint64_t runToken{};
    // design hand review (w27 A-01): the BarterMenu session and load generation
    // the plan was read in; the run is cancelled once either moves on.
    std::uint64_t menuGeneration{}, loadGeneration{};
    std::size_t candidatesRead{}, candidatesEligible{};
    std::uint64_t listEntries{}, readUs{};
    const char* readReason{"none"};
    bool reading{};
    js::RulePageState page{};
    std::vector<RuleGfxRead> reads{};
    std::vector<js::GfxListCandidateInput> inputs{};
    bool cachedBeforeUsable{};
    // w-probe-r33 (R38-1): rows that passed every exclusion except the run
    // limit, decided with the plan (WSelect.h RuleCandidatesSelectable).
    std::size_t candidatesSelectable{};
    js::CandidateRowSummary candidateRows{};
    std::size_t excludedNotDisplayed{}, excludedNoQuote{}, excludedIdentityUnresolved{},
        excludedFactsUnreadable{};
    std::int32_t issued{}, confirmed{};
    bool capReached{};
    std::string attemptTerminals{};
    std::uint64_t healthDropped{};
    bool fundsInsufficient{};
    // w-probe-r26 (R27d-2): the run's own merchant-balance fact, decided
    // once at planning (WSelect.h RuleBalanceSource) and reused by the
    // pre-dispatch re-validation below, so both see the same balance.
    bool balanceKnown{};
    std::int64_t balanceValue{-1};
    const char* balanceSource{"none"};
    std::int64_t balanceInventoryDelta{-1};
    // w-probe-r27 (R28-1): the sink's drop counter at this run's start, so
    // the run row can state how many hotkey edges were dropped while it was
    // live (edgesDroppedDuringRun).
    std::uint64_t edgesAtStart{};
    // w-probe-r27 fix round 2: the save generation this plan
    // belongs to. A save start after planning stops the run before its next
    // read or dispatch (save_started).
    std::uint64_t saveGeneration{};
    // w-probe-r28 (R29-1/R29-3): the one settings snapshot this run decided
    // with (thresholds for the selector and the two protection sources), the
    // config generation it belongs to -- a save that bumps the generation
    // after this point stops the run at its next dispatch check (R29-2) --
    // and the effective verdict token the run row carries (R29-3).
    RuleSettingsSnapshot settings{};
    std::uint64_t configGeneration{};
    const char* settingsReason{"OK"};
    // w-probe-r45: the run's stolen-goods acceptance, decided once in
    // BeginRuleRun from the merchant's vendor faction and the player's
    // BypassVendorStolenCheck (Oracle.h ReadStolenAcceptanceFacts), reused by
    // the pre-dispatch re-read and carried on each rule request.
    js::StolenAcceptance stolenAcceptance{js::StolenAcceptance::kRefused};
    // heavy-fix (w-probe-r56): this run's extra-list visit budget, fresh
    // with every RuleRunState (BeginRuleRun builds a new one per press).
    js::RuleXListBudget xListBudget{};
};
RuleRunState ruleRun{};
std::atomic<std::uint64_t> ruleRunTokenCounter{};
// w-probe-r24 fix round 2 (S-01): the pending "advance the rule run" request
// posted to the UI task; only one can be outstanding at a time since the
// sequencer only ever waits on one candidate's terminal.
std::atomic<bool> ruleAdvanceQueued{false};
std::atomic<bool> ruleReadQueued{false};
std::atomic<bool> ruleReading{false};
std::atomic<std::uint64_t> ruleReadNotBeforeNs{};
std::atomic<SaleTerminal> ruleAdvancePendingTerminal{SaleTerminal::kIssued};
// w-probe-r12 (R12-2): the id of the last sample that reached its js_oracle
// row. A settle sample id below this one that has still not arrived is lost,
// not slow, because samples publish in the order they are started. Declared
// here (w-probe-r44) because a reader above the oracle section needed it
// before its definition; MSVC rejected the later declaration (C2065).
std::atomic<std::uint64_t> lastPublishedSampleId{};
// w-probe-r27 (R28-1, fix round 2 audit A-001): pending <= 1 as one atomic
// claim state. The input sink claims an idle slot with compare_exchange before
// it queues the BeginRuleRun UI task (a queued or active slot drops the edge,
// counted in edgesDropped); the task publishes the claim as active when the
// run really starts, and every end path returns it to idle. The sink
// therefore reads no UI-task-owned object -- not the run struct and not its
// run-active bool -- and never reads a game object either.
std::atomic<js::SchedulerSlot> ruleRunSlot{js::SchedulerSlot::kIdle};
// w-probe-r27 fix round 2: the save-start generation. The
// kSaveGame message bumps it; a BeginRuleRun claim and a live run both record
// the generation they belong to, so a start queued before the save refuses at
// entry with save_started and a run planned before it stops before its next
// read or dispatch. Design section 5: no new issuance survives a save start.
std::atomic<std::uint64_t> saveGeneration{};
// w-probe-r34 (D-W08), w37 loop2 (S-001): saved settings-off requests are
// confirmed through the existing UI task queue. A bounded FIFO retains each
// accepted transition until the UI task writes its own receipt row.
std::atomic<bool> settingsStopRequested{};
std::atomic<bool> settingsStopTaskQueued{};
std::mutex settingsStopMutex;
js::SettingsStopQueue settingsStopQueue;

// ---------------------------------------------------------------------------
// stage S2c (the r2 design sections 2.1-2.11): one batch per
// press inside the BarterMenu. The pure half (plan, commit loop, row states,
// reconciliation, outcome) is Batch.h (S2a); this block is the engine side.
// Everything here is owned by the UI task, except BatchCapture, which the
// container sink also writes under its own mutex.
//   read (unchanged r50/r51 paged read)
//   -> ONE oracle request "batch_before" -> plan from its VENC balance
//   -> the per-press row cap (js::ApplyPressRowCap, r58)
//   -> commit tasks (js::RunCommitTask, 250 ms cap, spill to a later frame)
//   -> deferred rows only while rowsDeferred > 0
//   -> ONE oracle request "batch_after" (only when a row was dispatched)
//   -> reconcile -> classify -> one result line (ResultText.h) + receipts.
// ---------------------------------------------------------------------------
enum class BatchPhase : unsigned { kNone = 0, kBefore = 1, kCommit = 2, kDeferred = 3, kAfter = 4 };

// A container event the sink saw outside a Callback while the batch window
// was open: a late sale event (P0 false) or a foreign one.
struct BatchLateEvent {
    std::uint32_t oldContainer{}, newContainer{}, baseObj{};
    std::int32_t itemCount{};
};
constexpr std::size_t BatchLateEventCap = 1024;
// Spill pacing: a commit task that ended at the frame cap is followed by the
// next one no earlier than this (the RuleReadGapNs frame proxy of r50).
constexpr std::uint64_t BatchSpillGapNs = 16000000;

// The batch window's own event capture. While `open`, every container event
// goes here and never to the event ring, so a batch sale starts no
// sink_batch sample and no settle (design-r2 2.2: no per-row sample).
struct BatchCapture {
    std::mutex mutex;
    bool open{};
    bool inCall{};
    std::uint32_t playerId{}, vencId{};
    js::BatchRowEvents call{};
    std::vector<BatchLateEvent> late{};
    std::uint32_t lateDropped{};
} batchCapture;
// Mirror of batchCapture.open: the sink takes the lock only while it is set.
std::atomic<bool> batchCaptureOpen{false};

struct BatchRun {
    BatchPhase phase{BatchPhase::kNone};
    js::BatchState state{};
    // A valid plan was built (false: the press ends as blocked, unavailable).
    bool planned{};
    std::uint64_t runToken{}, edgeUs{};
    std::uint64_t menuGeneration{}, loadGeneration{}, saveGeneration{}, configGeneration{};
    std::uint64_t beforeSampleId{}, afterSampleId{};
    bool beforeValid{};
    std::int32_t beforePlayerGold{-1}, beforeVencGold{-1};
    std::uint32_t playerId{}, vencId{};
    const char* abort{"none"};
    js::ShadowPriceInputs shadow{};
    // v0.8.0: the batch's one price snapshot (plan time), the right-hand
    // side of the "Merchant check" pair, and its receipt block.
    js::PriceSnapshot priceSnapshot{};
    js::PriceSourceReceipt priceReceipt{};
    // Late events already attributed, and the accumulating events of the
    // dispatched rows still undecided, in plan order (P0 false only).
    std::size_t lateConsumed{};
    std::vector<js::BatchRowEvents> deferred{};
    std::uint32_t lateEvents{}, lateDropped{};
    std::uint64_t commitEndMs{};
    bool reconciled{}, reconcileOk{}, samplesValid{};
    std::int64_t playerDelta{}, vencDelta{};
};
BatchRun batchRun{};
// Pump-readable mirrors of the UI-task-owned phase and pacing.
std::atomic<BatchPhase> batchPhase{BatchPhase::kNone};
std::atomic<std::uint64_t> batchEdgeUs{}, batchNotBeforeNs{};
std::atomic<bool> batchTaskQueued{false};
// design-r2 2.11 P3: the main-thread gap after the last commit task.
std::atomic<std::int64_t> batchPostCommitGapMs{-1};
std::atomic<std::uint64_t> batchGapToken{};
// At most one outstanding "a sale is already running" line from the sink.
std::atomic<bool> runningLineQueued{false};
// Result text (S2b ResultText.h), parsed once at kDataLoaded, read-only after.
agh::junk_seller::result_text::Table resultLocalized{}, resultEnglish{};

// v0.1.0 (the v0.1.0 design r3 section 4): the one sell-anywhere
// run of a press. Owned by main-thread tasks (the BeginRuleRun UI task that
// gates, reads and plans it, then one AddTask per commit slice); the pump
// (writer thread) reads only the atomics below.
enum class ConvertPhase : unsigned { kNone = 0, kCommit = 1 };
struct ConvertRun {
    bool active{};
    std::uint64_t runToken{}, edgeUs{}, loadGeneration{}, saveGeneration{};
    js::ConvertState state{};
    js::ShadowPriceInputs inputs{};
    js::ConvertPrice price{};
    // v0.8.0: the press's one price snapshot (price == snapshot.price)
    // and its receipt block.
    js::PriceSnapshot snapshot{};
    js::PriceSourceReceipt priceReceipt{};
    js::SelectorThresholds thresholds{};
    js::ConvertGate gate{js::ConvertGate::kOk};
    std::uint64_t readUs{};
    std::int64_t goldBefore{-1};
    std::int32_t speechLevelBefore{-1};
    float speechXpBefore{std::numeric_limits<float>::quiet_NaN()};
    const char* settingsReason{"OK"};
};
ConvertRun convertRun{};
std::atomic<ConvertPhase> convertPhase{ConvertPhase::kNone};
std::atomic<std::uint64_t> convertNotBeforeNs{}, convertToken{};
std::atomic<bool> convertTaskQueued{false};
// A MISMATCH or LOST latches the anywhere path until a save is loaded.
std::atomic<bool> convertLatched{false};
// Between kPreLoadGame and kPostLoadGame (the world gate's "loading" term).
std::atomic<bool> loadInProgress{false};
// The MCM "Last merchant check" pair (design r3 2.5); -1 = none since the
// last load. Written by FinishBatch, read by the GetLastMerchantCheck native.
std::atomic<std::int64_t> merchantCheckMerchantGold{-1}, merchantCheckAnywhereGold{-1};
// v0.2.0 (the v0.2 update plan r2 section 2): the category facts, built once
// at kDataLoaded on the main thread (ConvertEngine.h BuildConvertCatalog) and
// read-only afterwards; convertCatalogReady publishes it. A press before it
// is ready (or after a failed build) is refused by the world gate (runtime).
js::ConvertCatalog convertCatalog{};
std::atomic<bool> convertCatalogReady{false};
// v0.2.0: the five "why kept" counters of the last planned
// press (-1 before one), for the MCM Diagnostics page (GetLastKept native).
std::array<std::atomic<std::int32_t>, 5> lastKept{{-1, -1, -1, -1, -1}};
// v0.5.0 (the +5.7 s frame investigation): the OBSERVE-ONLY frame window
// (FrameWindow.h), live only with the base INI key DevFrameWindow=1 (the
// shipped file says 0). `frameArmed` makes the pump keep one tick task
// queued; the window itself is guarded by `frameMutex` because a container
// event can come from a Papyrus VM thread. Nothing here reads or changes a
// sale: a press plans, commits and ends exactly as without it.
std::atomic<bool> devFrameWindow{false};
// v0.8.0: the base INI developer key DevPerkFallback=1 (shipped 0) makes
// every snapshot with "Price follows perks" ON use the shipped formula with the
// reason dev_forced, for the live L1 of the fallback path.
std::atomic<bool> devPerkFallback{false};
// v0.8.0 (the barter-price design r2 section 3.4): the MCM "Rate now"
// preview. RequestAnywhereRatePreview (VM thread) moves the request sequence
// and queues one UI task; that task reads the price snapshot on the main
// thread and publishes the percent with the sequence it answers. A preview is
// ready only while the published sequence equals the requested one, so a load
// (which moves the request sequence) or a newer request hides an older value.
std::atomic<std::uint64_t> ratePreviewRequested{0}, ratePreviewPublished{0};
std::atomic<std::int32_t> ratePreviewPercent{-1};
// v0.8.0 (section 3.4 step 4): the price source of the last sale (a
// planned sell-anywhere press or an in-menu batch), js::PriceSource as an
// integer, -1 before one since the last load. The preview never writes it.
std::atomic<std::int32_t> lastPriceSource{-1};
std::atomic<bool> frameArmed{false};
std::atomic<bool> frameTickQueued{false};
std::atomic<bool> frameCounting{false};
std::atomic<std::uint32_t> frameContainerDuring{};
// v0.5.1 (receipt, least load): the player's container-change events
// while a planned sell-anywhere press runs (from its plan to its line), for
// every press, dev key or not. Counting only; nothing reads it but the
// press's js_convert_run row (containerEvents).
std::atomic<bool> convertCounting{false};
std::atomic<std::uint32_t> convertContainerEvents{};
std::atomic<std::uint64_t> frameEpoch{};
// When the last HUD line task ran (BatchNowUs; 0 = none since start).
std::atomic<std::uint64_t> lastLineShownUs{};
std::mutex frameMutex;
js::FrameWindow frameWindow{};

void SetBatchPhase(BatchPhase phase) noexcept {
    batchRun.phase = phase;
    batchPhase.store(phase, std::memory_order_release);
}
// Monotonic microseconds for the batch (the key edge, the commit clock).
std::uint64_t BatchNowUs() noexcept { return NowNs() / 1000; }
void FinishBatch(const char* runReason, const char* abort);
void RequestBatchBefore();
void StartBatchFromSelection(const js::RuleSelectionRun& selection);

// The claim's two transitions. Both are called on the UI task that owns the
// run; the atomics are what the sink observes.
void ReleaseRuleRunSlot() noexcept {
    ruleRunSlot.store(js::SchedulerSlot::kIdle, std::memory_order_release);
}
void PublishRuleRunActive() noexcept {
    ruleRunSlot.store(js::SchedulerSlot::kActive, std::memory_order_release);
}

void QueueSettingsStopConfirmation();
void ConfirmSettingsStop();
// v0.1.0: ends a live sell-anywhere run (defined with the convert section).
void FinishConvertRun(const char* abort);
void CancelRuleRun(const char* reason);

bool EnqueueSettingsStopRequest(const js::SettingsStopRequest& request) {
    std::lock_guard<std::mutex> lock(settingsStopMutex);
    if (!settingsStopQueue.Push(request)) return false;
    settingsStopRequested.store(true, std::memory_order_release);
    return true;
}

bool SettingsStopRequestsPending() {
    std::lock_guard<std::mutex> lock(settingsStopMutex);
    return !settingsStopQueue.Empty();
}

void RecordSettingsStopCancellation() {
    std::lock_guard<std::mutex> lock(settingsStopMutex);
    settingsStopQueue.AddCancelled();
}

std::size_t DrainSettingsStopRequests(
    std::array<js::SettingsStopRequest, js::SettingsStopQueueCapacity>& drained) {
    std::lock_guard<std::mutex> lock(settingsStopMutex);
    std::size_t count = 0;
    while (count < drained.size() && settingsStopQueue.Pop(drained[count])) ++count;
    // The guard is cleared only after this task has removed every queued
    // request. A later apply sees false and posts a fresh confirmation task.
    settingsStopRequested.store(false, std::memory_order_release);
    settingsStopTaskQueued.store(false, std::memory_order_release);
    return count;
}

void EmitSettingsStopRow(const js::SettingsStopRequest& request, unsigned pendingAfter) {
    const auto now = NowNs();
    const auto idleUs = request.applyNs && now >= request.applyNs ?
        (now - request.applyNs) / 1000 : 1000001;
    const bool idle = js::RuleStopIdle(pendingAfter, idleUs);
    auto row = NewRow("js_stop", "settings", Phase::kUiTask);
    row.reason = "ENABLED_OFF";
    row.configGeneration = request.configGeneration;
    row.stopPendingBefore = request.pendingBefore;
    row.stopPendingAfter = pendingAfter;
    row.stopCancelled = request.cancelled;
    row.stopIdleUs = idleUs;
    row.stopIdle = idle;
    row.stopLatched = js::RuleStopLatched(idle);
    Log(row);
    if (row.stopLatched) issueLatched.store(true);
}

void EmitDrainedSettingsStopRows(unsigned pending) {
    std::array<js::SettingsStopRequest, js::SettingsStopQueueCapacity> drained{};
    const auto count = DrainSettingsStopRequests(drained);
    for (std::size_t index = 0; index < count; ++index) {
        EmitSettingsStopRow(drained[index], pending);
    }
}

void FailSettingsStopQueue() {
    const auto pending = js::RuleStopPendingCount(ruleRunSlot.load(), ruleAdvanceQueued.load(),
                                                  issueInFlight.load());
    EmitDrainedSettingsStopRows(pending);
}

bool PostSettingsStopConfirmation() {
    auto* tasks = SKSE::GetTaskInterface();
    if (!tasks) return false;
    try {
        tasks->AddUITask([] {
            try { ConfirmSettingsStop(); }
            catch (...) { Warning("STOP_CONFIRM_EXCEPTION"); FailSettingsStopQueue(); }
        });
        return true;
    } catch (...) {
        ++queueFailures;
        return false;
    }
}

void ConfirmSettingsStop() {
    EnterUiTask();
    const auto slot = ruleRunSlot.load(std::memory_order_acquire);
    if (slot == js::SchedulerSlot::kActive && ruleRun.active) {
        RecordSettingsStopCancellation();
        CancelRuleRun("run_invalidated");
    }
    // audit B-01: a live sell-anywhere run is the slot's other owner; it
    // stops before its next slice (slices are atomic) and frees the slot.
    if (slot == js::SchedulerSlot::kActive && convertRun.active) {
        RecordSettingsStopCancellation();
        js::StopConvert(convertRun.state, js::ConvertStop::kGate, js::ConvertGate::kDisabled);
        FinishConvertRun("SETTINGS_STOP");
    }
    const auto pending = js::RuleStopPendingCount(ruleRunSlot.load(std::memory_order_acquire),
                                                   ruleAdvanceQueued.load(), issueInFlight.load());
    if (pending != 0) {
        // Keep the guard while this task posts its continuation. This is one
        // confirmation path, not a second scheduler: applies append to the
        // FIFO and the eventual idle task drains it in FIFO order.
        if (!PostSettingsStopConfirmation()) FailSettingsStopQueue();
        return;
    }
    EmitDrainedSettingsStopRows(pending);
}

void QueueSettingsStopConfirmation() {
    if (settingsStopTaskQueued.exchange(true, std::memory_order_acq_rel)) return;
    if (!PostSettingsStopConfirmation()) FailSettingsStopQueue();
}

void EmitRuleCandidateRow(const RuleRunState& run, std::size_t index,
                         const js::GfxListCandidateInput& input, std::int32_t filteredIndex,
                         const std::array<char, TextBytes>& name, const char* decision,
                         const char* reason, unsigned bindWrites = 0,
                         bool bindReadbackMatch = false, bool indexDrift = false,
                         std::int32_t plannedIndex = -1) {
    auto row = NewRow("js_rule_candidate", "rule_run", Phase::kUiTask);
    // w-probe-r24 fix round 5 (S-01): this candidate's own binding evidence,
    // the same two facts the js_bind row already records for the BindKey
    // path. Zero writes on an EXCLUDE row that never reached the bind.
    // w-probe-r26 (R27d-2): merchantGold is the run's own balance, not a
    // per-candidate read; it stays -1 while the run has no balance (R-W21
    // measured 0 here, which read as "a merchant with no gold").
    row.bindWrites = bindWrites;
    row.bindReadbackMatch = bindReadbackMatch;
    row.merchantGold = static_cast<std::int32_t>(run.balanceKnown ? run.balanceValue : -1);
    row.runToken = run.runToken;
    row.issueSource = "rule";
    row.ruleCandidateIndex = static_cast<std::int32_t>(index);
    row.ruleCandidateBaseObj = input.baseObj;
    row.ruleCandidateHasIndividual = input.hasIndividual;
    row.ruleCandidateFilteredIndex = filteredIndex;
    row.ruleCandidateIndexDrift = indexDrift;
    row.ruleCandidatePlannedIndex = plannedIndex >= 0 ? plannedIndex : filteredIndex;
    row.ruleCandidateUnitPrice = input.infoValue;
    row.ruleCandidateName = name;
    row.ruleCandidateDecision = decision;
    row.ruleCandidateReason = reason;
    Log(row);
}

// w-probe-r28 (R29-3): one line built from the js_rule_run values already in
// memory, so the r29 Diagnostics page can name the last run without parsing a
// receipt. Written under lastRunMutex and read by the GetLastRunSummary
// native; "none" until the first run.
void RecordLastRunSummary(const char* gate, std::int32_t read, std::int32_t issued,
                         std::int32_t confirmed, const char* runReason,
                         const char* attemptTerminals,
                         const char* settingsReason) {
    std::string line = "gate=";
    line += (gate && gate[0]) ? gate : "none";
    line += " settings=";
    line += (settingsReason && settingsReason[0]) ? settingsReason : "none";
    line += " candidatesRead=" + std::to_string(read);
    line += " issued=" + std::to_string(issued);
    line += " confirmed=" + std::to_string(confirmed);
    line += " runReason=";
    line += (runReason && runReason[0]) ? runReason : "none";
    line += " attemptTerminals=";
    line += (attemptTerminals && attemptTerminals[0]) ? attemptTerminals : "none";
    line += " terminal=";
    line += (attemptTerminals && attemptTerminals[0]) ? attemptTerminals : "none";
    std::lock_guard<std::mutex> lock(lastRunMutex);
    CopyText(lastRunSummary, line.c_str());
}

const char* RuleRunReason(const char* reason) {
    if (!reason || !reason[0]) return "ERROR";
    if (std::strcmp(reason, "menu_closed") == 0) return "MENU_CLOSED";
    if (std::strcmp(reason, "save_started") == 0) return "SAVE_STARTED";
    if (std::strcmp(reason, "load_changed") == 0) return "LOAD_CHANGED";
    if (std::strcmp(reason, "run_invalidated") == 0) return "ERROR";
    if (std::strcmp(reason, "no_candidates") == 0) return "NO_CANDIDATES";
    if (std::strcmp(reason, "error") == 0) return "ERROR";
    return reason;
}

void RecordRuleFunds(const char* source, bool insufficient) {
    if (source && (std::strcmp(source, "cached_before") == 0 ||
                   std::strcmp(source, "last_sample") == 0 ||
                   std::strcmp(source, "venc_slot") == 0)) {
        ++fundsKnownCount;
    } else {
        ++fundsUnknownCount;
    }
    if (insufficient) ++fundsInsufficientCount;
}

void StampRuleOracleVerdict(Row& row) {
    auto& verdict = row.ruleOracleVerdict;
    verdict.lastSampleId = lastSample.sampleId.load();
    verdict.lastOrigin = lastSample.origin.load();
    verdict.lastSampleValid = lastSample.publishedValid.load();
    verdict.lastInvalidReason = lastSample.invalidReason.load();
    verdict.lastTargeted = lastSample.targeted.load();
    for (std::size_t i = 0; i < OracleStageSlots; ++i)
        verdict.lastStageCostUs[i] = lastSample.stageCostUs[i].load();
    for (std::size_t i = 0; i < OracleStageSlots; ++i)
        verdict.lastChunks[i] = lastSample.stageChunks[i].load();
    verdict.lastRestarts = lastSample.walkRestarts.load();
    verdict.lastScanned = lastSample.scanned.load();
    verdict.lastBaseCountVisits = lastSample.baseCountVisits.load();
    verdict.lastTruncatedStages = lastSample.truncatedStages.load();
    verdict.lastUnsampled = lastSample.unsampled.load();
    verdict.lastSpanUs = lastSample.spanUs.load();
    verdict.menuSamples = menuOracleTally.samples;
    verdict.menuValid = menuOracleTally.valid;
    verdict.menuInvalid = menuOracleTally.invalid;
    verdict.cachedBeforeUsable = ruleRun.cachedBeforeUsable;
}

void FinishRuleRun(const char* runReason = "COMPLETED") {
    if (!ruleRun.active) return;
    auto row = NewRow("js_rule_run", "rule_run", Phase::kUiTask);
    row.runToken = ruleRun.runToken;
    row.issueSource = "rule";
    row.ruleCandidatesRead = static_cast<std::int32_t>(ruleRun.candidatesRead);
    row.ruleCandidatesEligible = static_cast<std::int32_t>(ruleRun.candidatesEligible);
    // w-probe-r33 (R38-1): the selectable count the plan was decided with,
    // written right after candidatesEligible.
    row.ruleCandidatesSelectable = static_cast<std::int32_t>(ruleRun.candidatesSelectable);
    row.ruleCandidateRows = ruleRun.candidateRows;
    row.ruleExcludedNotDisplayed = static_cast<std::int32_t>(ruleRun.excludedNotDisplayed);
    row.ruleExcludedNoQuote = static_cast<std::int32_t>(ruleRun.excludedNoQuote);
    row.ruleExcludedIdentityUnresolved = static_cast<std::int32_t>(ruleRun.excludedIdentityUnresolved);
    row.ruleExcludedFactsUnreadable = static_cast<std::int32_t>(ruleRun.excludedFactsUnreadable);
    row.ruleIssued = ruleRun.issued;
    row.ruleConfirmed = ruleRun.confirmed;
    row.ruleCapReached = ruleRun.capReached;
    row.ruleRunReason = RuleRunReason(runReason);
    CopyText(row.ruleAttemptTerminals, ruleRun.attemptTerminals.c_str());
    CopyText(row.ruleTerminalReasons, ruleRun.attemptTerminals.c_str());
    row.ruleHealthDropped = ruleRun.healthDropped;
    row.ruleListEntries = ruleRun.listEntries;
    row.ruleReadUs = ruleRun.readUs;
    row.ruleReadReason = ruleRun.readReason;
    StampRuleOracleVerdict(row);
    // w-probe-r26 (R27d-2): the balance the run decided from and its source.
    row.ruleBalanceSource = ruleRun.balanceSource;
    row.ruleBalanceValue = ruleRun.balanceValue;
    row.ruleBalanceInventoryDelta = ruleRun.balanceInventoryDelta;
    // w-probe-r27 (R28-1): a run that reached this point passed the D-W07
    // foreground gate, and its own drop delta closes here.
    row.ruleGateReason = "OK";
    row.ruleEdgesDroppedDuringRun = edgesDropped.load() - ruleRun.edgesAtStart;
    // w-probe-r28 (R29-1/R29-3): the run's settings verdict, taken from the
    // snapshot it decided with, and the one-line summary the Diagnostics page
    // reads back.
    row.ruleSettingsReason = ruleRun.settingsReason;
    RecordRuleFunds(ruleRun.balanceSource, ruleRun.fundsInsufficient);
    RecordLastRunSummary("OK", row.ruleCandidatesRead, row.ruleIssued, row.ruleConfirmed,
                         row.ruleRunReason, ruleRun.attemptTerminals.c_str(), ruleRun.settingsReason);
    Log(row);
    ruleReading.store(false, std::memory_order_release);
    ruleRun = RuleRunState{};
    ruleAdvancePendingTerminal.store(SaleTerminal::kIssued);
    // w-probe-r27 fix round 2: the run is over, so the claim
    // returns to idle and the next hotkey edge may queue. Every end path --
    // cancelled, refused, completed -- passes through here.
    ReleaseRuleRunSlot();
}

// w-probe-r27 (R28-1, review fix item 2): a refused run still leaves a
// js_rule_run row carrying its one gate reason, so "nothing happened" is never
// the only evidence. There is no run to count drops for, so issued,
// candidatesRead and edgesDroppedDuringRun stay 0 (RULE-11); the session total
// is on HEALTH.edgesDropped.
void EmitGatedRuleRunRow(const char* gateReason, const char* settingsReason = "OK") {
    auto row = NewRow("js_rule_run", "rule_run", Phase::kUiTask);
    row.issueSource = "rule";
    row.ruleGateReason = gateReason;
    row.ruleRunReason = RuleRunReason(gateReason);
    row.ruleAttemptTerminals = {};
    row.ruleTerminalReasons = {};
    // w-probe-r28 (R29-1/R29-3): a refused run still states the settings
    // verdict of its snapshot; issued and candidatesRead stay 0 (RULE-11).
    row.ruleSettingsReason = settingsReason;
    // Gated runs did not read the list; the Row defaults keep all verdict
    // fields at their empty values.
    row.ruleListEntries = 0;
    row.ruleReadUs = 0;
    row.ruleOracleVerdict = {};
    RecordRuleFunds("none", false);
    RecordLastRunSummary(gateReason, 0, 0, 0, row.ruleRunReason, "none", settingsReason);
    Log(row);
}

// design hand review (w27 A-01): the terminal of the candidate the run is waiting
// on, folded into the run's counters exactly once (by the queued advance
// task, or by the cancel below when the menu closes first).
void FoldRuleTerminal(SaleTerminal terminal) {
    if (!TerminalPreDispatch(terminal)) ++ruleRun.issued;
    if (terminal == SaleTerminal::kConfirmed) ++ruleRun.confirmed;
    const auto* terminalName = SaleTerminalName(terminal);
    if (!ruleRun.attemptTerminals.empty()) ruleRun.attemptTerminals += ",";
    ruleRun.attemptTerminals += terminalName;
    if (terminal == SaleTerminal::kInsufficientFunds) ruleRun.fundsInsufficient = true;
}

bool RuleRunStale() {
    return ruleRun.active && js::RuleRunInvalidated(barterOpen.load(), ruleRun.menuGeneration,
                                                     menuGeneration.load(), ruleRun.loadGeneration,
                                                     epoch.load());
}

// Ends the run without dispatching anything further: a queued advance whose
// terminal has not been folded yet is folded here, and the js_rule_run row is
// written now with the run reason separate from the attempt terminal list.
// S2c: the run ends through FinishBatch, so a cancelled press still gets
// its js_batch row and its one result line (none after a load).
void CancelRuleRun(const char* reason) {
    if (!ruleRun.active) return;
    if (ruleAdvanceQueued.load() || ruleAdvancePendingTerminal.load() != SaleTerminal::kIssued) {
        FoldRuleTerminal(ruleAdvancePendingTerminal.load());
    }
    ruleAdvancePendingTerminal.store(SaleTerminal::kIssued);
    if (ruleRun.reading) ruleRun.readReason = reason;
    FinishBatch(RuleRunReason(reason), reason);
}

// w-probe-r27 (R28-1): the topmost question as the live read answers it, with
// the same UI:: calls FillWPhase above already makes (UI.h:83-102). True only
// when GetTopMostMenu positively named a front menu that is not the
// BarterMenu. fix round 2: R-W5 measured this call returning
// null while the BarterMenu was the only and topmost menu, so a null read is
// "nothing named", not "another menu in front", and never refuses on this
// term. This is the one engine part of the scheduler gate; the decision
// itself is the engine-free js::SchedulerGateReason (WSelect.h), so the host
// test covers every reason.
bool LiveTopmostOther(RE::UI* ui) noexcept {
    if (!ui) return false;
    RE::IMenu* top = nullptr;
    ui->GetTopMostMenu(&top, 0);
    return js::SchedulerTopmostOther(top, ui->GetMenu<RE::BarterMenu>().get());
}

// The live foreground phase the scheduler gate reads, filled from the same
// UI:: fields FillWPhase records. The four request gates are the probe's own
// atomics (no RE read); the fail-closed default (everything false but
// barterOpen false too) refuses rather than guesses when the UI singleton is
// gone.
void FillSchedulerPhase(js::SchedulerPhase& out) noexcept {
    out = js::SchedulerPhase{};
    auto* ui = RE::UI::GetSingleton();
    if (!ui) return;
    out.barterOpen = ui->IsMenuOpen(RE::BarterMenu::MENU_NAME);
    out.topmostOther = LiveTopmostOther(ui);
    out.modalOpen = ui->IsModalMenuOpen();
    out.quantityOpen = ui->IsMenuOpen("QuantityMenu");
    out.messageOpen = ui->IsMenuOpen("MessageBoxMenu");
    out.consoleOpen = ui->IsMenuOpen("Console");
    out.issueInFlight = issueInFlight.load();
    out.issueLatched = issueLatched.load();
    out.requestPending = pendingRequest.valid;
}

// Hotkey entry point for this stage: builds the candidate
// list from the live js_gfx_list read, applies the unchanged D-W06 Selector
// core, and queues the selected rows -- up to the run's own MaxItems (R29-1,
// w31 fix round 2 audit A-003) -- for sequential issuance. The
    // probe's hotkey-only path is untouched and
// stays the default hotkey behaviour outside this stage.
// w-probe-r27 (R28-1): the menu generation the task was queued in is compared
// at entry, and the D-W07 foreground gate is asked before anything is read.
// Every refusal writes a js_rule_run row with its one gate reason.
// fix round 2: a start queued before a save start refuses here
// with save_started, so a save can never be entered with a start still in
// flight. Every return of this task returns the claim to idle; only a run
// that really starts publishes it as active.
// Reading resumes on UI tasks paced by the writer pump.
void ReadRuleChunk();

// S2c: runs on the publish task of the batch's before sample. `before`
// is that sample when it is usable; the plan then cuts rows by its VENC
// balance (design-r2 2.6). Without it the r48 balance source is kept for the
// receipt and the batch ends before any dispatch (BEFORE_UNAVAILABLE).
void PlanRuleRun(const OracleSample* before) {
    const auto planStarted = NowNs();
    // w-probe-r26 (R27d-2/R27d-3): the run's balance. cachedBeforeSample
    // stays the first choice so the bind path is unchanged wherever that
    // cache is usable for this menu session; a rule run in a fresh menu
    // (no bind target yet, so no cache) falls back to the last valid sample
    // of the same menu/load generation. The inventory generation is recorded
    // as a delta, not required equal (R-W21 anomaly 1).
    js::RuleBalanceCandidate cachedBefore;
    cachedBefore.valid = cachedBeforeSample.sampleValid;
    cachedBefore.gold = cachedBeforeSample.merchantContainerGold;
    cachedBefore.startMenuGeneration = cachedBeforeSample.startMenuGeneration;
    cachedBefore.endMenuGeneration = cachedBeforeSample.endMenuGeneration;
    cachedBefore.startLoadGeneration = cachedBeforeSample.startLoadGeneration;
    cachedBefore.endLoadGeneration = cachedBeforeSample.endLoadGeneration;
    cachedBefore.endInventoryGeneration = cachedBeforeSample.endInventoryGeneration;
    js::RuleBalanceCandidate lastRead;
    lastRead.valid = lastSample.valid.load();
    lastRead.gold = lastSample.merchantContainerGold.load();
    lastRead.startMenuGeneration = lastSample.menuGeneration.load();
    lastRead.endMenuGeneration = lastRead.startMenuGeneration;
    lastRead.startLoadGeneration = lastSample.loadGeneration.load();
    lastRead.endLoadGeneration = lastRead.startLoadGeneration;
    lastRead.endInventoryGeneration = lastSample.inventoryGeneration.load();
    js::RuleBalanceCandidate slotRead;
    slotRead.valid = vencSlotBalance.complete && vencSlotBalance.sampleId != 0 &&
                     vencSlotBalance.merchantContainerID != 0 &&
                     js::RuleVencSlotInventoryStable(vencSlotBalance.startInventoryGeneration,
                                                     vencSlotBalance.endInventoryGeneration);
    slotRead.gold = vencSlotBalance.gold;
    slotRead.startMenuGeneration = vencSlotBalance.startMenuGeneration;
    slotRead.endMenuGeneration = vencSlotBalance.endMenuGeneration;
    slotRead.startLoadGeneration = vencSlotBalance.startLoadGeneration;
    slotRead.endLoadGeneration = vencSlotBalance.endLoadGeneration;
    slotRead.endInventoryGeneration = vencSlotBalance.endInventoryGeneration;
    auto balance = js::RuleBalanceSource(cachedBefore, lastRead, slotRead, ruleRun.menuGeneration,
                                              ruleRun.loadGeneration, inventoryGeneration.load());
    if (before) {
        // The batch's own before sample is the last published sample of this
        // menu and load generation, taken after the read: its VENC gold is
        // the balance the whole plan is cut by.
        balance.known = true;
        balance.value = before->merchantContainerGold;
        balance.source = js::RuleBalanceOrigin::kLastSample;
        balance.inventoryDelta = static_cast<std::int64_t>(inventoryGeneration.load()) -
                                 static_cast<std::int64_t>(before->endInventoryGeneration);
    }
    ruleRun.balanceKnown = balance.known;
    ruleRun.balanceValue = balance.value;
    ruleRun.balanceSource = js::RuleBalanceOriginName(balance.source);
    ruleRun.balanceInventoryDelta = balance.inventoryDelta;
    ruleRun.cachedBeforeUsable = balance.source == js::RuleBalanceOrigin::kCachedBefore;
    // w-probe-r28 (R29-3, item 6): the selector thresholds come from this
    // run's snapshot. With the default snapshot this is byte-for-byte the
    // accepted T-W06 decision (the host case asserts it).
    for (auto& input : ruleRun.inputs) {
        input.merchant.vencBalanceKnown = balance.known;
        input.merchant.vencBalance = balance.value;
        input.merchant.unitPriceKnown = input.infoValueKnown;
        input.merchant.unitPrice = input.infoValueKnown ? static_cast<int>(input.infoValue) : 0;
    }
    for (std::size_t i = 0; i < ruleRun.inputs.size(); ++i)
        ruleRun.reads[i].input = ruleRun.inputs[i];
    const auto selection = js::SelectRuleCandidates(
        ruleRun.inputs, js::DefaultSelectorContract, js::SelectorThresholdsFromSettings(ruleRun.settings.values));
    for (std::size_t i = 0; i < selection.selection.resultCount; ++i) {
        const auto term = selection.selection.results[i].term;
        if (term == js::SelectorTerm::kBalance || term == js::SelectorTerm::kBalanceUnknown) {
            ruleRun.fundsInsufficient = true;
            break;
        }
    }
    ruleRun.candidatesRead = selection.candidatesRead;
    ruleRun.candidatesEligible = selection.candidatesEligible;
    ruleRun.candidatesSelectable = selection.candidatesSelectable;
    ruleRun.excludedNotDisplayed = selection.excludedNotDisplayed;
    // Hidden rows had no candidate receipt before this revision either.
    ruleRun.candidateRows.excludedByTerm[20] = static_cast<std::uint32_t>(selection.excludedNotDisplayed);
    ruleRun.excludedNoQuote = selection.excludedNoQuote;
    ruleRun.excludedIdentityUnresolved = selection.excludedIdentityUnresolved;
    ruleRun.excludedFactsUnreadable = selection.excludedFactsUnreadable;
    // w31 fix round 2: the plan cap is this run's own MaxItems
    // (R29-1), bounded by SelectorMaxTransactions. w-probe-r31 (R31-1): the
    // cap is spent inside SelectCandidates on selected rows, so the rows it
    // cut are the selector's own kRemainder / kRunLimit results and
    // capReached is exactly that fact.
    ruleRun.capReached = js::RuleRunLimitReached(selection.selection);
    // Log EXCLUDE rows for every considered candidate the selector did not
    // pick. S2c: the selected rows are the batch plan; each one gets a
    // slim js_batch_row at the end of the batch instead of an ISSUE row.
    // design hand review (w27 B-02): a selector-eligible entry beyond this run's
    // own MaxItems cap is reported as EXCLUDE run_limit (results and
    // selectedIndices share the selector's list order), so the receipt shows
    // which eligible rows the cap cut instead of omitting them. w-probe-r31
    // (R31-1): that row already leaves the selector as kRemainder / kRunLimit,
    // so this loop only states each result's own term -- it never re-labels a
    // selected row, and no eligible row can be omitted for its position.
    for (std::size_t i = 0; i < ruleRun.reads.size(); ++i) {
        const auto& read = ruleRun.reads[i];
        if (!read.input.filteredIndexKnown) continue;
        const auto built = js::BuildRuleCandidate(read.input);
        if (built.failure != js::CandidateReadFailure::kNone) {
            const char* reason = js::CandidateReadFailureName(built.failure);
            if (ruleRun.candidateRows.RecordExclude(reason))
                EmitRuleCandidateRow(ruleRun, i, read.input, read.filteredIndex, read.name, "EXCLUDE", reason);
        }
    }
    for (std::size_t r = 0; r < selection.selection.resultCount && r < selection.candidates.size(); ++r) {
        const auto& result = selection.selection.results[r];
        if (result.decision == js::SelectorDecision::kSelect) continue;
        const auto& candidate = selection.candidates[r];
        const auto& read = ruleRun.reads[candidate.sourceIndex];
        const char* reason = js::SelectorTermName(result.term);
        if (ruleRun.candidateRows.RecordExclude(reason))
            EmitRuleCandidateRow(ruleRun, r, read.input, read.filteredIndex, read.name, "EXCLUDE", reason);
    }
    ruleRun.readUs += (NowNs() - planStarted) / 1000;
    batchRun.beforeValid = before != nullptr;
    StartBatchFromSelection(selection);
}

void ReadRuleChunk() {
    if (!ruleRun.active || !ruleRun.reading) return;
    auto* list = CurrentItemList();
    const auto listAddress = reinterpret_cast<std::uintptr_t>(list);
    const auto size = list ? static_cast<std::size_t>(list->items.size()) : 0;
    const auto anchor = list && ruleRun.page.cursor && size >= ruleRun.page.cursor
        ? reinterpret_cast<std::uintptr_t>(list->items[static_cast<std::uint32_t>(ruleRun.page.cursor - 1)]) : 0;
    const js::RulePageWindow current{
        barterOpen.load() && activeItemList.load() == listAddress,
        menuGeneration.load(), epoch.load(), saveGeneration.load(), configGeneration.load(),
        listAddress, size, anchor};
    const auto start = js::BeginRulePage(ruleRun.page, current);
    if (start == js::RulePageResult::kInvalidated) {
        if (SettingsStopRequestsPending() && ruleRun.configGeneration != configGeneration.load())
            RecordSettingsStopCancellation();
        CancelRuleRun("run_invalidated"); return;
    }
    if (start == js::RulePageResult::kError) {
        ruleRun.readReason = "chunk_limit"; FinishBatch("ERROR", "chunk_limit"); return;
    }
    if (start == js::RulePageResult::kComplete) {
        ruleRun.reading = false; ruleReading.store(false, std::memory_order_release);
        RequestBatchBefore(); return;
    }
    const auto previousCursor = ruleRun.page.cursor;
    const auto previousAnchor = ruleRun.page.anchorAddress;
    const auto started = NowNs();
    const auto deadline = started + js::RuleReadBudgetNs;
    while (ruleRun.page.cursor < ruleRun.page.window.entries) {
        const auto i = ruleRun.page.cursor;
        auto read = ReadRuleCandidateEntry(list->items[static_cast<std::uint32_t>(i)],
                                           ruleRun.settings, ruleRun.stolenAcceptance,
                                           ruleRun.xListBudget);
        if (read.input.filteredIndexKnown && (read.readDenied || read.visitAllowanceShort))
            ++ruleRun.healthDropped;
        ruleRun.inputs.push_back(read.input);
        ruleRun.reads.push_back(std::move(read));
        ++ruleRun.page.cursor;
        if (ruleRun.page.cursor % js::RuleReadDeadlineStride == 0 && NowNs() >= deadline) break;
    }
    const auto finished = NowNs();
    ruleRun.readUs += (finished - started) / 1000;
    const auto nextAnchor = ruleRun.page.cursor
        ? reinterpret_cast<std::uintptr_t>(list->items[static_cast<std::uint32_t>(ruleRun.page.cursor - 1)]) : 0;
    // Recheck after engine reads before publishing a plan from this chunk.
    const bool stillValid = !RuleRunStale() &&
        !js::SaveStartedSince(ruleRun.saveGeneration, saveGeneration.load()) &&
        ruleRun.configGeneration == configGeneration.load() &&
        activeItemList.load() == listAddress && CurrentItemList() == list &&
        static_cast<std::size_t>(list->items.size()) == ruleRun.page.window.entries &&
        (!previousCursor || previousAnchor ==
            reinterpret_cast<std::uintptr_t>(list->items[static_cast<std::uint32_t>(previousCursor - 1)])) &&
        (!ruleRun.page.cursor || nextAnchor ==
            reinterpret_cast<std::uintptr_t>(list->items[static_cast<std::uint32_t>(ruleRun.page.cursor - 1)]));
    if (!stillValid) { CancelRuleRun("run_invalidated"); return; }
    const auto result = js::EndRulePage(ruleRun.page, ruleRun.page.cursor, nextAnchor, finished);
    if (result == js::RulePageResult::kContinue) {
        ruleReadNotBeforeNs.store(ruleRun.page.notBeforeNs, std::memory_order_release);
        ruleReading.store(true, std::memory_order_release);
    }
    if (result == js::RulePageResult::kComplete) {
        ruleRun.reading = false;
        ruleReading.store(false, std::memory_order_release);
        RequestBatchBefore();
    }
}

// S2c: the one press outcome of a refused start (design-r2 2.10), from
// the live facts; defined with the batch section below.
js::BatchPressFacts CollectPressFacts(const js::SchedulerPhase& phase);
void EmitPressOutcome(js::BatchPressFacts facts, const char* abort);
// v0.1.0: the sell-anywhere press (defined after FinishBatch).
void BeginConvertRun(std::uint64_t edgeUs);

// S2c: `edgeUs` is the key edge (BatchNowUs at the input sink), the start
// of the batch watchdog (design-r2 2.3).
void BeginRuleRun(std::uint64_t queuedMenuGeneration, std::uint64_t queuedSaveGeneration,
                 std::uint64_t queuedConfigGeneration, std::uint64_t edgeUs, std::uint64_t queuedLoadGeneration) {
    EnterUiTask();
    if (ruleRun.active) return;
    if (js::SaveStartedSince(queuedSaveGeneration, saveGeneration.load())) {
        EmitGatedRuleRunRow("save_started");
        ReleaseRuleRunSlot();
        return;
    }
    if (queuedMenuGeneration != menuGeneration.load()) {
        EmitGatedRuleRunRow("run_invalidated");
        ReleaseRuleRunSlot();
        return;
    }
    // w-probe-r28 (R29-2): a start queued before a Set* saved names the older
    // config generation and refuses here, before any list read.
    if (queuedConfigGeneration != configGeneration.load()) {
        if (SettingsStopRequestsPending() &&
            queuedConfigGeneration < configGeneration.load()) RecordSettingsStopCancellation();
        EmitGatedRuleRunRow("run_invalidated");
        js::SchedulerPhase stalePhase{};
        FillSchedulerPhase(stalePhase);
        EmitPressOutcome(CollectPressFacts(stalePhase), "RUN_INVALIDATED");
        ReleaseRuleRunSlot();
        return;
    }
    js::SchedulerPhase phase{};
    FillSchedulerPhase(phase);
    // v0.1.0 (design r3 sections 1.2 and 1.4): with no BarterMenu open and
    // SellAnywhere ON the press is a sell-anywhere press; with SellAnywhere OFF
    // (the default) it stays the v0.0.4 BARTER_CLOSED refusal below, no line.
    // Audit A-05: a press queued before a load never converts the loaded
    // game's inventory: the load generation of the key edge must still hold.
    if (!phase.barterOpen && LiveSettingsCopy().sellAnywhere) {
        if (queuedLoadGeneration != epoch.load()) {
            ReleaseRuleRunSlot();
            return;
        }
        BeginConvertRun(edgeUs);
        return;
    }
    const char* const gateReason = js::SchedulerGateReason(phase);
    if (!js::SchedulerGatePassed(gateReason)) {
        EmitGatedRuleRunRow(gateReason);
        EmitPressOutcome(CollectPressFacts(phase), gateReason);
        ReleaseRuleRunSlot();
        return;
    }
    // w-probe-r28 (R29-1/R29-3): the one settings snapshot of this run, taken
    // here and never re-read (item 2 / R29-3), and the effective verdict it
    // produces. The refusal happens before any list read, so a SETTINGS_OFF
    // row has candidatesRead 0 and issued 0 (RULE-12).
    const auto snapshot = TakeRuleSettingsSnapshot();
    const bool adapterProven = AdapterProven();
    const auto effective = js::EffectiveState(snapshot.values, RuntimeOk(), adapterProven,
                                              configCorrupt.load(), apiMismatch.load());
    // A failed adapter verdict is a live cache input, not only a run-local
    // refusal. This keeps GetStatus()/IsEffectivelyEnabled() aligned with the
    // SETTINGS_OFF row emitted below.
    effectiveOnCache.store(effective.on);
    effectiveReasonCache.store(effective.reason);
    if (!effective.on) {
        EmitGatedRuleRunRow("SETTINGS_OFF", effective.reason);
        EmitPressOutcome(CollectPressFacts(phase), effective.reason);
        ReleaseRuleRunSlot();
        return;
    }
    auto* list = CurrentItemList();
    RuleRunState run{};
    run.active = true;
    run.runToken = ++ruleRunTokenCounter;
    run.menuGeneration = menuGeneration.load();
    run.loadGeneration = epoch.load();
    run.saveGeneration = saveGeneration.load();
    run.edgesAtStart = edgesDropped.load();
    run.settings = snapshot;
    run.configGeneration = snapshot.configGeneration;
    run.settingsReason = effective.reason;
    // S2c: the batch of this press, with the generations its commit
    // re-checks before every row (design-r2 2.2).
    batchRun = BatchRun{};
    batchRun.runToken = run.runToken;
    batchRun.edgeUs = edgeUs;
    batchRun.menuGeneration = run.menuGeneration;
    batchRun.loadGeneration = run.loadGeneration;
    batchRun.saveGeneration = run.saveGeneration;
    batchRun.configGeneration = run.configGeneration;
    batchEdgeUs.store(edgeUs, std::memory_order_release);
    if (!list) {
        // ADAPTER_UNAVAILABLE (design item 1): a list read that fails
        // entirely dispatches nothing.
        ruleRun = run;
        FinishBatch("ERROR", "NO_ITEMLIST");
        return;
    }
    run.listEntries = list->items.size();
    run.stolenAcceptance = js::DecideStolenAcceptance(
        js::ReadStolenAcceptanceFacts(js::MerchantFallback != 0));
    run.reading = true;
    run.page.window = js::RulePageWindow{
        barterOpen.load(), run.menuGeneration, run.loadGeneration, run.saveGeneration,
        run.configGeneration, reinterpret_cast<std::uintptr_t>(list),
        static_cast<std::size_t>(list->items.size()), 0};
    ruleRun = std::move(run);
    PublishRuleRunActive();
    ReadRuleChunk();
}

// A recorded request whose oracle job has not published inside the TTL (its
// queued slot was taken, or the job is slow) is dropped, fail-closed.
void ExpirePendingRequest() {
    if (!pendingRequest.valid) return;
    const auto now = NowMs();
    if (now >= pendingRequest.tickMs && now - pendingRequest.tickMs < js::RequestTtlMs) return;
    const auto request = pendingRequest;
    pendingRequest = {};
    DropRequest(request, SaleTerminal::kRequestExpired, "REQUEST_EXPIRED", RequestAgeMs(request, now));
}

// ---------------------------------------------------------------------------
// stage S2c: the engine side of the one-press batch (design-r2 2.1-2.11).
// The retired per-candidate sale (IssueFromSnapshot, its issue_before sample,
// its settle and its 5,000 ms deadline) is gone from the rule path; the one
// vanilla ItemSelect call of this module is EngineBatchHost::Dispatch below,
// reached only through js::RunCommitTask from RunBatchCommitTask.
// ---------------------------------------------------------------------------

// The upper-case token a cancel reason is recorded as on the js_batch row.
const char* BatchAbortToken(const char* reason) noexcept {
    if (!reason || !reason[0]) return "none";
    if (std::strcmp(reason, "menu_closed") == 0) return "MENU_CLOSED";
    if (std::strcmp(reason, "save_started") == 0) return "SAVE_STARTED";
    if (std::strcmp(reason, "load_changed") == 0) return "LOAD_CHANGED";
    if (std::strcmp(reason, "run_invalidated") == 0) return "RUN_INVALIDATED";
    if (std::strcmp(reason, "chunk_limit") == 0) return "CHUNK_LIMIT";
    if (std::strcmp(reason, "error") == 0) return "ERROR";
    return reason;
}

// The pure-layer stop a cancelled commit is closed with (design-r2 2.8).
js::BatchStop BatchStopForAbort(const char* abort) noexcept {
    if (std::strcmp(abort, "MENU_CLOSED") == 0) return js::BatchStop::kMenuClosed;
    if (std::strcmp(abort, "SAVE_STARTED") == 0 || std::strcmp(abort, "LOAD_CHANGED") == 0) {
        return js::BatchStop::kLoadOrSave;
    }
    if (std::strcmp(abort, "RUN_INVALIDATED") == 0) return js::BatchStop::kConfigChanged;
    if (std::strcmp(abort, "BATCH_TIMEOUT") == 0) return js::BatchStop::kWatchdog;
    return js::BatchStop::kAdapterUnavailable;
}

const char* BatchRowStateName(js::BatchRowState state) noexcept {
    switch (state) {
    case js::BatchRowState::kPlanned: return "planned";
    case js::BatchRowState::kDispatched: return "dispatched";
    case js::BatchRowState::kConfirmed: return "confirmed";
    case js::BatchRowState::kUnconfirmed: return "unconfirmed";
    }
    return "unknown";
}

const char* BatchEventModeName(js::BatchEventMode mode) noexcept {
    switch (mode) {
    case js::BatchEventMode::kUnknown: return "unknown";
    case js::BatchEventMode::kInCall: return "in_call";
    case js::BatchEventMode::kDeferred: return "deferred";
    }
    return "unknown";
}

const char* BatchOutcomeTerminal(js::BatchOutcome outcome) noexcept {
    switch (outcome) {
    case js::BatchOutcome::kSold: return "BATCH_SOLD";
    case js::BatchOutcome::kPartial: return "BATCH_PARTIAL";
    case js::BatchOutcome::kNothing: return "BATCH_NOTHING";
    case js::BatchOutcome::kBlocked: return "BATCH_BLOCKED";
    case js::BatchOutcome::kNone: return "BATCH_NONE";
    }
    return "BATCH_NONE";
}

// One container event into a row's event set: js::AddBatchRowEvent
// (Batch.h, r57: engine-free and host-tested; item and gold counts are
// summed per row).
static_assert(js::BatchGoldFormID == GoldFormID);

// Called by the container sink while the batch window is open. Returns true
// when the event now belongs to the batch (it then never reaches the ring).
bool CaptureBatchEvent(const RE::TESContainerChangedEvent& event) {
    std::lock_guard<std::mutex> lock(batchCapture.mutex);
    if (!batchCapture.open) return false;
    if (batchCapture.inCall) {
        js::AddBatchRowEvent(batchCapture.call, event.oldContainer, event.newContainer, event.baseObj,
                             event.itemCount, batchCapture.playerId, batchCapture.vencId);
        return true;
    }
    if (batchCapture.late.size() >= BatchLateEventCap) {
        ++batchCapture.lateDropped;
        return true;
    }
    batchCapture.late.push_back(BatchLateEvent{event.oldContainer, event.newContainer, event.baseObj,
                                               event.itemCount});
    return true;
}

void OpenBatchCapture(std::uint32_t playerId, std::uint32_t vencId) {
    {
        std::lock_guard<std::mutex> lock(batchCapture.mutex);
        batchCapture.open = true;
        batchCapture.inCall = false;
        batchCapture.playerId = playerId;
        batchCapture.vencId = vencId;
        batchCapture.call = {};
        batchCapture.late.clear();
        batchCapture.late.reserve(BatchLateEventCap);
        batchCapture.lateDropped = 0;
    }
    batchCaptureOpen.store(true, std::memory_order_release);
}

void CloseBatchCapture() {
    batchCaptureOpen.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(batchCapture.mutex);
    if (batchCapture.open) {
        batchRun.lateEvents = static_cast<std::uint32_t>(batchCapture.late.size());
        batchRun.lateDropped = batchCapture.lateDropped;
    }
    batchCapture.open = false;
    batchCapture.inCall = false;
    batchCapture.call = {};
    batchCapture.late.clear();
    batchCapture.lateDropped = 0;
}

// P0 (design-r2 2.11): the events delivered while this one call runs. The
// lock is never held across the Callback itself, because a synchronous sink
// takes it on this same thread.
void BeginBatchCall() {
    std::lock_guard<std::mutex> lock(batchCapture.mutex);
    batchCapture.call = {};
    batchCapture.inCall = true;
}

js::BatchRowEvents EndBatchCall() {
    std::lock_guard<std::mutex> lock(batchCapture.mutex);
    batchCapture.inCall = false;
    return batchCapture.call;
}

// Every value a batch generation check compares. Menu, load, save and config
// generations first (no engine read), then the live foreground gate.
bool BatchGenerationsCurrent() noexcept {
    return barterOpen.load() && menuGeneration.load() == batchRun.menuGeneration &&
           epoch.load() == batchRun.loadGeneration &&
           !js::SaveStartedSince(batchRun.saveGeneration, saveGeneration.load()) &&
           configGeneration.load() == batchRun.configGeneration;
}

// BatchHost on the engine (Batch.h). One instance per commit task, on the UI
// task; nothing it reads is kept across tasks.
class EngineBatchHost final : public js::BatchHost {
public:
    std::uint64_t NowUs() override { return BatchNowUs(); }

    js::BatchGate CheckGate() override {
        if (!barterOpen.load() || menuGeneration.load() != batchRun.menuGeneration) {
            return js::BatchGate::kMenuClosed;
        }
        if (epoch.load() != batchRun.loadGeneration ||
            js::SaveStartedSince(batchRun.saveGeneration, saveGeneration.load())) {
            return js::BatchGate::kLoadOrSave;
        }
        if (configGeneration.load() != batchRun.configGeneration) return js::BatchGate::kConfigChanged;
        auto* ui = RE::UI::GetSingleton();
        if (!ui) return js::BatchGate::kMenuClosed;
        auto menu = ui->GetMenu<RE::BarterMenu>();
        if (!menu) return js::BatchGate::kMenuClosed;
        // The r10 foreground gate, the same five terms the retired attempt
        // gate read (SaleGate.h). A QuantityMenu opened by the previous row
        // (P1 false) stops the batch here.
        const ForegroundGate gate{true, ui->IsModalMenuOpen(), ui->IsMenuOpen("QuantityMenu"),
                                  ui->IsMenuOpen("MessageBoxMenu"), ui->IsMenuOpen("Console")};
        return ForegroundGateOk(gate) ? js::BatchGate::kOk : js::BatchGate::kForeground;
    }

    js::BatchListIdentity CurrentList() override {
        auto* list = CurrentItemList();
        if (!list) return {};
        return js::BatchListIdentity{reinterpret_cast<std::uintptr_t>(list->items.data()),
                                     static_cast<std::size_t>(list->items.size())};
    }

    bool ReadRow(std::size_t index, js::BatchListEntry& out) override {
        auto* list = CurrentItemList();
        if (!list || index >= static_cast<std::size_t>(list->items.size())) return false;
        auto* item = list->items[static_cast<std::uint32_t>(index)];
        auto* desc = item ? item->data.objDesc : nullptr;
        if (!desc || !desc->object) return false;
        out.item = reinterpret_cast<std::uintptr_t>(item);
        out.baseFormKey = desc->object->GetFormID();
        return true;
    }

    bool ReadList(std::vector<js::BatchListEntry>& out) override {
        out.clear();
        auto* list = CurrentItemList();
        if (!list) return false;
        const auto size = static_cast<std::size_t>(list->items.size());
        out.reserve(size);
        for (std::size_t i = 0; i < size; ++i) {
            auto* item = list->items[static_cast<std::uint32_t>(i)];
            auto* desc = item ? item->data.objDesc : nullptr;
            js::BatchListEntry entry;
            entry.item = reinterpret_cast<std::uintptr_t>(item);
            entry.baseFormKey = desc && desc->object ? desc->object->GetFormID() : 0;
            out.push_back(entry);
        }
        return true;
    }

    // R20-3 (R-W18 / R-W19): the row's own GFx formId must name its base, the
    // selectedIndex write is a means only, and the bind is believed only once
    // GetSelectedItem() reads back the same Item* and objDesc.
    bool Bind(const js::BatchRow& row) override {
        const std::size_t index = row.listIndex;
        const std::uintptr_t item = row.item;
        // bindfix: every refusal names its step on the js_batch row
        // (bindFail), so a live BIND_FAILED says which check refused.
        bindFailure_ = "list_missing";
        selectionMoved_ = false;
        auto* list = CurrentItemList();
        if (!list || index >= static_cast<std::size_t>(list->items.size()) ||
            index > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)())) {
            return false;
        }
        bindFailure_ = "entry_changed";
        auto* entry = list->items[static_cast<std::uint32_t>(index)];
        if (!entry || reinterpret_cast<std::uintptr_t>(entry) != item || !entry->obj.IsObject()) return false;
        auto* desc = entry->data.objDesc;
        const std::uint32_t base = desc && desc->object ? desc->object->GetFormID() : 0;
        bindFailure_ = "source_missing";
        if (row.sourceIndex >= ruleRun.inputs.size()) return false;
        const auto fresh = ReadRuleCandidateEntry(entry, ruleRun.settings, ruleRun.stolenAcceptance,
                                                  ruleRun.xListBudget);
        const auto& original = ruleRun.inputs[row.sourceIndex];
        const auto reverify = js::RuleCandidateReverifyFailure(original, fresh.input);
        if (reverify != js::ReverifyFailure::kNone) {
            bindFailure_ = js::ReverifyFailureName(reverify);
            return false;
        }
        bindFailure_ = "candidate_invalid";
        const auto freshCandidate = js::BuildRuleCandidate(fresh.input);
        if (freshCandidate.failure != js::CandidateReadFailure::kNone) return false;
        const auto decision = js::DecideCandidate(freshCandidate.record.candidate,
            js::DefaultSelectorContract, js::SelectorThresholdsFromSettings(ruleRun.settings.values));
        bindFailure_ = "candidate_changed";
        if (decision.decision != js::SelectorDecision::kSelect ||
            freshCandidate.record.candidate.count < row.quantity ||
            fresh.input.infoValue != static_cast<double>(row.unitPrice)) return false;
        js::RuleBindEntry bind{};
        bind.reverifiedCandidate = true;
        RE::GFxValue value;
        if (entry->obj.GetMember("formId", &value) && value.IsNumber() && std::isfinite(value.GetNumber())) {
            bind.formIdKnown = true;
            bind.formId = static_cast<std::uint32_t>(value.GetNumber());
        }
        if (entry->obj.GetMember("filteredIndex", &value) && value.IsNumber() &&
            std::isfinite(value.GetNumber())) {
            bind.filteredIndexKnown = true;
            bind.filteredIndex = static_cast<std::int32_t>(value.GetNumber());
        }
        const std::array<js::RuleBindEntry, 1> candidates{bind};
        bindFailure_ = "bind_entry_missing";
        if (js::FindRuleBindIndex(std::span<const js::RuleBindEntry>(candidates), base,
                                  bind.filteredIndex) != 0) {
            return false;
        }
        // bindfix2: the list identity just before the write, so a
        // read-back naming another row can be told apart from a list that
        // changed under the write.
        const auto* dataBeforeWrite = list->items.data();
        const auto sizeBeforeWrite = list->items.size();
        list->root.SetMember("selectedIndex", RE::GFxValue(static_cast<double>(index)));
        auto* after = list->GetSelectedItem();
        const std::uint32_t afterBase = after && after->data.objDesc && after->data.objDesc->object
            ? after->data.objDesc->object->GetFormID() : 0;
        const bool bound = js::DecideRuleBind(0, after != nullptr && after == entry,
                                              after != nullptr && after->data.objDesc == desc, afterBase,
                                              base) == js::RuleBindOutcome::kBound;
        // A null read-back while the list is update-pending is the engine's
        // own staleness gate (ItemList byte 0x50), named apart from a plain
        // mismatch.
        bindFailure_ = bound ? "none" : !after ? (list->updatePending ? "readback_pending" : "readback_null")
                                               : "readback_mismatch";
        // bindfix2: SkyUI re-selected another live row of the same,
        // unchanged list (the row under the mouse after its scroll) while our
        // row's entry is still in place. The only refusal the commit task
        // may retry (BatchBindRewriteMax); nothing here changes a check.
        if (!bound && after && after != entry && list->items.data() == dataBeforeWrite &&
            list->items.size() == sizeBeforeWrite && list->items[static_cast<std::uint32_t>(index)] == entry) {
            bindFailure_ = "readback_moved";
            selectionMoved_ = true;
        }
        return bound;
    }

    bool LastBindSelectionMoved() override { return selectionMoved_; }

    // bindfix: ItemList byte 0x50 (unk50 in the 800d CommonLib,
    // updatePending in CommonLibSSE-NG 9.1; the same bool at 0x50, so the
    // w-probe-r71 v9 build reads it by the v9 name). Written only by js::RunCommitTask
    // around one row's bind + dispatch, under DecideBatchPendingBind.
    bool ListUpdatePending() override {
        auto* list = CurrentItemList();
        return list != nullptr && list->updatePending;
    }

    void SetListUpdatePending(bool pending) override {
        if (auto* list = CurrentItemList()) list->updatePending = pending;
    }

    const char* LastBindFailure() override { return bindFailure_; }

    // One vanilla ItemSelect with raw = {0, q, p, null} (D1: q is the whole
    // sellable stack). The events the sink delivers while the call runs are
    // this row's (P0).
    js::BatchCallResult Dispatch(const js::BatchRow& row) override {
        js::BatchCallResult result;
        auto* ui = RE::UI::GetSingleton();
        auto menu = ui ? ui->GetMenu<RE::BarterMenu>() : nullptr;
        auto* delegate = menu ? menu->fxDelegate.get() : nullptr;
        auto* movie = menu ? menu->uiMovie.get() : nullptr;
        if (!delegate || !movie) return result;
        RE::GFxValue raw[4];
        raw[0].SetNumber(0.0);
        raw[1].SetNumber(static_cast<double>(row.quantity));
        raw[2].SetNumber(static_cast<double>(row.unitPrice));
        raw[3].SetNull();
        BeginBatchCall();
        ++dispatchCount;
        delegate->Callback(movie, ItemSelectName, raw, 4);
        result.inCall = EndBatchCall();
        result.called = true;
        return result;
    }

private:
    const char* bindFailure_{"none"};
    bool selectionMoved_{false};
};

// design-r2 3.6: the receipt-only shadow quote inputs, read once per batch on
// the UI task. Live GMSTs, current Speech, the highest owned Haggling rank,
// AV 107 / AV 146 and whether a merchant-conditioned perk is owned. No engine
// price call and no perk-entry evaluation; nothing is written anywhere.
js::ShadowPriceInputs ReadShadowPriceInputs() {
    js::ShadowPriceInputs in;
    in.barterMax = std::numeric_limits<float>::quiet_NaN();
    in.barterMin = std::numeric_limits<float>::quiet_NaN();
    auto* player = RE::PlayerCharacter::GetSingleton();
    auto* settingsCollection = RE::GameSettingCollection::GetSingleton();
    if (!player || !settingsCollection) return in;
    const auto readFloat = [settingsCollection](const char* name, float& out) {
        auto* setting = settingsCollection->GetSetting(name);
        if (setting && setting->GetType() == RE::Setting::Type::kFloat) out = setting->GetFloat();
    };
    readFloat("fBarterMax", in.barterMax);
    readFloat("fBarterMin", in.barterMin);
    auto* values = player->AsActorValueOwner();
    if (!values) {
        in.barterMax = std::numeric_limits<float>::quiet_NaN();
        return in;
    }
    in.speech = values->GetActorValue(RE::ActorValue::kSpeech);
    in.av107Percent = values->GetActorValue(RE::ActorValue::kSpeechcraftModifier);
    in.av146Percent = values->GetActorValue(RE::ActorValue::kSpeechcraftPowerModifier);
    // Haggling 00/20/40/60/80 (the v0.1.0 design section 2.4): each
    // lower rank is conditioned on not owning the next, so the highest
    // owned rank is the one that applies.
    constexpr std::array<RE::FormID, 5> haggling{0x000BE128, 0x000C07CE, 0x000C07CF, 0x000C07D0,
                                                 0x000C07D1};
    for (std::size_t rank = haggling.size(); rank > 0; --rank) {
        auto* perk = RE::TESForm::LookupByID<RE::BGSPerk>(haggling[rank - 1]);
        if (perk && player->HasPerk(perk)) {
            in.hagglingRank = static_cast<int>(rank);
            break;
        }
    }
    // Allure, MGArchMageVendorPerk, MGEnthirVendorPerk: conditioned on the
    // merchant, so the reader leaves such rows out of the 1% rule.
    constexpr std::array<RE::FormID, 3> merchantConditioned{0x00058F75, 0x0010F9DB, 0x0006BC37};
    for (const auto id : merchantConditioned) {
        auto* perk = RE::TESForm::LookupByID<RE::BGSPerk>(id);
        if (perk && player->HasPerk(perk)) in.merchantConditionedPerk = true;
    }
    return in;
}

// v0.8.0 (the barter-price design r2 section 3.1): the one price snapshot
// of a sell-anywhere press, an in-menu batch or the MCM preview, on the main
// thread. With the switch OFF (or the developer key) nothing is walked and the
// price is the shipped formula of `inputs` exactly; with it ON the walk, the
// conditions (player in both roles) and the engine self-check run once.
struct PriceRead {
    js::PriceSnapshot snapshot{};
    js::PriceSourceReceipt receipt{};
};
PriceRead ReadPriceSnapshot(bool switchOn, const js::ShadowPriceInputs& inputs) {
    PriceRead read;
    const auto started = NowNs();
    const bool devForced = devPerkFallback.load(std::memory_order_relaxed);
    const bool walk = switchOn && !devForced;
    js::PerkWalkFacts facts;
    if (walk) js::ReadPerkWalk(RE::PlayerCharacter::GetSingleton(), facts);
    read.snapshot = js::DecidePriceSnapshot(switchOn, devForced, inputs, walk ? &facts : nullptr);
    const std::uint64_t evalUs = (NowNs() - started) / 1000;
    js::FillPriceSourceReceipt(read.receipt, read.snapshot, walk ? &facts : nullptr, evalUs);
    return read;
}

// Section 3.4 step 4: the source of the last sale, kept until the next one or
// a load.
void NoteLastPriceSource(const js::PriceSnapshot& snapshot) noexcept {
    lastPriceSource.store(static_cast<std::int32_t>(snapshot.source));
}

// A batch sample is usable when it is valid and belongs to the batch's own
// menu and load generation, with a resolved VENC and both balances read.
bool BatchSampleCurrent(const OracleSample& sample) noexcept {
    return sample.sampleValid && sample.startMenuGeneration == batchRun.menuGeneration &&
           sample.endMenuGeneration == batchRun.menuGeneration &&
           sample.startLoadGeneration == batchRun.loadGeneration &&
           sample.endLoadGeneration == batchRun.loadGeneration && sample.merchantContainerID != 0 &&
           sample.playerGold >= 0 && sample.merchantContainerGold >= 0;
}

// design-r2 2.1 step B: the one before sample, requested once the paged read
// is complete. The plan is built when it publishes (OnBatchSample).
void RequestBatchBefore() {
    if (!ruleRun.active || batchRun.runToken != ruleRun.runToken) {
        FinishBatch("ERROR", "RUN_MISMATCH");
        return;
    }
    SetBatchPhase(BatchPhase::kBefore);
    batchRun.beforeSampleId = RequestOracle("batch_before");
}

// design-r2 2.1 step A: the one after sample, only when a row was dispatched
// (nothing to reconcile otherwise).
void RequestBatchAfter() {
    if (batchRun.state.rowsDispatched == 0) {
        FinishBatch("COMPLETED", "none");
        return;
    }
    SetBatchPhase(BatchPhase::kAfter);
    batchRun.afterSampleId = RequestOracle("batch_after");
}

// The plan of this press (Batch.h BuildBatchPlan) from the selector's run.
// Only the selected rows need a live Item pointer; each is read from the list
// the paged read saw, and its base must still be the one the read recorded.
void StartBatchFromSelection(const js::RuleSelectionRun& selection) {
    auto* list = CurrentItemList();
    bool listSame = list && reinterpret_cast<std::uintptr_t>(list) == ruleRun.page.window.listAddress &&
                    static_cast<std::size_t>(list->items.size()) == ruleRun.page.window.entries;
    std::vector<js::CandidateMerchantRecord> records;
    std::vector<js::BatchRowSource> sources;
    records.reserve(selection.candidates.size());
    sources.reserve(selection.candidates.size());
    for (const auto& candidate : selection.candidates) {
        records.push_back(candidate.record);
        js::BatchRowSource source;
        source.listIndex = candidate.sourceIndex;
        source.sourceIndex = candidate.sourceIndex;
        source.baseFormKey = candidate.record.candidate.baseFormKey;
        sources.push_back(source);
    }
    for (const std::size_t index : selection.selection.selectedIndices) {
        if (!listSame) break;
        if (index >= sources.size() ||
            sources[index].listIndex >= static_cast<std::size_t>(list->items.size())) {
            listSame = false;
            break;
        }
        auto& source = sources[index];
        auto* item = list->items[static_cast<std::uint32_t>(source.listIndex)];
        auto* desc = item ? item->data.objDesc : nullptr;
        if (!desc || !desc->object || desc->object->GetFormID() != source.baseFormKey) {
            listSame = false;
            break;
        }
        source.item = reinterpret_cast<std::uintptr_t>(item);
    }
    js::BatchListIdentity identity{};
    if (list) {
        identity = js::BatchListIdentity{reinterpret_cast<std::uintptr_t>(list->items.data()),
                                         static_cast<std::size_t>(list->items.size())};
    }
    auto plan = js::BuildBatchPlan(std::span<const js::CandidateMerchantRecord>(records), selection.selection,
                                   std::span<const js::BatchRowSource>(sources), identity);
    // r58: at most PressRowCap(list size) rows per press (the engine
    // pays per sold row on a heavy list); the rest waits for the next press.
    js::ApplyPressRowCap(plan);
    batchRun.shadow = ReadShadowPriceInputs();
    js::AttachShadowQuotes(plan, batchRun.shadow);
    // v0.8.0 (section 3.2): the batch's one price snapshot at plan time,
    // made like a sell-anywhere press's; the transactions stay ItemSelect.
    {
        const auto priced = ReadPriceSnapshot(LiveSettingsCopy().perkPrice, batchRun.shadow);
        batchRun.priceSnapshot = priced.snapshot;
        batchRun.priceReceipt = priced.receipt;
    }
    batchRun.planned = plan.valid;
    batchRun.state = js::BeginBatch(std::move(plan), batchRun.edgeUs);
    const char* abort = !batchRun.beforeValid ? "BEFORE_UNAVAILABLE"
                      : !listSame ? "LIST_CHANGED"
                      : !batchRun.planned ? "PLAN_INVALID" : "none";
    if (std::strcmp(abort, "none") != 0) {
        FinishBatch("ERROR", abort);
        return;
    }
    if (batchRun.state.plan.rows.empty()) {
        FinishBatch("NO_CANDIDATES", "none");
        return;
    }
    // v0.8.0 (section 3.4 step 4): this batch sells; its price source is
    // the last sale's.
    NoteLastPriceSource(batchRun.priceSnapshot);
    OpenBatchCapture(batchRun.playerId, batchRun.vencId);
    batchNotBeforeNs.store(0, std::memory_order_release);
    SetBatchPhase(BatchPhase::kCommit);
}

// PublishOracle hands every published sample here; only the batch's own two
// ids are taken.
void OnBatchSample(const OracleSample& sample) {
    if (!ruleRun.active || batchRun.runToken != ruleRun.runToken) return;
    if (batchRun.phase == BatchPhase::kBefore && sample.sampleId == batchRun.beforeSampleId) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        batchRun.playerId = player ? player->GetFormID() : 0;
        const bool usable = batchRun.playerId != 0 && BatchSampleCurrent(sample) &&
                            sample.endInventoryGeneration == inventoryGeneration.load();
        if (usable) {
            batchRun.beforePlayerGold = sample.playerGold;
            batchRun.beforeVencGold = sample.merchantContainerGold;
            batchRun.vencId = sample.merchantContainerID;
        }
        PlanRuleRun(usable ? &sample : nullptr);
        return;
    }
    if (batchRun.phase == BatchPhase::kAfter && sample.sampleId == batchRun.afterSampleId) {
        batchRun.samplesValid = batchRun.beforeValid && BatchSampleCurrent(sample) &&
                                sample.sampleId > batchRun.beforeSampleId &&
                                sample.merchantContainerID == batchRun.vencId;
        if (batchRun.samplesValid) {
            batchRun.playerDelta = static_cast<std::int64_t>(sample.playerGold) - batchRun.beforePlayerGold;
            batchRun.vencDelta = static_cast<std::int64_t>(sample.merchantContainerGold) - batchRun.beforeVencGold;
        }
        const auto reconcile = js::ReconcileBatch(batchRun.state, batchRun.samplesValid,
                                                  batchRun.playerDelta, batchRun.vencDelta);
        batchRun.reconciled = true;
        batchRun.reconcileOk = reconcile.ok;
        FinishBatch("COMPLETED", "none");
    }
}

// P0 false only (rowsDeferred > 0): the late events, in arrival order, go to
// the dispatched rows still undecided, in plan order (the i-th item move and
// the i-th gold move to the i-th row). A row is decided once it holds a pair
// or a foreign event; the rest wait for BatchDeferredVerifyCapUs.
void DecideBatchDeferred() {
    auto& state = batchRun.state;
    auto& pending = batchRun.deferred;
    if (state.rowsDeferred == 0) {
        pending.clear();
        return;
    }
    std::vector<BatchLateEvent> fresh;
    std::uint32_t playerId = 0, vencId = 0;
    {
        std::lock_guard<std::mutex> lock(batchCapture.mutex);
        playerId = batchCapture.playerId;
        vencId = batchCapture.vencId;
        if (batchRun.lateConsumed < batchCapture.late.size()) {
            fresh.assign(batchCapture.late.begin() + static_cast<std::ptrdiff_t>(batchRun.lateConsumed),
                         batchCapture.late.end());
            batchRun.lateConsumed = batchCapture.late.size();
        }
    }
    std::size_t undecided = 0;
    for (const auto& row : state.plan.rows) {
        if (row.state == js::BatchRowState::kDispatched) ++undecided;
    }
    pending.resize(undecided);
    for (const auto& event : fresh) {
        if (pending.empty()) break;
        js::BatchRowEvents shape{};
        js::AddBatchRowEvent(shape, event.oldContainer, event.newContainer, event.baseObj, event.itemCount,
                             playerId, vencId);
        js::BatchRowEvents* target = &pending.front();
        for (auto& candidate : pending) {
            if ((shape.itemEvents && !candidate.itemEvents) || (shape.goldEvents && !candidate.goldEvents)) {
                target = &candidate;
                break;
            }
        }
        js::AddBatchRowEvent(*target, event.oldContainer, event.newContainer, event.baseObj, event.itemCount,
                             playerId, vencId);
    }
    std::size_t ready = 0;
    while (ready < pending.size() &&
           (pending[ready].foreignEvents != 0 ||
            (pending[ready].itemEvents != 0 && pending[ready].goldEvents != 0))) {
        ++ready;
    }
    if (ready == 0) return;
    js::DecideDeferredRows(state, std::span<const js::BatchRowEvents>(pending.data(), ready));
    pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(ready));
}

void AfterBatchCommit() {
    batchRun.commitEndMs = NowMs();
    if (batchRun.state.rowsDeferred > 0) {
        SetBatchPhase(BatchPhase::kDeferred);
        return;
    }
    RequestBatchAfter();
}

// The batch commit function (design-r2 2.1 step C): one call is one UI task.
// js::RunCommitTask dispatches the planned rows back to back and checks
// CommitFrameCapUs before each next row (at least one row per task). On kSpill
// this function only records when the next task may start; the pump starts
// it on a later frame. It never queues a task of its own.
void RunBatchCommitTask() {
    if (batchRun.phase != BatchPhase::kCommit) return;
    if (batchRun.state.rowsDeferred > 0) DecideBatchDeferred();
    EngineBatchHost host;
    const auto started = NowNs();
    const auto result = js::RunCommitTask(batchRun.state, host);
    // Metered for runSummary like every task. The house 2 ms rule does not
    // bind this one task (design-r2 2.3): its own cap is CommitFrameCapUs and
    // its cost is recorded apart as commitTaskUs (BatchState).
    taskMeter.Observe((NowNs() - started) / 1000);
    if (result.end == js::BatchTaskEnd::kSpill) {
        batchNotBeforeNs.store(NowNs() + BatchSpillGapNs, std::memory_order_release);
        return;
    }
    AfterBatchCommit();
}

// design-r2 2.11 P3: how long the main thread ran no SKSE task after the
// last commit task (the r35 stall measure). Queued by ServiceBatchTask, never
// by the commit function.
void QueueBatchGapProbe(std::uint64_t runToken, std::uint64_t commitEndMs) {
    batchPostCommitGapMs.store(-1);
    batchGapToken.store(runToken);
    auto* tasks = SKSE::GetTaskInterface();
    if (!tasks) { ++queueFailures; return; }
    try {
        tasks->AddTask([runToken, commitEndMs] {
            const auto now = NowMs();
            if (batchGapToken.load() == runToken) {
                batchPostCommitGapMs.store(now >= commitEndMs ? static_cast<std::int64_t>(now - commitEndMs) : -1);
            }
        });
    } catch (...) { ++queueFailures; }
}

// The one UI task the pump keeps queued while a batch is live (at most one
// outstanding): the next commit task, the deferred decision, or the watchdog
// of a sample that never arrived.
void ServiceBatchTask() {
    EnterUiTask();
    if (!ruleRun.active || batchRun.runToken != ruleRun.runToken) return;
    switch (batchRun.phase) {
    case BatchPhase::kCommit:
        RunBatchCommitTask();
        if (batchRun.phase != BatchPhase::kCommit && batchRun.commitEndMs) {
            QueueBatchGapProbe(batchRun.runToken, batchRun.commitEndMs);
        }
        break;
    case BatchPhase::kDeferred:
        DecideBatchDeferred();
        js::ExpireDeferredRows(batchRun.state, BatchNowUs());
        if (batchRun.state.rowsDeferred == 0) {
            batchRun.deferred.clear();
            RequestBatchAfter();
        }
        break;
    case BatchPhase::kBefore:
    case BatchPhase::kAfter:
        if (batchRun.edgeUs && BatchNowUs() - batchRun.edgeUs >= js::BatchWatchdogUs) {
            FinishBatch("ERROR", "BATCH_TIMEOUT");
        }
        break;
    case BatchPhase::kNone:
        break;
    }
}

// ---------------------------------------------------------------------------
// S2c: the one result line (design-r2 2.10). Its text comes only from
// ResultText.h (our own translation file, parsed once at kDataLoaded; no
// engine translation lookup). It leaves the DLL only through RE::DebugNotification in
// an AddTask lambda (sold) or the MessageBoxData factory route (partial,
// nothing, blocked). No engine message-create call, no menu stack walk and
// no IMenu name read (all three crash on 1.6.1170).
// ---------------------------------------------------------------------------
constexpr std::size_t ResultTextMaxBytes = 262144;

std::string ReadResultTextFile(const std::string& language) {
    std::ifstream input(std::filesystem::path("Data/Interface/Translations") /
                            ("AGH_JunkSeller_" + language + ".txt"),
                        std::ios::binary);
    if (!input) return {};
    std::string bytes(ResultTextMaxBytes + 1, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    if (bytes.size() > ResultTextMaxBytes) return {};
    return bytes;
}

// sLanguage:General, lower case, letters only; anything else is english
// (the pattern SR v1.6 pins).
std::string ResultLanguage() {
    const auto* setting = RE::GetINISetting("sLanguage:General");
    const char* raw = setting && setting->GetType() == RE::Setting::Type::kString ? setting->GetString() : nullptr;
    std::string language = raw && *raw ? raw : "english";
    for (char& c : language) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (language.empty() || language.size() > 32 ||
        language.find_first_not_of("abcdefghijklmnopqrstuvwxyz") != std::string::npos) {
        return "english";
    }
    return language;
}

// kDataLoaded only, before any press can exist. A missing file leaves an
// empty table, and ResultText::Render falls back per key to the English file
// and then to its built-in ASCII English table, so a line always exists.
void LoadResultTextTables() {
    namespace rt = agh::junk_seller::result_text;
    resultEnglish = rt::Parse(ReadResultTextFile("english")).entries;
    const auto language = ResultLanguage();
    resultLocalized = language == "english" ? resultEnglish : rt::Parse(ReadResultTextFile(language)).entries;
}

struct RenderedResult {
    std::string text;
    bool fallback{};
    std::uint32_t unknownPlaceholders{};
};

RenderedResult RenderResultLine(const js::BatchResultLine& line) {
    namespace rt = agh::junk_seller::result_text;
    RenderedResult out;
    if (line.outcome == js::BatchOutcome::kNone) return out;
    const rt::Table none{};
    const auto prefix = rt::Render("$AGH_JS_RESULT_PREFIX", resultLocalized, resultEnglish, none);
    rt::Table values{{"p", prefix.text}, {"n", std::to_string(line.n)}, {"m", std::to_string(line.m)},
                     {"g", std::to_string(line.g)}};
    out.fallback = prefix.usedEnglishFallback;
    out.unknownPlaceholders = static_cast<std::uint32_t>(prefix.unknownPlaceholders);
    if (line.reason != js::BatchReason::kNone) {
        const auto reason = rt::Render(js::BatchReasonKey(line.reason), resultLocalized, resultEnglish, none);
        values["r"] = reason.text;
        out.fallback = out.fallback || reason.usedEnglishFallback;
        out.unknownPlaceholders += static_cast<std::uint32_t>(reason.unknownPlaceholders);
    }
    const auto body = rt::Render(js::BatchOutcomeKey(line.outcome), resultLocalized, resultEnglish, values);
    out.text = body.text;
    out.fallback = out.fallback || body.usedEnglishFallback;
    out.unknownPlaceholders += static_cast<std::uint32_t>(body.unknownPlaceholders);
    return out;
}

// The one-button box has nothing to decide; its callback is a no-op.
// w-probe-r71 (v0.9.0): CommonLib v9 declares Run(std::uint8_t a_button)
// (include/RE/I/IMessageBoxCallback.h:16; the 800d `Message` enum is gone,
// survey error at this line).
class ResultBoxCallback final : public RE::IMessageBoxCallback {
public:
    void Run(std::uint8_t) override {}
};

// w-probe-r71 (v0.9.0): v9 dropped RE::DebugNotification (survey,
// C2039). The same engine function through our own relocation:
// RELOCATION_ID(52050, 52933) and the same arguments as the 800d pin's wrapper
// (vendor/commonlibsse-ng src/RE/M/Misc.cpp:44-49), so the same HUD line (the
// SR v1.9.0 route, pipeline/soul-recharge/native/src/probe/Probe.cpp).
void ShowHudNotification(const char* line) {
    using func_t = void (*)(const char*, const char*, bool);
    static REL::Relocation<func_t> func{ RELOCATION_ID(52050, 52933) };
    func(line, nullptr, true);
}

// The proven MessageBoxData factory route (uncapper PapyrusBridge.cpp,
// live on 1.6.1170 with Japanese text). Main thread (an AddTask) only.
bool ShowResultBox(const char* text) {
    const RE::BSTDerivedCreator<RE::MessageBoxData, RE::IUIMessageData>* creator = nullptr;
    RE::MessageBoxData* box = nullptr;
    try {
        auto* manager = RE::MessageDataFactoryManager::GetSingleton();
        auto* strings = RE::InterfaceStrings::GetSingleton();
        if (!manager || !strings) return false;
        creator = manager->GetCreator<RE::MessageBoxData>(strings->messageBoxData);
        if (!creator) return false;
        box = creator->Create();
        if (!box) return false;
        box->bodyText = text;
        box->buttonText.push_back("OK");
        auto callback = RE::make_smart<ResultBoxCallback>();
        if (!callback) {
            creator->Destroy(box);
            return false;
        }
        box->callback = std::move(callback);
        // v9: MessageBoxMenu::QueueMessage(MessageBoxData*), RELOCATION_ID(51422, 52271)
        // (include/RE/M/MessageBoxMenu.h:59); the 800d member call is gone.
        RE::MessageBoxMenu::QueueMessage(box);
    } catch (...) {
        if (box && creator) creator->Destroy(box);
        return false;
    }
    return true;
}

// Queues the one line of this press. Sold is a notification; every other
// outcome, including uncertainty, is a one-button box (design-r2 2.10).
// v0.1.0 (design r3 section 6): `hudOnly` makes every outcome of a
// sell-anywhere press one HUD notification (no box over the world).
const char* SurfaceResultLine(const js::BatchResultLine& line, const std::string& text, bool hudOnly = false) {
    if (line.outcome == js::BatchOutcome::kNone || text.empty()) return "none";
    auto* tasks = SKSE::GetTaskInterface();
    if (!tasks) { ++queueFailures; return "unavailable"; }
    const bool notify = hudOnly || line.outcome == js::BatchOutcome::kSold;
    // audit A-08: a line queued before a load is never shown after it.
    const auto lineEpoch = epoch.load();
    try {
        tasks->AddTask([text, notify, lineEpoch] {
            try {
                if (lineEpoch != epoch.load() || loadInProgress.load()) return;
                if (notify) {
                    if (RE::UI::GetSingleton()) ShowHudNotification(text.c_str());
                    lastLineShownUs.store(BatchNowUs(), std::memory_order_relaxed);
                } else {
                    ShowResultBox(text.c_str());
                }
            } catch (...) {}
        });
    } catch (...) {
        ++queueFailures;
        return "unavailable";
    }
    return notify ? "notification" : "message_box";
}

// The facts of design-r2 2.10's press gate, from the live phase and settings.
js::BatchPressFacts CollectPressFacts(const js::SchedulerPhase& phase) {
    js::BatchPressFacts facts;
    facts.barterOpen = phase.barterOpen;
    facts.settingsReadable = !configCorrupt.load();
    facts.enabled = LiveSettingsCopy().enabled;
    facts.foregroundOk = !(phase.topmostOther || phase.modalOpen || phase.quantityOpen ||
                           phase.messageOpen || phase.consoleOpen);
    facts.latched = phase.issueLatched;
    facts.running = phase.issueInFlight || phase.requestPending;
    facts.adapterAvailable = RuntimeOk() && !apiMismatch.load() && AdapterProven();
    return facts;
}

// The one line and the js_batch row (runToken 0) of a press that never
// started a run. Outside the BarterMenu there is neither (design-r2 2.13).
void EmitPressOutcome(js::BatchPressFacts facts, const char* abort) {
    js::BatchOutcomeInput input;
    input.gate = js::EvaluatePressGate(facts);
    // A start refused for a reason the table has no key of its own for
    // (a stale queued start) is "selling is unavailable now".
    if (input.gate == js::BatchPressGate::kOk) input.gate = js::BatchPressGate::kAdapterUnavailable;
    const auto line = js::ClassifyBatchOutcome(input);
    if (line.outcome == js::BatchOutcome::kNone) return;
    const auto text = RenderResultLine(line);
    auto row = NewRow("js_batch", "batch", Phase::kUiTask);
    row.issueSource = "rule";
    row.reason = js::BatchReasonName(line.reason);
    row.batch.outcome = js::BatchOutcomeName(line.outcome);
    row.batch.abort = abort && abort[0] ? abort : "none";
    row.batch.textFallback = text.fallback;
    row.batch.unknownPlaceholders = text.unknownPlaceholders;
    row.batch.surface = SurfaceResultLine(line, text.text);
    Log(row);
}

// A press dropped by the input sink while a run was live (design-r2 2.8).
void EmitRunningLine() {
    EnterUiTask();
    js::BatchPressFacts facts;
    facts.barterOpen = barterOpen.load();
    facts.running = true;
    EmitPressOutcome(facts, "EDGE_DROPPED");
}

// The end of every started run (design-r2 2.1 step E): stop a commit that is
// still live, latch an unreconciled dispatched batch, classify, write the
// slim rows, the js_batch row and the js_rule_run row, the MCM last-run line,
// then queue the one result line (none after a load).
void FinishBatch(const char* runReason, const char* abort) {
    if (!ruleRun.active) return;
    CloseBatchCapture();
    auto& run = batchRun;
    auto& state = run.state;
    const char* abortToken = BatchAbortToken(abort);
    if (std::strcmp(run.abort, "none") == 0 && std::strcmp(abortToken, "none") != 0) run.abort = abortToken;
    if (run.phase == BatchPhase::kCommit || run.phase == BatchPhase::kDeferred) {
        js::StopBatch(state, BatchStopForAbort(run.abort), false);
    }
    // rows the vanilla call already ran for but no event decided (a
    // cancel during the deferred phase) are Unconfirmed, never unsold; the
    // first stop reason above is kept.
    js::SettleUndecidedBatchRows(state);
    // 2.5 / 2.8: a batch that dispatched a row but was never reconciled
    // could not be verified; it latches like a mismatch.
    if (state.rowsDispatched > 0 && !run.reconciled) state.latched = true;
    if (state.latched) {
        issueLatched.store(true);
        if (state.stop == js::BatchStop::kReconcileMismatch) issueOracleMismatchLatched.store(true);
    }
    std::int64_t goldReceived = 0;
    const char* goldSource = "none";
    if (state.rowsDispatched > 0) {
        if (run.reconciled && run.samplesValid) {
            goldReceived = run.playerDelta;
            goldSource = run.reconcileOk ? "reconciled" : "player_delta";
        } else {
            goldReceived = state.goldConfirmed;
            goldSource = "events";
        }
    }
    js::BatchOutcomeInput input;
    if (run.planned) {
        input = js::BatchOutcomeInputFrom(state, goldReceived);
        input.stopped = input.stopped || std::strcmp(run.abort, "none") != 0;
    } else {
        input.gate = js::BatchPressGate::kAdapterUnavailable;
    }
    const auto line = js::ClassifyBatchOutcome(input);
    const auto text = RenderResultLine(line);
    const bool surface = std::strcmp(run.abort, "LOAD_CHANGED") != 0;
    const char* surfaceName = surface ? SurfaceResultLine(line, text.text) : "suppressed_load";
    const std::uint64_t runToken = ruleRun.runToken;
    std::uint32_t slimRows = 0;
    if (!state.plan.rows.empty()) {
        auto slim = NewRow("js_batch_row", "batch", Phase::kUiTask);
        slim.runToken = runToken;
        slim.issueSource = "rule";
        for (const auto& planned : state.plan.rows) {
            if (slimRows >= js::BatchMaxRows) break;
            slim.seq = NextSeq();
            auto& out = slim.batchRow;
            out = BatchRowReceipt{};
            out.token = planned.token;
            out.listIndex = static_cast<std::uint32_t>(planned.listIndex);
            out.baseObj = planned.baseFormKey;
            out.stackCount = planned.stackCount;
            out.quantity = planned.quantity;
            out.unitPrice = planned.unitPrice;
            out.total = planned.total;
            out.baseValue = planned.baseValue;
            out.shadowPrice = planned.shadowPrice;
            out.merchantPerk = state.plan.shadowMerchantPerk;
            out.state = BatchRowStateName(planned.state);
            out.pairInCall = planned.pairInCall;
            out.listRebuiltInCall = planned.listRebuiltInCall;
            out.itemEvents = planned.itemEvents;
            out.goldEvents = planned.goldEvents;
            out.foreignEvents = planned.foreignEvents;
            Log(slim);
            ++slimRows;
        }
    }
    auto row = NewRow("js_batch", "batch", Phase::kUiTask);
    row.runToken = runToken;
    row.issueSource = "rule";
    // v0.8.0: the batch's price snapshot (null when it never planned).
    row.priceSource = run.priceReceipt;
    row.reason = js::BatchReasonName(line.reason);
    row.beforeSampleId = run.beforeSampleId;
    row.afterSampleId = run.afterSampleId;
    auto& b = row.batch;
    b.outcome = js::BatchOutcomeName(line.outcome);
    b.stop = js::BatchStopName(state.stop);
    b.abort = run.abort;
    b.eventMode = BatchEventModeName(state.eventMode);
    b.goldSource = goldSource;
    b.surface = surfaceName;
    b.unitsEligible = state.plan.unitsEligible;
    b.unitsPlanned = state.plan.unitsPlanned;
    b.unitsSold = state.unitsSold;
    b.goldPlanned = state.plan.goldPlanned;
    b.goldDispatched = state.goldDispatched;
    b.goldConfirmed = state.goldConfirmed;
    b.goldReceived = goldReceived;
    b.playerGoldDelta = run.playerDelta;
    b.vencGoldDelta = run.vencDelta;
    b.rowsPlanned = static_cast<std::uint32_t>(state.plan.rows.size());
    b.rowsDispatched = state.rowsDispatched;
    b.rowsConfirmed = state.rowsConfirmed;
    b.rowsUnconfirmed = state.rowsUnconfirmed;
    b.rowsDeferred = state.rowsDeferred;
    b.commitFrames = state.commitFrames;
    b.commitTaskUsMax = state.commitTaskUsMax;
    b.commitTaskUsTotal = state.commitTaskUsTotal;
    b.pairInCallRows = state.pairInCallRows;
    b.listRebuiltInCallRows = state.listRebuiltInCallRows;
    b.listRemaps = state.listRemaps;
    b.pendingBypassRows = state.pendingBypassRows;
    b.bindFail = state.bindFailure;
    b.bindRewrites = state.bindRewrites;
    b.splitEventRows = state.splitEventRows;
    b.foreignKnown = state.firstForeign.known;
    b.foreignOld = state.firstForeign.oldContainer;
    b.foreignNew = state.firstForeign.newContainer;
    b.foreignBase = state.firstForeign.baseObj;
    b.foreignCount = state.firstForeign.itemCount;
    b.pressRowCap = static_cast<std::uint32_t>(state.plan.pressRowCap);
    b.rowsCapped = state.plan.pressCapped
                       ? static_cast<std::uint32_t>(state.plan.rowsSelected - state.plan.rows.size())
                       : 0;
    b.postCommitGapMs = batchGapToken.load() == runToken ? batchPostCommitGapMs.load() : -1;
    b.lateEvents = run.lateEvents;
    b.lateDropped = run.lateDropped;
    b.slimRows = slimRows;
    b.latched = state.latched;
    b.balanceLimited = state.plan.balanceLimited;
    b.runLimited = state.plan.runLimited;
    b.balanceUnknown = state.plan.balanceUnknown;
    b.samplesValid = run.samplesValid;
    b.reconciled = run.reconciled;
    b.reconcileOk = run.reconcileOk;
    b.textFallback = text.fallback;
    b.unknownPlaceholders = text.unknownPlaceholders;
    b.shadowKnown = state.plan.shadowKnown;
    b.shadowMerchantPerk = state.plan.shadowMerchantPerk;
    b.shadowMultiplier = state.plan.shadowMultiplier;
    b.shadowHagglingRank = run.shadow.hagglingRank;
    b.shadowSpeech = run.shadow.speech;
    b.shadowAv107 = run.shadow.av107Percent;
    b.shadowAv146 = run.shadow.av146Percent;
    b.shadowBarterMin = run.shadow.barterMin;
    b.shadowBarterMax = run.shadow.barterMax;
    const auto nowUs = BatchNowUs();
    b.edgeToLineMs = run.edgeUs && nowUs >= run.edgeUs ? (nowUs - run.edgeUs) / 1000 : 0;
    Log(row);
    const auto read = static_cast<std::int32_t>(ruleRun.candidatesRead);
    const char* settingsReason = ruleRun.settingsReason;
    const auto dispatched = static_cast<std::int32_t>(state.rowsDispatched);
    const auto confirmed = static_cast<std::int32_t>(state.rowsConfirmed);
    FinishRuleRun(runReason);
    // The MCM last-run line names the batch (FinishRuleRun wrote the
    // per-attempt counters, which stay 0 on the batch path).
    RecordLastRunSummary("OK", read, dispatched, confirmed, RuleRunReason(runReason),
                         BatchOutcomeTerminal(line.outcome), settingsReason);
    // v0.1.0 (design r3 2.5): the MCM "last merchant check" pair of this
    // batch's confirmed rows: the merchant's price and the anywhere formula.
    // v0.8.0 (section 3.2, audit A-03): the right-hand side is the batch's
    // own price snapshot (the perk price with the switch ON, else the formula).
    const auto check = js::MerchantCheckFromBatch(state.plan, run.priceSnapshot.price);
    if (check.known) {
        merchantCheckMerchantGold.store(check.merchantGold);
        merchantCheckAnywhereGold.store(check.anywhereGold);
    }
    batchRun = BatchRun{};
    SetBatchPhase(BatchPhase::kNone);
    batchNotBeforeNs.store(0, std::memory_order_release);
}

// Retired callback tracing is intentionally absent from the product path.

// ---------------------------------------------------------------------------
// v0.1.0 (the v0.1.0 design r3): sell anywhere by
// direct conversion. A press with no BarterMenu open and SellAnywhere ON is
// gated (5.3), read and planned in its BeginRuleRun UI task, committed in
// slices of at most 8 ms (one AddTask per slice, queued by the pump on a
// later frame), and ends in one HUD line (6) plus one js_convert_run row and
// its slim js_convert_row rows (4.4). The inventory calls live only in
// ConvertEngine.h; the rules live in the engine-free Convert.h.
// ---------------------------------------------------------------------------

// Menus that block a sell-anywhere press although they neither pause the
// game nor count as item/application/modal menus (design appendix R2 4.2:
// QuickLoot's LootMenu, the console, the dialogue and the favorites menus).
// The HUD, the fader and cursor overlays are allowed.
constexpr std::array<std::string_view, 27> ConvertBlockingMenus{{
    "Console", "Console Native UI Menu", "Dialogue Menu", "MessageBoxMenu", "LootMenu",
    "Loading Menu", "Main Menu", "InventoryMenu", "ContainerMenu", "BarterMenu",
    "GiftMenu", "MagicMenu", "FavoritesMenu", "TweenMenu", "Journal Menu", "MapMenu", "StatsMenu",
    "Crafting Menu", "Lockpicking Menu", "Book Menu", "Sleep/Wait Menu", "Training Menu",
    "RaceSex Menu", "LevelUp Menu", "Tutorial Menu", "QuantityMenu", "Creation Club Menu"}};

bool ConvertMenuOpen(RE::UI* ui) {
    if (!ui) return true;
    if (ui->GameIsPaused() || ui->IsModalMenuOpen() || ui->IsItemMenuOpen() || ui->IsApplicationMenuOpen()) {
        return true;
    }
    for (const auto name : ConvertBlockingMenus) {
        if (ui->IsMenuOpen(name)) return true;
    }
    return false;
}

// The world terms of the gate (design appendix R2 4.2), read live. Main
// thread only (a UI task or an AddTask).
void FillConvertWorldFacts(RE::PlayerCharacter* player, js::ConvertWorldFacts& facts) {
    facts.loading = loadInProgress.load();
    auto* ui = RE::UI::GetSingleton();
    facts.menuOpen = ConvertMenuOpen(ui);
    facts.dialogue = ui && ui->IsMenuOpen("Dialogue Menu");
    if (auto* topics = RE::MenuTopicManager::GetSingleton(); topics && topics->speaker) facts.dialogue = true;
    if (!player) {
        facts.dead = true;
        return;
    }
    facts.combat = player->IsInCombat();
    facts.dead = player->IsDead();
    facts.killMove = player->IsInKillMove();
    auto* controls = RE::ControlMap::GetSingleton();
    facts.controlsDisabled =
        !controls || !controls->IsMovementControlsEnabled() || !controls->IsActivateControlsEnabled();
}

RenderedResult RenderConvertLine(const js::ConvertResultLine& line) {
    namespace rt = agh::junk_seller::result_text;
    RenderedResult out;
    if (line.outcome == js::ConvertOutcome::kNone) return out;
    const rt::Table none{};
    const auto prefix = rt::Render("$AGH_JS_RESULT_PREFIX", resultLocalized, resultEnglish, none);
    rt::Table values{{"p", prefix.text}, {"n", std::to_string(line.n)}, {"m", std::to_string(line.m)},
                     {"g", std::to_string(line.g)}, {"c", std::to_string(line.c)}, {"s", std::to_string(line.s)}};
    out.fallback = prefix.usedEnglishFallback;
    out.unknownPlaceholders = static_cast<std::uint32_t>(prefix.unknownPlaceholders);
    if (line.reason != js::ConvertReason::kNone) {
        const auto reason = rt::Render(js::ConvertReasonKey(line.reason), resultLocalized, resultEnglish, none);
        values["r"] = reason.text;
        out.fallback = out.fallback || reason.usedEnglishFallback;
        out.unknownPlaceholders += static_cast<std::uint32_t>(reason.unknownPlaceholders);
    }
    const auto body = rt::Render(js::ConvertLineKey(line), resultLocalized, resultEnglish, values);
    out.text = body.text;
    out.fallback = out.fallback || body.usedEnglishFallback;
    out.unknownPlaceholders += static_cast<std::uint32_t>(body.unknownPlaceholders);
    return out;
}

// Every sell-anywhere outcome is one HUD notification (design r3 section 6):
// the shared surface with hudOnly, fed a non-empty outcome.
const char* SurfaceConvertLine(const js::ConvertResultLine& line, const std::string& text) {
    js::BatchResultLine surfaced;
    surfaced.outcome = line.outcome == js::ConvertOutcome::kNone ? js::BatchOutcome::kNone : js::BatchOutcome::kPartial;
    return SurfaceResultLine(surfaced, text, true);
}

std::string ConvertLastRunGate(js::ConvertGate gate) {
    if (gate == js::ConvertGate::kOk) return "ANYWHERE";
    std::string token = "ANYWHERE_";
    for (const char* c = js::ConvertGateName(gate); *c; ++c) {
        token += (*c >= 'a' && *c <= 'z') ? static_cast<char>(*c - 'a' + 'A') : *c;
    }
    return token;
}

// v0.5.0: the observe-only frame window (FrameWindow.h). What a tick
// reads on the main thread: the Speech level of the PlayerSkills block (the
// receipt's speechLevel), the Speech actor value base (what the console's
// getbaseav reads), the character level and XP.
js::FrameTickFacts ReadFrameTickFacts(RE::PlayerCharacter* player) {
    js::FrameTickFacts facts;
    if (!player) return facts;
    float xp = 0.0F;
    js::ReadConvertSpeech(player, facts.speechLevel, xp);
    if (auto* owner = player->AsActorValueOwner()) facts.speechBase = owner->GetBaseActorValue(RE::ActorValue::kSpeech);
    facts.playerLevel = static_cast<std::int32_t>(player->GetLevel());
    if (auto* skills = player->GetPlayerRuntimeData().skills; skills && skills->data) facts.playerXp = skills->data->xp;
    return facts;
}

// Writes the ended window as one js_frame_window row and disarms. The window
// is copied out under the lock; the row is built and logged outside it.
void LogEndedFrameWindow(const js::FrameWindow& ended) {
    auto row = NewRow("js_frame_window", "frame");
    row.runToken = ended.runToken;
    row.frame = ended;
    Log(row);
}

// Arms the window when a planned press ends (FinishConvertRun, main thread).
// A window still open from an earlier press is ended ("superseded") first.
void ArmFrameWindowAfter(const ConvertRun& run, RE::PlayerCharacter* player) {
    const auto facts = ReadFrameTickFacts(player);
    const auto target = CurrentDelivery().accepted;
    js::FrameWindow previous{};
    {
        std::lock_guard<std::mutex> lock(frameMutex);
        if (frameWindow.armed && !frameWindow.done) {
            js::EndFrameWindow(frameWindow, "superseded");
            previous = frameWindow;
        }
        js::ArmFrameWindow(frameWindow, run.runToken, run.edgeUs, BatchNowUs(), target, facts);
        auto& w = frameWindow;
        w.containerDuring = frameContainerDuring.load(std::memory_order_relaxed);
        w.rowsSold = run.state.rowsSold;
        w.unitsSold = run.state.unitsSold;
        w.goldPaid = run.state.goldPaid;
        w.xpCalls = run.state.xpCalls;
        for (const auto& planned : run.state.plan.rows) {
            if (planned.state != js::ConvertRowState::kConfirmed) continue;
            w.listsSold += planned.lists;
            w.xpPoints += static_cast<double>(planned.value) * planned.units;
        }
        frameEpoch.store(epoch.load());
    }
    if (previous.armed) LogEndedFrameWindow(previous);
    frameArmed.store(true, std::memory_order_release);
}

// One tick (an SKSE task the pump keeps queued while a window is armed).
void RunFrameTick() {
    if (!frameArmed.load(std::memory_order_acquire)) return;
    const auto now = BatchNowUs();
    const auto facts = ReadFrameTickFacts(RE::PlayerCharacter::GetSingleton());
    const auto delivered = CurrentDelivery().delivered;
    js::FrameWindow ended{};
    {
        std::lock_guard<std::mutex> lock(frameMutex);
        auto& w = frameWindow;
        if (!w.armed) return;
        if (w.lineShownAtMs < 0) {
            const auto shown = lastLineShownUs.load(std::memory_order_relaxed);
            if (shown >= w.convertEndUs) w.lineShownAtMs = js::FrameOffsetMs(w, shown);
        }
        if (epoch.load() != frameEpoch.load() || loadInProgress.load()) {
            js::EndFrameWindow(w, "load");
        } else if (now > w.edgeUs && now - w.edgeUs > js::FrameWindowEndUs + 5000000) {
            // No tick for seconds past the window (a paused task queue).
            js::EndFrameWindow(w, "timeout");
        } else {
            js::ObserveFrameTick(w, now, facts, delivered);
        }
        if (!w.done) return;
        ended = w;
        w = js::FrameWindow{};
        frameArmed.store(false, std::memory_order_release);
    }
    LogEndedFrameWindow(ended);
}

// A container change of the player outside the barter menu (the sink).
void NoteFrameContainer(const RE::TESContainerChangedEvent& event) {
    constexpr RE::FormID playerRef = 0x14;
    if (event.oldContainer != playerRef && event.newContainer != playerRef) return;
    if (frameCounting.load(std::memory_order_relaxed)) frameContainerDuring.fetch_add(1, std::memory_order_relaxed);
    if (!frameArmed.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(frameMutex);
    js::ObserveFrameContainer(frameWindow, BatchNowUs());
}

// v0.5.1: a container change of the player during a planned press
// (the sink, outside the barter menu). One relaxed increment.
void CountConvertContainer(const RE::TESContainerChangedEvent& event) {
    constexpr RE::FormID playerRef = 0x14;
    if (event.oldContainer != playerRef && event.newContainer != playerRef) return;
    convertContainerEvents.fetch_add(1, std::memory_order_relaxed);
}

// A tracked stat, a menu or a save while a window is armed (the sinks).
void NoteFrameNamed(js::FrameEventKind kind, const char* name) {
    if (!frameArmed.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(frameMutex);
    js::ObserveFrameNamed(frameWindow, kind, name, BatchNowUs());
}

// The end of every sell-anywhere press (design r3 4.1 step E): classify, the
// one line (none after a load), the slim rows, the js_convert_run row, the
// MCM last-run line; then the run slot returns to idle.
void FinishConvertRun(const char* abort) {
    auto& run = convertRun;
    auto& state = run.state;
    // v0.5.1: the press's container events end with its conversion.
    const bool counted = convertCounting.exchange(false, std::memory_order_relaxed);
    const std::uint32_t containerEvents = counted ? convertContainerEvents.load(std::memory_order_relaxed) : 0;
    auto input = js::ConvertOutcomeInputFrom(state);
    if (run.gate != js::ConvertGate::kOk) input.gate = run.gate;
    const auto line = js::ClassifyConvertOutcome(input);
    const auto text = RenderConvertLine(line);
    const bool load = abort && std::strcmp(abort, "LOAD_CHANGED") == 0;
    const char* surfaceName = load ? "suppressed_load" : SurfaceConvertLine(line, text.text);
    auto* player = RE::PlayerCharacter::GetSingleton();
    std::int64_t goldAfter = -1;
    std::int32_t levelAfter = -1;
    float xpAfter = std::numeric_limits<float>::quiet_NaN();
    if (run.runToken && !load) {
        js::EngineConvertHost host(player, run.thresholds.sellStolen);
        std::vector<js::ConvertCount> none;
        std::int64_t gold = 0;
        if (host.ReadCounts({}, none, gold)) goldAfter = gold;
        js::ReadConvertSpeech(player, levelAfter, xpAfter);
    }
    std::uint32_t slimRows = 0;
    if (run.runToken) {
        auto slim = NewRow("js_convert_row", "convert");
        slim.runToken = run.runToken;
        slim.issueSource = "anywhere";
        for (const auto& planned : state.plan.rows) {
            if (slimRows >= js::ConvertMaxRows) break;
            slim.seq = NextSeq();
            auto& out = slim.convertRow;
            out = ConvertRowReceipt{};
            out.token = planned.token;
            out.baseObj = planned.formKey;
            out.value = planned.value;
            out.price = planned.price;
            out.units = planned.units;
            out.countBefore = planned.countBefore;
            out.countAfter = planned.countAfter;
            out.slice = planned.slice;
            out.xpCalls = planned.xpCalls;
            out.plainUnits = planned.plainUnits;
            out.instanceUnits = planned.instanceUnits;
            out.lists = planned.lists;
            out.stolenUnits = planned.stolenUnits;
            // v0.4.0: a keep-N row that leaves units, and its keep.
            out.partial = planned.partial;
            out.keep = planned.keep;
            // v0.5.1: the engine removal calls of the row.
            out.removeCalls = planned.removeCalls;
            out.category = js::ConvertCategoryName(planned.type);
            out.state = js::ConvertRowStateName(planned.state);
            Log(slim);
            ++slimRows;
        }
    }
    auto row = NewRow("js_convert_run", "convert");
    row.runToken = run.runToken;
    row.issueSource = "anywhere";
    // v0.8.0: the press's price snapshot (taken before the gate).
    row.priceSource = run.priceReceipt;
    row.reason = js::ConvertReasonName(line.reason);
    auto& c = row.convert;
    c.outcome = js::ConvertOutcomeName(line.outcome);
    c.gate = js::ConvertGateName(input.gate);
    c.stop = js::ConvertStopName(state.stop);
    c.surface = surfaceName;
    c.unitsEligible = state.plan.unitsEligible;
    c.unitsPlanned = state.plan.unitsPlanned;
    c.unitsSold = state.unitsSold;
    c.goldPlanned = state.plan.goldPlanned;
    c.goldPaid = state.goldPaid;
    c.goldBefore = run.goldBefore;
    c.goldAfter = goldAfter;
    c.entriesRead = state.plan.entriesRead;
    c.rowsEligible = state.plan.rowsEligible;
    c.rowsPlanned = static_cast<std::uint32_t>(state.plan.rows.size());
    c.rowsSold = state.rowsSold;
    c.rowsRefused = state.rowsRefused;
    c.rowsRolledBack = state.rowsRolledBack;
    c.rowsMismatch = state.rowsMismatch;
    c.rowsLost = state.rowsLost;
    c.slimRows = slimRows;
    const auto& ex = state.plan.excluded;
    c.exType = ex.type;
    c.exCategoryOff = ex.categoryOff;
    c.exRule = ex.rule;
    c.exAbsolute = ex.absolute;
    c.exOwner = ex.owner;
    c.exInvalid = ex.invalid;
    c.exNotPlain = ex.notPlain;
    c.exZeroGold = ex.zeroGold;
    c.exRunLimit = ex.runLimit;
    c.sellIngredients = run.thresholds.sellIngredients;
    // v0.2.0: the Categories page of the run's snapshot, the line parts,
    // the five "why kept" counters and the catalog sizes.
    c.junkValueOnly = run.thresholds.junkValueOnly;
    c.junkMaxValue = run.thresholds.junkMaxValue;
    c.sellStolen = run.thresholds.sellStolen;
    c.sellWeakWeapons = run.thresholds.sellWeakWeapons;
    c.sellWeakArmor = run.thresholds.sellWeakArmor;
    c.sellClothing = run.thresholds.sellClothing;
    // v0.3.0: weak enchanted loot of the run's snapshot.
    c.sellEnchantedLoot = run.thresholds.sellEnchantedLoot;
    c.knownEnchantmentsOnly = run.thresholds.knownEnchantmentsOnly;
    c.enchantedLootMaxValue = run.thresholds.enchantedLootMaxValue;
    // v0.4.0: the keep-limit categories of the run's snapshot.
    const auto& keepRules = run.thresholds;
    // v0.5.0: plus the four potion and poison categories.
    // v0.6.0: plus mid-tier gear, high-tier gear and jewelry.
    static_assert(std::tuple_size_v<decltype(ConvertReceipt::keepOn)> == js::SettingsKeepCount);
    c.keepOn = {keepRules.sellIngredientsAnywhere, keepRules.sellCookedFood, keepRules.sellRawFood,
                keepRules.sellDrinks, keepRules.sellOre, keepRules.sellGems, keepRules.sellPelts,
                keepRules.sellTools, keepRules.sellRestorePotions, keepRules.sellSkillPotions,
                keepRules.sellOtherPotions, keepRules.sellPoisons, keepRules.sellMidGear,
                keepRules.sellHighGear, keepRules.sellJewelry};
    c.keepLimit = {keepRules.keepIngredients, keepRules.keepCookedFood, keepRules.keepRawFood,
                   keepRules.keepDrinks, keepRules.keepOre, keepRules.keepGems, keepRules.keepPelts,
                   keepRules.keepTools, keepRules.keepRestorePotions, keepRules.keepSkillPotions,
                   keepRules.keepOtherPotions, keepRules.keepPoisons, keepRules.keepMidGear,
                   keepRules.keepHighGear, keepRules.keepJewelry};
    c.exBuiltin = ex.builtin;
    c.keptAtLimit = ex.keptAtLimit;
    c.keptProtected = ex.keptProtected;
    c.keptStolen = ex.keptStolen;
    c.keptEnchantUnknown = ex.keptEnchantUnknown;
    c.keptCategoryOff = ex.keptCategoryOff;
    c.stolenRows = state.plan.stolenRows;
    c.stolenUnits = state.plan.stolenUnits;
    c.stolenUnitsSold = state.stolenUnitsSold;
    c.rowsCleared = state.rowsCleared;
    c.catalogGuardGear = static_cast<std::uint32_t>(convertCatalog.guardGear.size());
    c.catalogGuardNpcs = static_cast<std::uint32_t>(convertCatalog.guardNpcs.size());
    c.catalogGuardSourcesMissing = convertCatalog.guardSourcesMissing;
    c.catalogUniques = static_cast<std::uint32_t>(convertCatalog.uniques.size());
    c.catalogCarpenterInputs = static_cast<std::uint32_t>(convertCatalog.carpenterInputs.size());
    c.catalogToolForms = static_cast<std::uint32_t>(convertCatalog.toolForms.size());
    c.catalogNeverForms = static_cast<std::uint32_t>(convertCatalog.neverForms.size());
    c.catalogEnchantLevels = static_cast<std::uint32_t>(convertCatalog.enchantLevels.size());
    c.catalogArtifactKeywords = static_cast<std::uint32_t>(convertCatalog.artifactKeywords.size());
    c.catalogCookedForms = static_cast<std::uint32_t>(convertCatalog.cookedForms.size());
    c.catalogCookInputs = static_cast<std::uint32_t>(convertCatalog.cookInputs.size());
    c.catalogDrinkForms = static_cast<std::uint32_t>(convertCatalog.drinkForms.size());
    c.catalogDrinkKeywords = static_cast<std::uint32_t>(convertCatalog.drinkKeywords.size());
    c.catalogQuestFoods = static_cast<std::uint32_t>(convertCatalog.questFoods.size());
    if (run.runToken) {
        const std::uint32_t kept[5] = {ex.keptAtLimit, ex.keptProtected, ex.keptStolen, ex.keptEnchantUnknown,
                                       ex.keptCategoryOff};
        for (std::size_t i = 0; i < lastKept.size(); ++i) lastKept[i].store(static_cast<std::int32_t>(kept[i]));
    }
    // w-probe-r60: rows sold per category, the plan's instance units, and
    // the kept-list kinds of the notPlain rows.
    for (const auto& planned : state.plan.rows) {
        if (planned.state != js::ConvertRowState::kConfirmed) continue;
        switch (planned.type) {
        case js::CandidateType::kJunk: ++c.soldJunk; break;
        case js::CandidateType::kIngredient: ++c.soldIngredients; break;
        case js::CandidateType::kWeakWeapon: ++c.soldWeakWeapons; break;
        case js::CandidateType::kWeakArmor: ++c.soldWeakArmor; break;
        case js::CandidateType::kClothing: ++c.soldClothing; break;
        case js::CandidateType::kEnchantedLoot: ++c.soldEnchantedLoot; break;
        case js::CandidateType::kKeepIngredient: ++c.soldKeep[js::kKeepIndexIngredients]; break;
        case js::CandidateType::kCookedFood: ++c.soldKeep[js::kKeepIndexCookedFood]; break;
        case js::CandidateType::kRawFood: ++c.soldKeep[js::kKeepIndexRawFood]; break;
        case js::CandidateType::kDrink: ++c.soldKeep[js::kKeepIndexDrinks]; break;
        case js::CandidateType::kOre: ++c.soldKeep[js::kKeepIndexOre]; break;
        case js::CandidateType::kGem: ++c.soldKeep[js::kKeepIndexGems]; break;
        case js::CandidateType::kPelt: ++c.soldKeep[js::kKeepIndexPelts]; break;
        case js::CandidateType::kTool: ++c.soldKeep[js::kKeepIndexTools]; break;
        case js::CandidateType::kRestorePotion: ++c.soldKeep[js::kKeepIndexRestorePotions]; break;
        case js::CandidateType::kSkillPotion: ++c.soldKeep[js::kKeepIndexSkillPotions]; break;
        case js::CandidateType::kOtherPotion: ++c.soldKeep[js::kKeepIndexOtherPotions]; break;
        case js::CandidateType::kPoison: ++c.soldKeep[js::kKeepIndexPoisons]; break;
        case js::CandidateType::kMidGear: ++c.soldKeep[js::kKeepIndexMidGear]; break;
        case js::CandidateType::kHighGear: ++c.soldKeep[js::kKeepIndexHighGear]; break;
        case js::CandidateType::kJewelry: ++c.soldKeep[js::kKeepIndexJewelry]; break;
        default: break;
        }
        if (planned.partial) {
            ++c.partialRowsSold;
            c.partialUnitsSold += planned.units;
        }
        if (planned.instanceUnits > 0) {
            ++c.instanceRowsSold;
            c.instanceUnitsSold += planned.instanceUnits;
        }
    }
    static_assert(std::tuple_size_v<decltype(ConvertReceipt::keptKinds)> == js::ConvertKeptKindCount);
    static_assert(std::tuple_size_v<decltype(ConvertReceipt::otherTypes)> == js::ConvertOtherTypesPerRun);
    c.instanceRows = state.plan.instanceRows;
    c.instanceUnits = state.plan.instanceUnits;
    c.instanceLists = state.plan.instanceLists;
    for (std::size_t i = 0; i < c.keptKinds.size() && i < ex.keptKinds.size(); ++i) c.keptKinds[i] = ex.keptKinds[i];
    c.otherTypeCount = ex.otherTypeCount < c.otherTypes.size() ? ex.otherTypeCount :
                       static_cast<std::uint32_t>(c.otherTypes.size());
    for (std::uint32_t i = 0; i < c.otherTypeCount; ++i) c.otherTypes[i] = ex.otherTypes[i];
    static_assert(std::tuple_size_v<decltype(ConvertReceipt::keptRows)> == js::ConvertKeptRowsPerRun);
    c.keptRowCount = ex.keptRowCount < c.keptRows.size() ? ex.keptRowCount :
                     static_cast<std::uint32_t>(c.keptRows.size());
    for (std::uint32_t i = 0; i < c.keptRowCount; ++i) {
        c.keptRows[i] = {ex.keptRows[i].formKey, ex.keptRows[i].kinds, ex.keptRows[i].otherType};
    }
    c.xpCalls = state.xpCalls;
    // v0.5.1: the removal calls, gold credits and stack rows of the
    // press, and the container events the sink counted meanwhile.
    c.removeCalls = state.removeCalls;
    c.goldCalls = state.goldCalls;
    c.stackRows = state.stackRows;
    c.stackFallbackRows = state.stackFallbackRows;
    c.containerEvents = containerEvents;
    c.speechLevelBefore = run.speechLevelBefore;
    c.speechLevelAfter = levelAfter;
    c.speechXpBefore = run.speechXpBefore;
    c.speechXpAfter = xpAfter;
    c.priceKnown = run.price.known;
    c.clamped = run.price.clamped;
    c.ratePercent = js::ConvertRatePercent(run.price);
    c.hagglingRank = run.inputs.hagglingRank;
    if (run.price.known) {
        c.speech = run.price.speech;
        c.factor = run.price.factor;
        c.multiplier = run.price.multiplier;
        c.mPrime = run.price.capped;
    }
    c.av107 = run.inputs.av107Percent;
    c.av146 = run.inputs.av146Percent;
    c.barterMin = run.inputs.barterMin;
    c.barterMax = run.inputs.barterMax;
    c.readUs = run.readUs;
    c.readCountsUs = state.readCountsUs;
    c.slices = state.slices;
    c.sliceMaxUs = state.sliceMaxUs;
    c.sliceTotalUs = state.sliceTotalUs;
    c.latched = state.latched;
    c.runLimited = state.plan.runLimited;
    c.textFallback = text.fallback;
    c.unknownPlaceholders = text.unknownPlaceholders;
    const auto nowUs = BatchNowUs();
    c.edgeToLineMs = run.edgeUs && nowUs >= run.edgeUs ? (nowUs - run.edgeUs) / 1000 : 0;
    Log(row);
    RecordLastRunSummary(ConvertLastRunGate(input.gate).c_str(), static_cast<std::int32_t>(state.plan.entriesRead),
                         static_cast<std::int32_t>(state.plan.rows.size()), static_cast<std::int32_t>(state.rowsSold),
                         abort && abort[0] ? abort : "none", js::ConvertOutcomeName(line.outcome), run.settingsReason);
    if (state.latched) convertLatched.store(true);
    // v0.5.0: the observe-only frame window of this press (dev key only).
    if (run.runToken && !load && devFrameWindow.load(std::memory_order_relaxed)) ArmFrameWindowAfter(run, player);
    frameCounting.store(false, std::memory_order_relaxed);
    convertRun = ConvertRun{};
    convertPhase.store(ConvertPhase::kNone, std::memory_order_release);
    convertNotBeforeNs.store(0, std::memory_order_release);
    convertToken.store(0, std::memory_order_release);
    ReleaseRuleRunSlot();
}

// The press (a UI task, from BeginRuleRun): the one settings snapshot, the
// gate before any read, the read, the plan. A refused press writes its row
// (runToken 0) and its one line; a planned press publishes the run and the
// pump starts its first slice.
void BeginConvertRun(std::uint64_t edgeUs) {
    auto& run = convertRun;
    run = ConvertRun{};
    run.edgeUs = edgeUs;
    run.loadGeneration = epoch.load();
    run.saveGeneration = saveGeneration.load();
    const auto snapshot = TakeRuleSettingsSnapshot();
    run.thresholds = js::SelectorThresholdsFromSettings(snapshot.values);
    auto* player = RE::PlayerCharacter::GetSingleton();
    run.inputs = ReadShadowPriceInputs();
    // v0.8.0 (section 3.1): one price snapshot per press, at plan time,
    // honouring this press's "Price follows perks" value.
    {
        const auto priced = ReadPriceSnapshot(snapshot.values.perkPrice, run.inputs);
        run.snapshot = priced.snapshot;
        run.priceReceipt = priced.receipt;
    }
    run.price = run.snapshot.price;
    js::ConvertWorldFacts facts;
    facts.enabled = snapshot.values.enabled;
    // v0.2.0: no category is guessed before the catalog exists.
    facts.runtimeOk = RuntimeOk() && !apiMismatch.load() && convertCatalogReady.load(std::memory_order_acquire);
    facts.settingsReadable = !configCorrupt.load() && snapshot.protectionFactsKnown;
    facts.latched = convertLatched.load();
    facts.busy = ruleRun.active || issueInFlight.load();
    facts.priceKnown = run.price.known;
    FillConvertWorldFacts(player, facts);
    run.gate = js::EvaluateConvertGate(facts);
    run.settingsReason = run.gate == js::ConvertGate::kOk ? "OK" : js::ConvertGateName(run.gate);
    if (run.gate != js::ConvertGate::kOk) {
        FinishConvertRun("GATE");
        return;
    }
    run.active = true;
    run.runToken = ++ruleRunTokenCounter;
    js::EngineConvertHost host(player, run.thresholds.sellStolen);
    std::vector<js::ConvertCount> none;
    std::int64_t gold = 0;
    if (host.ReadCounts({}, none, gold)) run.goldBefore = gold;
    js::ReadConvertSpeech(player, run.speechLevelBefore, run.speechXpBefore);
    const auto readStarted = NowNs();
    const js::ConvertProtection protection{&snapshot.names, &snapshot.forms};
    const auto entries = js::ReadConvertEntries(player, protection, run.thresholds, convertCatalog);
    auto plan = js::BuildConvertPlan(entries, run.thresholds, run.price);
    run.readUs = (NowNs() - readStarted) / 1000;
    run.state = js::BeginConvert(std::move(plan), edgeUs);
    if (run.state.done) {
        FinishConvertRun("none");
        return;
    }
    // v0.8.0 (section 3.4 step 4): this press sells; its price source is
    // the last sale's.
    NoteLastPriceSource(run.snapshot);
    PublishRuleRunActive();
    // v0.5.1: every planned press counts its container events (receipt).
    convertContainerEvents.store(0, std::memory_order_relaxed);
    convertCounting.store(true, std::memory_order_relaxed);
    // v0.5.0: the frame window counts the container events of the
    // conversion itself (dev key only; observe-only).
    if (devFrameWindow.load(std::memory_order_relaxed)) {
        frameContainerDuring.store(0, std::memory_order_relaxed);
        frameCounting.store(true, std::memory_order_relaxed);
    }
    convertToken.store(run.runToken, std::memory_order_release);
    convertNotBeforeNs.store(0, std::memory_order_release);
    convertPhase.store(ConvertPhase::kCommit, std::memory_order_release);
}

// One commit slice (an AddTask the pump queued; design r3 4.1 and 4.3): the
// load / save generations and the world gate first, then js::RunConvertSlice
// (at most ConvertSliceBudgetUs of rows). It never queues a task of its own;
// a slice that leaves rows sets the earliest start of the next one.
void RunConvertSliceTask(std::uint64_t token) {
    EnterTask();
    auto& run = convertRun;
    if (!run.active || run.runToken != token || convertPhase.load() != ConvertPhase::kCommit) return;
    if (epoch.load() != run.loadGeneration) {
        js::StopConvert(run.state, js::ConvertStop::kLoadSave, js::ConvertGate::kLoading);
        FinishConvertRun("LOAD_CHANGED");
        return;
    }
    auto* player = RE::PlayerCharacter::GetSingleton();
    js::ConvertWorldFacts facts;
    facts.saving = js::SaveStartedSince(run.saveGeneration, saveGeneration.load());
    // Audit B-01: a switch or settings change mid-run ends it before the slice.
    const js::Settings live = LiveSettingsCopy();
    facts.enabled = live.enabled && live.sellAnywhere;
    facts.settingsReadable = !configCorrupt.load();
    facts.runtimeOk = RuntimeOk() && !apiMismatch.load();
    FillConvertWorldFacts(player, facts);
    const auto gate = js::EvaluateConvertGate(facts);
    if (gate != js::ConvertGate::kOk) {
        const bool loadSave = gate == js::ConvertGate::kSaving || gate == js::ConvertGate::kLoading;
        js::StopConvert(run.state, loadSave ? js::ConvertStop::kLoadSave : js::ConvertStop::kGate, gate);
        FinishConvertRun(gate == js::ConvertGate::kSaving ? "SAVE_STARTED" : "GATE");
        return;
    }
    js::EngineConvertHost host(player, run.thresholds.sellStolen);
    const auto started = NowNs();
    js::RunConvertSlice(run.state, host);
    taskMeter.Observe((NowNs() - started) / 1000);
    if (run.state.done) {
        FinishConvertRun("none");
        return;
    }
    convertNotBeforeNs.store(NowNs() + js::ConvertSliceGapUs * 1000, std::memory_order_release);
}

// A press the input sink dropped while a sell-anywhere run was live: its one
// line ("a sale is already running") and a js_convert_run row (runToken 0).
// The live run is not touched.
void EmitConvertRunningLine() {
    EnterUiTask();
    const js::ConvertResultLine line{js::ConvertOutcome::kBlocked, js::ConvertReason::kRunning, 0, 0, 0};
    const auto text = RenderConvertLine(line);
    auto row = NewRow("js_convert_run", "convert", Phase::kUiTask);
    row.issueSource = "anywhere";
    row.reason = js::ConvertReasonName(line.reason);
    row.convert.outcome = js::ConvertOutcomeName(line.outcome);
    row.convert.gate = js::ConvertGateName(js::ConvertGate::kBusy);
    row.convert.textFallback = text.fallback;
    row.convert.unknownPlaceholders = text.unknownPlaceholders;
    row.convert.surface = SurfaceConvertLine(line, text.text);
    Log(row);
}

// ---------------------------------------------------------------------------
// B-06 / audit B-01: the inventory cost split, one stage per UI task. Each stage
// re-acquires the inventory inside its own task, because an engine container is
// never held across a task boundary. The preparation call cannot be interrupted,
// so a stage only starts when the measured preparation still fits the budget.
// ---------------------------------------------------------------------------
// r3 (R-F1 anomaly 2): the oracle visit budget of the current menu session. It
// lives in two atomics because the six stage tasks of one sample run one after
// another but are not guaranteed to run on one thread (probe-js05), while the
// rule itself is SessionVisitBudget in WObserve.h, so the offline harness
// drives exactly the reset and the charge the runtime uses.
std::atomic<std::uint64_t> oracleBudgetSession{}, oracleSessionVisits{};
// r56: the menu session that already wrote its ORACLE_BUDGET_EXHAUSTED
// row (one row per exhausted session; ~0 = none yet).
std::atomic<std::uint64_t> oracleBudgetExhaustedSession{~0ull};
void StoreOracleBudget(const SessionVisitBudget& budget) noexcept {
    oracleBudgetSession.store(budget.session);
    oracleSessionVisits.store(budget.used);
}
// Opens the budget of one menu session, publishing the reset the first time a
// session is seen. The caller passes one session number through the whole
// stage, so a generation that moves mid stage cannot reset between the
// allowance and the charge.
SessionVisitBudget OpenOracleBudget(std::uint64_t session) noexcept {
    SessionVisitBudget budget{oracleBudgetSession.load(), oracleSessionVisits.load()};
    if (BeginSessionVisits(budget, session)) StoreOracleBudget(budget);
    return budget;
}

// w-probe-r2: the cumulative visit caps. Past a cap the stage stops reading
// and says VISIT_CAP; the cap is never raised on its own, because how much a
// real run needs is a reading to take back to the design, not a number this
// code may pick (design section 4.3, W design section 6.1).
//
// A short allowance is recorded, but is not a cap hit until it actually stops
// a read. This preserves every read charge without invalidating a completed
// sample merely because its stage allowance was smaller than its maximum.
// w-probe-r8 (design ruling R8-2): the entry cap follows the INI VisitCap like the
// other two. R-W3 reported entryVisitCap=8192 next to xListVisitCap=262144
// because this one alone was a fixed constant (anomaly A-02).
std::uint64_t TakeEntryVisits(std::uint64_t requested) noexcept {
    const auto allowed = VisitAllowance(entryVisits.load(), js::VisitCap, requested);
    if (allowed < requested) ++visitAllowanceShort;
    return allowed;
}
std::uint64_t TakeXListVisits(std::uint64_t requested) noexcept {
    const auto allowed = VisitAllowance(xListVisits.load(), js::VisitCap, requested);
    if (allowed < requested) ++visitAllowanceShort;
    return allowed;
}
// The entry count the last preparation measured. A single GetInventory cannot
// be stopped half way, so the cap is checked against this estimate before the
// call and the real length is charged after it.

// w-probe-r1: the most recently published sample, as values. A funds question
// may only be answered from a sample that is still inside the same
// generations; anything else is FUNDS_UNKNOWN, never "insufficient".
// Builds the W observation block of one js_selected row. Every value is either
// something the probe observed or an explicit unknown; the eligibility field
// can only come out REJECTED or UNPROVEN (WObserve.h).
void BuildObservation(const SelectedInfo& info, WObservation& out) {
    const auto menu = menuGeneration.load(), load = loadGeneration.load();
    out.observationId = info.observationId;
    const bool quoteKnown = false;
    out.argsEvidenceCallIndex = -1;
    out.quoteSource = QuoteSourceNone;
    out.proposedQuantity = -1;
    out.quantitySource = QuantitySourceUnknown;
    out.arg3Type = -1;
    out.arg3Bool = -1;
    // No pre-dispatch quote path has been proven, so this stays null, and the
    // response lifetime stays unknown (design section 2.3).
    out.preDispatchQuoteSource = nullptr;
    out.responseLifetime = ResponseLifetimeUnknown;
    // The native side has no foreground or modal detection API. It says so and
    // names the source the parser has to supply instead of guessing.
    out.foregroundEvidence = EvidenceUnknown;
    out.foregroundSource = "external_png_phase";
    out.modalEvidence = EvidenceUnknown;
    out.modalSource = "external_png_phase";
    const bool sampleUsable = lastSample.valid.load() &&
                              lastSample.menuGeneration.load() == menu &&
                              lastSample.loadGeneration.load() == load &&
                              lastSample.inventoryGeneration.load() == inventoryGeneration.load();
    const double total = quoteKnown ? static_cast<double>(out.proposedQuantity) : 0.0;
    out.fundsCheck = ClassifyFunds(quoteKnown, sampleUsable, lastSample.vendorGold.load(), total);
    const bool fundsUnknown = NameEqualsExact(out.fundsCheck, FundsUnknown);
    // The uniqueID read this build refuses for want of a defensible phase.
    out.readPhaseUnproven = true;
    out.readPhaseReason = ReadPhaseUnproven;
    ++readPhaseUnprovenCount;
    out.missingPrerequisites = kAlwaysMissingPrerequisites;
    if (fundsUnknown) out.missingPrerequisites |= kPrereqFundsQuote;
    if (!sampleUsable) out.missingPrerequisites |= kPrereqStableSample;
    if (!info.selectionValid) out.missingPrerequisites |= kPrereqInstanceBinding;
    out.dispatchEligibility = DispatchEligibility(out.missingPrerequisites);
}

void LogSelection(RE::ItemList* list, const char* origin, bool force,
                  std::uint64_t oracleRequestId) {
    SelectedInfo info;
    const auto started = NowNs();
    // The selection read walks one entry's extra lists, and it walks them
    // inside the cumulative cap like every other read.
    const auto factsInventoryGeneration = inventoryGeneration.load();
    const auto xListAllowance = TakeXListVisits(js::VisitCap);
    const char* reason = ReadSelected(list, info, static_cast<std::uint32_t>(xListAllowance));
    xListVisits += info.visits;
    // W-3: this read's protection facts, for the issuing path to compare. A
    // read that raced an inventory change is published as not usable.
    selectionFacts = SelectionFacts{};
    selectionFacts.readOk = reason && NameEqualsExact(reason, "OK") &&
        inventoryGeneration.load() == factsInventoryGeneration;
    selectionFacts.capReached = !xListAllowance || info.visitCapReached;
    selectionFacts.baseObj = info.baseObj;
    selectionFacts.ownerForm = info.ownerForm;
    selectionFacts.ownerReadable = info.ownerReadable;
    selectionFacts.stolenKnown = info.stolenKnown;
    selectionFacts.questObject = info.questObject;
    selectionFacts.favorite = info.favorite;
    selectionFacts.worn = info.worn;
    selectionFacts.menuGeneration = menuGeneration.load();
    selectionFacts.loadGeneration = epoch.load();
    selectionFacts.inventoryGeneration = factsInventoryGeneration;
    selectionFacts.readMs = NowMs();
    const auto readUs = (NowNs() - started) / 1000;
    const bool changed = info.baseObj != lastSelectedBase || info.selectedIndexRead != lastSelectedIndex;
    // r5 / probe-js04 anomaly 1: this clock decides when a row is written. The
    // clock that decides when the poll runs is lastPollMs and lives in Pump.
    // The identification bundle and the older js_selected heartbeat share the
    // existing MaxSelectedRows cap. A cap stop makes the mixed-stack count and
    // the phase evidence visibly incomplete instead of filling the queue.
    if (selectedRows.fetch_add(1) >= MaxSelectedRows) { ++truncated; return; }
    const bool selectedDue = SelectionRowDue(force, changed, NowMs(), lastSelectionMs.load(), SelectionHeartbeatMs);
    info.observationId = ++observationId;
    auto binding = NewRow("js_retired", origin, Phase::kUiTask);
    binding.reason = reason;
    binding.selected = info;
    lastSelectedBase.store(info.baseObj);
    binding.baseObj = info.baseObj;
    binding.wObservation.observationId = info.observationId;
    FillWPhase(binding, false, 0);
    Log(binding);
    auto phase = NewRow("js_retired", origin, Phase::kUiTask);
    phase.reason = reason;
    phase.wObservation.observationId = info.observationId;
    FillWPhase(phase, false, 0);
    Log(phase);
    if (!selectedDue) return;
    lastSelectedBase = info.baseObj;
    lastSelectedIndex = info.selectedIndexRead;
    lastSelectionMs = NowMs();
    auto row = NewRow("js_retired", origin, Phase::kUiTask);
    row.reason = reason;
    row.selected = info;
    row.baseObj = info.baseObj;
    // The sample this row belongs to, named instead of carried half filled.
    row.oracleRequestId = oracleRequestId;
    row.perf.stage = "selection_read";
    row.perf.selectionReadUs = readUs;
    row.perf.selectionVisits = info.visits;
    row.perf.entryVisitsTotal = entryVisits.load();
    row.perf.xListVisitsTotal = xListVisits.load();
    row.perf.oracleVisitsTotal = oracleEntryVisits.load();
    row.perf.visitAllowanceShort = visitAllowanceShort.load();
    row.perf.oracleSessionVisits = oracleSessionVisits.load();
    if (info.xListTruncated) ++visitCapHits;
    row.perf.visitCapReached = info.xListTruncated;
    BuildObservation(info, row.wObservation);
    ++observationCount;
    Log(row);
}

RE::ItemList* CurrentItemList() {
    auto* ui = RE::UI::GetSingleton();
    auto menu = ui ? ui->GetMenu<RE::BarterMenu>() : nullptr;
    return menu ? menu->GetRuntimeData().itemList : nullptr;
}

class GfxEntryVisitor final : public RE::GFxValue::ObjectVisitor {
public:
    explicit GfxEntryVisitor(Row& row, std::string prefix = {}, unsigned depth = 0) :
        row_(row), prefix_(std::move(prefix)), depth_(depth) {}
    void Visit(const char* name, const RE::GFxValue& value) override {
        auto& gfx = row_.MutableGfx();
        ++gfx.gfxMemberCountTotal;
        if (gfx.gfxMemberCount >= Row::MaxGfxMembers) { gfx.gfxTruncated = true; return; }
        auto& member = gfx.gfxMembers[gfx.gfxMemberCount++];
        const std::string full = prefix_ + (name ? name : "");
        CopyText(member.name, full.c_str()); member.gfxType = static_cast<std::uint32_t>(value.GetType());
        if (value.IsNumber()) { member.valueKind = 1; member.number = value.GetNumber(); }
        else if (value.IsString()) { member.valueKind = 2; CopyText(member.text, value.GetString()); }
        if (depth_ == 0 && value.IsObject() && gfx.gfxMemberCount < Row::MaxGfxMembers) {
            GfxEntryVisitor nested(row_, full + ".", depth_ + 1); value.VisitMembers(&nested);
        }
    }
private:
    Row& row_; std::string prefix_; unsigned depth_{};
};

// w-probe-r18 (R18-4): true when a root member's name should be walked one
// level deeper (a category/tab list). Matched literally, case as found, on
// the raw member name (never the "." prefixed path GfxEntryVisitor builds).
bool NameLooksLikeCategoryOrList(const char* name) noexcept {
    if (!name) return false;
    const std::string_view view(name);
    return view.find("ategory") != std::string_view::npos ||
           view.find("List") != std::string_view::npos;
}

// w-probe-r18 (R18-4): visits the members of one nested group (a root
// member that qualified for the one nested level). Same shape and cap as
// GfxRootVisitor's own member array; never recurses further.
class GfxRootNestedVisitor final : public RE::GFxValue::ObjectVisitor {
public:
    explicit GfxRootNestedVisitor(Row::GfxRootNested& group) : group_(group) {}
    void Visit(const char* name, const RE::GFxValue& value) override {
        ++group_.memberCountTotal;
        if (group_.memberCount >= Row::MaxGfxRootMembers) { group_.truncated = true; return; }
        auto& member = group_.members[group_.memberCount++];
        CopyText(member.name, name ? name : "");
        member.gfxType = static_cast<std::uint32_t>(value.GetType());
        if (value.IsNumber()) { member.valueKind = 1; member.number = value.GetNumber(); }
        else if (value.IsBool()) { member.valueKind = 1; member.number = value.GetBool() ? 1.0 : 0.0; }
        else if (value.IsString()) { member.valueKind = 2; CopyText(member.text, value.GetString()); }
    }
private:
    Row::GfxRootNested& group_;
};

// w-probe-r18 (R18-4): visits the BarterMenu root object's own members. No
// mutation, no write-back, no callback. Numbers and booleans share valueKind 1
// (a boolean stored as 0/1 in `number`; `gfxType` still distinguishes them);
// strings use valueKind 2. A member whose GFx type is object and whose name
// looks like a category/tab list gets exactly one nested level.
class GfxRootVisitor final : public RE::GFxValue::ObjectVisitor {
public:
    explicit GfxRootVisitor(Row& row) : row_(row) {}
    void Visit(const char* name, const RE::GFxValue& value) override {
        auto& gfx = row_.MutableGfx();
        ++gfx.gfxRootMemberCountTotal;
        if (gfx.gfxRootMemberCount >= Row::MaxGfxRootMembers) { gfx.gfxRootTruncated = true; return; }
        auto& member = gfx.gfxRootMembers[gfx.gfxRootMemberCount++];
        CopyText(member.name, name ? name : "");
        member.gfxType = static_cast<std::uint32_t>(value.GetType());
        if (value.IsNumber()) { member.valueKind = 1; member.number = value.GetNumber(); }
        else if (value.IsBool()) { member.valueKind = 1; member.number = value.GetBool() ? 1.0 : 0.0; }
        else if (value.IsString()) { member.valueKind = 2; CopyText(member.text, value.GetString()); }
        if (value.IsObject() && NameLooksLikeCategoryOrList(name) &&
            gfx.gfxRootNestedCount < Row::MaxGfxRootNested) {
            auto& group = gfx.gfxRootNested[gfx.gfxRootNestedCount++];
            CopyText(group.parentName, name ? name : "");
            GfxRootNestedVisitor visitor(group);
            value.VisitMembers(&visitor);
        }
    }
private:
    Row& row_;
};

// w-probe-r18 (R18-4): the BarterMenu root object's own members, read once at
// the snapshot key (same UI task as ObserveGfxList, right after it). Fail-
// closed to gfxRootPresent=false when the movie or the "_root.Menu_mc" path
// is unavailable or not an object -- never a throw. Read-only: GetVariable
// then VisitMembers, no mutation, no write-back, no callback.
void ObserveGfxRoot(const char* trigger) noexcept {
    auto row = NewRow("js_retired", trigger, Phase::kUiTask);
    row.gfxTrigger = trigger;
    try {
        auto* ui = RE::UI::GetSingleton();
        auto menu = ui ? ui->GetMenu<RE::BarterMenu>() : nullptr;
        if (!menu || !menu->uiMovie) { row.gfxReason = "NO_MENU"; Log(row); return; }
        RE::GFxValue rootValue;
        if (!menu->uiMovie->GetVariable(&rootValue, "_root.Menu_mc") || !rootValue.IsObject()) {
            row.gfxReason = "NOT_OBJECT"; Log(row); return;
        }
        row.gfxRootPresent = true;
        GfxRootVisitor visitor(row);
        rootValue.VisitMembers(&visitor);
    } catch (...) {
        row.gfxReason = "EXCEPTION";
        // R21-4: reset means "start over with an empty payload", not
        // "zero the fields in place" -- the payload lives on the heap now.
        // w-probe-r20 (R21-4): the GFx arrays live in a heap GfxPayload, never
        // inline in Row; Row stays a few KB on the UI task thread stack.
        row.gfxRootPresent = false; row.gfx.reset();
    }
    Log(row);
}

void ObserveGfxEntry(const char* trigger, bool priceKnown = false, double price = 0.0) noexcept {
    auto row = NewRow("js_retired", trigger, Phase::kUiTask);
    row.gfxTrigger = trigger; row.gfxArgsUnitPriceKnown = priceKnown; row.gfxArgsUnitPrice = price;
    try {
        auto* list = CurrentItemList();
        if (!list) { row.gfxReason = "NO_ITEMLIST"; Log(row); return; }
        RE::GFxValue index;
        if (!list->root.GetMember("selectedIndex", &index) || !index.IsNumber()) { row.gfxReason = "NO_INDEX"; Log(row); return; }
        const auto indexNumber = index.GetNumber();
        if (!std::isfinite(indexNumber) || std::trunc(indexNumber) != indexNumber ||
            indexNumber < 0.0 || indexNumber > static_cast<double>((std::numeric_limits<std::int32_t>::max)())) {
            row.gfxReason = "INDEX_RANGE"; Log(row); return;
        }
        row.gfxSelectedIndex = static_cast<std::int32_t>(indexNumber);
        if (row.gfxSelectedIndex < 0 || static_cast<std::size_t>(row.gfxSelectedIndex) >= list->items.size()) { row.gfxReason = "INDEX_RANGE"; Log(row); return; }
        auto* item = list->items[row.gfxSelectedIndex];
        if (!item || !item->data.objDesc) { row.gfxReason = "NO_ENTRY"; Log(row); return; }
        row.baseObj = item->data.objDesc->object ? item->data.objDesc->object->GetFormID() : 0;
        row.count = item->data.objDesc->countDelta;
        if (!item->obj.IsObject()) { row.gfxReason = "NON_OBJECT"; Log(row); return; }
        GfxEntryVisitor visitor(row); item->obj.VisitMembers(&visitor);
    } catch (...) { row.gfxReason = "EXCEPTION"; row.gfx.reset(); }
    Log(row);
}

// w-probe-r16 (R16-2): the list-wide counterpart of ObserveGfxEntry. Every
// ItemList entry's rendered GFx object is read by name only (GetMember),
// read-only, never a mutating call and never a generic member walk. A
// missing or non-numeric/non-string member stays null; the walk is bounded
// by js::PopulateGfxListEntries so a large list cannot grow the row past
// Row::MaxGfxListEntries.
// w-probe-r17 (R17-3): adds filterFlag/filteredIndex/itemIndex/type/
// formType/equipState (the same numeric members already proven on
// js_gfx_entry, a live test log and rw12) and reads the
// display name from `text`, the GFx string member the r15 `str` map showed
// for the selected entry (`a live test log`,
// js_gfx_entry.members: name="text" str="..."). The plain "name" member is
// not on the live object, so the r16 read never populated it.
void ObserveGfxList(const char* trigger) noexcept {
    auto row = NewRow("js_retired", trigger, Phase::kUiTask);
    row.gfxTrigger = trigger;
    const auto started = NowNs();
    try {
        auto* list = CurrentItemList();
        if (!list) { row.gfxReason = "NO_ITEMLIST"; Log(row); return; }
        js::PopulateGfxListEntries(row, list->items.size(),
            [&](std::size_t i, Row::GfxListEntry& dst) {
                auto* item = list->items[static_cast<RE::BSTArray<RE::ItemList::Item*>::size_type>(i)];
                if (!item || !item->obj.IsObject()) return;
                RE::GFxValue value;
                if (item->obj.GetMember("baseId", &value) && value.IsNumber() &&
                    std::isfinite(value.GetNumber())) {
                    dst.baseId = static_cast<std::uint32_t>(value.GetNumber()); dst.baseIdKnown = true;
                }
                if (item->obj.GetMember("eslId", &value) && value.IsNumber() &&
                    std::isfinite(value.GetNumber())) {
                    dst.eslId = value.GetNumber(); dst.eslIdKnown = true;
                }
                if (item->obj.GetMember("count", &value) && value.IsNumber() &&
                    std::isfinite(value.GetNumber())) {
                    dst.count = value.GetNumber(); dst.countKnown = true;
                }
                if (item->obj.GetMember("infoValue", &value) && value.IsNumber() &&
                    std::isfinite(value.GetNumber())) {
                    dst.infoValue = value.GetNumber(); dst.infoValueKnown = true;
                }
                if (item->obj.GetMember("infoValueWeight", &value) && value.IsNumber() &&
                    std::isfinite(value.GetNumber())) {
                    dst.infoValueWeight = value.GetNumber(); dst.infoValueWeightKnown = true;
                }
                if (item->obj.GetMember("infoWeight", &value) && value.IsNumber() &&
                    std::isfinite(value.GetNumber())) {
                    dst.infoWeight = value.GetNumber(); dst.infoWeightKnown = true;
                }
                if (item->obj.GetMember("filterFlag", &value) && value.IsNumber() &&
                    std::isfinite(value.GetNumber())) {
                    dst.filterFlag = value.GetNumber(); dst.filterFlagKnown = true;
                }
                if (item->obj.GetMember("filteredIndex", &value) && value.IsNumber() &&
                    std::isfinite(value.GetNumber())) {
                    dst.filteredIndex = value.GetNumber(); dst.filteredIndexKnown = true;
                }
                if (item->obj.GetMember("itemIndex", &value) && value.IsNumber() &&
                    std::isfinite(value.GetNumber())) {
                    dst.itemIndex = value.GetNumber(); dst.itemIndexKnown = true;
                }
                if (item->obj.GetMember("type", &value) && value.IsNumber() &&
                    std::isfinite(value.GetNumber())) {
                    dst.type = value.GetNumber(); dst.typeKnown = true;
                }
                if (item->obj.GetMember("formType", &value) && value.IsNumber() &&
                    std::isfinite(value.GetNumber())) {
                    dst.formType = value.GetNumber(); dst.formTypeKnown = true;
                }
                if (item->obj.GetMember("equipState", &value) && value.IsNumber() &&
                    std::isfinite(value.GetNumber())) {
                    dst.equipState = value.GetNumber(); dst.equipStateKnown = true;
                }
                // w-probe-r17: the display name lives on `text`, not `name`
                // (r15 str map, design rulings R17-2/R17-3).
                if (item->obj.GetMember("text", &value) && value.IsString()) {
                    CopyText(dst.name, value.GetString()); dst.nameKnown = true;
                }
            });
    } catch (...) {
        row.gfxReason = "EXCEPTION";
        row.gfx.reset();
    }
    row.perf.stage = "gfx_list_read";
    row.perf.totalUs = (NowNs() - started) / 1000;
    if (const auto& gfx = row.gfx) {
        row.perf.entryCount = gfx->gfxListEntryCountTotal;
        row.perf.truncated = gfx->gfxListTruncated;
    }
    Log(row);
}

// ---------------------------------------------------------------------------
// r5 / probe-js05 anomaly 2: one oracle job is six stages and one task per
// stage. A job is claimed by whoever wants a snapshot, filled in by the stage
// tasks, and written out by the publish stage on its own js_oracle row. Rows
// that asked for a snapshot carry oracleRequestId and are never held back by
// it, so the container events that answer O-2 reach the receipt immediately.
// ---------------------------------------------------------------------------
std::uint64_t StartOracle(const char* origin, std::uint64_t requestedId = 0,
                          std::uint32_t targetBase = 0);

// The purpose of a request, taken from the origin the caller named. The origin
// string and the kind are always stored together, so an upgraded queued slot
// cannot claim a purpose its sample will not serve.
OracleRequestKind OracleRequestKindOf(const char* origin) noexcept {
    if (NameEqualsExact(origin, "issue_settle")) return OracleRequestKind::kIssueSettle;
    if (NameEqualsExact(origin, "issue_before")) return OracleRequestKind::kIssueBefore;
    return OracleRequestKind::kQuiet;
}

bool RunOracleStage(UiStage stage, std::uint64_t start) {
    const auto slot = OracleStageSlot(stage);
    if (slot >= OracleStageSlots) return false;
    if (slot < 1 || slot > 3) ++oracleSample.stageChunks[slot];
    if (slot == 3 && !oracleWalk.chunks) {
        vencSlotBalance = {};
        vencSlotBalance.sampleId = oracleSample.sampleId;
        vencSlotBalance.startMenuGeneration = menuGeneration.load();
        vencSlotBalance.startLoadGeneration = loadGeneration.load();
        vencSlotBalance.startInventoryGeneration = inventoryGeneration.load();
    }
    // A load between the request and this stage makes the reading meaningless
    // (B-04). The remaining candidates are named as unsampled and the publish
    // stage says EPOCH_STALE.
    if (oracleSample.epoch != epoch.load()) {
        if (slot >= 1 && slot <= 3 && oracleWalk.chunks) {
            oracleWalk.abandonReason = "LOAD_CHANGED";
            js::FinishWalkSlot(oracleWalk, oracleSample, slot, false);
            oracleWalk = {};
            oracleStageNotBeforeNs.store(0);
        } else {
            oracleSample.unsampled |= OracleSlotFailureBits(slot);
        }
        return false;
    }
    // F1b reads the player and VENC base count inside their chunked gold walks.
    // The retained helper has no target count to serve on the other slots.
    if (slot != 1 && slot != 3 && RunOracleBaseCount(slot, oracleSample)) {
        oracleSample.truncated = true;
        oracleSample.truncatedStages |= 1u << slot;
    }
    const auto deadline = DeadlineNs(start, StageBudgetUs(js::TaskBudgetUs));
    const auto bit = OracleSlotBit(slot);
    if (bit && (slot < 1 || slot > 3) &&
        !TakeStage(NowNs(), deadline, bit, oracleSample)) return false;
    // w-probe-r2 / r3: the visit cap (design section 4.3, r3 section 10). A
    // repeated read is charged again, and past the cap the stage stops reading
    // instead of quietly paying for more. The stage says so; it does not report
    // a reading it did not take. r7 spends the configured VisitCap per menu
    // session and across the process: R-W2 showed that the earlier fixed 4,096
    // bound could not reach the target in a large inventory.
    // One session number is read here and used for both the allowance and the
    // charge, so a menu generation that moves mid stage cannot reset the budget
    // between them.
    const auto session = menuGeneration.load();
    auto budget = OpenOracleBudget(session);
    // W-4: VisitCap is the configured oracle entry cap as well as the xList
    // cap. The deadline above remains an independent per-stage gate.
    // r56 (design ruling 2026-09-29 on heavy-fix decision 2): the runaway
    // guard is the barter session's own budget only (js::OracleStageAllowance).
    // Up to r55 the allowance was also bounded by the process-lifetime total
    // oracleEntryVisits, which nothing resets: on a ~1,700-entry inventory a
    // sample costs about 1,700-2,000 visits, so after a few dozen barter
    // visits in one game process every later sample was VISIT_CAP, the batch
    // before-sample was unusable and every press sold nothing until restart.
    // oracleEntryVisits stays the process total the receipts report.
    const auto oracleCap = js::VisitCap;
    const auto allowance = static_cast<std::uint32_t>(js::OracleStageAllowance(budget, oracleCap));
    // An exhausted session budget is a runaway session, so reaching it is
    // written out once per session instead of being left to a counter.
    if (!allowance && oracleBudgetExhaustedSession.exchange(session) != session) {
        auto exhausted = NewRow("js_warning", "oracle_budget", Phase::kUiTask);
        exhausted.reason = "ORACLE_BUDGET_EXHAUSTED";
        exhausted.perf.stage = "oracle_budget";
        exhausted.perf.oracleVisitsTotal = oracleEntryVisits.load();
        exhausted.perf.oracleSessionVisits = budget.used;
        exhausted.perf.visitCapReached = true;
        exhausted.pendingObservations = pendingObservationTasks.load();
        Log(exhausted);
    }
    // A short allowance is visible in HEALTH and the row, but does not make
    // this sample invalid unless the slot actually consumes it and stops.
    if (allowance < oracleCap) {
        ++visitAllowanceShort;
    }
    const bool baseOnlyAfterCap = !allowance && (slot == 1 || slot == 3) && oracleSample.targetBase;
    if (!allowance) {
        if (!oracleSample.visitCapReached) ++visitCapHits;
        oracleSample.visitCapReached = true;
        oracleSample.unsampled |= OracleSlotFailureBits(slot);
        if (!baseOnlyAfterCap) {
            if (slot >= 1 && slot <= 3) {
                oracleWalk.abandonReason = "VISIT_CAP";
                js::FinishWalkSlot(oracleWalk, oracleSample, slot, false);
                ++oracleSample.stageChunks[slot];
                oracleWalk = {};
            }
            return false;
        }
    }
    if (slot >= 1 && slot <= 3) {
        if (!oracleWalk.chunks) {
            oracleWalk = {};
            oracleWalk.refFormID = slot == 1 ? 0x14 :
                (slot == 2 ? oracleSample.merchantActorID : oracleSample.merchantContainerID);
            oracleWalk.startMs = NowMs();
            oracleWalk.menuGeneration = menuGeneration.load();
            oracleWalk.loadGeneration = loadGeneration.load();
            oracleWalk.inventoryGeneration = inventoryGeneration.load();
            oracleWalk.epoch = epoch.load();
            oracleWalk.barterOpenAtStart = barterOpen.load();
            oracleWalk.baseWanted = (slot == 1 || slot == 3) && oracleSample.targetBase;
            oracleWalk.baseDone = !oracleWalk.baseWanted;
            oracleWalk.base.targetFormID = oracleSample.targetBase;
            auto* ref = oracleWalk.refFormID == 0x14
                ? static_cast<RE::TESObjectREFR*>(RE::PlayerCharacter::GetSingleton())
                : RE::TESForm::LookupByID<RE::TESObjectREFR>(oracleWalk.refFormID);
            auto* changes = ref ? ref->GetInventoryChanges(true) : nullptr;
            oracleWalk.changesAddr = reinterpret_cast<std::uintptr_t>(changes);
        }
        if (baseOnlyAfterCap) {
            oracleWalk.gold = {};
            oracleWalk.goldWanted = false;
            oracleWalk.goldCapped = true;
        }
        std::uint32_t visits = 0, goldVisits = 0;
        const auto restartsBefore = oracleWalk.restarts;
        js::WalkResult result = js::WalkResult::kAbandon;
        try {
            bool matches = oracleWalk.refFormID &&
                (slot == 1 || (slot == 2 && oracleWalk.refFormID == oracleSample.merchantActorID) ||
                 (slot == 3 && oracleWalk.refFormID == oracleSample.merchantContainerID));
            if (matches && slot == 3) {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(oracleSample.merchantActorID);
                auto* container = js::ResolveMerchantContainer(actor);
                matches = container && container->GetFormID() == oracleWalk.refFormID;
            }
            result = js::RunWalkChunk(oracleWalk, oracleSample, deadline, NowMs(),
                menuGeneration.load(), loadGeneration.load(), inventoryGeneration.load(),
                epoch.load(), barterOpen.load(), matches,
                oracleWalk.gold.scanned + allowance, visits, goldVisits);
        } catch (...) { oracleWalk.abandonReason = "EXCEPTION"; }
        oracleSample.scannedEntries += goldVisits;
        oracleSample.entryVisits += goldVisits;
        oracleEntryVisits += goldVisits;
        ChargeSessionVisits(budget, goldVisits);
        StoreOracleBudget(budget);
        const auto cost = (NowNs() - start) / 1000;
        oracleSample.stageCostUs[slot] += static_cast<std::uint32_t>(cost);
        oracleSample.costUs += cost;
        ++oracleSample.stageChunks[slot];
        oracleSample.walkRestarts += oracleWalk.restarts - restartsBefore;
        if (oracleWalk.goldCapped && !oracleSample.visitCapReached) {
            ++visitCapHits;
            oracleSample.visitCapReached = true;
        }
        if (result == js::WalkResult::kContinue && oracleWalk.barterOpenAtStart) {
            oracleStageNotBeforeNs.store(NowNs() + js::ChunkGapUs * 1000);
            return true;
        }
        oracleStageNotBeforeNs.store(0);
        if (result != js::WalkResult::kDone) {
            if (result == js::WalkResult::kContinue) oracleWalk.abandonReason = "MENU_CLOSED";
            if (std::strcmp(oracleWalk.abandonReason, "ENTRY_LIMIT") == 0)
                oracleSample.visitCapReached = true;
        }
        js::FinishWalkSlot(oracleWalk, oracleSample, slot, result == js::WalkResult::kDone);
        if (slot == 3) {
            vencSlotBalance.gold = oracleSample.merchantContainerGold;
            vencSlotBalance.endMenuGeneration = menuGeneration.load();
            vencSlotBalance.endLoadGeneration = loadGeneration.load();
            vencSlotBalance.endInventoryGeneration = inventoryGeneration.load();
            vencSlotBalance.merchantContainerID = oracleSample.merchantContainerID;
            vencSlotBalance.complete = js::RuleVencSlotComplete(
                true, (oracleSample.truncatedStages & (1u << slot)) != 0,
                (oracleSample.unsampled & OracleSlotFailureBits(slot)) != 0,
                oracleSample.visitCapReached, oracleSample.merchantResolved,
                oracleSample.containerResolved) &&
                js::RuleVencSlotMerchantWindow(oracleMerchantWindow,
                    vencSlotBalance.startMenuGeneration, vencSlotBalance.endMenuGeneration);
        }
        oracleWalk = {};
        return false;
    }
    const bool truncatedBefore = oracleSample.truncated;
    const auto scannedBefore = oracleSample.scannedEntries;
    // The sample may already be truncated by an earlier slot. Isolate slot 3's
    // walk so a partial positive gold tally cannot look complete in F1a.
    if (slot == 3) oracleSample.truncated = false;
    if (slot == 0) oracleMerchantWindow.startMenuGeneration = menuGeneration.load();
    RunOracleSlot(slot, oracleSample, deadline, js::MerchantFallback != 0, allowance);
    if (slot == 0) oracleMerchantWindow.endMenuGeneration = menuGeneration.load();
    const bool vencWalkTruncated = slot == 3 && oracleSample.truncated;
    oracleSample.truncated |= truncatedBefore;
    const auto scanned = oracleSample.scannedEntries - scannedBefore;
    oracleSample.entryVisits += scanned;
    oracleEntryVisits += scanned;
    // The same visits, charged once to the session budget they were spent in.
    ChargeSessionVisits(budget, scanned);
    StoreOracleBudget(budget);
    if (allowance < oracleCap && scanned >= allowance &&
        (vencWalkTruncated || (oracleSample.truncated && !truncatedBefore))) {
        ++visitCapHits;
        oracleSample.visitCapReached = true;
    }
    const auto cost = (NowNs() - start) / 1000;
    oracleSample.stageCostUs[slot] = static_cast<std::uint32_t>(cost);
    oracleSample.costUs += cost;
    if (!slot) oracleSample.prepareUs = cost;
    if (vencWalkTruncated || (oracleSample.truncated && !truncatedBefore))
        oracleSample.truncatedStages |= 1u << slot;
    if (slot == 3) {
        vencSlotBalance.gold = oracleSample.merchantContainerGold;
        vencSlotBalance.endMenuGeneration = menuGeneration.load();
        vencSlotBalance.endLoadGeneration = loadGeneration.load();
        vencSlotBalance.endInventoryGeneration = inventoryGeneration.load();
        vencSlotBalance.merchantContainerID = oracleSample.merchantContainerID;
        vencSlotBalance.complete = js::RuleVencSlotComplete(
            true, (oracleSample.truncatedStages & (1u << slot)) != 0,
            (oracleSample.unsampled & OracleSlotFailureBits(slot)) != 0,
            oracleSample.visitCapReached, oracleSample.merchantResolved,
            oracleSample.containerResolved) &&
            js::RuleVencSlotMerchantWindow(oracleMerchantWindow,
                                           vencSlotBalance.startMenuGeneration,
                                           vencSlotBalance.endMenuGeneration);
    }
    return false;
}

// W-3 ruling 2: the attempt is confirmed from what was measured, not from a
// quiet inventory generation (the sale's own two events always move it, so
// the r5 equality could never hold). Only the settle sample this attempt asked
// for is used. CONFIRMED needs the exact event set (item player->vendor and
// gold vendor->player, each once, and no other container event since the
// dispatch) and the after-sample equalities on the measured pairs. The
// inventory generation is left on the rows as a diagnostic only.
void CompleteIssueFromSample(const OracleSample& sample) {
    if (!issueContext.active || !issueContext.afterPending ||
        sample.sampleId != issueContext.settleSampleId) return;
    issueContext.observedEvents = containerEventTotal.load() - issueContext.startContainerEvents;
    const bool eventsExact = EventSetExact(issueContext.itemEvent, issueContext.goldEvent,
                                           false, issueContext.observedEvents,
                                           issueContext.handledEvents);
    const bool sampleUsable = sample.sampleValid && sample.sampleId > issueContext.beforeSampleId &&
        sample.targetBase == issueContext.baseObj &&
        sample.endMenuGeneration == issueContext.startMenuGeneration &&
        sample.endLoadGeneration == issueContext.startLoadGeneration &&
        sample.merchantContainerID == issueContext.vencID;
    issueContext.afterSampleId = sampleUsable ? sample.sampleId : 0;
    issueContext.measuredAfter = MeasuredPair{sample.playerBaseCount, sample.vendorBaseCount,
                                              sample.playerGold, sample.merchantContainerGold};
    issueContext.afterBalances = {sample.playerGold, sample.merchantActorGold,
                                  sample.merchantContainerGold, sample.merchantInventoryGold};
    for (unsigned i = 0; i < issueContext.balanceDeltas.size(); ++i) {
        issueContext.balanceDeltas[i] = issueContext.afterBalances[i] - issueContext.beforeBalances[i];
    }
    // Ruling 3: the item deltas are derived from the two measured pairs.
    issueContext.measuredPairsValid = sampleUsable && MeasuredPairKnown(issueContext.measuredBefore) &&
        MeasuredPairKnown(issueContext.measuredAfter);
    issueContext.inventoryDeltas = issueContext.measuredPairsValid
        ? MeasuredInventoryDeltas(issueContext.measuredBefore, issueContext.measuredAfter)
        : std::array<std::int32_t, 2>{};
    issueContext.afterValid = issueContext.measuredPairsValid;
    const bool pairsMatch = issueContext.measuredPairsValid &&
        MeasuredPairsMatch(issueContext.measuredBefore, issueContext.measuredAfter,
                           issueContext.quantity, issueContext.unitPrice);
    issueContext.deltaMatch = eventsExact && pairsMatch &&
        BalanceDeltasMatch(issueContext.balanceDeltas, issueContext.inventoryDeltas,
                           issueContext.quantity, issueContext.unitPrice);
    issueContext.afterPending = false;
    // R12-1: the settle sample this attempt asked for did arrive.
    issueContext.settleSampleState = SettleSampleStateName(SettleSampleState::kPublished);
    issueContext.settleElapsedMs = AttemptElapsedMs(issueContext.settleRequestMs, NowMs());
    issueContext.settlePublishedMs = NowMs();
    issueContext.settleOracle = sample;
    // w-probe-r24 fix round 3 (A-02): the settle sample is diagnostic only
    // (deltaMatch/measuredPairsValid/settleSampleState above are receipt
    // fields, nothing more). It must never call Finish or FinalizeIssue and
    // must never advance the rule run: CheckIssueTimeout, gated on
    // AttemptDeadlineMs, is the single terminal site for every attempt,
    // rule-issued or bind/manual alike. The old call here reached Finish as
    // soon as the settle sample published, even off the deadline and with a
    // still-undrained extra event in the ring (eventsExact false), so a sale
    // could be finalized CONFIRMED before CheckIssueTimeout ever ran.
    // eventsExact/pairsMatch above still gate deltaMatch, which
    // CheckIssueTimeout's own Finish() call reads from this same
    // issueContext.
}

// w-probe-r12 (R12-1): the deadline runs from the settle request once the
// settle has asked for its sample, because that is the measurement the attempt
// is waiting for, and it is 5000 ms: R-W7 measured 1010 ms for one six-stage
// sample in a paused BarterMenu, which the r11 deadline of 1000 ms from the
// dispatch could never cover. The row states what became of the settle sample
// and how long the attempt waited. A menu close or a load boundary still ends
// the attempt at once, on their own paths.
void CheckIssueTimeout() {
    if (!issueContext.active || !issueInFlight.load()) return;
    const auto nowMs = NowMs();
    const auto startMs = issueContext.startNs / 1000000;
    if (!AttemptDeadlineReached(startMs, issueContext.settleRequestMs, nowMs, AttemptDeadlineMs)) return;
    DrainAttemptEventsThroughDeadline();
    issueContext.settleSampleState = SettleSampleStateName(
        ClassifySettleSample(issueContext.settleRequestMs != 0, issueContext.afterPending,
                             issueContext.settleSampleId, lastPublishedSampleId.load()));
    issueContext.settleElapsedMs =
        AttemptElapsedMs(AttemptDeadlineBaseMs(startMs, issueContext.settleRequestMs), nowMs);
    // w-probe-r24 fix round 5 (A-01): the loss measurement of this attempt,
    // taken after the drain so every event that could still be examined has
    // been. observedEvents counts what the sink saw in this attempt's window
    // before any cap; handledEvents counts what the attempt examined; the
    // sink and ring drop counters cover what never got that far. Any of the
    // three being non-zero denies CONFIRMED (ClassifyAttempt) and names
    // events_lost as the timeout detail.
    issueContext.observedEvents = containerEventTotal.load() - issueContext.startContainerEvents;
    issueContext.eventsLost = js::AttemptEventsLost(attemptEventsLost.load(),
                                                    eventRingDrops.load() - issueContext.startEventRingDrops,
                                                    issueContext.observedEvents,
                                                    issueContext.handledEvents);
    issueMachine.NoteEventsLost(issueContext.eventsLost);
    const bool exactEventSet = issueMachine.EventPairComplete();
    const auto terminal = issueMachine.Finish(true, exactEventSet);
    const char* detail = terminal == SaleTerminal::kAttemptTimeout
        ? js::AttemptTimeoutDetail(issueMachine.state) : "ATTEMPT_TIMEOUT";
    if (terminal != SaleTerminal::kIssued) FinalizeIssue(terminal, detail);
}

void PublishOracle() {
    oracleSample.spanUs = (NowNs() - oracleSample.sampleNs) / 1000;
    // The closing edge of the sample window. A sample is built across six
    // tasks, so the generations, the event sequence and the merchant are read
    // again here; a change between the two edges invalidates the sample rather
    // than producing a "simultaneous" reading of two different moments.
    oracleSample.endLoadGeneration = loadGeneration.load();
    oracleSample.endMenuGeneration = menuGeneration.load();
    oracleSample.endInventoryGeneration = inventoryGeneration.load();
    oracleSample.lastEventSeq = lastEventSeqSeen.load();
    oracleSample.endMerchantID = ReResolveMerchant(oracleSample.merchantActorID);
    const auto* invalid = OracleSampleInvalidReason(oracleSample);
    oracleSample.sampleValid = invalid == nullptr;
    oracleSample.invalidationReason = invalid ? invalid : "none";
    ++sampleCount;
    lastPublishedSampleId.store(oracleSample.sampleId);
    if (oracleSample.sampleValid) ++stableSampleCount; else ++invalidSampleCount;
    // The values a later funds question may use, and only while the
    // generations still match. VENC is the observed pay source (W design
    // section 3.1), so that is the balance the question is asked against.
    lastSample.sampleId.store(oracleSample.sampleId);
    lastSample.menuGeneration.store(oracleSample.endMenuGeneration);
    lastSample.loadGeneration.store(oracleSample.endLoadGeneration);
    lastSample.inventoryGeneration.store(oracleSample.endInventoryGeneration);
    lastSample.lastEventSeq.store(oracleSample.lastEventSeq);
    lastSample.playerID.store(RE::PlayerCharacter::GetSingleton()
                                  ? RE::PlayerCharacter::GetSingleton()->GetFormID() : 0);
    lastSample.merchantContainerID.store(oracleSample.merchantContainerID);
    lastSample.playerGold.store(oracleSample.playerGold);
    lastSample.merchantActorGold.store(oracleSample.merchantActorGold);
    lastSample.merchantContainerGold.store(oracleSample.merchantContainerGold);
    lastSample.merchantInventoryGold.store(oracleSample.merchantInventoryGold);
    lastSample.vendorGold.store(oracleSample.merchantContainerGold);
    const auto verdict = SummarizeSample(oracleSample);
    lastSample.origin.store(oracleOrigin);
    lastSample.invalidReason.store(verdict.lastInvalidReason);
    lastSample.targeted.store(verdict.lastTargeted);
    for (std::size_t i = 0; i < OracleStageSlots; ++i)
        lastSample.stageCostUs[i].store(verdict.lastStageCostUs[i]);
    for (std::size_t i = 0; i < OracleStageSlots; ++i)
        lastSample.stageChunks[i].store(verdict.lastChunks[i]);
    lastSample.walkRestarts.store(verdict.lastRestarts);
    lastSample.scanned.store(verdict.lastScanned);
    lastSample.baseCountVisits.store(verdict.lastBaseCountVisits);
    lastSample.truncatedStages.store(verdict.lastTruncatedStages);
    lastSample.unsampled.store(verdict.lastUnsampled);
    lastSample.spanUs.store(verdict.lastSpanUs);
    lastSample.publishedValid.store(verdict.lastSampleValid);
    lastSample.valid.store(oracleSample.sampleValid);
    menuOracleTally.Add(verdict);
    if (oracleSample.sampleValid &&
        (NameEqualsExact(oracleOrigin, "menu_open") ||
         NameEqualsExact(oracleOrigin, "observation")) &&
        oracleSample.targetBase &&
        MeasuredPairKnown(MeasuredPair{oracleSample.playerBaseCount,
                                       oracleSample.vendorBaseCount,
                                       oracleSample.playerGold,
                                       oracleSample.merchantContainerGold})) {
        cachedBeforeSample = oracleSample;
        cachedBeforeSource = NameEqualsExact(oracleOrigin, "menu_open")
            ? "cached_menu_open" : "cached_observation";
        cachedBeforeContainerGeneration = inventoryGeneration.load();
    }
    auto row = NewRow("js_retired", oracleOrigin, Phase::kUiTask);
    row.reason = "OK";
    row.oracleRequestId = oracleSample.sampleId;
    row.oracle = oracleSample;
    row.perf.entryVisitsTotal = entryVisits.load();
    row.perf.xListVisitsTotal = xListVisits.load();
    row.perf.oracleVisitsTotal = oracleEntryVisits.load();
    row.perf.visitAllowanceShort = visitAllowanceShort.load();
    row.perf.oracleSessionVisits = oracleSessionVisits.load();
    row.perf.visitCapReached = oracleSample.visitCapReached;
    row.barterOpen = barterOpen.load();
    row.firstEventSeq = oracleSample.firstEventSeq;
    row.lastEventSeq = oracleSample.lastEventSeq;
    if (oracleSample.epoch != epoch.load()) { ++epochStale; row.reason = "EPOCH_STALE"; }
    else if (const auto* reason = OracleReason(row.oracle)) row.reason = reason;
    else if (!oracleSample.sampleValid) row.reason = oracleSample.invalidationReason;
    if (oracleSample.truncated) ++oracleTruncations;
    Log(row);
    const auto published = oracleSample;
    // R23-3(a): claim and start the deferred job on this publish task before
    // attempt handlers can consume another deadline tick.
    const auto queued = oracleRequests.Complete();
    if (queued.valid) StartOracle(queued.origin, queued.sampleId, queued.targetBase);
    CompleteIssueFromSample(published);
    ExpirePendingRequest();
    // S2c: the batch's own two samples (batch_before plans the press,
    // batch_after reconciles it). Every other sample passes by.
    OnBatchSample(published);
}

// w-probe-r1 (design sections 6.3 and 8): the bounded quiet observation. At
// most one is ever outstanding; an armed one whose generation is gone is
// dropped and counted, never run against a window it no longer belongs to.
std::uint64_t RequestOracle(const char* origin, bool* deferred, std::uint64_t* coalescedInto);

void RunObservation() {
    const bool stale = ObservationStale(armedLoadGeneration.load(), loadGeneration.load(),
                                        armedMenuGeneration.load(), menuGeneration.load());
    observationArmed = false;
    if (pendingObservationTasks.load()) --pendingObservationTasks;
    if (stale || !barterOpen.load()) {
        ++staleObservationCount;
        auto row = NewRow("js_warning", "observation", Phase::kUiTask);
        row.reason = stale ? "OBSERVATION_GENERATION_STALE" : "OBSERVATION_MENU_CLOSED";
        row.pendingObservations = pendingObservationTasks.load();
        Log(row);
        return;
    }
    // The sample is asked for first so the selection row can name it.
    const auto requestId = RequestOracle("observation");
    LogSelection(CurrentItemList(), "observation", true, requestId);
}

void ArmObservation() {
    if (!ObservationArmable(pendingObservationTasks.load())) return;
    if (observationArmed.exchange(true)) return;
    ++pendingObservationTasks;
    ++observationRequests;
    armedLoadGeneration = loadGeneration.load();
    armedMenuGeneration = menuGeneration.load();
    stageMask.fetch_or(StageBit(UiStage::kQuietObservation));
}
// An invalidated generation drops the armed request instead of letting it run
// against a window it no longer belongs to.
void DropObservation() {
    stageMask.fetch_and(~StageBit(UiStage::kQuietObservation));
    if (observationArmed.exchange(false)) {
        ++staleObservationCount;
        if (pendingObservationTasks.load()) --pendingObservationTasks;
    }
}

void RunStage() {
    EnterUiTask();
    const auto start = NowNs();
    const auto index = NextStage(stageMask.load());
    if (index < StageCount) {
        stageMask.fetch_and(~(1u << index));
        switch (static_cast<UiStage>(index)) {
        case UiStage::kSelectionOpen:
            LogSelection(CurrentItemList(), "open", true);
            break;
        case UiStage::kQuietObservation: RunObservation(); break;
        case UiStage::kOraclePublish: PublishOracle(); break;
        default:
            if (RunOracleStage(static_cast<UiStage>(index), start))
                stageMask.fetch_or(1u << index);
            break;
        }
    }
    if (!taskMeter.Observe((NowNs() - start) / 1000)) ++budgetOverruns;
}

// The claim is already held by the caller.
std::uint64_t StartOracle(const char* origin, std::uint64_t requestedId,
                          std::uint32_t targetBase) {
    const auto id = requestedId;
    if (NameEqualsExact(origin, "issue_settle") && issueContext.active) {
        issueContext.settleSampleId = id;
    }
    oracleSample = OracleSample{};
    oracleWalk = {};
    oracleStageNotBeforeNs.store(0);
    oracleMerchantWindow = {};
    oracleSample.sampleId = id;
    oracleSample.sampleNs = NowNs();
    oracleSample.epoch = epoch.load();
    oracleSample.attached = true;
    // The opening edge of the sample window (design section 4.2).
    oracleSample.startLoadGeneration = loadGeneration.load();
    oracleSample.startMenuGeneration = menuGeneration.load();
    oracleSample.startInventoryGeneration = inventoryGeneration.load();
    oracleSample.firstEventSeq = lastEventSeqSeen.load();
    // W-3 ruling 3: the jobs that belong to a sale also count the sale's base
    // in the player and vendor walks they already make for gold.
    oracleSample.targetBase = targetBase;
    oracleOrigin = origin;
    stageMask.fetch_or(OracleStageMask);
    PumpStage();
    return id;
}
// Returns the id of the sample that will cover the caller's row. A free channel
// starts immediately; a busy channel coalesces equal targets or remembers a
// different target for its own later sample.
std::uint64_t RequestOracle(const char* origin, bool* deferred, std::uint64_t* coalescedInto) {
    if (deferred) *deferred = false;
    if (coalescedInto) *coalescedInto = 0;
    // w-probe-r12 (R12-2): the next queued slot is never overwritten. In r11
    // the next deferred request stored over it, so the id a settle had already
    // been handed (R-W7 sample 8) was never started. Same-target requests are
    // coalesced with the scheduled sample; different targets wait for their
    // own sample, so every id a caller holds publishes the right target.
    std::uint32_t targetBase = 0;
    if (NameEqualsExact(origin, "issue_before") && pendingRequest.valid) {
        targetBase = pendingRequest.baseFormID;
    } else if (NameEqualsExact(origin, "issue_settle") && issueContext.active) {
        targetBase = issueContext.baseObj;
    } else if (NameEqualsExact(origin, "menu_open") || NameEqualsExact(origin, "observation")) {
        targetBase = CaptureSelectedBase();
    }
    const auto decision = oracleRequests.Request(origin, OracleRequestKindOf(origin), targetBase);
    // The queue gives every caller an id while holding its serialized state
    // transition. A free channel starts exactly that id here.
    if (decision.started) return StartOracle(origin, decision.sampleId, targetBase);
    ++oracleDeferred;
    if (deferred) *deferred = true;
    if (decision.coalescedInto) {
        if (coalescedInto) *coalescedInto = decision.coalescedInto;
        return decision.sampleId;
    }
    return decision.sampleId;
}

void OnBarterOpen() {
    EnterUiTask();
    menuOracleTally.Reset(menuGeneration.load());
    barterMenuOpenCache.store(true);
    // w-probe-r27 (R28-1, design ERROR_LATCH row): the new session's
    // explicit latch reset, and its receipt. latchCleared says whether a
    // latch was set before the clear (design: the latch is re-verified and
    // cleared at the next BarterMenu open, never silently). queuedRunDropped
    // says whether a BeginRuleRun task from the previous menu generation is
    // still queued; that task then refuses on its own generation check
    // (run_invalidated), so no stale plan is ever read in the new session.
    const bool latchCleared = issueLatched.exchange(false);
    const bool queuedRunDropped = js::SchedulerSlotQueued(ruleRunSlot.load());
    issueOracleMismatchLatched.store(false);
    const auto start = NowNs();
    // The menu generation was advanced by the sink, at the event itself; this
    // task only reads the lifetime it belongs to.
    auto row = NewRow("js_retired", "menu_open", Phase::kMenuOpen);
    row.latchCleared = latchCleared;
    row.queuedRunDropped = queuedRunDropped;
    const auto sessionNumber = sessions.fetch_add(1);
    const auto capAction = js::SessionCapAction(sessionNumber, MaxSessions);
    if (!capAction.logRow) {
        ++truncated;
        row.reason = "SESSION_CAP";
        if (!sessionCapWarning.exchange(true)) Warning("SESSION_CAP", "session_cap");
    }
    auto* ui = RE::UI::GetSingleton();
    auto menu = ui ? ui->GetMenu<RE::BarterMenu>() : nullptr;
    if (!menu) {
        PublishAdapterVerdict(false);
        RefreshEffectiveState();
        row.reason = "NO_MENU";
        if (capAction.logRow) Log(row);
        return;
    }
    auto* raw = menu.get();
    PublishAdapterVerdict(raw->fxDelegate.get() != nullptr && raw->uiMovie != nullptr);
    RefreshEffectiveState();
    auto& data = raw->GetRuntimeData();
    activeItemList.store(reinterpret_cast<std::uintptr_t>(data.itemList));
    row.itemCount = data.itemList ? static_cast<std::int32_t>(data.itemList->items.size()) : -1;
    row.reason = "OK";
    row.barterOpen = true;
    // The start of this session's lifetime, as an event sequence boundary.
    row.firstEventSeq = lastEventSeqSeen.load();
    // r5: the balances are no longer read inside this task. The row names the
    // sample the stage queue is about to build and is written without waiting
    // for it.
    row.oracleRequestId = RequestOracle("menu_open");
    row.elapsedUs = (NowNs() - start) / 1000;
    if (!taskMeter.Observe(row.elapsedUs)) { ++budgetOverruns; row.reason = "BUDGET"; }
    if (capAction.logRow) Log(row);
    // Selection and the three inventory stages each run as their own bounded UI task.
    stageMask.fetch_or(StageBit(UiStage::kSelectionOpen));
    // The session's opening quiet observation: the "before" side of the
    // stable pair the W stage needs (design section 6.3).
    ArmObservation();
    PumpStage();
}

void OnBarterClose() {
    EnterUiTask();
    barterMenuOpenCache.store(false);
    PublishAdapterVerdict(false);
    RefreshEffectiveState();
    const auto start = NowNs();
    if (issueContext.active && issueInFlight.load()) FinalizeIssue(SaleTerminal::kMenuClosed, "MENU_CLOSED");
    if (pendingRequest.valid) {
        const auto request = pendingRequest;
        pendingRequest = {};
        DropRequest(request, SaleTerminal::kRequestMismatch, "MENU_CLOSED", RequestAgeMs(request, NowMs()));
    }
    // design hand review (w27 A-01): the rule run planned in this session ends
    // with it, before anything of the next session can be read or requested.
    CancelRuleRun("menu_closed");
    selectionFacts = {};
    issueInFlight.store(false);
    // w-probe-r25 (loop2 B-01): the menu close is a terminal path that does
    // not go through FinalizeIssue when no context is active, so the retained
    // token is cleared here too. No event of the next session may be stamped
    // with a token from this one.
    issuedToken.store(0);
    issueLatched.store(false);
    issueOracleMismatchLatched.store(false);
    issueMachine.ClearMenuLatch();
    auto row = NewRow("js_retired", "menu_close", Phase::kMenuClose);
    row.reason = "OK";
    row.nameCounters = true;
    row.oracleRequestId = RequestOracle("menu_close", &row.oracleDeferredRequest);
    // The close invalidates this session's lifetime. What it left behind is
    // stated on this row instead of being inferred from the absence of rows.
    invalidationMs = NowMs();
    idleReported = false;
    previousItemSelectHandler = currentItemSelectHandler.exchange(0);
    DropObservation();
    row.pendingObservations = pendingObservationTasks.load();
    row.elapsedUs = (NowNs() - start) / 1000;
    if (!taskMeter.Observe(row.elapsedUs)) { ++budgetOverruns; row.reason = "BUDGET"; }
    Log(row);
    activeItemList = 0;
    lastSelectedBase = 0; lastSelectedIndex = -1;
    // The close row's values belong to the session that ended: a later reader
    // must not take them for the state of the next one.
    lastSample.valid.store(false);
    // The selection stage belongs to the session that just ended. The oracle
    // stages do not: the close snapshot is still being built.
    stageMask.fetch_and(~StageBit(UiStage::kSelectionOpen));
}


void HandleIssueEvent(const EventRecord& record) {
    if (!issueContext.active || !issueInFlight.load() || record.kind != EventRecord::Kind::kContainer ||
        record.seq <= issueContext.startEventSeq) return;
    // w-probe-r24 fix round 2 (A-07): an event captured with no active
    // attempt (token 0) or with a different attempt's token can never be
    // attributed to this attempt, even if its shape matches. Checked before
    // any other classification so a foreign same-shaped transfer cannot be
    // mistaken for this attempt's own pair.
    if (record.attemptToken == 0 || record.attemptToken != issueContext.token) {
        issueMachine.ForeignEvent();
        return;
    }
    // w-probe-r24 fix round 3 (A-03): an event whose own capture stamp
    // landed after AttemptDeadlineMs is never attributed to this attempt's
    // pair, even though it carries this attempt's own token -- otherwise a
    // pair delivered after the deadline but drained before the next Tick's
    // CheckIssueTimeout could still confirm. EventCaptureLate (WIssue.h) is
    // the same host-testable arithmetic AttemptDeadlineReached already uses.
    if (js::EventCaptureLate(issueContext.startNs / 1000000, record.ns / 1000000, AttemptDeadlineMs)) {
        issueMachine.LateEvent();
        issueContext.lateEvents = issueMachine.state.lateEvents;
        return;
    }
    // W-3 ruling 2: every container event after the dispatch is recorded and
    // examined; CompleteIssueFromSample compares this with the sink's count.
    ++issueContext.handledEvents;
    if (record.epoch != issueContext.startLoadGeneration ||
        record.menuGeneration != issueContext.startMenuGeneration || !record.barterOpen) {
        issueMachine.ForeignEvent();
        return;
    }
    const bool item = record.oldContainer == issueContext.playerID &&
                      record.newContainer == issueContext.vencID &&
                      record.baseObj == issueContext.baseObj &&
                      record.itemCount == issueContext.quantity;
    const bool gold = record.oldContainer == issueContext.vencID &&
                      record.newContainer == issueContext.playerID &&
                      record.baseObj == GoldFormID &&
                      record.itemCount == issueContext.quantity * issueContext.unitPrice;
    // A container event that is neither expected move is foreign: the
    // measured pairs can no longer be attributed to this sale.
    if (!item && !gold) {
        issueMachine.ForeignEvent();
        return;
    }
    const auto result = issueMachine.Event(item, gold, true, IssueGeneration());
    if (result != SaleTerminal::kIssued) {
        return;
    }
    if (item) {
        issueContext.itemEvent = true;
        issueContext.itemEventSeq = record.seq;
        issueContext.itemEventCount = record.itemCount;
    } else {
        issueContext.goldEvent = true;
        issueContext.goldEventSeq = record.seq;
        issueContext.goldEventCount = record.itemCount;
        issueContext.goldOldContainer = record.oldContainer;
        issueContext.goldNewContainer = record.newContainer;
        issueContext.goldBaseObj = record.baseObj;
    }
    // w-probe-r24 fix round 2 (A-05/A-06): the r24 early-confirm-on-pair
    // block was removed. Confirming here, off the deadline, accepted a pair
    // delivered after AttemptDeadlineMs but before the next Tick's
    // CheckIssueTimeout ran, and it also finalized (and thereby deactivated)
    // the attempt on the first matching pair, so a later extra/duplicate/
    // foreign event in the same drained batch hit the inactive early return
    // above instead of being classified. The attempt now stays inFlight
    // (EventPairComplete() below is diagnostic only, unused as a gate) until
    // CheckIssueTimeout runs at or after the deadline and calls
    // issueMachine.Finish(), which is the sole confirmation site; every
    // event in between -- including a same-batch extra one -- is still
    // examined by the checks above (identity, generation, duplicate,
    // foreign) and can still invalidate the attempt.
}

void DrainEvents(std::uint64_t deadline, bool drainQueued = false) {
    std::array<EventRecord, DrainBatch> records{};
    unsigned count = 0;
    while (count < records.size() && (drainQueued || !DeadlineReached(NowNs(), deadline)) &&
           eventRing.Pop(records[count])) ++count;
    if (!count) return;
    const auto current = epoch.load();
    unsigned fresh = 0, freshInMenu = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (ClassifyRecord(records[i].epoch, current).stale) continue;
        ++fresh;
        if (records[i].barterOpen) ++freshInMenu;
    }
    // One sample per batch. Every fresh row says which batch it belongs to, so a
    // reader never mistakes a shared post-hoc reading for a per-event value, and
    // a record from a previous generation gets no balance at all (B-04).
    // r5: the snapshot is requested, not taken here, and it is only requested
    // while the barter menu is open - outside it there is no merchant to
    // resolve and a stat event from ordinary play must not start a job.
    std::uint64_t requestId = 0;
    if (fresh && barterOpen.load()) requestId = RequestOracle("sink_batch");
    // The event sequence window this batch covers, so a reader never has to
    // reconstruct it from the file order (design section 4.1).
    const auto batchFirstSeq = count ? records[0].seq : 0;
    const auto batchLastSeq = count ? records[count - 1].seq : 0;
    unsigned freshIndex = 0;
    for (unsigned i = 0; i < count; ++i) {
        const auto& record = records[i];
        HandleIssueEvent(record);
        if (record.kind == EventRecord::Kind::kContainer && issueContext.active &&
            record.attemptToken == issueContext.token) {
            issueContext.mainTaskGapMs = MainTaskGapMs(record.mainTaskBeforeMs, NowMs());
        }
        const auto decision = ClassifyRecord(record.epoch, current);
        if (decision.stale) ++epochStale;
        auto row = NewRow("js_retired", decision.stale ? "stale" : "sink");
        row.seq = record.seq;
        row.monotonicNs = record.ns;
        row.epoch = record.epoch;
        row.thread = record.thread;
        row.reason = decision.reason;
        row.oldContainer = record.oldContainer;
        row.newContainer = record.newContainer;
        row.baseObj = record.baseObj;
        row.referenceID = record.referenceID;
        row.uniqueID = record.uniqueID;
        row.itemCount = record.itemCount;
        row.statValue = record.statValue;
        row.text = record.text;
        row.barterOpen = record.barterOpen;
        // w-probe-r24 fix round 2 (A-07): the attempt token active when this
        // native event was captured (0 = none), so the parser can validate
        // the same exact-token-match requirement HandleIssueEvent enforces
        // natively, instead of accepting a missing/zero token as a match.
        row.attemptToken = record.attemptToken;
        row.firstEventSeq = batchFirstSeq;
        row.lastEventSeq = batchLastSeq;
        if (decision.attachOracle) {
            row.oracleRequestId = requestId;
            row.batchIndex = freshIndex++;
            row.batchSize = fresh;
        }
        Log(row);
    }
    // r5: only an event that arrived inside the menu arms the settled snapshot.
    // The stat sink now sees ordinary play as well, and a quiet period after a
    // stat that has nothing to do with a sale is not a transaction boundary.
    if (fresh) lastEventMs = NowMs();
    if (freshInMenu) settlePending = true;
    if (eventRing.available.load()) ++continuations;
}

// At an attempt deadline, the bounded normal drain may have left event rows
// behind its batch limit. Snapshot the queued count and consume that complete
// snapshot before terminal classification, so an extra or late row cannot
// confirm this attempt or be attributed to the next one.
void DrainAttemptEventsThroughDeadline() {
    const auto queuedAtDeadline = eventRing.available.load();
    for (std::uint64_t drained = 0; drained < queuedAtDeadline; drained += DrainBatch) {
        DrainEvents(0, true);
    }
}

void Settle() {
    settlePending = false;
    const bool issueSettle = issueContext.active && issueInFlight.load() &&
                             issueContext.itemEvent && issueContext.goldEvent;
    auto row = NewRow("js_retired", issueSettle ? "issue_settle" : "quiet", Phase::kMainTask);
    row.reason = "SETTLED";
    row.barterOpen = barterOpen.load();
    row.lastEventSeq = lastEventSeqSeen.load();
    // The settled snapshot is the one O-3 reads. It is requested here and
    // published on its own js_oracle row with this id.
    if (barterOpen.load()) {
        row.oracleRequestId = RequestOracle(issueSettle ? "issue_settle" : "quiet",
                                            &row.oracleDeferredRequest, &row.coalescedInto);
        if (issueSettle) {
            issueContext.afterPending = true;
            issueContext.settleSampleId = row.oracleRequestId;
            // R12-1: the deadline the attempt is judged against starts here.
            issueContext.settleRequestMs = NowMs();
            issueContext.settleDeferred = row.oracleDeferredRequest;
            issueContext.settleCoalescedInto = row.coalescedInto;
            issueContext.settleSampleState = SettleSampleStateName(SettleSampleState::kPending);
            row.settleSampleId = row.oracleRequestId;
            row.settleSampleState = issueContext.settleSampleState;
        }
    } else if (issueSettle) {
        FinalizeIssue(SaleTerminal::kMenuClosed, "MENU_CLOSED");
    }
    row.pendingObservations = pendingObservationTasks.load();
    Log(row);
    // The quiet point is where the "after" side of a stable pair belongs, so
    // the bounded observation is armed here as well (design section 6.3).
    if (barterOpen.load()) { ArmObservation(); PumpStage(); }
}

// w-probe-r1 (design section 4.2): the measured time from a generation
// invalidation to the moment nothing of the probe is outstanding any more.
// Stated once per invalidation, so "the old observation task never restarted"
// is a reading and not an absence of rows.
void ReportIdle() {
    if (idleReported.load()) return;
    if (stageMask.load() || observationArmed.load() || oracleRequests.Busy()) return;
    if (eventRing.available.load() || settlePending.load()) return;
    if (idleReported.exchange(true)) return;
    const auto since = invalidationMs.load();
    auto row = NewRow("js_retired", "idle");
    row.reason = "OBSERVATION_IDLE";
    row.pendingObservations = pendingObservationTasks.load();
    row.msSinceInvalidationAtReport = since && NowMs() >= since ? NowMs() - since : 0;
    Log(row);
}

void Tick() {
    EnterTask();
    const auto start = NowNs();
    const auto deadline = DeadlineNs(start, js::TaskBudgetUs);
    DrainEvents(deadline);
    if (!DeadlineReached(NowNs(), deadline) && settlePending.load() &&
        NowMs() - lastEventMs.load() >= SettleMs) Settle();
    CheckIssueTimeout();
    ReportIdle();
    if (!taskMeter.Observe((NowNs() - start) / 1000)) ++budgetOverruns;
    lastMainTaskCompletedMs = NowMs();
}

void UiTick() {
    EnterUiTask();
    const auto start = NowNs();
    // r5 / probe-js04 anomaly 1: the poll clock advances here, on the poll, and
    // not in LogSelection, which only runs when a row is actually written.
    lastPollMs = NowMs();
    LogSelection(CurrentItemList(), "poll", false);
    ExpirePendingRequest();
    if (!taskMeter.Observe((NowNs() - start) / 1000)) ++budgetOverruns;
}

void PumpStage() {
    if (!js::StageRunnableNow(stageMask.load(), oracleStageNotBeforeNs.load(), NowNs())) return;
    auto* tasks = SKSE::GetTaskInterface();
    if (!tasks) { ++queueFailures; return; }
    if (stageQueued.exchange(true)) return;
    try { tasks->AddUITask([] {
        try { RunStage(); } catch (...) { Warning("UI_STAGE_EXCEPTION"); }
        stageQueued = false;
        PumpStage();
    }); } catch (...) { stageQueued = false; ++queueFailures; }
}

void Pump() {
    auto* tasks = SKSE::GetTaskInterface();
    if (!tasks) { ++queueFailures; return; }
    const bool work = eventRing.available.load() || settlePending.load() ||
                      (issueContext.active && issueInFlight.load());
    if (work && !taskQueued.exchange(true)) {
        try { tasks->AddTask([] {
            try { Tick(); } catch (...) { Warning("TASK_EXCEPTION"); }
            taskQueued = false;
            // Anything the budget left behind is picked up by one continuation.
            Pump();
        }); } catch (...) { taskQueued = false; ++queueFailures; }
    }
    if (barterOpen.load() &&
        PollDue(NowMs(), lastPollMs.load(), SelectionPeriodMs) && !uiQueued.exchange(true)) {
        try { tasks->AddUITask([] {
            try { UiTick(); } catch (...) { Warning("UI_TASK_EXCEPTION"); }
            uiQueued = false;
        }); } catch (...) { uiQueued = false; ++queueFailures; }
    }
    PumpStage();
    if (ruleRunSlot.load(std::memory_order_acquire) == js::SchedulerSlot::kActive &&
        ruleReading.load(std::memory_order_acquire) &&
        NowNs() >= ruleReadNotBeforeNs.load(std::memory_order_acquire) &&
        !ruleReadQueued.exchange(true)) {
        const auto queuedToken = ruleRunTokenCounter.load();
        try { tasks->AddUITask([queuedToken] {
            try { if (ruleRun.active && ruleRun.runToken == queuedToken) ReadRuleChunk(); } catch (...) {
                CancelRuleRun("error"); Warning("RULE_READ_EXCEPTION");
            }
            ruleReadQueued.store(false);
        }); } catch (...) { ruleReadQueued.store(false); ++queueFailures; }
    }
    // S2c: at most one batch task outstanding. The next commit task
    // starts on a later pump frame once the spill gap passed; the deferred
    // decision runs every frame; a before/after sample that never arrives is
    // bounded by the batch watchdog.
    const auto phase = batchPhase.load(std::memory_order_acquire);
    const auto edge = batchEdgeUs.load(std::memory_order_acquire);
    const bool batchDue =
        (phase == BatchPhase::kCommit && NowNs() >= batchNotBeforeNs.load(std::memory_order_acquire)) ||
        phase == BatchPhase::kDeferred ||
        ((phase == BatchPhase::kBefore || phase == BatchPhase::kAfter) && edge &&
         BatchNowUs() - edge >= js::BatchWatchdogUs);
    if (batchDue && !batchTaskQueued.exchange(true)) {
        const auto queuedToken = ruleRunTokenCounter.load();
        try { tasks->AddUITask([queuedToken] {
            try { if (ruleRun.active && ruleRun.runToken == queuedToken) ServiceBatchTask(); } catch (...) {
                CancelRuleRun("error"); Warning("BATCH_TASK_EXCEPTION");
            }
            batchTaskQueued.store(false);
        }); } catch (...) { batchTaskQueued.store(false); ++queueFailures; }
    }
    // v0.5.0: the observe-only frame window keeps one tick task queued
    // (dev key only): SKSE drains its queue once per frame; the intervals are
    // task-sampling intervals, not frame durations (FrameWindow.h).
    if (frameArmed.load(std::memory_order_acquire) && !frameTickQueued.exchange(true)) {
        try { tasks->AddTask([] {
            try { RunFrameTick(); } catch (...) {}
            frameTickQueued.store(false);
        }); } catch (...) { frameTickQueued.store(false); ++queueFailures; }
    }
    // v0.1.0 (design r3 4.1): at most one sell-anywhere slice task
    // outstanding; the next slice starts on a later pump tick once the slice
    // gap passed, so a slice never shares a frame with the one before it.
    if (convertPhase.load(std::memory_order_acquire) == ConvertPhase::kCommit &&
        NowNs() >= convertNotBeforeNs.load(std::memory_order_acquire) &&
        !convertTaskQueued.exchange(true)) {
        const auto queuedToken = convertToken.load(std::memory_order_acquire);
        try { tasks->AddTask([queuedToken] {
            try { RunConvertSliceTask(queuedToken); } catch (...) {
                Warning("CONVERT_TASK_EXCEPTION");
                try { if (convertRun.active && convertRun.runToken == queuedToken) FinishConvertRun("ERROR"); }
                catch (...) {}
            }
            convertTaskQueued.store(false);
        }); } catch (...) { convertTaskQueued.store(false); ++queueFailures; }
    }
}

class Sinks final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>,
                    public RE::BSTEventSink<RE::TESContainerChangedEvent>,
                    public RE::BSTEventSink<RE::TESTrackedStatsEvent> {
public:
    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event,
                                          RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override {
        if (!latch.Armed() || !sinksLive.Armed()) return RE::BSEventNotifyControl::kContinue;
        const auto started = NowNs();
        // v0.5.0: observe-only (dev key; no effect when no window is armed).
        if (event && frameArmed.load(std::memory_order_relaxed)) {
            NoteFrameNamed(event->opening ? js::FrameEventKind::kMenuOpen : js::FrameEventKind::kMenuClose,
                           event->menuName.c_str());
        }
        if (event && event->menuName == RE::BarterMenu::MENU_NAME) {
            barterOpen.store(event->opening);
            // w-probe-r1: the menu generation advances at the event, not in the
            // task that follows it. A callback that arrives before the open
            // task runs still belongs to the session that just started.
            if (event->opening) ++menuGeneration;
            auto* tasks = SKSE::GetTaskInterface();
            if (!tasks) ++queueFailures;
            // r3: the menu tasks are guarded like every other queued task. The
            // guard covers C++ exceptions only, which is why the crash fix is
            // the walk in Oracle.h and not this try block.
            else if (event->opening) {
                tasks->AddUITask([] {
                    try { OnBarterOpen(); } catch (...) { Warning("BARTER_OPEN_EXCEPTION"); }
                });
            } else {
                tasks->AddUITask([] {
                    try { OnBarterClose(); } catch (...) { Warning("BARTER_CLOSE_EXCEPTION"); }
                });
            }
        }
        if (!sinkMeter.Observe((NowNs() - started) / 1000)) ++budgetOverruns;
        return RE::BSEventNotifyControl::kContinue;
    }
    RE::BSEventNotifyControl ProcessEvent(const RE::TESContainerChangedEvent* event,
                                          RE::BSTEventSource<RE::TESContainerChangedEvent>*) override {
        if (!latch.Armed() || !sinksLive.Armed()) return RE::BSEventNotifyControl::kContinue;
        const auto started = NowNs();
        // v0.5.0: observe-only (dev key): the player's container changes
        // outside the barter menu, during and after a press.
        if (event && !barterOpen.load() && devFrameWindow.load(std::memory_order_relaxed)) NoteFrameContainer(*event);
        // v0.5.1: the container events of a planned press (receipt only).
        if (event && !barterOpen.load() && convertCounting.load(std::memory_order_relaxed)) {
            CountConvertContainer(*event);
        }
        // r4: the event itself is the O-2 material and is recorded whenever the
        // probe is armed. The Oracle channel gates the balance sample attached to
        // it (DrainEvents), not whether the event is seen at all.
        if (event && barterOpen.load()) {
            // audit B-02: the generation advances on the event itself. A change
            // the row cap dropped still changed the inventory, so a sample that
            // spans it must not pass for a stable window.
            ++inventoryGeneration;
            // W-3 ruling 2: counted before the cap, so a sale window with an
            // event the cap dropped can never read as the exact event set.
            ++containerEventTotal;
            // w-probe-r24 fix round 5 (A-01): a container event the row cap
            // drops is never recorded and never reaches HandleIssueEvent, so
            // an attempt in flight has lost it. Counted here, at the drop
            // site, because nothing downstream can see this event again.
            // S2c: while a batch window is open its events belong to the
            // batch capture (in-call or late) and never reach the ring, so a
            // batch sale starts no sink_batch sample and no settle.
            if (batchCaptureOpen.load(std::memory_order_acquire) && CaptureBatchEvent(*event)) {
                lastEventMs = NowMs();
            }
            else if (containerRows.fetch_add(1) >= MaxContainerRows) {
                ++truncated;
                if (issueInFlight.load()) ++attemptEventsLost;
            }
            else {
                EventRecord record;
                record.kind = EventRecord::Kind::kContainer;
                record.seq = NextSeq(); record.ns = started; record.epoch = epoch.load();
                record.menuGeneration = menuGeneration.load();
                record.mainTaskBeforeMs = lastMainTaskCompletedMs.load();
                record.thread = CurrentThread();
                record.barterOpen = true;
                record.oldContainer = event->oldContainer;
                record.newContainer = event->newContainer;
                record.baseObj = event->baseObj;
                record.itemCount = event->itemCount;
                record.uniqueID = event->uniqueID;
                record.referenceID = event->reference.native_handle();
                // w-probe-r24 fix round 2 (A-07): issuedToken is set once per
                // successful dispatch and held unchanged until the next one,
                // so reading it here stamps the token of whichever attempt
                // (if any) is currently active at capture time. 0 means no
                // attempt has ever been dispatched yet in this session.
                record.attemptToken = issuedToken.load();
                // w-probe-r24 fix round 5 (A-01): a record the ring refuses is
                // lost for good; while an attempt is in flight that is one of
                // its own events, whatever kind it was.
                if (!eventRing.Push(record)) {
                    ++ringDrops;
                    ++eventRingDrops;
                    if (issueInFlight.load()) ++attemptEventsLost;
                }
                lastEventSeqSeen = record.seq;
                lastEventMs = NowMs();
                Pump();
            }
        }
        if (!sinkMeter.Observe((NowNs() - started) / 1000)) ++budgetOverruns;
        return RE::BSEventNotifyControl::kContinue;
    }
    RE::BSEventNotifyControl ProcessEvent(const RE::TESTrackedStatsEvent* event,
                                          RE::BSTEventSource<RE::TESTrackedStatsEvent>*) override {
        if (!latch.Armed() || !sinksLive.Armed()) return RE::BSEventNotifyControl::kContinue;
        const auto started = NowNs();
        // v0.5.0: observe-only (dev key; no effect when no window is armed).
        if (event && frameArmed.load(std::memory_order_relaxed)) NoteFrameNamed(js::FrameEventKind::kStat, event->stat.c_str());
        // r5 / probe-js05 anomaly 5: the barterOpen gate is gone. js_stat was
        // empty for a whole run of three sales, and with the gate in place
        // "selling raises no tracked stat" could not be told apart from "the
        // stat arrived a moment after the menu closed". Every name is counted
        // whether or not a row survives the cap, so O-4 is answerable from the
        // HEALTH table alone; each row states the menu state it arrived in.
        if (event) {
            statByName.Add(event->stat.c_str());
            if (statRows.fetch_add(1) >= MaxStatRows) { ++truncated; }
            else {
                EventRecord record;
                record.kind = EventRecord::Kind::kStat;
                record.seq = NextSeq(); record.ns = started; record.epoch = epoch.load();
                record.menuGeneration = menuGeneration.load();
                record.thread = CurrentThread();
                record.barterOpen = barterOpen.load();
                // The stat name is never filtered: discovering it is the point (J31).
                CopyText(record.text, event->stat.c_str());
                record.statValue = event->value;
                // w-probe-r24 fix round 5 (A-01): a record the ring refuses is
                // lost for good; while an attempt is in flight that is one of
                // its own events, whatever kind it was.
                if (!eventRing.Push(record)) {
                    ++ringDrops;
                    ++eventRingDrops;
                    if (issueInFlight.load()) ++attemptEventsLost;
                }
                lastEventSeqSeen = record.seq;
                lastEventMs = NowMs();
                Pump();
            }
        }
        if (!sinkMeter.Observe((NowNs() - started) / 1000)) ++budgetOverruns;
        return RE::BSEventNotifyControl::kContinue;
    }
} sinks;

// Input handling only recognizes a keyboard down edge and queues work. It
// neither reads menus nor dispatches from the sink.
class ProbeInputSink final : public RE::BSTEventSink<RE::InputEvent*> {
public:
    RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* events,
                                          RE::BSTEventSource<RE::InputEvent*>*) override {
        if (!latch.Armed() || !sinksLive.Armed()) return RE::BSEventNotifyControl::kContinue;
        for (auto* event = events ? *events : nullptr; event; event = event->next) {
            const auto* button = event->AsButtonEvent();
            if (!button || event->GetDevice() != RE::INPUT_DEVICE::kKeyboard ||
                !button->IsDown() || button->IsRepeating()) continue;
            const js::Settings productSettings = LiveSettingsCopy();
            if (productSettings.hotkey == 0 ||
                button->GetIDCode() != static_cast<std::uint32_t>(productSettings.hotkey)) continue;
            auto* tasks = SKSE::GetTaskInterface();
            // The MCM Hotkey is the sole input and enters the proven D-W07
            // scheduler below; no probe capture or binding key can queue work.
            // w-probe-r27 (R28-1, review fix item 1): pending <= 1. At most
            // one BeginRuleRun task may be queued at a time; an edge that
            // arrives while a task is queued, while a run is active or while
            // an attempt is in flight is dropped here and counted in
            // edgesDropped (never queued, never a reserved next run). The
            // sink reads no game object and no UI-task-owned object: the
            // claim is the atomic compare_exchange below (fix round 2, audit
            // A-001), and the in-flight bit is its own atomic.
            if (!tasks) { ++queueFailures; break; }
            // S2c: the key edge, the start of this press's batch watchdog.
            const auto edgeUs = BatchNowUs();
            js::SchedulerSlot idle = js::SchedulerSlot::kIdle;
            if (issueInFlight.load() ||
                !ruleRunSlot.compare_exchange_strong(idle, js::SchedulerSlot::kQueued)) {
                ++edgesDropped;
                // design-r2 2.10: a press inside the BarterMenu while a run is
                // live still gets its one line ("a sale is already running"),
                // at most one queued at a time; outside the menu nothing.
                if (barterOpen.load() && !runningLineQueued.exchange(true)) {
                    try {
                        tasks->AddUITask([] {
                            try { EmitRunningLine(); } catch (...) { Warning("RUNNING_LINE_EXCEPTION"); }
                            runningLineQueued.store(false);
                        });
                    } catch (...) { runningLineQueued.store(false); ++queueFailures; }
                } else if (!barterOpen.load() && convertPhase.load(std::memory_order_acquire) != ConvertPhase::kNone &&
                           !runningLineQueued.exchange(true)) {
                    // v0.1.0: a press while a sell-anywhere run is live
                    // gets its one line too (design r3 section 6, RUNNING);
                    // audit A-08: not when a load came in between.
                    const auto runningEpoch = epoch.load();
                    try {
                        tasks->AddUITask([runningEpoch] {
                            try { if (runningEpoch == epoch.load()) EmitConvertRunningLine(); }
                            catch (...) { Warning("RUNNING_LINE_EXCEPTION"); }
                            runningLineQueued.store(false);
                        });
                    } catch (...) { runningLineQueued.store(false); ++queueFailures; }
                }
                break;
            }
            // The generation pair is captured at queue time and compared at
            // entry, so a task left over from the previous menu session
            // refuses with run_invalidated and one left over from before a
            // save start refuses with save_started, instead of reading the
            // following session's list.
            const auto queuedMenuGeneration = menuGeneration.load();
            const auto queuedSaveGeneration = saveGeneration.load();
            // w-probe-r28 (R29-2): the settings generation is captured with
            // the other two, so a Set* between the claim and the entry is a
            // run_invalidated refusal there rather than a stale plan.
            const auto queuedConfigGeneration = configGeneration.load();
            // audit A-05: and the load generation, for a sell-anywhere press.
            const auto queuedLoadGeneration = epoch.load();
            try {
                tasks->AddUITask([queuedMenuGeneration, queuedSaveGeneration, queuedConfigGeneration, edgeUs,
                                  queuedLoadGeneration] {
                    try { BeginRuleRun(queuedMenuGeneration, queuedSaveGeneration, queuedConfigGeneration, edgeUs,
                                       queuedLoadGeneration); }
                    catch (...) {
                        if (ruleRun.active) CancelRuleRun("error");
                        else ReleaseRuleRunSlot();
                        ++queueFailures; Warning("RULE_RUN_EXCEPTION");
                    }
                });
            } catch (...) { ReleaseRuleRunSlot(); ++queueFailures; }
            break;
        }
        return RE::BSEventNotifyControl::kContinue;
    }
} inputSink;

// r4 / probe-js02 anomaly 1. Only Form.SendModEvent produces a ModCallbackEvent
// (see Names.h), so the quest sends through that and this sink finally sees it.
// The name is compared without case because BSFixedString is case insensitive,
// and all three payload values are recorded: the phase string, the heartbeat
// number (a gap means a row was dropped) and the sender FormID.
class PapyrusSink final : public RE::BSTEventSink<SKSE::ModCallbackEvent> {
public:
    RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* event,
                                          RE::BSTEventSource<SKSE::ModCallbackEvent>*) override {
        if (!latch.Armed() || !sinksLive.Armed()) return RE::BSEventNotifyControl::kContinue;
        if (event && PapyrusEventMatches(event->eventName.c_str())) {
            if (papyrusRows.fetch_add(1) >= MaxPapyrusRows) { ++truncated; }
            else {
                auto row = NewRow("js_retired", "quest");
                row.reason = "QUEST_ALIVE";
                row.modEventSource = true;
                CopyText(row.name, event->eventName.c_str());
                CopyText(row.text, event->strArg.c_str());
                row.numArg = event->numArg;
                row.senderFormID = event->sender ? event->sender->GetFormID() : 0;
                Log(row);
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }
} papyrusSink;
}

// C-01: the latch is set only after every sink is registered. A transiently
// unavailable event source is retried on the next lifecycle message, up to three
// attempts, and every attempt is stated in the receipt and in HEALTH.
// w-probe-r71 (v0.9.0, E1 latch rules L2/L3): the five registrations are
// the compound "listener_sinks" (Install.h kSinkSubs) through
// agh::rt::RunCompound with the commit latch sinksLive: a registration that
// throws removes every sink of the compound (newest first), leaves sinksLive
// disarmed and makes the plugin inert with one load_failure row
// (listener_sinks/<sub>); nothing catches a registration. The two optional
// sources keep their v0.8.0 outcome: no input manager = no hotkey sink, no
// ModCallbackEvent source = the NO_MODEVENT_SOURCE warning row.
// RemoveEventSink is a no-op for an absent sink (v9 include/RE/B/BSTEvent.h).
struct SinkOps {
    RE::UI* ui{};
    RE::ScriptEventSourceHolder* holder{};
    RE::BSInputDeviceManager* input{};
    RE::BSTEventSource<SKSE::ModCallbackEvent>* mods{};
    bool Add(std::size_t j) {
        switch (j) {
        case 0: ui->AddEventSink<RE::MenuOpenCloseEvent>(&sinks); return true;
        case 1: holder->AddEventSink<RE::TESContainerChangedEvent>(&sinks); return true;
        case 2: holder->AddEventSink<RE::TESTrackedStatsEvent>(&sinks); return true;
        case 3: if (input) input->AddEventSink(&inputSink); return true;
        case 4: if (mods) mods->AddEventSink(&papyrusSink); return true;
        default: return false;
        }
    }
    void Remove(std::size_t j) noexcept {
        switch (j) {
        case 0: ui->RemoveEventSink<RE::MenuOpenCloseEvent>(&sinks); return;
        case 1: holder->RemoveEventSink<RE::TESContainerChangedEvent>(&sinks); return;
        case 2: holder->RemoveEventSink<RE::TESTrackedStatsEvent>(&sinks); return;
        case 3: if (input) input->RemoveEventSink(&inputSink); return;
        case 4: if (mods) mods->RemoveEventSink(&papyrusSink); return;
        default: return;
        }
    }
};
static_assert(e1::kSinkSubs.size() == 5, "SinkOps binds exactly the five sub-calls of kSinkSubs");

// The late-failure writer (Install.h LateFailure): the shared gate's line
// through the open receipt writer.
void LogLateFailure(const std::string& json) { (void)LogRawLine(json); }

void TryRegister() {
    if (registered.load()) return;
    if (registerAttempts.load() >= MaxRegisterAttempts) return;
    if (registerInFlight.exchange(true)) return;
    EnterTask();
    const auto attempt = ++registerAttempts;
    auto row = NewRow("js_retired", "register");
    row.attempt = static_cast<unsigned>(attempt);
    auto* ui = RE::UI::GetSingleton();
    auto* holder = RE::ScriptEventSourceHolder::GetSingleton();
    if (!ui || !holder) {
        row.reason = attempt >= MaxRegisterAttempts ? "NO_EVENT_SOURCE_GIVEN_UP" : "NO_EVENT_SOURCE_RETRY";
        Log(row);
        registerInFlight = false;
        return;
    }
    // TESSellEvent is deliberately not sinked: its layout is unknown (U07).
    // r4: a missing ModEvent source used to be skipped in silence, which made
    // "the sink was never registered" and "the sender used the wrong mechanism"
    // indistinguishable. The register row now states which one it was.
    SinkOps ops{ ui, holder, RE::BSInputDeviceManager::GetSingleton(), SKSE::GetModCallbackEventSource() };
    const auto outcome = agh::rt::RunCompound(std::span<const agh::rt::SubSpec>{ e1::kSinkSubs }, ops, &sinksLive);
    if (outcome.result != agh::rt::StepResult::kOk) {
        // Every sink of the compound is removed; the plugin goes inert and one
        // load_failure row names listener_sinks/<sub> (design 2.4).
        e1::LateFailure("listener_sinks", outcome, latch, &LogLateFailure);
        registerInFlight = false;
        return;
    }
    if (!ops.mods) Warning("NO_MODEVENT_SOURCE");
    row.modEventSource = ops.mods != nullptr;
    registered = true;
    SetPump(Pump);
    row.reason = "SINKS_REGISTERED";
    Log(row);
    registerInFlight = false;
}

// w-probe-r28 (R29-2): the optional user settings file, the third source of
// the load order (defaults -> the probe ini's two protection keys -> this
// file). Called from LoadSettings on the SKSE load thread, before any row is
// written, so the snapshot it installs is the one every later row echoes.
// A file that is absent leaves the ini-derived settings live. A file that
// exists but does not parse is CONFIG_CORRUPT: the ini-derived values stay
// live, the effective state reads OFF, and the first error is kept for
// GetLastSaveError, so the refusal is never silent.
void EnsureUserSettingsLoaded() {
    std::ifstream input(UserSettingsPath);
    if (!input) return;
    std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) {
        configCorrupt.store(true);
        std::lock_guard<std::mutex> lock(lastRunMutex);
        CopyText(lastSaveError, "read_error");
        return;
    }
    js::Settings candidate;
    {
        std::lock_guard<std::mutex> lock(settingsMutex);
        candidate = liveSettings;
    }
    std::vector<std::string> errors;
    if (!js::ParseSettings(text, candidate, errors)) {
        configCorrupt.store(true);
        const std::string first = errors.empty() ? std::string("parse_error") : errors.front();
        std::lock_guard<std::mutex> lock(lastRunMutex);
        CopyText(lastSaveError, first.c_str());
        return;
    }
    {
        std::lock_guard<std::mutex> lock(settingsMutex);
        liveSettings = std::move(candidate);
    }
    configCorrupt.store(false);
    settingsSourceKind.store(2);
}

bool LoadSettings() {
    // Strict one-section base INI. The product settings live in the user file.
    std::ifstream input("Data/SKSE/Plugins/AGH_JunkSeller.ini");
    if (!input) return false;
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    // v0.5.0: the one developer key (DevFrameWindow, 0 in the shipped file).
    bool devWindow = false;
    // v0.8.0: the second developer key (DevPerkFallback, 0 in the shipped file).
    bool devPerk = false;
    if (input.bad() || !js::ParseBaseIni(text, &devWindow, &devPerk)) return false;
    devFrameWindow.store(devWindow);
    devPerkFallback.store(devPerk);
    CopyText(settings.protectionFacts, "live");
    settingsSourceKind.store(0);
    EnsureUserSettingsLoaded();
    configuredVisitCap = js::VisitCap;
    eventRing.SetLimit(js::RingSize);
    taskMeter.Configure(js::TaskBudgetUs);
    sinkMeter.Configure(SinkBudgetUs);
    hookMeter.Configure(SinkBudgetUs);
    formatMeter.Configure(2000);
    prepareMeter.Configure(js::TaskBudgetUs);
    formScanMeter.Configure(js::TaskBudgetUs);
    xListScanMeter.Configure(js::TaskBudgetUs);
    // The row echo and the first rule run read the cache, never the file.
    RefreshEffectiveState();
    return true;
}

// w-probe-r71 (v0.9.0): reason, runtime and skse come from the shared
// runtime gate's labels (Install.h LabelsFor), never from a literal: on
// 1.6.1170 + SKSE 2.2.6 they are the v0.8.0 bytes ("RUNTIME_1.6.1170_SKSE_2.2.6",
// "1.6.1170.0", "2.2.6.0"); off 1.6.1170 the row also carries the Q4 suffix.
// Row::reason is a pointer: the token lives in this process-lifetime buffer.
std::array<char, 96> startupReason{};

void ConfigureStartupRow(Row& row, const e1::StartupLabels& labels) {
    Configure(row);
    CopyText(startupReason, labels.reason.c_str());
    row.reason = startupReason.data();
    CopyText(row.releaseId, BuildInfo::ReleaseId);
    CopyText(row.runtime, labels.runtime.c_str());
    CopyText(row.skse, labels.skse.c_str());
    CopyText(row.alProof, labels.alProof.c_str());
    CopyText(row.addressLibrary, BuildInfo::AddressLibrary);
    CopyText(row.baseIniSha256, BuildInfo::BaseIniSha256);
    CopyText(row.espSha256, BuildInfo::EspSha256);
    CopyText(row.acceptanceRun, BuildInfo::AcceptanceRun);
    CopyText(row.receiptBasename, ReceiptBasename);
    row.phase = Phase::kPostload;
    row.thread = ThreadKind::kLifecycle;
    row.wObservation.dispatchEligibility = DispatchRejected;
    row.wObservation.missingPrerequisites = kAlwaysMissingPrerequisites;
    StampGenerations(row);
}

// ---------------------------------------------------------------------------
// w-probe-r28 (R29-1/R29-2/R29-3): the runtime half of the AGH_JS_Native
// natives. Declared in PapyrusBridge.h, registered on the SKSE Papyrus
// interface by PapyrusBridge.cpp, defined here next to the one settings mutex.
// Every function in this block runs on the Papyrus VM thread (a native call),
// takes the mutex for exactly one snapshot or one swap, and touches no RE
// object. The row writers below run with the mutex released, because NewRow
// reads the live snapshot itself.
// ---------------------------------------------------------------------------

// The live settings as one copy.
js::Settings LiveSettingsCopy() {
    js::Settings live;
    std::lock_guard<std::mutex> lock(settingsMutex);
    live = liveSettings;
    return live;
}

// The js_settings_apply row of one Set* call. `reason` is one of the
// ApplySetting tokens; saved/readbackOk are the R29-2 file round trip.
void EmitSettingsApplyRow(const char* key, bool accepted, bool saved, bool readbackOk,
                          const char* value, const char* reason) {
    auto row = NewRow("js_settings_apply", "settings");
    row.settingsApplyKey = js::SettingsKeyConstant(key ? std::string_view(key) : std::string_view{});
    row.settingsApplyValue = key && std::strcmp(key, js::SettingsKeyEnabled) == 0 &&
                             value && std::strcmp(value, "1") == 0;
    row.settingsApplyAccepted = accepted;
    row.settingsApplySaved = saved;
    row.settingsApplyReadbackOk = readbackOk;
    row.reason = (reason && reason[0]) ? reason : "none";
    Log(row);
}

// One save failure: the text GetLastSaveError reports plus a named warning row,
// so a save that did not happen is never silent.
void RecordSaveFailure(const char* detail) {
    {
        std::lock_guard<std::mutex> lock(lastRunMutex);
        CopyText(lastSaveError, (detail && detail[0]) ? detail : "save_failed");
    }
    Warning("SETTINGS_SAVE_FAILED");
}

js::SettingSaveOutcome ApplyAndSaveSetting(const char* key, const char* value) {
    js::SettingSaveOutcome outcome;
    const std::string keyText = key ? key : "";
    const std::string valueText = value ? value : "";
    js::Settings intended = LiveSettingsCopy();
    const bool wasEnabled = intended.enabled;
    const auto applied = js::ApplySetting(intended, keyText, valueText);
    outcome.accepted = applied.accepted;
    outcome.reason = applied.reason;
    if (!applied.accepted) {
        // R29-2/RULE-14: a refused key never writes the file and never moves
        // the config generation, so the live snapshot is untouched.
        EmitSettingsApplyRow(keyText.c_str(), false, false, false, valueText.c_str(), applied.reason);
        return outcome;
    }
    // R29-2 save order: serialize -> write "<file>.tmp" beside the target ->
    // rename over the target -> read the target back and parse it. The save is
    // a success only when the readback equals the intended settings; on any
    // failure the previous live settings stay live and the failure is reported.
    // w-probe-r29 (B-002 of a review): the round trip
    // lives in js::SaveSettingsToFile (Settings.h) as an engine-free function
    // of (settings, target path), so the host harness runs these exact
    // branches in a temporary directory; this call site only owns the path and
    // the swap below.
    const std::filesystem::path target = UserSettingsPath;
    const js::SettingsSaveResult save = js::SaveSettingsToFile(intended, target);
    outcome.saved = save.saved;
    outcome.readbackOk = save.readbackOk;
    if (!outcome.saved || !outcome.readbackOk) {
        RecordSaveFailure(save.detail);
        EmitSettingsApplyRow(keyText.c_str(), outcome.accepted, outcome.saved, outcome.readbackOk,
                             valueText.c_str(), outcome.reason);
        return outcome;
    }
    {
        std::lock_guard<std::mutex> lock(settingsMutex);
        liveSettings = intended;
    }
    // R29-2: the generation moves only here, once the readback matched.
    settingsSourceKind.store(2);
    configCorrupt.store(false);
    const auto newGeneration = ++configGeneration;
    RefreshEffectiveState();
    {
        std::lock_guard<std::mutex> lock(lastRunMutex);
        CopyText(lastSaveError, "none");
    }
    const bool enabledOff = keyText == js::SettingsKeyEnabled && wasEnabled && !intended.enabled;
    js::SettingsStopRequest stopRequest{};
    if (enabledOff) {
        stopRequest.applyNs = NowNs();
        stopRequest.configGeneration = newGeneration;
        stopRequest.pendingBefore = js::RuleStopPendingCount(
            ruleRunSlot.load(std::memory_order_acquire), ruleAdvanceQueued.load(),
            issueInFlight.load());
    }
    const bool stopQueued = enabledOff && EnqueueSettingsStopRequest(stopRequest);
    EmitSettingsApplyRow(keyText.c_str(), true, true, true, valueText.c_str(), outcome.reason);
    if (enabledOff) {
        if (stopQueued) QueueSettingsStopConfirmation();
        else Warning("STOP_QUEUE_FULL", "stop_queue_full");
    }
    return outcome;
}

bool ReadSettingBool(const char* key) {
    const js::Settings live = LiveSettingsCopy();
    const std::string_view name = key ? std::string_view(key) : std::string_view{};
    if (name == js::SettingsKeyEnabled) return live.enabled;
    if (name == js::SettingsKeyConfirmBeforeSell) return live.confirmBeforeSell;
    if (name == js::SettingsKeySellIngredients) return live.sellIngredients;
    if (name == js::SettingsKeySellAnywhere) return live.sellAnywhere;
    // v0.2.0: the Categories page (JunkRule reads as "Value only").
    if (name == js::SettingsKeyJunkRule) return live.junkValueOnly;
    if (name == js::SettingsKeySellStolen) return live.sellStolen;
    if (name == js::SettingsKeySellWeakWeapons) return live.sellWeakWeapons;
    if (name == js::SettingsKeySellWeakArmor) return live.sellWeakArmor;
    if (name == js::SettingsKeySellClothing) return live.sellClothing;
    // v0.3.0: weak enchanted loot.
    if (name == js::SettingsKeySellEnchantedLoot) return live.sellEnchantedLoot;
    if (name == js::SettingsKeyKnownEnchantmentsOnly) return live.knownEnchantmentsOnly;
    // v0.8.0: "Price follows perks".
    if (name == js::SettingsKeyPerkPrice) return live.perkPrice;
    // v0.4.0: the keep-limit category switches.
    if (const auto i = js::SettingsKeepSwitchIndex(name); i < js::SettingsKeepCount) return live.keepOn[i];
    return false;
}

// v0.2.0: one "why kept" counter of the last planned press
// (0 at limit, 1 protected, 2 stolen, 3 enchantment unknown, 4 never sold);
// -1 before the first one or for another index.
std::int32_t ReadLastKept(std::int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= lastKept.size()) return -1;
    return lastKept[static_cast<std::size_t>(index)].load();
}

// v0.1.0 (design r3 2.5): the shipped formula's rate, round(100 x m')
// from the live inputs; nothing is stored. It runs on the Papyrus VM thread and
// only reads (the GMSTs, the player's actor values and perks), as the vanilla
// Papyrus getters of the same values do. Since v0.8.0 the MCM shows the
// main-thread preview below instead (perk conditions are evaluated only on the
// main thread); this native stays for API callers and never walks perks.
std::int32_t ReadAnywhereRatePercent() {
    return js::ConvertRatePercent(js::ComputeConvertPrice(ReadShadowPriceInputs()));
}

// v0.8.0 (the barter-price design r2 section 3.4 step 1): one preview
// request. Moves the request sequence (so an older value is never shown) and
// queues one UI task that publishes the rate of a price snapshot taken like a
// press's. False when no task could be queued (the row then stays pending).
void PublishRatePreview(std::uint64_t requestSequence) {
    EnterUiTask();
    if (ratePreviewRequested.load(std::memory_order_acquire) != requestSequence) return;
    const auto priced = ReadPriceSnapshot(LiveSettingsCopy().perkPrice, ReadShadowPriceInputs());
    ratePreviewPercent.store(js::ConvertRatePercent(priced.snapshot.price), std::memory_order_relaxed);
    ratePreviewPublished.store(requestSequence, std::memory_order_release);
}

bool RequestAnywhereRatePreview() {
    const std::uint64_t requestSequence = ratePreviewRequested.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto* tasks = SKSE::GetTaskInterface();
    if (!tasks) return false;
    try {
        tasks->AddUITask([requestSequence] {
            try { PublishRatePreview(requestSequence); }
            catch (...) { Warning("RATE_PREVIEW_EXCEPTION", "convert"); }
        });
        return true;
    } catch (...) {
        ++queueFailures;
        return false;
    }
}

// Section 3.4 step 2: -2 while the preview of the latest request is not
// published, -1 when its price could not be read, else round(100 x m').
std::int32_t ReadAnywhereRatePreview() {
    const std::uint64_t requested = ratePreviewRequested.load(std::memory_order_acquire);
    if (requested == 0 || ratePreviewPublished.load(std::memory_order_acquire) != requested) return -2;
    return ratePreviewPercent.load(std::memory_order_relaxed);
}

// Section 3.4 step 4: js::PriceSource of the last sale (0 perks, 1 formula,
// 2 fallback), -1 before one since the last load.
std::int32_t ReadLastPriceSource() { return lastPriceSource.load(); }

// v0.1.0 (design r3 2.5): "<merchant> / <anywhere>" gold of the last
// in-menu batch since the last load, or the empty string.
std::string ReadLastMerchantCheck() {
    const auto merchant = merchantCheckMerchantGold.load();
    const auto anywhere = merchantCheckAnywhereGold.load();
    if (merchant < 0 || anywhere < 0) return std::string{};
    return std::to_string(merchant) + " / " + std::to_string(anywhere);
}

int ReadSettingInt(const char* key) {
    const js::Settings live = LiveSettingsCopy();
    const std::string_view name = key ? std::string_view(key) : std::string_view{};
    if (name == js::SettingsKeyHotkey) return live.hotkey;
    if (name == js::SettingsKeyMaxValue) return live.maxValue;
    if (name == js::SettingsKeyMaxItems) return live.maxItems;
    if (name == js::SettingsKeyJunkMaxValue) return live.junkMaxValue;
    if (name == js::SettingsKeyEnchantedLootMaxValue) return live.enchantedLootMaxValue;
    // v0.4.0: the keep per kind of the keep-limit categories.
    if (const auto i = js::SettingsKeepLimitIndex(name); i < js::SettingsKeepCount) return live.keepLimit[i];
    return 0;
}

float ReadSettingFloat(const char* key) {
    const js::Settings live = LiveSettingsCopy();
    const std::string_view name = key ? std::string_view(key) : std::string_view{};
    if (name == js::SettingsKeyMinWeight) return live.minWeight;
    if (name == js::SettingsKeyMaxValueWeight) return live.maxValueWeight;
    return 0.0F;
}

std::string ReadSettingText(const char* key) {
    const js::Settings live = LiveSettingsCopy();
    const std::string_view name = key ? std::string_view(key) : std::string_view{};
    if (name == js::SettingsKeyProtectedNames) {
        return js::JoinSettingsList(live.protectedNames, js::SettingsListSeparator);
    }
    if (name == js::SettingsKeyProtectedForms) {
        return js::JoinSettingsList(live.protectedForms, js::SettingsListSeparator);
    }
    return std::string{};
}

// The runtime/version this module loaded under: the pair the shared runtime
// gate passed (Install.h LabelsFor, set at receipt_open before any native is
// bound), the same pair the js_startup row names. w-probe-r71 (v0.9.0):
// built from the labels instead of the v0.8.0 literal; on 1.6.1170 + SKSE 2.2.6
// the text is the same bytes ("<release>; 1.6.1170.0 + SKSE 2.2.6.0; receiptFallback=...").
std::array<char, 64> runtimeLabel{};
std::array<char, 64> skseLabel{};
std::array<char, 256> runtimeNameText[2]{};

void SetRuntimeLabels(const char* runtime, const char* skse) noexcept {
    try {
        CopyText(runtimeLabel, runtime);
        CopyText(skseLabel, skse);
        for (int fallback = 0; fallback < 2; ++fallback) {
            const auto text = e1::RuntimeNameText(BuildInfo::ReleaseId, runtimeLabel.data(), skseLabel.data(), fallback != 0);
            CopyText(runtimeNameText[fallback], text.c_str());
        }
    } catch (...) {}
}

const char* GetRuntimeName() noexcept {
    // Diagnostics reads this native directly. Keep the runtime pair intact and
    // append the receipt destination state for the product warning surface.
    return runtimeNameText[ReceiptFallback() ? 1 : 0].data();
}

std::uint64_t GetConfigGeneration() noexcept { return configGeneration.load(); }

// audit A-06: defined below, next to GetStatusReason.
bool AnywhereReady() noexcept;
bool IsEffectivelyEnabled() noexcept { return effectiveOnCache.load() || AnywhereReady(); }

// v0.1.0: the in-menu adapter is proven only inside a
// BarterMenu, but sell anywhere needs no adapter. With SellAnywhere ON and the
// only failed term the unproven adapter, the product is ready and says so.
bool AnywhereReady() noexcept {
    try {
        const js::Settings live = LiveSettingsCopy();
        return live.enabled && live.sellAnywhere && !apiMismatch.load() &&
               std::strcmp(effectiveReasonCache.load(), js::EffectiveReasonAdapterUnproven) == 0;
    } catch (...) {
        return false;
    }
}

const char* GetStatusReason() noexcept {
    if (AnywhereReady()) return js::EffectiveReasonOk;
    if (std::strcmp(effectiveReasonCache.load(), js::EffectiveReasonAdapterUnproven) == 0 &&
        !barterMenuOpenCache.load()) return "AWAITING_MENU";
    return effectiveReasonCache.load();
}

const char* BuildReleaseId() noexcept { return BuildInfo::ReleaseId; }

std::string ReadLastSaveError() {
    std::lock_guard<std::mutex> lock(lastRunMutex);
    return std::string(lastSaveError.data());
}

std::string ReadLastRunSummary() {
    std::lock_guard<std::mutex> lock(lastRunMutex);
    return std::string(lastRunSummary.data());
}

void NoteApiMismatch() {
    apiMismatch.store(true);
    RefreshEffectiveState();
}

// The process identity every row carries. Called once before the log opens.
void InitProcessRun() {
    const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    SetProcessRun(static_cast<std::uint64_t>(GetCurrentProcessId()),
                  static_cast<std::uint64_t>(wall));
}

void OnMessage(SKSE::MessagingInterface::Message* message) {
    if (!latch.Armed()) return;
    if (!message) return;
    using M = SKSE::MessagingInterface;
    // w-probe-r27 (R28-1, review fix item 3a): a save that starts while a run
    // is live stops it. Design section 5: no new issuance while the save
    // runs, the attempt in flight keeps its own result verification exactly
    // as before, and the queued advance (if any) finds the run inactive and
    // folds nothing. Nothing new is issued from it.
    // fix round 2: the save generation moves first, so a start
    // still queued from before the save refuses at its own entry (it recorded
    // the older generation) and a plan read before the save stops at its next
    // dispatch check. The probe has no save-completion message, so the save
    // start is where the invalidation ends: a run planned after it belongs to
    // the new save generation.
    if (message->type == M::kSaveGame) {
        ++saveGeneration;
        // v0.5.0: observe-only (dev key; no effect when no window is armed).
        NoteFrameNamed(js::FrameEventKind::kSave, "save");
        CancelRuleRun("save_started");
    }
    if (message->type == M::kPreLoadGame) loadInProgress.store(true);
    if (message->type == M::kPreLoadGame || message->type == M::kNewGame) {
        if (issueContext.active && issueInFlight.load()) FinalizeIssue(SaleTerminal::kLoadChanged, "LOAD_CHANGED");
        // design hand review (w27 A-01): same discipline as the issue context
        // above; the epoch check in RuleRunStale covers a queued advance that
        // is already on its way.
        CancelRuleRun("load_changed");
        // v0.1.0 (design r3 2.5, audit A-09): the merchant check lives
        // until a load; cleared after the cancel above, which may publish one
        // last pair from the batch it ends.
        merchantCheckMerchantGold.store(-1);
        merchantCheckAnywhereGold.store(-1);
        // v0.8.0 (section 3.4 step 3): neither the preview nor the last
        // sale's price source crosses a load.
        ratePreviewRequested.fetch_add(1, std::memory_order_acq_rel);
        lastPriceSource.store(-1);
        issueInFlight.store(false);
        // w-probe-r25 (loop2 B-01): the load change is the other terminal that
        // bypasses FinalizeIssue when no context is active; the retained token
        // is cleared with it so no event after the load carries the old one.
        issuedToken.store(0);
        issueLatched.store(true);
        ++epoch;
        barterOpen = false;
        barterMenuOpenCache.store(false);
        PublishAdapterVerdict(false);
        RefreshEffectiveState();
        settlePending = false;
        // The armed observation belongs to the generation that is ending. It is
        // dropped and counted, never carried across the load (design section 8).
        DropObservation();
        invalidationMs = NowMs();
        idleReported = false;
        lastSample.valid.store(false);
        cachedBeforeSample = {};
        cachedBeforeSource = "none";
        cachedBeforeContainerGeneration = 0;
        auto row = NewRow("js_retired", "lifecycle", Phase::kPreload);
        row.reason = "GENERATION_INVALIDATED";
        row.pendingObservations = pendingObservationTasks.load();
        Log(row);
        stageMask = 0;
        // A job whose stages were just dropped can never publish, so the claim
        // is released here and the loss is counted instead of stalling every
        // later snapshot.
        if (oracleRequests.Abort()) ++oracleAborted;
    }
    if (message->type == M::kDataLoaded || message->type == M::kPostLoadGame || message->type == M::kNewGame) {
        RefreshEffectiveState();
        // v0.1.0: the load has finished, and a sell-anywhere latch ends
        // with it ("safety stop, load a save to reset").
        if (message->type == M::kPostLoadGame || message->type == M::kNewGame) {
            loadInProgress.store(false);
            convertLatched.store(false);
            merchantCheckMerchantGold.store(-1);
            merchantCheckAnywhereGold.store(-1);
            ratePreviewRequested.fetch_add(1, std::memory_order_acq_rel);
            lastPriceSource.store(-1);
        }
        if (message->type == M::kPostLoadGame) {
            issueLatched.store(false);
            issueOracleMismatchLatched.store(false);
            auto row = NewRow("js_retired", "lifecycle", Phase::kPostload);
            row.reason = "GENERATION_STARTED";
            Log(row);
        }
        // w-probe-r28 (R29-1): the AGH_JS_Native surface is registered on
        // kDataLoaded, in the same branch that queues the event-sink
        // registration, so a refused probe exposes no surface at all. The
        // natives touch no RE object except this VM registration.
        if (message->type == M::kDataLoaded && !js::RegisterPapyrusBridge()) {
            Warning("PAPYRUS_BRIDGE_REFUSED");
        }
        // S2c: the result-line text tables, read once before any press
        // can exist (design-r2 2.10; no engine translation lookup at runtime).
        if (message->type == M::kDataLoaded) {
            try { LoadResultTextTables(); } catch (...) { Warning("RESULT_TEXT_LOAD_EXCEPTION"); }
            // v0.2.0: the category catalog, once, on this main-thread
            // message, before any press can exist.
            if (!convertCatalogReady.load()) {
                bool built = false;
                try { built = js::BuildConvertCatalog(convertCatalog); } catch (...) { built = false; }
                if (built) {
                    convertCatalogReady.store(true, std::memory_order_release);
                    if (convertCatalog.guardSourcesMissing) Warning("CONVERT_GUARD_SOURCE_ABSENT", "convert");
                } else {
                    convertCatalog = js::ConvertCatalog{};
                    Warning("CONVERT_CATALOG_FAILED", "convert");
                }
            }
        }
        auto* tasks = SKSE::GetTaskInterface();
        if (!tasks) { ++queueFailures; return; }
        try { tasks->AddTask([] { TryRegister(); }); }
        catch (...) { ++queueFailures; }
    }
}
}
