#pragma once
// B-02 oracle. Four candidate balances are sampled side by side because none of
// them has been shown to be the balance the vanilla sale actually debits:
//   merchant Actor inventory gold (J20/J21), the vendor faction VENC container
//   (J23), the engine merchant InventoryChanges (J24) and player gold.
// Every sample carries its own id, timestamp and epoch; a sample taken in a
// deferred task is a post-hoc reading, never an intermediate value of a mutation.
// r2: each candidate is an independent budgeted stage and every walk
// is bounded by both an entry cap and a deadline. A stage the budget cut off is
// named in "unsampled" rather than left looking like a real reading.
// r3 (probe-js01 crash): no CommonLib inventory helper is called any more.
// Actor::GetGoldAmount and TESObjectREFR::GetInventory are both gone; the four
// candidates share one walk that validates every pointer before dereferencing
// it. See Inventory.h for the rules and the design document's r3 section for the
// evidence.
// r5 (probe-js05 anomalies 2 and 3): the stages no longer share one task. Each
// one runs in its own UI task against its own deadline (Stages.h), so a long
// entry list can no longer starve the candidates behind it, and the walk stops
// as soon as it has counted gold. The merchant reference is resolved once, in
// its own stage, and every candidate reference it considered is recorded; the
// later stages look the adopted reference up again by FormID instead of holding
// an engine pointer across a task boundary.
// The design section 3.3 contract is unchanged: noInit stays true, so the probe
// never forces an engine side inventory into existence.
#include "Inventory.h"
#include "Walk.h"
#include "Merchant.h"
#include "Perf.h"
#include "Stages.h"
#include "StolenRule.h"
#include "receipt/Log.h"
#include <memory>
namespace js {
// One candidate reference, looked up and recorded exactly as it came back. An
// empty handle is refused before the lookup: the handle manager answers slot
// zero with the PlayerCharacter, which is how r4 came to call the player the
// merchant twelve times in a row (probe-js05 section 4.2).
inline void ResolveCandidate(std::uint32_t handle, MerchantCandidate& out) {
    out.handle = handle;
    if (!handle) return;
    const auto reference = RE::TESObjectREFR::LookupByHandle(handle);
    if (!reference) return;
    out.resolved = true;
    out.formID = reference->GetFormID();
    auto* actor = reference->As<RE::Actor>();
    out.isActor = actor != nullptr;
    if (actor) {
        auto* base = actor->GetActorBase();
        out.baseID = base ? base->GetFormID() : 0;
    } else {
        auto* base = reference->GetBaseObject();
        out.baseID = base ? base->GetFormID() : 0;
    }
    ClassifyMerchant(out);
}
inline RE::TESObjectREFR* ResolveMerchantContainer(RE::Actor* merchant) {
    if (!merchant) return nullptr;
    auto* faction = merchant->GetVendorFaction();
    return faction ? faction->vendorData.merchantContainer : nullptr;
}

// w-probe-r45: the two engine inputs of the stolen-goods acceptance
// gate (StolenRule.h), read once per rule run on the UI task. Read-only; no
// engine state is written and no pointer is kept.
//  - The merchant is resolved exactly as ResolveMerchantStage does (BarterMenu
//    target handle first, the dialogue speakers only with allowFallback), so
//    the gate and the VENC balance describe the same actor.
//  - Its current vendor faction comes from Actor::GetVendorFaction
//    (Actor.h:560), the call ResolveMerchantContainer already makes live. The
//    pinned implementation (Actor.cpp:590-597) computes a missing cache but
//    returns the value it read before computing, so a null answer is asked
//    once more; a second null means "no vendor faction" (input reads no).
//  - buysStolen is FACTION_VENDOR_DATA_VALUES::buysStolen (TESFaction.h:84),
//    the VENV byte at offset 8. buysNonStolen (offset 0xA) is not used: it is
//    0 on all 145 vanilla vendor factions (the design notes D5).
//  - The player's BypassVendorStolenCheck (ActorValues.h:101) is the current
//    value through Actor::AsActorValueOwner (Actor.h:725) and
//    ActorValueOwner::GetActorValue (ActorValueOwner.h:15), the pattern the
//    uncapper and enchant-uniques lanes use live.
inline StolenAcceptanceFacts ReadStolenAcceptanceFacts(bool allowFallback) {
    StolenAcceptanceFacts facts;
    MerchantCandidate target, speaker, lastSpeaker;
    ResolveCandidate(static_cast<std::uint32_t>(RE::BarterMenu::GetTargetRefHandle()), target);
    if (auto* topics = RE::MenuTopicManager::GetSingleton()) {
        ResolveCandidate(topics->speaker.native_handle(), speaker);
        ResolveCandidate(topics->lastSpeaker.native_handle(), lastSpeaker);
    }
    const auto source = ChooseMerchant(target, speaker, lastSpeaker, allowFallback);
    if (source != MerchantSource::kNone) {
        const MerchantCandidate& chosen = source == MerchantSource::kTargetHandle ? target
                                        : source == MerchantSource::kSpeaker      ? speaker
                                                                                 : lastSpeaker;
        auto* merchant = RE::TESForm::LookupByID<RE::Actor>(chosen.formID);
        facts.merchantResolved = merchant != nullptr;
        if (merchant) {
            auto* faction = merchant->GetVendorFaction();
            if (!faction) faction = merchant->GetVendorFaction();
            facts.vendorFactionKnown = faction != nullptr;
            facts.vendorBuysStolen = faction && faction->vendorData.vendorValues.buysStolen;
        }
    }
    if (auto* player = RE::PlayerCharacter::GetSingleton()) {
        if (auto* values = player->AsActorValueOwner()) {
            facts.bypassKnown = true;
            facts.bypassValue = values->GetActorValue(RE::ActorValue::kBypassVendorStolenCheck);
        }
    }
    return facts;
}

// Page state of a candidate pointer. One region is remembered between calls so
// a long entry list does not pay for a VirtualQuery per item: forms allocated
// next to each other share a region, and a pointer outside it is re-queried.
inline FormProbe ProbeForm(const void* object) noexcept {
    static thread_local std::uintptr_t cachedBase = 0;
    static thread_local std::uintptr_t cachedEnd = 0;
    FormProbe probe;
    probe.address = reinterpret_cast<std::uintptr_t>(object);
    if (!probe.address) return probe;
    if (cachedEnd && probe.address >= cachedBase && probe.address + 0x40 <= cachedEnd) {
        probe.queried = true;
        probe.committed = true;
        probe.readable = true;
        return probe;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(object, &info, sizeof(info)) != sizeof(info)) return probe;
    probe.queried = true;
    probe.committed = info.State == MEM_COMMIT;
    probe.guarded = (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0;
    probe.readable = (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                      PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                      PAGE_EXECUTE_WRITECOPY)) != 0;
    if (probe.committed && probe.readable && !probe.guarded) {
        cachedBase = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        cachedEnd = cachedBase + info.RegionSize;
    }
    return probe;
}

// Reads one bound object only after the probe accepted its pointer.
inline void ReadGoldEntry(RE::TESBoundObject* object, std::int32_t count, GoldEntry& out) noexcept {
    out.objectPresent = object != nullptr;
    if (!out.objectPresent) return;
    if (!FormPointerUsable(ProbeForm(object))) return;
    out.objectUsable = true;
    out.formID = object->GetFormID();
    out.formType = static_cast<std::uint8_t>(object->GetFormType());
    out.count = count;
}

inline GoldEntry ReadWalkIdentity(RE::TESBoundObject* object, bool entryPresent,
                                 std::int32_t count, std::uintptr_t gold,
                                 std::uintptr_t target, std::uint32_t targetFormID) {
    const auto address = reinterpret_cast<std::uintptr_t>(object);
    return ReadIdentityEntry(entryPresent, address, gold, target,
                             [address, gold, targetFormID, count](GoldEntry& read) {
                                 read.objectUsable = true;
                                 read.formID = address == gold ? GoldFormID : targetFormID;
                                 read.formType = address == gold ? MiscFormType : 1;
                                 read.count = count;
                             });
}

// The cursor contains only numbers. Engine pointers live for this task only.
inline WalkResult RunWalkChunk(WalkCursor& c, OracleSample& sample,
                               std::uint64_t deadlineNs, std::uint64_t nowMs,
                               std::uint64_t menu, std::uint64_t load,
                               std::uint64_t inventory, std::uint64_t currentEpoch,
                               bool open, bool referenceMatches,
                               std::uint32_t goldAllowance,
                               std::uint32_t& visits, std::uint32_t& goldVisits) {
    ++c.chunks;
    if (const auto* limit = WalkLimit(c, nowMs)) { c.abandonReason = limit; return WalkResult::kAbandon; }
    if (load != c.loadGeneration || currentEpoch != c.epoch) {
        c.abandonReason = "LOAD_CHANGED";
        return WalkResult::kAbandon;
    }
    if (menu != c.menuGeneration) {
        c.abandonReason = "MENU_CHANGED";
        return WalkResult::kAbandon;
    }
    if (DeadlineReached(NowNs(), deadlineNs)) return WalkResult::kContinue;
    // Resolve engine-owned forms for this task. The cursor retains only IDs.
    auto* goldForm = RE::TESForm::LookupByID<RE::TESObjectMISC>(GoldFormID);
    auto* targetForm = c.baseWanted
        ? RE::TESForm::LookupByID<RE::TESBoundObject>(c.base.targetFormID) : nullptr;
    if (c.goldWanted && !goldForm) { c.abandonReason = "GOLD_UNRESOLVED"; return WalkResult::kAbandon; }
    if (c.baseWanted && !targetForm) { c.abandonReason = "TARGET_UNRESOLVED"; return WalkResult::kAbandon; }
    const auto goldAddress = reinterpret_cast<std::uintptr_t>(
        static_cast<RE::TESBoundObject*>(goldForm));
    const auto targetAddress = reinterpret_cast<std::uintptr_t>(targetForm);
    auto* reference = c.refFormID == 0x14
        ? static_cast<RE::TESObjectREFR*>(RE::PlayerCharacter::GetSingleton())
        : RE::TESForm::LookupByID<RE::TESObjectREFR>(c.refFormID);
    auto* changes = reference ? reference->GetInventoryChanges(true) : nullptr;
    auto* list = changes ? changes->entryList : nullptr;
    auto* container = reference ? reference->GetContainer() : nullptr;
    if (container && !FormPointerUsable(ProbeForm(container))) container = nullptr;
    const auto count = container && container->containerObjects ? container->numContainerObjects : 0;
    std::uintptr_t anchor = 0;
    if (c.phase == WalkPhase::kChanges && c.ordinal && list) {
        std::uint32_t skipped = 0;
        for (auto it = list->begin(); it != list->end() && skipped < c.ordinal; ++it, ++skipped) {
            anchor = reinterpret_cast<std::uintptr_t>(std::addressof(*it));
            if (skipped % OracleDeadlineStride == 0 && DeadlineReached(NowNs(), deadlineNs))
                return WalkResult::kContinue;
        }
    }
    const auto* invalid = WalkInvalidation(c, menu, load, inventory, currentEpoch, open,
        reference != nullptr, referenceMatches,
        reinterpret_cast<std::uintptr_t>(changes), anchor, count);
    if (invalid) {
        if (WalkFatalInvalidation(invalid)) { c.abandonReason = invalid; return WalkResult::kAbandon; }
        RestartWalk(c, menu, load, inventory, currentEpoch, open,
                    reinterpret_cast<std::uintptr_t>(changes));
        if (const auto* limit = WalkLimit(c, nowMs)) { c.abandonReason = limit; return WalkResult::kAbandon; }
        return WalkResult::kContinue;
    }
    if (!reference) return WalkResult::kContinue;
    if (c.phase == WalkPhase::kChanges) {
        if (changes) c.base.changesListPresent = true;
        if (list && !c.ordinal) BeginGoldList(c.gold);
        std::uint32_t index = 0;
        if (list) for (auto it = list->begin(); it != list->end(); ++it, ++index) {
            if (index < c.ordinal) {
                if (index % OracleDeadlineStride == 0 && DeadlineReached(NowNs(), deadlineNs))
                    return WalkResult::kContinue;
                continue;
            }
            auto* entry = *it;
            const auto read = ReadWalkIdentity(entry ? entry->object : nullptr,
                                               entry != nullptr, entry ? entry->countDelta : 0,
                                               goldAddress, targetAddress, c.base.targetFormID);
            const bool goldActive = c.goldWanted && !c.goldDone;
            OfferWalkEntry(c, read, false, goldAllowance);
            if (goldActive) ++goldVisits;
            ++visits;
            ++c.ordinal;
            c.anchorAddr = reinterpret_cast<std::uintptr_t>(std::addressof(*it));
            if (c.base.truncated) { c.abandonReason = "ENTRY_LIMIT"; return WalkResult::kAbandon; }
            if (c.goldDone && c.baseDone) break;
            if (visits % OracleDeadlineStride == 0 && DeadlineReached(NowNs(), deadlineNs))
                return WalkResult::kContinue;
        }
        c.phase = WalkPhase::kContainer;
        c.ordinal = 0;
        c.goldDone = false;
        c.baseDone = !c.baseWanted;
    }
    if (container) c.base.containerPresent = true;
    if (count && !c.ordinal) BeginGoldList(c.gold);
    for (; c.ordinal < count; ++c.ordinal) {
        auto* entry = container->containerObjects[c.ordinal];
        if (entry && !FormPointerUsable(ProbeForm(entry))) {
            c.abandonReason = "ENTRY_UNREADABLE";
            return WalkResult::kAbandon;
        }
        const bool entryPresent = entry != nullptr;
        const auto read = ReadWalkIdentity(entryPresent ? entry->obj : nullptr,
                                           entryPresent, entryPresent ? entry->count : 0,
                                           goldAddress, targetAddress, c.base.targetFormID);
        const bool goldActive = c.goldWanted && !c.goldDone;
        OfferWalkEntry(c, read, true, goldAllowance);
        if (goldActive) ++goldVisits;
        ++visits;
        if (c.base.truncated) { c.abandonReason = "ENTRY_LIMIT"; return WalkResult::kAbandon; }
        if (c.goldDone && c.baseDone) { ++c.ordinal; break; }
        if (visits % OracleDeadlineStride == 0 && DeadlineReached(NowNs(), deadlineNs)) {
            ++c.ordinal;
            return WalkResult::kContinue;
        }
    }
    c.phase = WalkPhase::kDone;
    sample.rejectedEntries += c.gold.rejected;
    if (c.gold.completed) ++sample.earlyExits;
    return WalkResult::kDone;
}
// MaxOracleEntries and OracleDeadlineStride are defined in Inventory.h next to
// the rules they bound.

// The InventoryChanges entry list of one reference. noInit stays true, so the
// engine side inventory is never forced into existence by the probe (design
// section 3.3): a reference without ExtraContainerChanges reports -1 instead.
inline void WalkChanges(RE::TESObjectREFR* reference, std::uint64_t deadlineNs, GoldTally& tally,
                        std::uint32_t maxEntries) {
    if (!reference) return;
    auto* changes = reference->GetInventoryChanges(true);
    if (!changes || !changes->entryList) return;
    BeginGoldList(tally);
    for (auto* entry : *changes->entryList) {
        GoldEntry read;
        read.entryPresent = entry != nullptr;
        if (read.entryPresent) ReadGoldEntry(entry->object, entry->countDelta, read);
        if (OfferGoldEntry(tally, read, maxEntries) == GoldStep::kStop) break;
        if (tally.scanned % OracleDeadlineStride == 0 && DeadlineReached(NowNs(), deadlineNs)) {
            tally.truncated = true;
            break;
        }
    }
}

// The base container record behind the reference. CommonLib's GetInventory used
// to fold this in and the VENC vendor chest keeps part of its stock here, so the
// candidate would under-report without it. The array is walked directly, under
// the same pointer rules, instead of through ForEachContainerObject.
inline void WalkBaseContainer(RE::TESObjectREFR* reference, std::uint64_t deadlineNs, GoldTally& tally,
                              std::uint32_t maxEntries) {
    if (!reference) return;
    auto* container = reference->GetContainer();
    if (!container || !FormPointerUsable(ProbeForm(container))) return;
    auto** objects = container->containerObjects;
    const auto count = container->numContainerObjects;
    if (!objects || !count) return;
    BeginGoldList(tally);
    for (std::uint32_t i = 0; i < count; ++i) {
        auto* entry = objects[i];
        GoldEntry read;
        read.entryPresent = entry != nullptr && FormPointerUsable(ProbeForm(entry));
        if (read.entryPresent) ReadGoldEntry(entry->obj, entry->count, read);
        if (OfferGoldEntry(tally, read, maxEntries) == GoldStep::kStop) break;
        if (tally.scanned % OracleDeadlineStride == 0 && DeadlineReached(NowNs(), deadlineNs)) {
            tally.truncated = true;
            break;
        }
    }
}

// One candidate balance: the changes list plus the base container, exactly the
// pair CommonLib's GetInventory merged, with every dereference checked first.
// r5: each walk returns as soon as it has counted the gold entry, so the cost is
// the distance to gold rather than the length of the list.
// w-probe-r8 (R8-1): the per-base count is no longer taken here. Carrying a
// target forced this walk to run to the end of the list looking for it, which
// is what made every R-W3 issue_before sample TRUNCATED; CountBaseObject below
// answers that question on its own.
inline std::int32_t CountGold(RE::TESObjectREFR* reference, std::uint64_t deadlineNs, OracleSample& sample,
                              std::uint32_t maxEntries) {
    if (!reference) return -1;
    GoldTally tally;
    WalkChanges(reference, deadlineNs, tally, maxEntries);
    if (!tally.truncated) WalkBaseContainer(reference, deadlineNs, tally, maxEntries);
    return FoldGoldTally(sample, tally);
}

// w-probe-r8 (design rulings R8-1 and R8-3): the base count oracle. One pass over
// the reference's InventoryEntryData list for the quoted base (countDelta) plus
// that object's rows in its base TESContainer, with the same pointer rules the
// gold walk uses. Members verified against vendor/commonlibsse-ng:
//   InventoryEntryData::object / countDelta  (include/RE/I/InventoryEntryData.h)
//   InventoryChanges::entryList              (include/RE/I/InventoryChanges.h)
//   TESContainer::containerObjects / numContainerObjects and
//   ContainerObject::obj / count             (include/RE/T/TESContainer.h)
//   TESObjectREFR::GetInventoryChanges(bool) / GetContainer()
//                                            (include/RE/T/TESObjectREFR.h)
// noInit stays true, so this never forces an engine side inventory into
// existence (design section 3.3). It uses no deadline or visit allowance, but
// the dedicated hard bound fails closed instead of returning a partial count.
inline std::int32_t CountBaseObject(RE::TESObjectREFR* reference, std::uint32_t targetFormID,
                                    const char** source, std::uint32_t* visited,
                                    bool* truncatedOut) noexcept {
    if (source) *source = CountSourceNone;
    if (visited) *visited = 0;
    if (truncatedOut) *truncatedOut = false;
    if (!reference || !targetFormID) return -1;
    BaseCountTally tally;
    tally.targetFormID = targetFormID;
    auto* changes = reference->GetInventoryChanges(true);
    if (changes) {
        tally.changesListPresent = true;
        if (changes->entryList) {
            for (auto* entry : *changes->entryList) {
                GoldEntry read;
                read.entryPresent = entry != nullptr;
                if (read.entryPresent) ReadGoldEntry(entry->object, entry->countDelta, read);
                if (OfferBaseCountEntry(tally, read, false) == BaseCountStep::kStop) break;
                if (BaseCountLimitReached(tally, MaxBaseCountEntries)) {
                    tally.truncated = true;
                    break;
                }
            }
        }
    }
    auto* container = reference->GetContainer();
    if (container && FormPointerUsable(ProbeForm(container))) {
        tally.containerPresent = true;
        auto** objects = container->containerObjects;
        const auto count = container->numContainerObjects;
        if (objects && count && !tally.truncated) {
            for (std::uint32_t i = 0; i < count; ++i) {
                if (BaseCountLimitReached(tally, MaxBaseCountEntries)) {
                    tally.truncated = true;
                    break;
                }
                auto* entry = objects[i];
                GoldEntry read;
                read.entryPresent = entry != nullptr && FormPointerUsable(ProbeForm(entry));
                if (read.entryPresent) ReadGoldEntry(entry->obj, entry->count, read);
                if (OfferBaseCountEntry(tally, read, true) == BaseCountStep::kStop) break;
            }
        }
    }
    if (source) *source = BaseCountSource(tally);
    if (visited) *visited = tally.visited;
    if (truncatedOut) *truncatedOut = tally.truncated;
    return BaseCountTotal(tally);
}

inline std::int32_t CountMerchantInventoryGold(std::uint64_t deadlineNs, OracleSample& sample,
                                               std::uint32_t maxEntries) {
    auto* goldForm = RE::TESForm::LookupByID<RE::TESObjectMISC>(GoldFormID);
    if (!goldForm) { sample.unsampled |= kUnsampledMerchantInventory; return -1; }
    const auto goldAddress = reinterpret_cast<std::uintptr_t>(
        static_cast<RE::TESBoundObject*>(goldForm));
    auto* data = RE::TESDataHandler::GetSingleton();
    auto* changes = data ? data->GetMerchantInventory() : nullptr;
    if (!changes || !changes->entryList) return -1;
    GoldTally tally;
    tally.listPresent = true;
    for (auto* entry : *changes->entryList) {
        const auto read = ReadWalkIdentity(entry ? entry->object : nullptr,
                                           entry != nullptr, entry ? entry->countDelta : 0,
                                           goldAddress, 0, 0);
        if (OfferGoldEntry(tally, read, maxEntries) == GoldStep::kStop) break;
        if (tally.scanned % OracleDeadlineStride == 0 && DeadlineReached(NowNs(), deadlineNs)) {
            tally.truncated = true;
            break;
        }
    }
    return FoldGoldTally(sample, tally);
}

inline bool ReadSpeech(OracleSample& sample) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    if (!player) return false;
    auto* skills = player->GetPlayerRuntimeData().skills;
    if (!skills || !skills->data) return false;
    const auto& entry = skills->data->skills[RE::PlayerCharacter::PlayerSkills::Data::Skills::kSpeech];
    sample.speechLevel = entry.level;
    sample.speechXp = entry.xp;
    sample.speechThreshold = entry.levelThreshold;
    return true;
}

