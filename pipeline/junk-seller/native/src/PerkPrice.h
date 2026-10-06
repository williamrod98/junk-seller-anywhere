#pragma once
// v0.8.0 (the barter-price design r2 sections 3.1, 4 and
// 6; the PROBE-JS-PERKPRICE live PASS of 2026-10-04,
// a live test log): the engine-free half of "Price
// follows perks". With the switch ON, sell anywhere pays what the player's own
// price perks earn at a neutral merchant:
//
//   * the kModSellPrices perk entries the player's ForEachPerkEntry walk finds
//     (PerkPriceEngine.h reads them into PerkEntry; nothing here names an
//     engine type),
//   * ClassifyPerkEntry: merchant-dependent when a later ("Target") tab has a
//     condition or a first-tab condition runs on the target; every other
//     entry is player-side,
//   * PerkEntryTerm: the three multiplicative functions (MultiplyValue,
//     MultiplyActorValueMult, Multiply1PlusActorValueMult) in float, exactly
//     as the engine multiplies its float in place; anything else is
//     unsupported (perk_function),
//   * CombinePerkEntries: perkFactor = the float product of the passing
//     player-side entries in walk order (the neutral merchant), allPlayer =
//     the product of every passing entry (the engine's own value with the
//     player as the target, the self-check),
//   * ComputePerkConvertPrice: m = perkFactor / f in float with S and f
//     exactly as ComputeConvertPrice (Convert.h), m' = min(m, 1); the unit
//     price stays ConvertUnitPrice (V x m' rounded in double). The live run
//     measured this rounding order: the menu rounds V times the engine's float factor
//     in double precision (D4: 300 x 0.574999988 = 172.499996 -> 172 where
//     exact decimals give 173),
//   * DecidePriceSnapshot: the one price of a press. OFF = the shipped
//     formula, untouched (reason "off", no walk at all); ON = the perk price,
//     or the shipped formula with the first failure of design section 6 as
//     its reason (perk_table, perk_unavailable, perk_overflow, perk_function,
//     perk_value, perk_mismatch) or the developer key's dev_forced.
//
// The factor does not depend on the item (the entry point has no item
// parameter), so one evaluation serves the whole press: no per-row and no
// per-unit work.
#include "Batch.h"
#include "Convert.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace js {

// Design 3.1 step 2: the walk keeps at most 64 entries (more: perk_overflow).
inline constexpr std::uint32_t PerkWalkMaxEntries = 64;
// The condition items read per tab (a cycle guard; the walk only counts).
// A tab longer than this leaves its shape incomplete: the price then falls
// back with perk_overflow, never classifies on a prefix.
inline constexpr std::uint32_t PerkWalkMaxItemsPerTab = 256;
// Design 3.1 step 6: relative difference allowed between engine and product.
inline constexpr double PerkSelfCheckTolerance = 1e-5;
// BGSEntryPointPerkEntry::EntryData::Function (B/BGSEntryPointPerkEntry.h:20-37).
inline constexpr std::uint8_t PerkFnMultiplyValue = 3;
inline constexpr std::uint8_t PerkFnMultiplyActorValueMult = 13;
inline constexpr std::uint8_t PerkFnMultiply1PlusActorValueMult = 14;
// BGSEntryPointFunctionData::ENTRY_POINT_FUNCTION_DATA (B/BGSEntryPointFunctionData.h:16-26).
inline constexpr std::int32_t PerkDataNone = -1;
inline constexpr std::int32_t PerkDataOneValue = 1;
inline constexpr std::int32_t PerkDataTwoValue = 2;
// CONDITIONITEMOBJECT::kTarget (T/TESCondition.h:17).
inline constexpr std::uint8_t PerkRunOnTarget = 1;
// ActorValue::kTotal (A/ActorValues.h:173).
inline constexpr std::int32_t PerkActorValueTotal = 164;

// One kModSellPrices perk entry as the walk found it.
struct PerkEntry {
    std::uint32_t perk{};
    std::uint8_t function{};
    std::int32_t dataType{PerkDataNone};
    // Two-value data: read only when the object's vtable is the engine's
    // two-value vtable (the float[2] at +0x08 has no CommonLib header).
    bool layoutOk{};
    float value0{std::numeric_limits<float>::quiet_NaN()};
    float value1{std::numeric_limits<float>::quiet_NaN()};
    // Condition shape, counted without evaluating anything.
    std::uint32_t tab0Items{};
    std::uint32_t laterTabItems{};
    bool tab0RunsOnTarget{};
    // False when a tab had more items than PerkWalkMaxItemsPerTab: the side
    // is then unknown (an unread item may run on the target).
    bool shapeComplete{true};
    // The actor value read on the player for the two AV functions (NaN otherwise).
    float avRead{std::numeric_limits<float>::quiet_NaN()};
    // Every tab passed with the player in both roles (design 3.1 step 4).
    bool passed{};
};

