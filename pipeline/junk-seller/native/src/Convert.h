#pragma once
// v0.1.0 (the v0.1.0 design, design r3): the
// engine-free half of "sell anywhere" by direct conversion. With SellAnywhere
// ON and no menu open, the hotkey converts every plain unit of every eligible
// row into gold and Speech XP in one batch: no merchant, no BarterMenu.
//
//   * ComputeConvertPrice / ConvertUnitPrice -- the closed price model of 2.1:
//       S = clamp(current Speech, 0, 100), f = fBarterMax - (fBarterMax -
//       fBarterMin) * S / 100, m = H(Haggling) * (1 + AV107/100) *
//       (1 + AV146/100) / f in float32, m' = min(m, 1.0), p =
//       round-to-nearest(V * m'). Exact on the 20 perk-controlled cards of the
//       2026-09-24 probe.
//   * ConvertCandidateType -- the category of one inventory entry (junk,
//       ingredient, food, misc, spare weapon/armor), fed to the unchanged
//       Selector.h AND rule and its absolute protections.
//   * AddConvertExtra / ConvertListVerdict (w-probe-r60, addendum IU) -- one
//       ExtraDataList on its own: plain (count only), a convertible instance
//       list (every extra inert: the player's own ownership, a unique id, the
//       leveled-item mark, cannot-wear; since w-probe-r61 also the scale and
//       a reference handle that names no reference) or a kept list (worn,
//       favourite, enchanted, quest alias, foreign owner, value-changing,
//       renamed, linked to a live reference or any other extra; fail closed).
//   * BuildConvertPlan -- whole stacks whose every unit is plain or sits in
//       a convertible instance list (no kept list), at most MaxItems rows,
//       p >= 1 (the zero_gold term).
//   * RunConvertSlice -- ONE call is ONE main-thread task of at most
//       ConvertSliceBudgetUs (8 ms) of rows: remove every row, verify the
//       counts with one read, one gold credit for the slice and its verify,
//       then the Speech XP of each sold row (v0.4.0). A failed gold step rolls the
//       slice back. It never queues anything; the caller's pump runs the next
//       slice on a later frame (4.1).
//   * EvaluateConvertGate / ClassifyConvertOutcome -- the world gate of 5.3
//       and exactly one result line per press (6).
//   * MerchantCheckFromBatch -- the MCM "last merchant check" line of 2.5.
//   * v0.2.0 (the v0.2 update plan r2, section 6 row
//     v0.2.0): ConvertMaterialTier / ConvertGearType (weak and guard
//     weapons and armor, clothing), DecideConvertCandidate (the junk rule
//     choice Classic / Value only, "sell all" for the gear categories), the
//     "Include stolen items" list kind (a list whose only kept reason is a
//     foreign owner, removed by its own pointer like an inert list), the
//     built-in never-sell facts (tools, Lockpick, home-building materials,
//     uniques), the five "why kept" counters and the line parts "kinds
//     cleared" / "stolen". Whole rows only, as in v0.1.0.
//   * v0.3.0 (the v0.2 update plan r2 section 6 row v0.3.0):
//     ConvertEnchantedLoot, the weak enchanted loot category (a templated
//     WEAP / ARMO whose enchantment comes from its record: the weak material
//     tier or guard gear for weapons and armor; enchantment level 1-3 of the
//     vanilla 01-06 ladder, or a base value at most the slider for other
//     enchantments, for jewelry and clothing), and "Known enchants only"
//     (an item whose enchantment the player has not learned stays; the
//     fourth "why kept" counter). Whole rows only.
//   * v0.4.0 (the v0.2 update plan r2 section 3 and section 6 row
//     v0.4.0): the keep-N engine. Eight keep-limit categories (alchemy
//     ingredients, cooked food, raw food, drinks and quest food, ore and
//     ingots, gems, pelts, tools; ConvertKeepFoodType / ConvertKeepMiscType
//     / ConvertGearType) keep N units per kind and sell only the surplus;
//     the sell-all categories of v0.2.0 (junk, weak weapons, weak armor,
//     clothing) keep 0 the same way, so a row with a kept list now sells
//     its convertible units. sell = min(total - max(N, floor),
//     convertible); stolen lists first, then inert lists (whole lists,
//     smallest first, never past the surplus), then plain units, and a plain
//     removal only when every list left in the entry is inert or stolen
//     (ConvertKeepTake). The quest-food floor applies even at N = 0. Weak
//     enchanted loot keeps the v0.3.0 whole-row rule. Speech XP: one
//     UseSkill call per row (units x value points, design ruling).
//   * v0.5.0 (the v0.2 update plan r2 section 2.2 rows "Potions" and
//     "Poisons", section 6 row v0.5.0, design ruling Q4): potions and poisons,
//     player-made ones included, classified by their costliest effect
//     (ConvertPotionType). Restore Health / Magicka / Stamina and Cure
//     Disease are one pool each, every fortified skill actor value one pool;
//     a pool keeps its number of units across all its kinds, the strongest
//     kinds first (higher value, ties by the lower form id), and each kind
//     then sells its surplus through the keep-N engine
//     (ConvertPotionPoolKeeps, one pass at plan time, no Papyrus). Other
//     potions and poisons keep their number per kind.
//   * v0.5.1 (fix build, least load): a row that sells its whole stack
//     and holds two or more removal steps (lists, plus its plain units) is
//     removed with ONE engine call for the stack (ConvertHost::RemoveStack)
//     instead of one call per list; a one-key read proves the stack is gone,
//     and whatever the call left is removed by the v0.5.0 per-list path. A
//     partial (keep-N) row keeps the v0.5.0 per-list removal (which units
//     stay is its rule). Every sale rule, plan and receipt value is
//     unchanged; the receipt counts the engine removal calls.
//   * v0.6.0 (the v0.2 update plan r2 section 2.2 rows "Weapons, armor",
//     "Weak enchanted loot" and "Clothing", section 6 row v0.6.0): mid-tier
//     gear and high-tier gear (the material tier; no known material keyword
//     counts as mid), each its own switch and keep per kind (default 10),
//     and jewelry (keep per kind, default 10), through the v0.4.0 keep-N
//     engine. Mid / high enchanted loot (ConvertEnchantedLootTier: the
//     templated loot variants above the weak tier, and enchanted jewelry and
//     clothing of ladder level 4-6 or above the value bound) follows its
//     tier's switch and keep, and only while "Weak enchanted loot" is ON too;
//     like weak loot, a loot row with any kept list stays whole and "Known
//     enchants only" keeps unlearned loot.
//
// Nothing here includes an engine header or touches an RE object. The engine
// side (ConvertEngine.h, the only native file allowed to call RemoveItem,
// AddObjectToContainer and UseSkill) implements ConvertHost.
#include "Batch.h"
#include "Selector.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace js {

// ---------------------------------------------------------------------------
// Price (design r3 section 2.1). The inputs are the ones the v0.0.4 shadow
// quote already reads (ShadowPriceInputs, Batch.h), so both paths agree on
// what "the formula" is; the anywhere path adds the m' cap and float32
// rounding measured on the arm cards.

struct ConvertPrice {
    bool known{};
    float speech{};
    float factor{};
    float multiplier{};
    // min(multiplier, 1.0): what one unit of value V pays, before rounding.
    float capped{};
    bool clamped{};
};

inline float ConvertHagglingMultiplier(int rank) noexcept {
    switch (rank) {
    case 1: return 1.10F;
    case 2: return 1.15F;
    case 3: return 1.20F;
    case 4: return 1.25F;
    case 5: return 1.30F;
    default: return 1.0F;
    }
}

