#pragma once
// Engine-free D-W06 candidate selection rules.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace js {

// D2 (design-r2 section 5): one press may plan up to 256 rows (item
// stacks). This is also the Settings.h MaxItems ceiling and the batch row
// watchdog, so the three can never disagree.
inline constexpr std::size_t SelectorMaxTransactions = 256;

enum class CandidateType : unsigned {
    kJunk, kIngredient, kMaterial, kEquipment, kBook, kKey, kSoulGem, kPotion, kUnknown,
    // v0.1.0: the player-facing categories of the sell-anywhere path
    // (a design request): food (Alchemy items with the food flag) and misc
    // (Misc bases carrying a non-junk vendor keyword) and gear (spare weapons
    // and armor). Appended so no existing value moves, and kEquipment keeps
    // its "type" exclusion; only the anywhere reader (Convert.h
    // ConvertCandidateType) produces the three.
    kFood, kMisc, kGear,
    // v0.2.0 (the v0.2 update plan r2 section 2.2): weak
    // and guard weapons, weak and guard armor, and clothing, sold whole on
    // the sell-anywhere path. Appended (no existing value moves); only
    // Convert.h ConvertGearType produces them. Since v0.2.0 kFood, kMisc and
    // kGear have no switch: the v0.1.0 SellFood / SellMisc / SellGear keys
    // are retired (read and ignored), so the three are always category OFF
    // until their keep-limit categories arrive (v0.4.0+).
    kWeakWeapon, kWeakArmor, kClothing,
    // v0.3.0 (the v0.2 update plan r2 section 2.2 row "Weak enchanted
    // loot"): weak base-enchanted loot variants (a templated WEAP / ARMO whose
    // enchantment comes from its record), sold whole on the sell-anywhere
    // path. Appended (no existing value moves); only Convert.h
    // ConvertGearType produces it.
    kEnchantedLoot,
    // v0.4.0 (the v0.2 update plan r2 sections 2.2 and 3, keep-N): the
    // keep-limit categories of the sell-anywhere path: alchemy ingredients
    // (the anywhere twin of kIngredient, which keeps the in-menu switch),
    // cooked food, raw food, drinks and quest food, ore and ingots, gems,
    // pelts and tools. Appended (no existing value moves); only the anywhere
    // reader (Convert.h ConvertKeepFoodType / ConvertKeepMiscType /
    // ConvertGearType) produces them.
    kKeepIngredient, kCookedFood, kRawFood, kDrink, kOre, kGem, kPelt, kTool,
    // v0.5.0 (the v0.2 update plan r2 section 2.2 rows "Potions" and
    // "Poisons", Q4): restore and cure potions (one pool per effect class:
    // Restore Health, Restore Magicka, Restore Stamina, Cure Disease), skill
    // potions (one pool per fortified skill actor value), other potions (keep
    // per kind) and poisons (keep per kind), player-made ones included.
    // Appended (no existing value moves); only the anywhere reader (Convert.h
    // ConvertPotionType) produces them; kPotion keeps its in-menu meaning.
    kRestorePotion, kSkillPotion, kOtherPotion, kPoison,
    // v0.6.0 (the v0.2 update plan r2 section 2.2 rows "Weapons, armor",
    // "Weak enchanted loot" and "Clothing", section 6 row v0.6.0): mid-tier
    // gear (the mid material tier; no known material keyword counts as mid),
    // high-tier gear (the high material tier), each with its own switch and
    // keep per kind, the mid / high enchanted loot variants with them, and
    // jewelry (keep per kind). Appended (no existing value moves); only
    // Convert.h ConvertGearType produces them. kGear keeps staves only.
    kMidGear, kHighGear, kJewelry,
};

enum class SelectorContract : unsigned { kOr, kAnd };
inline constexpr SelectorContract DefaultSelectorContract = SelectorContract::kAnd;

enum class SelectorDecision : unsigned {
    kSelect, kExcludedType, kExcludedRule, kExcludedAbsolute, kExcludedStolen,
    kExcludedUnknown, kInsufficientFunds, kAdapterUnavailable, kRemainder,
};

enum class SelectorTerm : unsigned {
    kNone, kType, kPrice, kWeight, kRatio, kContract, kQuest, kEnchanted, kFavorite,
    kEquipped, kOwnerUnknown, kNameProtected, kFormListProtected, kStolen,
    kMixedProtection, kCandidateInvalid, kBalance, kQuote, kZeroGold, kRunLimit,
    // w-probe-r26 (R27d-3): the adapter-unavailable balance case gets its own
    // term. kBalance stays the insufficient-funds term, so a row that reads
    // "balance" always means a real balance was compared with the quote.
    kBalanceUnknown,
    // (design-r2 section 4.2): an ingredient while SellIngredients is
    // OFF. Appended at the end so no existing term value moves.
    kCategoryOff,
};

