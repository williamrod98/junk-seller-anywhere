#pragma once
// Engine free merchant identity rules (r5 / probe-js05 anomaly 3).
//
// Why this file exists: BarterMenu::GetTargetRefHandle() is a relocated global
// (J02), and in probe-js05 it produced a handle that resolved to the player
// (refr 0x14, base 0x7) for all twelve samples. The probe still wrote
// merchantResolved=true, so the receipt said "resolved" while it held the wrong
// reference, and the VENC container lookup that hangs off it returned null
// because the player has no vendor faction. Two things follow: a candidate that
// resolves to the player is not a merchant, and the raw handle has to be in the
// receipt so "empty handle" and "wrong reference" can be told apart.
//
// Nothing here dereferences anything or names an engine type. The caller looks
// each candidate up, records what came back, and the rules below decide.
#include <cstdint>
namespace js {
// Skyrim.esm: the player reference and the player base form.
constexpr std::uint32_t PlayerRefFormID = 0x00000014u;
constexpr std::uint32_t PlayerBaseFormID = 0x00000007u;

// Where a merchant reference came from. Recorded on every sample so a reader
// never has to guess which path produced the balance that follows.
enum class MerchantSource : unsigned { kNone = 0, kTargetHandle = 1, kSpeaker = 2, kLastSpeaker = 3 };

// One candidate reference exactly as the lookup found it.
struct MerchantCandidate {
    std::uint32_t handle{}, formID{}, baseID{};
    bool resolved{}, isActor{}, isPlayer{};
};

// A candidate is the player when either half says so. The base form is checked
// as well as the reference, because a handle that lands on handle slot zero
// yields the PlayerCharacter singleton whatever its reference FormID reads as.
inline void ClassifyMerchant(MerchantCandidate& candidate) noexcept {
    candidate.isPlayer = candidate.resolved &&
                         (candidate.formID == PlayerRefFormID || candidate.baseID == PlayerBaseFormID);
}
// Usable means: a non empty handle, something came back, it is an Actor, and it
// is not the player. An empty handle is refused before the lookup, because the
// handle manager answers slot zero with the player.
inline bool MerchantUsable(const MerchantCandidate& candidate) noexcept {
    if (!candidate.handle || !candidate.resolved) return false;
    if (!candidate.isActor || candidate.isPlayer) return false;
    return candidate.formID != 0;
}

// The adopted candidate. The BarterMenu target handle is always tried first and
// is the only path the probe uses on its own; the dialogue speaker candidates
// are recorded next to it on every sample and are adopted only when the INI
// says so (MerchantFallback), because which path is correct is a reading of the
// next run, not a guess this code is allowed to make.
inline MerchantSource ChooseMerchant(const MerchantCandidate& target, const MerchantCandidate& speaker,
                                     const MerchantCandidate& lastSpeaker, bool allowFallback) noexcept {
    if (MerchantUsable(target)) return MerchantSource::kTargetHandle;
    if (!allowFallback) return MerchantSource::kNone;
    if (MerchantUsable(speaker)) return MerchantSource::kSpeaker;
    if (MerchantUsable(lastSpeaker)) return MerchantSource::kLastSpeaker;
    return MerchantSource::kNone;
}
inline const char* MerchantSourceName(MerchantSource source) noexcept {
    switch (source) {
    case MerchantSource::kTargetHandle: return "target_handle";
    case MerchantSource::kSpeaker: return "speaker";
    case MerchantSource::kLastSpeaker: return "last_speaker";
    default: return "none";
    }
}
// The reason code a sample carries when no candidate was usable. r4 wrote
// merchantResolved=true for a player reference, which is the one thing the
// receipt must never do: a failure has to read as a failure.
constexpr const char* MerchantUnresolvedReason = "MERCHANT_UNRESOLVED";
}