// Stage 0. Resolves the merchant once for the whole sample and records every
// candidate it considered, adopted or not. Only FormIDs are kept: the stages
// that follow run in later tasks and look the reference up again, because an
// engine pointer is never held across a task boundary (design section 4).
inline void ResolveMerchantStage(OracleSample& sample, bool allowFallback) {
    MerchantCandidate target, speaker, lastSpeaker;
    ResolveCandidate(static_cast<std::uint32_t>(RE::BarterMenu::GetTargetRefHandle()), target);
    // The dialogue speaker is the alternative path the CommonLib headers offer
    // for "who is the player talking to": MenuTopicManager keeps the current and
    // the last speaker as ObjectRefHandles. Both are read through the same
    // handle lookup as the primary path, so no new engine call is introduced.
    if (auto* topics = RE::MenuTopicManager::GetSingleton()) {
        ResolveCandidate(topics->speaker.native_handle(), speaker);
        ResolveCandidate(topics->lastSpeaker.native_handle(), lastSpeaker);
    }
    sample.merchantHandle = target.handle;
    sample.handleRefID = target.formID;
    sample.handleBaseID = target.baseID;
    sample.handleIsPlayer = target.isPlayer;
    sample.speakerHandle = speaker.handle;
    sample.speakerID = speaker.formID;
    sample.lastSpeakerHandle = lastSpeaker.handle;
    sample.lastSpeakerID = lastSpeaker.formID;
    const auto source = ChooseMerchant(target, speaker, lastSpeaker, allowFallback);
    sample.merchantSource = MerchantSourceName(source);
    sample.fallbackUsed = source == MerchantSource::kSpeaker || source == MerchantSource::kLastSpeaker;
    sample.merchantResolved = source != MerchantSource::kNone;
    if (!sample.merchantResolved) {
        sample.unsampled |= kUnsampledMerchantActor | kUnsampledMerchantContainer;
        return;
    }
    const MerchantCandidate& chosen = source == MerchantSource::kTargetHandle ? target
                                    : source == MerchantSource::kSpeaker      ? speaker
                                                                             : lastSpeaker;
    sample.merchantActorID = chosen.formID;
    sample.merchantBaseID = chosen.baseID;
    auto* container = ResolveMerchantContainer(RE::TESForm::LookupByID<RE::Actor>(chosen.formID));
    sample.containerResolved = container != nullptr;
    if (container) sample.merchantContainerID = container->GetFormID();
    else sample.unsampled |= kUnsampledMerchantContainer;
}