inline ConvertPrice ComputeConvertPrice(const ShadowPriceInputs& in) noexcept {
    ConvertPrice price;
    if (!std::isfinite(in.barterMax) || !std::isfinite(in.barterMin) || !std::isfinite(in.speech) ||
        !std::isfinite(in.av107Percent) || !std::isfinite(in.av146Percent) || in.hagglingRank < 0 ||
        in.hagglingRank > 5) {
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
    const float gear = 1.0F + in.av107Percent / 100.0F;
    const float potion = 1.0F + in.av146Percent / 100.0F;
    const float m = ConvertHagglingMultiplier(in.hagglingRank) * gear * potion / factor;
    if (!std::isfinite(m) || !(m > 0.0F)) return price;
    price.known = true;
    price.speech = speech;
    price.factor = factor;
    price.multiplier = m;
    price.clamped = m > 1.0F;
    price.capped = price.clamped ? 1.0F : m;
    return price;
}

// Round to nearest of V times the float32 multiplier, the product taken at
// double precision (arm B0: 300 x 0.575F = 172.4999964 -> 172, measured 172;
// a float32 product would round to 172.5 -> 173, and ceil / floor are
// excluded by the same cards). A value <= 0 or an unknown price pays 0.
inline std::int32_t ConvertUnitPrice(std::int32_t value, const ConvertPrice& price) noexcept {
    if (value <= 0 || !price.known) return 0;
    const double product = static_cast<double>(value) * static_cast<double>(price.capped);
    if (!std::isfinite(product) || product <= 0.0) return 0;
    if (product >= 2147483000.0) return 2147483000;
    return static_cast<std::int32_t>(std::llround(product));
}

// The MCM "Rate now" line: round(100 x m'), or -1 when the inputs are unknown.
inline std::int32_t ConvertRatePercent(const ConvertPrice& price) noexcept {
    if (!price.known) return -1;
    return static_cast<std::int32_t>(std::lround(price.capped * 100.0F));
}

// ---------------------------------------------------------------------------
// Categories (design r3 section 5.1 plus the player-facing switches of
// a design request). The engine side names the form kind; the category
// switches themselves are Selector.h CategoryOff terms, OFF by default.

enum class ConvertFormKind : unsigned { kOther, kMisc, kIngredient, kAlchemy, kWeapon, kArmor };

// `miscJunk`: a Misc base with none of the non-junk vendor keywords
// (Protection.h MiscNonJunkKeywords). `food`: an Alchemy item with the food
// flag. A potion or poison, an unknown kind and everything else stays
// kPotion / kUnknown, which the selector excludes.
inline CandidateType ConvertCandidateType(ConvertFormKind kind, bool miscJunk, bool food) noexcept {
    switch (kind) {
    case ConvertFormKind::kIngredient: return CandidateType::kIngredient;
    case ConvertFormKind::kMisc: return miscJunk ? CandidateType::kJunk : CandidateType::kMisc;
    case ConvertFormKind::kAlchemy: return food ? CandidateType::kFood : CandidateType::kPotion;
    case ConvertFormKind::kWeapon:
    case ConvertFormKind::kArmor: return CandidateType::kGear;
    case ConvertFormKind::kOther: return CandidateType::kUnknown;
    }
    return CandidateType::kUnknown;
}

// w-probe-r60: the receipt category of a planned row (the receipt's
// soldByCategory buckets). Junk is the always-on category; the anywhere
// reader produces no other planned type ("other" never occurs).
inline const char* ConvertCategoryName(CandidateType type) noexcept {
    switch (type) {
    case CandidateType::kJunk: return "junk";
    case CandidateType::kIngredient: return "ingredients";
    case CandidateType::kFood: return "food";
    case CandidateType::kMisc: return "misc";
    case CandidateType::kGear: return "gear";
    case CandidateType::kWeakWeapon: return "weakWeapons";
    case CandidateType::kWeakArmor: return "weakArmor";
    case CandidateType::kClothing: return "clothing";
    case CandidateType::kEnchantedLoot: return "enchantedLoot";
    case CandidateType::kKeepIngredient: return "alchemyIngredients";
    case CandidateType::kCookedFood: return "cookedFood";
    case CandidateType::kRawFood: return "rawFood";
    case CandidateType::kDrink: return "drinks";
    case CandidateType::kOre: return "ore";
    case CandidateType::kGem: return "gems";
    case CandidateType::kPelt: return "pelts";
    case CandidateType::kTool: return "tools";
    case CandidateType::kRestorePotion: return "restorePotions";
    case CandidateType::kSkillPotion: return "skillPotions";
    case CandidateType::kOtherPotion: return "otherPotions";
    case CandidateType::kPoison: return "poisons";
    case CandidateType::kMidGear: return "midGear";
    case CandidateType::kHighGear: return "highGear";
    case CandidateType::kJewelry: return "jewelry";
    default: return "other";
    }
}

// ---------------------------------------------------------------------------
// v0.2.0 gear categories (the v0.2 update plan r2 section 2.2). The tier of
// a weapon or armor record is the strongest tier among its material keywords
// on the vanilla smithing-perk ladder (the table below, the model's tables of
// the offline model); no known material keyword =
// unknown, which counts as mid (never sold in v0.2.0; v0.6.0: mid-tier
// gear, under its own switch). The engine resolves
// every EditorID below to its BGSKeyword pointer once at kDataLoaded
// (ConvertEngine.h ConvertCatalog), so a press compares pointers only.

enum class ConvertTier : unsigned { kUnknown, kWeak, kMid, kHigh };

struct ConvertMaterialKeyword {
    const char* editorId;
    ConvertTier tier;
};

inline constexpr std::array<ConvertMaterialKeyword, 19> ConvertWeaponMaterials{{
    {"WeapMaterialIron", ConvertTier::kWeak},
    {"WeapMaterialSteel", ConvertTier::kWeak},
    {"WeapMaterialImperial", ConvertTier::kWeak},
    {"WeapMaterialDraugr", ConvertTier::kWeak},
    {"WeapMaterialWood", ConvertTier::kWeak},
    {"WeapMaterialFalmer", ConvertTier::kWeak},
    {"DLC2WeaponMaterialNordic", ConvertTier::kMid},
    {"WeapMaterialSilver", ConvertTier::kMid},
    {"WeapMaterialOrcish", ConvertTier::kMid},
    {"WeapMaterialDwarven", ConvertTier::kMid},
    {"WeapMaterialElven", ConvertTier::kMid},
    {"WeapMaterialDraugrHoned", ConvertTier::kMid},
    {"WeapMaterialFalmerHoned", ConvertTier::kMid},
    {"DLC1WeapMaterialDragonbone", ConvertTier::kHigh},
    {"WeapMaterialGlass", ConvertTier::kHigh},
    {"WeapMaterialEbony", ConvertTier::kHigh},
    {"WeapMaterialDaedric", ConvertTier::kHigh},
    {"DLC2WeaponMaterialStalhrim", ConvertTier::kHigh},
    {"WeapMaterialDragonbone", ConvertTier::kHigh},
}};

// v0.7.0: the Stormcloak officer keyword as the masters name it
// (ArmorMaterialBearStormcloak, Update.esm 0009BE; the table had
// ArmorMaterialStormcloakBear, which no master defines, so that gear counted
// as no material = mid), and the Blades (ArmorMaterialBlades, Update.esm
// 0009C0) and Penitus Oculatus (ArmorMaterialPenitus, Update.esm 0009BB)
// sets by their temper material (steel ingot / leather): weak, like the
// Stormcloak, Imperial and steel rows.
inline constexpr std::array<ConvertMaterialKeyword, 46> ConvertArmorMaterials{{
    {"ArmorMaterialHide", ConvertTier::kWeak},
    {"ArmorMaterialLeather", ConvertTier::kWeak},
    {"ArmorMaterialIron", ConvertTier::kWeak},
    {"ArmorMaterialIronBanded", ConvertTier::kWeak},
    {"ArmorMaterialStudded", ConvertTier::kWeak},
    {"ArmorMaterialImperialLight", ConvertTier::kWeak},
    {"ArmorMaterialImperialStudded", ConvertTier::kWeak},
    {"ArmorMaterialImperialHeavy", ConvertTier::kWeak},
    {"ArmorMaterialStormcloak", ConvertTier::kWeak},
    {"ArmorMaterialBearStormcloak", ConvertTier::kWeak},
    {"ArmorMaterialBlades", ConvertTier::kWeak},
    {"ArmorMaterialPenitus", ConvertTier::kWeak},
    {"ArmorMaterialForsworn", ConvertTier::kWeak},
    {"ArmorMaterialFalmer", ConvertTier::kWeak},
    {"ArmorMaterialDraugr", ConvertTier::kWeak},
    {"ArmorMaterialSteel", ConvertTier::kWeak},
    {"ArmorMaterialMS02Forsworn", ConvertTier::kWeak},
    {"ArmorMaterialThievesGuild", ConvertTier::kMid},
    {"ArmorMaterialThievesGuildLeader", ConvertTier::kMid},
    {"ArmorMaterialScaled", ConvertTier::kMid},
    {"ArmorMaterialSteelPlate", ConvertTier::kMid},
    {"ArmorMaterialDwarven", ConvertTier::kMid},
    {"ArmorMaterialElven", ConvertTier::kMid},
    {"ArmorMaterialElvenGilded", ConvertTier::kMid},
    {"ArmorMaterialOrcish", ConvertTier::kMid},
    {"DLC2ArmorMaterialBonemoldLight", ConvertTier::kMid},
    {"DLC2ArmorMaterialBonemoldHeavy", ConvertTier::kMid},
    {"DLC2ArmorMaterialChitinLight", ConvertTier::kMid},
    {"DLC2ArmorMaterialChitinHeavy", ConvertTier::kMid},
    {"DLC2ArmorMaterialMoragTong", ConvertTier::kMid},
    {"DLC1ArmorMaterialDawnguard", ConvertTier::kMid},
    {"DLC1ArmorMaterialFalmerHardened", ConvertTier::kMid},
    {"DLC1ArmorMaterialFalmerHeavy", ConvertTier::kMid},
    {"DLC1ArmorMaterialFalmerHeavyOriginal", ConvertTier::kMid},
    {"DLC1ArmorMaterialVampire", ConvertTier::kMid},
    {"DLC1ArmorMaterialHunter", ConvertTier::kMid},
    {"DLC2ArmorMaterialNordicLight", ConvertTier::kMid},
    {"DLC2ArmorMaterialNordicHeavy", ConvertTier::kMid},
    {"ArmorNightingale", ConvertTier::kHigh},
    {"ArmorMaterialGlass", ConvertTier::kHigh},
    {"ArmorMaterialEbony", ConvertTier::kHigh},
    {"ArmorMaterialDaedric", ConvertTier::kHigh},
    {"ArmorMaterialDragonscale", ConvertTier::kHigh},
    {"ArmorMaterialDragonplate", ConvertTier::kHigh},
    {"DLC2ArmorMaterialStalhrimLight", ConvertTier::kHigh},
    {"DLC2ArmorMaterialStalhrimHeavy", ConvertTier::kHigh},
}};

// The tier one material keyword names for a weapon (true) or armor record;
// kUnknown for any other keyword. The comparison is exact (EditorIDs are
// case-sensitive in the data; the engine side matches the same strings).
inline ConvertTier ConvertMaterialTier(std::string_view editorId, bool weapon) noexcept {
    if (weapon) {
        for (const auto& entry : ConvertWeaponMaterials) {
            if (editorId == entry.editorId) return entry.tier;
        }
        return ConvertTier::kUnknown;
    }
    for (const auto& entry : ConvertArmorMaterials) {
        if (editorId == entry.editorId) return entry.tier;
    }
    return ConvertTier::kUnknown;
}

// The strongest of two tiers: high > mid > weak > unknown (a record with a
// weak and a mid keyword is mid; the model's gear_tier).
inline ConvertTier StrongerConvertTier(ConvertTier a, ConvertTier b) noexcept {
    const auto rank = [](ConvertTier t) {
        switch (t) {
        case ConvertTier::kHigh: return 3;
        case ConvertTier::kMid: return 2;
        case ConvertTier::kWeak: return 1;
        case ConvertTier::kUnknown: return 0;
        }
        return 0;
    };
    return rank(a) >= rank(b) ? a : b;
}

// The facts of one weapon or armor record the category needs, each read or
// resolved once (the tier from the keyword pointers, the lists at kDataLoaded).
struct ConvertGearFacts {
    bool weapon{};     // WEAP; false = ARMO
    bool clothing{};   // ARMO whose armor type is clothing (BGSBipedObjectForm::IsClothing)
    bool jewelry{};    // ARMO with ArmorJewelry or VendorItemJewelry
    bool tool{};       // WEAP in MineOreToolsList / woodChoppingAxes or with VendorItemTool
    bool staff{};      // WEAP of the staff type or with WeapTypeStaff
    bool guardForm{};  // on the guard gear list (ConvertEngine.h ConvertCatalog)
    ConvertTier tier{ConvertTier::kUnknown};
    // v0.3.0 (the v0.2 update plan r2 section 2.2, weak enchanted loot):
    // the record's own enchantment (TESEnchantableForm EITM), its template
    // (WEAP CNAM / ARMO TNAM: the leveled loot variants such as "Iron Sword
    // of Frost"), a Daedric artifact keyword, the enchantment's level on the
    // vanilla 01-06 ladder (1..6; 0 = not on the ladder, e.g. a mod
    // enchantment; ConvertLists.h ConvertEnchantLevels), whether the player
    // knows the enchantment or its base enchantment (the enchanting table's
    // known flag) and the record's base value (TESValueForm).
    bool baseEnchanted{};
    bool templated{};
    bool artifact{};
    std::int32_t enchantLevel{};
    bool enchantKnown{};
    std::int32_t baseValue{};
};

// The default "Jewelry max value" of weak enchanted loot (the v0.2 update plan
// r2 section 4.2: slider 0-2000, step 50, default 300).
inline constexpr int ConvertEnchantedLootMaxValueDefault = 300;
inline constexpr int ConvertEnchantedLootMaxValueCeiling = 2000;
// The weak rungs of the vanilla enchantment ladder (01-06): 1..3.
inline constexpr std::int32_t ConvertEnchantWeakLevelMax = 3;

// v0.3.0: the loot rule (design ruling revision 2026-09-29).
// Never loot: no base enchantment, no template (uniques, artifacts, Dragon
// Priest masks, faction sets and set pieces carry their own record), a
// Daedric artifact keyword, tools and staves. The unique list, the absolute
// protections, player enchantments (a kept ExtraEnchantment list) and the
// known-enchantment switch are applied by the plan, not here.
// v0.6.0: the tier of a loot record (kUnknown = not loot), so the mid /
// high loot follows the tier switches. Weapons and armor: the weak material
// tier or guard gear (guard origin included, audit A-02 of v0.2.0) = weak,
// the high tier = high, anything else (mid, or no known material) = mid.
// Jewelry and clothing (no material): an enchantment on the ladder at level
// 1-3, or one off the ladder on an item whose base value is at most
// `maxValue`, = weak; a ladder level 4-6 or a larger value = mid (the r2
// model's ench_loot_rule); a negative value off the ladder is not loot.
inline ConvertTier ConvertEnchantedLootTier(const ConvertGearFacts& facts, bool guardOrigin, int maxValue) noexcept {
    if (!facts.baseEnchanted || !facts.templated || facts.artifact) return ConvertTier::kUnknown;
    if (facts.weapon && (facts.tool || facts.staff)) return ConvertTier::kUnknown;
    if (!facts.weapon && (facts.clothing || facts.jewelry)) {
        if (facts.enchantLevel >= 1) {
            return facts.enchantLevel <= ConvertEnchantWeakLevelMax ? ConvertTier::kWeak : ConvertTier::kMid;
        }
        if (facts.baseValue < 0) return ConvertTier::kUnknown;
        return facts.baseValue <= maxValue ? ConvertTier::kWeak : ConvertTier::kMid;
    }
    if (facts.guardForm || guardOrigin || facts.tier == ConvertTier::kWeak) return ConvertTier::kWeak;
    return facts.tier == ConvertTier::kHigh ? ConvertTier::kHigh : ConvertTier::kMid;
}

// v0.3.0: true when a base-enchanted weapon or armor record is weak
// enchanted loot (sold whole under "Weak enchanted loot").
inline bool ConvertEnchantedLoot(const ConvertGearFacts& facts, bool guardOrigin, int maxValue) noexcept {
    return ConvertEnchantedLootTier(facts, guardOrigin, maxValue) == ConvertTier::kWeak;
}

// v0.6.0: true when a base-enchanted record is mid or high enchanted
// loot (sold under its tier's switch and keep; the plan also needs "Weak
// enchanted loot" ON, keeps a row with any kept list whole and applies
// "Known enchants only").
inline bool ConvertMidHighLoot(const ConvertGearFacts& facts, bool guardOrigin, int maxValue) noexcept {
    const auto tier = ConvertEnchantedLootTier(facts, guardOrigin, maxValue);
    return tier == ConvertTier::kMid || tier == ConvertTier::kHigh;
}

// The category of a weapon or armor record (the model's category()): staves
// never (kGear), tools kTool (v0.4.0: their own keep-limit category,
// keep 1; kGear, never, in v0.2.0 / v0.3.0), clothing = kClothing, guard gear
// and the weak material tier = weak. `guardOrigin` is true when
// every unit of the row sits in a stolen list whose owner is a guard: a
// pickpocketed Steel Plate or Scaled piece then counts as weak regardless of
// its material.
// v0.3.0: a base-enchanted record that is weak enchanted loot is
// kEnchantedLoot (its own switch).
// v0.6.0: jewelry of any armor type is kJewelry (kGear, never, in
// v0.2.0 .. v0.5.0; a mod ring of the light or heavy type with a weak
// material or on a guard list is jewelry too, audit A-02 of the v0.2.0 review);
// the high material tier is kHighGear and everything else (the mid tier, no
// known material) kMidGear; mid / high enchanted loot takes its tier's type
// (ConvertMidHighLoot). Any other base-enchanted record keeps its type and
// the plan's enchanted protection keeps it.
inline CandidateType ConvertGearType(const ConvertGearFacts& facts, bool guardOrigin,
                                     int enchantMaxValue = ConvertEnchantedLootMaxValueDefault) noexcept {
    if (facts.baseEnchanted) {
        const auto loot = ConvertEnchantedLootTier(facts, guardOrigin, enchantMaxValue);
        if (loot == ConvertTier::kWeak) return CandidateType::kEnchantedLoot;
        if (loot == ConvertTier::kMid) return CandidateType::kMidGear;
        if (loot == ConvertTier::kHigh) return CandidateType::kHighGear;
    }
    if (facts.weapon) {
        if (facts.staff) return CandidateType::kGear;
        if (facts.tool) return CandidateType::kTool;
    } else {
        if (facts.jewelry) return CandidateType::kJewelry;
        if (facts.clothing) return CandidateType::kClothing;
    }
    const bool weak = facts.guardForm || guardOrigin || facts.tier == ConvertTier::kWeak;
    if (weak) return facts.weapon ? CandidateType::kWeakWeapon : CandidateType::kWeakArmor;
    return facts.tier == ConvertTier::kHigh ? CandidateType::kHighGear : CandidateType::kMidGear;
}

// True for the whole-row gear categories of v0.2.0 and v0.3.0 ("sell all").
inline bool ConvertSellAllType(CandidateType type) noexcept {
    return type == CandidateType::kWeakWeapon || type == CandidateType::kWeakArmor ||
           type == CandidateType::kClothing || type == CandidateType::kEnchantedLoot;
}

// ---------------------------------------------------------------------------
// v0.4.0 keep-limit categories (the v0.2 update plan r2 sections 2.2 and 3).

// The facts of one food record (an Alchemy item with the food flag), each
// resolved once at kDataLoaded (ConvertEngine.h ConvertCatalog): a drink (the
// ConvertLists.h ConvertDrinkForms table, or a mod keyword naming a drink or
// alcohol), the created object of a recipe at a cooking pot or oven, raw
// food (VendorItemFoodRaw, or an input of such a recipe), and the
// quest-food floor (ConvertLists.h ConvertQuestFoods; 0 = no quest need).
struct ConvertFoodFacts {
    bool drink{};
    bool cooked{};
    bool raw{};
    std::int32_t questFloor{};
};

// The food category (the v0.2 update plan r2 section 2.2): a drink OR a food
// on the quest-food table is "drinks and quest food" (its own switch and
// keep, never below the floor), whatever its recipes; then cooked, then raw
// (the r2 model's order); any other food (neither made nor used in cooking:
// cheese wedges, bought bread) counts as cooked. Audit A-01 of
// the v0.4.0 review: a raw or cooked quest food (an apple, a loaf of bread)
// follows the quest-food category, so switching it OFF keeps it.
inline CandidateType ConvertKeepFoodType(const ConvertFoodFacts& facts) noexcept {
    if (facts.drink || facts.questFloor > 0) return CandidateType::kDrink;
    if (facts.cooked) return CandidateType::kCookedFood;
    if (facts.raw) return CandidateType::kRawFood;
    return CandidateType::kCookedFood;
}

// The Misc category from the index of its first non-junk vendor keyword
// (Protection.h MiscNonJunkKeywords: 0 VendorItemGem, 1 VendorItemOreIngot,
// 2 VendorItemAnimalHide; ConvertEngine.h pins the order): gems, ore and
// ingots, pelts; no keyword = junk; tools, animal parts and Daedric
// artifacts stay kMisc (never sold).
inline CandidateType ConvertKeepMiscType(int keywordIndex) noexcept {
    switch (keywordIndex) {
    case -1: return CandidateType::kJunk;
    case 0: return CandidateType::kGem;
    case 1: return CandidateType::kOre;
    case 2: return CandidateType::kPelt;
    default: return CandidateType::kMisc;
    }
}

// ---------------------------------------------------------------------------
// v0.5.0 potions and poisons (the v0.2 update plan r2 section 2.2, Q4).

// The facts of one non-food Alchemy item, read per row at plan time
// (ConvertEngine.h ReadConvertPotionFacts): the poison flag and the
// archetype, primary actor value and Recover flag of its costliest effect
// (MagicItem::GetCostliestEffectItem, the effect the game names a potion
// after). A player-made potion is an ordinary created AlchemyItem with its
// own effect list, so it is read the same way at runtime.
struct ConvertPotionFacts {
    bool poison{};
    bool effect{};                // a costliest effect with a base effect was found
    std::int32_t archetype{-1};   // EffectSetting::Archetype
    std::int32_t actorValue{-1};  // EffectSetting primaryAV
    bool recover{};               // EffectSettingData::Flag::kRecover
};

// The archetypes and actor values the classes name (CommonLib
// EffectArchetypes.h and ActorValues.h; pinned by a static_assert on the
// engine side).
inline constexpr std::int32_t ConvertArchetypeValueModifier = 0;
inline constexpr std::int32_t ConvertArchetypeCureDisease = 3;
inline constexpr std::int32_t ConvertArchetypeDualValueModifier = 5;
inline constexpr std::int32_t ConvertArchetypePeakValueModifier = 34;
inline constexpr std::int32_t ConvertAvHealth = 24;
inline constexpr std::int32_t ConvertAvStamina = 26;
inline constexpr std::int32_t ConvertAvSkillFirst = 6;               // OneHanded
inline constexpr std::int32_t ConvertAvSkillLast = 23;               // Enchanting
inline constexpr std::int32_t ConvertAvSkillModifierFirst = 96;      // OneHandedModifier
inline constexpr std::int32_t ConvertAvSkillModifierLast = 113;      // EnchantingModifier
inline constexpr std::int32_t ConvertAvSkillPowerModifierFirst = 135;  // OneHandedPowerModifier
inline constexpr std::int32_t ConvertAvSkillPowerModifierLast = 152;   // EnchantingPowerModifier

// Restore: a Value, Peak or Dual value modifier of Health, Magicka or
// Stamina without the Recover flag (a Fortify has it), or the Cure Disease
// archetype.
inline bool ConvertRestoreEffect(const ConvertPotionFacts& facts) noexcept {
    if (!facts.effect) return false;
    if (facts.archetype == ConvertArchetypeCureDisease) return true;
    const bool modifier = facts.archetype == ConvertArchetypeValueModifier ||
                          facts.archetype == ConvertArchetypePeakValueModifier ||
                          facts.archetype == ConvertArchetypeDualValueModifier;
    return modifier && facts.actorValue >= ConvertAvHealth && facts.actorValue <= ConvertAvStamina && !facts.recover;
}

// Skill: a Value or Peak value modifier that fortifies a skill (a skill
// actor value with the Recover flag, or a skill modifier / power modifier
// actor value), the r2 model's potion_class rule.
inline bool ConvertSkillEffect(const ConvertPotionFacts& facts) noexcept {
    if (!facts.effect) return false;
    if (facts.archetype != ConvertArchetypeValueModifier && facts.archetype != ConvertArchetypePeakValueModifier) {
        return false;
    }
    const std::int32_t av = facts.actorValue;
    return (av >= ConvertAvSkillModifierFirst && av <= ConvertAvSkillModifierLast) ||
           (av >= ConvertAvSkillPowerModifierFirst && av <= ConvertAvSkillPowerModifierLast) ||
           (av >= ConvertAvSkillFirst && av <= ConvertAvSkillLast && facts.recover);
}

// The category of a non-food Alchemy item: a poison is a poison whatever its
// effects; then restore, skill, and every other potion (no readable effect
// included, fail safe: other potions keep their number per kind).
inline CandidateType ConvertPotionType(const ConvertPotionFacts& facts) noexcept {
    if (facts.poison) return CandidateType::kPoison;
    if (ConvertRestoreEffect(facts)) return CandidateType::kRestorePotion;
    if (ConvertSkillEffect(facts)) return CandidateType::kSkillPotion;
    return CandidateType::kOtherPotion;
}

// The effect pool of a restore or skill potion (0 = not pooled): one pool
// per restored actor value (Health, Magicka, Stamina), one for Cure Disease,
// one per fortified skill actor value (design ruling Q4, the r2 model's
// pool_key).
inline constexpr std::uint32_t ConvertPoolRestore = 0x10000u;
inline constexpr std::uint32_t ConvertPoolCureDisease = 0x20000u;
inline constexpr std::uint32_t ConvertPoolSkill = 0x30000u;
inline std::uint32_t ConvertPotionPool(const ConvertPotionFacts& facts) noexcept {
    switch (ConvertPotionType(facts)) {
    case CandidateType::kRestorePotion:
        if (facts.archetype == ConvertArchetypeCureDisease) return ConvertPoolCureDisease;
        return ConvertPoolRestore | static_cast<std::uint32_t>(facts.actorValue & 0xFFFF);
    case CandidateType::kSkillPotion: return ConvertPoolSkill | static_cast<std::uint32_t>(facts.actorValue & 0xFFFF);
    default: return 0;
    }
}

// The two pooled categories.
inline bool ConvertPooledType(CandidateType type) noexcept {
    return type == CandidateType::kRestorePotion || type == CandidateType::kSkillPotion;
}

// The keep-limit categories (eight of v0.4.0, four of v0.5.0, three of
// v0.6.0).
inline bool ConvertKeepType(CandidateType type) noexcept {
    return type == CandidateType::kKeepIngredient || type == CandidateType::kCookedFood ||
           type == CandidateType::kRawFood || type == CandidateType::kDrink || type == CandidateType::kOre ||
           type == CandidateType::kGem || type == CandidateType::kPelt || type == CandidateType::kTool ||
           type == CandidateType::kRestorePotion || type == CandidateType::kSkillPotion ||
           type == CandidateType::kOtherPotion || type == CandidateType::kPoison ||
           type == CandidateType::kMidGear || type == CandidateType::kHighGear || type == CandidateType::kJewelry;
}

// The keep per kind of a category: its slider for a keep-limit category, 0
// for every other category (sell all). For a pooled category the slider is
// the keep of the whole pool; ConvertPotionPoolKeeps shares it out.
inline int ConvertKeepLimit(CandidateType type, const SelectorThresholds& thresholds) noexcept {
    switch (type) {
    case CandidateType::kKeepIngredient: return thresholds.keepIngredients;
    case CandidateType::kCookedFood: return thresholds.keepCookedFood;
    case CandidateType::kRawFood: return thresholds.keepRawFood;
    case CandidateType::kDrink: return thresholds.keepDrinks;
    case CandidateType::kOre: return thresholds.keepOre;
    case CandidateType::kGem: return thresholds.keepGems;
    case CandidateType::kPelt: return thresholds.keepPelts;
    case CandidateType::kTool: return thresholds.keepTools;
    case CandidateType::kRestorePotion: return thresholds.keepRestorePotions;
    case CandidateType::kSkillPotion: return thresholds.keepSkillPotions;
    case CandidateType::kOtherPotion: return thresholds.keepOtherPotions;
    case CandidateType::kPoison: return thresholds.keepPoisons;
    case CandidateType::kMidGear: return thresholds.keepMidGear;
    case CandidateType::kHighGear: return thresholds.keepHighGear;
    case CandidateType::kJewelry: return thresholds.keepJewelry;
    default: return 0;
    }
}

// The categories the keep-N engine sells (partial rows allowed): the keep-
// limit categories and the v0.2.0 sell-all ones with keep 0. Weak enchanted
// loot keeps the v0.3.0 whole-row rule (the r2 model keeps a loot row with
// any kept list whole).
inline bool ConvertPartialType(CandidateType type) noexcept {
    return ConvertKeepType(type) || type == CandidateType::kJunk || type == CandidateType::kWeakWeapon ||
           type == CandidateType::kWeakArmor || type == CandidateType::kClothing;
}

// ---------------------------------------------------------------------------
// Instance units (w-probe-r60, the v0.1.0 design
// addendum IU). The r59 L1 press on the heavy seed planned 3 rows and
// excluded 57 junk rows as not plain: their units sit in ExtraDataLists (one
// per unit, as the vanilla sale's per-list container events showed on the
// same seed). Every list is classified on its own. A list whose every extra
// is inert is convertible: its units are removed with that very list pointer
// (ConvertEngine.h RemoveInstances). Any other extra keeps the list, and a row
// with a kept list is never planned (so the plain units of a row are removed
// only once no list is left in it, the case proven live in r59).
// w-probe-r61 (addendum IU.10, the development notes): the r60 L1 kept all 57
// rows for two extras the offline read of the seed save names exactly: an
// ExtraReferenceHandle whose saved reference is null (3,878 of the 4,007
// handle lists; a picked-up world object whose reference is gone) and an
// ExtraScale (the picked-up object's size). Both are inert: a handle that
// resolves to no reference links the unit to nothing, and a scale changes no
// value. A handle that still resolves stays kept (fail closed).

// The receipt kinds of a kept list (one bit each; a row counts once per kind).
enum ConvertKeptKind : std::uint32_t {
    kConvertKeptWorn = 1u << 0,       // ExtraWorn / ExtraWornLeft
    kConvertKeptFavorite = 1u << 1,   // ExtraHotkey
    kConvertKeptEnchanted = 1u << 2,  // ExtraEnchantment
    kConvertKeptQuest = 1u << 3,      // ExtraAliasInstanceArray / ExtraFromAlias
    kConvertKeptOwner = 1u << 4,      // ExtraOwnership naming anyone but the player
    kConvertKeptValue = 1u << 5,      // ExtraHealth / ExtraCharge / ExtraPoison / ExtraSoul
    kConvertKeptRenamed = 1u << 6,    // ExtraTextDisplayData
    kConvertKeptReference = 1u << 7,  // a live ExtraReferenceHandle / ExtraOriginalReference / ExtraPromotedRef
    kConvertKeptOther = 1u << 8,      // any other extra: fail closed
};
inline constexpr std::size_t ConvertKeptKindCount = 9;

inline const char* ConvertKeptKindName(std::size_t bit) noexcept {
    switch (bit) {
    case 0: return "worn";
    case 1: return "favorite";
    case 2: return "enchanted";
    case 3: return "quest";
    case 4: return "foreignOwner";
    case 5: return "value";
    case 6: return "renamed";
    case 7: return "reference";
    case 8: return "other";
    default: return "other";
    }
}

// One extra of a list as the engine names it (ConvertEngine.h ConvertExtraOf).
enum class ConvertExtra : unsigned {
    kCount,        // ExtraCount: the list's unit count, never instance data
    kOwnership,    // ExtraOwnership (`foreignOwner` says whose)
    kUniqueId,     // ExtraUniqueID: inert bookkeeping of one inventory instance
    kLeveledItem,  // ExtraLeveledItem: inert leveled-list mark
    kCannotWear,   // ExtraCannotWear: inert for a sold unit
    kScale,        // ExtraScale (w-probe-r61): the size of a picked-up object, inert
    kDeadReference,  // ExtraReferenceHandle naming no reference (w-probe-r61): inert
    kWorn,
    kHotkey,
    kEnchantment,
    kAlias,
    kValue,
    kRenamed,
    kReference,    // a live ExtraReferenceHandle, ExtraOriginalReference, ExtraPromotedRef
    kOther,
};

// The verdict on one list, built extra by extra (AddConvertExtra).
struct ConvertListVerdict {
    bool instance{};             // at least one extra besides the count
    std::uint32_t kept{};        // ConvertKeptKind bits; 0 = every extra inert
    std::uint32_t otherType{};   // the raw type of the first kOther extra
    // v0.2.0: the foreign owner is a guard NPC (the engine
    // reads it from the ExtraOwnership owner; meaningful for a stolen list).
    bool guardOwner{};
    [[nodiscard]] bool Convertible() const noexcept { return instance && kept == 0; }
    // v0.2.0: a stolen list, i.e. its ONLY kept reason is an owner that
    // is not the player. With "Include stolen items" ON it is removed with
    // its own pointer like an inert list; a stolen list that is also worn,
    // favourite, enchanted, a quest alias, tempered, renamed, a live link or
    // anything else stays kept.
    [[nodiscard]] bool StolenOnly() const noexcept { return instance && kept == kConvertKeptOwner; }
    // The list's verdict under the run's switch.
    [[nodiscard]] bool ConvertibleWith(bool sellStolen) const noexcept {
        return Convertible() || (sellStolen && StolenOnly());
    }
};

inline void AddConvertExtra(ConvertListVerdict& verdict, ConvertExtra extra, bool foreignOwner,
                            std::uint32_t rawType) noexcept {
    if (extra == ConvertExtra::kCount) return;
    verdict.instance = true;
    switch (extra) {
    case ConvertExtra::kOwnership:
        if (foreignOwner) verdict.kept |= kConvertKeptOwner;
        return;
    case ConvertExtra::kUniqueId:
    case ConvertExtra::kLeveledItem:
    case ConvertExtra::kCannotWear:
    case ConvertExtra::kScale:
    case ConvertExtra::kDeadReference: return;
    case ConvertExtra::kWorn: verdict.kept |= kConvertKeptWorn; return;
    case ConvertExtra::kHotkey: verdict.kept |= kConvertKeptFavorite; return;
    case ConvertExtra::kEnchantment: verdict.kept |= kConvertKeptEnchanted; return;
    case ConvertExtra::kAlias: verdict.kept |= kConvertKeptQuest; return;
    case ConvertExtra::kValue: verdict.kept |= kConvertKeptValue; return;
    case ConvertExtra::kRenamed: verdict.kept |= kConvertKeptRenamed; return;
    case ConvertExtra::kReference: verdict.kept |= kConvertKeptReference; return;
    case ConvertExtra::kCount:
    case ConvertExtra::kOther: break;
    }
    if (!(verdict.kept & kConvertKeptOther)) verdict.otherType = rawType;
    verdict.kept |= kConvertKeptOther;
}

// At most this many distinct raw types of "other" extras per entry and per
// press reach the receipt.
inline constexpr std::size_t ConvertOtherTypesPerEntry = 4;
inline constexpr std::size_t ConvertOtherTypesPerRun = 8;
// Audit B-03 (a review): the first notPlain rows of a press, each with
// its FormKey, kept kinds and first "other" raw type, so a live run can tell
// which row a kind kept (bounded; the counts above cover the rest).
inline constexpr std::size_t ConvertKeptRowsPerRun = 16;
struct ConvertKeptRow {
    std::uint32_t formKey{};
    std::uint32_t kinds{};
    std::uint32_t otherType{};
};

// ---------------------------------------------------------------------------
// World gate (design r3 section 5.3, appendix R2 4.2). Evaluated before the
// read and again before every slice. Fixed priority; the first failed term
// is the reason of the one blocked line.

enum class ConvertGate : unsigned {
    kOk,
    kDisabled,      // Enabled OFF
    kRuntime,       // runtime or API mismatch
    kSettings,      // settings file unreadable
    kLatched,       // a MISMATCH / LOST earlier in this session
    kLoading,       // between kPreLoadGame and kPostLoadGame
    kSaving,        // a save started since the run began (mid-run only)
    kMenu,          // any menu open or the game paused
    kDialogue,      // dialogue menu or a live dialogue speaker
    kCombat,
    kDead,
    kKillMove,
    kControls,      // player controls disabled (a scene)
    kBusy,          // another run active
    kPrice,         // a GMST missing or not finite
};

struct ConvertWorldFacts {
    bool enabled{true};
    bool runtimeOk{true};
    bool settingsReadable{true};
    bool latched{};
    bool loading{};
    bool saving{};
    bool menuOpen{};
    bool dialogue{};
    bool combat{};
    bool dead{};
    bool killMove{};
    bool controlsDisabled{};
    bool busy{};
    bool priceKnown{true};
};

inline ConvertGate EvaluateConvertGate(const ConvertWorldFacts& facts) noexcept {
    if (!facts.enabled) return ConvertGate::kDisabled;
    if (!facts.runtimeOk) return ConvertGate::kRuntime;
    if (!facts.settingsReadable) return ConvertGate::kSettings;
    if (facts.latched) return ConvertGate::kLatched;
    if (facts.loading) return ConvertGate::kLoading;
    if (facts.saving) return ConvertGate::kSaving;
    if (facts.menuOpen) return ConvertGate::kMenu;
    if (facts.dialogue) return ConvertGate::kDialogue;
    if (facts.combat) return ConvertGate::kCombat;
    if (facts.dead) return ConvertGate::kDead;
    if (facts.killMove) return ConvertGate::kKillMove;
    if (facts.controlsDisabled) return ConvertGate::kControls;
    if (facts.busy) return ConvertGate::kBusy;
    if (!facts.priceKnown) return ConvertGate::kPrice;
    return ConvertGate::kOk;
}

inline const char* ConvertGateName(ConvertGate gate) noexcept {
    switch (gate) {
    case ConvertGate::kOk: return "OK";
    case ConvertGate::kDisabled: return "disabled";
    case ConvertGate::kRuntime: return "runtime";
    case ConvertGate::kSettings: return "settings";
    case ConvertGate::kLatched: return "latched";
    case ConvertGate::kLoading: return "loading";
    case ConvertGate::kSaving: return "saving";
    case ConvertGate::kMenu: return "menu";
    case ConvertGate::kDialogue: return "dialogue";
    case ConvertGate::kCombat: return "combat";
    case ConvertGate::kDead: return "dead";
    case ConvertGate::kKillMove: return "killmove";
    case ConvertGate::kControls: return "controls";
    case ConvertGate::kBusy: return "busy";
    case ConvertGate::kPrice: return "price";
    }
    return "OK";
}

// ---------------------------------------------------------------------------
// Plan (design r3 sections 4.1 and 5.1).

inline constexpr std::size_t ConvertMaxRows = SelectorMaxTransactions;
// One slice (one main-thread task) commits rows until the next row would
// start after this budget; at least one row per slice (decision D3: 8 ms).
inline constexpr std::uint64_t ConvertSliceBudgetUs = 8000;
// Watchdog of one press: slices and wall time from the key edge (4.1).
inline constexpr std::uint32_t ConvertMaxSlices = 64;
inline constexpr std::uint64_t ConvertWatchdogUs = 5000000;
// Pacing between slices: the next slice task starts no earlier than this
// after the previous one ended (the BatchSpillGapNs frame proxy).
inline constexpr std::uint64_t ConvertSliceGapUs = 16000;
// Audit A-03: the slice admits a row only while its removals plus the XP
// calls it will owe fit the budget. The per-call estimate starts at the
// design's 20 us fallback threshold and is replaced by the mean measured in
// the previous slice (at least 1 us).
inline constexpr std::uint64_t ConvertXpCallUsDefault = 20;
// w-probe-r60: the estimated cost of one instance-list removal (a fresh walk
// of the player's entries plus one RemoveItem with that list), counted
// against the slice budget before a row with instance lists is admitted.
inline constexpr std::uint64_t ConvertListCallUs = 100;
// v0.5.1: a whole-stack row (ConvertStackRow) keeps this charge per list
// although it is removed in one call: the slice admits rows by the v0.5.0
// estimate, and the measured time of the calls decides the rest, so a stack
// call that is cheaper than its lists can only end a slice sooner or let it
// take its next row earlier (never a row the v0.5.0 estimate refuses).
// Audit A-04: the player's gold count is a signed 32-bit engine count; a
// slice never credits past it.
inline constexpr std::int64_t ConvertGoldCeiling = 2147483647;
// v0.4.0 (the v0.2 update plan r2 section 4.4, design ruling): one UseSkill
// call per row, points = units x value (it replaces design r3's one call per
// unit, ruling D2), so the catch-up press of about 17,900 units stays inside
// the slice watchdog.
inline constexpr std::uint32_t ConvertXpCallsPerRow = 1;
// One slice never credits more than this much gold in one call (int32 safe).
inline constexpr std::int64_t ConvertSliceGoldCap = 1000000000;

// One inventory entry as the engine read found it. `candidate.count` is the
// entry's total count; `plainUnits` the units with no instance data (no
// ExtraDataList, or a list whose only extra is the count); `instanceUnits` /
// `instanceLists` the units and lists of convertible instance lists (w-probe-
// r60); `nonPlainUnits` the units of kept lists, whose kinds are `keptKinds`
// (receipt only). The absolute facts (quest, enchanted, favorite, equipped,
// owner) still describe the whole entry, so one flagged unit keeps the row.
struct ConvertEntry {
    CandidateRecord candidate{};
    std::int32_t plainUnits{};
    std::int32_t nonPlainUnits{};
    std::int32_t instanceUnits{};
    std::int32_t instanceLists{};
    std::uint32_t keptKinds{};
    std::array<std::uint32_t, ConvertOtherTypesPerEntry> otherTypes{};
    std::uint32_t otherTypeCount{};
    // v0.2.0: the part of instanceUnits / instanceLists that sits in
    // stolen lists (only with "Include stolen items" ON; OFF keeps them as
    // kept lists), and whether a built-in never-sell rule protects the kind
    // (tool, Lockpick, home-building material, unique; it also sets
    // candidate.formListProtected, so the selector excludes it as absolute).
    std::int32_t stolenUnits{};
    std::int32_t stolenLists{};
    bool builtinNever{};
    // v0.3.0: a weak enchanted loot row whose enchantment (and its base
    // enchantment) the player has not learned; with "Known enchants only" ON
    // the plan keeps it and counts it as "enchantment not known".
    bool enchantUnknown{};
    // v0.4.0 (keep-N): the unit count of every stolen list (convertible
    // with the switch ON, part of instanceUnits / instanceLists) and of every
    // inert list, each sorted ascending (the reader sorts; the plan refuses an
    // unsorted or inconsistent row), and the quest-food floor of the kind (the
    // units kept even when its slider is 0).
    std::vector<std::int32_t> stolenListUnits{};
    std::vector<std::int32_t> inertListUnits{};
    std::int32_t questFloor{};
    // v0.5.0: the effect pool of a restore or skill potion
    // (ConvertPotionPool; 0 = not pooled).
    std::uint32_t potionPool{};
    // v0.6.0: a mid or high enchanted loot row (ConvertMidHighLoot; its
    // type is kMidGear / kHighGear). `enchantUnknown` applies to it as to
    // weak loot.
    bool midHighLoot{};
};

enum class ConvertRowState : unsigned { kPlanned, kConfirmed, kRefused, kRolledBack, kMismatch, kLost };

inline const char* ConvertRowStateName(ConvertRowState state) noexcept {
    switch (state) {
    case ConvertRowState::kPlanned: return "PLANNED";
    case ConvertRowState::kConfirmed: return "CONFIRMED";
    case ConvertRowState::kRefused: return "REFUSED";
    case ConvertRowState::kRolledBack: return "ROLLED_BACK";
    case ConvertRowState::kMismatch: return "MISMATCH";
    case ConvertRowState::kLost: return "LOST";
    }
    return "PLANNED";
}

struct ConvertRow {
    std::uint32_t token{};
    std::uint32_t formKey{};
    std::int32_t value{};
    std::int32_t price{};
    // units = plainUnits + instanceUnits (the whole stack).
    std::int32_t units{};
    std::int32_t plainUnits{};
    std::int32_t instanceUnits{};
    std::int32_t lists{};
    // v0.2.0: the units of the row's stolen lists (part of instanceUnits).
    std::int32_t stolenUnits{};
    CandidateType type{CandidateType::kUnknown};
    // Total, kept (non-plain) and convertible-instance count when planned,
    // then as the slice read them.
    std::int32_t countBefore{};
    std::int32_t nonPlainBefore{};
    std::int32_t instanceBefore{};
    // v0.2.0: the stolen units when planned.
    std::int32_t stolenBefore{};
    std::int32_t countAfter{-1};
    std::uint32_t slice{};
    std::uint32_t xpCalls{};
    ConvertRowState state{ConvertRowState::kPlanned};
    // v0.4.0: a keep-N row that leaves units behind (the lists it names
    // are picked by kind: `stolenUnits` of stolen lists, then the rest of
    // `instanceUnits` of inert lists, smallest first), and the keep it
    // applied (max(the slider, the quest floor); 0 for sell all).
    bool partial{};
    std::int32_t keep{};
    // v0.5.1: the engine removal calls the slice made for this row
    // (receipt only; 0 for a row never reached or refused before a call).
    std::uint32_t removeCalls{};
};

// v0.5.1: a row removed with one engine call for its whole stack: it
// sells every unit it holds (not partial, no kept unit) and the v0.5.0 path
// would need two or more calls (its lists, plus one for its plain units). A
// row of one list or of plain units only is already one call and keeps its
// v0.5.0 path.
inline bool ConvertStackRow(const ConvertRow& row) noexcept {
    return !row.partial && row.instanceUnits > 0 && row.units == row.countBefore && row.nonPlainBefore == 0 &&
           row.lists + (row.plainUnits > 0 ? 1 : 0) >= 2;
}

// Why entries were not planned (receipt only). `rule` covers every Selector.h
// exclusion; the split by term is not needed for the anywhere receipt.
struct ConvertExclusions {
    std::uint32_t type{}, categoryOff{}, rule{}, absolute{}, owner{}, invalid{};
    std::uint32_t notPlain{}, zeroGold{}, runLimit{};
    // v0.2.0: rows a built-in never-sell rule protects (part of absolute).
    std::uint32_t builtin{};
    // v0.2.0 (audit B-03, interim until J3): why each kind that was not
    // planned stayed, one counter per kind: at or under a limit (the junk
    // value / Classic rule now, keep limits from v0.4.0), protected (quest,
    // favourite, worn, enchanted, protected name / form, built-in never-sell,
    // a kept list), stolen with the switch OFF, enchantment not known (
    // v0.3.0: weak enchanted loot whose enchantment the player has not
    // learned, with "Known enchants only" ON) and never sold (a type or
    // category that is off, worth 0 gold, or unreadable). A row cut by the
    // row limit is none of them.
    std::uint32_t keptAtLimit{}, keptProtected{}, keptStolen{}, keptEnchantUnknown{}, keptCategoryOff{};
    // w-probe-r60: the kept-list kinds of the notPlain rows (a row counts once
    // per kind) and the first distinct raw types of their "other" extras.
    std::array<std::uint32_t, ConvertKeptKindCount> keptKinds{};
    std::array<std::uint32_t, ConvertOtherTypesPerRun> otherTypes{};
    std::uint32_t otherTypeCount{};
    std::array<ConvertKeptRow, ConvertKeptRowsPerRun> keptRows{};
    std::uint32_t keptRowCount{};
};

struct ConvertPlan {
    bool valid{};
    std::vector<ConvertRow> rows{};
    std::uint32_t entriesRead{};
    std::uint32_t rowsEligible{};
    std::int64_t unitsEligible{};
    std::int64_t unitsPlanned{};
    std::int64_t goldPlanned{};
    // w-probe-r60: planned rows with instance units, those units and lists.
    std::uint32_t instanceRows{};
    std::int64_t instanceUnits{};
    std::int64_t instanceLists{};
    // v0.2.0: stolen units and rows of the plan.
    std::int64_t stolenUnits{};
    std::uint32_t stolenRows{};
    bool runLimited{};
    ConvertExclusions excluded{};
};

// The receipt tally of one notPlain row: each kept kind once, and its raw
// "other" types while the run's list has room (distinct only).
inline void TallyConvertKept(ConvertExclusions& excluded, const ConvertEntry& entry) noexcept {
    for (std::size_t bit = 0; bit < ConvertKeptKindCount; ++bit) {
        if (entry.keptKinds & (1u << bit)) ++excluded.keptKinds[bit];
    }
    if (excluded.keptRowCount < ConvertKeptRowsPerRun) {
        excluded.keptRows[excluded.keptRowCount++] =
            ConvertKeptRow{entry.candidate.baseFormKey, entry.keptKinds, entry.otherTypeCount ? entry.otherTypes[0] : 0};
    }
    const std::uint32_t own = entry.otherTypeCount < ConvertOtherTypesPerEntry ? entry.otherTypeCount :
                              static_cast<std::uint32_t>(ConvertOtherTypesPerEntry);
    for (std::uint32_t i = 0; i < own; ++i) {
        bool seen = false;
        for (std::uint32_t k = 0; k < excluded.otherTypeCount; ++k) seen = seen || excluded.otherTypes[k] == entry.otherTypes[i];
        if (!seen && excluded.otherTypeCount < ConvertOtherTypesPerRun) {
            excluded.otherTypes[excluded.otherTypeCount++] = entry.otherTypes[i];
        }
    }
}

inline void CountConvertExclusion(ConvertExclusions& excluded, const CandidateResult& result) noexcept {
    switch (result.decision) {
    case SelectorDecision::kExcludedType:
        if (result.term == SelectorTerm::kCategoryOff) ++excluded.categoryOff;
        else ++excluded.type;
        ++excluded.keptCategoryOff;
        return;
    case SelectorDecision::kExcludedRule:
        ++excluded.rule;
        ++excluded.keptAtLimit;
        return;
    case SelectorDecision::kExcludedAbsolute:
    case SelectorDecision::kExcludedStolen:
        ++excluded.absolute;
        ++excluded.keptProtected;
        return;
    case SelectorDecision::kExcludedUnknown:
        if (result.term == SelectorTerm::kOwnerUnknown) {
            ++excluded.owner;
            ++excluded.keptStolen;
        } else if (result.term == SelectorTerm::kType) {
            ++excluded.type;
            ++excluded.keptCategoryOff;
        } else {
            ++excluded.invalid;
            ++excluded.keptCategoryOff;
        }
        return;
    default:
        ++excluded.invalid;
        ++excluded.keptCategoryOff;
        return;
    }
}

// v0.2.0: the anywhere decision of one entry. The unchanged selector
// (Selector.h DecideCandidate, AND contract) decides every term up to the
// thresholds, so the absolute protections, the category switches and the
// owner term are exactly the v0.1.0 ones; only the last step, the rule of the
// category, differs on this path:
//   * junk with the "Value only" rule: value <= junkMaxValue, weight and
//     ratio dropped (Classic keeps the AND rule of the three thresholds);
//   * weak weapons, weak armor, clothing: sell all (no threshold);
//   * v0.4.0 keep-limit categories: more units than the keep per kind
//     (the plan then applies the quest floor and sells the surplus only).
//     v0.5.0: `poolKeep` >= 0 is the share of its effect pool a pooled
//     potion kind keeps (ConvertPotionPoolKeeps), used instead of the slider.
// The in-menu path never calls this function (it keeps the v0.0.4 rule).
inline CandidateResult DecideConvertCandidate(const CandidateRecord& candidate,
                                              const SelectorThresholds& thresholds,
                                              std::int32_t poolKeep = -1) noexcept {
    CandidateResult result = DecideCandidate(candidate, SelectorContract::kAnd, thresholds);
    if (result.decision != SelectorDecision::kSelect && result.decision != SelectorDecision::kExcludedRule) {
        return result;
    }
    bool accepted = result.decision == SelectorDecision::kSelect;
    if (candidate.type == CandidateType::kJunk && thresholds.junkValueOnly) {
        accepted = candidate.basePrice <= thresholds.junkMaxValue;
    } else if (ConvertSellAllType(candidate.type)) {
        accepted = true;
    } else if (ConvertKeepType(candidate.type)) {
        accepted = candidate.count > (poolKeep >= 0 ? poolKeep : ConvertKeepLimit(candidate.type, thresholds));
    }
    if (accepted) {
        result.decision = SelectorDecision::kSelect;
        result.term = SelectorTerm::kNone;
    } else {
        result.decision = SelectorDecision::kExcludedRule;
        result.term = thresholds.junkValueOnly && candidate.type == CandidateType::kJunk ? SelectorTerm::kPrice :
                                                                                            SelectorTerm::kContract;
    }
    return result;
}

// The reader's shortcut (ConvertEngine.h ReadConvertEntries): false when the
// category's own rule already refuses the row by its value and weight, so the
// row's expensive facts are never read. The same rule as the last step of
// DecideConvertCandidate, so the shortcut never changes a sale.
// v0.5.0: a pooled potion kind's share of its pool is known only once
// every kind is read, so its facts are always read.
inline bool ConvertRuleMayPass(const CandidateRecord& candidate, const SelectorThresholds& thresholds) noexcept {
    if (ConvertSellAllType(candidate.type) || ConvertPooledType(candidate.type)) return true;
    if (ConvertKeepType(candidate.type)) return candidate.count > ConvertKeepLimit(candidate.type, thresholds);
    if (candidate.type == CandidateType::kJunk && thresholds.junkValueOnly) {
        return candidate.basePrice <= thresholds.junkMaxValue;
    }
    const auto match = MatchThresholds(candidate, thresholds);
    return match.price && match.weight && match.ratio;
}

// v0.4.0 (the v0.2 update plan r2 section 3): what one keep-N row sells.
// sell = min(total - limit, convertible): stolen lists first (whole lists,
// smallest first, never past the surplus), then inert lists the same way,
// then plain units -- but plain units only while every list the entry keeps
// is a stolen list (a plain removal next to a tempered, renamed, soul,
// reference or unknown list is not proven: that row sells its lists only).
struct ConvertTake {
    std::int32_t sell{};
    std::int32_t stolenUnits{};
    std::int32_t listUnits{};
    std::int32_t lists{};
    std::int32_t plain{};
};

// True when the entry's list facts are consistent: every count non-negative,
// plain + instance + kept = total, kept units exactly when a kept kind is
// named (a kept list of no known kind is refused, fail closed), the stolen and inert list sizes sum to the
// units and number the lists, each list at least one unit, both ascending.
inline bool ConvertKeepFactsValid(const ConvertEntry& entry) noexcept {
    if (entry.plainUnits < 0 || entry.instanceUnits < 0 || entry.nonPlainUnits < 0 || entry.stolenUnits < 0 ||
        entry.stolenUnits > entry.instanceUnits || entry.stolenLists < 0 || entry.stolenLists > entry.instanceLists ||
        entry.plainUnits + entry.instanceUnits + entry.nonPlainUnits != entry.candidate.count ||
        (entry.nonPlainUnits > 0) != (entry.keptKinds != 0)) {
        return false;
    }
    const auto sum = [](const std::vector<std::int32_t>& sizes, std::int64_t& out) {
        out = 0;
        std::int32_t last = 1;
        for (const std::int32_t units : sizes) {
            if (units < last) return false;  // below one unit, or not ascending
            last = units;
            out += units;
        }
        return true;
    };
    std::int64_t stolen = 0, inert = 0;
    return sum(entry.stolenListUnits, stolen) && sum(entry.inertListUnits, inert) && stolen == entry.stolenUnits &&
           stolen + inert == entry.instanceUnits &&
           static_cast<std::int64_t>(entry.stolenListUnits.size()) == entry.stolenLists &&
           static_cast<std::int64_t>(entry.stolenListUnits.size() + entry.inertListUnits.size()) == entry.instanceLists;
}

inline ConvertTake ConvertKeepTake(const ConvertEntry& entry, std::int32_t limit) noexcept {
    ConvertTake take;
    const std::int32_t surplus = entry.candidate.count - (limit > 0 ? limit : 0);
    if (surplus <= 0) return take;
    std::int32_t rest = surplus;
    for (const std::int32_t units : entry.stolenListUnits) {
        if (units > rest) break;
        rest -= units;
        take.stolenUnits += units;
        ++take.lists;
    }
    for (const std::int32_t units : entry.inertListUnits) {
        if (units > rest) break;
        rest -= units;
        ++take.lists;
    }
    take.listUnits = surplus - rest;
    const bool plainAllowed = (entry.keptKinds & ~static_cast<std::uint32_t>(kConvertKeptOwner)) == 0;
    if (plainAllowed) take.plain = rest < entry.plainUnits ? rest : entry.plainUnits;
    take.sell = take.listUnits + take.plain;
    return take;
}

// v0.5.0 (the v0.2 update plan r2 section 2.2 row "Potions", design ruling
// Q4): the share of its effect pool every pooled potion kind keeps. The kinds
// of one pool (every entry of a switched-on restore or skill category with
// the same ConvertEntry::potionPool) are ranked strongest first -- the
// higher value, then the lower form id -- and take the pool's keep in that
// order: keep = min(count, what the pool has left). Every other entry gets
// -1 (not pooled). A kind the plan protects (favourite, quest, ...) still
// takes its place in the ranking: its units are kept anyway, so they fill
// the pool first (the r2 model's potion_pool). One sort over the pooled
// entries only, at plan time.
inline std::vector<std::int32_t> ConvertPotionPoolKeeps(std::span<const ConvertEntry> entries,
                                                        const SelectorThresholds& thresholds) {
    std::vector<std::int32_t> keeps(entries.size(), -1);
    std::vector<std::size_t> pooled;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        if (!ConvertPooledType(entry.candidate.type) || entry.potionPool == 0 || entry.candidate.count <= 0 ||
            CategoryOff(entry.candidate.type, thresholds)) {
            continue;
        }
        pooled.push_back(i);
    }
    std::sort(pooled.begin(), pooled.end(), [&entries](std::size_t a, std::size_t b) {
        const auto& x = entries[a];
        const auto& y = entries[b];
        if (x.potionPool != y.potionPool) return x.potionPool < y.potionPool;
        if (x.candidate.basePrice != y.candidate.basePrice) return x.candidate.basePrice > y.candidate.basePrice;
        if (x.candidate.baseFormKey != y.candidate.baseFormKey) return x.candidate.baseFormKey < y.candidate.baseFormKey;
        return a < b;
    });
    std::uint32_t pool = 0;
    std::int32_t left = 0;
    for (std::size_t k = 0; k < pooled.size(); ++k) {
        const auto& entry = entries[pooled[k]];
        if (k == 0 || entry.potionPool != pool) {
            pool = entry.potionPool;
            const int limit = ConvertKeepLimit(entry.candidate.type, thresholds);
            left = limit > 0 ? limit : 0;
        }
        const std::int32_t keep = entry.candidate.count < left ? entry.candidate.count : left;
        keeps[pooled[k]] = keep;
        left -= keep;
    }
    return keeps;
}

