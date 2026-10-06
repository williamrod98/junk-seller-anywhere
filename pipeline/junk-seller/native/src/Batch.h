#pragma once
// (the r2 design, stage S2a): the engine-free half of the
// v0.0.4 one-press batch sale inside the BarterMenu (approach A).
//
//   * BuildBatchPlan    -- the selector's rows as one ordered plan: whole
//                          sellable stacks (D1), the running merchant-gold
//                          cut (2.6), the 256-row cap (D2), one token per row.
//   * ApplyPressRowCap  -- r58: at most PressRowCap(list size) rows
//                          per press, so one press stays about 1-1.5 s on a
//                          heavy list (the engine pays per sold row).
//   * RunCommitTask     -- ONE call is ONE UI task: it dispatches planned rows
//                          back to back through a host interface, checks the
//                          250 ms frame cap before each next row (at least one
//                          row per task) and, when the cap is reached, returns
//                          kSpill with the next row saved. It never queues
//                          anything itself; the caller's pump runs the next
//                          task on the next frame (2.3).
//   * row states        -- Planned -> Dispatched -> Confirmed / Unconfirmed,
//                          never back (2.8). An Unconfirmed row latches.
//   * Reconcile          -- the after-sample check of 2.5.
//   * ClassifyBatchOutcome -- exactly one result line per press (2.10).
//   * ShadowPriceMultiplier -- the receipt-only formula quote of 3.6. It is
//                          never read by the plan, the commit loop or a gate.
//
// Nothing here includes an engine header or touches an RE object. The
// engine side (Probe.cpp, stage S2c) implements BatchHost.
#include "Selector.h"
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace js {

// Design-r2 2.3: the commit task's own frame cap (not the house 2 ms rule;
// the game is paused for the whole BarterMenu session).
// r56 (design ruling 2026-09-29 on heavy-fix decision 1): 250 ms, was 80 ms.
// The r56 reason ("the engine re-lists the whole BarterMenu once per commit
// frame, so one frame of 100 rows costs one ~1 s hitch") is REFUTED by L1b
// r57 (design ruling 2026-09-29): on the 1,756-entry list one commit
// frame of 43 rows (252 ms of task time) was followed by one 23,864 ms frame,
// about 555 ms of engine work per sold row. The engine pays per sale, not per
// commit frame, so this cap bounds only our own task time; what bounds the
// freeze the player feels is the per-press row cap below (PressRowCap).
inline constexpr std::uint64_t CommitFrameCapUs = 250000;
// Design-r2 2.3 batch watchdog: rows, commit frames, wall time from the edge.
inline constexpr std::size_t BatchMaxRows = SelectorMaxTransactions;

// ---------------------------------------------------------------------------
// w-probe-r58 (design ruling 2026-09-29): the per-press row cap.
// The engine pays a cost per sold row that grows with the size of the
// displayed list (measured live, press to the next drawn frame):
//   * L1a r6, 150-entry list: 30 rows, 61 ms task + 701 ms to the gold
//     change -> about 23.4 ms per row;
//   * L1b r57, 1,756-entry list: 43 rows, 252 ms task + one 23,864 ms frame
//     -> about 556 ms per row (r56: 1 row, one 772 ms frame; the vanilla
//     one-unit hand sale on the same list: one 1,021 ms frame).
// Model (PressRowCostUs): 156 us per entry up to 150 entries, above that the
// straight line through both measured points, 23.4 ms + 331.6 us per entry
// beyond 150. Only the two end points are measured, and they are not the same
// quantity (L1a: press to the visible gold change; L1b: the long frame). The
// line between them is an ASSUMPTION: it lies above a power-law curve through
// the same points (about N^1.3), so it is conservative only if the real
// growth is of that kind; no list size in between has been measured.
// One press may plan PressCostBudgetUs / cost rows (at least 1, at most
// BatchMaxRows): 51 rows at 150 entries (the L1a r6 case sells all 30), 16 at
// 300, 3 at 1,000, 2 at 1,756 (predicted, not measured: about 1.1 s by the
// model; the r56 one-row frame of 772 ms and the vanilla one-unit hand sale
// of 1,021 ms suggest 1.3-1.6 s), 1 above about 1,880 entries. The rows beyond
// the cap stay in the inventory; the result line says how many units were
// sold and to press again (BatchReason::kLimit). No other speed work
// (v0.1.0 direct conversion is the main path for a heavy inventory).
inline constexpr std::uint64_t PressCostBudgetUs = 1200000;
inline constexpr std::size_t PressCostAnchorEntries = 150;
inline constexpr std::uint64_t PressCostAnchorRowUs = 23400;
inline constexpr std::uint64_t PressCostAnchorPerEntryUs = 156;
// 331.6 us per entry above the anchor, in tenths of a microsecond.
inline constexpr std::uint64_t PressCostSlopeTenthUsPerEntry = 3316;
// Larger lists are costed as this many entries (no overflow; the cap is 1
// long before it).
inline constexpr std::size_t PressCostMaxEntries = 1000000;

inline std::uint64_t PressRowCostUs(std::size_t listEntries) noexcept {
    const std::size_t entries = listEntries > PressCostMaxEntries ? PressCostMaxEntries : listEntries;
    if (entries <= PressCostAnchorEntries) return static_cast<std::uint64_t>(entries) * PressCostAnchorPerEntryUs;
    return PressCostAnchorRowUs +
           static_cast<std::uint64_t>(entries - PressCostAnchorEntries) * PressCostSlopeTenthUsPerEntry / 10;
}

// Rows one press may sell on a list of `listEntries` displayed entries.
inline std::size_t PressRowCap(std::size_t listEntries) noexcept {
    const std::uint64_t cost = PressRowCostUs(listEntries);
    if (cost == 0) return BatchMaxRows;
    const std::uint64_t cap = PressCostBudgetUs / cost;
    if (cap < 1) return 1;
    if (cap > BatchMaxRows) return BatchMaxRows;
    return static_cast<std::size_t>(cap);
}
inline constexpr std::uint32_t BatchMaxCommitFrames = 4;
inline constexpr std::uint64_t BatchWatchdogUs = 30000000;
// Design-r2 2.3: rows whose events were not delivered inside the call are
// decided after the task, within this cap, or they are Unconfirmed.
inline constexpr std::uint64_t BatchDeferredVerifyCapUs = 250000;
// bindfix2: extra binds of the same row after a read-back that the list
// UI moved (BatchHost::LastBindSelectionMoved). See RunCommitTask.
inline constexpr std::uint32_t BatchBindRewriteMax = 2;

// The identity of the BarterMenu ItemList a plan was built on: the `items`
// data pointer and size, as integers (never dereferenced here).
struct BatchListIdentity {
    std::uintptr_t data{};
    std::size_t size{};
};

inline bool SameBatchList(const BatchListIdentity& left, const BatchListIdentity& right) noexcept {
    return left.data == right.data && left.size == right.size;
}

// One displayed row as the host reads it: the Item pointer (as an integer)
// and the base form of its objDesc.
struct BatchListEntry {
    std::uintptr_t item{};
    std::uint32_t baseFormKey{};
};

// Where the selector input record i sits in the displayed list. The caller
// passes one source per selector input record, in the same order.
struct BatchRowSource {
    std::size_t listIndex{};
    std::uintptr_t item{};
    std::uint32_t baseFormKey{};
    std::size_t sourceIndex{};
};

enum class BatchRowState : unsigned { kPlanned, kDispatched, kConfirmed, kUnconfirmed };

struct BatchRow {
    std::uint32_t token{};
    std::size_t listIndex{};
    std::size_t sourceIndex{};
    std::uintptr_t item{};
    std::uint32_t baseFormKey{};
    std::int32_t stackCount{};
    std::int32_t quantity{};
    std::int32_t unitPrice{};
    std::int64_t total{};
    // The item's base value V (CandidateRecord::basePrice), for the shadow
    // quote only.
    std::int32_t baseValue{};
    // Receipt-only shadow quote (3.6); -1 = not computed.
    std::int64_t shadowPrice{-1};
    BatchRowState state{BatchRowState::kPlanned};
    // r57: the row's own item and gold events both arrived inside the
    // call (one or more of each; was "exactly one of each" before r57).
    bool pairInCall{};
    bool listRebuiltInCall{};
    // r57: the event counts the row was decided on (receipt only).
    std::uint32_t itemEvents{};
    std::uint32_t goldEvents{};
    std::uint32_t foreignEvents{};
};

struct BatchPlan {
    bool valid{};
    BatchListIdentity list{};
    std::vector<BatchRow> rows{};
    std::int64_t unitsEligible{};
    std::int64_t unitsPlanned{};
    std::int64_t goldPlanned{};
    bool balanceLimited{};
    bool runLimited{};
    bool balanceUnknown{};
    // Receipt-only (3.6): the shadow multiplier was computed, and whether the
    // player owns a merchant-conditioned perk (those rows are excluded from
    // the 1% rule by the reader).
    bool shadowKnown{};
    bool shadowMerchantPerk{};
    double shadowMultiplier{};
    // r58: the per-press row cap on this list (ApplyPressRowCap), the
    // rows the selector chose before it, and whether it cut the plan.
    std::size_t pressRowCap{};
    std::size_t rowsSelected{};
    bool pressCapped{};
};

// Builds the ordered plan from the selector's run. `records` and `sources`
// are the selector's inputs in the same order. A plan is invalid when the
// inputs disagree in size, a selected row has no usable quantity, or the run
// selected more rows than the batch may dispatch.
inline BatchPlan BuildBatchPlan(std::span<const CandidateMerchantRecord> records,
                                const SelectionRun& selection,
                                std::span<const BatchRowSource> sources,
                                const BatchListIdentity& list) {
    BatchPlan plan;
    plan.list = list;
    plan.unitsEligible = selection.unitsEligible;
    plan.balanceLimited = selection.balanceLimited;
    plan.runLimited = selection.runLimited;
    plan.balanceUnknown = selection.balanceUnknown;
    if (records.size() != sources.size() || selection.results.size() != records.size() ||
        selection.selectedCount != selection.selectedIndices.size() ||
        selection.selectedCount > BatchMaxRows) {
        return plan;
    }
    plan.rows.reserve(selection.selectedCount);
    for (const std::size_t index : selection.selectedIndices) {
        if (index >= records.size()) return BatchPlan{};
        const CandidateResult& result = selection.results[index];
        if (result.decision != SelectorDecision::kSelect || result.quantity <= 0 ||
            result.unitPrice <= 0 || result.quantity > records[index].candidate.count) {
            return BatchPlan{};
        }
        BatchRow row;
        row.token = static_cast<std::uint32_t>(plan.rows.size() + 1);
        row.listIndex = sources[index].listIndex;
        row.sourceIndex = sources[index].sourceIndex;
        row.item = sources[index].item;
        row.baseFormKey = sources[index].baseFormKey;
        row.stackCount = records[index].candidate.count;
        row.quantity = result.quantity;
        row.unitPrice = result.unitPrice;
        row.total = static_cast<std::int64_t>(result.quantity) * result.unitPrice;
        row.baseValue = records[index].candidate.basePrice;
        plan.unitsPlanned += row.quantity;
        plan.goldPlanned += row.total;
        plan.rows.push_back(row);
    }
    plan.valid = true;
    return plan;
}

// r58: keeps the first PressRowCap(list size) rows of a valid plan (the
// selector's order) and recomputes the planned units and gold from them. The
// cut rows are not sold by this press; they stay in the inventory for the
// next one.
inline void ApplyPressRowCap(BatchPlan& plan) {
    plan.pressRowCap = PressRowCap(plan.list.size);
    plan.rowsSelected = plan.rows.size();
    plan.pressCapped = false;
    if (!plan.valid || plan.rows.size() <= plan.pressRowCap) return;
    plan.rows.resize(plan.pressRowCap);
    plan.unitsPlanned = 0;
    plan.goldPlanned = 0;
    for (const auto& row : plan.rows) {
        plan.unitsPlanned += row.quantity;
        plan.goldPlanned += row.total;
    }
    plan.pressCapped = true;
}

// ---------------------------------------------------------------------------
// Host interface. Probe.cpp (S2c) implements it on the UI thread inside the
// one commit task; the host test drives it with a fake.

// Menu, load/save and config generations plus the foreground gate, checked
// before every row.
enum class BatchGate : unsigned { kOk, kMenuClosed, kLoadOrSave, kConfigChanged, kForeground };

// Skyrim.esm gold (the same value as Inventory.h GoldFormID, which this
// engine-free header does not include).
inline constexpr std::uint32_t BatchGoldFormID = 0x0000000Fu;

// One container event the batch could not attribute to the row, kept for the
// receipt (the first one of the batch). known = false: none was seen.
struct BatchForeignEvent {
    bool known{};
    std::uint32_t oldContainer{};
    std::uint32_t newContainer{};
    std::uint32_t baseObj{};
    std::int32_t itemCount{};
};

// The container events one ItemSelect produced, as seen by the sinks.
// "Item" = player -> VENC of a non-gold base; "gold" = VENC -> player gold.
// `foreignEvents` counts any other container event seen in the same window.
//
// r57 (L1b r56: row 1 sold 7 of 7 exactly, yet stopped FOREIGN_EVENT):
// the engine may deliver ONE sale as several container events of the same
// direction, one per extra-data list of the sold stack (a natural stack on a
// heavy save carries several lists; every row sold live before r56 was a
// console-staged stack with none, one event each). So itemCount and
// goldAmount are the SUMS over the row's item and gold events, and the row is
// judged on those sums (DecideBatchRow), not on the number of events.
struct BatchRowEvents {
    std::uint32_t itemEvents{};
    bool itemPlayerToVenc{};
    // The base of the first item event; itemBaseMixed = a later item event
    // named another base.
    std::uint32_t itemBase{};
    bool itemBaseMixed{};
    std::int64_t itemCount{};
    std::uint32_t goldEvents{};
    bool goldVencToPlayer{};
    std::int64_t goldAmount{};
    std::uint32_t foreignEvents{};
    BatchForeignEvent firstForeign{};
    // r57 audit B-01: the first item event, and the first item event
    // whose base differed from it, so a base the row did not plan is named
    // in the receipt too (RecordBatchRowEvents).
    BatchForeignEvent firstItem{};
    BatchForeignEvent firstMixedItem{};
};

// One container event into a row's event set (the sink's classification,
// engine-free so the host test drives the product code). An event with a
// non-positive count is never part of a sale: it counts as foreign.
inline void AddBatchRowEvent(BatchRowEvents& events, std::uint32_t oldContainer, std::uint32_t newContainer,
                             std::uint32_t baseObj, std::int32_t itemCount, std::uint32_t playerId,
                             std::uint32_t vencId) noexcept {
    const bool pair = playerId != 0 && vencId != 0 && itemCount > 0;
    if (pair && oldContainer == playerId && newContainer == vencId && baseObj != BatchGoldFormID) {
        if (events.itemEvents == 0) {
            events.itemBase = baseObj;
            events.firstItem = BatchForeignEvent{true, oldContainer, newContainer, baseObj, itemCount};
        } else if (events.itemBase != baseObj) {
            if (!events.itemBaseMixed) {
                events.firstMixedItem = BatchForeignEvent{true, oldContainer, newContainer, baseObj, itemCount};
            }
            events.itemBaseMixed = true;
        }
        ++events.itemEvents;
        events.itemPlayerToVenc = true;
        events.itemCount += itemCount;
    } else if (pair && oldContainer == vencId && newContainer == playerId && baseObj == BatchGoldFormID) {
        ++events.goldEvents;
        events.goldVencToPlayer = true;
        events.goldAmount += itemCount;
    } else {
        if (!events.firstForeign.known) {
            events.firstForeign = BatchForeignEvent{true, oldContainer, newContainer, baseObj, itemCount};
        }
        ++events.foreignEvents;
    }
}

struct BatchCallResult {
    // False: the vanilla sale callback was not available, nothing was called.
    bool called{};
    // The events delivered while the call was in progress (P0).
    BatchRowEvents inCall{};
};

class BatchHost {
public:
    virtual ~BatchHost() = default;
    // Monotonic microseconds.
    virtual std::uint64_t NowUs() = 0;
    virtual BatchGate CheckGate() = 0;
    // The current ItemList identity (pointer and size only).
    virtual BatchListIdentity CurrentList() = 0;
    // Reads one row of the current list (Item pointer and objDesc base).
    // Never called for a row this batch already dispatched in the same list.
    virtual bool ReadRow(std::size_t index, BatchListEntry& out) = 0;
    // A pointer-only read of every row of the current list; used only after
    // the list identity changed (P2 true), to rebuild the base-to-index map.
    virtual bool ReadList(std::vector<BatchListEntry>& out) = 0;
    // R20-3 bind: selectedIndex write plus GetSelectedItem read-back equality.
    virtual bool Bind(const BatchRow& row) = 0;
    // One vanilla ItemSelect with raw = {0, quantity, unitPrice, null}.
    virtual BatchCallResult Dispatch(const BatchRow& row) = 0;
    // bindfix: the list's update-pending flag (ItemList byte 0x50; the
    // vendored CommonLib calls it unk50, CommonLibSSE-NG 9.1 names it
    // updatePending). While it is set, GetSelectedItem() answers null, so
    // neither our read-back nor the vanilla handler can see a selection.
    virtual bool ListUpdatePending() = 0;
    virtual void SetListUpdatePending(bool pending) = 0;
    // Why the last Bind() returned false, as a static token for the receipt.
    virtual const char* LastBindFailure() { return "unknown"; }
    // bindfix2: the last Bind() refused only because the read-back named
    // another live row of the same, unchanged list while the row's own entry
    // was still in place (the list UI moved the selection after our write).
    // Every other refusal answers false.
    virtual bool LastBindSelectionMoved() { return false; }
};

// Why a batch stopped. kNone = still running or completed.
enum class BatchStop : unsigned {
    kNone,
    kMenuClosed,
    kLoadOrSave,
    kConfigChanged,
    kForeground,
    kListLost,
    kRowMismatch,
    kBindFailed,
    kAdapterUnavailable,
    kEventsMissing,
    kForeignEvent,
    kCountMismatch,
    kGoldMismatch,
    kDeferredTimeout,
    kReconcileMismatch,
    kWatchdog,
};

inline const char* BatchStopName(BatchStop value) noexcept {
    switch (value) {
    case BatchStop::kNone: return "none";
    case BatchStop::kMenuClosed: return "MENU_CLOSED";
    case BatchStop::kLoadOrSave: return "LOAD_OR_SAVE";
    case BatchStop::kConfigChanged: return "CONFIG_CHANGED";
    case BatchStop::kForeground: return "FOREGROUND";
    case BatchStop::kListLost: return "LIST_LOST";
    case BatchStop::kRowMismatch: return "ROW_MISMATCH";
    case BatchStop::kBindFailed: return "BIND_FAILED";
    case BatchStop::kAdapterUnavailable: return "ADAPTER_UNAVAILABLE";
    case BatchStop::kEventsMissing: return "UNCONFIRMED";
    case BatchStop::kForeignEvent: return "FOREIGN_EVENT";
    case BatchStop::kCountMismatch: return "COUNT_MISMATCH";
    case BatchStop::kGoldMismatch: return "GOLD_MISMATCH";
    case BatchStop::kDeferredTimeout: return "UNCONFIRMED_TIMEOUT";
    case BatchStop::kReconcileMismatch: return "ORACLE_MISMATCH";
    case BatchStop::kWatchdog: return "BATCH_TIMEOUT";
    }
    return "unknown";
}

inline BatchStop BatchStopFromGate(BatchGate gate) noexcept {
    switch (gate) {
    case BatchGate::kOk: return BatchStop::kNone;
    case BatchGate::kMenuClosed: return BatchStop::kMenuClosed;
    case BatchGate::kLoadOrSave: return BatchStop::kLoadOrSave;
    case BatchGate::kConfigChanged: return BatchStop::kConfigChanged;
    case BatchGate::kForeground: return BatchStop::kForeground;
    }
    return BatchStop::kForeground;
}

// P0 as the batch observed it: unknown until the first dispatched row.
enum class BatchEventMode : unsigned { kUnknown, kInCall, kDeferred };

struct BatchState {
    BatchPlan plan{};
    BatchListIdentity list{};
    std::size_t nextRow{};
    std::uint64_t edgeUs{};
    std::uint64_t lastTaskEndUs{};
    std::uint32_t commitFrames{};
    std::uint64_t commitTaskUsMax{};
    std::uint64_t commitTaskUsTotal{};
    std::uint32_t rowsDispatched{};
    std::uint32_t rowsConfirmed{};
    std::uint32_t rowsUnconfirmed{};
    std::uint32_t rowsDeferred{};
    std::uint32_t pairInCallRows{};
    std::uint32_t listRebuiltInCallRows{};
    std::uint32_t listRemaps{};
    // bindfix: rows bound and dispatched across the batch's own
    // update-pending flag (DecideBatchPendingBind), and the first bind
    // failure as a static token ("none" until one happens).
    std::uint32_t pendingBypassRows{};
    const char* bindFailure{"none"};
    // bindfix2: binds repeated after a moved read-back (all rows).
    std::uint32_t bindRewrites{};
    // r57: rows decided on more than one item or gold event (a sale the
    // engine delivered in parts), and the first foreign event of the batch.
    std::uint32_t splitEventRows{};
    BatchForeignEvent firstForeign{};
    std::int64_t unitsSold{};
    std::int64_t goldDispatched{};
    std::int64_t goldConfirmed{};
    BatchEventMode eventMode{BatchEventMode::kUnknown};
    BatchStop stop{BatchStop::kNone};
    bool latched{};
    // the after-sample reconciliation failed. Kept apart from `stop`,
    // which keeps the first stop reason (for example FOREGROUND), so a failed
    // reconciliation after an earlier stop still makes the outcome uncertain.
    bool reconcileFailed{};
    // Every planned row was dispatched or the batch stopped. Deferred rows
    // may still be undecided (rowsDeferred > 0).
    bool done{};
};

inline BatchState BeginBatch(BatchPlan plan, std::uint64_t edgeUs) {
    BatchState state;
    state.list = plan.list;
    state.plan = std::move(plan);
    state.edgeUs = edgeUs;
    state.done = !state.plan.valid || state.plan.rows.empty();
    return state;
}

// w-probe-r71: the parameter is not named `latch` (js::latch is the plugin latch; MSVC C4459).
inline void StopBatch(BatchState& state, BatchStop reason, bool latchRun) noexcept {
    if (state.stop == BatchStop::kNone) state.stop = reason;
    if (latchRun) state.latched = true;
    state.done = true;
}

// The row's own events against its plan (2.2). kNone = Confirmed.
//
// r57: the row is judged on the SUM of its item events and the SUM of
// its gold events, so a sale the engine delivered in several parts (one event
// per extra-data list) is Confirmed when the parts add up exactly. Nothing
// else was relaxed: any event that is not player -> VENC of a non-gold base
// or VENC -> player gold is still foreign; an item event of another base
// (mixed or not the row's) is foreign; an extra same-base move or an extra
// gold move breaks the exact sum (COUNT_MISMATCH / GOLD_MISMATCH). Each
// event carries at least one unit, so a row has at most `quantity` item
// events and `total` gold events when it is Confirmed.
inline BatchStop DecideBatchRow(const BatchRow& row, const BatchRowEvents& events) noexcept {
    if (events.foreignEvents != 0 || events.itemBaseMixed) return BatchStop::kForeignEvent;
    if (events.itemEvents == 0 || events.goldEvents == 0) return BatchStop::kEventsMissing;
    if (!events.itemPlayerToVenc || events.itemBase != row.baseFormKey) return BatchStop::kForeignEvent;
    if (events.itemCount != row.quantity) return BatchStop::kCountMismatch;
    if (!events.goldVencToPlayer || events.goldAmount != row.total) return BatchStop::kGoldMismatch;
    return BatchStop::kNone;
}

// r57: keeps the event shape a row was decided on (receipt only; no
// decision reads it). The batch's first unattributed event is the first
// foreign event, else the first item event of a base the row
// did not plan: a later base that differed from the first, or the first
// item event itself when its base is not the row's.
inline void RecordBatchRowEvents(BatchState& state, BatchRow& row, const BatchRowEvents& events) noexcept {
    row.itemEvents = events.itemEvents;
    row.goldEvents = events.goldEvents;
    row.foreignEvents = events.foreignEvents;
    if (events.itemEvents > 1 || events.goldEvents > 1) ++state.splitEventRows;
    if (state.firstForeign.known) return;
    if (events.firstForeign.known) {
        state.firstForeign = events.firstForeign;
    } else if (events.firstItem.known && events.itemBase != row.baseFormKey) {
        state.firstForeign = events.firstItem;
    } else if (events.itemBaseMixed) {
        state.firstForeign = events.firstMixedItem;
    }
}

inline void ApplyBatchRowDecision(BatchState& state, BatchRow& row, BatchStop decision) noexcept {
    if (row.state != BatchRowState::kDispatched) return;
    if (decision == BatchStop::kNone) {
        row.state = BatchRowState::kConfirmed;
        ++state.rowsConfirmed;
        state.unitsSold += row.quantity;
        state.goldConfirmed += row.total;
    } else {
        row.state = BatchRowState::kUnconfirmed;
        ++state.rowsUnconfirmed;
        StopBatch(state, decision, true);
    }
}

// After the list identity changed: map every row not yet dispatched to its
// unique index in the new list by base. Zero or several matches fail closed.
inline bool RemapBatchRows(BatchState& state, const std::vector<BatchListEntry>& entries,
                           const BatchListIdentity& identity) {
    if (entries.size() != identity.size) return false;
    for (std::size_t r = state.nextRow; r < state.plan.rows.size(); ++r) {
        BatchRow& row = state.plan.rows[r];
        if (row.state != BatchRowState::kPlanned) continue;
        std::size_t found = entries.size();
        std::size_t matches = 0;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].baseFormKey == row.baseFormKey) {
                found = i;
                ++matches;
            }
        }
        if (matches != 1) return false;
        row.listIndex = found;
        row.item = entries[found].item;
    }
    state.list = identity;
    ++state.listRemaps;
    return true;
}