inline void StageMerchantActorGold(OracleSample& sample, std::uint64_t deadlineNs,
                                   std::uint32_t maxEntries) {
    auto* merchant = sample.merchantActorID ? RE::TESForm::LookupByID<RE::Actor>(sample.merchantActorID)
                                            : nullptr;
    if (!merchant) { sample.unsampled |= kUnsampledMerchantActor; return; }
    sample.merchantActorGold = CountGold(merchant, deadlineNs, sample, maxEntries);
    sample.merchantActorGoldSource = "changes+base_container";
}
inline void StageMerchantContainerGold(OracleSample& sample, std::uint64_t deadlineNs,
                                       std::uint32_t maxEntries) {
    auto* container = sample.merchantContainerID
                          ? RE::TESForm::LookupByID<RE::TESObjectREFR>(sample.merchantContainerID)
                          : nullptr;
    if (!container) { sample.unsampled |= kUnsampledMerchantContainer; return; }
    sample.merchantContainerGold = CountGold(container, deadlineNs, sample, maxEntries);
    sample.merchantContainerGoldSource = "changes+base_container";
}

// w-probe-r1: the adopted merchant, looked up again by FormID at the end of
// the sample. It proves the reference the six stages shared is still there; it
// is not a claim that the menu still points at the same merchant, which is
// what the menu generation in the sample window is for.
inline std::uint32_t ReResolveMerchant(std::uint32_t formID) {
    if (!formID) return 0;
    auto* actor = RE::TESForm::LookupByID<RE::Actor>(formID);
    return actor ? actor->GetFormID() : 0;
}

