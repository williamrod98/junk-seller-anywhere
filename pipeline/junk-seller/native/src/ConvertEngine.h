#pragma once
// v0.1.0 (the v0.1.0 design r3): the engine half
// of sell anywhere. This is the ONE native file allowed to call RemoveItem,
// AddObjectToContainer and UseSkill (the static check narrows the ban here,
// design appendix R2 section 1.3 item 7); AddSkillExperience, AddItem and
// Invoke stay banned everywhere. It is included by Probe.cpp only, and every
// function here runs on the main thread (a UI task or an AddTask task).
//
// CommonLib members used, checked against vendor/commonlibsse-ng (800d61e):
//   TESObjectREFR::GetInventoryChanges(bool)        include/RE/T/TESObjectREFR.h:406
//   TESObjectREFR::GetContainer()                   include/RE/T/TESObjectREFR.h:389
//   InventoryChanges::entryList                     include/RE/I/InventoryChanges.h:69
//   InventoryEntryData object/countDelta/extraLists, GetValue, GetWeight,
//     IsQuestObject, IsEnchanted, IsFavorited, IsWorn, GetOwner
//                                                   include/RE/I/InventoryEntryData.h
//   ExtraDataList begin()/end() (BSExtraData::next), GetCount()
//                                                   include/RE/E/ExtraDataList.h:143-185
//   BSExtraData::GetType()                          include/RE/B/BSExtraData.h:19
//   TESContainer::containerObjects / numContainerObjects, ContainerObject obj/count
//                                                   include/RE/T/TESContainer.h:64-65
//   Actor::RemoveItem(obj, n, kRemove, nullptr, nullptr)   include/RE/A/Actor.h:298
//     (v0.5.1: also the one call for a whole stack, RemoveStack)
//   Actor::RemoveItem(obj, n, kRemove, list, nullptr) (w-probe-r60: the
//     units of exactly that ExtraDataList; the vanilla sale removes a stack
//     list by list, one container event per list, r57 L1b receipt)
//                                                   include/RE/A/Actor.h:298
//   ExtraDataType kCount / kOwnership / kUniqueID / kLeveledItem / kCannotWear
//     / kWorn / kWornLeft / kHotkey / kEnchantment / kAliasInstanceArray /
//     kFromAlias / kHealth / kCharge / kPoison / kSoul / kTextDisplayData /
//     kReferenceHandle / kOriginalReference / kPromotedRef
//                                                   include/RE/E/ExtraDataTypes.h
//   ExtraOwnership::owner                           include/RE/E/ExtraOwnership.h:21
//   ExtraReferenceHandle::containerRef (ObjectRefHandle; operator bool = a
//     nonzero handle, get() = the engine's handle lookup, null when the
//     reference is gone), ExtraDataType kScale (w-probe-r61)
//                                                   include/RE/E/ExtraReferenceHandle.h:30,
//                                                   include/RE/B/BSPointerHandle.h:155-164
//   Actor::AddObjectToContainer(obj, nullptr, n, nullptr)  include/RE/A/Actor.h:302
//   Actor::UseSkill slot 0x0F7 through REL::RelocateVirtual with the fifth
//     integer ABI argument, exactly as Soul Recharge calls it
//     (pipeline/soul-recharge/native/src/adapter/NativeBindings.h:48-56)
//                                                   include/RE/A/Actor.h:443,
//                                                   include/REL/Relocation.h:2074-2129
//   AlchemyItem::IsFood()                           include/RE/A/AlchemyItem.h:71
// v0.2.0 (w-probe-r62; the catalog below, built once at kDataLoaded):
//   TESDataHandler::GetFormArray<T>() / LookupForm<T>(local, plugin) /
//     LookupFormID(local, plugin)                   include/RE/T/TESDataHandler.h:48-72
//   BGSKeyword::GetFormEditorID() (formEditorID kept at runtime)
//                                                   include/RE/B/BGSKeyword.h:29,35
//   BGSKeywordForm keywords / numKeywords           include/RE/B/BGSKeywordForm.h:51-52
//   TESLeveledList::entries (SimpleArray<LEVELED_OBJECT>), LEVELED_OBJECT::form
//                                                   include/RE/T/TESLeveledList.h:19,69
//   BGSOutfit::outfitItems                          include/RE/B/BGSOutfit.h:43
//   BGSConstructibleObject requiredItems / createdItem / benchKeyword
//                                                   include/RE/B/BGSConstructibleObject.h:41-44
//   BGSListForm::forms                              include/RE/B/BGSListForm.h:51
//   BGSBipedObjectForm::IsClothing() (TESObjectARMO base)
//                                                   include/RE/B/BGSBipedObjectForm.h:80
//   TESObjectWEAP::IsStaff(), weaponData.flags Data::Flag::kNonPlayable (1 << 7)
//                                                   include/RE/T/TESObjectWEAP.h:163,183,253,257
//   TESObjectARMO::RecordFlags::kNonPlayable (1 << 2), TESForm::GetFormFlags()
//                                                   include/RE/T/TESObjectARMO.h:47, include/RE/T/TESForm.h:290
//   TESNPC::defaultOutfit                           include/RE/T/TESNPC.h:282
//   TESActorBaseData::IsUnique(), TESContainer (TESActorBase base)
//                                                   include/RE/T/TESActorBaseData.h:108, include/RE/T/TESActorBase.h:22
// v0.3.0 (w-probe-r63; weak enchanted loot, read per row at plan time):
//   TESEnchantableForm::formEnchanting (EITM; a base of TESObjectWEAP at 0x088
//     and of TESObjectARMO at 0x050)            include/RE/T/TESEnchantableForm.h:26,
//                                                   include/RE/T/TESObjectWEAP.h:67, include/RE/T/TESObjectARMO.h:26
//   TESValueForm::value (a base of both)        include/RE/T/TESValueForm.h:20,
//                                                   include/RE/T/TESObjectWEAP.h:68, include/RE/T/TESObjectARMO.h:27
//   TESObjectWEAP::templateWeapon (CNAM)        include/RE/T/TESObjectWEAP.h:269
//   TESObjectARMO::templateArmor (TNAM)         include/RE/T/TESObjectARMO.h:74
//   EnchantmentItem::data.baseEnchantment       include/RE/E/EnchantmentItem.h:45
//   TESForm::RecordFlags::kKnown (1 << 6), TESForm::GetFormFlags()
//                                                   include/RE/T/TESForm.h:70,290
// v0.4.0 (w-probe-r64; keep-N, the catalog below and the per-row facts):
//   AlchemyItem : MagicItem : BGSKeywordForm (the food keywords, an upcast)
//                                                   include/RE/A/AlchemyItem.h:15-16, include/RE/M/MagicItem.h:26-29
//   AlchemyItem::IsFood() (flag 2 and not a poison) include/RE/A/AlchemyItem.h:71
//   BGSConstructibleObject createdItem / benchKeyword / requiredItems (the
//     cooking pot and oven recipes)                 include/RE/B/BGSConstructibleObject.h:41-44
//   BGSKeywordForm::HasKeywordString (the six Misc vendor keywords, as the
//     v0.1.0 junk rule already reads them)          include/RE/B/BGSKeywordForm.h:35
// v0.5.0 (w-probe-r65; potions and poisons, read per row at plan time):
//   AlchemyItem::IsPoison() (flag 1 << 17)          include/RE/A/AlchemyItem.h:72
//   MagicItem::GetCostliestEffectItem(kTotal, false) -> Effect* (RELOCATION_ID
//     11216, 11335; Offset::MagicItem::GetCostliestEffectItem)
//                                                   include/RE/M/MagicItem.h:115, include/RE/Offsets.h:372
//   Effect::baseEffect (EffectSetting*)             include/RE/E/Effect.h:40
//   EffectSetting::data.archetype / data.primaryAV / data.flags
//     (EffectSettingData::Flag::kRecover = 1 << 1)  include/RE/E/EffectSetting.h:49,69,88-89,150
//   EffectSetting::Archetype kValueModifier 0, kCureDisease 3,
//     kDualValueModifier 5, kPeakValueModifier 34   include/RE/E/EffectArchetypes.h:10,13,15,44
//   ActorValue kOneHanded 6 .. kEnchanting 23, kHealth 24, kStamina 26,
//     kOneHandedModifier 96 .. kEnchantingModifier 113, kOneHandedPowerModifier
//     135 .. kEnchantingPowerModifier 152           include/RE/A/ActorValues.h:14,31-34,104,121,143,160
// v0.6.0 (w-probe-r67; mid / high gear and jewelry): no new CommonLib
//   member; the reader types gear by the facts above (ReadConvertGearFacts)
//   and Convert.h ConvertGearType / ConvertMidHighLoot.
#include "Convert.h"
#include "ConvertLists.h"
#include "Protection.h"
#include "StolenRule.h"
#include "receipt/Log.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace js {

