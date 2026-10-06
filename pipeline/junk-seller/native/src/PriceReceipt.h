#pragma once
// v0.8.0 (w-probe-r69, the barter-price design r2 section 6): the
// priceSource block of js_convert_run and js_batch from one price snapshot
// (PerkPrice.h) and, when the walk ran, its facts. Engine free, so the host
// test fills and emits exactly what the runtime writes.
#include "PerkPrice.h"
#include "receipt/Log.h"
#include <cstdint>

namespace js {

inline void FillPriceSourceReceipt(PriceSourceReceipt& out, const PriceSnapshot& snapshot,
                                   const PerkWalkFacts* walk, std::uint64_t evalUs) noexcept {
    out = PriceSourceReceipt{};
    out.present = true;
    out.source = PriceSourceName(snapshot.source);
    out.reason = snapshot.reason;
    out.switchOn = snapshot.switchOn;
    out.evalUs = evalUs;
    if (!walk) return;
    out.tableOk = walk->tableOk;
    out.walked = walk->walked;
    // A-04: an incomplete condition shape is an overflow too (dropped stays
    // the entries the 64 cap dropped).
    out.overflow = PerkWalkOverflow(*walk);
    out.visits = walk->visits;
    out.collected = walk->count;
    out.dropped = walk->dropped;
    out.engineCalled = walk->engineCalled;
    out.engineSelf = walk->engineSelf;
    if (!walk->walked) return;
    // The two products are reported even when the press fell back, so a
    // perk_mismatch row shows both sides of the self-check.
    const PerkTotals totals =
        CombinePerkEntries(std::span<const PerkEntry>(walk->entries.data(),
                                                      walk->count < PerkWalkMaxEntries ? walk->count
                                                                                       : PerkWalkMaxEntries));
    out.perkFactor = totals.perkFactor;
    out.allPlayer = totals.allPlayer;
    out.playerSidePassing = totals.playerSidePassing;
    out.merchantSidePassing = totals.merchantSidePassing;
    out.unsupportedPassing = totals.unsupportedPassing;
    for (std::uint32_t i = 0; i < walk->count && i < PerkWalkMaxEntries && out.entryRows < out.entries.size(); ++i) {
        const PerkEntry& entry = walk->entries[i];
        const PerkTerm term = PerkEntryTerm(entry);
        auto& row = out.entries[out.entryRows++];
        row.perk = entry.perk;
        row.function = entry.function;
        row.dataType = static_cast<std::int8_t>(entry.dataType);
        row.merchantSide = ClassifyPerkEntry(entry) == PerkSide::kMerchant;
        row.passed = entry.passed;
        row.supported = term.supported;
        row.value0 = entry.value0;
        row.value1 = entry.value1;
        row.avRead = entry.avRead;
        row.term = term.factor;
    }
}

}  // namespace js