// The plan of one press: the unchanged AND selector (Selector.h
// DecideCandidate, the run's thresholds) on every entry, then whole stacks of
// plain and convertible instance units only (w-probe-r60), then p >= 1. At most thresholds.maxItems rows are planned; every
// further eligible row makes the press "limit reached, press again".
inline ConvertPlan BuildConvertPlan(std::span<const ConvertEntry> entries, const SelectorThresholds& thresholds,
                                    const ConvertPrice& price) {
    ConvertPlan plan;
    if (!price.known) return plan;
    plan.valid = true;
    plan.entriesRead = static_cast<std::uint32_t>(entries.size());
    std::size_t rowCap = thresholds.maxItems > 0 ? static_cast<std::size_t>(thresholds.maxItems) : 0;
    if (rowCap > ConvertMaxRows) rowCap = ConvertMaxRows;
    // v0.5.0: every pooled potion kind's share of its pool.
    const auto poolKeeps = ConvertPotionPoolKeeps(entries, thresholds);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        const std::int32_t poolKeep = poolKeeps[index];
        const auto result = DecideConvertCandidate(entry.candidate, thresholds, poolKeep);
        if (result.decision != SelectorDecision::kSelect) {
            CountConvertExclusion(plan.excluded, result);
            if (entry.builtinNever && result.decision == SelectorDecision::kExcludedAbsolute) ++plan.excluded.builtin;
            continue;
        }
        // v0.6.0: mid / high enchanted loot sells only while "Weak
        // enchanted loot" is ON too (never more enchanted loot than the weak
        // switch allows); like weak loot, a row with any kept list (a player
        // enchantment, tempered, renamed, stolen with the switch OFF, ...)
        // stays whole, and "Known enchants only" keeps unlearned loot. Only
        // then does its keep per kind apply (the r2 model's ench_loot_rule).
        if (entry.midHighLoot) {
            if (!thresholds.sellEnchantedLoot) {
                ++plan.excluded.absolute;
                ++plan.excluded.keptProtected;
                continue;
            }
            if (entry.nonPlainUnits != 0) {
                ++plan.excluded.notPlain;
                TallyConvertKept(plan.excluded, entry);
                if (entry.keptKinds == kConvertKeptOwner) ++plan.excluded.keptStolen;
                else ++plan.excluded.keptProtected;
                continue;
            }
            if (entry.enchantUnknown && thresholds.knownEnchantmentsOnly) {
                ++plan.excluded.keptEnchantUnknown;
                continue;
            }
        }
        // v0.4.0: the keep of the row (the slider, raised to the quest
        // floor; 0 = sell all) and the keep-N take. A keep-N category row
        // that sells everything and holds no kept list takes the whole-row
        // path below unchanged (the v0.3.0 shape); weak enchanted loot always
        // does.
        // v0.5.0: a pooled potion kind keeps its share of the pool.
        const std::int32_t slider = poolKeep >= 0 ? poolKeep : ConvertKeepLimit(entry.candidate.type, thresholds);
        const std::int32_t limit = ConvertPartialType(entry.candidate.type) ?
            (entry.questFloor > slider ? entry.questFloor : slider) : 0;
        const bool keepN = ConvertPartialType(entry.candidate.type) && (limit > 0 || entry.nonPlainUnits != 0);
        ConvertTake take;
        if (keepN) {
            if (!ConvertKeepFactsValid(entry)) {
                ++plan.excluded.notPlain;
                TallyConvertKept(plan.excluded, entry);
                ++plan.excluded.keptProtected;
                continue;
            }
            take = ConvertKeepTake(entry, limit);
            if (take.sell <= 0) {
                // Nothing convertible above the keep: a kept list holds the
                // surplus (protected, or stolen with the switch OFF), or the
                // row is at its floor / its lists are larger than the surplus.
                if (entry.nonPlainUnits > 0 && entry.candidate.count - limit > 0) {
                    ++plan.excluded.notPlain;
                    TallyConvertKept(plan.excluded, entry);
                    if (entry.keptKinds == kConvertKeptOwner) ++plan.excluded.keptStolen;
                    else ++plan.excluded.keptProtected;
                } else {
                    ++plan.excluded.rule;
                    ++plan.excluded.keptAtLimit;
                }
                continue;
            }
        }
        // Audit A-01, kept by w-probe-r60: a whole row with any kept list is
        // never planned. RemoveItem without an extra list is proven only on an
        // entry that holds no list (r59 L1), and the instance lists of a
        // planned row are all removed, each with its own pointer, before its
        // plain units; so the plain removal always meets that proven case.
        // v0.4.0: a keep-N row (above) may leave lists behind; its plain
        // removal is allowed only next to inert or stolen lists (ConvertKeepTake).
        const std::int32_t units = keepN ? take.sell : entry.plainUnits + entry.instanceUnits;
        if (!keepN && (units <= 0 || entry.plainUnits < 0 || entry.instanceUnits < 0 || entry.nonPlainUnits != 0 ||
            units != entry.candidate.count || entry.stolenUnits < 0 || entry.stolenUnits > entry.instanceUnits ||
            entry.stolenLists < 0 || entry.stolenLists > entry.instanceLists)) {
            ++plan.excluded.notPlain;
            TallyConvertKept(plan.excluded, entry);
            // v0.2.0: a row kept only by stolen lists (the switch OFF)
            // is "stolen"; any other kept list is "protected".
            if (entry.keptKinds == kConvertKeptOwner) ++plan.excluded.keptStolen;
            else ++plan.excluded.keptProtected;
            continue;
        }
        // v0.3.0: "Known enchants only" keeps weak enchanted loot whose
        // enchantment the player has not learned (the enchanting table loses
        // nothing); OFF sells it like any other loot row.
        if (entry.candidate.type == CandidateType::kEnchantedLoot && entry.enchantUnknown &&
            thresholds.knownEnchantmentsOnly) {
            ++plan.excluded.keptEnchantUnknown;
            continue;
        }
        const std::int32_t unitPrice = ConvertUnitPrice(entry.candidate.basePrice, price);
        if (unitPrice < 1) {
            ++plan.excluded.zeroGold;
            ++plan.excluded.keptCategoryOff;
            continue;
        }
        // A row whose own gold would not fit one credit is never planned.
        if (static_cast<std::int64_t>(units) * unitPrice > ConvertSliceGoldCap) {
            ++plan.excluded.zeroGold;
            ++plan.excluded.keptCategoryOff;
            continue;
        }
        ++plan.rowsEligible;
        plan.unitsEligible += units;
        if (plan.rows.size() >= rowCap) {
            ++plan.excluded.runLimit;
            plan.runLimited = true;
            continue;
        }
        ConvertRow row;
        row.token = static_cast<std::uint32_t>(plan.rows.size() + 1);
        row.formKey = entry.candidate.baseFormKey;
        row.value = entry.candidate.basePrice;
        row.price = unitPrice;
        row.units = units;
        row.plainUnits = keepN ? take.plain : entry.plainUnits;
        row.instanceUnits = keepN ? take.listUnits : entry.instanceUnits;
        row.lists = keepN ? take.lists : entry.instanceLists;
        row.stolenUnits = keepN ? take.stolenUnits : entry.stolenUnits;
        row.partial = keepN && units != entry.candidate.count;
        row.keep = limit;
        row.type = entry.candidate.type;
        row.countBefore = entry.candidate.count;
        row.nonPlainBefore = entry.nonPlainUnits;
        row.instanceBefore = entry.instanceUnits;
        row.stolenBefore = entry.stolenUnits;
        if (row.instanceUnits > 0) {
            ++plan.instanceRows;
            plan.instanceUnits += row.instanceUnits;
            plan.instanceLists += row.lists;
        }
        if (row.stolenUnits > 0) {
            ++plan.stolenRows;
            plan.stolenUnits += row.stolenUnits;
        }
        plan.unitsPlanned += row.units;
        plan.goldPlanned += static_cast<std::int64_t>(row.units) * row.price;
        plan.rows.push_back(row);
    }
    return plan;
}