// Gold001, the gold row of every measured sale (Inventory.h GoldFormID).
inline constexpr std::uint32_t ConvertGoldFormID = 0x0000000Fu;

// ---------------------------------------------------------------------------
// v0.2.0 (the v0.2 update plan r2 sections 2.1 / 2.2): the facts the new
// categories need, resolved ONCE at kDataLoaded into sorted pointer and
// FormID vectors (least load: a press compares pointers and binary-searches
// FormIDs, it never compares a string or walks a leveled list). Built on the
// main thread before any press can exist, read-only afterwards.
//   * material keywords -> tier (Convert.h ConvertWeaponMaterials / Armor);
//   * home-building: every MISC with a BYOHHouseCrafting* keyword, and the
//     MISC inputs of every recipe at the Carpenter's workbench
//     (BYOHBuildingCarpenter) -> never;
//   * tools: MineOreToolsList, woodChoppingAxes, VendorItemTool, the CC
//     fishing-rod keyword -> never in v0.2.0 (keep 1 from v0.4.0);
//   * never forms: the Lockpick (value 2, weight 0: the value-only junk rule
//     must not reach it) and the HearthFires Wooden Sword (an adoption gift
//     like the doll of Protection.h BuiltinProtectedForms);
//   * guard gear: ConvertLists.h ConvertGuardSources expanded in the live
//     load order, plus what a guard NPC carries in its own inventory (CNTO,
//     e.g. the Winterhold Guard's Shield); a form found there is weak unless
//     its material is mid/high AND some non-guard outfit or leveled list also
//     holds it (generic gear guards share keeps its tier) -- the offline
//     model's GUARD_FORMS rule;
//   * guard owners: ConvertLists.h ConvertGuardNpcs, plus any
//     NPC whose default outfit is a guard outfit;
//   * uniques: a playable, named WEAP / ARMO that no leveled item list, no
//     outfit and no non-unique NPC's own inventory (a generic encounter NPC,
//     e.g. a Falmer's shield) holds and that no creating bench (forge, skyforge, Dawnguard and
//     Aetherium forges, tanning rack, staff enchanter, the CC workbench)
//     makes -> never. It follows mods: a world-placed mod unique is kept, mod
//     gear on leveled lists is generic. Fail closed: an unknown form is kept.
//   * v0.3.0: the enchantment ladder rungs (ConvertLists.h
//     ConvertEnchantLevels resolved to FormIDs; EditorIDs of enchantments are
//     not kept at runtime) and the Daedric artifact keywords (never loot).
//   * v0.4.0: the food facts of the keep-limit categories: the created
//     objects and the inputs of every recipe at a cooking pot or oven
//     (CraftingCookpot, BYOHCraftingOven), the VendorItemFoodRaw keyword, the
//     drinks (ConvertLists.h ConvertDrinkForms, plus any food with a keyword
//     whose EditorID names a drink or alcohol: a mod's own drink keyword) and
//     the quest-food floors (ConvertLists.h ConvertQuestFoods).
struct ConvertCatalog {
    bool built{};
    std::vector<std::pair<const RE::BGSKeyword*, ConvertTier>> weaponMaterials{}, armorMaterials{};
    std::vector<const RE::BGSKeyword*> byohKeywords{}, toolKeywords{}, staffKeywords{}, jewelryKeywords{};
    std::vector<RE::FormID> toolForms{}, neverForms{}, carpenterInputs{}, guardGear{}, guardNpcs{}, uniques{};
    std::vector<const RE::BGSOutfit*> guardOutfits{};
    std::uint32_t guardSourcesResolved{}, guardSourcesMissing{}, guardNpcsResolved{};
    // v0.3.0: sorted by FormID; rung 1..6.
    std::vector<std::pair<RE::FormID, std::uint8_t>> enchantLevels{};
    std::vector<const RE::BGSKeyword*> artifactKeywords{};
    // v0.4.0: sorted; quest floors by FormID.
    std::vector<RE::FormID> cookedForms{}, cookInputs{}, drinkForms{};
    std::vector<const RE::BGSKeyword*> rawKeywords{}, drinkKeywords{};
    std::vector<std::pair<RE::FormID, std::int32_t>> questFoods{};
};

// v0.5.0: the Convert.h potion class constants are the CommonLib enum
// values.
static_assert(static_cast<std::int32_t>(RE::EffectArchetypes::ArchetypeID::kValueModifier) == ConvertArchetypeValueModifier &&
              static_cast<std::int32_t>(RE::EffectArchetypes::ArchetypeID::kCureDisease) == ConvertArchetypeCureDisease &&
              static_cast<std::int32_t>(RE::EffectArchetypes::ArchetypeID::kDualValueModifier) ==
                  ConvertArchetypeDualValueModifier &&
              static_cast<std::int32_t>(RE::EffectArchetypes::ArchetypeID::kPeakValueModifier) ==
                  ConvertArchetypePeakValueModifier);
static_assert(static_cast<std::int32_t>(RE::ActorValue::kHealth) == ConvertAvHealth &&
              static_cast<std::int32_t>(RE::ActorValue::kStamina) == ConvertAvStamina &&
              static_cast<std::int32_t>(RE::ActorValue::kOneHanded) == ConvertAvSkillFirst &&
              static_cast<std::int32_t>(RE::ActorValue::kEnchanting) == ConvertAvSkillLast &&
              static_cast<std::int32_t>(RE::ActorValue::kOneHandedModifier) == ConvertAvSkillModifierFirst &&
              static_cast<std::int32_t>(RE::ActorValue::kEnchantingModifier) == ConvertAvSkillModifierLast &&
              static_cast<std::int32_t>(RE::ActorValue::kOneHandedPowerModifier) == ConvertAvSkillPowerModifierFirst &&
              static_cast<std::int32_t>(RE::ActorValue::kEnchantingPowerModifier) == ConvertAvSkillPowerModifierLast);

// v0.4.0: ConvertKeepMiscType reads the index of Protection.h
// MiscNonJunkKeywords; its first three entries must stay gem, ore, hide.
static_assert(std::string_view(MiscNonJunkKeywords[0]) == "VendorItemGem" &&
              std::string_view(MiscNonJunkKeywords[1]) == "VendorItemOreIngot" &&
              std::string_view(MiscNonJunkKeywords[2]) == "VendorItemAnimalHide");

// Built-in never forms of the anywhere path (plugin, local FormID).
struct ConvertNeverForm {
    const char* plugin;
    std::uint32_t local;
    const char* editorId;
};
inline constexpr std::array<ConvertNeverForm, 2> ConvertNeverForms{{
    {"Skyrim.esm", 0x00000A, "Lockpick"},
    {"HearthFires.esm", 0x004D91, "BYOHWoodenSword"},
}};
// The two vanilla tool FormLists (Skyrim.esm).
inline constexpr std::array<std::uint32_t, 2> ConvertToolFormLists{{0x10ACC4, 0x10ACCC}};
// Benches whose recipes CREATE an item (temper benches excluded: a temper
// recipe names the tempered item itself as its created object).
inline constexpr std::array<const char*, 7> ConvertCreatingBenches{{
    "CraftingSmithingForge", "CraftingSmithingSkyforge", "DLC1CraftingDawnguard", "DLC1LD_CraftingForgeAetherium",
    "CraftingTanningRack", "DLC2StaffEnchanter", "ccVSVSSE004_K_CraftingWorkbenchExt"}};
inline constexpr const char* ConvertCarpenterBench = "BYOHBuildingCarpenter";
// v0.4.0: the cooking stations (the r2 model's COOK_BENCHES).
inline constexpr std::array<const char*, 2> ConvertCookingBenches{{"CraftingCookpot", "BYOHCraftingOven"}};

template <class T>
inline bool ConvertSortedHas(const std::vector<T>& sorted, const T& value) noexcept {
    return std::binary_search(sorted.begin(), sorted.end(), value);
}