enum class PerkSide : unsigned { kPlayer = 0, kMerchant = 1 };

constexpr PerkSide ClassifyPerkEntry(const PerkEntry& entry) noexcept {
    return entry.laterTabItems > 0 || entry.tab0RunsOnTarget ? PerkSide::kMerchant : PerkSide::kPlayer;
}

// Design 3.1 step 3: one condition tab's shape, counted without evaluating
// anything (Item is RE::TESConditionItem at runtime, a host struct in the
// tests; it needs only `next`). At most PerkWalkMaxItemsPerTab items are
// read; an item left unread marks the shape incomplete.
template <class Item, class RunsOnTarget>
inline void CountPerkConditionTab(const Item* head, bool firstTab, PerkEntry& entry, RunsOnTarget runsOnTarget) {
    std::uint32_t walked = 0;
    const Item* item = head;
    for (; item && walked < PerkWalkMaxItemsPerTab; item = item->next) {
        ++walked;
        if (firstTab) {
            ++entry.tab0Items;
            if (runsOnTarget(*item)) entry.tab0RunsOnTarget = true;
        } else {
            ++entry.laterTabItems;
        }
    }
    if (item) entry.shapeComplete = false;
}

constexpr const char* PerkSideName(PerkSide side) noexcept {
    return side == PerkSide::kMerchant ? "merchant" : "player";
}

// The actor value index a two-value entry names: an integral float in [0, kTotal).
inline bool PerkActorValueIndex(float raw, std::int32_t& index) noexcept {
    index = -1;
    if (!std::isfinite(raw) || raw < 0.0F || raw >= static_cast<float>(PerkActorValueTotal)) return false;
    if (std::floor(raw) != raw) return false;
    index = static_cast<std::int32_t>(raw);
    return true;
}

// The two-value entry's actor value, or -1 when it names none.
inline std::int32_t PerkEntryActorValue(const PerkEntry& entry) noexcept {
    std::int32_t index = -1;
    if (entry.dataType == PerkDataTwoValue && entry.layoutOk) PerkActorValueIndex(entry.value0, index);
    return index;
}

constexpr bool PerkUsesActorValue(std::uint8_t function) noexcept {
    return function == PerkFnMultiplyActorValueMult || function == PerkFnMultiply1PlusActorValueMult;
}

struct PerkTerm {
    bool supported{};
    // A supported function whose number is not usable (a non-finite value or
    // AV read, or a product that overflows): perk_value, not perk_function
    //.
    bool numeric{};
    float factor{1.0F};
    const char* reason{"none"};
};

// Design 3.1 step 5, in float as the engine multiplies its float in place.
inline PerkTerm PerkEntryTerm(const PerkEntry& entry) noexcept {
    PerkTerm term;
    switch (entry.function) {
    case PerkFnMultiplyValue:
        if (entry.dataType != PerkDataOneValue) { term.reason = "data_type"; return term; }
        if (!std::isfinite(entry.value0)) { term.reason = "value"; term.numeric = true; return term; }
        term.supported = true;
        term.factor = entry.value0;
        return term;
    case PerkFnMultiplyActorValueMult:
    case PerkFnMultiply1PlusActorValueMult: {
        if (entry.dataType != PerkDataTwoValue) { term.reason = "data_type"; return term; }
        if (!entry.layoutOk) { term.reason = "two_value_layout"; return term; }
        std::int32_t index = -1;
        if (!PerkActorValueIndex(entry.value0, index)) { term.reason = "av_index"; return term; }
        if (!std::isfinite(entry.value1)) { term.reason = "value"; term.numeric = true; return term; }
        if (!std::isfinite(entry.avRead)) { term.reason = "av_read"; term.numeric = true; return term; }
        const float scaled = entry.avRead * entry.value1;
        term.factor = entry.function == PerkFnMultiplyActorValueMult ? scaled : 1.0F + scaled;
        if (!std::isfinite(term.factor)) { term.reason = "value"; term.numeric = true; return term; }
        term.supported = true;
        return term;
    }
    default:
        term.reason = "function";
        return term;
    }
}