// ---------------------------------------------------------------------------
// Commit (design r3 sections 4.1 and 4.3).

// One key as one read saw it: the entry's total count, the units of its kept
// lists (`nonPlain`) and the units of its convertible instance lists
// (`instance`, w-probe-r60). A key with no entry reads {0, 0, 0}.
struct ConvertCount {
    std::int32_t total{};
    std::int32_t nonPlain{};
    std::int32_t instance{};
    // v0.2.0 (audit A-01 of the v0.2.0 review): the units of the key's
    // stolen lists (part of `instance`, only with "Include stolen items" ON).
    std::int32_t stolen{};
};

class ConvertHost {
public:
    virtual ~ConvertHost() = default;
    virtual std::uint64_t NowUs() = 0;
    // One pass over the player's inventory: the count of each key and the
    // player's gold. False: the inventory could not be read (nothing may be
    // concluded from `out` or `gold`).
    virtual bool ReadCounts(std::span<const std::uint32_t> keys, std::span<ConvertCount> out,
                            std::int64_t& gold) = 0;
    // RemoveItem of `units` plain units (no extra list, no destination).
    // False: the base form did not resolve; nothing was called.
    virtual bool Remove(std::uint32_t formKey, std::int32_t units) = 0;
    // v0.4.0 (keep-N): removes `units` units of convertible instance
    // lists of one kind (`pick`: stolen lists or inert lists), list by list,
    // each time the SMALLEST list of that kind that still fits in what is left
    // (the plan's own order, ConvertKeepTake), re-found and re-classified in a
    // fresh walk before each call. Returns the units the calls asked for; the
    // caller's reads decide. The default (a host that cannot pick) asks
    // nothing, which refuses the row.
    enum class ListPick : unsigned { kStolen, kInert };
    virtual std::int32_t RemovePickedInstances(std::uint32_t formKey, std::int32_t units, ListPick pick) {
        (void)formKey;
        (void)units;
        (void)pick;
        return 0;
    }
    // w-probe-r60: removes `units` units of convertible instance lists, list
    // by list. Before each call the entry and the list are found again in a
    // fresh walk and the list is classified again (AddConvertExtra
    // rules); its whole count is removed with that list pointer, which is
    // never touched afterwards. Returns the units the calls asked for (0 when
    // nothing was called); fewer than `units` means a list vanished or no
    // longer fits, and the caller's verify read decides.
    virtual std::int32_t RemoveInstances(std::uint32_t formKey, std::int32_t units) = 0;
    // The rollback: the same number of plain units back.
    virtual bool AddUnits(std::uint32_t formKey, std::int32_t units) = 0;
    // One AddObjectToContainer(Gold001) of `amount`.
    virtual bool AddGold(std::int32_t amount) = 0;
    // One UseSkill(Speech, points) through the engine skill-use path
    // (ConvertEngine.h; the method name keeps the call token out of this file).
    virtual void GrantSpeechXp(float points) = 0;
    // v0.5.1: ONE engine removal of `units` units of the kind with no
    // extra list (the call the plain removal already makes), asked for the
    // whole stack of a ConvertStackRow. True when the call was made; the
    // caller's one-key read decides what it removed. The default (a host
    // without it) makes no call and returns false: the v0.5.0 per-list path
    // runs instead.
    virtual bool RemoveStack(std::uint32_t formKey, std::int32_t units) {
        (void)formKey;
        (void)units;
        return false;
    }
    // v0.5.1: true when this host has RemoveStack (the stack
    // path then re-reads the row and owns it; false keeps the v0.5.0 path).
    [[nodiscard]] virtual bool CanRemoveStack() const noexcept { return false; }
    // v0.5.1: every engine removal call this host made (one per
    // RemoveItem: plain, stack, or one list), counted by the implementation.
    // RunConvertSlice reads the difference per row and per slice (receipt).
    std::uint64_t removeCalls{};
};

