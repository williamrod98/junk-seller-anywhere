#include "receipt/Log.h"
#include "Perf.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <future>
#include <iomanip>
#include <locale>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
namespace js {
std::atomic<std::uint64_t> drops{}, budgetOverruns{}, wrongThreadCount{}, truncated{};
std::atomic<std::uint64_t> callbackTruncated{}, ringDrops{}, queueFailures{}, hookExceptions{}, rotations{};
// w-probe-r27 (R28-1): the input sink's own drop counter (Log.h).
std::atomic<std::uint64_t> edgesDropped{};
// Never incremented anywhere in this probe. It exists so the receipt states the
// zero rather than leaving the reader to trust the prose.
std::atomic<std::uint64_t> mutations{};
// r2 counters: stale-generation records, registration attempts, perf latches,
// truncated oracle samples and budget continuations.
std::atomic<std::uint64_t> epochStale{}, registerAttempts{}, perfLatches{}, oracleTruncations{}, continuations{};
// r4 counters: calls the name filter kept out of the ring, and the thread ids
// the probe actually ran on (0 until the first task, UI task or hook capture).
std::atomic<std::uint64_t> traceExcluded{};
std::atomic<std::uint64_t> taskThreadId{}, uiThreadId{}, hookThreadId{};
// r5 counters: oracle jobs that had to wait for the one in front of them, jobs a
// load abandoned, and every tracked stat name the sink saw (O-4 survives the
// row cap because the names are counted even when a row is not written).
std::atomic<std::uint64_t> oracleDeferred{}, oracleAborted{};
NameCounters traceExcludedByName, truncatedByName, statByName;
// w-probe-r1 counters. dispatchCount is the second counter in this file that
// exists only to state a zero: nothing increments it, and the static gate
// proves that instead of asking the reader to trust the prose.
std::atomic<std::uint64_t> dispatchCount{};
std::atomic<std::uint64_t> originalPassCount{}, manualItemSelectCount{};
std::atomic<std::uint64_t> reentryCount{}, hookCallCount{};
std::atomic<std::uint64_t> observationCount{}, observationRequests{}, staleObservationCount{};
std::atomic<std::uint64_t> argsShapeMatchCount{}, argsRejectedCount{}, argsUnknownCount{};
std::atomic<std::uint64_t> fundsKnownCount{}, fundsInsufficientCount{}, fundsUnknownCount{};
std::atomic<std::uint64_t> stableSampleCount{}, invalidSampleCount{}, sampleCount{};
std::atomic<std::uint64_t> phaseUnknownCount{}, readPhaseUnprovenCount{};
std::atomic<unsigned> pendingObservationTasks{};
// One counter, two names: Probe.cpp binds its "epoch" to loadGeneration so the
// O rows keep the name they have always used and the W rows get the explicit
// one, without a second counter that could drift.
std::atomic<std::uint64_t> loadGeneration{}, menuGeneration{}, inventoryGeneration{};
std::atomic<std::uint64_t> lastEventSeqSeen{};
std::atomic<std::uint64_t> entryVisits{}, xListVisits{}, oracleEntryVisits{}, visitCapHits{}, visitAllowanceShort{};
std::atomic<std::uint64_t> configuredVisitCap{MaxXListVisits};
namespace {
// The process identity every row carries. Written once before the log opens,
// read only afterwards, and formatted here so this file stays Windows free.
std::array<char, 32> processRun{};
struct Writer {
    std::array<Row, LogRows> rows{};
    std::mutex mutex;
    std::size_t head{}, tail{}, count{};
    std::atomic<std::uint64_t> attempted{}, accepted{}, dropped{}, ioFailed{}, written{}, lastTaskMs{};
    std::atomic<void (*)()> pump{};
    // w-probe-r71 (v0.9.0): the shared runtime gate's JSON lines
    // (LogRawLine), drained by the worker before the rows. Fixed storage: the
    // queue never allocates.
    std::array<std::array<char, RawLineBytes>, RawLines> raw{};
    std::size_t rawHead{}, rawTail{}, rawCount{};
    std::atomic<std::uint64_t> rawDropped{};
    bool Pop(Row& row) {
        std::lock_guard lock(mutex);
        if (!count) return false;
        row = rows[tail]; tail = (tail + 1) % rows.size(); --count; return true;
    }
    bool PopRaw(std::array<char, RawLineBytes>& line) {
        std::lock_guard lock(mutex);
        if (!rawCount) return false;
        line = raw[rawTail]; rawTail = (rawTail + 1) % raw.size(); --rawCount; return true;
    }
};
// Process lifetime. No destructor joins a worker under the loader lock.
Writer* writer = new Writer;
std::atomic<bool> receiptFallback{};
std::atomic<bool> receiptReady{};
// w-probe-r14 (R14-2): where the startup note goes. Process lifetime for
// the same reason as the writer: nothing runs a destructor at detach.
std::filesystem::path* diagnosticDirectory = new std::filesystem::path;

void Number(std::ostream& out, double n) {
    if (std::isfinite(n)) out << n; else out << "null";
}
void Quoted(std::ostream& out, const char* text) {
    out << '"';
    for (const char* p = text; p && *p; ++p) {
        const auto byte = static_cast<unsigned char>(*p);
        if (*p == '"' || *p == '\\') out << '\\' << *p;
        else if (byte < 0x20 || byte >= 0x7F) {
            constexpr char hex[] = "0123456789abcdef";
            out << "\\u00" << hex[byte >> 4] << hex[byte & 15];
        } else out << *p;
    }
    out << '"';
}
template <std::size_t N>
void Quoted(std::ostream& out, const std::array<char, N>& text) {
    std::array<char, N> safe = text;
    safe[N - 1] = '\0';
    Quoted(out, safe.data());
}
template <std::size_t N>
void QuotedLimited(std::ostream& out, const std::array<char, N>& text, std::size_t limit) {
    constexpr char hex[] = "0123456789abcdef";
    out << '"';
    std::size_t used = 2; // Both quotes are included in the serialized budget.
    for (std::size_t i = 0; i + 1 < N && text[i]; ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        const std::size_t width = (byte < 0x20 || byte >= 0x7F) ? 6 :
                                  (text[i] == '"' || text[i] == '\\') ? 2 : 1;
        if (used + width > limit) break;
        if (width == 6) out << "\\u00" << hex[byte >> 4] << hex[byte & 15];
        else if (width == 2) out << '\\' << text[i];
        else out << text[i];
        used += width;
    }
    out << '"';
}
// A fixed literal or JSON null. The one field that is null by contract
// (preDispatchQuoteSource) goes through this, so "no proven source" cannot be
// mistaken for the string "none".
void QuotedOrNull(std::ostream& out, const char* text) {
    if (text) Quoted(out, text); else out << "null";
}
const char* ThreadName(ThreadKind kind) {
    switch (kind) {
    case ThreadKind::kMainTask: return "main_task";
    case ThreadKind::kUiTask: return "ui_task";
    case ThreadKind::kLifecycle: return "lifecycle";
    case ThreadKind::kOther: return "other";
    default: return "unknown";
    }
}
// Counts by name as one JSON object. Unnamed overflow is stated beside it so a
// reader never mistakes a capped table for a complete one.
void EmitNames(std::ostream& o, const char* key, const NameCounters& counters) {
    o << ",\"" << key << "\":{";
    bool first = true;
    for (unsigned i = 0; i < counters.Size(); ++i) {
        if (!counters.Ready(i)) continue;
        if (!first) o << ',';
        first = false;
        Quoted(o, counters.NameAt(i));
        o << ':' << counters.CountAt(i);
    }
    o << "},\"" << key << "Overflow\":" << counters.Overflow();
}
void EmitStageCosts(std::ostream& o, const OracleSample& s) {
    o << ",\"stageCostUs\":[";
    for (unsigned i = 0; i < OracleStageSlots; ++i) {
        if (i) o << ',';
        o << s.stageCostUs[i];
    }
    o << ']';
    o << ",\"stageChunks\":[";
    for (unsigned i = 0; i < OracleStageSlots; ++i) {
        if (i) o << ',';
        o << s.stageChunks[i];
    }
    o << ']';
}
void EmitRunSummaryMeter(std::ostream& o, const char* name, const Meter& meter);
void EmitOracle(std::ostream& o, const OracleSample& s) {
    o << "{\"sampleId\":" << s.sampleId << ",\"sampleNs\":" << s.sampleNs
      << ",\"costUs\":" << s.costUs
      << ",\"spanUs\":" << s.spanUs
      << ",\"prepareUs\":" << s.prepareUs
      << ",\"stagesRun\":" << s.stagesRun
      << ",\"merchantActorID\":" << s.merchantActorID
      << ",\"merchantBaseID\":" << s.merchantBaseID
      << ",\"merchantContainerID\":" << s.merchantContainerID
      << ",\"playerGold\":" << s.playerGold
      << ",\"merchantActorGold\":" << s.merchantActorGold
      << ",\"merchantContainerGold\":" << s.merchantContainerGold
      << ",\"merchantInventoryGold\":" << s.merchantInventoryGold
      << ",\"playerItemCount\":" << s.playerItemCount
      << ",\"targetBase\":" << s.targetBase
      << ",\"playerBaseCount\":" << s.playerBaseCount
      << ",\"vendorBaseCount\":" << s.vendorBaseCount
      << ",\"baseCountVisits\":" << s.baseCountVisits
      << ",\"walkRestarts\":" << s.walkRestarts
      << ",\"walkAbandonReason\":"; Quoted(o, s.walkAbandonReason);
    o
      << ",\"oracleEpoch\":" << s.epoch
      << ",\"unsampled\":" << s.unsampled
      << ",\"scannedEntries\":" << s.scannedEntries
      << ",\"rejectedEntries\":" << s.rejectedEntries
      << ",\"earlyExits\":" << s.earlyExits
      << ",\"truncatedStages\":" << s.truncatedStages
      << ",\"merchantHandle\":" << s.merchantHandle
      << ",\"handleRefID\":" << s.handleRefID
      << ",\"handleBaseID\":" << s.handleBaseID
      << ",\"handleIsPlayer\":" << (s.handleIsPlayer ? "true" : "false")
      << ",\"speakerHandle\":" << s.speakerHandle
      << ",\"speakerID\":" << s.speakerID
      << ",\"lastSpeakerHandle\":" << s.lastSpeakerHandle
      << ",\"lastSpeakerID\":" << s.lastSpeakerID
      << ",\"merchantSource\":"; Quoted(o, s.merchantSource);
    o << ",\"fallbackUsed\":" << (s.fallbackUsed ? "true" : "false")
      << ",\"attached\":" << (s.attached ? "true" : "false")
      << ",\"oracleTruncated\":" << (s.truncated ? "true" : "false")
      << ",\"speechLevel\":"; Number(o, s.speechLevel);
    o << ",\"speechXp\":"; Number(o, s.speechXp);
    o << ",\"speechThreshold\":"; Number(o, s.speechThreshold);
    o << ",\"merchantResolved\":" << (s.merchantResolved ? "true" : "false")
      << ",\"containerResolved\":" << (s.containerResolved ? "true" : "false")
      << ",\"speechResolved\":" << (s.speechResolved ? "true" : "false");
    // w-probe-r1: the window the sample was built in, its verdict, and where
    // each reading came from.
    o << ",\"startMenuGeneration\":" << s.startMenuGeneration
      << ",\"endMenuGeneration\":" << s.endMenuGeneration
      << ",\"startLoadGeneration\":" << s.startLoadGeneration
      << ",\"endLoadGeneration\":" << s.endLoadGeneration
      << ",\"startInventoryGeneration\":" << s.startInventoryGeneration
      << ",\"endInventoryGeneration\":" << s.endInventoryGeneration
      << ",\"firstEventSeq\":" << s.firstEventSeq
      << ",\"lastEventSeq\":" << s.lastEventSeq
      << ",\"startMerchantID\":" << s.startMerchantID
      << ",\"endMerchantID\":" << s.endMerchantID
      << ",\"entryVisits\":" << s.entryVisits
      << ",\"visitCapReached\":" << (s.visitCapReached ? "true" : "false")
      << ",\"sampleValid\":" << (s.sampleValid ? "true" : "false")
      << ",\"invalidationReason\":"; Quoted(o, s.invalidationReason);
    // w-probe-r8 (R8-1): where the two per-base counts came from.
    o << ",\"countSource\":"; Quoted(o, s.countSource);
    o << ",\"vendorCountSource\":"; Quoted(o, s.vendorCountSource);
    o << ",\"playerGoldSource\":"; Quoted(o, s.playerGoldSource);
    o << ",\"merchantActorGoldSource\":"; Quoted(o, s.merchantActorGoldSource);
    o << ",\"merchantContainerGoldSource\":"; Quoted(o, s.merchantContainerGoldSource);
    o << ",\"merchantInventorySource\":"; Quoted(o, s.merchantInventorySource);
    o << ",\"speechSource\":"; Quoted(o, s.speechSource);
    EmitStageCosts(o, s);
    o << '}';
}
void EmitSelected(std::ostream& o, const SelectedInfo& s) {
    o << "{\"listSize\":" << s.listSize
      << ",\"selectedIndexRead\":" << s.selectedIndexRead
      << ",\"indexRead\":" << (s.indexRead ? "true" : "false")
      << ",\"baseObj\":" << s.baseObj << ",\"ownerForm\":" << s.ownerForm
      << ",\"countDelta\":" << s.countDelta << ",\"xListCount\":" << s.xListCount
      << ",\"soulLevel\":" << s.soulLevel << ",\"value\":" << s.value
      << ",\"questObject\":" << (s.questObject ? "true" : "false")
      << ",\"favorite\":" << (s.favorite ? "true" : "false")
      << ",\"worn\":" << (s.worn ? "true" : "false")
      << ",\"enchanted\":" << (s.enchanted ? "true" : "false")
      << ",\"canBeTaken\":" << (s.canBeTaken ? "true" : "false")
      << ",\"owned\":" << (s.owned ? "true" : "false")
      << ",\"ownerReadable\":" << (s.ownerReadable ? "true" : "false")
      << ",\"ownerIsPlayer\":" << (s.ownerIsPlayer ? "true" : "false")
      << ",\"extraDataListPresent\":" << (s.extraDataListPresent ? "true" : "false")
      << ",\"extraUniqueIdPresent\":" << (s.extraUniqueIdPresent ? "true" : "false")
      << ",\"extraUniqueId\":" << s.extraUniqueId
      << ",\"stolenKnown\":" << (s.stolenKnown ? "true" : "false")
      << ",\"mixedExtraListStacks\":" << s.mixedExtraListStacks
      << ",\"displayName\":"; Quoted(o, s.displayName);
    // w-probe-r1: the identity of the row and the instant it was read, kept
    // apart. selectionValid says the index did not move between the two reads.
    o << ",\"observationId\":" << s.observationId
      << ",\"sampleStartNs\":" << s.sampleStartNs
      << ",\"sampleEndNs\":" << s.sampleEndNs
      << ",\"selectedIndexAfter\":" << s.selectedIndexAfter
      << ",\"selectionValid\":" << (s.selectionValid ? "true" : "false")
      << ",\"visits\":" << s.visits
      << ",\"visitCapReached\":" << (s.visitCapReached ? "true" : "false")
      << ",\"xListTruncated\":" << (s.xListTruncated ? "true" : "false")
      << ",\"source\":"; Quoted(o, s.source);
    o << ",\"indexSource\":"; Quoted(o, s.indexSource);
    o << ",\"instanceKind\":"; Quoted(o, s.instanceKind);
    o << ",\"identityQuality\":"; Quoted(o, s.identityQuality);
    o << '}';
}
void EmitWPhase(std::ostream& o, const WPhaseInfo& p) {
    o << "{\"capture\":" << (p.capture ? "true" : "false")
      << ",\"mainUiThread\":" << (p.mainUiThread ? "true" : "false")
      << ",\"barterOpen\":" << (p.barterOpen ? "true" : "false")
      << ",\"uiMoviePresent\":" << (p.uiMoviePresent ? "true" : "false")
      << ",\"itemSelectPresent\":" << (p.itemSelectPresent ? "true" : "false")
      << ",\"handlerUnchanged\":" << (p.handlerUnchanged ? "true" : "false")
      << ",\"firstObservation\":" << (p.firstObservation ? "true" : "false")
      << ",\"negativeTopmost\":" << (p.negativeTopmost ? "true" : "false")
      << ",\"modalOpen\":" << (p.modalOpen ? "true" : "false")
      << ",\"gamePaused\":" << (p.gamePaused ? "true" : "false")
      << ",\"firstUiTaskThread\":" << p.firstUiTaskThread
      << ",\"currentThread\":" << p.currentThread
      << ",\"handlerPointer\":" << p.handlerPointer
      << ",\"previousHandlerPointer\":" << p.previousHandlerPointer
      << ",\"reentryDepth\":" << p.reentryDepth << ",\"topmostMenu\":";
    Quoted(o, p.topmostMenu); o << ",\"openMenus\":"; Quoted(o, p.openMenus); o << '}';
}
void EmitWArgs(std::ostream& o, const WArgsInfo& a) {
    o << "{\"responseId\":" << a.responseId
      << ",\"responseKnown\":" << (a.responseKnown ? "true" : "false")
      << ",\"responseMatchesPrevious\":" << (a.responseMatchesPrevious ? "true" : "false")
      << ",\"handlerArgCount\":" << a.handlerArgCount << ",\"unitPrice\":";
    if (a.unitPriceKnown) Number(o, a.unitPrice); else o << "null";
    o << ",\"unitPriceSource\":"; Quoted(o, a.unitPriceSource); o << '}';
}
// The W observation block of a js_selected row (design section 4.2). Nothing
// here is an argument to anything: dispatchEligibility can only read REJECTED
// or UNPROVEN and missingPrerequisites is a list of what is absent.
void EmitWObservation(std::ostream& o, const WObservation& w) {
    o << "{\"observationId\":" << w.observationId << ",\"argsEvidenceCallIndex\":";
    if (w.argsEvidenceCallIndex < 0) o << "null"; else o << w.argsEvidenceCallIndex;
    o << ",\"quoteSource\":"; Quoted(o, w.quoteSource);
    o << ",\"preDispatchQuoteSource\":"; QuotedOrNull(o, w.preDispatchQuoteSource);
    o << ",\"responseLifetime\":"; Quoted(o, w.responseLifetime);
    o << ",\"foregroundEvidence\":"; Quoted(o, w.foregroundEvidence);
    o << ",\"foregroundSource\":"; Quoted(o, w.foregroundSource);
    o << ",\"modalEvidence\":"; Quoted(o, w.modalEvidence);
    o << ",\"modalSource\":"; Quoted(o, w.modalSource);
    o << ",\"proposedQuantity\":" << w.proposedQuantity << ",\"quantitySource\":";
    Quoted(o, w.quantitySource);
    // r3: the observed fourth argument of the quoted trace. Both print null
    // when nothing was observed, so a default can never read as a reading.
    o << ",\"arg3Type\":";
    if (w.arg3Type < 0) o << "null"; else o << w.arg3Type;
    o << ",\"arg3Bool\":";
    if (w.arg3Bool < 0) o << "null"; else o << (w.arg3Bool ? "true" : "false");
    o << ",\"fundsCheck\":"; Quoted(o, w.fundsCheck);
    o << ",\"dispatchEligibility\":"; Quoted(o, w.dispatchEligibility);
    o << ",\"readPhaseUnproven\":" << (w.readPhaseUnproven ? "true" : "false")
      << ",\"readPhaseReason\":"; Quoted(o, w.readPhaseReason);
    o << ",\"missingPrerequisites\":[";
    bool first = true;
    for (unsigned i = 0; i < PrerequisiteCount; ++i) {
        if (!(w.missingPrerequisites & (1u << i))) continue;
        if (!first) o << ',';
        first = false;
        Quoted(o, PrerequisiteName(i));
    }
    o << "]}";
}
void EmitPerf(std::ostream& o, const PerfInfo& p) {
    o << "{\"stage\":";
    Quoted(o, p.stage);
    o << ",\"budgetUs\":" << p.budgetUs << ",\"estimateUs\":" << p.estimateUs
      << ",\"prechecked\":" << (p.prechecked ? "true" : "false")
      << ",\"latched\":" << (p.latched ? "true" : "false")
      << ",\"prepareUs\":" << p.prepareUs << ",\"formScanUs\":" << p.formScanUs
      << ",\"xListScanUs\":" << p.xListScanUs << ",\"totalUs\":" << p.totalUs
      << ",\"entryCount\":" << p.entryCount << ",\"formCount\":" << p.formCount
      << ",\"xListCount\":" << p.xListCount
      << ",\"truncated\":" << (p.truncated ? "true" : "false");
    // w-probe-r1: the added reads are metered on their own and the run's
    // cumulative visits travel with the row. The four stages W would need and
    // this probe does not implement are named, so a missing stage can never
    // read as a stage that cost nothing.
    o << ",\"selectionReadUs\":" << p.selectionReadUs
      << ",\"protectionUs\":" << p.protectionUs
      << ",\"selectionVisits\":" << p.selectionVisits
      << ",\"protectionVisits\":" << p.protectionVisits
      << ",\"entryVisitsTotal\":" << p.entryVisitsTotal
      << ",\"xListVisitsTotal\":" << p.xListVisitsTotal
      << ",\"oracleVisitsTotal\":" << p.oracleVisitsTotal
      << ",\"visitAllowanceShort\":" << p.visitAllowanceShort
      << ",\"oracleSessionVisits\":" << p.oracleSessionVisits
      << ",\"visitCapReached\":" << (p.visitCapReached ? "true" : "false")
      << ",\"notMeasured\":[";
    for (unsigned i = 0; i < NotMeasuredCount; ++i) {
        if (i) o << ',';
        Quoted(o, NotMeasuredName(i));
    }
    o << "]}";
}
void EmitArgs(std::ostream& o, const Row& r) {
    o << '[';
    for (unsigned i = 0; i < r.capturedArgs && i < MaxArgs; ++i) {
        const auto& a = r.args[i];
        if (i) o << ',';
        o << "{\"index\":" << i << ",\"type\":" << a.type << ",\"number\":";
        if (a.hasNumber) Number(o, a.number); else o << "null";
        o << ",\"boolean\":" << (a.boolean ? "true" : "false")
          << ",\"hasBool\":" << (a.hasBool ? "true" : "false") << ",\"text\":";
        if (a.hasText) Quoted(o, a.text); else o << "null";
        o << '}';
    }
    o << ']';
}
void EmitSlimRuleCandidate(std::ostream& o, const Row& r) {
        std::ostringstream head;
        head.copyfmt(o);
        head << "{\"schema\":1,\"shape\":\"slim\",\"revision\":";
        Quoted(head, Revision);
        head << ",\"type\":\"js_rule_candidate\",\"seq\":" << r.seq
          << ",\"ms\":" << r.ms << ",\"processRunId\":";
        Quoted(head, processRun);
        head << ",\"menuGeneration\":" << r.menuGeneration
          << ",\"loadGeneration\":" << r.loadGeneration
          << ",\"inventoryGeneration\":" << r.inventoryGeneration
          << ",\"runToken\":" << r.runToken
          << ",\"index\":" << r.ruleCandidateIndex
          << ",\"baseObj\":" << r.ruleCandidateBaseObj
          << ",\"individual\":" << (r.ruleCandidateHasIndividual ? "true" : "false")
          << ",\"filteredIndex\":" << r.ruleCandidateFilteredIndex
          << ",\"indexDrift\":" << (r.ruleCandidateIndexDrift ? 1 : 0)
          << ",\"plannedIndex\":" << r.ruleCandidatePlannedIndex
          << ",\"unitPrice\":";
        Number(head, r.ruleCandidateUnitPrice);
        head << ",\"name\":";
        std::ostringstream tail;
        tail.copyfmt(o);
        tail << ",\"decision\":"; Quoted(tail, r.ruleCandidateDecision);
        tail << ",\"reason\":"; Quoted(tail, r.ruleCandidateReason);
        tail << ",\"bindWrites\":" << r.bindWrites
          << ",\"bindReadbackMatch\":" << (r.bindReadbackMatch ? "true" : "false")
          << ",\"merchantGold\":" << r.merchantGold << '}';
        const auto prefix = head.str();
        const auto suffix = tail.str();
        constexpr std::size_t SlimMaxBytes = 700; // Includes the JSONL newline.
        o << prefix;
        QuotedLimited(o, r.ruleCandidateName,
                      prefix.size() + suffix.size() + 3 <= SlimMaxBytes ?
                          SlimMaxBytes - prefix.size() - suffix.size() - 1 : 2);
        o << suffix << '\n';
}
// S2c (design-r2 2.11 / 3.6): one planned row of a batch. Fixed shape,
// no name, no config block; at most 256 per batch.
void EmitSlimBatchRow(std::ostream& o, const Row& r) {
    const auto& b = r.batchRow;
    o << "{\"schema\":1,\"shape\":\"slim\",\"revision\":";
    Quoted(o, Revision);
    o << ",\"type\":\"js_batch_row\",\"seq\":" << r.seq
      << ",\"ms\":" << r.ms << ",\"processRunId\":";
    Quoted(o, processRun);
    o << ",\"menuGeneration\":" << r.menuGeneration
      << ",\"loadGeneration\":" << r.loadGeneration
      << ",\"inventoryGeneration\":" << r.inventoryGeneration
      << ",\"runToken\":" << r.runToken
      << ",\"token\":" << b.token
      << ",\"listIndex\":" << b.listIndex
      << ",\"baseObj\":" << b.baseObj
      << ",\"stackCount\":" << b.stackCount
      << ",\"quantity\":" << b.quantity
      << ",\"unitPrice\":" << b.unitPrice
      << ",\"total\":" << b.total
      << ",\"baseValue\":" << b.baseValue
      << ",\"shadowPrice\":" << b.shadowPrice
      << ",\"merchantPerk\":" << (b.merchantPerk ? "true" : "false")
      << ",\"state\":";
    Quoted(o, b.state);
    o << ",\"pairInCall\":" << (b.pairInCall ? "true" : "false")
      << ",\"listRebuiltInCall\":" << (b.listRebuiltInCall ? "true" : "false")
      << ",\"itemEvents\":" << b.itemEvents
      << ",\"goldEvents\":" << b.goldEvents
      << ",\"foreignEvents\":" << b.foreignEvents << "}\n";
}
// S2c: the js_batch block, appended after the shared row fields. The
// shared block already carries reason / beforeSampleId / afterSampleId.
// v0.8.0 (w-probe-r69): the price snapshot of a js_convert_run or a
// js_batch row; JSON null when the row took none (a batch that never planned).
void EmitPriceSource(std::ostream& o, const PriceSourceReceipt& p) {
    o << ",\"priceSource\":";
    if (!p.present) {
        o << "null";
        return;
    }
    o << "{\"source\":"; Quoted(o, p.source);
    o << ",\"reason\":"; Quoted(o, p.reason);
    o << ",\"switchOn\":" << (p.switchOn ? "true" : "false")
      << ",\"walked\":" << (p.walked ? "true" : "false")
      << ",\"tableOk\":" << (p.tableOk ? "true" : "false")
      << ",\"visits\":" << p.visits
      << ",\"collected\":" << p.collected
      << ",\"dropped\":" << p.dropped
      << ",\"overflow\":" << (p.overflow ? "true" : "false")
      << ",\"playerSidePassing\":" << p.playerSidePassing
      << ",\"merchantSidePassing\":" << p.merchantSidePassing
      << ",\"unsupportedPassing\":" << p.unsupportedPassing
      << ",\"perkFactor\":"; Number(o, p.perkFactor);
    o << ",\"perkFactorAllPlayer\":"; Number(o, p.allPlayer);
    o << ",\"engineCalled\":" << (p.engineCalled ? "true" : "false")
      << ",\"engineSelf\":"; Number(o, p.engineSelf);
    o << ",\"evalUs\":" << p.evalUs
      << ",\"entries\":[";
    for (std::uint32_t i = 0; i < p.entryRows && i < p.entries.size(); ++i) {
        const auto& e = p.entries[i];
        o << (i ? "," : "") << "{\"perk\":" << e.perk
          << ",\"function\":" << static_cast<unsigned>(e.function)
          << ",\"dataType\":" << static_cast<int>(e.dataType)
          << ",\"values\":["; Number(o, e.value0);
        o << ','; Number(o, e.value1);
        o << "],\"avRead\":"; Number(o, e.avRead);
        o << ",\"side\":\"" << (e.merchantSide ? "merchant" : "player") << '"'
          << ",\"passed\":" << (e.passed ? "true" : "false")
          << ",\"term\":";
        if (e.supported) Number(o, e.term); else o << "null";
        o << '}';
    }
    o << "]}";
}
void EmitBatchBlock(std::ostream& o, const Row& r) {
    const auto& b = r.batch;
    o << ",\"outcome\":"; Quoted(o, b.outcome);
    o << ",\"stop\":"; Quoted(o, b.stop);
    o << ",\"abort\":"; Quoted(o, b.abort);
    o << ",\"eventMode\":"; Quoted(o, b.eventMode);
    o << ",\"unitsEligible\":" << b.unitsEligible
      << ",\"unitsPlanned\":" << b.unitsPlanned
      << ",\"unitsSold\":" << b.unitsSold
      << ",\"goldPlanned\":" << b.goldPlanned
      << ",\"goldDispatched\":" << b.goldDispatched
      << ",\"goldConfirmed\":" << b.goldConfirmed
      << ",\"goldReceived\":" << b.goldReceived
      << ",\"goldSource\":"; Quoted(o, b.goldSource);
    o << ",\"playerGoldDelta\":" << b.playerGoldDelta
      << ",\"vencGoldDelta\":" << b.vencGoldDelta
      << ",\"samplesValid\":" << (b.samplesValid ? "true" : "false")
      << ",\"reconciled\":" << (b.reconciled ? "true" : "false")
      << ",\"reconcileOk\":" << (b.reconcileOk ? "true" : "false")
      << ",\"rowsPlanned\":" << b.rowsPlanned
      << ",\"rowsDispatched\":" << b.rowsDispatched
      << ",\"rowsConfirmed\":" << b.rowsConfirmed
      << ",\"rowsUnconfirmed\":" << b.rowsUnconfirmed
      << ",\"rowsDeferred\":" << b.rowsDeferred
      << ",\"commitFrames\":" << b.commitFrames
      << ",\"commitTaskUs\":" << b.commitTaskUsMax
      << ",\"commitTaskUsTotal\":" << b.commitTaskUsTotal
      << ",\"pairInCall\":" << b.pairInCallRows
      << ",\"listRebuiltInCall\":" << b.listRebuiltInCallRows
      << ",\"listRemaps\":" << b.listRemaps
      << ",\"postCommitGapMs\":" << b.postCommitGapMs
      << ",\"lateEvents\":" << b.lateEvents
      << ",\"lateDropped\":" << b.lateDropped
      << ",\"pendingBypassRows\":" << b.pendingBypassRows
      << ",\"bindFail\":"; Quoted(o, b.bindFail);
    o << ",\"bindRewrites\":" << b.bindRewrites
      << ",\"splitEventRows\":" << b.splitEventRows
      << ",\"foreignEvent\":{\"known\":" << (b.foreignKnown ? "true" : "false")
      << ",\"oldContainer\":" << b.foreignOld
      << ",\"newContainer\":" << b.foreignNew
      << ",\"baseObj\":" << b.foreignBase
      << ",\"itemCount\":" << b.foreignCount << '}'
      << ",\"pressRowCap\":" << b.pressRowCap
      << ",\"rowsCapped\":" << b.rowsCapped
      << ",\"slimRows\":" << b.slimRows
      << ",\"batchLatched\":" << (b.latched ? "true" : "false")
      << ",\"balanceLimited\":" << (b.balanceLimited ? "true" : "false")
      << ",\"runLimited\":" << (b.runLimited ? "true" : "false")
      << ",\"balanceUnknown\":" << (b.balanceUnknown ? "true" : "false")
      << ",\"edgeToLineMs\":" << b.edgeToLineMs
      << ",\"surface\":"; Quoted(o, b.surface);
    o << ",\"textFallback\":" << (b.textFallback ? "true" : "false")
      << ",\"unknownPlaceholders\":" << b.unknownPlaceholders
      << ",\"shadow\":{\"known\":" << (b.shadowKnown ? "true" : "false")
      << ",\"merchantPerk\":" << (b.shadowMerchantPerk ? "true" : "false")
      << ",\"multiplier\":"; Number(o, b.shadowMultiplier);
    o << ",\"hagglingRank\":" << b.shadowHagglingRank
      << ",\"speech\":"; Number(o, b.shadowSpeech);
    o << ",\"av107\":"; Number(o, b.shadowAv107);
    o << ",\"av146\":"; Number(o, b.shadowAv146);
    o << ",\"barterMin\":"; Number(o, b.shadowBarterMin);
    o << ",\"barterMax\":"; Number(o, b.shadowBarterMax);
    o << '}';
    EmitPriceSource(o, r.priceSource);
}
// v0.1.0 (design r3 4.4): one planned row of a sell-anywhere press.
// Fixed shape, no name, no config block; at most 256 per press.
void EmitSlimConvertRow(std::ostream& o, const Row& r) {
    const auto& c = r.convertRow;
    o << "{\"schema\":1,\"shape\":\"slim\",\"revision\":";
    Quoted(o, Revision);
    o << ",\"type\":\"js_convert_row\",\"seq\":" << r.seq
      << ",\"ms\":" << r.ms << ",\"processRunId\":";
    Quoted(o, processRun);
    o << ",\"loadGeneration\":" << r.loadGeneration
      << ",\"runToken\":" << r.runToken
      << ",\"token\":" << c.token
      << ",\"baseObj\":" << c.baseObj
      << ",\"value\":" << c.value
      << ",\"price\":" << c.price
      << ",\"units\":" << c.units
      << ",\"countBefore\":" << c.countBefore
      << ",\"countAfter\":" << c.countAfter
      << ",\"slice\":" << c.slice
      << ",\"xpCalls\":" << c.xpCalls
      << ",\"plainUnits\":" << c.plainUnits
      << ",\"instanceUnits\":" << c.instanceUnits
      << ",\"lists\":" << c.lists
      << ",\"stolenUnits\":" << c.stolenUnits
      << ",\"partial\":" << (c.partial ? "true" : "false")
      << ",\"keep\":" << c.keep
      << ",\"removeCalls\":" << c.removeCalls
      << ",\"category\":";
    Quoted(o, c.category);
    o << ",\"result\":";
    Quoted(o, c.state);
    o << "}\n";
}
// v0.5.0: the observe-only frame window of one press (FrameWindow.h),
// slim like js_convert_row: no config block. Offsets are ms from the key
// edge; -1 = not seen.
void EmitSlimFrameWindow(std::ostream& o, const Row& r) {
    const auto& f = r.frame;
    o << "{\"schema\":1,\"shape\":\"slim\",\"revision\":";
    Quoted(o, Revision);
    o << ",\"type\":\"js_frame_window\",\"seq\":" << r.seq
      << ",\"ms\":" << r.ms << ",\"processRunId\":";
    Quoted(o, processRun);
    o << ",\"loadGeneration\":" << r.loadGeneration
      << ",\"runToken\":" << f.runToken
      << ",\"end\":";
    Quoted(o, f.end);
    o << ",\"windowStartMs\":" << FrameWindowStartUs / 1000
      << ",\"windowEndMs\":" << FrameWindowEndUs / 1000
      << ",\"convertEndMs\":" << (f.convertEndUs >= f.edgeUs ? (f.convertEndUs - f.edgeUs) / 1000 : 0)
      << ",\"rowsSold\":" << f.rowsSold
      << ",\"unitsSold\":" << f.unitsSold
      << ",\"listsSold\":" << f.listsSold
      << ",\"goldPaid\":" << f.goldPaid
      << ",\"speechXpCalls\":" << f.xpCalls
      << ",\"speechXpPoints\":";
    Number(o, f.xpPoints);
    o << ",\"receiptTarget\":" << f.receiptTarget
      << ",\"receiptDrainAtMs\":" << f.receiptDrainAtMs
      << ",\"lineShownAtMs\":" << f.lineShownAtMs
      << ",\"ticks\":" << f.ticks
      << ",\"ticksInWindow\":" << f.ticksInWindow
      << ",\"longFramesInWindow\":" << f.longFramesInWindow
      << ",\"maxGapUs\":" << f.maxGapUs
      << ",\"maxGapAtMs\":" << f.maxGapAtMs
      << ",\"maxGapBeforeUs\":" << f.maxGapBeforeUs
      << ",\"maxGapBeforeAtMs\":" << f.maxGapBeforeAtMs
      << ",\"windowSumUs\":" << f.windowSumUs
      << ",\"topGaps\":[";
    for (std::uint32_t i = 0; i < f.topCount && i < f.top.size(); ++i) {
        o << (i ? "," : "") << "{\"atMs\":" << f.top[i].atMs << ",\"gapUs\":" << f.top[i].gapUs << '}';
    }
    o << "],\"speechLevelAtEnd\":" << f.atArm.speechLevel
      << ",\"speechLevelLast\":" << f.last.speechLevel
      << ",\"speechLevelChangeAtMs\":" << f.speechLevelChangeAtMs
      << ",\"speechBaseAtEnd\":";
    Number(o, f.atArm.speechBase);
    o << ",\"speechBaseLast\":";
    Number(o, f.last.speechBase);
    o << ",\"speechBaseChangeAtMs\":" << f.speechBaseChangeAtMs
      << ",\"playerLevelAtEnd\":" << f.atArm.playerLevel
      << ",\"playerLevelLast\":" << f.last.playerLevel
      << ",\"playerLevelChangeAtMs\":" << f.playerLevelChangeAtMs
      << ",\"playerXpAtEnd\":";
    Number(o, f.atArm.playerXp);
    o << ",\"playerXpLast\":";
    Number(o, f.last.playerXp);
    o << ",\"playerXpChangeAtMs\":" << f.playerXpChangeAtMs
      << ",\"containerDuring\":" << f.containerDuring
      << ",\"containerAfter\":" << f.containerAfter
      << ",\"containerFirstAfterAtMs\":" << f.containerFirstAfterAtMs
      << ",\"containerLastAfterAtMs\":" << f.containerLastAfterAtMs
      << ",\"events\":[";
    for (std::uint32_t i = 0; i < f.namedCount && i < f.named.size(); ++i) {
        const auto& e = f.named[i];
        o << (i ? "," : "") << "{\"kind\":";
        Quoted(o, FrameEventKindName(e.kind));
        o << ",\"name\":";
        Quoted(o, e.name.data());
        o << ",\"atMs\":" << e.atMs << '}';
    }
    o << "],\"eventsDropped\":" << f.namedDropped << "}\n";
}
// v0.1.0: the js_convert_run block, appended after the shared row
// fields (which already carry reason and runToken).
void EmitConvertBlock(std::ostream& o, const Row& r) {
    const auto& c = r.convert;
    o << ",\"outcome\":"; Quoted(o, c.outcome);
    o << ",\"gate\":"; Quoted(o, c.gate);
    o << ",\"stop\":"; Quoted(o, c.stop);
    o << ",\"unitsEligible\":" << c.unitsEligible
      << ",\"unitsPlanned\":" << c.unitsPlanned
      << ",\"unitsSold\":" << c.unitsSold
      << ",\"goldPlanned\":" << c.goldPlanned
      << ",\"goldPaid\":" << c.goldPaid
      << ",\"goldBefore\":" << c.goldBefore
      << ",\"goldAfter\":" << c.goldAfter
      << ",\"entriesRead\":" << c.entriesRead
      << ",\"rowsEligible\":" << c.rowsEligible
      << ",\"rowsPlanned\":" << c.rowsPlanned
      << ",\"rowsSold\":" << c.rowsSold
      << ",\"rowsRefused\":" << c.rowsRefused
      << ",\"rowsRolledBack\":" << c.rowsRolledBack
      << ",\"rowsMismatch\":" << c.rowsMismatch
      << ",\"rowsLost\":" << c.rowsLost
      << ",\"excluded\":{\"type\":" << c.exType
      << ",\"categoryOff\":" << c.exCategoryOff
      << ",\"rule\":" << c.exRule
      << ",\"absolute\":" << c.exAbsolute
      << ",\"owner\":" << c.exOwner
      << ",\"invalid\":" << c.exInvalid
      << ",\"notPlain\":" << c.exNotPlain
      << ",\"zeroGold\":" << c.exZeroGold
      << ",\"runLimit\":" << c.exRunLimit
      << ",\"builtin\":" << c.exBuiltin << '}'
      << ",\"notPlainKinds\":{";
    constexpr const char* keptNames[] = {"worn", "favorite", "enchanted", "quest", "foreignOwner", "value",
                                         "renamed", "reference", "other"};
    for (std::size_t i = 0; i < c.keptKinds.size(); ++i) {
        o << (i ? "," : "") << '"' << keptNames[i] << "\":" << c.keptKinds[i];
    }
    o << "},\"notPlainOtherTypes\":[";
    for (std::uint32_t i = 0; i < c.otherTypeCount && i < c.otherTypes.size(); ++i) {
        o << (i ? "," : "") << c.otherTypes[i];
    }
    o << "],\"notPlainRows\":[";
    for (std::uint32_t i = 0; i < c.keptRowCount && i < c.keptRows.size(); ++i) {
        o << (i ? "," : "") << '[' << c.keptRows[i][0] << ',' << c.keptRows[i][1] << ',' << c.keptRows[i][2] << ']';
    }
    o << ']'
      << ",\"instanceRows\":" << c.instanceRows
      << ",\"instanceUnits\":" << c.instanceUnits
      << ",\"instanceLists\":" << c.instanceLists
      << ",\"instanceRowsSold\":" << c.instanceRowsSold
      << ",\"instanceUnitsSold\":" << c.instanceUnitsSold
      << ",\"categories\":{\"junk\":true,\"ingredients\":" << (c.sellIngredients ? "true" : "false")
      << ",\"weakWeapons\":" << (c.sellWeakWeapons ? "true" : "false")
      << ",\"weakArmor\":" << (c.sellWeakArmor ? "true" : "false")
      << ",\"clothing\":" << (c.sellClothing ? "true" : "false")
      << ",\"enchantedLoot\":" << (c.sellEnchantedLoot ? "true" : "false");
    constexpr const char* keepNames[] = {"alchemyIngredients", "cookedFood", "rawFood", "drinks",
                                         "ore", "gems", "pelts", "tools",
                                         "restorePotions", "skillPotions", "otherPotions", "poisons",
                                         "midGear", "highGear", "jewelry"};
    static_assert(std::size(keepNames) == std::tuple_size_v<decltype(c.keepOn)>);
    for (std::size_t i = 0; i < c.keepOn.size(); ++i) {
        o << ",\"" << keepNames[i] << "\":" << (c.keepOn[i] ? "true" : "false");
    }
    o << '}'
      << ",\"soldByCategory\":{\"junk\":" << c.soldJunk
      << ",\"ingredients\":" << c.soldIngredients
      << ",\"weakWeapons\":" << c.soldWeakWeapons
      << ",\"weakArmor\":" << c.soldWeakArmor
      << ",\"clothing\":" << c.soldClothing
      << ",\"enchantedLoot\":" << c.soldEnchantedLoot;
    for (std::size_t i = 0; i < c.soldKeep.size(); ++i) o << ",\"" << keepNames[i] << "\":" << c.soldKeep[i];
    o << '}'
      << ",\"keep\":{";
    for (std::size_t i = 0; i < c.keepLimit.size(); ++i) {
        o << (i ? "," : "") << '"' << keepNames[i] << "\":" << c.keepLimit[i];
    }
    o << '}'
      << ",\"partialRowsSold\":" << c.partialRowsSold
      << ",\"partialUnitsSold\":" << c.partialUnitsSold
      << ",\"junkRule\":\"" << (c.junkValueOnly ? "valueOnly" : "classic") << '"'
      << ",\"junkMaxValue\":" << c.junkMaxValue
      << ",\"sellStolen\":" << (c.sellStolen ? "true" : "false")
      << ",\"knownEnchantmentsOnly\":" << (c.knownEnchantmentsOnly ? "true" : "false")
      << ",\"enchantedLootMaxValue\":" << c.enchantedLootMaxValue
      << ",\"stolenRows\":" << c.stolenRows
      << ",\"stolenUnits\":" << c.stolenUnits
      << ",\"stolenUnitsSold\":" << c.stolenUnitsSold
      << ",\"rowsCleared\":" << c.rowsCleared
      << ",\"kept\":{\"atLimit\":" << c.keptAtLimit
      << ",\"protected\":" << c.keptProtected
      << ",\"stolen\":" << c.keptStolen
      << ",\"enchantUnknown\":" << c.keptEnchantUnknown
      << ",\"categoryOff\":" << c.keptCategoryOff << '}'
      << ",\"catalog\":{\"guardGear\":" << c.catalogGuardGear
      << ",\"guardNpcs\":" << c.catalogGuardNpcs
      << ",\"guardSourcesMissing\":" << c.catalogGuardSourcesMissing
      << ",\"uniques\":" << c.catalogUniques
      << ",\"carpenterInputs\":" << c.catalogCarpenterInputs
      << ",\"toolForms\":" << c.catalogToolForms
      << ",\"neverForms\":" << c.catalogNeverForms
      << ",\"enchantLevels\":" << c.catalogEnchantLevels
      << ",\"artifactKeywords\":" << c.catalogArtifactKeywords
      << ",\"cookedForms\":" << c.catalogCookedForms
      << ",\"cookInputs\":" << c.catalogCookInputs
      << ",\"drinkForms\":" << c.catalogDrinkForms
      << ",\"drinkKeywords\":" << c.catalogDrinkKeywords
      << ",\"questFoods\":" << c.catalogQuestFoods << '}'
      << ",\"speechXpCalls\":" << c.xpCalls
      << ",\"speechLevelBefore\":" << c.speechLevelBefore
      << ",\"speechLevelAfter\":" << c.speechLevelAfter
      << ",\"speechXpBefore\":"; Number(o, c.speechXpBefore);
    o << ",\"speechXpAfter\":"; Number(o, c.speechXpAfter);
    o << ",\"priceModel\":{\"known\":" << (c.priceKnown ? "true" : "false")
      << ",\"clamped\":" << (c.clamped ? "true" : "false")
      << ",\"ratePercent\":" << c.ratePercent
      << ",\"hagglingRank\":" << c.hagglingRank
      << ",\"speech\":"; Number(o, c.speech);
    o << ",\"f\":"; Number(o, c.factor);
    o << ",\"m\":"; Number(o, c.multiplier);
    o << ",\"mPrime\":"; Number(o, c.mPrime);
    o << ",\"av107\":"; Number(o, c.av107);
    o << ",\"av146\":"; Number(o, c.av146);
    o << ",\"barterMin\":"; Number(o, c.barterMin);
    o << ",\"barterMax\":"; Number(o, c.barterMax);
    o << '}'
      << ",\"readUs\":" << c.readUs
      << ",\"readCountsUs\":" << c.readCountsUs
      << ",\"slices\":" << c.slices
      << ",\"sliceMaxUs\":" << c.sliceMaxUs
      << ",\"sliceTotalUs\":" << c.sliceTotalUs
      << ",\"removeCalls\":" << c.removeCalls
      << ",\"goldCalls\":" << c.goldCalls
      << ",\"stackRows\":" << c.stackRows
      << ",\"stackFallbackRows\":" << c.stackFallbackRows
      << ",\"containerEvents\":" << c.containerEvents
      << ",\"slimRows\":" << c.slimRows
      << ",\"convertLatched\":" << (c.latched ? "true" : "false")
      << ",\"runLimited\":" << (c.runLimited ? "true" : "false")
      << ",\"edgeToLineMs\":" << c.edgeToLineMs
      << ",\"surface\":"; Quoted(o, c.surface);
    o << ",\"textFallback\":" << (c.textFallback ? "true" : "false")
      << ",\"unknownPlaceholders\":" << c.unknownPlaceholders;
    EmitPriceSource(o, r.priceSource);
}
void Emit(std::ostream& o, const Row& r) {
    if (std::strcmp(r.type, "js_convert_row") == 0) {
        EmitSlimConvertRow(o, r);
        return;
    }
    if (std::strcmp(r.type, "js_frame_window") == 0) {
        EmitSlimFrameWindow(o, r);
        return;
    }
    if (std::strcmp(r.type, "js_rule_candidate") == 0) {
        EmitSlimRuleCandidate(o, r);
        return;
    }
    if (std::strcmp(r.type, "js_batch_row") == 0) {
        EmitSlimBatchRow(o, r);
        return;
    }
    o << "{\"schema\":1,\"revision\":";
    Quoted(o, Revision);
    o << ",\"type\":";
    Quoted(o, r.type);
    o << ",\"reason\":"; Quoted(o, (std::strcmp(r.type, "js_gfx_entry") == 0 ||
                                    std::strcmp(r.type, "js_gfx_list") == 0 ||
                                    std::strcmp(r.type, "js_gfx_root") == 0)
                                       ? r.gfxReason : r.reason);
    o << ",\"origin\":"; Quoted(o, r.origin);
    o << ",\"seq\":" << r.seq << ",\"ms\":" << r.ms << ",\"epoch\":" << r.epoch
      << ",\"monotonicNs\":" << r.monotonicNs << ",\"elapsedUs\":" << r.elapsedUs
      << ",\"thread\":"; Quoted(o, ThreadName(r.thread));
    o << ",\"mutation\":\"none\",\"callbackInvocation\":false";
    // w-probe-r1: which process wrote the row, where it was produced and which
    // generations were current. On every row, so a reader never has to infer
    // any of the three from the file order.
    o << ",\"processRunId\":"; Quoted(o, processRun);
    o << ",\"phase\":"; Quoted(o, PhaseName(r.phase));
    o << ",\"menuGeneration\":" << r.menuGeneration
      << ",\"loadGeneration\":" << r.loadGeneration
      << ",\"inventoryGeneration\":" << r.inventoryGeneration
      << ",\"firstEventSeq\":" << r.firstEventSeq
      << ",\"lastEventSeq\":" << r.lastEventSeq;
    // w-probe-r40: only user-settings protection values remain in this block.
    o << ",\"config\":{\"ProtectedNames\":";
    Quoted(o, r.cfgProtectedNames);
    o << ",\"ProtectedForms\":";
    Quoted(o, r.cfgProtectedForms);
    o << ",\"ProtectionFacts\":";
    Quoted(o, r.cfgProtectionFacts);
    o << '}';
    // w-probe-r28 (R29-1/R29-3), extended by w-probe-r29 (B-001): the D-W11
    // settings echo, append-only. The eight v0.1 keys (cfgMode is the
    // display-only `Mode`), the source the live snapshot came from, the atomic
    // config generation and the effective state are written on every row.
    o << ",\"cfgMode\":"; Quoted(o, r.cfgMode);
    o << ",\"cfgEnabled\":" << r.cfgEnabled
      << ",\"cfgConfirmBeforeSell\":" << r.cfgConfirmBeforeSell
      << ",\"cfgHotkey\":" << r.cfgHotkey
      << ",\"cfgMaxValue\":" << r.cfgMaxValue
      << ",\"cfgMinWeight\":"; Number(o, r.cfgMinWeight);
    o << ",\"cfgMaxValueWeight\":"; Number(o, r.cfgMaxValueWeight);
    o << ",\"cfgMaxItems\":" << r.cfgMaxItems
      << ",\"settingsSource\":"; Quoted(o, r.settingsSource);
    o << ",\"configGeneration\":" << r.configGeneration
      << ",\"effectiveOn\":" << (r.effectiveOn ? "true" : "false")
      << ",\"effectiveReason\":"; Quoted(o, r.effectiveReason);
    o << ",\"releaseId\":"; Quoted(o, r.releaseId);
    o << ",\"runtime\":"; Quoted(o, r.runtime);
    o << ",\"skse\":"; Quoted(o, r.skse);
    o << ",\"addressLibrary\":"; Quoted(o, r.addressLibrary);
    o << ",\"assetHashes\":{\"baseIni\":"; Quoted(o, r.baseIniSha256);
    o << ",\"esp\":"; Quoted(o, r.espSha256); o << '}';
    o << ",\"acceptanceRun\":"; Quoted(o, r.acceptanceRun);
    o << ",\"receiptBasename\":"; Quoted(o, r.receiptBasename);
    o << ",\"receiptFallback\":" << (r.receiptFallback ? "true" : "false")
      << ",\"menuOpen\":" << (r.menuOpen ? "true" : "false")
      << ",\"rotateBytes\":" << r.rotateBytes
      << ",\"retainedGenerations\":" << r.retainedGenerations;
    if (r.rotatedFrom[0]) {
        o << ",\"rotatedFrom\":"; Quoted(o, r.rotatedFrom);
        o << ",\"rotatedBytes\":" << r.rotatedBytes
          << ",\"generation\":" << r.generation;
    }
    // w-probe-r20 (R21-4): the four GFx arrays live in the heap payload now.
    // A row of a type that never observed GFx carries no payload; `empty`
    // reproduces the exact same all-zero/false/empty shape the inline arrays
    // used to default to, so the emitted JSON is unchanged either way.
    static const GfxPayload kEmptyGfx{};
    const GfxPayload& g = r.gfx ? *r.gfx : kEmptyGfx;
    if (std::strcmp(r.type, "js_gfx_entry") == 0) {
        o << ",\"trigger\":"; Quoted(o, r.gfxTrigger);
        o << ",\"selectedIndex\":" << r.gfxSelectedIndex << ",\"baseFormKey\":" << r.baseObj
          << ",\"count\":" << r.count << ",\"memberCountTotal\":" << g.gfxMemberCountTotal
          << ",\"truncated\":" << (g.gfxTruncated ? "true" : "false") << ",\"argsUnitPrice\":";
        if (r.gfxArgsUnitPriceKnown) Number(o, r.gfxArgsUnitPrice); else o << "null";
        o << ",\"members\":[";
        for (unsigned i = 0; i < g.gfxMemberCount; ++i) {
            if (i) o << ',';
            const auto& member = g.gfxMembers[i];
            o << "{\"name\":"; Quoted(o, member.name);
            o << ",\"gfxType\":" << member.gfxType << ',';
            if (member.valueKind == 1) { o << "\"num\":"; Number(o, member.number); }
            else if (member.valueKind == 2) { o << "\"str\":"; Quoted(o, member.text); }
            else o << "\"null\":null";
            o << '}';
        }
        o << ']';
    }
    if (std::strcmp(r.type, "js_gfx_list") == 0) {
        // w-probe-r16 (R16-2): list-wide rendered price read. Bounded to
        // Row::MaxGfxListEntries; entryCountTotal/truncated say when the walk
        // was cut short, never silently.
        // w-probe-r17 (R17-3): each entry additionally carries filterFlag/
        // filteredIndex/itemIndex/type/formType/equipState (null when the GFx
        // member was missing or non-numeric) and cap 128 -> 256.
        o << ",\"trigger\":"; Quoted(o, r.gfxTrigger);
        o << ",\"sellSide\":";
        if (r.gfxListSellSideKnown) o << (r.gfxListSellSide ? "true" : "false"); else o << "null";
        o << ",\"entryCountTotal\":" << g.gfxListEntryCountTotal
          << ",\"truncated\":" << (g.gfxListTruncated ? "true" : "false");
        o << ",\"entries\":[";
        for (unsigned i = 0; i < g.gfxListEntryCount; ++i) {
            if (i) o << ',';
            const auto& e = g.gfxListEntries[i];
            o << "{\"index\":" << e.index << ",\"baseId\":";
            if (e.baseIdKnown) o << e.baseId; else o << "null";
            o << ",\"eslId\":";
            if (e.eslIdKnown) Number(o, e.eslId); else o << "null";
            o << ",\"count\":";
            if (e.countKnown) Number(o, e.count); else o << "null";
            o << ",\"infoValue\":";
            if (e.infoValueKnown) Number(o, e.infoValue); else o << "null";
            o << ",\"infoValueWeight\":";
            if (e.infoValueWeightKnown) Number(o, e.infoValueWeight); else o << "null";
            o << ",\"infoWeight\":";
            if (e.infoWeightKnown) Number(o, e.infoWeight); else o << "null";
            o << ",\"filterFlag\":";
            if (e.filterFlagKnown) Number(o, e.filterFlag); else o << "null";
            o << ",\"filteredIndex\":";
            if (e.filteredIndexKnown) Number(o, e.filteredIndex); else o << "null";
            o << ",\"itemIndex\":";
            if (e.itemIndexKnown) Number(o, e.itemIndex); else o << "null";
            o << ",\"type\":";
            if (e.typeKnown) Number(o, e.type); else o << "null";
            o << ",\"formType\":";
            if (e.formTypeKnown) Number(o, e.formType); else o << "null";
            o << ",\"equipState\":";
            if (e.equipStateKnown) Number(o, e.equipState); else o << "null";
            o << ",\"name\":";
            if (e.nameKnown) Quoted(o, e.name); else o << "null";
            o << '}';
        }
        o << ']';
    }
    if (std::strcmp(r.type, "js_gfx_root") == 0) {
        // w-probe-r18 (R18-4): the BarterMenu root object's own members.
        // `members` shares its per-member shape with js_gfx_entry (name/
        // gfxType/num-or-str-or-null); `nested` is one array per root member
        // that qualified for the one nested level, each with its own
        // memberCountTotal/truncated.
        const auto emitMembers = [&o](const Row::GfxMember* members, unsigned count) {
            o << '[';
            for (unsigned i = 0; i < count; ++i) {
                if (i) o << ',';
                const auto& member = members[i];
                o << "{\"name\":"; Quoted(o, member.name);
                o << ",\"gfxType\":" << member.gfxType << ',';
                if (member.valueKind == 1) { o << "\"num\":"; Number(o, member.number); }
                else if (member.valueKind == 2) { o << "\"str\":"; Quoted(o, member.text); }
                else o << "\"null\":null";
                o << '}';
            }
            o << ']';
        };
        o << ",\"trigger\":"; Quoted(o, r.gfxTrigger);
        o << ",\"rootPresent\":" << (r.gfxRootPresent ? "true" : "false");
        o << ",\"memberCountTotal\":" << g.gfxRootMemberCountTotal
          << ",\"truncated\":" << (g.gfxRootTruncated ? "true" : "false");
        o << ",\"members\":"; emitMembers(g.gfxRootMembers.data(), g.gfxRootMemberCount);
        o << ",\"nested\":[";
        for (unsigned n = 0; n < g.gfxRootNestedCount; ++n) {
            if (n) o << ',';
            const auto& group = g.gfxRootNested[n];
            o << "{\"parentName\":"; Quoted(o, group.parentName);
            o << ",\"memberCountTotal\":" << group.memberCountTotal
              << ",\"truncated\":" << (group.truncated ? "true" : "false");
            o << ",\"members\":"; emitMembers(group.members.data(), group.memberCount);
            o << '}';
        }
        o << ']';
    }
    o << ",\"name\":"; Quoted(o, r.name);
    if (std::strcmp(r.type, "js_bind") != 0) {
        o << ",\"text\":"; Quoted(o, r.text);
    }
    o << ",\"moduleRelOffset\":" << r.moduleRelOffset
      << ",\"acceptOffset\":" << r.acceptOffset << ",\"hashOffset\":" << r.hashOffset
      << ",\"handlerRelOffset\":" << r.handlerRelOffset
      << ",\"delegateRelOffset\":" << r.delegateRelOffset
      << ",\"callbackCount\":" << r.callbackCount << ",\"hashCount\":" << r.hashCount
      << ",\"onlyInAccept\":" << r.onlyInAccept << ",\"onlyInHash\":" << r.onlyInHash
      << ",\"offsetDifferCount\":" << r.offsetDifferCount
      << ",\"handlerDifferCount\":" << r.handlerDifferCount
      << ",\"handlerIsMenu\":" << (r.handlerIsMenu ? "true" : "false")
      << ",\"presentInHash\":" << (r.presentInHash ? "true" : "false")
      << ",\"fromAccept\":" << (r.fromAccept ? "true" : "false")
      << ",\"offsetDiffers\":" << (r.offsetDiffers ? "true" : "false")
      << ",\"handlerDiffers\":" << (r.handlerDiffers ? "true" : "false")
      << ",\"argCount\":" << r.argCount << ",\"capturedArgs\":" << r.capturedArgs
      << ",\"callIndex\":" << r.callIndex << ",\"originalNs\":" << r.originalNs
      << ",\"barterOpen\":" << (r.barterOpen ? "true" : "false")
      << ",\"delegateMatched\":" << (r.delegateMatched ? "true" : "false")
    // w-probe-r1: the thread the producer really ran on, the kind of task, the
    // two instants of the chain boundary, the re-entry depth and whether every
    // raw argument and its type were captured.
      << ",\"producerThreadId\":" << r.producerThreadId
      << ",\"taskKind\":"; Quoted(o, r.taskKind);
    o << ",\"captureNs\":" << r.captureNs << ",\"publishNs\":" << r.publishNs
      << ",\"reentryDepth\":" << r.reentryDepth
      << ",\"argsComplete\":" << (r.argsComplete ? "true" : "false")
      << ",\"argsShape\":"; Quoted(o, r.argsShape);
    o << ",\"argsShapeReason\":"; Quoted(o, r.argsShapeReason);
    o << ",\"args\":"; EmitArgs(o, r);
    o << ",\"oldContainer\":" << r.oldContainer << ",\"newContainer\":" << r.newContainer
      << ",\"baseObj\":" << r.baseObj << ",\"referenceID\":" << r.referenceID
      << ",\"uniqueID\":" << r.uniqueID << ",\"itemCount\":" << r.itemCount
      << ",\"statValue\":" << r.statValue
      << ",\"batchIndex\":" << r.batchIndex << ",\"batchSize\":" << r.batchSize
      << ",\"attempt\":" << r.attempt
      << ",\"attemptToken\":" << r.attemptToken
      << ",\"terminalReason\":"; Quoted(o, r.terminalReason);
    o << ",\"dispatchCount\":" << r.dispatchCount
      << ",\"responseIdSource\":"; Quoted(o, r.responseIdSource);
    o << ",\"responseValueSource\":"; Quoted(o, r.responseValueSource);
    o << ",\"refusalTerm\":"; Quoted(o, r.refusalTerm);
    o << ",\"quantity\":" << r.quantity << ",\"count\":" << r.count;
    o << ",\"price\":"; Number(o, r.price);
    o << ",\"merchantGold\":" << r.merchantGold;
    o << ",\"beforeSampleId\":" << r.beforeSampleId
      << ",\"afterSampleId\":" << r.afterSampleId
      << ",\"settleSampleId\":" << r.settleSampleId
      << ",\"settleElapsedMs\":" << r.settleElapsedMs
      << ",\"coalescedInto\":" << r.coalescedInto
      << ",\"settleSampleState\":"; Quoted(o, r.settleSampleState);
    if (std::strcmp(r.type, "js_sale_attempt") == 0) {
        o << ",\"mainTaskGapMs\":" << r.mainTaskGapMs;
    }
    o << ",\"quoteBaseObj\":" << r.quoteBaseObj
      << ",\"protectionRule\":"; Quoted(o, r.protectionRule);
    o << ",\"itemEventAttemptToken\":" << r.itemEventAttemptToken
      << ",\"goldEventAttemptToken\":" << r.goldEventAttemptToken
      << ",\"itemEventSeq\":" << r.itemEventSeq
      << ",\"goldEventSeq\":" << r.goldEventSeq
      << ",\"itemEventCount\":" << r.itemEventCount
      << ",\"goldEventCount\":" << r.goldEventCount
      << ",\"lateEvents\":" << r.lateEvents
      << ",\"eventsLost\":" << r.eventsLost
      << ",\"attemptQuantity\":" << r.attemptQuantity
      << ",\"attemptUnitPrice\":" << r.attemptUnitPrice
      << ",\"goldOldContainer\":" << r.goldOldContainer
      << ",\"goldNewContainer\":" << r.goldNewContainer
      << ",\"goldBaseObj\":" << r.goldBaseObj;
    o << ",\"unitPriceSource\":"; Quoted(o, r.unitPriceSource);
    o << ",\"foregroundTerms\":"; Quoted(o, r.foregroundTerms);
    // w-probe-r10 (R10-2/R10-3): the attempt's own phase snapshot at gate
    // time, same encoding as js_w_phase.
    o << ",\"topmostMenu\":"; Quoted(o, r.topmostMenu);
    o << ",\"negativeTopmost\":" << (r.negativeTopmost ? "true" : "false");
    o << ",\"openMenus\":"; Quoted(o, r.openMenus);
    o << ",\"paused\":" << (r.paused ? "true" : "false");
    o << ",\"beforeSource\":"; Quoted(o, r.beforeSource);
    o << ",\"itemEvent\":" << (r.itemEvent ? "true" : "false")
      << ",\"goldEvent\":" << (r.goldEvent ? "true" : "false")
      << ",\"balanceDeltasValid\":" << (r.balanceDeltasValid ? "true" : "false")
      << ",\"balanceDeltas\":";
    if (r.balanceDeltasValid) {
        o << '[' << r.balanceDeltas[0] << ',' << r.balanceDeltas[1] << ','
          << r.balanceDeltas[2] << ',' << r.balanceDeltas[3] << ']';
    } else {
        o << "[null,null,null,null]";
    }
    o << ",\"inventoryDeltas\":";
    if (r.balanceDeltasValid) {
        o << '[' << r.inventoryDeltas[0] << ',' << r.inventoryDeltas[1] << ']';
    } else {
        o << "[null,null]";
    }
    o << ",\"deltaInvalidReason\":"; Quoted(o, r.deltaInvalidReason);
    // w-probe-r6 (W-3 ruling 3): the measured pairs the deltas above are
    // derived from. A value that was not measured is null, never a number.
    const auto measured = [&o](const std::array<std::int32_t, 4>& pair) {
        o << '[';
        for (unsigned i = 0; i < pair.size(); ++i) {
            if (i) o << ',';
            if (pair[i] >= 0) o << pair[i]; else o << "null";
        }
        o << ']';
    };
    o << ",\"measuredBefore\":"; measured(r.measuredBefore);
    o << ",\"measuredAfter\":"; measured(r.measuredAfter);
    o << ",\"measuredPairsValid\":" << (r.measuredPairsValid ? "true" : "false")
      << ",\"requestAgeMs\":" << r.requestAgeMs;
    o << ",\"itemEventBaseObj\":" << r.baseObj
      << ",\"numArg\":"; Number(o, r.numArg);
    o << ",\"senderFormID\":" << r.senderFormID
      << ",\"modEventSource\":" << (r.modEventSource ? "true" : "false")
      << ",\"oracleRequestId\":" << r.oracleRequestId
      << ",\"oracleDeferredRequest\":" << (r.oracleDeferredRequest ? "true" : "false");
    // The barter close row carries the per name totals so one session can be
    // read without joining it to the HEALTH line that follows it.
    if (r.nameCounters) {
        EmitNames(o, "traceExcludedByName", traceExcludedByName);
        EmitNames(o, "truncatedByName", truncatedByName);
        EmitNames(o, "statByName", statByName);
    }
    o << ",\"pendingObservations\":" << r.pendingObservations
      << ",\"msSinceInvalidationAtReport\":" << r.msSinceInvalidationAtReport;
    o << ",\"oracle\":"; EmitOracle(o, r.oracle);
    o << ",\"selected\":"; EmitSelected(o, r.selected);
    o << ",\"wObservation\":"; EmitWObservation(o, r.wObservation);
    o << ",\"wPhase\":"; EmitWPhase(o, r.wPhase);
    o << ",\"wArgs\":"; EmitWArgs(o, r.wArgs);
    o << ",\"perf\":"; EmitPerf(o, r.perf);
    // w-probe-r22 (R23-3): facts and new attempt evidence are append-only.
    o << ",\"factsReadOk\":" << (r.factsReadOk ? "true" : "false")
      << ",\"factsBaseObj\":" << r.factsBaseObj
      << ",\"factsMenuGeneration\":" << r.factsMenuGeneration
      << ",\"factsLoadGeneration\":" << r.factsLoadGeneration
      << ",\"factsInventoryGeneration\":" << r.factsInventoryGeneration
      << ",\"factsAgeMs\":" << r.factsAgeMs
      << ",\"factsOwnerForm\":" << r.factsOwnerForm
      << ",\"factsOwnerReadable\":" << (r.factsOwnerReadable ? "true" : "false")
      << ",\"settleRequestedMs\":" << r.settleRequestedMs
      << ",\"settlePublishedMs\":" << r.settlePublishedMs
      << ",\"settleDeferred\":" << (r.settleDeferred ? "true" : "false")
      << ",\"latchCleared\":" << (r.latchCleared ? "true" : "false")
      // w-probe-r27 (R28-1): the BarterMenu open row's own latch/queue
      // evidence; false on every other row kind.
      << ",\"queuedRunDropped\":" << (r.queuedRunDropped ? "true" : "false")
      << ",\"oracleBlockSource\":"; Quoted(o, r.oracleBlockSource);
    // w-probe-r24 (W-5, R25-4): append-only, every row kind (same rule as
    // the r21 facts block above); runToken/issueSource join a js_rule_run
    // and its js_rule_candidate rows to the attempts it issued.
    o << ",\"runToken\":" << r.runToken
      << ",\"issueSource\":"; Quoted(o, r.issueSource);
    if (std::strcmp(r.type, "js_rule_run") == 0) {
        const auto& verdict = r.ruleOracleVerdict;
        o << ",\"listEntries\":" << r.ruleListEntries
          << ",\"readUs\":" << r.ruleReadUs
          << ",\"readReason\":"; Quoted(o, r.ruleReadReason);
        o << ",\"oracleVerdict\":{\"lastSampleId\":" << verdict.lastSampleId
          << ",\"lastOrigin\":"; Quoted(o, verdict.lastOrigin);
        o << ",\"lastSampleValid\":" << (verdict.lastSampleValid ? "true" : "false")
          << ",\"lastInvalidReason\":"; Quoted(o, verdict.lastInvalidReason);
        o << ",\"lastTargeted\":" << (verdict.lastTargeted ? "true" : "false")
          << ",\"lastStageCostUs\":[";
        for (std::size_t i = 0; i < verdict.lastStageCostUs.size(); ++i) {
            if (i) o << ',';
            o << verdict.lastStageCostUs[i];
        }
        o << "],\"lastScanned\":" << verdict.lastScanned
          << ",\"lastChunks\":[";
        for (std::size_t i = 0; i < verdict.lastChunks.size(); ++i) {
            if (i) o << ',';
            o << verdict.lastChunks[i];
        }
        o << "],\"lastRestarts\":" << verdict.lastRestarts
          << ",\"lastBaseCountVisits\":" << verdict.lastBaseCountVisits
          << ",\"lastTruncatedStages\":" << verdict.lastTruncatedStages
          << ",\"lastUnsampled\":" << verdict.lastUnsampled
          << ",\"lastSpanUs\":" << verdict.lastSpanUs
          << ",\"menuSamples\":" << verdict.menuSamples
          << ",\"menuValid\":" << verdict.menuValid
          << ",\"menuInvalid\":{\"TRUNCATED\":" << verdict.menuInvalid[0]
          << ",\"UNSAMPLED_CANDIDATE\":" << verdict.menuInvalid[1]
          << ",\"INVENTORY_CHANGED\":" << verdict.menuInvalid[2]
          << ",\"STAGES_INCOMPLETE\":" << verdict.menuInvalid[3]
          << ",\"VISIT_CAP\":" << verdict.menuInvalid[4]
          << ",\"other\":" << verdict.menuInvalid[5]
          << "},\"cachedBeforeUsable\":" << (verdict.cachedBeforeUsable ? "true" : "false")
          << '}';
        o << ",\"candidatesRead\":" << r.ruleCandidatesRead
          << ",\"candidatesEligible\":" << r.ruleCandidatesEligible
          // w-probe-r33 (R38-1): how many candidates passed every exclusion
          // except `run_limit` (= selected + run_limit-excluded). It answers
          // the R-W24d question candidatesEligible could not; RULE-16
          // reconciles it with the rows the run left (issued + pre-dispatch
          // refusals + run_limit), an equality only for a COMPLETED run.
          << ",\"candidatesSelectable\":" << r.ruleCandidatesSelectable
          << ",\"excludedByTerm\":{";
        for (std::size_t i = 0; i < CandidateExclusionTerms.size(); ++i) {
            if (i) o << ',';
            Quoted(o, CandidateExclusionTerms[i]);
            o << ':' << r.ruleCandidateRows.excludedByTerm[i];
        }
        o << "},\"candidateRowsSuppressed\":" << r.ruleCandidateRows.candidateRowsSuppressed
          << ",\"excludedNotDisplayed\":" << r.ruleExcludedNotDisplayed
          << ",\"excludedNoQuote\":" << r.ruleExcludedNoQuote
          << ",\"excludedIdentityUnresolved\":" << r.ruleExcludedIdentityUnresolved
          << ",\"excludedFactsUnreadable\":" << r.ruleExcludedFactsUnreadable
          << ",\"issued\":" << r.ruleIssued
          << ",\"confirmed\":" << r.ruleConfirmed
          << ",\"capReached\":" << (r.ruleCapReached ? "true" : "false")
          << ",\"runReason\":"; Quoted(o, r.ruleRunReason);
        o << ",\"attemptTerminals\":"; Quoted(o, r.ruleAttemptTerminals);
        o << ",\"terminalReasons\":"; Quoted(o, r.ruleTerminalReasons);
        o << ",\"healthDropped\":" << r.ruleHealthDropped
          // w-probe-r26 (R27d-2/R27d-3): where this run's balance came from,
          // its value (-1 unknown), and how many inventory generations passed
          // between the chosen sample and the run (-1 when there was none).
          << ",\"balanceSource\":"; Quoted(o, r.ruleBalanceSource);
        o << ",\"balanceValue\":" << r.ruleBalanceValue
          << ",\"balanceInventoryDelta\":" << r.ruleBalanceInventoryDelta
          // w-probe-r27 (R28-1): the one gate reason this run was decided
          // with ("OK" when it passed) and the edges the sink dropped while
          // the run was live. A refused run writes 0 here and says why on
          // gateReason; the session total is on HEALTH.edgesDropped.
          << ",\"gateReason\":"; Quoted(o, r.ruleGateReason);
        o << ",\"edgesDroppedDuringRun\":" << r.ruleEdgesDroppedDuringRun
          // w-probe-r28 (R29-1/R29-3): the settings verdict of this run's
          // snapshot. RULE-12: a SETTINGS_OFF run has issued 0 and
          // candidatesRead 0 and names one of the six fixed tokens here.
          << ",\"settingsReason\":"; Quoted(o, r.ruleSettingsReason);
        o << ",\"runSummary\":{\"mutations\":" << mutations.load()
          << ",\"taskP99Us\":" << taskMeter.P99()
          << ",\"taskMaxUs\":" << taskMeter.max.load()
          << ",\"taskOverruns\":" << taskMeter.overruns.load()
          << ",\"oracleDeferred\":" << oracleDeferred.load()
          << ",\"oracleAborted\":" << oracleAborted.load()
          << ",\"taskThreadId\":" << taskThreadId.load()
          << ",\"uiThreadId\":" << uiThreadId.load()
          << ",\"hookThreadId\":" << hookThreadId.load()
          << ",\"stages\":{\"sink\":{\"p99Us\":" << sinkMeter.P99()
          << ",\"maxUs\":" << sinkMeter.max.load()
          << ",\"overruns\":" << sinkMeter.overruns.load() << '}';
        EmitRunSummaryMeter(o, "hook", hookMeter);
        EmitRunSummaryMeter(o, "task", taskMeter);
        EmitRunSummaryMeter(o, "format", formatMeter);
        EmitRunSummaryMeter(o, "prepare", prepareMeter);
        EmitRunSummaryMeter(o, "formScan", formScanMeter);
        EmitRunSummaryMeter(o, "xListScan", xListScanMeter);
        o << "}}";
    }
    if (std::strcmp(r.type, "js_batch") == 0) EmitBatchBlock(o, r);
    if (std::strcmp(r.type, "js_convert_run") == 0) EmitConvertBlock(o, r);
    // w-probe-r28 (R29-1/R29-2): the js_settings_apply row. `reason` and
    // `configGeneration` come from the common block above (the row carries
    // one of each); this branch adds the per-key apply evidence only.
    if (std::strcmp(r.type, "js_settings_apply") == 0) {
        o << ",\"key\":"; Quoted(o, r.settingsApplyKey);
        o << ",\"value\":" << (r.settingsApplyValue ? "true" : "false")
          << ",\"accepted\":" << (r.settingsApplyAccepted ? "true" : "false")
          << ",\"saved\":" << (r.settingsApplySaved ? "true" : "false")
          << ",\"readbackOk\":" << (r.settingsApplyReadbackOk ? "true" : "false");
    }
    if (std::strcmp(r.type, "js_stop") == 0) {
        o << ",\"pendingBefore\":" << r.stopPendingBefore
          << ",\"pendingAfter\":" << r.stopPendingAfter
          << ",\"cancelled\":" << r.stopCancelled
          << ",\"idleUs\":" << r.stopIdleUs
          << ",\"idle\":" << (r.stopIdle ? "true" : "false")
          << ",\"latched\":" << (r.stopLatched ? "true" : "false");
    }
    // w-probe-r71 (v0.9.0): the Q4 proof suffix, js_startup off 1.6.1170
    // only (empty everywhere else, so every other row keeps its bytes).
    if (r.alProof[0] && std::strcmp(r.type, "js_startup") == 0) {
        std::array<char, AlProofBytes> proof = r.alProof;
        proof[AlProofBytes - 1] = '\0';
        o << proof.data();
    }
    o << "}\n";
}
void EmitMeter(std::ostream& o, const char* name, const Meter& meter) {
    o << ",\"" << name << "\":{\"samples\":" << meter.samples.load()
      << ",\"p99Us\":" << meter.P99() << ",\"maxUs\":" << meter.max.load()
      << ",\"limitUs\":" << meter.Limit() << ",\"overruns\":" << meter.overruns.load() << '}';
}
void EmitRunSummaryMeter(std::ostream& o, const char* name, const Meter& meter) {
    o << ",\"" << name << "\":{\"p99Us\":" << meter.P99()
      << ",\"maxUs\":" << meter.max.load()
      << ",\"overruns\":" << meter.overruns.load() << '}';
}
[[maybe_unused]] void EmitHealth(std::ostream& o, std::uint64_t ms) {
    o << "{\"schema\":1,\"revision\":";
    Quoted(o, Revision);
    o << ",\"type\":\"js_health\",\"role\":\"HEALTH-06\",\"reason\":\"HEALTH\",\"ms\":" << ms
      << ",\"mutation\":\"none\",\"callbackInvocation\":false"
      << ",\"mutations\":" << mutations.load()
      << ",\"drops\":" << drops.load()
      << ",\"budgetOverruns\":" << budgetOverruns.load()
      << ",\"wrongThreadCount\":" << wrongThreadCount.load()
      << ",\"truncated\":" << truncated.load()
      << ",\"traceExcluded\":" << traceExcluded.load()
      << ",\"taskThreadId\":" << taskThreadId.load()
      << ",\"uiThreadId\":" << uiThreadId.load()
      << ",\"hookThreadId\":" << hookThreadId.load()
      << ",\"callbackTruncated\":" << callbackTruncated.load()
      << ",\"ringDrops\":" << ringDrops.load()
      << ",\"queueFailures\":" << queueFailures.load()
      // w-probe-r27 (R28-1): the session total of hotkey edges the input sink
      // dropped under the pending<=1 rule (per-run deltas on js_rule_run).
      << ",\"edgesDropped\":" << edgesDropped.load()
      << ",\"hookExceptions\":" << hookExceptions.load()
      << ",\"rotations\":" << rotations.load()
      << ",\"epochStale\":" << epochStale.load()
      << ",\"registerAttempts\":" << registerAttempts.load()
      << ",\"perfLatches\":" << perfLatches.load()
      << ",\"oracleTruncations\":" << oracleTruncations.load()
      << ",\"oracleDeferred\":" << oracleDeferred.load()
      << ",\"oracleAborted\":" << oracleAborted.load()
      << ",\"continuations\":" << continuations.load()
      << ",\"lastTaskMs\":" << writer->lastTaskMs.load()
      << ",\"attempted\":" << writer->attempted.load()
      << ",\"accepted\":" << writer->accepted.load()
      << ",\"dropped\":" << writer->dropped.load()
      << ",\"ioFailed\":" << writer->ioFailed.load()
      << ",\"written\":" << writer->written.load();
    // w-probe-r1 (design section 4.3). Every counter below is printed with the
    // denominator it belongs to, because "0 of 0" is not a pass.
    const auto delivery = CurrentDelivery();
    o << ",\"processRunId\":"; Quoted(o, processRun);
    o << ",\"generated\":" << delivery.generated
      << ",\"delivered\":" << delivery.delivered
      << ",\"pending\":" << delivery.pending
      << ",\"fileDropped\":" << delivery.fileDropped
      << ",\"deliveryConsistent\":" << (DeliveryConsistent(delivery) ? "true" : "false")
      << ",\"dispatchCount\":" << dispatchCount.load()
      << ",\"hookCallCount\":" << hookCallCount.load()
      << ",\"originalPassCount\":" << originalPassCount.load()
      << ",\"manualItemSelectCount\":" << manualItemSelectCount.load()
      << ",\"reentryCount\":" << reentryCount.load()
      << ",\"observationCount\":" << observationCount.load()
      << ",\"observationRequests\":" << observationRequests.load()
      << ",\"staleObservationCount\":" << staleObservationCount.load()
      << ",\"pendingObservationTasks\":" << pendingObservationTasks.load()
      << ",\"argsShapeMatchCount\":" << argsShapeMatchCount.load()
      << ",\"argsRejectedCount\":" << argsRejectedCount.load()
      << ",\"argsUnknownCount\":" << argsUnknownCount.load()
      << ",\"fundsKnownCount\":" << fundsKnownCount.load()
      << ",\"fundsInsufficientCount\":" << fundsInsufficientCount.load()
      << ",\"fundsUnknownCount\":" << fundsUnknownCount.load()
      << ",\"sampleCount\":" << sampleCount.load()
      << ",\"stableSampleCount\":" << stableSampleCount.load()
      << ",\"invalidSampleCount\":" << invalidSampleCount.load()
      << ",\"phaseUnknownCount\":" << phaseUnknownCount.load()
      << ",\"readPhaseUnprovenCount\":" << readPhaseUnprovenCount.load()
      << ",\"loadGeneration\":" << loadGeneration.load()
      << ",\"menuGeneration\":" << menuGeneration.load()
      << ",\"inventoryGeneration\":" << inventoryGeneration.load()
      << ",\"lastEventSeq\":" << lastEventSeqSeen.load()
      << ",\"entryVisits\":" << entryVisits.load()
      << ",\"xListVisits\":" << xListVisits.load()
      << ",\"oracleEntryVisits\":" << oracleEntryVisits.load()
      << ",\"visitCapHits\":" << visitCapHits.load()
      << ",\"visitAllowanceShort\":" << visitAllowanceShort.load()
      // w-probe-r8 (R8-2): one configured cap bounds all three walks, and all
      // three are still printed beside the visits they bound.
      << ",\"entryVisitCap\":" << configuredVisitCap.load()
      << ",\"xListVisitCap\":" << configuredVisitCap.load()
      // r3: oracleEntryVisits stays the process wide total. r56: the
      // cap printed beside it is the per session budget only (no process
      // valve), which travels on every row as perf.oracleSessionVisits.
      << ",\"oracleEntryVisitCap\":" << configuredVisitCap.load();
    EmitNames(o, "traceExcludedByName", traceExcludedByName);
    EmitNames(o, "truncatedByName", truncatedByName);
    EmitNames(o, "statByName", statByName);
    EmitMeter(o, "sink", sinkMeter);
    EmitMeter(o, "hook", hookMeter);
    EmitMeter(o, "task", taskMeter);
    EmitMeter(o, "format", formatMeter);
    EmitMeter(o, "prepare", prepareMeter);
    EmitMeter(o, "formScan", formScanMeter);
    EmitMeter(o, "xListScan", xListScanMeter);
    o << "}\n";
}
}
std::uint64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::uint64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::uint64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool WriteRow(std::ostream& out, const Row& row) {
    Emit(out, row);
    out.flush();
    return static_cast<bool>(out);
}

namespace {
// w-probe-r14/r36: opens one candidate receipt for append. The writer owns
// the startup append and rotation check. The caller learns which step refused instead of
// getting a bare false, because R-W8 and R-W9 measured exactly this refusal
// with no trace anywhere.
bool OpenReceipt(const std::filesystem::path& path, const char** stage) {
    std::ofstream initial(path, std::ios::binary | std::ios::app);
    if (!initial) { *stage = "RECEIPT_OPEN_REFUSED"; return false; }
    initial.imbue(std::locale::classic()); initial << std::setprecision(9);
    *stage = "none";
    return true;
}

std::string Serialize(const Row& row) {
    std::ostringstream text;
    text.imbue(std::locale::classic()); text << std::setprecision(9);
    Emit(text, row);
    return text.str();
}

bool RotateBeforeAppend(std::ofstream& out, const std::filesystem::path& path,
                        std::uint64_t nextBytes, std::uint64_t rotateBytes,
                        bool fallback) {
    // The size is read from the file, never from the stream position: on MSVC a stream
    // opened with std::ios::app reports position 0 until its first write, so
    // a 21 MiB primary looked empty at startup and was never rotated
    // (R-V001-01 run 1, a live test log). The stream is
    // flushed after every append, so the on-disk size is the current size.
    if (!out) return false;
    std::error_code sizeError;
    const auto measured = std::filesystem::file_size(path, sizeError);
    const std::uint64_t current = sizeError ? 0 : static_cast<std::uint64_t>(measured);
    if (current <= rotateBytes && nextBytes <= rotateBytes - current) return true;
    out.close();
    auto previous = path; previous += ".1";
    std::error_code code;
    std::filesystem::remove(previous, code);
    std::filesystem::rename(path, previous, code);
    if (code) return false;
    out.open(path, std::ios::binary | std::ios::trunc);
    out.imbue(std::locale::classic()); out << std::setprecision(9);
    if (!out) return false;
    Row rotation;
    rotation.type = "js_rotation";
    rotation.reason = "RECEIPT_ROTATED";
    rotation.origin = "writer";
    rotation.ms = NowMs();
    rotation.receiptFallback = fallback;
    rotation.rotateBytes = rotateBytes;
    rotation.retainedGenerations = RetainedGenerations;
    rotation.rotatedBytes = current;
    rotation.generation = RetainedGenerations;
    CopyText(rotation.rotatedFrom, path.filename().string().c_str());
    const auto marker = Serialize(rotation);
    out << marker;
    if (!out) return false;
    ++rotations;
    return true;
}

bool AppendWithRotation(std::ofstream& out, const std::filesystem::path& path,
                        const Row& row, std::uint64_t rotateBytes, bool fallback) {
    const auto text = Serialize(row);
    if (!RotateBeforeAppend(out, path, text.size(), rotateBytes, fallback)) return false;
    out << text;
    out.flush();
    return static_cast<bool>(out);
}

// w-probe-r71 (v0.9.0): one shared-gate line (already one JSON object
// plus its newline) under the same rotation rule as a row.
bool AppendRawWithRotation(std::ofstream& out, const std::filesystem::path& path,
                           const std::string& text, std::uint64_t rotateBytes, bool fallback) {
    if (!RotateBeforeAppend(out, path, text.size(), rotateBytes, fallback)) return false;
    out << text;
    out.flush();
    return static_cast<bool>(out);
}

// "<stem>.<processRunId>.jsonl", built from ASCII bytes only. The directory
// part is reused as a path object, so a non ASCII directory is never converted
// to a narrow string here (a conversion that can throw on Windows).
std::filesystem::path FallbackReceipt(const std::filesystem::path& path, std::string& name) {
    name = std::string(FallbackReceiptStem) + "." + processRun.data() + ".jsonl";
    return path.parent_path() / name;
}
}

void SetDiagnosticDirectory(const std::filesystem::path& directory) noexcept {
    try { *diagnosticDirectory = directory; } catch (...) {}
}

void ReportStartupFailure(const char* stage, const char* detail) noexcept {
    try {
        if (receiptReady.load()) {
            Row row;
            row.type = "js_startup_diagnostic";
            row.origin = "startup";
            row.reason = stage ? stage : "unknown";
            CopyText(row.text, detail ? detail : "none");
            Log(row);
            return;
        }
        if (diagnosticDirectory->empty()) return;
        std::ofstream note(*diagnosticDirectory / StartupNoteBasename,
                           std::ios::binary | std::ios::app);
        if (!note) return;
        note.imbue(std::locale::classic());
        note << "{\"type\":\"js_startup_diagnostic\",\"revision\":";
        Quoted(note, Revision);
        note << ",\"processRunId\":"; Quoted(note, processRun.data());
        note << ",\"stage\":"; Quoted(note, stage ? stage : "unknown");
        note << ",\"detail\":"; Quoted(note, detail ? detail : "none");
        note << ",\"ms\":" << NowMs() << "}\n";
        note.flush();
    } catch (...) {}
}

// audit B-01: one shared-gate line (the only diagnostic of a
// refused or failed start). A refused append on the current receipt switches
// to the per-process fallback receipt (useFallback) and retries once there, the
// same recovery the row drain of OpenLog's worker gives a row. Returns whether
// the line was written. Host-tested with fakes (tests/js_e1_test.cpp).
template <class Append, class UseFallback>
bool DrainRawLine(const std::string& text, bool fallback, Append&& append, UseFallback&& useFallback) {
    if (append(text, fallback)) return true;
    return useFallback() && append(text, true);
}

bool OpenLog(const std::filesystem::path& path, Row startup, std::filesystem::path* usedPath) {
    // R13-1/r36: startup is a durable boundary. The worker writes and flushes
    // it before resolving this call, so it is both durable and writer-owned.
    // R14-2: a refused primary receipt is recorded and retried once on a per
    // process file, so an unwritable or locked receipt costs one row of
    // provenance instead of the whole run's evidence.
    startup.rotateBytes = RotateBytes;
    startup.retainedGenerations = RetainedGenerations;
    startup.receiptFallback = false;
    receiptFallback = false;
    std::promise<bool> opened;
    auto result = opened.get_future();
    auto actualPath = std::make_shared<std::filesystem::path>();
    std::thread worker([primary = path, startup, actualPath, promise = std::move(opened)]() mutable {
        auto target = primary;
        std::ofstream out;
        bool fallback = false;
        const auto open = [&](const std::filesystem::path& candidate) {
            const char* stage = "none";
            if (!OpenReceipt(candidate, &stage)) {
                ReportStartupFailure(stage, ReceiptBasename);
                return false;
            }
            out.open(candidate, std::ios::binary | std::ios::app);
            out.imbue(std::locale::classic()); out << std::setprecision(9);
            return static_cast<bool>(out);
        };
        const auto useFallback = [&]() {
            if (fallback) return false;
            out.close();
            std::string name;
            target = FallbackReceipt(primary, name);
            fallback = true;
            receiptFallback = true;
            CopyText(startup.receiptBasename, name.c_str());
            startup.receiptFallback = true;
            if (!open(target) || !AppendWithRotation(out, target, startup, RotateBytes, true)) {
                ReportStartupFailure("STARTUP_WRITE_REFUSED", name.c_str());
                return false;
            }
            ReportStartupFailure("RECEIPT_FALLBACK_USED", name.c_str());
            return true;
        };
        if (!open(target) || !AppendWithRotation(out, target, startup, RotateBytes, false)) {
            ReportStartupFailure("STARTUP_WRITE_REFUSED", ReceiptBasename);
            if (!useFallback()) { promise.set_value(false); return; }
        }
        *actualPath = target;
        ++writer->attempted;
        ++writer->accepted;
        ++writer->written;
        promise.set_value(true);
        for (;;) {
            if (auto pump = writer->pump.load()) pump();
            std::array<char, RawLineBytes> line{};
            while (writer->PopRaw(line)) {
                line[RawLineBytes - 1] = '\0';
                const std::string text = std::string(line.data()) + "\n";
                // audit B-01: a refused append moves to the fallback
                // receipt and retries once, exactly as a row does below.
                const bool written = DrainRawLine(
                    text, fallback,
                    [&](const std::string& t, bool fb) { return AppendRawWithRotation(out, target, t, RotateBytes, fb); },
                    useFallback);
                if (written) ++writer->written; else ++writer->ioFailed;
            }
            Row row;
            for (unsigned i = 0; i < 64 && writer->Pop(row); ++i) {
                const auto start = NowNs();
                row.receiptFallback = fallback;
                row.rotateBytes = RotateBytes;
                row.retainedGenerations = RetainedGenerations;
                bool written = AppendWithRotation(out, target, row, RotateBytes, fallback);
                if (!written && useFallback()) {
                    row.receiptFallback = true;
                    written = AppendWithRotation(out, target, row, RotateBytes, true);
                }
                formatMeter.Observe((NowNs() - start) / 1000);
                if (written) ++writer->written; else ++writer->ioFailed;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    const bool success = result.get();
    if (success) receiptReady = true;
    if (success && usedPath) *usedPath = *actualPath;
    if (success) worker.detach(); else worker.join();
    // R14-2: the startup row is already durable here, so a worker that cannot
    // open the same file is the one remaining silent failure. Name it.
    if (!success) ReportStartupFailure("WORKER_OPEN_REFUSED", ReceiptBasename);
    return success;
}
bool AppendReceiptForTest(const std::filesystem::path& path, const Row& row,
                          std::uint64_t rotateBytes) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) return false;
    return AppendWithRotation(out, path, row, rotateBytes, false);
}
bool ReceiptFallback() noexcept { return receiptFallback.load(); }
bool LogRawLine(const std::string& json) noexcept {
    try {
        if (json.size() + 1 > RawLineBytes) { ++writer->rawDropped; return false; }
        std::lock_guard lock(writer->mutex);
        if (writer->rawCount >= writer->raw.size()) { ++writer->rawDropped; return false; }
        auto& slot = writer->raw[writer->rawHead];
        slot.fill('\0');
        json.copy(slot.data(), json.size());
        writer->rawHead = (writer->rawHead + 1) % writer->raw.size();
        ++writer->rawCount;
        ++writer->accepted;
        return true;
    } catch (...) {
        ++writer->rawDropped;
        return false;
    }
}
bool AppendRawLine(const std::filesystem::path& path, const std::string& json) noexcept {
    try {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        if (!out) return false;
        out << json << '\n';
        out.flush();
        return static_cast<bool>(out);
    } catch (...) {
        return false;
    }
}
void Log(const Row& row) noexcept {
    static constexpr std::array<const char*, 14> ProductTypes{
        "js_rule_run", "js_rule_candidate", "js_sale_attempt",
        "js_settings_apply", "js_stop", "js_warning", "js_startup",
        "js_rotation", "js_startup_diagnostic", "js_batch", "js_batch_row",
        "js_convert_run", "js_convert_row", "js_frame_window"};
    bool retained = false;
    for (const char* type : ProductTypes) retained |= std::strcmp(row.type, type) == 0;
    if (!retained) return;
    ++writer->attempted;
    try {
        // heavy-fix (w-probe-r56): a run-terminal row (js_rule_run,
        // js_batch) waits for the lock -- the writer holds it only to copy one
        // row out in Pop -- and may use the reserved slots; every other row
        // keeps the non-blocking try_to_lock and stops short of the reserve.
        const bool reserved = LogRowUsesReserve(row.type);
        std::unique_lock lock(writer->mutex, std::defer_lock);
        if (reserved) lock.lock();
        else lock = std::unique_lock(writer->mutex, std::try_to_lock);
        if (!lock || writer->count >= LogRowAdmitLimit(reserved)) { ++writer->dropped; ++drops; return; }
        writer->rows[writer->head] = row;
        // R21-4: the stamp lands on the ring copy, never on the caller's row
        // (the caller only ever holds a const reference now).
        writer->rows[writer->head].ms = NowMs();
        writer->head = (writer->head + 1) % writer->rows.size();
        ++writer->count; ++writer->accepted;
    } catch (...) { ++writer->dropped; ++drops; }
}
void MarkTask(std::uint64_t ms) noexcept { writer->lastTaskMs = ms; }
void SetPump(void (*pump)()) noexcept { writer->pump = pump; }
// "<startEpochMs>-<pid>". A fixed length field, formatted by hand so this file
// needs neither Windows nor a formatting library, written once before the log
// opens and only read afterwards.
void SetProcessRun(std::uint64_t processId, std::uint64_t startEpochMs) noexcept {
    std::array<char, 32> text{};
    const auto append = [&text](std::size_t at, std::uint64_t value) noexcept {
        char digits[24]{};
        unsigned length = 0;
        do { digits[length++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
        while (length && at + 1 < text.size()) text[at++] = digits[--length];
        return at;
    };
    std::size_t next = append(0, startEpochMs);
    if (next + 1 < text.size()) text[next++] = '-';
    next = append(next, processId);
    text[next < text.size() ? next : text.size() - 1] = '\0';
    processRun = text;
}
const char* ProcessRunId() noexcept { return processRun.data(); }
Delivery CurrentDelivery() noexcept {
    Delivery delivery;
    delivery.generated = writer->attempted.load();
    delivery.accepted = writer->accepted.load();
    delivery.dropped = writer->dropped.load();
    delivery.delivered = writer->written.load();
    const auto accepted = delivery.accepted, delivered = delivery.delivered;
    delivery.pending = accepted > delivered ? accepted - delivered : 0;
    delivery.fileDropped = writer->ioFailed.load();
    return delivery;
}
}
