#pragma once
// Engine-free state and decisions for one resumable inventory reading.
#include "Inventory.h"
#include "Stages.h"
#include <cstdint>

namespace js {
enum class WalkPhase : unsigned { kChanges, kContainer, kDone };
enum class WalkResult : unsigned { kContinue, kDone, kAbandon };
struct WalkCursor {
    std::uint32_t refFormID{};
    WalkPhase phase{WalkPhase::kChanges};
    std::uint32_t ordinal{};
    std::uintptr_t changesAddr{}, anchorAddr{};
    std::uint64_t menuGeneration{}, loadGeneration{}, inventoryGeneration{}, epoch{};
    bool barterOpenAtStart{};
    std::uint32_t chunks{}, restarts{};
    std::uint64_t startMs{}, notBeforeNs{};
    GoldTally gold{};
    bool goldWanted{true}, goldDone{}, goldCapped{};
    BaseCountTally base{};
    bool baseWanted{}, baseDone{};
    const char* abandonReason{"none"};
};
inline constexpr std::uint64_t ChunkGapUs = 16000;
inline constexpr std::uint32_t MaxWalkChunks = 64;
inline constexpr std::uint32_t MaxWalkRestarts = 2;
inline constexpr std::uint64_t MaxWalkMs = 1500;

// A changed load or menu belongs to another sample; the other changes may
// restart this slot from zero, with both tallies thrown away.
inline const char* WalkInvalidation(const WalkCursor& c, std::uint64_t menu,
                                    std::uint64_t load, std::uint64_t inventory,
                                    std::uint64_t currentEpoch, bool open,
                                    bool referenceResolved, bool referenceMatches,
                                    std::uintptr_t changes, std::uintptr_t anchor,
                                    std::uint32_t containerCount) noexcept {
    if (load != c.loadGeneration || currentEpoch != c.epoch) return "LOAD_CHANGED";
    if (menu != c.menuGeneration) return "MENU_CHANGED";
    if (inventory != c.inventoryGeneration) return "INVENTORY_CHANGED";
    if (open != c.barterOpenAtStart) return "BARTER_CHANGED";
    if (!referenceResolved || !referenceMatches) return "REFERENCE_CHANGED";
    if (changes != c.changesAddr ||
        (c.phase == WalkPhase::kChanges && c.ordinal && anchor != c.anchorAddr))
        return "CHANGES_CHANGED";
    if (c.phase == WalkPhase::kContainer && containerCount < c.ordinal)
        return "CONTAINER_SHRANK";
    return nullptr;
}
inline bool WalkFatalInvalidation(const char* reason) noexcept {
    return reason && (reason[0] == 'L' || reason[0] == 'M');
}
inline void RestartWalk(WalkCursor& c, std::uint64_t menu, std::uint64_t load,
                        std::uint64_t inventory, std::uint64_t currentEpoch,
                        bool open, std::uintptr_t changes) noexcept {
    ++c.restarts;
    c.phase = WalkPhase::kChanges;
    c.ordinal = 0;
    c.changesAddr = changes;
    c.anchorAddr = 0;
    c.menuGeneration = menu;
    c.loadGeneration = load;
    c.inventoryGeneration = inventory;
    c.epoch = currentEpoch;
    c.barterOpenAtStart = open;
    const bool capped = c.goldCapped;
    c.gold = {};
    c.goldWanted = !capped;
    c.goldDone = false;
    c.goldCapped = capped;
    const auto target = c.base.targetFormID;
    c.base = {};
    c.base.targetFormID = target;
    c.baseDone = !c.baseWanted;
}
inline const char* WalkLimit(const WalkCursor& c, std::uint64_t nowMs) noexcept {
    if (c.restarts > MaxWalkRestarts) return "RESTART_LIMIT";
    if (c.chunks > MaxWalkChunks) return "CHUNK_LIMIT";
    if (nowMs - c.startMs > MaxWalkMs) return "WALL_LIMIT";
    return nullptr;
}
inline void OfferWalkEntry(WalkCursor& c, const GoldEntry& entry, bool container,
                           std::uint32_t maxGoldEntries) noexcept {
    if (c.goldWanted && !c.goldDone) {
        if (OfferGoldEntry(c.gold, entry, maxGoldEntries) == GoldStep::kStop) {
            c.goldDone = true;
        }
        if (c.gold.truncated) {
            c.gold = {};
            c.goldWanted = false;
            c.goldCapped = true;
        }
    }
    if (c.baseWanted && !c.baseDone) {
        if (OfferBaseCountEntry(c.base, entry, container) == BaseCountStep::kStop)
            c.baseDone = true;
        if (BaseCountLimitReached(c.base, MaxBaseCountEntries)) c.base.truncated = true;
    }
}
inline void FinishWalkSlot(WalkCursor& c, OracleSample& sample, unsigned slot,
                           bool complete) noexcept {
    ++sample.stagesRun;
    if (!complete) {
        sample.unsampled |= OracleSlotFailureBits(slot);
        sample.truncated = true;
        sample.truncatedStages |= 1u << slot;
        sample.walkAbandonReason = c.abandonReason;
        return;
    }
    if (c.goldCapped) {
        sample.visitCapReached = true;
        sample.unsampled |= OracleSlotFailureBits(slot);
        sample.truncated = true;
        sample.truncatedStages |= 1u << slot;
    }
    const auto gold = c.goldCapped ? -1 : GoldTotal(c.gold);
    if (slot == 1) { sample.playerGold = gold; sample.playerGoldSource = "changes+base_container"; }
    if (slot == 2) { sample.merchantActorGold = gold; sample.merchantActorGoldSource = "changes+base_container"; }
    if (slot == 3) { sample.merchantContainerGold = gold; sample.merchantContainerGoldSource = "changes+base_container"; }
    if (c.baseWanted) {
        const auto count = BaseCountTotal(c.base);
        sample.baseCountVisits += c.base.visited;
        if (slot == 1) { sample.playerBaseCount = count; sample.countSource = BaseCountSource(c.base); }
        if (slot == 3) { sample.vendorBaseCount = count; sample.vendorCountSource = BaseCountSource(c.base); }
    }
}
}