struct PerkTotals {
    // The passing player-side entries: the neutral merchant (step 7).
    float perkFactor{1.0F};
    // Every passing entry with the player as the target (step 6).
    float allPlayer{1.0F};
    std::uint32_t playerSidePassing{};
    std::uint32_t merchantSidePassing{};
    // A passing entry whose function, data type, layout or AV index is
    // unsupported (perk_function).
    std::uint32_t unsupportedPassing{};
    // A passing supported entry whose number is unusable (perk_value; audit
    // B-02). Not a receipt field: its entry rows show term null.
    std::uint32_t invalidValuePassing{};
};

// The two products, each in walk order (the order the engine multiplies).
inline PerkTotals CombinePerkEntries(std::span<const PerkEntry> entries) noexcept {
    PerkTotals totals;
    for (const PerkEntry& entry : entries) {
        if (!entry.passed) continue;
        const PerkTerm term = PerkEntryTerm(entry);
        if (!term.supported) {
            if (term.numeric) ++totals.invalidValuePassing;
            else ++totals.unsupportedPassing;
            continue;
        }
        totals.allPlayer *= term.factor;
        if (ClassifyPerkEntry(entry) == PerkSide::kPlayer) {
            totals.perkFactor *= term.factor;
            ++totals.playerSidePassing;
        } else {
            ++totals.merchantSidePassing;
        }
    }
    return totals;
}

// |engine / ours - 1|, or +inf when either side is not a positive finite number.
inline double PerkRelativeDifference(float engine, float ours) noexcept {
    if (!std::isfinite(engine) || !std::isfinite(ours) || !(engine > 0.0F) || !(ours > 0.0F))
        return std::numeric_limits<double>::infinity();
    return std::fabs(static_cast<double>(engine) / static_cast<double>(ours) - 1.0);
}

inline bool PerkSelfCheckEqual(float engine, float ours) noexcept {
    return PerkRelativeDifference(engine, ours) <= PerkSelfCheckTolerance;
}

// The perk price: S and f exactly as ComputeConvertPrice (the clamp of Speech
// at 0..100, f in float), m = perkFactor / f in float, m' = min(m, 1). The
// formula's own inputs (Haggling rank, AV 107 / 146) are not used: the walk
// already holds them as entries.
inline ConvertPrice ComputePerkConvertPrice(float perkFactor, const ShadowPriceInputs& in) noexcept {
    ConvertPrice price;
    if (!std::isfinite(in.barterMax) || !std::isfinite(in.barterMin) || !std::isfinite(in.speech) ||
        !std::isfinite(perkFactor) || !(perkFactor > 0.0F)) {
        return price;
    }
    float speech = in.speech;
    if (speech < 0.0F) speech = 0.0F;
    if (speech > 100.0F) speech = 100.0F;
    // Named hi/lo, not max/min: the Windows headers define those as macros.
    const float hi = in.barterMax;
    const float lo = in.barterMin;
    const float factor = hi - (hi - lo) * speech / 100.0F;
    if (!(factor > 0.0F) || !std::isfinite(factor)) return price;
    const float m = perkFactor / factor;
    if (!std::isfinite(m) || !(m > 0.0F)) return price;
    price.known = true;
    price.speech = speech;
    price.factor = factor;
    price.multiplier = m;
    price.clamped = m > 1.0F;
    price.capped = price.clamped ? 1.0F : m;
    return price;
}

// What the engine adapter read on one walk (PerkPriceEngine.h ReadPerkWalk).
struct PerkWalkFacts {
    // Step 1: the entry-point table holds two parameters "Perk Owner" /
    // "Target" and a value function.
    bool tableOk{};
    // Step 2: the walk ran on the player.
    bool walked{};
    bool overflow{};
    std::uint32_t visits{}, otherVisits{}, dropped{};
    std::uint32_t count{};
    std::array<PerkEntry, PerkWalkMaxEntries> entries{};
    // Step 6: HandleEntryPoint(kModSellPrices, player, player, &x), x = 1.0F.
    bool engineCalled{};
    float engineSelf{std::numeric_limits<float>::quiet_NaN()};
};