struct CandidateRecord {
    std::uint32_t baseFormKey{};
    CandidateType type{CandidateType::kUnknown};
    int basePrice{};
    float weight{};
    int count{};
    bool questItem{};
    bool enchanted{};
    bool favorite{};
    bool equipped{};
    bool stolen{};
    bool ownerUnknown{};
    bool nameProtected{};
    bool formListProtected{};
    bool sameBaseMixedProtection{};
};

struct MerchantRecord {
    bool vencBalanceKnown{};
    std::int64_t vencBalance{};
    bool unitPriceKnown{};
    int unitPrice{};
};

struct CandidateMerchantRecord {
    CandidateRecord candidate{};
    MerchantRecord merchant{};
};

struct CandidateResult {
    SelectorDecision decision{SelectorDecision::kExcludedUnknown};
    SelectorTerm term{SelectorTerm::kCandidateInvalid};
    std::size_t inputIndex{};
    // (design-r2 sections 2.6 / D1): a selected row sells `quantity`
    // units at `unitPrice` each; quotedTotal == quantity * unitPrice. The
    // quantity is the whole stack unless the merchant's running balance cuts
    // it (a partial stack, exactly what the vanilla slider would allow).
    std::int64_t quotedTotal{};
    std::int32_t quantity{};
    std::int32_t unitPrice{};
};

struct SelectionRun {
    std::vector<CandidateResult> results{};
    // a vector, since the 256-row cap makes a fixed array too large to
    // copy around per run.
    std::vector<std::size_t> selectedIndices{};
    std::size_t resultCount{};
    std::size_t selectedCount{};
    // (design-r2 section 2.6 / 2.10): `unitsEligible` counts every unit
    // of a row that matched the rules and has a usable quote (price > 0),
    // before the merchant-gold cap and the row cap; `unitsPlanned` is the sum
    // of the selected quantities. unitsPlanned <= unitsEligible always.
    std::int64_t unitsEligible{};
    std::int64_t unitsPlanned{};
    // A priced row was cut (fully or to a partial stack) by the merchant's
    // running balance.
    bool balanceLimited{};
    // At least one matching row was cut by MaxItems (kRemainder / kRunLimit).
    bool runLimited{};
    // A matching priced row inside the cap had no known merchant balance.
    bool balanceUnknown{};
};

struct ThresholdMatch {
    bool price{};
    bool weight{};
    bool ratio{};
};

// w-probe-r28 (R29-3): the four D-W11 thresholds the rule path now takes from
// the run's settings snapshot instead of the built-in constants. The value
// defaults are the accepted T-W06 constants (10 / 0.5 / 10); moved the
// row cap to 256 (D2) and added the ingredient switch, default OFF (design-r2
// section 4). Only those two named changes move a default decision.
struct SelectorThresholds {
    int maxValue{10};
    float minWeight{0.5F};
    float maxValueWeight{10.0F};
    int maxItems{static_cast<int>(SelectorMaxTransactions)};
    bool sellIngredients{false};
    // v0.2.0 (the v0.2 update plan r2 sections 2.2 and 4.1): the
    // sell-anywhere categories, all OFF by default (public build). The junk
    // rule is a choice: Classic (false) = the v0.1.0 AND rule of
    // the three thresholds above, Value only (true) = value <= junkMaxValue.
    // Weak weapons / weak armor / clothing sell whole rows ("sell all").
    // sellStolen admits a list whose only kept reason is a foreign owner.
    bool junkValueOnly{false};
    int junkMaxValue{50};
    bool sellStolen{false};
    bool sellWeakWeapons{false};
    bool sellWeakArmor{false};
    bool sellClothing{false};
    // v0.3.0: "Weak enchanted loot" (OFF by default), its jewelry and
    // clothing value bound (0..2000, default 300; used where the enchantment
    // is not on the vanilla 01-06 ladder) and "Known enchants only" (ON by
    // default: an item whose enchantment the player has not learned stays).
    bool sellEnchantedLoot{false};
    int enchantedLootMaxValue{300};
    bool knownEnchantmentsOnly{true};
    // v0.4.0 (the v0.2 update plan r2 sections 3, 4.1 and 4.2): the
    // keep-limit categories, each OFF by default, with its keep per kind
    // (0 = sell all; the quest-food floor still applies): ingredients 500,
    // cooked food 10, raw food 20, drinks and quest food 10, ore and ingots
    // 50, gems 50, pelts 100, tools 1.
    bool sellIngredientsAnywhere{false};
    bool sellCookedFood{false};
    bool sellRawFood{false};
    bool sellDrinks{false};
    bool sellOre{false};
    bool sellGems{false};
    bool sellPelts{false};
    bool sellTools{false};
    int keepIngredients{500};
    int keepCookedFood{10};
    int keepRawFood{20};
    int keepDrinks{10};
    int keepOre{50};
    int keepGems{50};
    int keepPelts{100};
    int keepTools{1};
    // v0.5.0 (the v0.2 update plan r2 sections 2.2, 4.1 and 4.2): potions
    // and poisons, each OFF by default. Restore / cure and skill potions keep
    // their number per effect pool (the strongest kinds first, Convert.h
    // ConvertPotionPoolKeeps), other potions and poisons per kind.
    bool sellRestorePotions{false};
    bool sellSkillPotions{false};
    bool sellOtherPotions{false};
    bool sellPoisons{false};
    int keepRestorePotions{100};
    int keepSkillPotions{100};
    int keepOtherPotions{10};
    int keepPoisons{10};
    // v0.6.0 (the v0.2 update plan r2 sections 2.2, 4.1 and 4.2): mid-tier
    // gear, high-tier gear and jewelry, each OFF by default, keep 10 per kind
    // (0 = sell all). The enchanted loot of the mid / high tier follows its
    // tier's switch and keep, and only while sellEnchantedLoot is ON too.
    bool sellMidGear{false};
    bool sellHighGear{false};
    bool sellJewelry{false};
    int keepMidGear{10};
    int keepHighGear{10};
    int keepJewelry{10};
};