enum class ConvertStop : unsigned {
    kNone,
    kReadFailed,      // the slice's first count read failed; nothing was removed
    kMismatch,        // a count dropped by another amount; rolled back, latched
    kRolledBack,      // the gold step failed; the slice's items were put back
    kLost,            // a rollback did not verify; latched
    kGoldMismatch,    // gold moved by another amount; latched
    kWatchdog,        // slices or wall time
    kGate,            // the world gate refused before a slice
    kLoadSave,        // a save or load started
};

inline const char* ConvertStopName(ConvertStop stop) noexcept {
    switch (stop) {
    case ConvertStop::kNone: return "none";
    case ConvertStop::kReadFailed: return "READ_FAILED";
    case ConvertStop::kMismatch: return "MISMATCH";
    case ConvertStop::kRolledBack: return "ROLLED_BACK";
    case ConvertStop::kLost: return "LOST";
    case ConvertStop::kGoldMismatch: return "GOLD_MISMATCH";
    case ConvertStop::kWatchdog: return "WATCHDOG";
    case ConvertStop::kGate: return "GATE";
    case ConvertStop::kLoadSave: return "LOAD_OR_SAVE";
    }
    return "none";
}

struct ConvertState {
    ConvertPlan plan{};
    std::size_t nextRow{};
    std::uint64_t edgeUs{};
    std::uint32_t slices{};
    std::uint64_t sliceMaxUs{};
    std::uint64_t sliceTotalUs{};
    std::int64_t unitsSold{};
    std::int64_t goldPaid{};
    std::uint32_t rowsSold{};
    std::uint32_t rowsRefused{};
    std::uint32_t rowsRolledBack{};
    std::uint32_t rowsMismatch{};
    std::uint32_t rowsLost{};
    // v0.2.0: the line parts: rows the press emptied (count after 0) and
    // the stolen units of the sold rows.
    std::uint32_t rowsCleared{};
    std::int64_t stolenUnitsSold{};
    std::uint64_t xpCalls{};
    // v0.5.1 (receipt, least load): the engine removal calls of the press
    // (each is one container-change event of the player), the gold credits,
    // the rows removed by one stack call, and the rows whose stack call left
    // units that the per-list path then removed (or tried to).
    std::uint64_t removeCalls{};
    std::uint32_t goldCalls{};
    std::uint32_t stackRows{};
    std::uint32_t stackFallbackRows{};
    // Every count read of every slice (re-resolve, verify, gold, rollback).
    std::uint64_t readCountsUs{};
    // Audit A-03: the XP cost estimate of the next slice (see above).
    std::uint64_t xpCallUs{ConvertXpCallUsDefault};
    ConvertStop stop{ConvertStop::kNone};
    // The gate that ended the run between slices (kGate), for the line.
    ConvertGate stopGate{ConvertGate::kOk};
    bool latched{};
    bool done{};
};