// ---------------------------------------------------------------------------
// bindfix (L1a live 2026-09-25: 20 rows planned, row 1 sold, then
// stop=BIND_FAILED with listRemaps 0 and listRebuiltInCall false).
//
// A vanilla sale does not rebuild the ItemList inside the call (P2 false,
// measured); the engine only marks the list update-pending and re-lists it
// later. While that flag is set, ItemList::GetSelectedItem() returns null
// (CommonLib reproduces the engine's own check), so the next row's
// selectedIndex read-back fails and so would the vanilla handler's own
// selection read. Nothing else about the next row is stale: its Item, its
// objDesc and its GFx members are untouched by a sale of another base.
//
// The fix lets the commit task bind the next row across a pending flag that
// THIS task's own confirmed sales raised, and only then:
//   * at least one row of this same task was dispatched (the flag was clear
//     at the task's first bind, so every later raise came from our calls);
//   * every dispatched row of the batch is Confirmed by its exact in-call
//     events (no deferred, no unconfirmed row, no foreign event);
//   * the list identity did not change during this task;
//   * no earlier dispatched row has the same base (a same-base stack could
//     share the changed inventory entry, so it is treated as stale).
// Otherwise the pending flag is a real stop (BIND_FAILED with a reason).
// The caller clears the flag only around its own bind + dispatch and raises
// it again afterwards, so the engine still runs its re-list after the
// commit task. (r58: that re-list is not the whole cost; L1b r57 showed
// engine work per sold row after the task, see PressRowCap.)
//
// r53 audit A-01: "a confirmed earlier sale" alone does not prove that
// the flag now set is ours. A flag left clear by our last sale and raised
// later by an unrelated refresh passed every check above and was cleared.
// So ownership is proven per dispatch (BatchPendingProof): the task reads the
// flag and the list identity immediately before its own vanilla dispatch
// (the flag must be clear there, or the row is not dispatched) and
// immediately after it. Only "clear before, set after, same list identity"
// within the same task proves that our sale raised it; a flag the task
// itself raised again after a bypassed call (restoring the one it cleared)
// counts as ours too. Any other state -- no dispatch yet in this task, the
// flag set before our dispatch, the flag left clear by our last dispatch and
// set by someone else since, or the list identity changed across or after
// the dispatch -- stops the batch as BIND_FAILED with a named reason.
// Residual (not observable through a single byte): another writer raising
// the flag during our own dispatch, or after our own raise, is
// indistinguishable from ours; the per-row entry, bind re-verification and
// event checks still guard those rows.
// ---------------------------------------------------------------------------
enum class BatchPendingBind : unsigned { kClear, kBypass, kRefuse };