inline constexpr SelectorThresholds DefaultSelectorThresholds{};

inline bool TypeAllowed(CandidateType type) noexcept {
    return type == CandidateType::kJunk || type == CandidateType::kIngredient ||
           type == CandidateType::kMaterial || type == CandidateType::kFood ||
           type == CandidateType::kMisc || type == CandidateType::kGear ||
           type == CandidateType::kWeakWeapon || type == CandidateType::kWeakArmor ||
           type == CandidateType::kClothing || type == CandidateType::kEnchantedLoot ||
           type == CandidateType::kKeepIngredient || type == CandidateType::kCookedFood ||
           type == CandidateType::kRawFood || type == CandidateType::kDrink || type == CandidateType::kOre ||
           type == CandidateType::kGem || type == CandidateType::kPelt || type == CandidateType::kTool ||
           type == CandidateType::kRestorePotion || type == CandidateType::kSkillPotion ||
           type == CandidateType::kOtherPotion || type == CandidateType::kPoison ||
           type == CandidateType::kMidGear || type == CandidateType::kHighGear || type == CandidateType::kJewelry;
}

// (design-r2 section 4.2): an allowed type whose category the settings
// snapshot switched off: ingredients unless SellIngredients=1. v0.2.0:
// food, non-junk misc and every other weapon/armor (mid/high tier, unknown
// material, tools, staves, jewelry) have no switch in this version and are
// always off; weak weapons, weak armor and clothing follow their own switch.
// v0.3.0: weak enchanted loot follows its own switch.
// v0.4.0: each keep-limit category follows its own switch.
// v0.5.0: each potion and poison category follows its own switch.
// v0.6.0: mid-tier gear, high-tier gear and jewelry follow their own
// switch; kGear (staves) stays always off.
// The in-menu reader never produces any of these twenty-two types.
inline bool CategoryOff(CandidateType type, const SelectorThresholds& thresholds) noexcept {
    return (type == CandidateType::kIngredient && !thresholds.sellIngredients) ||
           type == CandidateType::kFood || type == CandidateType::kMisc || type == CandidateType::kGear ||
           (type == CandidateType::kWeakWeapon && !thresholds.sellWeakWeapons) ||
           (type == CandidateType::kWeakArmor && !thresholds.sellWeakArmor) ||
           (type == CandidateType::kClothing && !thresholds.sellClothing) ||
           (type == CandidateType::kEnchantedLoot && !thresholds.sellEnchantedLoot) ||
           (type == CandidateType::kKeepIngredient && !thresholds.sellIngredientsAnywhere) ||
           (type == CandidateType::kCookedFood && !thresholds.sellCookedFood) ||
           (type == CandidateType::kRawFood && !thresholds.sellRawFood) ||
           (type == CandidateType::kDrink && !thresholds.sellDrinks) ||
           (type == CandidateType::kOre && !thresholds.sellOre) ||
           (type == CandidateType::kGem && !thresholds.sellGems) ||
           (type == CandidateType::kPelt && !thresholds.sellPelts) ||
           (type == CandidateType::kTool && !thresholds.sellTools) ||
           (type == CandidateType::kRestorePotion && !thresholds.sellRestorePotions) ||
           (type == CandidateType::kSkillPotion && !thresholds.sellSkillPotions) ||
           (type == CandidateType::kOtherPotion && !thresholds.sellOtherPotions) ||
           (type == CandidateType::kPoison && !thresholds.sellPoisons) ||
           (type == CandidateType::kMidGear && !thresholds.sellMidGear) ||
           (type == CandidateType::kHighGear && !thresholds.sellHighGear) ||
           (type == CandidateType::kJewelry && !thresholds.sellJewelry);
}

