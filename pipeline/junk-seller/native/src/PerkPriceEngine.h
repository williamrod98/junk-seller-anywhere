#pragma once
// v0.8.0 (the barter-price design r2 section 3.1 steps
// 1-6): the engine half of "Price follows perks". It reads the facts the pure
// PerkPrice.h decides on, with exactly the calls the PROBE-JS-PERKPRICE DLL
// made live on 1.6.1170 (pipeline/junk-seller/probes/perkprice/Plugin.cpp,
// a live test log: D1-D4 and D6, no CTD):
//
//   1. GetEntryPoint(kModSellPrices): two parameter name slots ("Perk Owner",
//      "Target"; AE stores char* names, not the header's 16-byte records) and
//      a value function;
//   2. player->ForEachPerkEntry(kModSellPrices, visitor): the visitor only
//      collects (at most PerkWalkMaxEntries; more is an overflow), no
//      condition is evaluated during the walk;
//   3. per entry: the function, the one-value float, the two-value float[2]
//      at +0x08 only behind the engine's own two-value vtable, the condition
//      shape (item counts per tab and whether a first-tab item runs on the
//      target; a tab longer than the per-tab bound is incomplete), the
//      actor value the two-value functions read on the player;
//   4. the conditions with the player in both roles (TESCondition::IsTrue
//      with two valid actor references, never a null one);
//   6. HandleEntryPoint(kModSellPrices, player, player, &x) with x = 1.0F.
//
// Main thread only (a UI task or a main-thread task). Reads only: nothing here
// writes an item, a perk, an actor value, gold or the save. Included only by
// Probe.cpp, after PCH.h.
#include "PerkPrice.h"

namespace js {

// Design 3.1 step 1 (the SR Entry49Contract pattern, EPARGS r2-r6 start rows).
inline bool PerkPriceTableOk() noexcept {
    const auto* table = RE::BGSEntryPoint::GetEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModSellPrices);
    if (!table || table->parameters.count != 2 || !table->parameters.data ||
        table->functionType != RE::BGSEntryPointFunction::ENTRY_POINT_FUNCTION_TYPE::kValue) {
        return false;
    }
    const auto* names = reinterpret_cast<const char* const*>(table->parameters.data);
    return names[0] && names[1] && std::string_view(names[0]) == "Perk Owner" &&
           std::string_view(names[1]) == "Target";
}

// The engine's own two-value data class (Offsets_VTABLE.h, AE id 195194; the
// probe matched it on every live two-value entry).
inline std::uintptr_t PerkTwoValueVtable() noexcept {
    static const std::uintptr_t address = RE::VTABLE_BGSEntryPointFunctionDataTwoValue[0].address();
    return address;
}

// Step 2: the walk only collects the kModSellPrices entries.
class PerkSellCollector final : public RE::PerkEntryVisitor {
public:
    RE::BSContainer::ForEachResult Visit(RE::BGSPerkEntry* base) override {
        ++visits;
        auto* entry = (base && base->GetType() == RE::PERK_ENTRY_TYPE::kEntryPoint)
            ? static_cast<RE::BGSEntryPointPerkEntry*>(base) : nullptr;
        if (!entry || !entry->IsEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModSellPrices)) {
            ++otherVisits;
            return RE::BSContainer::ForEachResult::kContinue;
        }
        if (count >= PerkWalkMaxEntries) {
            overflow = true;
            ++dropped;
            return RE::BSContainer::ForEachResult::kContinue;
        }
        found[count++] = entry;
        return RE::BSContainer::ForEachResult::kContinue;
    }
    std::array<RE::BGSEntryPointPerkEntry*, PerkWalkMaxEntries> found{};
    std::uint32_t count{};
    std::uint32_t visits{};
    std::uint32_t otherVisits{};
    std::uint32_t dropped{};
    bool overflow{};
};