inline ConvertTier ConvertKeywordTier(const std::vector<std::pair<const RE::BGSKeyword*, ConvertTier>>& table,
                                      const RE::BGSKeyword* keyword) noexcept {
    const auto hit = std::lower_bound(table.begin(), table.end(), keyword,
                                      [](const auto& entry, const RE::BGSKeyword* k) { return entry.first < k; });
    return hit != table.end() && hit->first == keyword ? hit->second : ConvertTier::kUnknown;
}

// Any keyword of `form` in the sorted pointer set.
inline bool ConvertHasAnyKeyword(const RE::BGSKeywordForm* form, const std::vector<const RE::BGSKeyword*>& sorted) noexcept {
    if (!form || !form->keywords || sorted.empty()) return false;
    for (std::uint32_t i = 0; i < form->numKeywords; ++i) {
        if (ConvertSortedHas(sorted, static_cast<const RE::BGSKeyword*>(form->keywords[i]))) return true;
    }
    return false;
}

// The strongest material tier among the keywords of a weapon or armor form.
inline ConvertTier ConvertFormTier(const RE::BGSKeywordForm* form, bool weapon, const ConvertCatalog& catalog) noexcept {
    ConvertTier tier = ConvertTier::kUnknown;
    if (!form || !form->keywords) return tier;
    const auto& table = weapon ? catalog.weaponMaterials : catalog.armorMaterials;
    for (std::uint32_t i = 0; i < form->numKeywords; ++i) {
        tier = StrongerConvertTier(tier, ConvertKeywordTier(table, form->keywords[i]));
    }
    return tier;
}

// Every WEAP / ARMO a leveled item list or an outfit reaches, recursively
// (visited lists are skipped, so a cycle ends).
inline void ConvertExpandGear(const RE::TESForm* form, std::vector<const RE::TESForm*>& visited,
                              std::vector<RE::FormID>& out) {
    if (!form) return;
    if (const auto* list = form->As<RE::TESLevItem>()) {
        if (std::find(visited.begin(), visited.end(), form) != visited.end()) return;
        visited.push_back(form);
        for (std::uint32_t i = 0; i < list->entries.size(); ++i) ConvertExpandGear(list->entries[i].form, visited, out);
        return;
    }
    if (const auto* outfit = form->As<RE::BGSOutfit>()) {
        if (std::find(visited.begin(), visited.end(), form) != visited.end()) return;
        visited.push_back(form);
        for (const auto* item : outfit->outfitItems) ConvertExpandGear(item, visited, out);
        return;
    }
    const auto type = form->GetFormType();
    if (type == RE::FormType::Weapon || type == RE::FormType::Armor) out.push_back(form->GetFormID());
}

inline bool ConvertGearPlayableNamed(const RE::TESForm* form) noexcept {
    if (!form) return false;
    const char* name = form->GetName();
    if (!name || !name[0]) return false;
    if (const auto* weapon = form->As<RE::TESObjectWEAP>()) {
        return !weapon->weaponData.flags.any(RE::TESObjectWEAP::Data::Flag::kNonPlayable);
    }
    return (form->GetFormFlags() & RE::TESObjectARMO::RecordFlags::kNonPlayable) == 0;
}