// w-probe-r8 (R8-3): the base count of the stage about to run, taken before the
// stage's visit allowance is even consulted and charged to no cap. A budget
// that stops the gold walk can no longer turn a known count into a -1, and a
// stage the allowance refuses still publishes the count the issuance path
// compares. Slot 1 is the player side and slot 3 the VENC side; every other
// slot has no base count to take. Idempotent, so a repeated call is free.
inline bool RunOracleBaseCount(unsigned slot, OracleSample& sample) noexcept {
    if (!sample.targetBase) return false;
    try {
        if (slot == 1) {
            if (sample.playerBaseCount >= 0) return false;
            bool countTruncated = false;
            sample.playerBaseCount = CountBaseObject(RE::PlayerCharacter::GetSingleton(),
                                                     sample.targetBase, &sample.countSource,
                                                     &sample.baseCountVisits, &countTruncated);
            return countTruncated;
        } else if (slot == 3) {
            if (sample.vendorBaseCount >= 0 || !sample.merchantContainerID) return false;
            std::uint32_t visited = 0;
            bool countTruncated = false;
            sample.vendorBaseCount =
                CountBaseObject(RE::TESForm::LookupByID<RE::TESObjectREFR>(sample.merchantContainerID),
                                sample.targetBase, &sample.vendorCountSource, &visited, &countTruncated);
            sample.baseCountVisits += visited;
            return countTruncated;
        }
    } catch (...) {
        // A throwing lookup leaves the count at its "not measured" default; it
        // never marks the sample truncated, because no reading was cut short.
    }
    return false;
}