struct BatchPendingDecision {
    BatchPendingBind bind{BatchPendingBind::kClear};
    const char* reason{"none"};
};

// What the task observed around its latest own dispatch. Task-local: a new
// task starts with observed = false, so a flag found at a task's first row
// is never ours.
struct BatchPendingProof {
    // A dispatch of this task was observed.
    bool observed{};
    // The flag was read clear immediately before that dispatch.
    bool clearBeforeDispatch{};
    // The flag was read set immediately after that dispatch, or the task
    // itself raised it again right after a bypassed call.
    bool setAfterDispatch{};
    // The list identity was the same before and after that dispatch.
    bool listStable{};
    // The list identity read immediately after that dispatch.
    BatchListIdentity list{};
};

inline BatchPendingProof ObserveBatchDispatchPending(bool pendingBefore, const BatchListIdentity& listBefore,
                                                     bool pendingAfter, const BatchListIdentity& listAfter,
                                                     bool raisedAgainByTask) noexcept {
    BatchPendingProof proof;
    proof.observed = true;
    proof.clearBeforeDispatch = !pendingBefore;
    proof.listStable = SameBatchList(listBefore, listAfter);
    proof.setAfterDispatch = proof.clearBeforeDispatch && proof.listStable && (pendingAfter || raisedAgainByTask);
    proof.list = listAfter;
    return proof;
}