// Step 3: one entry's function, data, condition shape and actor value read.
inline void ReadPerkSellEntry(RE::BGSEntryPointPerkEntry* source, RE::Actor* player, PerkEntry& entry) {
    entry = PerkEntry{};
    entry.perk = source->perk ? source->perk->GetFormID() : 0;
    entry.function = static_cast<std::uint8_t>(source->entryData.function.underlying());
    if (auto* data = source->functionData) {
        entry.dataType = static_cast<std::int32_t>(data->GetType());
        if (entry.dataType == PerkDataOneValue) {
            entry.value0 = static_cast<const RE::BGSEntryPointFunctionDataOneValue*>(data)->data;
        } else if (entry.dataType == PerkDataTwoValue) {
            const std::uintptr_t vtable = *reinterpret_cast<const std::uintptr_t*>(data);
            entry.layoutOk = vtable == PerkTwoValueVtable();
            if (entry.layoutOk) {
                const auto* values = reinterpret_cast<const float*>(reinterpret_cast<const std::byte*>(data) + 0x08);
                entry.value0 = values[0];
                entry.value1 = values[1];
            }
        }
    }
    // At most PerkWalkMaxItemsPerTab items per tab; a longer tab leaves the
    // shape incomplete and the price falls back.
    const auto tabs = static_cast<std::uint32_t>(source->conditions.size());
    for (std::uint32_t tab = 0; tab < tabs; ++tab) {
        CountPerkConditionTab(source->conditions[tab].head, tab == 0, entry,
                              [](const RE::TESConditionItem& item) {
                                  return item.data.object.underlying() == PerkRunOnTarget;
                              });
    }
    const std::int32_t av = PerkEntryActorValue(entry);
    if (PerkUsesActorValue(entry.function) && av >= 0) {
        if (auto* values = player->AsActorValueOwner()) {
            entry.avRead = values->GetActorValue(static_cast<RE::ActorValue>(av));
        }
    }
}

// Step 4: tab 0 ("Perk Owner") runs on the owner with the target as its
// target; later tabs ("Target") on the target with the owner as its target.
// An empty tab passes. Both actors are the player here.
inline bool PerkSellConditionsPass(RE::BGSEntryPointPerkEntry* source, RE::Actor* owner, RE::Actor* target) {
    const auto tabs = static_cast<std::uint32_t>(source->conditions.size());
    for (std::uint32_t tab = 0; tab < tabs; ++tab) {
        const auto& condition = source->conditions[tab];
        if (!condition.head) continue;
        RE::Actor* subject = tab == 0 ? owner : target;
        RE::Actor* other = tab == 0 ? target : owner;
        if (!condition.IsTrue(subject, other)) return false;
    }
    return true;
}

// Steps 1-4 and 6 on the player. A refused table or a missing player leaves
// the facts unwalked; DecidePriceSnapshot then falls back with the reason.
inline void ReadPerkWalk(RE::PlayerCharacter* player, PerkWalkFacts& facts) {
    facts = PerkWalkFacts{};
    facts.tableOk = PerkPriceTableOk();
    // The probe passed RE::Actor* in both roles; the same static type here.
    RE::Actor* actor = player;
    if (!facts.tableOk || !actor) return;
    PerkSellCollector collector;
    actor->ForEachPerkEntry(RE::BGSEntryPoint::ENTRY_POINT::kModSellPrices, collector);
    facts.walked = true;
    facts.visits = collector.visits;
    facts.otherVisits = collector.otherVisits;
    facts.dropped = collector.dropped;
    facts.overflow = collector.overflow;
    facts.count = collector.count;
    for (std::uint32_t i = 0; i < collector.count; ++i) {
        ReadPerkSellEntry(collector.found[i], actor, facts.entries[i]);
        facts.entries[i].passed = PerkSellConditionsPass(collector.found[i], actor, actor);
    }
    float value = 1.0F;
    RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModSellPrices, actor, actor, &value);
    facts.engineCalled = true;
    facts.engineSelf = value;
}

}  // namespace js