// One stage of one sample. A throwing candidate leaves its own field at the "not
// read" default and names itself in unsampled; the stages behind it still run,
// which is the whole point of the split. This catches C++ exceptions only:
// /EHsc does not turn an access violation into one, which is why r3 stopped
// calling the helper that raised one instead of relying on this guard.
inline void RunOracleSlot(unsigned slot, OracleSample& sample, std::uint64_t deadlineNs,
                          bool allowFallback, std::uint32_t maxEntries) noexcept {
    try {
        switch (slot) {
        case 0:
            ResolveMerchantStage(sample, allowFallback);
            sample.startMerchantID = sample.merchantActorID;
            break;
        case 1:
            sample.playerGold =
                CountGold(RE::PlayerCharacter::GetSingleton(), deadlineNs, sample, maxEntries);
            sample.playerGoldSource = "changes+base_container";
            break;
        case 2: StageMerchantActorGold(sample, deadlineNs, maxEntries); break;
        case 3: StageMerchantContainerGold(sample, deadlineNs, maxEntries); break;
        case 4:
            sample.merchantInventoryGold = CountMerchantInventoryGold(deadlineNs, sample, maxEntries);
            sample.merchantInventorySource = "data_handler_merchant_inventory";
            break;
        case 5:
            sample.speechResolved = ReadSpeech(sample);
            if (sample.speechResolved) sample.speechSource = "player_skills_data";
            break;
        default: return;
        }
    } catch (...) {
        sample.truncated = true;
        sample.unsampled |= OracleSlotFailureBits(slot);
    }
    ++sample.stagesRun;
}
}