inline bool BatchBaseDispatched(const BatchState& state, std::uint32_t base) noexcept {
    for (const auto& row : state.plan.rows) {
        if (row.state != BatchRowState::kPlanned && row.baseFormKey == base) return true;
    }
    return false;
}

inline BatchPendingDecision DecideBatchPendingBind(bool pending, const BatchState& state,
                                                   const BatchRow& row, std::uint32_t rowsThisTask,
                                                   bool listChangedThisTask, const BatchPendingProof& proof,
                                                   const BatchListIdentity& current) noexcept {
    if (!pending) return {BatchPendingBind::kClear, "none"};
    if (rowsThisTask == 0 || state.rowsDispatched == 0) return {BatchPendingBind::kRefuse, "pending_not_ours"};
    if (!proof.observed) return {BatchPendingBind::kRefuse, "pending_not_ours"};
    if (listChangedThisTask || !proof.listStable || !SameBatchList(proof.list, current)) {
        return {BatchPendingBind::kRefuse, "pending_list_changed"};
    }
    if (!proof.clearBeforeDispatch) return {BatchPendingBind::kRefuse, "pending_before_dispatch"};
    // Our last dispatch left the flag clear: whoever raised it since, it was
    // not our sale.
    if (!proof.setAfterDispatch) return {BatchPendingBind::kRefuse, "pending_external"};
    if (state.rowsConfirmed != state.rowsDispatched || state.rowsDeferred != 0 || state.rowsUnconfirmed != 0 ||
        state.eventMode != BatchEventMode::kInCall) {
        return {BatchPendingBind::kRefuse, "pending_unconfirmed"};
    }
    if (BatchBaseDispatched(state, row.baseFormKey)) return {BatchPendingBind::kRefuse, "pending_same_base"};
    return {BatchPendingBind::kBypass, "none"};
}