inline bool ValidCandidate(const CandidateRecord& candidate) noexcept {
    return candidate.baseFormKey != 0 && candidate.count > 0 && candidate.basePrice >= 0 &&
           std::isfinite(candidate.weight) && candidate.weight >= 0.0F;
}

inline ThresholdMatch MatchThresholds(const CandidateRecord& candidate,
                                      const SelectorThresholds& thresholds) noexcept {
    ThresholdMatch match;
    if (!ValidCandidate(candidate)) return match;
    match.price = candidate.basePrice <= thresholds.maxValue;
    match.weight = candidate.weight >= thresholds.minWeight;
    match.ratio = candidate.weight > 0.0F &&
                  static_cast<double>(candidate.basePrice) / candidate.weight <=
                      static_cast<double>(thresholds.maxValueWeight);
    return match;
}

// The default-snapshot spelling the accepted T-W06 cases already call.
inline ThresholdMatch MatchThresholds(const CandidateRecord& candidate) noexcept {
    return MatchThresholds(candidate, DefaultSelectorThresholds);
}

inline CandidateResult DecideCandidate(const CandidateRecord& candidate, SelectorContract contract,
                                       const SelectorThresholds& thresholds) noexcept {
    CandidateResult result;
    if (candidate.type == CandidateType::kUnknown || !ValidCandidate(candidate)) {
        result.decision = SelectorDecision::kExcludedUnknown;
        result.term = candidate.type == CandidateType::kUnknown ? SelectorTerm::kType : SelectorTerm::kCandidateInvalid;
        return result;
    }
    if (!TypeAllowed(candidate.type)) {
        result.decision = SelectorDecision::kExcludedType;
        result.term = SelectorTerm::kType;
        return result;
    }
    if (CategoryOff(candidate.type, thresholds)) {
        result.decision = SelectorDecision::kExcludedType;
        result.term = SelectorTerm::kCategoryOff;
        return result;
    }
    if (candidate.stolen) {
        result.decision = SelectorDecision::kExcludedStolen;
        result.term = SelectorTerm::kStolen;
        return result;
    }
    if (candidate.sameBaseMixedProtection) {
        result.decision = SelectorDecision::kExcludedStolen;
        result.term = SelectorTerm::kMixedProtection;
        return result;
    }
    if (candidate.questItem || candidate.enchanted || candidate.favorite || candidate.equipped ||
        candidate.nameProtected || candidate.formListProtected) {
        result.decision = SelectorDecision::kExcludedAbsolute;
        result.term = candidate.questItem ? SelectorTerm::kQuest : candidate.enchanted ? SelectorTerm::kEnchanted :
                      candidate.favorite ? SelectorTerm::kFavorite : candidate.equipped ? SelectorTerm::kEquipped :
                      candidate.nameProtected ? SelectorTerm::kNameProtected : SelectorTerm::kFormListProtected;
        return result;
    }
    if (candidate.ownerUnknown) {
        result.decision = SelectorDecision::kExcludedUnknown;
        result.term = SelectorTerm::kOwnerUnknown;
        return result;
    }
    const ThresholdMatch matched = MatchThresholds(candidate, thresholds);
    const bool price = matched.price;
    const bool weight = matched.weight;
    const bool ratio = matched.ratio;
    const bool accepted = contract == SelectorContract::kOr ? (price || weight || ratio) : (price && weight && ratio);
    if (!accepted) {
        result.decision = SelectorDecision::kExcludedRule;
        result.term = contract == SelectorContract::kAnd ? SelectorTerm::kContract :
                      !price ? SelectorTerm::kPrice : !weight ? SelectorTerm::kWeight : SelectorTerm::kRatio;
        return result;
    }
    result.decision = SelectorDecision::kSelect;
    result.term = SelectorTerm::kNone;
    return result;
}