// Step 2 / A-04: the walk exceeded a bound (more than 64 entries, or a
// condition tab longer than PerkWalkMaxItemsPerTab whose shape is unknown).
inline bool PerkWalkOverflow(const PerkWalkFacts& walk) noexcept {
    if (walk.overflow || walk.count > PerkWalkMaxEntries) return true;
    for (std::uint32_t i = 0; i < walk.count; ++i) {
        if (!walk.entries[i].shapeComplete) return true;
    }
    return false;
}

// The source of one price snapshot (the Diagnostics row and the receipt).
enum class PriceSource : std::int32_t { kPerks = 0, kFormula = 1, kFallback = 2 };

constexpr const char* PriceSourceName(PriceSource source) noexcept {
    switch (source) {
    case PriceSource::kPerks: return "perks";
    case PriceSource::kFormula: return "formula";
    case PriceSource::kFallback: return "fallback";
    }
    return "formula";
}

// Design section 6 reason tokens, the first failure wins.
inline constexpr const char* PriceReasonNone = "none";
inline constexpr const char* PriceReasonOff = "off";
inline constexpr const char* PriceReasonDevForced = "dev_forced";
inline constexpr const char* PriceReasonTable = "perk_table";
inline constexpr const char* PriceReasonUnavailable = "perk_unavailable";
inline constexpr const char* PriceReasonOverflow = "perk_overflow";
inline constexpr const char* PriceReasonFunction = "perk_function";
inline constexpr const char* PriceReasonValue = "perk_value";
inline constexpr const char* PriceReasonMismatch = "perk_mismatch";

struct PriceSnapshot {
    // The price the press pays (perk price or the shipped formula).
    ConvertPrice price{};
    PriceSource source{PriceSource::kFormula};
    const char* reason{PriceReasonOff};
    bool switchOn{};
    // Filled only when the walk ran (switch ON, no developer key).
    bool walked{};
    PerkTotals totals{};
    float engineSelf{std::numeric_limits<float>::quiet_NaN()};
};

// The one price of a press or an in-menu batch (design 3.1 and 6). `walk` is
// null when the switch is OFF or the developer key forces the fallback (the
// engine side then never walks). The shipped formula is exactly
// ComputeConvertPrice of the same inputs, so OFF pays what v0.7.0 paid.
inline PriceSnapshot DecidePriceSnapshot(bool switchOn, bool devForced, const ShadowPriceInputs& inputs,
                                         const PerkWalkFacts* walk) noexcept {
    PriceSnapshot snapshot;
    snapshot.switchOn = switchOn;
    snapshot.price = ComputeConvertPrice(inputs);
    if (!switchOn) {
        snapshot.source = PriceSource::kFormula;
        snapshot.reason = PriceReasonOff;
        return snapshot;
    }
    snapshot.source = PriceSource::kFallback;
    if (devForced) {
        snapshot.reason = PriceReasonDevForced;
        return snapshot;
    }
    if (!walk || !walk->tableOk) {
        snapshot.reason = PriceReasonTable;
        return snapshot;
    }
    snapshot.walked = walk->walked;
    snapshot.engineSelf = walk->engineSelf;
    if (!walk->walked) {
        snapshot.reason = PriceReasonUnavailable;
        return snapshot;
    }
    if (PerkWalkOverflow(*walk)) {
        snapshot.reason = PriceReasonOverflow;
        return snapshot;
    }
    snapshot.totals = CombinePerkEntries(std::span<const PerkEntry>(walk->entries.data(), walk->count));
    const PerkTotals& totals = snapshot.totals;
    if (totals.unsupportedPassing > 0) {
        snapshot.reason = PriceReasonFunction;
        return snapshot;
    }
    if (totals.invalidValuePassing > 0 || !std::isfinite(totals.perkFactor) || !(totals.perkFactor > 0.0F) ||
        !std::isfinite(totals.allPlayer) || !(totals.allPlayer > 0.0F)) {
        snapshot.reason = PriceReasonValue;
        return snapshot;
    }
    if (!walk->engineCalled || !PerkSelfCheckEqual(walk->engineSelf, totals.allPlayer)) {
        snapshot.reason = PriceReasonMismatch;
        return snapshot;
    }
    const ConvertPrice perks = ComputePerkConvertPrice(totals.perkFactor, inputs);
    if (!perks.known) {
        snapshot.reason = PriceReasonValue;
        return snapshot;
    }
    snapshot.price = perks;
    snapshot.source = PriceSource::kPerks;
    snapshot.reason = PriceReasonNone;
    return snapshot;
}

}  // namespace js