enum class BatchTaskEnd : unsigned { kDone, kSpill, kStopped };

struct BatchTaskResult {
    BatchTaskEnd end{BatchTaskEnd::kDone};
    std::uint32_t rows{};
    std::uint64_t taskUs{};
};

// One UI task of the commit (design-r2 2.1 step C). Returns kSpill when the
// frame cap ended the task with rows left; the caller runs the next task on
// its next pump frame. Returns kDone / kStopped when the batch is over.
inline BatchTaskResult RunCommitTask(BatchState& state, BatchHost& host) {
    BatchTaskResult result;
    if (state.done) {
        result.end = state.stop == BatchStop::kNone ? BatchTaskEnd::kDone : BatchTaskEnd::kStopped;
        return result;
    }
    const std::uint64_t start = host.NowUs();
    if (state.commitFrames >= BatchMaxCommitFrames) {
        StopBatch(state, BatchStop::kWatchdog, false);
        result.end = BatchTaskEnd::kStopped;
        return result;
    }
    ++state.commitFrames;
    bool spill = false;
    std::vector<BatchListEntry> entries;
    // bindfix: the remap count when this task bound its first row; a
    // later change means the list changed inside this task.
    std::uint32_t remapsAtFirstBind = state.listRemaps;
    // r53 A-01: the pending-flag ownership proof of this task's latest
    // own dispatch (empty until this task dispatched a row).
    BatchPendingProof proof;
    while (state.nextRow < state.plan.rows.size()) {
        const std::uint64_t now = host.NowUs();
        // The frame cap, checked before each next row; the first row of a
        // task always runs.
        if (result.rows > 0 && now - start >= CommitFrameCapUs) {
            spill = true;
            break;
        }
        if (now - state.edgeUs >= BatchWatchdogUs || state.rowsDispatched >= BatchMaxRows) {
            StopBatch(state, BatchStop::kWatchdog, false);
            break;
        }
        const BatchGate gate = host.CheckGate();
        if (gate != BatchGate::kOk) {
            StopBatch(state, BatchStopFromGate(gate), false);
            break;
        }
        const BatchListIdentity current = host.CurrentList();
        if (!SameBatchList(current, state.list)) {
            // P2 true: the previous call rebuilt the list. Re-map before any
            // row of the new list is read.
            if (!host.ReadList(entries) || !RemapBatchRows(state, entries, current)) {
                StopBatch(state, BatchStop::kListLost, false);
                break;
            }
        }
        BatchRow& row = state.plan.rows[state.nextRow];
        BatchListEntry entry;
        if (!host.ReadRow(row.listIndex, entry)) {
            StopBatch(state, BatchStop::kListLost, false);
            break;
        }
        if (entry.item != row.item || entry.baseFormKey != row.baseFormKey) {
            StopBatch(state, BatchStop::kRowMismatch, false);
            break;
        }
        if (result.rows == 0) remapsAtFirstBind = state.listRemaps;
        const BatchPendingDecision pending = DecideBatchPendingBind(
            host.ListUpdatePending(), state, row, result.rows, state.listRemaps != remapsAtFirstBind, proof, current);
        if (pending.bind == BatchPendingBind::kRefuse) {
            state.bindFailure = pending.reason;
            StopBatch(state, BatchStop::kBindFailed, false);
            break;
        }
        const bool bypass = pending.bind == BatchPendingBind::kBypass;
        if (bypass) host.SetListUpdatePending(false);
        // bindfix2 (L1a r4 2026-09-27: 15 of 20 rows, then BIND_FAILED
        // readback_mismatch at the first row outside the visible window).
        // SkyUI's list scrolls to a written selectedIndex that lies outside
        // the visible rows, and its redraw then re-selects the row under the
        // mouse cursor (ScrollingList.UpdateList, mouse-driven navigation,
        // the PC default), so the read-back names that row instead. After
        // that scroll our row is visible, so the same write holds the second
        // time. Bind() is repeated in full (entry, re-verification, write,
        // read-back) at most BatchBindRewriteMax times, only while the host
        // reports exactly that refusal; nothing is dispatched on a read-back
        // that does not name the row.
        bool bound = host.Bind(row);
        for (std::uint32_t rewrite = 0;
             !bound && rewrite < BatchBindRewriteMax && host.LastBindSelectionMoved(); ++rewrite) {
            ++state.bindRewrites;
            bound = host.Bind(row);
        }
        if (!bound) {
            // Never leave the flag lower than we found it.
            if (bypass) host.SetListUpdatePending(true);
            state.bindFailure = host.LastBindFailure();
            StopBatch(state, BatchStop::kBindFailed, false);
            break;
        }
        // r53 A-01: the flag must be clear immediately before our own
        // dispatch (cleared by us on a bypass, or never raised), on the list
        // this row was read from. A flag set here was not raised by a sale of
        // ours: nothing is dispatched and the flag is left as found.
        const bool pendingBeforeDispatch = host.ListUpdatePending();
        const BatchListIdentity listBeforeDispatch = host.CurrentList();
        if (pendingBeforeDispatch || !SameBatchList(listBeforeDispatch, current)) {
            state.bindFailure = pendingBeforeDispatch ? "pending_before_dispatch" : "pending_list_changed";
            StopBatch(state, BatchStop::kBindFailed, false);
            break;
        }
        const BatchCallResult call = host.Dispatch(row);
        const bool pendingAfterDispatch = host.ListUpdatePending();
        const BatchListIdentity listAfterDispatch = host.CurrentList();
        bool raisedAgain = false;
        if (bypass) {
            // Raise the engine's pending re-list again unless the call
            // re-listed the list itself (P2 true), so the engine still runs
            // its re-list after the commit task.
            if (SameBatchList(listAfterDispatch, current) && !pendingAfterDispatch) {
                host.SetListUpdatePending(true);
                raisedAgain = true;
            }
            if (call.called) ++state.pendingBypassRows;
        }
        proof = ObserveBatchDispatchPending(pendingBeforeDispatch, listBeforeDispatch, pendingAfterDispatch,
                                            listAfterDispatch, raisedAgain);
        if (!call.called) {
            StopBatch(state, BatchStop::kAdapterUnavailable, false);
            break;
        }
        row.state = BatchRowState::kDispatched;
        ++state.rowsDispatched;
        state.goldDispatched += row.total;
        ++state.nextRow;
        ++result.rows;
        row.listRebuiltInCall = !SameBatchList(listAfterDispatch, current);
        if (row.listRebuiltInCall) ++state.listRebuiltInCallRows;
        const BatchRowEvents& events = call.inCall;
        const bool anyInCall = events.itemEvents != 0 || events.goldEvents != 0 || events.foreignEvents != 0;
        if (anyInCall) {
            row.pairInCall = events.itemEvents >= 1 && events.goldEvents >= 1;
            if (row.pairInCall) ++state.pairInCallRows;
            if (state.eventMode == BatchEventMode::kUnknown) state.eventMode = BatchEventMode::kInCall;
            RecordBatchRowEvents(state, row, events);
            ApplyBatchRowDecision(state, row, DecideBatchRow(row, events));
        } else if (state.eventMode == BatchEventMode::kInCall) {
            // P0 was observed true on an earlier row: silence now is a
            // missing pair, not a late one.
            ApplyBatchRowDecision(state, row, BatchStop::kEventsMissing);
        } else {
            state.eventMode = BatchEventMode::kDeferred;
            ++state.rowsDeferred;
        }
        if (state.stop != BatchStop::kNone) break;
    }
    if (!spill && state.stop == BatchStop::kNone && state.nextRow >= state.plan.rows.size()) {
        state.done = true;
    }
    const std::uint64_t end = host.NowUs();
    result.taskUs = end - start;
    state.lastTaskEndUs = end;
    state.commitTaskUsTotal += result.taskUs;
    if (result.taskUs > state.commitTaskUsMax) state.commitTaskUsMax = result.taskUs;
    result.end = state.stop != BatchStop::kNone ? BatchTaskEnd::kStopped :
                 state.done ? BatchTaskEnd::kDone : BatchTaskEnd::kSpill;
    return result;
}