// The default-snapshot spelling the accepted T-W06 cases already call.
inline CandidateResult DecideCandidate(const CandidateRecord& candidate,
                                       SelectorContract contract = DefaultSelectorContract) noexcept {
    return DecideCandidate(candidate, contract, DefaultSelectorThresholds);
}

// w-probe-r31 (R31-1, design ruling section 31): `MaxItems` is the transaction
// bound of one run -- the number of rows it may SELECT -- and never a position
// in the displayed list. R-W24 measured the old index test excluding the two
// eligible rows at the tail of a seven-row list with `MaxItems=2` before they
// were ever decided (rows 3..7 `run_limit`, issued 0). Every record is decided
// now, and only a record whose own decision is `kSelect` while the run already
// holds `maxItems` selections becomes `kRemainder` / `kRunLimit`: no balance
// deduction, no `selectedIndices` entry. Every other decision is recorded
// exactly as before, in every position, and `resultCount` stays
// `records.size()`. The balance walk keeps list order over the selected rows.
inline SelectionRun SelectCandidates(std::span<const CandidateMerchantRecord> records,
                                     SelectorContract contract,
                                     const SelectorThresholds& thresholds) {
    SelectionRun run;
    std::int64_t remainingBalance = 0;
    bool balanceInitialized = false;
    // A non-positive cap selects nothing (the same answer the old index test
    // gave for 0); Settings.h refuses such a value, so this is the floor.
    const std::size_t runLimit =
        thresholds.maxItems > 0 ? static_cast<std::size_t>(thresholds.maxItems) : 0;
    run.results.reserve(records.size());
    for (std::size_t index = 0; index < records.size(); ++index) {
        CandidateResult result = DecideCandidate(records[index].candidate, contract, thresholds);
        result.inputIndex = index;
        const auto& merchant = records[index].merchant;
        const std::int32_t stack = records[index].candidate.count;
        // a matching row with a usable quote is eligible, whatever the
        // gold cap or the row cap decide below.
        const bool priced = merchant.unitPriceKnown && merchant.unitPrice > 0;
        if (result.decision == SelectorDecision::kSelect && priced) run.unitsEligible += stack;
        if (result.decision == SelectorDecision::kSelect) {
            if (run.selectedCount >= runLimit) {
                result.decision = SelectorDecision::kRemainder;
                result.term = SelectorTerm::kRunLimit;
                run.runLimited = true;
            } else {
                if (!merchant.vencBalanceKnown || !merchant.unitPriceKnown || merchant.unitPrice <= 0) {
                    result.decision = SelectorDecision::kAdapterUnavailable;
                    // w-probe-r26 (R27d-3): an unknown vendor balance is not a
                    // balance the adapter compared; only kInsufficientFunds
                    // below keeps the kBalance name.
                    result.term = merchant.vencBalanceKnown ? (merchant.unitPriceKnown ? SelectorTerm::kZeroGold : SelectorTerm::kQuote) : SelectorTerm::kBalanceUnknown;
                    if (!merchant.vencBalanceKnown && priced) run.balanceUnknown = true;
                } else {
                    // (design-r2 section 2.6, FR-06): whole stacks, cut by
                    // the running merchant balance in list order:
                    // q = min(stack, floor(remaining / p)); q == 0 is `balance`.
                    if (!balanceInitialized) { remainingBalance = merchant.vencBalance; balanceInitialized = true; }
                    const std::int64_t price = merchant.unitPrice;
                    const std::int64_t affordable = remainingBalance > 0 ? remainingBalance / price : 0;
                    const std::int64_t quantity = affordable < stack ? affordable : stack;
                    if (quantity < stack) run.balanceLimited = true;
                    if (quantity <= 0) {
                        result.decision = SelectorDecision::kInsufficientFunds;
                        result.term = SelectorTerm::kBalance;
                    } else {
                        result.quantity = static_cast<std::int32_t>(quantity);
                        result.unitPrice = merchant.unitPrice;
                        result.quotedTotal = quantity * price;
                        remainingBalance -= result.quotedTotal;
                    }
                }
            }
        }
        run.results.push_back(result);
        ++run.resultCount;
        if (result.decision == SelectorDecision::kSelect) {
            run.selectedIndices.push_back(index);
            ++run.selectedCount;
            run.unitsPlanned += result.quantity;
        }
    }
    return run;
}

// The default-snapshot spelling the accepted T-W06 cases already call.
inline SelectionRun SelectCandidates(std::span<const CandidateMerchantRecord> records,
                                     SelectorContract contract = DefaultSelectorContract) {
    return SelectCandidates(records, contract, DefaultSelectorThresholds);
}

}  // namespace js
