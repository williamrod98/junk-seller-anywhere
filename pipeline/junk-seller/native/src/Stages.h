#pragma once
// Engine free staging rules (r5).
//
// Why this file exists: probe-js05 showed one UI task trying to read all four
// balance candidates plus Speech inside a single 2 ms budget. The player gold
// walk alone spent the whole budget (640-704 entries at the deadline stride), so
// three candidates and Speech were never sampled at all and every sample was a
// budget overrun. r5 splits the oracle into one candidate per task and serves
// the queued stages one at a time, so no single task can exceed the budget
// however long an entry list turns out to be.
//
// Nothing here touches the engine: the queue is a bit mask, the order is fixed,
// and the offline harness drives exactly the arithmetic the runtime uses.
#include "receipt/Log.h"
#include <cstdint>
namespace js {
// The staged UI work in the order a queued set is served. Selection comes first;
// the oracle stages follow in the order the design document lists the
// candidates (player -> merchant Actor -> VENC container -> merchant inventory
// -> Speech), with a resolve stage in front and a publish stage behind.
// w-probe-r1: kQuietObservation is the bounded quiet sample the W stage needs
// (design section 6.3). It sits behind the open selection read and at most one
// may be outstanding at a time.
enum class UiStage : unsigned {
    kSelectionOpen = 0,
    kQuietObservation = 1,
    kOracleResolve = 2,
    kOraclePlayerGold = 3,
    kOracleMerchantActor = 4,
    kOracleMerchantContainer = 5,
    kOracleMerchantInventory = 6,
    kOracleSpeech = 7,
    kOraclePublish = 8,
    kStageCount = 9
};
constexpr unsigned StageCount = static_cast<unsigned>(UiStage::kStageCount);
inline constexpr unsigned StageBit(UiStage stage) noexcept {
    return 1u << static_cast<unsigned>(stage);
}
// One oracle job is exactly this set of stages, queued together and served one
// per task. The publish stage is part of the set so the sample is always
// written out, even when every candidate stage refused to run.
constexpr unsigned OracleStageMask = StageBit(UiStage::kOracleResolve) |
                                     StageBit(UiStage::kOraclePlayerGold) |
                                     StageBit(UiStage::kOracleMerchantActor) |
                                     StageBit(UiStage::kOracleMerchantContainer) |
                                     StageBit(UiStage::kOracleMerchantInventory) |
                                     StageBit(UiStage::kOracleSpeech) |
                                     StageBit(UiStage::kOraclePublish);

// The next stage to serve: the lowest queued bit, or StageCount when the queue
// is empty. A fixed order keeps the receipt readable (resolve before the walks
// that depend on it) and makes the queue testable without a scheduler.
inline unsigned NextStage(unsigned mask) noexcept {
    for (unsigned i = 0; i < StageCount; ++i) {
        if (mask & (1u << i)) return i;
    }
    return StageCount;
}
inline bool StageRunnableNow(unsigned mask, std::uint64_t notBeforeNs,
                             std::uint64_t nowNs) noexcept {
    return mask && (!notBeforeNs || nowNs >= notBeforeNs);
}
inline unsigned ClearStage(unsigned mask, unsigned index) noexcept {
    return index < StageCount ? (mask & ~(1u << index)) : mask;
}

// Per stage budget. A stage gets half the task budget so the task that runs it
// still fits the budget with its own bookkeeping on top; a task budget of zero
// means unbounded, exactly like DeadlineNs.
inline std::uint64_t StageBudgetUs(std::uint64_t taskBudgetUs) noexcept {
    return taskBudgetUs / 2;
}

// Which oracle candidate a stage fills in. The slot is the index into the per
// stage cost array the receipt carries; a stage that is not an oracle stage
// answers OracleStageSlots.
inline unsigned OracleStageSlot(UiStage stage) noexcept {
    switch (stage) {
    case UiStage::kOracleResolve: return 0;
    case UiStage::kOraclePlayerGold: return 1;
    case UiStage::kOracleMerchantActor: return 2;
    case UiStage::kOracleMerchantContainer: return 3;
    case UiStage::kOracleMerchantInventory: return 4;
    case UiStage::kOracleSpeech: return 5;
    default: return OracleStageSlots;
    }
}
// The "not sampled" marker a slot owns. The resolve slot owns no marker of its
// own: when it fails it names the two merchant side candidates it feeds.
inline std::uint32_t OracleSlotBit(unsigned slot) noexcept {
    switch (slot) {
    case 1: return kUnsampledPlayerGold;
    case 2: return kUnsampledMerchantActor;
    case 3: return kUnsampledMerchantContainer;
    case 4: return kUnsampledMerchantInventory;
    case 5: return kUnsampledSpeech;
    default: return 0;
    }
}
// What an exception or an abandoned job leaves unsampled from this slot on.
inline std::uint32_t OracleSlotFailureBits(unsigned slot) noexcept {
    if (slot == 0) return kUnsampledMerchantActor | kUnsampledMerchantContainer;
    return OracleSlotBit(slot);
}

// Poll scheduling (r5 / probe-js04 anomaly 1). The selection poll used to be
// gated on the timestamp of the last row it wrote, which only moves when the
// highlighted item changes, so the poll ran on every pass of the receipt writer
// loop (82 per second measured) instead of twice per second. The poll clock and
// the row clock are now separate values and this rule owns the poll clock.
inline bool PollDue(std::uint64_t nowMs, std::uint64_t lastPollMs, std::uint64_t periodMs) noexcept {
    if (!periodMs) return true;
    return nowMs - lastPollMs >= periodMs;
}
// The row clock: a row is written when it was asked for, when the selection
// changed, or when the heartbeat interval has passed since the last row.
inline bool SelectionRowDue(bool force, bool changed, std::uint64_t nowMs,
                            std::uint64_t lastRowMs, std::uint64_t heartbeatMs) noexcept {
    if (force || changed) return true;
    return nowMs - lastRowMs >= heartbeatMs;
}
}