// P0 false: the events of deferred rows arrive after the task. `perRow[k]`
// holds the events attributed to the k-th row still Dispatched, in plan
// order. Every mismatch latches. Returns the number of rows decided.
inline std::size_t DecideDeferredRows(BatchState& state, std::span<const BatchRowEvents> perRow) {
    std::size_t k = 0;
    for (auto& row : state.plan.rows) {
        if (row.state != BatchRowState::kDispatched) continue;
        if (k >= perRow.size()) break;
        RecordBatchRowEvents(state, row, perRow[k]);
        ApplyBatchRowDecision(state, row, DecideBatchRow(row, perRow[k]));
        ++k;
        if (state.rowsDeferred > 0) --state.rowsDeferred;
    }
    return k;
}

// Rows still undecided when the deferred cap passed are Unconfirmed.
inline bool ExpireDeferredRows(BatchState& state, std::uint64_t nowUs) {
    if (state.rowsDeferred == 0 || nowUs - state.lastTaskEndUs < BatchDeferredVerifyCapUs) return false;
    for (auto& row : state.plan.rows) {
        if (row.state == BatchRowState::kDispatched) {
            ApplyBatchRowDecision(state, row, BatchStop::kDeferredTimeout);
        }
    }
    state.rowsDeferred = 0;
    return true;
}

