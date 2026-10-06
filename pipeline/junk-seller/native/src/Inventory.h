#pragma once
// Engine free half of the B-02 gold walk (r3).
//
// Why this file exists: probe-js01 crashed the game the moment BarterMenu
// opened. The access violation was inside CommonLib's Actor::GetGoldAmount,
// not in the probe's own arithmetic, so r3 stops calling that helper (and
// TESObjectREFR::GetInventory with it) and walks the entry lists itself. Every
// decision the walk makes is stated here, with no engine type in sight, so the
// offline harness can feed it the exact broken shapes the crash log showed:
// a null entry, a null object, and a non-null object pointer that cannot be
// dereferenced.
//
// Nothing in this header dereferences anything. The caller reads one entry,
// records what it found, and hands the record over; the tally decides.
#include "Merchant.h"
#include "receipt/Log.h"
#include <cstdint>
namespace js {
// Gold is Skyrim.esm 0x0000000F and it is a MISC form. Both are checked: a
// FormID alone is a four byte comparison that a garbage page can pass.
constexpr std::uint32_t GoldFormID = 0x0000000Fu;
// FormType::Misc == 0x20 and FormType::Max == 0x8A in CommonLibSSE-NG
// (include/RE/F/FormTypes.h). A byte outside the enum means the pointer is not
// a form, whatever else it may be.
constexpr std::uint8_t MiscFormType = 0x20u;
constexpr std::uint8_t MaxFormType = 0x8Au;
// The lowest 64 KiB of the address space is never mapped on Windows, and every
// TESForm allocation is at least 8 byte aligned. The crash log's rcx was
// 0x00160000: aligned and above the reserved region, so the range test alone is
// not enough and the page state below is what actually decides.
constexpr std::uintptr_t MinFormAddress = 0x10000u;
// Walk bounds (B-02). Both live here so the offline harness can drive the
// same caps the runtime uses.
constexpr std::uint32_t MaxOracleEntries = 262144;
constexpr std::uint32_t OracleDeadlineStride = 16;
// The dedicated base-count lookup is exempt from the xList/oracle allowance,
// but it still has a hard bound so a malformed or unusually large entry map
// cannot hold the UI task forever.
constexpr std::uint32_t MaxBaseCountEntries = MaxOracleEntries;

// Page state of a candidate object, filled in with VirtualQuery before anything
// dereferences it. Same shape and the same style of rule as the executable-memory target, so the
// decision is testable without Windows.
struct FormProbe {
    std::uintptr_t address{};
    bool queried{};
    bool committed{};
    bool readable{};
    bool guarded{};
};
inline bool FormPointerUsable(const FormProbe& probe) noexcept {
    if (probe.address < MinFormAddress) return false;
    if (probe.address % alignof(std::uint64_t)) return false;
    if (!probe.queried) return false;
    if (!probe.committed || !probe.readable || probe.guarded) return false;
    return true;
}

// One entry exactly as the walk found it.
struct GoldEntry {
    bool entryPresent{};
    bool objectPresent{};
    bool objectUsable{};
    std::uint32_t formID{};
    std::uint8_t formType{};
    std::int32_t count{};
};
// F1c: an unrelated object is classified by pointer identity alone. Only a
// gold or quoted-base match reaches readMatched; no other object is probed.
template <class ReadMatched>
inline GoldEntry ReadIdentityEntry(bool entryPresent, std::uintptr_t object,
                                   std::uintptr_t gold, std::uintptr_t target,
                                   ReadMatched&& readMatched) {
    GoldEntry read;
    read.entryPresent = entryPresent;
    read.objectPresent = object != 0;
    if (!entryPresent || !object) return read;
    if (object == gold || (target && object == target)) {
        readMatched(read);
    } else {
        // A nonmatching pointer is deliberately never classified as a form.
        // These values mean only that neither tally should reject it.
        read.objectUsable = true;
        read.formType = 1;
    }
    return read;
}
enum class GoldStep : unsigned { kContinue = 0, kStop = 1 };

// Running total for one candidate balance. "listPresent" stays false when the
// walk never found a list at all; that candidate then reports -1 instead of a
// zero that would read like a real balance.
struct GoldTally {
    std::int32_t total{};
    std::uint32_t scanned{}, counted{}, rejected{};
    bool listPresent{};
    bool truncated{};
    // r5 / probe-js05 anomaly 2: the walk stopped because it had the answer,
    // not because it ran out of budget. The two must never be confused in the
    // receipt: truncated means the reading is incomplete, completed means the
    // rest of the list could not have changed it.
    bool completed{};
    // Per list: whether gold has been seen in the list being walked now.
    // BeginGoldList resets it at the start of each list.
    // w-probe-r8 (R8-1): the gold walk carries no target any more. R-W3
    // measured every issue_before sample coming back TRUNCATED because the
    // walk kept going past the gold entry looking for the quoted base and ran
    // into the 2 ms stage deadline; the base count now has its own lookup
    // (BaseCountTally below) and this walk stops at gold again.
    bool listGold{};
};

inline void BeginGoldList(GoldTally& tally) noexcept {
    tally.listPresent = true;
    tally.listGold = false;
}

// Accepts one entry and says whether the walk may continue. An entry that is
// missing, has no object, or has a matching object the reader refused is
// counted as rejected and contributes nothing; it never stops the walk, because
// one broken slot must not hide the rest of the inventory.
inline GoldStep OfferGoldEntry(GoldTally& tally, const GoldEntry& entry,
                               std::uint32_t maxEntries) noexcept {
    ++tally.scanned;
    bool isGold = false;
    if (!entry.entryPresent || !entry.objectPresent || !entry.objectUsable) {
        ++tally.rejected;
    } else if (entry.formType == MiscFormType && entry.formID == GoldFormID) {
        tally.total += entry.count;
        ++tally.counted;
        isGold = true;
    } else if (entry.formType == 0 || entry.formType >= MaxFormType) {
        // A matching object failed validation. Unmatched objects are never
        // read, so their memory or form status is intentionally unknown.
        ++tally.rejected;
    }
    if (isGold) tally.listGold = true;
    if (maxEntries && tally.scanned >= maxEntries) {
        tally.truncated = true;
        return GoldStep::kStop;
    }
    // r5 / probe-js05 anomaly 2: one list holds gold in one entry, so once that
    // entry has been counted the remaining entries cannot change the total. The
    // walk stops here instead of paying for the rest of the list, which is what
    // spent the whole 2 ms budget on the player alone (640-704 entries scanned
    // for a value that was already in hand).
    if (isGold) {
        tally.completed = true;
        return GoldStep::kStop;
    }
    return GoldStep::kContinue;
}
inline std::int32_t GoldTotal(const GoldTally& tally) noexcept {
    return tally.listPresent ? tally.total : -1;
}

// ---------------------------------------------------------------------------
// w-probe-r8 (design rulings R8-1 and R8-3): the per-base count oracle.
//
// R-W3 measured issued=0 on a hoard save: every issue_before sample said
// TRUNCATED / sampleValid=false / playerBaseCount=-1, so the issuance path
// refused with BEFORE_UNAVAILABLE and nothing could ever be sold. The count was
// a by-product of the gold walk, which had to keep scanning past the gold entry
// until it also met the quoted base, and on a large inventory that walk ran out
// of its stage deadline first.
//
// The count is taken on its own here: the reference's InventoryEntryData for
// the quoted base contributes countDelta, that object's rows in the reference's
// base TESContainer contribute their count, and the two are added. This is
// exactly what CommonLib's TESObjectREFR::GetInventory folds together
// (vendor/commonlibsse-ng/src/RE/T/TESObjectREFR.cpp, GetInventory: entryList
// entries give countDelta, ForEachContainerObject adds ContainerObject::count),
// minus the leveled-item de-duplication, which cannot be done without walking
// an entry's extra data lists and does not apply to the concrete base object a
// barter selection quotes.
//
// Nothing here enumerates an extra data list or consumes a visit allowance. A
// lookup that reaches its dedicated bound is unavailable; an object the two
// readable records do not mention is a measured zero (R8-1).
struct BaseCountTally {
    std::uint32_t targetFormID{};
    // The two halves are kept apart so the receipt can say which record spoke.
    std::int32_t changesDelta{};
    std::int32_t containerCount{};
    std::uint32_t visited{}, rejected{};
    bool changesListPresent{}, containerPresent{};
    bool entryFound{}, containerFound{};
    bool truncated{};
};
enum class BaseCountStep : unsigned { kContinue = 0, kStop = 1 };

inline bool BaseCountLimitReached(const BaseCountTally& tally,
                                  std::uint32_t maxEntries) noexcept {
    return maxEntries && tally.visited >= maxEntries;
}

// Accepts one entry of either record and says whether the walk may continue.
// The engine keeps at most one InventoryEntryData per bound object, so a
// match there ends the pass over that record. The base TESContainer has no
// such uniqueness rule: CommonLib's GetInventory (audit-r2 B-01) accumulates
// every ContainerObject whose obj equals the quoted base, so this keeps
// walking the container record after a match instead of stopping at the
// first one.
inline BaseCountStep OfferBaseCountEntry(BaseCountTally& tally, const GoldEntry& entry,
                                         bool baseContainer) noexcept {
    ++tally.visited;
    if (!entry.entryPresent || !entry.objectPresent || !entry.objectUsable) {
        ++tally.rejected;
        return BaseCountStep::kContinue;
    }
    if (entry.formType == 0 || entry.formType >= MaxFormType) {
        // Readable memory that is not a form; the same rule the gold walk uses.
        ++tally.rejected;
        return BaseCountStep::kContinue;
    }
    if (!tally.targetFormID || entry.formID != tally.targetFormID) return BaseCountStep::kContinue;
    if (baseContainer) {
        tally.containerCount += entry.count;
        tally.containerFound = true;
        return BaseCountStep::kContinue;
    }
    tally.changesDelta += entry.count;
    tally.entryFound = true;
    return BaseCountStep::kStop;
}

// True once at least one of the two records was readable. Nothing readable at
// all is the only case that yields "not measured".
inline bool BaseCountKnown(const BaseCountTally& tally) noexcept {
    return tally.targetFormID && !tally.truncated &&
           (tally.changesListPresent || tally.containerPresent);
}
// The count the issuance path compares, or -1 when neither record was readable.
// countDelta is a signed delta against the base container, so the sum is
// clamped at zero rather than allowed to read as a negative holding.
inline std::int32_t BaseCountTotal(const BaseCountTally& tally) noexcept {
    if (!BaseCountKnown(tally)) return -1;
    const std::int64_t total =
        static_cast<std::int64_t>(tally.containerCount) + static_cast<std::int64_t>(tally.changesDelta);
    if (total <= 0) return 0;
    if (total > 2147483647LL) return 2147483647;
    return static_cast<std::int32_t>(total);
}
inline const char* BaseCountSource(const BaseCountTally& tally) noexcept {
    if (!BaseCountKnown(tally)) return CountSourceNone;
    return (tally.entryFound || tally.containerFound) ? CountSourceEntryDelta : CountSourceAbsent;
}
// Folds one candidate's tally into the sample the receipt carries.
inline std::int32_t FoldGoldTally(OracleSample& sample, const GoldTally& tally) noexcept {
    sample.scannedEntries += tally.scanned;
    sample.rejectedEntries += tally.rejected;
    if (tally.truncated) sample.truncated = true;
    if (tally.completed) ++sample.earlyExits;
    return GoldTotal(tally);
}

// Reason code for the row that carries this sample. A sample that could not
// read a single balance says ORACLE_UNAVAILABLE rather than letting the -1
// defaults look like readings; a sample that had to refuse an entry says
// ORACLE_DEGRADED. Returns nullptr when the sample is clean.
inline const char* OracleReason(const OracleSample& sample) noexcept {
    if (!sample.attached) return nullptr;
    if (sample.playerGold < 0 && sample.merchantActorGold < 0 &&
        sample.merchantContainerGold < 0 && sample.merchantInventoryGold < 0) {
        return "ORACLE_UNAVAILABLE";
    }
    // r5 / probe-js05 anomaly 3: a sample that could not name a merchant says so
    // on its own row. r4 read the player back as the merchant and still called
    // itself resolved, so the two merchant side candidates looked like empty
    // readings rather than a failure to find the reference at all.
    if (!sample.merchantResolved) return MerchantUnresolvedReason;
    if (sample.rejectedEntries) return "ORACLE_DEGRADED";
    return nullptr;
}
}