template <class T>
inline void ConvertSortUnique(std::vector<T>& values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

// Builds the catalog once (Probe.cpp OnMessage kDataLoaded, main thread).
// False: the data handler is missing; the anywhere path then refuses every
// press (world gate "runtime") instead of guessing a category.
inline bool BuildConvertCatalog(ConvertCatalog& catalog) {
    catalog = ConvertCatalog{};
    auto* data = RE::TESDataHandler::GetSingleton();
    if (!data) return false;
    const RE::BGSKeyword* carpenter = nullptr;
    std::vector<const RE::BGSKeyword*> creating, cooking;
    for (auto* keyword : data->GetFormArray<RE::BGSKeyword>()) {
        if (!keyword) continue;
        const char* edid = keyword->GetFormEditorID();
        if (!edid || !edid[0]) continue;
        const std::string_view name(edid);
        if (const auto tier = ConvertMaterialTier(name, true); tier != ConvertTier::kUnknown) {
            catalog.weaponMaterials.emplace_back(keyword, tier);
        }
        if (const auto tier = ConvertMaterialTier(name, false); tier != ConvertTier::kUnknown) {
            catalog.armorMaterials.emplace_back(keyword, tier);
        }
        if (name.substr(0, 17) == "BYOHHouseCrafting") catalog.byohKeywords.push_back(keyword);
        if (name == "VendorItemTool" || name == "ccBGSSSE001_FishingPoleKW") catalog.toolKeywords.push_back(keyword);
        if (name == "WeapTypeStaff") catalog.staffKeywords.push_back(keyword);
        if (name == "ArmorJewelry" || name == "VendorItemJewelry") catalog.jewelryKeywords.push_back(keyword);
        if (name == "VendorItemDaedricArtifact" || name == "DaedricArtifact") catalog.artifactKeywords.push_back(keyword);
        if (name == ConvertCarpenterBench) carpenter = keyword;
        for (const char* bench : ConvertCreatingBenches) {
            if (name == bench) creating.push_back(keyword);
        }
        // v0.4.0: the food keywords and the cooking stations.
        if (name == "VendorItemFoodRaw") catalog.rawKeywords.push_back(keyword);
        if (name.find("Drink") != std::string_view::npos || name.find("Alcohol") != std::string_view::npos) {
            catalog.drinkKeywords.push_back(keyword);
        }
        for (const char* bench : ConvertCookingBenches) {
            if (name == bench) cooking.push_back(keyword);
        }
    }
    const auto byPointer = [](const auto& a, const auto& b) { return a.first < b.first; };
    std::sort(catalog.weaponMaterials.begin(), catalog.weaponMaterials.end(), byPointer);
    std::sort(catalog.armorMaterials.begin(), catalog.armorMaterials.end(), byPointer);
    ConvertSortUnique(catalog.byohKeywords);
    ConvertSortUnique(catalog.toolKeywords);
    ConvertSortUnique(catalog.staffKeywords);
    ConvertSortUnique(catalog.jewelryKeywords);
    ConvertSortUnique(catalog.artifactKeywords);
    ConvertSortUnique(catalog.rawKeywords);
    ConvertSortUnique(catalog.drinkKeywords);
    ConvertSortUnique(creating);
    ConvertSortUnique(cooking);
    for (const std::uint32_t local : ConvertToolFormLists) {
        if (auto* list = data->LookupForm<RE::BGSListForm>(local, "Skyrim.esm")) {
            for (const auto* form : list->forms) {
                if (form) catalog.toolForms.push_back(form->GetFormID());
            }
        }
    }
    ConvertSortUnique(catalog.toolForms);
    for (const auto& form : ConvertNeverForms) {
        if (const auto id = data->LookupFormID(form.local, form.plugin)) catalog.neverForms.push_back(id);
    }
    ConvertSortUnique(catalog.neverForms);
    for (const auto& npc : ConvertGuardNpcs) {
        if (const auto id = data->LookupFormID(npc.local, npc.plugin)) {
            catalog.guardNpcs.push_back(id);
            ++catalog.guardNpcsResolved;
        }
    }
    ConvertSortUnique(catalog.guardNpcs);
    // Guard sources, then every other list: the guard gear, what non-guard
    // lists hold directly, and everything any list or outfit reaches.
    std::vector<const RE::TESForm*> sources;
    for (const auto& source : ConvertGuardSources) {
        auto* form = data->LookupForm(source.local, source.plugin);
        if (!form) {
            ++catalog.guardSourcesMissing;
            continue;
        }
        ++catalog.guardSourcesResolved;
        sources.push_back(form);
        if (source.outfit) {
            if (const auto* outfit = form->As<RE::BGSOutfit>()) catalog.guardOutfits.push_back(outfit);
        }
    }
    ConvertSortUnique(sources);
    ConvertSortUnique(catalog.guardOutfits);
    std::vector<RE::FormID> expanded;
    for (const auto* source : sources) {
        std::vector<const RE::TESForm*> visited;
        ConvertExpandGear(source, visited, expanded);
    }
    ConvertSortUnique(expanded);
    std::vector<RE::FormID> nonGuard, reached;
    const auto direct = [&](const RE::TESForm* list, const RE::TESForm* item) {
        if (!item) return;
        reached.push_back(item->GetFormID());
        const auto type = item->GetFormType();
        if ((type == RE::FormType::Weapon || type == RE::FormType::Armor) && !ConvertSortedHas(sources, list)) {
            nonGuard.push_back(item->GetFormID());
        }
    };
    for (auto* list : data->GetFormArray<RE::TESLevItem>()) {
        if (!list) continue;
        for (std::uint32_t i = 0; i < list->entries.size(); ++i) direct(list, list->entries[i].form);
    }
    for (auto* outfit : data->GetFormArray<RE::BGSOutfit>()) {
        if (!outfit) continue;
        for (const auto* item : outfit->outfitItems) direct(outfit, item);
    }
    // Every weapon and armor piece a guard NPC carries directly in its own
    // inventory is guard gear (author 2026-09-29; a leveled list there
    // is not expanded: its generic members keep their material tier), and
    // what any non-unique NPC carries is generic, never a unique (a unique
    // NPC's own items stay candidates).
    for (auto* npc : data->GetFormArray<RE::TESNPC>()) {
        if (!npc || !npc->containerObjects) continue;
        const bool guard = ConvertSortedHas(catalog.guardNpcs, npc->GetFormID());
        if (!guard && npc->IsUnique()) continue;
        for (std::uint32_t i = 0; i < npc->numContainerObjects; ++i) {
            const auto* object = npc->containerObjects[i];
            if (!object || !object->obj) continue;
            reached.push_back(object->obj->GetFormID());
            const auto type = object->obj->GetFormType();
            if (guard && (type == RE::FormType::Weapon || type == RE::FormType::Armor)) {
                expanded.push_back(object->obj->GetFormID());
            }
        }
    }
    ConvertSortUnique(expanded);
    ConvertSortUnique(nonGuard);
    ConvertSortUnique(reached);
    for (const RE::FormID id : expanded) {
        const auto* form = RE::TESForm::LookupByID(id);
        const auto* keywords = form ? form->As<RE::BGSKeywordForm>() : nullptr;
        const bool weapon = form && form->GetFormType() == RE::FormType::Weapon;
        const auto tier = ConvertFormTier(keywords, weapon, catalog);
        if (tier == ConvertTier::kUnknown || tier == ConvertTier::kWeak || !ConvertSortedHas(nonGuard, id)) {
            catalog.guardGear.push_back(id);
        }
    }
    ConvertSortUnique(catalog.guardGear);
    // Recipes: created items at creating benches; MISC inputs at the carpenter's.
    std::vector<RE::FormID> created;
    for (auto* recipe : data->GetFormArray<RE::BGSConstructibleObject>()) {
        if (!recipe || !recipe->benchKeyword) continue;
        if (recipe->createdItem && ConvertSortedHas(creating, static_cast<const RE::BGSKeyword*>(recipe->benchKeyword))) {
            created.push_back(recipe->createdItem->GetFormID());
        }
        // v0.4.0: cooked food = what a cooking recipe creates; raw food
        // = what one uses (the reader applies both to food only).
        if (ConvertSortedHas(cooking, static_cast<const RE::BGSKeyword*>(recipe->benchKeyword))) {
            if (recipe->createdItem) catalog.cookedForms.push_back(recipe->createdItem->GetFormID());
            if (recipe->requiredItems.containerObjects) {
                for (std::uint32_t i = 0; i < recipe->requiredItems.numContainerObjects; ++i) {
                    const auto* input = recipe->requiredItems.containerObjects[i];
                    if (input && input->obj) catalog.cookInputs.push_back(input->obj->GetFormID());
                }
            }
        }
        if (carpenter && recipe->benchKeyword == carpenter && recipe->requiredItems.containerObjects) {
            for (std::uint32_t i = 0; i < recipe->requiredItems.numContainerObjects; ++i) {
                const auto* input = recipe->requiredItems.containerObjects[i];
                if (input && input->obj && input->obj->GetFormType() == RE::FormType::Misc) {
                    catalog.carpenterInputs.push_back(input->obj->GetFormID());
                }
            }
        }
    }
    ConvertSortUnique(created);
    ConvertSortUnique(catalog.carpenterInputs);
    ConvertSortUnique(catalog.cookedForms);
    ConvertSortUnique(catalog.cookInputs);
    const auto unique = [&](const RE::TESForm* form) {
        if (!form || !ConvertGearPlayableNamed(form)) return;
        const auto id = form->GetFormID();
        if (!ConvertSortedHas(reached, id) && !ConvertSortedHas(created, id)) catalog.uniques.push_back(id);
    };
    for (auto* weapon : data->GetFormArray<RE::TESObjectWEAP>()) unique(weapon);
    for (auto* armor : data->GetFormArray<RE::TESObjectARMO>()) unique(armor);
    ConvertSortUnique(catalog.uniques);
    // v0.3.0: the ladder rungs of the masters' enchantments (a missing
    // plugin or record is simply off the ladder: the value bound applies).
    for (const auto& level : ConvertEnchantLevels) {
        if (const auto id = data->LookupFormID(level.local, level.plugin)) catalog.enchantLevels.emplace_back(id, level.level);
    }
    ConvertSortUnique(catalog.enchantLevels);
    // v0.4.0: the drinks and the quest-food floors (a missing plugin or
    // record is simply not in the table; two table rows of one form keep the
    // larger floor).
    for (const auto& drink : ConvertDrinkForms) {
        if (const auto id = data->LookupFormID(drink.local, drink.plugin)) catalog.drinkForms.push_back(id);
    }
    ConvertSortUnique(catalog.drinkForms);
    for (const auto& food : ConvertQuestFoods) {
        if (const auto id = data->LookupFormID(food.local, food.plugin)) catalog.questFoods.emplace_back(id, food.need);
    }
    std::sort(catalog.questFoods.begin(), catalog.questFoods.end());
    for (std::size_t i = 1; i < catalog.questFoods.size();) {
        if (catalog.questFoods[i].first == catalog.questFoods[i - 1].first) {
            catalog.questFoods[i - 1].second = (std::max)(catalog.questFoods[i - 1].second, catalog.questFoods[i].second);
            catalog.questFoods.erase(catalog.questFoods.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    catalog.built = true;
    return true;
}

// v0.3.0: the ladder rung of one enchantment (1..6), 0 when it is off
// the ladder. One binary search over the catalog.
inline std::int32_t ConvertEnchantLevelOf(const ConvertCatalog& catalog, RE::FormID id) noexcept {
    const auto hit = std::lower_bound(catalog.enchantLevels.begin(), catalog.enchantLevels.end(), id,
                                      [](const auto& entry, RE::FormID k) { return entry.first < k; });
    return hit != catalog.enchantLevels.end() && hit->first == id ? hit->second : 0;
}

// v0.3.0: the enchanting table's known flag on an enchantment or on its
// base enchantment (the one disenchanting teaches).
inline bool ConvertEnchantKnown(const RE::EnchantmentItem* enchantment) noexcept {
    if (!enchantment) return false;
    if ((enchantment->GetFormFlags() & RE::TESForm::RecordFlags::kKnown) != 0) return true;
    const auto* base = enchantment->data.baseEnchantment;
    return base && (base->GetFormFlags() & RE::TESForm::RecordFlags::kKnown) != 0;
}

// v0.4.0: the quest-food floor of a form (0 = none). One binary search.
inline std::int32_t ConvertQuestFloorOf(const ConvertCatalog& catalog, RE::FormID id) noexcept {
    const auto hit = std::lower_bound(catalog.questFoods.begin(), catalog.questFoods.end(), id,
                                      [](const auto& entry, RE::FormID k) { return entry.first < k; });
    return hit != catalog.questFoods.end() && hit->first == id ? hit->second : 0;
}

// v0.4.0: the food facts of one food record (binary searches and one
// keyword pass; pointer compares only).
inline ConvertFoodFacts ReadConvertFoodFacts(const RE::AlchemyItem* food, const ConvertCatalog& catalog) noexcept {
    ConvertFoodFacts facts;
    if (!food) return facts;
    const auto id = food->GetFormID();
    const auto* keywords = static_cast<const RE::BGSKeywordForm*>(food);
    facts.drink = ConvertSortedHas(catalog.drinkForms, id) || ConvertHasAnyKeyword(keywords, catalog.drinkKeywords);
    facts.cooked = ConvertSortedHas(catalog.cookedForms, id);
    facts.raw = ConvertSortedHas(catalog.cookInputs, id) || ConvertHasAnyKeyword(keywords, catalog.rawKeywords);
    facts.questFloor = ConvertQuestFloorOf(catalog, id);
    return facts;
}

// v0.5.0: the potion facts of one non-food Alchemy item (a plugin
// potion or poison, or a player-made one: a created AlchemyItem carries its
// own effect list). The class follows the costliest effect, the one the game
// names the potion after; no effect or no base effect reads "other".
inline ConvertPotionFacts ReadConvertPotionFacts(const RE::AlchemyItem* alchemy) noexcept {
    ConvertPotionFacts facts;
    if (!alchemy) return facts;
    facts.poison = alchemy->IsPoison();
    const auto* effect = alchemy->GetCostliestEffectItem();
    const auto* base = effect ? effect->baseEffect : nullptr;
    if (!base) return facts;
    facts.effect = true;
    facts.archetype = static_cast<std::int32_t>(base->data.archetype);
    facts.actorValue = static_cast<std::int32_t>(base->data.primaryAV);
    facts.recover = base->data.flags.all(RE::EffectSetting::EffectSettingData::Flag::kRecover);
    return facts;
}

// Audit A-02: true when a list's owner is a guard NPC base.
inline bool ConvertGuardOwner(const RE::TESForm* owner, const ConvertCatalog* catalog) noexcept {
    if (!owner || !catalog) return false;
    if (ConvertSortedHas(catalog->guardNpcs, owner->GetFormID())) return true;
    const auto* npc = owner->As<RE::TESNPC>();
    return npc && npc->defaultOutfit &&
           ConvertSortedHas(catalog->guardOutfits, static_cast<const RE::BGSOutfit*>(npc->defaultOutfit));
}

// The category facts of one weapon or armor base (pointer compares only).
// v0.3.0: plus the weak enchanted loot facts, O(1) per row (pointer and
// flag reads, one binary search for the ladder rung); the known flag is read
// at the press, so an enchantment learned since the last press counts.
inline ConvertGearFacts ReadConvertGearFacts(RE::TESBoundObject* object, const ConvertCatalog& catalog) noexcept {
    ConvertGearFacts facts;
    if (!object) return facts;
    const auto* keywords = object->As<RE::BGSKeywordForm>();
    facts.weapon = object->GetFormType() == RE::FormType::Weapon;
    const RE::EnchantmentItem* enchantment = nullptr;
    if (facts.weapon) {
        const auto* weapon = object->As<RE::TESObjectWEAP>();
        facts.tool = ConvertSortedHas(catalog.toolForms, object->GetFormID()) ||
                     ConvertHasAnyKeyword(keywords, catalog.toolKeywords);
        facts.staff = (weapon && weapon->IsStaff()) || ConvertHasAnyKeyword(keywords, catalog.staffKeywords);
        if (weapon) {
            enchantment = static_cast<const RE::TESEnchantableForm*>(weapon)->formEnchanting;
            facts.templated = weapon->templateWeapon != nullptr;
            facts.baseValue = static_cast<const RE::TESValueForm*>(weapon)->value;
        }
    } else if (const auto* armor = object->As<RE::TESObjectARMO>()) {
        facts.clothing = armor->IsClothing();
        facts.jewelry = ConvertHasAnyKeyword(keywords, catalog.jewelryKeywords);
        enchantment = static_cast<const RE::TESEnchantableForm*>(armor)->formEnchanting;
        facts.templated = armor->templateArmor != nullptr;
        facts.baseValue = static_cast<const RE::TESValueForm*>(armor)->value;
    }
    if (enchantment) {
        facts.baseEnchanted = true;
        facts.enchantLevel = ConvertEnchantLevelOf(catalog, enchantment->GetFormID());
        facts.enchantKnown = ConvertEnchantKnown(enchantment);
        facts.artifact = ConvertHasAnyKeyword(keywords, catalog.artifactKeywords);
    }
    facts.guardForm = ConvertSortedHas(catalog.guardGear, object->GetFormID());
    facts.tier = ConvertFormTier(keywords, facts.weapon, catalog);
    return facts;
}

// w-probe-r60 (design addendum IU): the Convert.h name of one engine extra.
inline ConvertExtra ConvertExtraOf(RE::ExtraDataType type) noexcept {
    switch (type) {
    case RE::ExtraDataType::kCount: return ConvertExtra::kCount;
    case RE::ExtraDataType::kOwnership: return ConvertExtra::kOwnership;
    case RE::ExtraDataType::kUniqueID: return ConvertExtra::kUniqueId;
    case RE::ExtraDataType::kLeveledItem: return ConvertExtra::kLeveledItem;
    case RE::ExtraDataType::kCannotWear: return ConvertExtra::kCannotWear;
    case RE::ExtraDataType::kScale: return ConvertExtra::kScale;
    case RE::ExtraDataType::kWorn:
    case RE::ExtraDataType::kWornLeft: return ConvertExtra::kWorn;
    case RE::ExtraDataType::kHotkey: return ConvertExtra::kHotkey;
    case RE::ExtraDataType::kEnchantment: return ConvertExtra::kEnchantment;
    case RE::ExtraDataType::kAliasInstanceArray:
    case RE::ExtraDataType::kFromAlias: return ConvertExtra::kAlias;
    case RE::ExtraDataType::kHealth:
    case RE::ExtraDataType::kCharge:
    case RE::ExtraDataType::kPoison:
    case RE::ExtraDataType::kSoul: return ConvertExtra::kValue;
    case RE::ExtraDataType::kTextDisplayData: return ConvertExtra::kRenamed;
    case RE::ExtraDataType::kReferenceHandle:
    case RE::ExtraDataType::kOriginalReference:
    case RE::ExtraDataType::kPromotedRef: return ConvertExtra::kReference;
    default: return ConvertExtra::kOther;
    }
}

// w-probe-r61 (addendum IU.10): true when an ExtraReferenceHandle still names
// a reference. A zero handle, or one the engine's lookup no longer resolves
// (the picked-up world object is gone: the seed save stores its RefID as
// null), links the unit to nothing and is inert; a handle that resolves keeps
// the list (fail closed), whatever state the reference is in.
inline bool ConvertReferenceLive(const RE::ExtraReferenceHandle& extra) noexcept {
    if (!extra.containerRef) return false;
    return static_cast<bool>(extra.containerRef.get());
}

// The verdict on one list (Convert.h AddConvertExtra): plain, a convertible
// instance list, or a kept list. An ownership naming anyone but the player
// (stolen, a faction, another actor) keeps the list; no owner or the player
// is inert. A reference handle naming no reference is inert (w-probe-r61);
// one that resolves keeps the list. A null list reads plain.
// Audit A-02 (a review): `leveledKept` = the player's base container
// also holds this form. CommonLib's inventory count leaves the base count out
// for an entry with ExtraLeveledItem (TESObjectREFR.cpp GetInventory,
// InventoryEntryData::IsLeveled), while our counts add it; rather than guess
// which the engine means, such a leveled list is kept ("other", its raw type),
// so the two definitions never differ for a planned row.
// v0.2.0: with `catalog` set, a foreign owner that is a guard NPC marks
// the list; the verdict itself does not change.
inline ConvertListVerdict ClassifyConvertList(const RE::ExtraDataList* list, std::uint32_t playerId,
                                              bool leveledKept, const ConvertCatalog* catalog = nullptr) noexcept {
    ConvertListVerdict verdict;
    if (!list) return verdict;
    for (const auto& extra : *list) {
        const auto type = extra.GetType();
        auto kind = ConvertExtraOf(type);
        if (leveledKept && kind == ConvertExtra::kLeveledItem) kind = ConvertExtra::kOther;
        if (type == RE::ExtraDataType::kReferenceHandle &&
            !ConvertReferenceLive(static_cast<const RE::ExtraReferenceHandle&>(extra))) {
            kind = ConvertExtra::kDeadReference;
        }
        bool foreign = false;
        if (kind == ConvertExtra::kOwnership) {
            const auto* owner = static_cast<const RE::ExtraOwnership&>(extra).owner;
            foreign = ForeignOwner(owner ? owner->GetFormID() : 0, playerId);
            if (foreign && ConvertGuardOwner(owner, catalog)) verdict.guardOwner = true;
        }
        AddConvertExtra(verdict, kind, foreign, static_cast<std::uint32_t>(type));
    }
    return verdict;
}

// The units of one list (its ExtraCount, 1 without one), never below 1.
inline std::int32_t ConvertListUnits(const RE::ExtraDataList* list) noexcept {
    const auto count = list ? list->GetCount() : 0;
    return count > 0 ? count : 1;
}

// The lists of one entry, split: kept units, convertible instance units and
// lists, and the kept kinds (receipt). Plain units are the entry's total
// minus both; a count-only list holds plain units.
struct ConvertSplit {
    std::int32_t keptUnits{};
    std::int32_t instanceUnits{};
    std::int32_t instanceLists{};
    // v0.2.0: the stolen lists counted in instanceUnits / instanceLists
    // (only with "Include stolen items" ON) and their units whose owner is a
    // guard.
    std::int32_t stolenUnits{};
    std::int32_t stolenLists{};
    std::int32_t guardStolenUnits{};
    std::uint32_t keptKinds{};
    std::array<std::uint32_t, ConvertOtherTypesPerEntry> otherTypes{};
    std::uint32_t otherTypeCount{};
};

// v0.4.0: with `sizes` set, the unit count of every stolen and every
// inert list goes to the entry's keep-N vectors (sorted by the reader).
inline ConvertSplit ConvertSplitEntry(const RE::InventoryEntryData* entry, std::uint32_t playerId,
                                      bool leveledKept, bool sellStolen = false,
                                      const ConvertCatalog* catalog = nullptr, ConvertEntry* sizes = nullptr) {
    ConvertSplit split;
    if (!entry || !entry->extraLists) return split;
    for (auto* list : *entry->extraLists) {
        if (!list) continue;
        const auto verdict = ClassifyConvertList(list, playerId, leveledKept, catalog);
        if (!verdict.instance) continue;
        const auto units = ConvertListUnits(list);
        if (verdict.Convertible()) {
            split.instanceUnits += units;
            ++split.instanceLists;
            if (sizes) sizes->inertListUnits.push_back(units);
            continue;
        }
        if (sellStolen && verdict.StolenOnly()) {
            split.instanceUnits += units;
            ++split.instanceLists;
            split.stolenUnits += units;
            ++split.stolenLists;
            if (verdict.guardOwner) split.guardStolenUnits += units;
            if (sizes) sizes->stolenListUnits.push_back(units);
            continue;
        }
        split.keptUnits += units;
        split.keptKinds |= verdict.kept;
        if ((verdict.kept & kConvertKeptOther) && split.otherTypeCount < ConvertOtherTypesPerEntry) {
            bool seen = false;
            for (std::uint32_t k = 0; k < split.otherTypeCount; ++k) seen = seen || split.otherTypes[k] == verdict.otherType;
            if (!seen) split.otherTypes[split.otherTypeCount++] = verdict.otherType;
        }
    }
    return split;
}

// The count the player's base container (the NPC record) adds for `formId`.
inline std::int32_t ConvertBaseContainerCount(RE::TESObjectREFR* reference, std::uint32_t formId) noexcept {
    if (!reference || !formId) return 0;
    auto* container = reference->GetContainer();
    if (!container || !container->containerObjects) return 0;
    std::int32_t count = 0;
    for (std::uint32_t i = 0; i < container->numContainerObjects; ++i) {
        const auto* object = container->containerObjects[i];
        if (object && object->obj && object->obj->GetFormID() == formId) count += object->count;
    }
    return count;
}

// The Misc junk rule of the in-menu reader (Probe.cpp MiscBaseIsJunk), on the
// same Protection.h keyword table.
// v0.4.0: the index of the first non-junk keyword (-1 = junk, -2 = no
// form), which also names the gem / ore / pelt keep categories.
inline int ConvertMiscKeywordIndex(RE::TESObjectMISC* form) noexcept {
    if (!form) return -2;
    return FirstNonJunkKeywordIndex([form](const char* keyword) { return form->HasKeywordString(keyword); });
}

inline bool ConvertMiscIsJunk(RE::TESObjectMISC* form) noexcept {
    if (!form) return false;
    return MiscTypeForKeywordIndex(ConvertMiscKeywordIndex(form)) == CandidateType::kJunk;
}

// The protection sources of one press (the run's settings snapshot).
struct ConvertProtection {
    const ProtectedNames* names{};
    const ProtectedFormSet* forms{};
};

// One read of the player's inventory for the plan (design r3 section 4.1 R):
// every InventoryChanges entry, classified, with the absolute facts of the
// whole entry and its plain / non-plain split. The expensive facts are read
// only for entries whose type and thresholds could pass.
inline std::vector<ConvertEntry> ReadConvertEntries(RE::PlayerCharacter* player, const ConvertProtection& protection,
                                                    const SelectorThresholds& thresholds,
                                                    const ConvertCatalog& catalog) {
    std::vector<ConvertEntry> entries;
    if (!player || !catalog.built) return entries;
    auto* changes = player->GetInventoryChanges(true);
    if (!changes || !changes->entryList) return entries;
    const std::uint32_t playerId = player->GetFormID();
    for (auto* entry : *changes->entryList) {
        if (!entry || !entry->object) continue;
        auto* object = entry->object;
        const std::uint32_t formId = object->GetFormID();
        if (formId == ConvertGoldFormID) continue;
        ConvertFormKind kind = ConvertFormKind::kOther;
        bool miscJunk = false;
        bool food = false;
        // v0.4.0: the keep-limit type of an ingredient, a Misc with a
        // gem / ore / hide keyword and a food (kUnknown = not one of them).
        CandidateType keepType = CandidateType::kUnknown;
        std::int32_t questFloor = 0;
        // v0.5.0: the effect pool of a restore or skill potion.
        std::uint32_t potionPool = 0;
        // v0.2.0: the built-in never-sell facts (pointer / FormID only).
        bool builtinNever = ConvertSortedHas(catalog.neverForms, formId);
        bool gear = false;
        ConvertGearFacts gearFacts{};
        switch (object->GetFormType()) {
        case RE::FormType::Ingredient:
            kind = ConvertFormKind::kIngredient;
            keepType = CandidateType::kKeepIngredient;
            break;
        case RE::FormType::Misc: {
            kind = ConvertFormKind::kMisc;
            auto* misc = object->As<RE::TESObjectMISC>();
            const int keywordIndex = ConvertMiscKeywordIndex(misc);
            miscJunk = misc && MiscTypeForKeywordIndex(keywordIndex) == CandidateType::kJunk;
            if (misc && keywordIndex >= 0) {
                const auto misc040 = ConvertKeepMiscType(keywordIndex);
                if (ConvertKeepType(misc040)) keepType = misc040;
            }
            builtinNever = builtinNever || ConvertSortedHas(catalog.carpenterInputs, formId) ||
                           ConvertHasAnyKeyword(misc, catalog.byohKeywords);
            break;
        }
        case RE::FormType::AlchemyItem: {
            kind = ConvertFormKind::kAlchemy;
            const auto* alchemy = object->As<RE::AlchemyItem>();
            food = alchemy && alchemy->IsFood();
            if (food) {
                const auto foodFacts = ReadConvertFoodFacts(alchemy, catalog);
                keepType = ConvertKeepFoodType(foodFacts);
                questFloor = foodFacts.questFloor;
            } else if (alchemy) {
                // v0.5.0: potions and poisons by their costliest effect.
                const auto potionFacts = ReadConvertPotionFacts(alchemy);
                keepType = ConvertPotionType(potionFacts);
                potionPool = ConvertPotionPool(potionFacts);
            }
            break;
        }
        case RE::FormType::Weapon:
        case RE::FormType::Armor:
            kind = object->GetFormType() == RE::FormType::Weapon ? ConvertFormKind::kWeapon : ConvertFormKind::kArmor;
            gear = true;
            gearFacts = ReadConvertGearFacts(object, catalog);
            builtinNever = builtinNever || ConvertSortedHas(catalog.uniques, formId);
            break;
        default: kind = ConvertFormKind::kOther; break;
        }
        ConvertEntry out;
        auto& candidate = out.candidate;
        candidate.baseFormKey = formId;
        candidate.type = gear ? ConvertGearType(gearFacts, false, thresholds.enchantedLootMaxValue) :
                         keepType != CandidateType::kUnknown ? keepType : ConvertCandidateType(kind, miscJunk, food);
        out.questFloor = questFloor;
        out.potionPool = potionPool;
        const std::int32_t base = ConvertBaseContainerCount(player, formId);
        const std::int32_t total = entry->countDelta + base;
        if (total <= 0) continue;
        candidate.count = total;
        // v0.6.0: a mid / high enchanted loot record (its type is its
        // tier's, kMidGear / kHighGear).
        out.midHighLoot = gear && ConvertMidHighLoot(gearFacts, false, thresholds.enchantedLootMaxValue);
        // Audit A-02: mid/high/unknown gear whose every unit sits in a stolen
        // list owned by a guard counts as weak (only with the stolen switch,
        // since otherwise those lists are kept anyway).
        // v0.6.0: mid / high gear is kMidGear / kHighGear now (kGear
        // until v0.5.0); jewelry (kJewelry) and staves (kGear) never promote.
        ConvertSplit split{};
        bool splitRead = false;
        if (gear && (candidate.type == CandidateType::kMidGear || candidate.type == CandidateType::kHighGear) &&
            thresholds.sellStolen && !gearFacts.tool && !gearFacts.staff && !gearFacts.clothing) {
            split = ConvertSplitEntry(entry, playerId, base != 0, true, &catalog, &out);
            splitRead = true;
            if (split.guardStolenUnits > 0 && split.guardStolenUnits == total) {
                candidate.type = ConvertGearType(gearFacts, true, thresholds.enchantedLootMaxValue);
                out.midHighLoot = ConvertMidHighLoot(gearFacts, true, thresholds.enchantedLootMaxValue);
            }
        }
        if (!TypeAllowed(candidate.type) || CategoryOff(candidate.type, thresholds)) {
            // The selector decides the term; no further read is needed.
            entries.push_back(std::move(out));
            continue;
        }
        candidate.basePrice = entry->GetValue();
        candidate.weight = entry->GetWeight();
        if (!ConvertRuleMayPass(candidate, thresholds)) {
            entries.push_back(std::move(out));
            continue;
        }
        candidate.questItem = entry->IsQuestObject();
        // v0.3.0: weak enchanted loot is enchanted by its own record, so
        // the entry-wide fact (which also reads the base EITM) is not used for
        // it; a player enchantment sits in an ExtraEnchantment list, which the
        // split below keeps (and marks the row enchanted, fail closed).
        // v0.6.0: the same for mid / high enchanted loot.
        const bool loot = candidate.type == CandidateType::kEnchantedLoot || out.midHighLoot;
        candidate.enchanted = loot ? false : entry->IsEnchanted();
        candidate.favorite = entry->IsFavorited();
        candidate.equipped = entry->IsWorn();
        auto* owner = entry->GetOwner();
        // v0.2.0: with "Include stolen items" ON the entry-level owner
        // term is not used: each list's own owner decides (a stolen list is
        // convertible, a stolen list with any other kept extra stays kept).
        // v0.4.0 (the v0.2 update plan r2 section 2.1 row "Stolen", OFF =
        // kept and counted toward N): a keep-N category row does not use it
        // with the switch OFF either; its stolen lists are kept lists (owner),
        // counted toward the keep, and only its other units are sold.
        candidate.ownerUnknown = !thresholds.sellStolen && !ConvertPartialType(candidate.type) &&
                                 ForeignOwnerRefused(owner ? owner->GetFormID() : 0, playerId,
                                                     StolenAcceptance::kRefused);
        // Plain units carry no custom name, so the base form's full name is
        // the displayed name; a name the buffer cannot hold is protected
        // (fail closed).
        std::array<char, TextBytes> name{};
        const bool nameFits = CopyProtectedDisplayName(name, object->GetName());
        candidate.nameProtected = !nameFits || (protection.names && protection.names->Matches(name.data()));
        candidate.formListProtected = (protection.forms && protection.forms->Contains(formId)) || builtinNever;
        out.builtinNever = builtinNever;
        // v0.4.0: a keep-N row needs the size of every stolen and inert
        // list (smallest first); the guard-origin read above already filled
        // them for a row it promoted to weak gear.
        const bool keepN = ConvertPartialType(candidate.type);
        if (!splitRead) {
            split = ConvertSplitEntry(entry, playerId, base != 0, thresholds.sellStolen, &catalog, keepN ? &out : nullptr);
        }
        if (keepN) {
            std::sort(out.stolenListUnits.begin(), out.stolenListUnits.end());
            std::sort(out.inertListUnits.begin(), out.inertListUnits.end());
        }
        if (loot) {
            if (split.keptKinds & kConvertKeptEnchanted) candidate.enchanted = true;
            out.enchantUnknown = !gearFacts.enchantKnown;
        }
        out.nonPlainUnits = split.keptUnits;
        out.instanceUnits = split.instanceUnits;
        out.instanceLists = split.instanceLists;
        out.stolenUnits = split.stolenUnits;
        out.stolenLists = split.stolenLists;
        out.keptKinds = split.keptKinds;
        out.otherTypes = split.otherTypes;
        out.otherTypeCount = split.otherTypeCount;
        const std::int32_t plain = total - split.keptUnits - split.instanceUnits;
        out.plainUnits = plain > 0 ? plain : 0;
        entries.push_back(std::move(out));
    }
    return entries;
}

// The ConvertHost of one slice on the player. Pointers are looked up inside
// the task and never kept across a task boundary.
class EngineConvertHost final : public ConvertHost {
public:
    // v0.2.0: `sellStolen` is the run's "Include stolen items" switch;
    // every count read and every list removal classifies with it, exactly as
    // the plan read did.
    explicit EngineConvertHost(RE::PlayerCharacter* player, bool sellStolen = false) noexcept
        : player_(player), sellStolen_(sellStolen) {}

    std::uint64_t NowUs() override { return js::NowUs(); }

    // Never throws: an allocation failure is a failed read (false), which the
    // slice treats as unverified and rolls back.
    bool ReadCounts(std::span<const std::uint32_t> keys, std::span<ConvertCount> out,
                    std::int64_t& gold) override {
        try {
            return ReadCountsImpl(keys, out, gold);
        } catch (...) {
            return false;
        }
    }

    bool Remove(std::uint32_t formKey, std::int32_t units) override {
        auto* object = RE::TESForm::LookupByID<RE::TESBoundObject>(formKey);
        if (!player_ || !object || units <= 0) return false;
        ++removeCalls;
        (void)player_->RemoveItem(object, units, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
        return true;
    }

    // v0.5.1: ONE RemoveItem for the whole stack of a ConvertStackRow
    // (no extra list: the engine picks the units; every unit of the entry is
    // asked for, so no unit is left for it to choose wrongly). The caller's
    // one-key read proves the entry empty; a unit the engine leaves goes
    // through the v0.5.0 per-list path (Convert.h RemoveConvertStack).
    [[nodiscard]] bool CanRemoveStack() const noexcept override { return true; }
    bool RemoveStack(std::uint32_t formKey, std::int32_t units) override {
        auto* object = RE::TESForm::LookupByID<RE::TESBoundObject>(formKey);
        if (!player_ || !object || units <= 0) return false;
        ++removeCalls;
        (void)player_->RemoveItem(object, units, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
        return true;
    }

    // w-probe-r60 (design addendum IU): one RemoveItem per convertible list.
    // Each step walks the player's entries again, finds this form's entry,
    // takes the first list that classifies as a convertible instance list
    // now, and removes its whole count with that pointer. The pointer is
    // never used again (the engine frees an emptied list), and nothing is
    // carried from one step to the next but the running count, so a list
    // freed or changed by the engine or by another mod's container-event
    // sink is simply not found. A list larger than what is left of the plan
    // stops the row (the state changed since the read). Each step removes at
    // least one unit, so the loop ends after at most `units` steps.
    std::int32_t RemoveInstances(std::uint32_t formKey, std::int32_t units) override {
        auto* object = RE::TESForm::LookupByID<RE::TESBoundObject>(formKey);
        if (!player_ || !object || units <= 0) return 0;
        const std::uint32_t playerId = player_->GetFormID();
        const bool leveledKept = ConvertBaseContainerCount(player_, formKey) != 0;
        std::int32_t asked = 0;
        // Audit A-01: the address and count of the previous step's list, as
        // plain values (never dereferenced). The same list found again with
        // the same count means the engine did not remove it: stop asking.
        std::uintptr_t lastAddress = 0;
        std::int32_t lastUnits = 0;
        for (std::int32_t step = 0; step < units && asked < units; ++step) {
            RE::ExtraDataList* pick = nullptr;
            std::int32_t pickUnits = 0;
            auto* changes = player_->GetInventoryChanges(true);
            if (!changes || !changes->entryList) break;
            for (auto* entry : *changes->entryList) {
                if (!entry || entry->object != object) continue;
                if (entry->extraLists) {
                    for (auto* list : *entry->extraLists) {
                        if (!list || !ClassifyConvertList(list, playerId, leveledKept).ConvertibleWith(sellStolen_)) continue;
                        pick = list;
                        pickUnits = ConvertListUnits(list);
                        break;
                    }
                }
                break;
            }
            if (!pick || pickUnits > units - asked) break;
            if (reinterpret_cast<std::uintptr_t>(pick) == lastAddress && pickUnits == lastUnits) break;
            lastAddress = reinterpret_cast<std::uintptr_t>(pick);
            lastUnits = pickUnits;
            ++removeCalls;
            (void)player_->RemoveItem(object, pickUnits, RE::ITEM_REMOVE_REASON::kRemove, pick, nullptr);
            asked += pickUnits;
        }
        return asked;
    }

    // v0.4.0 (keep-N, the v0.2 update plan r2 section 3): the same one
    // RemoveItem per list as RemoveInstances, but only lists of one kind
    // (kStolen: a stolen list with the run's switch ON; kInert: a list whose
    // every extra is inert) and each time the SMALLEST such list that still
    // fits in what is left -- exactly the lists the plan counted
    // (Convert.h ConvertKeepTake), so a partial row leaves the planned lists
    // behind. The same per-step re-walk, re-classification, pointer-once and
    // same-list stop rules as RemoveInstances; nothing but the running count
    // is carried between steps.
    std::int32_t RemovePickedInstances(std::uint32_t formKey, std::int32_t units, ListPick pick) override {
        auto* object = RE::TESForm::LookupByID<RE::TESBoundObject>(formKey);
        if (!player_ || !object || units <= 0) return 0;
        if (pick == ListPick::kStolen && !sellStolen_) return 0;
        const std::uint32_t playerId = player_->GetFormID();
        const bool leveledKept = ConvertBaseContainerCount(player_, formKey) != 0;
        std::int32_t asked = 0;
        std::uintptr_t lastAddress = 0;
        std::int32_t lastUnits = 0;
        for (std::int32_t step = 0; step < units && asked < units; ++step) {
            RE::ExtraDataList* chosen = nullptr;
            std::int32_t pickUnits = 0;
            auto* changes = player_->GetInventoryChanges(true);
            if (!changes || !changes->entryList) break;
            for (auto* entry : *changes->entryList) {
                if (!entry || entry->object != object) continue;
                if (entry->extraLists) {
                    for (auto* list : *entry->extraLists) {
                        if (!list) continue;
                        const auto verdict = ClassifyConvertList(list, playerId, leveledKept);
                        const bool match = pick == ListPick::kStolen ? verdict.StolenOnly() : verdict.Convertible();
                        if (!match) continue;
                        const auto listUnits = ConvertListUnits(list);
                        if (listUnits > units - asked) continue;
                        if (!chosen || listUnits < pickUnits) {
                            chosen = list;
                            pickUnits = listUnits;
                        }
                    }
                }
                break;
            }
            if (!chosen) break;
            if (reinterpret_cast<std::uintptr_t>(chosen) == lastAddress && pickUnits == lastUnits) break;
            lastAddress = reinterpret_cast<std::uintptr_t>(chosen);
            lastUnits = pickUnits;
            ++removeCalls;
            (void)player_->RemoveItem(object, pickUnits, RE::ITEM_REMOVE_REASON::kRemove, chosen, nullptr);
            asked += pickUnits;
        }
        return asked;
    }

    bool AddUnits(std::uint32_t formKey, std::int32_t units) override {
        auto* object = RE::TESForm::LookupByID<RE::TESBoundObject>(formKey);
        if (!player_ || !object || units <= 0) return false;
        player_->AddObjectToContainer(object, nullptr, units, nullptr);
        return true;
    }

    bool AddGold(std::int32_t amount) override {
        auto* gold = RE::TESForm::LookupByID<RE::TESBoundObject>(ConvertGoldFormID);
        if (!player_ || !gold || amount <= 0) return false;
        player_->AddObjectToContainer(gold, nullptr, amount, nullptr);
        return true;
    }

    void GrantSpeechXp(float points) override {
        if (!player_ || !(points > 0.0F)) return;
        // The pinned Actor::UseSkill declaration omits a fifth integer ABI
        // argument; the player's slot 0xF7 reads it (vanilla passes 0). The
        // virtual dispatch keeps every skill-use hook of other mods in the
        // path (Soul Recharge NativeBindings.h RechargeSkillUse).
        using Fn = void(RE::PlayerCharacter*, RE::ActorValue, float, RE::TESForm*, std::uint32_t);
        REL::RelocateVirtual<Fn>(0xF7, 0xF9, player_, RE::ActorValue::kSpeech, points,
                                 static_cast<RE::TESForm*>(nullptr), std::uint32_t{0});
    }

private:
    // One pass over the changes list plus the base container: the total and
    // non-plain count of each key and the gold count.
    bool ReadCountsImpl(std::span<const std::uint32_t> keys, std::span<ConvertCount> out, std::int64_t& gold) {
        if (!player_ || out.size() != keys.size()) return false;
        auto* changes = player_->GetInventoryChanges(true);
        if (!changes || !changes->entryList) return false;
        const std::uint32_t playerId = player_->GetFormID();
        std::vector<std::pair<std::uint32_t, std::size_t>> index;
        index.reserve(keys.size());
        for (std::size_t i = 0; i < keys.size(); ++i) index.emplace_back(keys[i], i);
        std::sort(index.begin(), index.end());
        // The first slot of `formId` in the sorted index (end() when absent).
        const auto first = [&index](std::uint32_t formId) {
            return std::lower_bound(index.begin(), index.end(), std::make_pair(formId, std::size_t{0}));
        };
        for (auto& count : out) count = ConvertCount{};
        // The base container first: whether a key has a base
        // count decides how its leveled lists are classified below, exactly
        // as in the plan read (ReadConvertEntries).
        std::vector<std::int32_t> base(keys.size(), 0);
        std::int64_t goldTotal = 0;
        if (auto* container = player_->GetContainer(); container && container->containerObjects) {
            for (std::uint32_t i = 0; i < container->numContainerObjects; ++i) {
                const auto* object = container->containerObjects[i];
                if (!object || !object->obj) continue;
                const std::uint32_t formId = object->obj->GetFormID();
                if (formId == ConvertGoldFormID) {
                    goldTotal += object->count;
                    continue;
                }
                for (auto hit = first(formId); hit != index.end() && hit->first == formId; ++hit) {
                    out[hit->second].total += object->count;
                    base[hit->second] += object->count;
                }
            }
        }
        for (auto* entry : *changes->entryList) {
            if (!entry || !entry->object) continue;
            const std::uint32_t formId = entry->object->GetFormID();
            if (formId == ConvertGoldFormID) {
                goldTotal += entry->countDelta;
                continue;
            }
            // Duplicate keys share one entry; every slot of the key is filled.
            auto hit = first(formId);
            if (hit == index.end() || hit->first != formId) continue;
            const auto split = ConvertSplitEntry(entry, playerId, base[hit->second] != 0, sellStolen_);
            for (; hit != index.end() && hit->first == formId; ++hit) {
                out[hit->second].total += entry->countDelta;
                out[hit->second].nonPlain += split.keptUnits;
                out[hit->second].instance += split.instanceUnits;
                out[hit->second].stolen += split.stolenUnits;
            }
        }
        gold = goldTotal;
        return true;
    }

    RE::PlayerCharacter* player_{};
    bool sellStolen_{};
};

// The Speech skill level and XP (PlayerSkills, the Oracle.h ReadSpeech source),
// for the receipt only. False when the skills block is unreadable.
inline bool ReadConvertSpeech(RE::PlayerCharacter* player, std::int32_t& level, float& xp) noexcept {
    if (!player) return false;
    auto* skills = player->GetPlayerRuntimeData().skills;
    if (!skills || !skills->data) return false;
    const auto& entry = skills->data->skills[RE::PlayerCharacter::PlayerSkills::Data::Skills::kSpeech];
    level = static_cast<std::int32_t>(entry.level);
    xp = entry.xp;
    return true;
}

}  // namespace js