// a batch that ends while rows are still Dispatched (the menu closed
// or the run was cancelled during the deferred-event phase). The vanilla sale
// call already ran for those rows but no event decided them, so they become
// Unconfirmed: the receipt counts them as uncertain, not as unsold, and the
// outcome is uncertain. The first stop reason is kept (StopBatch). Returns
// the number of rows settled.
inline std::size_t SettleUndecidedBatchRows(BatchState& state) noexcept {
    std::size_t settled = 0;
    for (auto& row : state.plan.rows) {
        if (row.state != BatchRowState::kDispatched) continue;
        ApplyBatchRowDecision(state, row, BatchStop::kEventsMissing);
        ++settled;
    }
    state.rowsDeferred = 0;
    return settled;
}

// Design-r2 2.5: player gold delta == +sum(q x p) and VENC delta == -sum over
// every dispatched row. Any mismatch (or unusable samples) latches.
struct BatchReconcileResult {
    bool ok{};
    std::int64_t expected{};
};

inline BatchReconcileResult ReconcileBatch(BatchState& state, bool samplesValid,
                                           std::int64_t playerGoldDelta, std::int64_t vencGoldDelta) {
    BatchReconcileResult result;
    result.expected = state.goldDispatched;
    result.ok = samplesValid && playerGoldDelta == result.expected && vencGoldDelta == -result.expected;
    if (!result.ok) {
        if (state.stop == BatchStop::kNone) state.stop = BatchStop::kReconcileMismatch;
        state.latched = true;
        state.reconcileFailed = true;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Outcome classification (design-r2 2.10): exactly one line per press inside
// the BarterMenu, none outside it.

enum class BatchOutcome : unsigned { kNone, kSold, kPartial, kNothing, kBlocked, kUncertain };

enum class BatchReason : unsigned {
    kNone,
    // Partial reasons, in priority order.
    kMerchantGold,
    kLimit,
    kStopped,
    // Blocked reasons.
    kNoGold,
    kBusy,
    kRunning,
    kOff,
    kSettings,
    kLatched,
    kUnavailable,
};

// The press gate, evaluated before the read. kBarterClosed = no outcome.
enum class BatchPressGate : unsigned {
    kOk,
    kBarterClosed,
    kSettingsCorrupt,
    kSettingsOff,
    kBusy,
    kLatched,
    kRunning,
    kAdapterUnavailable,
};

struct BatchPressFacts {
    bool barterOpen{};
    bool settingsReadable{true};
    bool enabled{true};
    bool foregroundOk{true};
    bool latched{};
    bool running{};
    bool adapterAvailable{true};
};

// Design-r2 2.1 gate order: barter open first (outside it nothing happens),
// then settings, foreground, latch, a batch already running, the adapter.
inline BatchPressGate EvaluatePressGate(const BatchPressFacts& facts) noexcept {
    if (!facts.barterOpen) return BatchPressGate::kBarterClosed;
    if (!facts.settingsReadable) return BatchPressGate::kSettingsCorrupt;
    if (!facts.enabled) return BatchPressGate::kSettingsOff;
    if (!facts.foregroundOk) return BatchPressGate::kBusy;
    if (facts.latched) return BatchPressGate::kLatched;
    if (facts.running) return BatchPressGate::kRunning;
    if (!facts.adapterAvailable) return BatchPressGate::kAdapterUnavailable;
    return BatchPressGate::kOk;
}

struct BatchOutcomeInput {
    BatchPressGate gate{BatchPressGate::kOk};
    std::int64_t unitsEligible{};  // m
    std::int64_t unitsPlanned{};
    std::int64_t unitsSold{};      // n
    std::int64_t goldReceived{};   // g (the reconciled player delta)
    bool balanceLimited{};
    bool runLimited{};
    bool balanceUnknown{};
    bool stopped{};
    bool uncertain{};
    // r58: the per-press row cap cut the plan (ApplyPressRowCap).
    bool pressCapped{};
};

struct BatchResultLine {
    BatchOutcome outcome{BatchOutcome::kNone};
    BatchReason reason{BatchReason::kNone};
    std::int64_t n{};
    std::int64_t m{};
    std::int64_t g{};
};

inline BatchResultLine ClassifyBatchOutcome(const BatchOutcomeInput& input) noexcept {
    BatchResultLine line;
    line.n = input.unitsSold;
    line.m = input.unitsEligible;
    line.g = input.goldReceived;
    switch (input.gate) {
    case BatchPressGate::kBarterClosed: return BatchResultLine{};
    case BatchPressGate::kSettingsCorrupt: line.reason = BatchReason::kSettings; break;
    case BatchPressGate::kSettingsOff: line.reason = BatchReason::kOff; break;
    case BatchPressGate::kBusy: line.reason = BatchReason::kBusy; break;
    case BatchPressGate::kLatched: line.reason = BatchReason::kLatched; break;
    case BatchPressGate::kRunning: line.reason = BatchReason::kRunning; break;
    case BatchPressGate::kAdapterUnavailable: line.reason = BatchReason::kUnavailable; break;
    case BatchPressGate::kOk: break;
    }
    if (input.gate != BatchPressGate::kOk) {
        line.outcome = BatchOutcome::kBlocked;
        line.n = 0;
        line.g = 0;
        return line;
    }
    if (input.uncertain) {
        line.outcome = BatchOutcome::kUncertain;
        line.reason = BatchReason::kStopped;
        return line;
    }
    if (input.unitsEligible <= 0) {
        line.outcome = BatchOutcome::kNothing;
        return line;
    }
    if (input.unitsSold >= input.unitsEligible) {
        line.outcome = BatchOutcome::kSold;
        return line;
    }
    if (input.unitsSold > 0) {
        line.outcome = BatchOutcome::kPartial;
        // r58: a press the row cap cut, and that sold every row it
        // planned, says "limit reached, press again" first: the next press
        // sells more, even when the merchant's gold also cut the selection.
        line.reason = (input.pressCapped && !input.stopped) ? BatchReason::kLimit :
                      input.balanceLimited ? BatchReason::kMerchantGold :
                      input.runLimited ? BatchReason::kLimit : BatchReason::kStopped;
        return line;
    }
    line.outcome = BatchOutcome::kBlocked;
    // n == 0 and m > 0: a stop before the first sale or an unknown balance is
    // "unavailable"; a plan the merchant's gold emptied is "no gold".
    line.reason = (!input.stopped && !input.balanceUnknown && input.unitsPlanned == 0 &&
                   input.balanceLimited) ? BatchReason::kNoGold : BatchReason::kUnavailable;
    return line;
}

inline BatchOutcomeInput BatchOutcomeInputFrom(const BatchState& state, std::int64_t goldReceived) noexcept {
    BatchOutcomeInput input;
    input.unitsEligible = state.plan.unitsEligible;
    input.unitsPlanned = state.plan.unitsPlanned;
    input.unitsSold = state.unitsSold;
    input.goldReceived = goldReceived;
    input.balanceLimited = state.plan.balanceLimited;
    input.runLimited = state.plan.runLimited;
    input.balanceUnknown = state.plan.balanceUnknown;
    input.pressCapped = state.plan.pressCapped;
    input.stopped = state.stop != BatchStop::kNone;
    // uncertain whatever the first stop was: an Unconfirmed row, a
    // failed reconciliation, or a dispatched row still undecided at the end.
    bool undecided = state.rowsDeferred > 0;
    for (const auto& row : state.plan.rows) {
        if (row.state == BatchRowState::kDispatched) undecided = true;
    }
    input.uncertain = state.stop == BatchStop::kReconcileMismatch || state.reconcileFailed ||
                      state.rowsUnconfirmed > 0 || undecided;
    return input;
}

// Translation keys (design-r2 2.10); the text lives in the MCM translation
// files and is loaded by ResultText.h (S2b).
inline const char* BatchOutcomeKey(BatchOutcome value) noexcept {
    switch (value) {
    case BatchOutcome::kSold: return "$AGH_JS_RESULT_SOLD";
    case BatchOutcome::kPartial: return "$AGH_JS_RESULT_PARTIAL";
    case BatchOutcome::kNothing: return "$AGH_JS_RESULT_NOTHING";
    case BatchOutcome::kBlocked: return "$AGH_JS_RESULT_BLOCKED";
    case BatchOutcome::kUncertain: return "$AGH_JS_RESULT_UNCERTAIN";
    case BatchOutcome::kNone: return "";
    }
    return "";
}

inline const char* BatchReasonKey(BatchReason value) noexcept {
    switch (value) {
    case BatchReason::kMerchantGold: return "$AGH_JS_REASON_MERCHANT_GOLD";
    case BatchReason::kLimit: return "$AGH_JS_REASON_LIMIT";
    case BatchReason::kStopped: return "$AGH_JS_REASON_STOPPED";
    case BatchReason::kNoGold: return "$AGH_JS_REASON_NO_GOLD";
    case BatchReason::kBusy: return "$AGH_JS_REASON_BUSY";
    case BatchReason::kRunning: return "$AGH_JS_REASON_RUNNING";
    case BatchReason::kOff: return "$AGH_JS_REASON_OFF";
    case BatchReason::kSettings: return "$AGH_JS_REASON_SETTINGS";
    case BatchReason::kLatched: return "$AGH_JS_REASON_LATCHED";
    case BatchReason::kUnavailable: return "$AGH_JS_REASON_UNAVAILABLE";
    case BatchReason::kNone: return "";
    }
    return "";
}

// Receipt spellings (js_batch `outcome` / `reason`).
inline const char* BatchOutcomeName(BatchOutcome value) noexcept {
    switch (value) {
    case BatchOutcome::kSold: return "sold";
    case BatchOutcome::kPartial: return "partial";
    case BatchOutcome::kNothing: return "nothing";
    case BatchOutcome::kBlocked: return "blocked";
    case BatchOutcome::kUncertain: return "uncertain";
    case BatchOutcome::kNone: return "none";
    }
    return "none";
}

inline const char* BatchReasonName(BatchReason value) noexcept {
    switch (value) {
    case BatchReason::kMerchantGold: return "merchant_gold";
    case BatchReason::kLimit: return "limit";
    case BatchReason::kStopped: return "stopped";
    case BatchReason::kNoGold: return "no_gold";
    case BatchReason::kBusy: return "busy";
    case BatchReason::kRunning: return "running";
    case BatchReason::kOff: return "off";
    case BatchReason::kSettings: return "settings";
    case BatchReason::kLatched: return "latched";
    case BatchReason::kUnavailable: return "unavailable";
    case BatchReason::kNone: return "none";
    }
    return "none";
}

// ---------------------------------------------------------------------------
// Shadow price quote (design-r2 3.6), receipt only. m_formula =
//   1 / (fBarterMax - (fBarterMax - fBarterMin) x Speech/100)
//   x Haggling rank multiplier x (1 + AV107/100) x (1 + AV146/100).
// Speech is clamped to 0..100 [I: the vanilla barter factor stops at 100].
// Haggling ranks 1..5 are +10/15/20/25/30 percent [I: vanilla perk text].
struct ShadowPriceInputs {
    float barterMax{3.3F};
    float barterMin{2.0F};
    float speech{};
    int hagglingRank{};
    float av107Percent{};
    float av146Percent{};
    // The player owns Allure or one of the two vendor perks: rows are kept
    // but flagged, and the reader leaves them out of the 1% rule.
    bool merchantConditionedPerk{};
};

struct ShadowPriceMultiplierResult {
    bool known{};
    double multiplier{};
};

inline double HagglingMultiplier(int rank) noexcept {
    switch (rank) {
    case 1: return 1.10;
    case 2: return 1.15;
    case 3: return 1.20;
    case 4: return 1.25;
    case 5: return 1.30;
    default: return 1.0;
    }
}

inline ShadowPriceMultiplierResult ShadowPriceMultiplier(const ShadowPriceInputs& in) noexcept {
    ShadowPriceMultiplierResult result;
    if (!std::isfinite(in.barterMax) || !std::isfinite(in.barterMin) || !std::isfinite(in.speech) ||
        !std::isfinite(in.av107Percent) || !std::isfinite(in.av146Percent) ||
        in.hagglingRank < 0 || in.hagglingRank > 5) {
        return result;
    }
    double speech = in.speech;
    if (speech < 0.0) speech = 0.0;
    if (speech > 100.0) speech = 100.0;
    // Named hi/lo, not max/min: the Windows headers define those as macros.
    const double hi = in.barterMax;
    const double lo = in.barterMin;
    const double factor = hi - (hi - lo) * speech / 100.0;
    if (!(factor > 0.0)) return result;
    const double m = HagglingMultiplier(in.hagglingRank) * (1.0 + in.av107Percent / 100.0) *
                     (1.0 + in.av146Percent / 100.0) / factor;
    if (!std::isfinite(m) || !(m > 0.0)) return result;
    result.known = true;
    result.multiplier = m;
    return result;
}

inline std::int64_t ShadowPrice(std::int32_t baseValue, double multiplier) noexcept {
    if (baseValue <= 0 || !(multiplier > 0.0)) return 0;
    return static_cast<std::int64_t>(std::llround(static_cast<double>(baseValue) * multiplier));
}

// Writes the receipt-only quote into every row. Touches no field the plan,
// the commit loop, the reconciliation or the outcome read.
inline void AttachShadowQuotes(BatchPlan& plan, const ShadowPriceInputs& inputs) noexcept {
    const auto m = ShadowPriceMultiplier(inputs);
    plan.shadowKnown = m.known;
    plan.shadowMultiplier = m.multiplier;
    plan.shadowMerchantPerk = inputs.merchantConditionedPerk;
    for (auto& row : plan.rows) row.shadowPrice = m.known ? ShadowPrice(row.baseValue, m.multiplier) : -1;
}

}  // namespace js