inline ConvertState BeginConvert(ConvertPlan plan, std::uint64_t edgeUs) {
    ConvertState state;
    state.plan = std::move(plan);
    state.edgeUs = edgeUs;
    state.done = !state.plan.valid || state.plan.rows.empty();
    return state;
}

// Ends the run before the next slice (a gate change, a save or load, the
// watchdog). Rows not reached stay Planned: they were never touched.
inline void StopConvert(ConvertState& state, ConvertStop stop, ConvertGate gate = ConvertGate::kOk) noexcept {
    if (state.stop == ConvertStop::kNone) {
        state.stop = stop;
        state.stopGate = gate;
    }
    state.done = true;
}

enum class ConvertSliceEnd : unsigned { kMore, kDone };

// Puts back `dropped[k]` units of every row in `rows` whose count dropped,
// then reads the counts again. True when every such row is back at its count
// before, its kept units unchanged. Instance units come back as plain units
// (their inert extras cannot be rebuilt: the player's own ownership, a unique
// id, the leveled mark), which is the same item for the player. `keys` / `back` are the caller's buffers, already sized to at least
// rows.size(), so nothing is allocated between a removal and its rollback.
inline bool RollBackConvertRows(ConvertState& state, ConvertHost& host, std::span<const std::size_t> rows,
                                std::span<const std::int32_t> dropped, std::span<std::uint32_t> keys,
                                std::span<ConvertCount> back) {
    for (std::size_t k = 0; k < rows.size(); ++k) {
        if (dropped[k] > 0) (void)host.AddUnits(state.plan.rows[rows[k]].formKey, dropped[k]);
    }
    for (std::size_t k = 0; k < rows.size(); ++k) keys[k] = state.plan.rows[rows[k]].formKey;
    std::int64_t gold = 0;
    const std::uint64_t readStart = host.NowUs();
    const bool read = host.ReadCounts(keys.first(rows.size()), back.first(rows.size()), gold);
    state.readCountsUs += host.NowUs() - readStart;
    if (!read) return false;
    bool ok = true;
    for (std::size_t k = 0; k < rows.size(); ++k) {
        const auto& row = state.plan.rows[rows[k]];
        if (back[k].total != row.countBefore || back[k].nonPlain != row.nonPlainBefore) ok = false;
        // Audit A-01 (the v0.2.0 review): units put back come back as plain
        // units, so a stolen list cannot be restored with its owner (the
        // pinned CommonLib offers no ExtraDataList constructor to rebuild
        // one). A row that had stolen units is therefore never reported as
        // returned: the verify sees the stolen units missing, the rollback
        // does not verify (LOST, latched, the "uncertain" line).
        if (back[k].stolen != row.stolenBefore) ok = false;
    }
    return ok;
}

