#pragma once
// w-probe-r45: engine-free rule for selling foreign-owned (stolen)
// items to a merchant that accepts stolen goods in vanilla barter.
// Design and evidence: the design notes.
//
// The engine function that combines the inputs below is not named in the
// pinned CommonLibSSE-NG headers, so this file does not claim to be that
// function. It is a fail-closed input gate in front of the engine's own
// authority: the product only reads rows the BarterMenu displays and only
// sells through the vanilla BarterMenu callback (G0/W path), so the engine
// still refuses whatever vanilla refuses. The two inputs are:
//  - vendorBuysStolen: the VENV "buys stolen items" byte of the merchant's
//    current vendor faction, TESFaction.h:84 (FACTION_VENDOR_DATA_VALUES::
//    buysStolen) read through Actor::GetVendorFaction (Actor.h:560). In
//    Skyrim.esm it is set on exactly the nine Thieves Guild fence factions.
//  - bypassValue: the player's current BypassVendorStolenCheck actor value,
//    ActorValues.h:101 (kBypassVendorStolenCheck = 93), read through
//    ActorValueOwner::GetActorValue (ActorValueOwner.h:15). The vanilla Speech
//    "Fence" perk (PERK 00058F79) is an ability (SPEL 000AF668) whose only
//    effect (MGEF 000AF667) is a Value Modifier of this actor value, 1.0.
// There is no perk entry point for stolen barter (BGSEntryPoint.h:13-109).
// Nothing here names an engine type; Oracle.h reads the facts.
#include <cmath>
#include <cstdint>

namespace js {

// One read per rule run, taken on the UI task before the list read. A fact
// that could not be read is "Known=false" and counts as "no" for that input
// only: an unreadable input never grants acceptance.
struct StolenAcceptanceFacts {
    bool merchantResolved{};
    bool vendorFactionKnown{};
    bool vendorBuysStolen{};
    bool bypassKnown{};
    float bypassValue{};
};

enum class StolenAcceptance : unsigned {
    kRefused,
    kVendorBuysStolen,
    kBypassPerk,
    kVendorAndPerk,
};

inline bool VendorInputAccepts(const StolenAcceptanceFacts& facts) noexcept {
    return facts.merchantResolved && facts.vendorFactionKnown && facts.vendorBuysStolen;
}

inline bool BypassInputAccepts(const StolenAcceptanceFacts& facts) noexcept {
    return facts.bypassKnown && std::isfinite(facts.bypassValue) && facts.bypassValue > 0.0F;
}

inline StolenAcceptance DecideStolenAcceptance(const StolenAcceptanceFacts& facts) noexcept {
    const bool vendor = VendorInputAccepts(facts);
    const bool bypass = BypassInputAccepts(facts);
    if (vendor && bypass) return StolenAcceptance::kVendorAndPerk;
    if (vendor) return StolenAcceptance::kVendorBuysStolen;
    if (bypass) return StolenAcceptance::kBypassPerk;
    return StolenAcceptance::kRefused;
}

inline bool StolenAccepted(StolenAcceptance value) noexcept {
    return value == StolenAcceptance::kVendorBuysStolen || value == StolenAcceptance::kBypassPerk ||
           value == StolenAcceptance::kVendorAndPerk;
}

inline const char* StolenAcceptanceName(StolenAcceptance value) noexcept {
    switch (value) {
    case StolenAcceptance::kRefused: return "refused";
    case StolenAcceptance::kVendorBuysStolen: return "vendor_buys_stolen";
    case StolenAcceptance::kBypassPerk: return "bypass_perk";
    case StolenAcceptance::kVendorAndPerk: return "vendor_and_perk";
    }
    return "refused";
}

// The v0.0.1 ownership class (r42 Probe.cpp ReadRuleCandidateEntry): an owner
// that is set and is not the player. A missing player form (0) never matches,
// so every set owner is foreign then -- the same answer r42 gave.
inline bool ForeignOwner(std::uint32_t ownerForm, std::uint32_t playerForm) noexcept {
    return ownerForm != 0 && !(playerForm != 0 && ownerForm == playerForm);
}

// The selector's ownerUnknown input from r45 on: a foreign owner is refused
// only when this run's merchant does not accept stolen goods. With kRefused
// this is exactly the r42 expression, so the refused branch (and its
// owner_unknown receipt term) is byte-for-byte v0.0.1.
inline bool ForeignOwnerRefused(std::uint32_t ownerForm, std::uint32_t playerForm,
                                StolenAcceptance acceptance) noexcept {
    return ForeignOwner(ownerForm, playerForm) && !StolenAccepted(acceptance);
}

}  // namespace js