// v0.5.1: the removal of one ConvertStackRow inside its slice. Audit
// A-01 (the v0.5.1 review): the slice's first read is older than the removals
// of the rows before this one, whose synchronous container-event sinks may
// have made a list of this row kept (its total unchanged), and the stack call
// names no list, so the row is read again right before the call: anything
// but exactly the planned total, kept, instance and stolen units (or a failed
// read) refuses the row with no call (`tried` true, `called` false). One
// RemoveStack call then asks for the whole stack; a one-key read in the same
// task then proves it gone (count 0). If the engine left units, and they are only
// units the row planned to sell (no kept unit appeared, no more than planned
// of any kind), the v0.5.0 steps finish them: RemoveInstances for the lists
// left, a one-key read proving no list is left, then the plain
// removal. Anything else stops the row and the slice's verify read decides
// (MISMATCH: rollback, latch). `mid` is the caller's buffer (no allocation
// between a removal and its verify). A host without RemoveStack returns
// `tried` false and the caller runs the v0.5.0 path unchanged.
struct ConvertStackResult {
    bool tried{};   // the stack path took the row (the v0.5.0 path does not run)
    bool called{};  // the stack call was made (a removal was asked for)
    bool whole{};   // the one-key reads prove every unit of the row gone
};

inline bool ReadConvertOne(ConvertState& state, ConvertHost& host, std::uint32_t formKey, ConvertCount& out) {
    std::int64_t gold = 0;
    const std::uint64_t start = host.NowUs();
    const bool read = host.ReadCounts(std::span<const std::uint32_t>(&formKey, 1), std::span<ConvertCount>(&out, 1),
                                      gold);
    state.readCountsUs += host.NowUs() - start;
    return read;
}

inline ConvertStackResult RemoveConvertStack(ConvertState& state, ConvertHost& host, const ConvertRow& row,
                                             ConvertCount& mid) {
    ConvertStackResult result;
    if (!host.CanRemoveStack()) return result;
    result.tried = true;
    ConvertCount fresh{};
    if (!ReadConvertOne(state, host, row.formKey, fresh) || fresh.total != row.countBefore ||
        fresh.nonPlain != row.nonPlainBefore || fresh.instance != row.instanceBefore ||
        fresh.stolen != row.stolenBefore) {
        return result;
    }
    if (!host.RemoveStack(row.formKey, row.units)) return result;
    result.called = true;
    if (!ReadConvertOne(state, host, row.formKey, mid)) return result;
    if (mid.total == row.countBefore - row.units && mid.nonPlain == row.nonPlainBefore && mid.instance == 0 &&
        mid.stolen == 0) {
        ++state.stackRows;
        result.whole = true;
        return result;
    }
    const std::int32_t plainLeft = mid.total - mid.nonPlain - mid.instance;
    const bool onlyPlanned = mid.nonPlain == row.nonPlainBefore && mid.total > 0 && mid.total <= row.countBefore &&
                             mid.instance >= 0 && mid.instance <= row.instanceUnits && mid.stolen >= 0 &&
                             mid.stolen <= row.stolenUnits && mid.stolen <= mid.instance && plainLeft >= 0 &&
                             plainLeft <= row.plainUnits;
    if (!onlyPlanned) return result;
    ++state.stackFallbackRows;
    bool whole = true;
    if (mid.instance > 0) {
        whole = host.RemoveInstances(row.formKey, mid.instance) == mid.instance;
        if (whole && plainLeft > 0) {
            ConvertCount again{};
            whole = ReadConvertOne(state, host, row.formKey, again) && again.instance == 0 && again.stolen == 0 &&
                    again.nonPlain == row.nonPlainBefore && again.total == row.nonPlainBefore + plainLeft;
        }
    }
    if (whole && plainLeft > 0) whole = host.Remove(row.formKey, plainLeft);
    result.whole = whole;
    return result;
}

// ONE call is ONE main-thread task (design r3 4.1): at least one row, then
// rows back to back until the next one would start after
// ConvertSliceBudgetUs. Order inside the slice (4.3): re-resolve every row
// (one read) -> RemoveItem each -> verify all counts (one read) -> one gold
// credit for the verified rows -> verify gold -> UseSkill per sold row (v0.4.0).
// XP never runs on a refused, rolled-back or mismatched row and is never
// rolled back. The function queues nothing.
inline ConvertSliceEnd RunConvertSlice(ConvertState& state, ConvertHost& host) {
    if (state.done) return ConvertSliceEnd::kDone;
    auto& rows = state.plan.rows;
    if (state.nextRow >= rows.size()) {
        state.done = true;
        return ConvertSliceEnd::kDone;
    }
    const std::uint64_t start = host.NowUs();
    if (state.slices >= ConvertMaxSlices ||
        (state.edgeUs && start >= state.edgeUs && start - state.edgeUs >= ConvertWatchdogUs)) {
        StopConvert(state, ConvertStop::kWatchdog);
        return ConvertSliceEnd::kDone;
    }
    const std::uint32_t slice = ++state.slices;
    // v0.5.1: the host's removal-call count at the slice start (receipt).
    const std::uint64_t callsAtStart = host.removeCalls;
    // 1. Re-resolve: one read of every remaining row and the player's gold.
    std::vector<std::uint32_t> keys;
    keys.reserve(rows.size() - state.nextRow);
    for (std::size_t i = state.nextRow; i < rows.size(); ++i) keys.push_back(rows[i].formKey);
    std::vector<ConvertCount> before(keys.size());
    std::int64_t goldBefore = 0;
    const std::uint64_t readStart = host.NowUs();
    if (!host.ReadCounts(keys, before, goldBefore)) {
        state.readCountsUs += host.NowUs() - readStart;
        StopConvert(state, ConvertStop::kReadFailed);
        return ConvertSliceEnd::kDone;
    }
    state.readCountsUs += host.NowUs() - readStart;
    // Every buffer the rest of the slice needs is allocated here, before the
    // first removal, so an allocation failure can never leave removed units
    // without their gold or their rollback.
    const std::size_t remaining = keys.size();
    std::vector<std::size_t> removed;
    removed.reserve(remaining);
    std::vector<std::uint32_t> removedKeys(remaining);
    std::vector<ConvertCount> after(remaining);
    std::vector<std::int32_t> dropped(remaining, 0);
    std::vector<std::size_t> sold;
    sold.reserve(remaining);
    std::vector<std::int32_t> soldDropped;
    soldDropped.reserve(remaining);
    // 2. Remove rows back to back inside the budget. A row is admitted while
    // the time spent plus the XP calls the slice will owe (this row's
    // included) fits ConvertSliceBudgetUs; the first row always is.
    std::int64_t sliceGold = 0;
    std::uint64_t pendingXpCalls = 0;
    const std::size_t first = state.nextRow;
    // Audit A-01: the one-key read between a row's instance and plain removal.
    ConvertCount mid{};
    while (state.nextRow < rows.size()) {
        auto& row = rows[state.nextRow];
        const std::uint64_t rowCalls = ConvertXpCallsPerRow;
        // v0.5.1: a whole-stack row is one engine call for its stack.
        const bool stackRow = ConvertStackRow(row);
        // w-probe-r60: the row's own list removals count against the budget too.
        const std::uint64_t rowListUs = static_cast<std::uint64_t>(row.lists > 0 ? row.lists : 0) * ConvertListCallUs;
        if (!removed.empty() &&
            (host.NowUs() - start) + (pendingXpCalls + rowCalls) * state.xpCallUs + rowListUs >= ConvertSliceBudgetUs) {
            break;
        }
        const auto& now = before[state.nextRow - first];
        // A row whose units changed since the plan is refused (never an
        // error): the read no longer describes it.
        const bool current = now.total == row.countBefore && now.nonPlain == row.nonPlainBefore &&
                             now.stolen == row.stolenBefore &&
                             now.instance == row.instanceBefore &&
                             now.total - now.nonPlain >= row.units;
        const std::int64_t rowGold = static_cast<std::int64_t>(row.units) * row.price;
        if (current && sliceGold + rowGold > ConvertSliceGoldCap && !removed.empty()) break;
        // Audit A-04: a row whose gold would push the player's count past its
        // signed 32-bit range is refused before anything is removed.
        const bool headroom = goldBefore >= 0 && goldBefore + sliceGold + rowGold <= ConvertGoldCeiling;
        ++state.nextRow;
        row.slice = slice;
        // w-probe-r60: the instance lists first (each with its own pointer),
        // then the plain units, once the entry holds no list. A partial
        // instance removal skips the plain step and still goes to the verify,
        // which sees the short drop (MISMATCH: rollback, latch).
        bool called = false;
        bool whole = current && headroom;
        const std::uint64_t rowCallsAtStart = host.removeCalls;
        // v0.5.1: a whole-stack row goes in one engine call (above).
        bool stacked = false;
        if (whole && stackRow) {
            const auto stack = RemoveConvertStack(state, host, row, mid);
            stacked = stack.tried;
            if (stacked) {
                called = stack.called;
                whole = stack.whole;
            }
        }
        if (!stacked && whole && row.instanceUnits > 0) {
            std::int32_t asked = 0;
            if (row.partial) {
                // v0.4.0: the stolen lists first, then the inert ones,
                // each kind by the plan's own pick (smallest first).
                if (row.stolenUnits > 0) {
                    asked = host.RemovePickedInstances(row.formKey, row.stolenUnits, ConvertHost::ListPick::kStolen);
                }
                if (asked == row.stolenUnits && row.instanceUnits > row.stolenUnits) {
                    asked += host.RemovePickedInstances(row.formKey, row.instanceUnits - row.stolenUnits,
                                                        ConvertHost::ListPick::kInert);
                }
            } else {
                asked = host.RemoveInstances(row.formKey, row.instanceUnits);
            }
            called = asked > 0;
            whole = asked == row.instanceUnits;
            // Audit A-01 (a review): a request is no proof of a
            // removal, and a container-event sink may have changed the row
            // inside the calls. Before the plain removal the row is read again
            // in this task: every instance unit gone, the kept units as
            // planned and exactly the plain units left. Otherwise the plain
            // units are not touched and the verify read decides.
            if (whole && row.plainUnits > 0) {
                std::int64_t midGold = 0;
                const std::uint64_t midStart = host.NowUs();
                const bool midRead = host.ReadCounts(std::span<const std::uint32_t>(&row.formKey, 1),
                                                     std::span<ConvertCount>(&mid, 1), midGold);
                state.readCountsUs += host.NowUs() - midStart;
                // v0.4.0: in general form (a keep-N row keeps lists and
                // plain units): exactly the removed list units gone, the kept
                // and stolen units as planned. For a whole row this is the
                // v0.1.0 test (no list left, plain + kept units remain).
                whole = midRead && mid.instance == row.instanceBefore - row.instanceUnits &&
                        mid.stolen == row.stolenBefore - row.stolenUnits && mid.nonPlain == row.nonPlainBefore &&
                        mid.total == row.countBefore - row.instanceUnits;
            }
        }
        if (!stacked && whole && row.plainUnits > 0) {
            whole = host.Remove(row.formKey, row.plainUnits);
            called = called || whole;
        }
        row.removeCalls = static_cast<std::uint32_t>(host.removeCalls - rowCallsAtStart);
        if (!called) {
            row.state = ConvertRowState::kRefused;
            row.countAfter = now.total;
            ++state.rowsRefused;
            continue;
        }
        removed.push_back(state.nextRow - 1);
        sliceGold += rowGold;
        pendingXpCalls += rowCalls;
    }
    if (removed.empty()) {
        state.removeCalls += host.removeCalls - callsAtStart;
        const std::uint64_t spent = host.NowUs() - start;
        state.sliceTotalUs += spent;
        if (spent > state.sliceMaxUs) state.sliceMaxUs = spent;
        if (state.nextRow >= rows.size()) state.done = true;
        return state.done ? ConvertSliceEnd::kDone : ConvertSliceEnd::kMore;
    }
    // 3. Verify every removed row with one read.
    const std::size_t count = removed.size();
    for (std::size_t k = 0; k < count; ++k) removedKeys[k] = rows[removed[k]].formKey;
    std::int64_t goldMid = 0;
    const std::uint64_t verifyStart = host.NowUs();
    const bool afterOk = host.ReadCounts(std::span<const std::uint32_t>(removedKeys.data(), count),
                                         std::span<ConvertCount>(after.data(), count), goldMid);
    state.readCountsUs += host.NowUs() - verifyStart;
    std::int64_t gold = 0;
    bool mismatch = !afterOk || goldMid != goldBefore;
    for (std::size_t k = 0; k < count && afterOk; ++k) {
        auto& row = rows[removed[k]];
        row.countAfter = after[k].total;
        dropped[k] = row.countBefore - after[k].total;
        if (dropped[k] == 0) {
            // The count did not drop: refused, not in the gold sum.
            row.state = ConvertRowState::kRefused;
            ++state.rowsRefused;
        } else if (dropped[k] != row.units || after[k].nonPlain != row.nonPlainBefore ||
                   after[k].instance != row.instanceBefore - row.instanceUnits) {
            mismatch = true;
        } else if (after[k].stolen != row.stolenBefore - row.stolenUnits) {
            // v0.4.0: the engine took another list kind than planned
            // (an inert list for a stolen one or the reverse).
            mismatch = true;
        } else {
            gold += static_cast<std::int64_t>(row.units) * row.price;
        }
    }
    if (!afterOk) {
        // Audit A-02: the verify read failed, so what was removed is unknown.
        // Nothing is added back on a guess (that could duplicate units the
        // engine did not remove): no gold, no XP, the rows are LOST, latched,
        // the line is "uncertain".
        for (std::size_t k = 0; k < count; ++k) {
            auto& row = rows[removed[k]];
            row.state = ConvertRowState::kLost;
            ++state.rowsLost;
        }
        state.latched = true;
        StopConvert(state, ConvertStop::kLost);
    } else if (mismatch) {
        // A count moved by another amount: roll the slice's removed rows back
        // by what each actually lost, latch, end the run.
        const bool back = RollBackConvertRows(state, host, removed, std::span<const std::int32_t>(dropped.data(), count),
                                              removedKeys, after);
        for (std::size_t k = 0; k < count; ++k) {
            auto& row = rows[removed[k]];
            if (row.state == ConvertRowState::kRefused) continue;
            row.state = back ? ConvertRowState::kMismatch : ConvertRowState::kLost;
            if (back) ++state.rowsMismatch; else ++state.rowsLost;
        }
        state.latched = true;
        StopConvert(state, back ? ConvertStop::kMismatch : ConvertStop::kLost);
    } else if (gold > 0) {
        // 4. One gold credit for the verified rows, then its verify.
        ++state.goldCalls;
        const bool added = host.AddGold(static_cast<std::int32_t>(gold));
        std::int64_t goldAfter = 0;
        const std::uint64_t goldStart = host.NowUs();
        const bool goldRead = host.ReadCounts({}, {}, goldAfter);
        state.readCountsUs += host.NowUs() - goldStart;
        const std::int64_t delta = goldRead ? goldAfter - goldBefore : -1;
        if (goldRead && delta == gold) {
            // 5. Sold: XP per row, in the same task, after the gold verified.
            const std::uint64_t xpStart = host.NowUs();
            std::uint64_t xpCalls = 0;
            for (const std::size_t index : removed) {
                auto& row = rows[index];
                if (row.state == ConvertRowState::kRefused) continue;
                row.state = ConvertRowState::kConfirmed;
                ++state.rowsSold;
                state.unitsSold += row.units;
                state.stolenUnitsSold += row.stolenUnits;
                if (row.countAfter == 0) ++state.rowsCleared;
                // v0.4.0: one call per row, units x value points.
                if (row.units > 0) {
                    host.GrantSpeechXp(static_cast<float>(static_cast<double>(row.value) * row.units));
                    ++row.xpCalls;
                    ++state.xpCalls;
                    ++xpCalls;
                }
            }
            if (xpCalls > 0) {
                const std::uint64_t mean = (host.NowUs() - xpStart) / xpCalls;
                state.xpCallUs = mean > 0 ? mean : 1;
            }
            state.goldPaid += gold;
        } else if (goldRead && delta == 0) {
            // The gold step failed and left the gold unchanged: rollback.
            (void)added;
            for (std::size_t k = 0; k < count; ++k) {
                if (rows[removed[k]].state == ConvertRowState::kRefused) continue;
                sold.push_back(removed[k]);
                soldDropped.push_back(dropped[k]);
            }
            const bool back = RollBackConvertRows(state, host, sold, soldDropped, removedKeys, after);
            for (const std::size_t index : sold) {
                rows[index].state = back ? ConvertRowState::kRolledBack : ConvertRowState::kLost;
                if (back) ++state.rowsRolledBack; else ++state.rowsLost;
            }
            if (!back) state.latched = true;
            StopConvert(state, back ? ConvertStop::kRolledBack : ConvertStop::kLost);
        } else {
            // Gold moved by another amount (or could not be read): nothing
            // can be put back honestly. Uncertain, latched.
            for (const std::size_t index : removed) {
                if (rows[index].state == ConvertRowState::kRefused) continue;
                rows[index].state = ConvertRowState::kLost;
                ++state.rowsLost;
            }
            state.latched = true;
            StopConvert(state, ConvertStop::kGoldMismatch);
        }
    }
    state.removeCalls += host.removeCalls - callsAtStart;
    const std::uint64_t spent = host.NowUs() - start;
    state.sliceTotalUs += spent;
    if (spent > state.sliceMaxUs) state.sliceMaxUs = spent;
    if (state.nextRow >= rows.size()) state.done = true;
    return state.done ? ConvertSliceEnd::kDone : ConvertSliceEnd::kMore;
}

// ---------------------------------------------------------------------------
// One honest line per press (design r3 section 6).

enum class ConvertOutcome : unsigned { kNone, kSold, kPartial, kNothing, kReturned, kUncertain, kBlocked };

enum class ConvertReason : unsigned {
    kNone,
    kLimit,
    kStopped,
    kRunning,
    kOff,
    kSettings,
    kUnavailable,
    kCombat,
    kWorldBusy,
    kLoadSave,
    kPrice,
    kLatchedWorld,
};

struct ConvertResultLine {
    ConvertOutcome outcome{ConvertOutcome::kNone};
    ConvertReason reason{ConvertReason::kNone};
    std::int64_t n{};
    std::int64_t m{};
    std::int64_t g{};
    // v0.2.0: kinds cleared ({c}) and stolen units sold ({s}).
    std::int64_t c{};
    std::int64_t s{};
};

inline ConvertReason ConvertGateReason(ConvertGate gate) noexcept {
    switch (gate) {
    case ConvertGate::kOk: return ConvertReason::kNone;
    case ConvertGate::kDisabled: return ConvertReason::kOff;
    case ConvertGate::kRuntime: return ConvertReason::kUnavailable;
    case ConvertGate::kSettings: return ConvertReason::kSettings;
    case ConvertGate::kLatched: return ConvertReason::kLatchedWorld;
    case ConvertGate::kLoading:
    case ConvertGate::kSaving: return ConvertReason::kLoadSave;
    case ConvertGate::kMenu:
    case ConvertGate::kDialogue:
    case ConvertGate::kKillMove:
    case ConvertGate::kControls: return ConvertReason::kWorldBusy;
    case ConvertGate::kCombat: return ConvertReason::kCombat;
    case ConvertGate::kDead: return ConvertReason::kUnavailable;
    case ConvertGate::kBusy: return ConvertReason::kRunning;
    case ConvertGate::kPrice: return ConvertReason::kPrice;
    }
    return ConvertReason::kUnavailable;
}

struct ConvertOutcomeInput {
    ConvertGate gate{ConvertGate::kOk};
    std::int64_t unitsEligible{};  // m
    std::int64_t unitsSold{};      // n
    std::int64_t goldPaid{};       // g
    bool runLimited{};
    ConvertStop stop{ConvertStop::kNone};
    ConvertGate stopGate{ConvertGate::kOk};
    std::uint32_t rowsRefused{};
    std::uint32_t rowsLost{};
    std::uint32_t rowsMismatch{};
    std::uint32_t rowsCleared{};
    std::int64_t stolenUnitsSold{};
};

inline ConvertOutcomeInput ConvertOutcomeInputFrom(const ConvertState& state) noexcept {
    ConvertOutcomeInput input;
    input.unitsEligible = state.plan.unitsEligible;
    input.unitsSold = state.unitsSold;
    input.goldPaid = state.goldPaid;
    input.runLimited = state.plan.runLimited;
    input.stop = state.stop;
    input.stopGate = state.stopGate;
    input.rowsRefused = state.rowsRefused;
    input.rowsLost = state.rowsLost;
    input.rowsMismatch = state.rowsMismatch;
    input.rowsCleared = state.rowsCleared;
    input.stolenUnitsSold = state.stolenUnitsSold;
    if (!state.plan.valid) input.gate = ConvertGate::kPrice;
    return input;
}

// The reason a stopped run names: the gate that ended it, a save or load, or
// the generic safety stop.
inline ConvertReason ConvertStopReason(const ConvertOutcomeInput& input) noexcept {
    if (input.stop == ConvertStop::kLoadSave) return ConvertReason::kLoadSave;
    if (input.stop == ConvertStop::kGate) {
        const auto reason = ConvertGateReason(input.stopGate);
        return reason == ConvertReason::kNone ? ConvertReason::kStopped : reason;
    }
    return ConvertReason::kStopped;
}

inline ConvertResultLine ClassifyConvertOutcome(const ConvertOutcomeInput& input) noexcept {
    ConvertResultLine line;
    line.n = input.unitsSold;
    line.m = input.unitsEligible;
    line.g = input.goldPaid;
    line.c = input.rowsCleared;
    line.s = input.stolenUnitsSold;
    if (input.gate != ConvertGate::kOk) {
        line.outcome = ConvertOutcome::kBlocked;
        line.reason = ConvertGateReason(input.gate);
        line.n = 0;
        line.g = 0;
        return line;
    }
    if (input.rowsLost > 0 || input.rowsMismatch > 0 || input.stop == ConvertStop::kLost ||
        input.stop == ConvertStop::kMismatch || input.stop == ConvertStop::kGoldMismatch) {
        line.outcome = ConvertOutcome::kUncertain;
        return line;
    }
    if (input.unitsEligible <= 0) {
        line.outcome = ConvertOutcome::kNothing;
        return line;
    }
    if (input.unitsSold >= input.unitsEligible) {
        line.outcome = ConvertOutcome::kSold;
        return line;
    }
    if (input.unitsSold > 0) {
        // Partial: the stop that ended the run first, else the row limit, else
        // rows the slice refused (their count changed since the plan).
        line.outcome = ConvertOutcome::kPartial;
        line.reason = input.stop != ConvertStop::kNone ? ConvertStopReason(input) :
                      input.runLimited ? ConvertReason::kLimit : ConvertReason::kStopped;
        return line;
    }
    if (input.stop == ConvertStop::kRolledBack) {
        line.outcome = ConvertOutcome::kReturned;
        return line;
    }
    line.outcome = ConvertOutcome::kBlocked;
    line.reason = input.stop == ConvertStop::kNone ? ConvertReason::kUnavailable : ConvertStopReason(input);
    if (line.reason == ConvertReason::kStopped) line.reason = ConvertReason::kUnavailable;
    line.g = 0;
    return line;
}

inline const char* ConvertOutcomeKey(ConvertOutcome value) noexcept {
    switch (value) {
    case ConvertOutcome::kSold: return "$AGH_JS_RESULT_SOLD";
    case ConvertOutcome::kPartial: return "$AGH_JS_RESULT_PARTIAL";
    case ConvertOutcome::kNothing: return "$AGH_JS_RESULT_NOTHING";
    case ConvertOutcome::kReturned: return "$AGH_JS_RESULT_RETURNED";
    case ConvertOutcome::kUncertain: return "$AGH_JS_RESULT_UNCERTAIN";
    case ConvertOutcome::kBlocked: return "$AGH_JS_RESULT_BLOCKED";
    case ConvertOutcome::kNone: return "";
    }
    return "";
}

// v0.2.0 (the v0.2 update plan r2 section 4.3): the key of the one line.
// A sold press that emptied rows names them ("{c} kinds cleared"), and one
// that sold stolen units also names those ("({s} stolen)"); every other line
// is the v0.1.0 key. Both forms fit the 75 / 52 bounds at n = 99,999,
// g = 9,999,999, c = 999, s = 99,999 (js_resulttext_test).
inline const char* ConvertLineKey(const ConvertResultLine& line) noexcept {
    if (line.outcome == ConvertOutcome::kSold && line.s > 0) return "$AGH_JS_RESULT_SOLD_STOLEN";
    if (line.outcome == ConvertOutcome::kSold && line.c > 0) return "$AGH_JS_RESULT_SOLD_CLEARED";
    return ConvertOutcomeKey(line.outcome);
}

inline const char* ConvertReasonKey(ConvertReason value) noexcept {
    switch (value) {
    case ConvertReason::kLimit: return "$AGH_JS_REASON_LIMIT";
    case ConvertReason::kStopped: return "$AGH_JS_REASON_STOPPED";
    case ConvertReason::kRunning: return "$AGH_JS_REASON_RUNNING";
    case ConvertReason::kOff: return "$AGH_JS_REASON_OFF";
    case ConvertReason::kSettings: return "$AGH_JS_REASON_SETTINGS";
    case ConvertReason::kUnavailable: return "$AGH_JS_REASON_UNAVAILABLE";
    case ConvertReason::kCombat: return "$AGH_JS_REASON_COMBAT";
    case ConvertReason::kWorldBusy: return "$AGH_JS_REASON_WORLD_BUSY";
    case ConvertReason::kLoadSave: return "$AGH_JS_REASON_LOADSAVE";
    case ConvertReason::kPrice: return "$AGH_JS_REASON_PRICE";
    case ConvertReason::kLatchedWorld: return "$AGH_JS_REASON_LATCHED_WORLD";
    case ConvertReason::kNone: return "";
    }
    return "";
}

inline const char* ConvertOutcomeName(ConvertOutcome value) noexcept {
    switch (value) {
    case ConvertOutcome::kSold: return "sold";
    case ConvertOutcome::kPartial: return "partial";
    case ConvertOutcome::kNothing: return "nothing";
    case ConvertOutcome::kReturned: return "returned";
    case ConvertOutcome::kUncertain: return "uncertain";
    case ConvertOutcome::kBlocked: return "blocked";
    case ConvertOutcome::kNone: return "none";
    }
    return "none";
}

inline const char* ConvertReasonName(ConvertReason value) noexcept {
    switch (value) {
    case ConvertReason::kLimit: return "limit";
    case ConvertReason::kStopped: return "stopped";
    case ConvertReason::kRunning: return "running";
    case ConvertReason::kOff: return "off";
    case ConvertReason::kSettings: return "settings";
    case ConvertReason::kUnavailable: return "unavailable";
    case ConvertReason::kCombat: return "combat";
    case ConvertReason::kWorldBusy: return "world_busy";
    case ConvertReason::kLoadSave: return "loadsave";
    case ConvertReason::kPrice: return "price";
    case ConvertReason::kLatchedWorld: return "latched_world";
    case ConvertReason::kNone: return "none";
    }
    return "none";
}

// ---------------------------------------------------------------------------
// The MCM "Last merchant check" line (design r3 section 2.5 item 3): after an
// in-menu batch, what the merchant paid for its confirmed rows and what the
// anywhere formula would have paid for the same units. Kept in memory only.

struct MerchantCheck {
    bool known{};
    std::int64_t merchantGold{};
    std::int64_t anywhereGold{};
};

inline MerchantCheck MerchantCheckFromBatch(const BatchPlan& plan, const ConvertPrice& price) noexcept {
    MerchantCheck check;
    if (!price.known) return check;
    for (const auto& row : plan.rows) {
        if (row.state != BatchRowState::kConfirmed || row.quantity <= 0) continue;
        check.known = true;
        check.merchantGold += row.total;
        check.anywhereGold += static_cast<std::int64_t>(row.quantity) * ConvertUnitPrice(row.baseValue, price);
    }
    return check;
}

}  // namespace js
