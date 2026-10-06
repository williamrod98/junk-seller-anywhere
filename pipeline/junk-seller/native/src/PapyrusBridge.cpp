#include "PCH.h"
#include "PapyrusBridge.h"
#include "BuildInfo.h"
#include "Settings.h"
#include "Install.h"
#include "receipt/Log.h"
#include <string>
#include <string_view>

// w-probe-r28 (R29-1/R29-2): AGH_JS_Native, the global native surface of
// Source/Scripts/AGH_JS_Native.psc. The shape follows the sibling probes' own
// bridge (pipeline/uncapper/native/src/PapyrusBridge.cpp:298-325): one
// file-local thunk per name, one Register() lambda that names the script string
// once. The StaticFunctionTag base argument is not a PSC-visible parameter
// (RE/T/TypeTraits.h:9), the registration is
// IVirtualMachine::RegisterFunction (RE/I/IVirtualMachine.h:149-150), and the
// interface is SKSE::GetPapyrusInterface (SKSE/API.h:22) ->
// PapyrusInterface::Register (SKSE/Interfaces.h:218-249).
//
// Every thunk below runs on the Papyrus VM thread and touches no RE object: the
// bodies in Probe.cpp read and write js::Settings under the settings mutex and
// nothing else.
namespace {

// True only when all three of ApplySetting accepted, the file was written and
// the readback matched (R29-2).
bool Settled(const js::SettingSaveOutcome& outcome) {
    return outcome.accepted && outcome.saved && outcome.readbackOk;
}

js::SettingSaveOutcome ApplyBool(const char* key, bool value) {
    return js::ApplyAndSaveSetting(key, value ? "1" : "0");
}

js::SettingSaveOutcome ApplyInt(const char* key, int value) {
    const std::string text = std::to_string(value);
    return js::ApplyAndSaveSetting(key, text.c_str());
}

js::SettingSaveOutcome ApplyFloat(const char* key, float value) {
    const std::string text = js::FormatSettingFloat(value);
    return js::ApplyAndSaveSetting(key, text.c_str());
}

// The Papyrus list spelling is `|` joined; the file keeps `;`. Only this
// boundary translates, so a value the probe ini already had keeps loading
// unchanged.
std::string PapyrusListToStored(const std::string& text) {
    std::string stored = text;
    for (char& c : stored) {
        if (c == js::PapyrusListSeparator) c = js::SettingsListSeparator;
    }
    return stored;
}

std::string StoredListToPapyrus(const std::string& text) {
    std::string joined = text;
    for (char& c : joined) {
        if (c == js::SettingsListSeparator) c = js::PapyrusListSeparator;
    }
    return joined;
}

js::SettingSaveOutcome ApplyList(const char* key, const std::string& value) {
    const std::string stored = PapyrusListToStored(value);
    return js::ApplyAndSaveSetting(key, stored.c_str());
}

// (design-r2 4.2): API 2 adds GetSellIngredients/SetSellIngredients.
// v0.1.0 (design r3 section 7): API 3 adds the sell-anywhere switch, the
// three category switches, GetAnywhereRatePercent and GetLastMerchantCheck.
// v0.2.0 (the v0.2 update plan r2 section 4.2): API 4 retires the three
// v0.1.0 category switches (food, misc, spare gear) and adds the Categories
// page (junk rule, junk max value, include stolen items, weak weapons, weak
// armor, clothing) and GetLastKept (the Diagnostics "why kept" counters).
// v0.3.0 (the v0.2 update plan r2 section 4.2): API 5 adds weak enchanted
// loot: Get/SetSellEnchantedLoot, Get/SetEnchantedLootMaxValue and
// Get/SetKnownEnchantmentsOnly (the Categories page rows of v0.3.0).
// v0.4.0 (the v0.2 update plan r2 section 4.2): API 6 adds the eight
// keep-limit categories through four indexed natives (Settings.h
// SettingsKeepCategories order): Get/SetKeepCategory (the switch) and
// Get/SetKeepLimit (the keep per kind, 0 = sell all).
// v0.5.0 (the v0.2 update plan r2 section 4.2): API 7, the same four
// natives over twelve rows (indices 8..11: restore potions, skill potions,
// other potions, poisons). No native is added; the API moves so a v0.5.0 MCM
// (twelve rows) never runs against a v0.4.0 DLL (eight rows), and the
// reverse.
// v0.6.0 (the v0.2 update plan r2 section 4.2): API 8, the same four
// natives over fifteen rows (indices 12..14: mid-tier gear, high-tier gear,
// jewelry). No native is added; the API moves so a v0.6.0 MCM (fifteen rows)
// never runs against a v0.5.x DLL (twelve rows), and the reverse.
// v0.8.0 (the barter-price design r2 sections 3.4 and 4): API 9 adds
// "Price follows perks" (Get/SetPerkPrice, default OFF), the main-thread "Rate
// now" preview (RequestRatePreview, GetRatePreview) and GetLastPriceSource
// (the Diagnostics "Last price source" row). GetAnywhereRatePercent stays
// (the shipped formula, never a perk walk).
// PapyrusGetAPI and PapyrusCheckAPI both compare against this one constant, so
// the two can never drift apart. A caller built against API 1 calls Available()
// with ExpectedApi() 1, sees GetAPI() (2) disagree at the first check and never
// reaches CheckAPI at all (AGH_JS_MCM.psc Available()); only a caller that
// already expects the live API 2 calls CheckAPI, and it must see true.
constexpr std::int32_t kJsNativeApiVersion = 9;

int PapyrusGetAPI(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return 0; return kJsNativeApiVersion; }

std::string PapyrusGetStatus(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return std::string();
    return std::string(js::GetStatusReason());
}

bool PapyrusIsEffectivelyEnabled(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return false; return js::IsEffectivelyEnabled(); }

std::string PapyrusGetRuntime(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return std::string();
    return std::string(js::GetRuntimeName());
}

std::string PapyrusGetReleaseId(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return std::string();
    return std::string(js::BuildReleaseId());
}

// (design-r2 section 8, S2e): the product version (BuildInfo.h), so the
// MCM Diagnostics RuntimeText() names the version without a psc literal; a
// future version bump needs no psc edit, only BuildInfo.h/CMakeLists.txt.
std::string PapyrusGetVersion(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return std::string();
    return std::string(js::BuildInfo::Version);
}

std::int32_t PapyrusGetConfigGeneration(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return 0;
    return static_cast<std::int32_t>(js::GetConfigGeneration());
}

bool PapyrusGetEnabled(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return false;
    return js::ReadSettingBool(js::SettingsKeyEnabled);
}

bool PapyrusSetEnabled(RE::StaticFunctionTag*, bool a_value) { if (!js::latch.Armed()) return false;
    return Settled(ApplyBool(js::SettingsKeyEnabled, a_value));
}

bool PapyrusGetConfirmBeforeSell(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return false;
    return js::ReadSettingBool(js::SettingsKeyConfirmBeforeSell);
}

bool PapyrusSetConfirmBeforeSell(RE::StaticFunctionTag*, bool a_value) { if (!js::latch.Armed()) return false;
    return Settled(ApplyBool(js::SettingsKeyConfirmBeforeSell, a_value));
}

// (design-r2 4.2): the ingredient switch, API 2. Same pattern as
// ConfirmBeforeSell; ApplyBool/ApplyAndSaveSetting already carry the "OFF
// never written" rule of Settings.h SerializeSettings.
bool PapyrusGetSellIngredients(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return false;
    return js::ReadSettingBool(js::SettingsKeySellIngredients);
}

bool PapyrusSetSellIngredients(RE::StaticFunctionTag*, bool a_value) { if (!js::latch.Armed()) return false;
    return Settled(ApplyBool(js::SettingsKeySellIngredients, a_value));
}

// v0.1.0 (design r3 1.1/7): the sell-anywhere switch and the three
// category switches, API 3. Same pattern as SellIngredients: OFF is never
// written to the user file (Settings.h SerializeSettings).
bool PapyrusGetSellAnywhere(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return false;
    return js::ReadSettingBool(js::SettingsKeySellAnywhere);
}

bool PapyrusSetSellAnywhere(RE::StaticFunctionTag*, bool a_value) { if (!js::latch.Armed()) return false;
    return Settled(ApplyBool(js::SettingsKeySellAnywhere, a_value));
}

// v0.2.0 (the v0.2 update plan r2 section 4.2): the Categories page, API
// 4. JunkRule is a choice stored as Classic / ValueOnly; the MCM toggle is
// "Junk: value only". Every key is written only when it differs from the
// shipped default (Settings.h SerializeSettings).
bool PapyrusGetJunkValueOnly(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return false;
    return js::ReadSettingBool(js::SettingsKeyJunkRule);
}

bool PapyrusSetJunkValueOnly(RE::StaticFunctionTag*, bool a_value) { if (!js::latch.Armed()) return false;
    return Settled(js::ApplyAndSaveSetting(js::SettingsKeyJunkRule, a_value ? js::SettingsJunkRuleValueOnly :
                                                                              js::SettingsJunkRuleClassic));
}

std::int32_t PapyrusGetJunkMaxValue(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return 0;
    return js::ReadSettingInt(js::SettingsKeyJunkMaxValue);
}

bool PapyrusSetJunkMaxValue(RE::StaticFunctionTag*, std::int32_t a_value) { if (!js::latch.Armed()) return false;
    return Settled(ApplyInt(js::SettingsKeyJunkMaxValue, a_value));
}

bool PapyrusGetSellStolen(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return false;
    return js::ReadSettingBool(js::SettingsKeySellStolen);
}

bool PapyrusSetSellStolen(RE::StaticFunctionTag*, bool a_value) { if (!js::latch.Armed()) return false;
    return Settled(ApplyBool(js::SettingsKeySellStolen, a_value));
}

bool PapyrusGetSellWeakWeapons(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return false;
    return js::ReadSettingBool(js::SettingsKeySellWeakWeapons);
}

bool PapyrusSetSellWeakWeapons(RE::StaticFunctionTag*, bool a_value) { if (!js::latch.Armed()) return false;
    return Settled(ApplyBool(js::SettingsKeySellWeakWeapons, a_value));
}

bool PapyrusGetSellWeakArmor(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return false;
    return js::ReadSettingBool(js::SettingsKeySellWeakArmor);
}

bool PapyrusSetSellWeakArmor(RE::StaticFunctionTag*, bool a_value) { if (!js::latch.Armed()) return false;
    return Settled(ApplyBool(js::SettingsKeySellWeakArmor, a_value));
}

bool PapyrusGetSellClothing(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return false;
    return js::ReadSettingBool(js::SettingsKeySellClothing);
}

bool PapyrusSetSellClothing(RE::StaticFunctionTag*, bool a_value) { if (!js::latch.Armed()) return false;
    return Settled(ApplyBool(js::SettingsKeySellClothing, a_value));
}

// v0.3.0: weak enchanted loot, API 5. SellEnchantedLoot is written only
// when ON, EnchantedLootMaxValue only when it is not 300, and
// KnownEnchantmentsOnly only when OFF (Settings.h SerializeSettings).
bool PapyrusGetSellEnchantedLoot(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return false;
    return js::ReadSettingBool(js::SettingsKeySellEnchantedLoot);
}

bool PapyrusSetSellEnchantedLoot(RE::StaticFunctionTag*, bool a_value) { if (!js::latch.Armed()) return false;
    return Settled(ApplyBool(js::SettingsKeySellEnchantedLoot, a_value));
}

std::int32_t PapyrusGetEnchantedLootMaxValue(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return 0;
    return js::ReadSettingInt(js::SettingsKeyEnchantedLootMaxValue);
}

bool PapyrusSetEnchantedLootMaxValue(RE::StaticFunctionTag*, std::int32_t a_value) { if (!js::latch.Armed()) return false;
    return Settled(ApplyInt(js::SettingsKeyEnchantedLootMaxValue, a_value));
}

bool PapyrusGetKnownEnchantmentsOnly(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return false;
    return js::ReadSettingBool(js::SettingsKeyKnownEnchantmentsOnly);
}

bool PapyrusSetKnownEnchantmentsOnly(RE::StaticFunctionTag*, bool a_value) { if (!js::latch.Armed()) return false;
    return Settled(ApplyBool(js::SettingsKeyKnownEnchantmentsOnly, a_value));
}

// v0.4.0: the keep-limit categories, API 6 (twelve rows since API 7,
// v0.5.0; fifteen since API 8, v0.6.0). An index outside the table
// reads OFF / 0 and refuses every write (no key is touched). A switch is
// written only when ON and a keep only when it is not the table default
// (Settings.h SerializeSettings).
const js::SettingsKeepCategory* KeepCategoryAt(std::int32_t a_index) {
    if (a_index < 0 || static_cast<std::size_t>(a_index) >= js::SettingsKeepCount) return nullptr;
    return &js::SettingsKeepCategories[static_cast<std::size_t>(a_index)];
}

bool PapyrusGetKeepCategory(RE::StaticFunctionTag*, std::int32_t a_index) { if (!js::latch.Armed()) return false;
    const auto* category = KeepCategoryAt(a_index);
    return category && js::ReadSettingBool(category->switchKey);
}

bool PapyrusSetKeepCategory(RE::StaticFunctionTag*, std::int32_t a_index, bool a_value) { if (!js::latch.Armed()) return false;
    const auto* category = KeepCategoryAt(a_index);
    return category && Settled(ApplyBool(category->switchKey, a_value));
}

std::int32_t PapyrusGetKeepLimit(RE::StaticFunctionTag*, std::int32_t a_index) { if (!js::latch.Armed()) return 0;
    const auto* category = KeepCategoryAt(a_index);
    return category ? js::ReadSettingInt(category->keepKey) : 0;
}

bool PapyrusSetKeepLimit(RE::StaticFunctionTag*, std::int32_t a_index, std::int32_t a_value) { if (!js::latch.Armed()) return false;
    const auto* category = KeepCategoryAt(a_index);
    return category && Settled(ApplyInt(category->keepKey, a_value));
}

std::int32_t PapyrusGetLastKept(RE::StaticFunctionTag*, std::int32_t a_index) { if (!js::latch.Armed()) return 0;
    return js::ReadLastKept(a_index);
}

std::int32_t PapyrusGetAnywhereRatePercent(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return 0;
    return js::ReadAnywhereRatePercent();
}

std::string PapyrusGetLastMerchantCheck(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return std::string();
    return js::ReadLastMerchantCheck();
}

// v0.8.0: "Price follows perks", API 9. Written only when ON (Settings.h
// SerializeSettings).
bool PapyrusGetPerkPrice(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return false;
    return js::ReadSettingBool(js::SettingsKeyPerkPrice);
}

bool PapyrusSetPerkPrice(RE::StaticFunctionTag*, bool a_value) { if (!js::latch.Armed()) return false;
    return Settled(ApplyBool(js::SettingsKeyPerkPrice, a_value));
}

bool PapyrusRequestRatePreview(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return false;
    return js::RequestAnywhereRatePreview();
}

std::int32_t PapyrusGetRatePreview(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return 0;
    return js::ReadAnywhereRatePreview();
}

std::int32_t PapyrusGetLastPriceSource(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return 0;
    return js::ReadLastPriceSource();
}

std::int32_t PapyrusGetHotkey(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return 0;
    return js::ReadSettingInt(js::SettingsKeyHotkey);
}

bool PapyrusSetHotkey(RE::StaticFunctionTag*, std::int32_t a_value) { if (!js::latch.Armed()) return false;
    return Settled(ApplyInt(js::SettingsKeyHotkey, a_value));
}

std::int32_t PapyrusGetMaxValue(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return 0;
    return js::ReadSettingInt(js::SettingsKeyMaxValue);
}

bool PapyrusSetMaxValue(RE::StaticFunctionTag*, std::int32_t a_value) { if (!js::latch.Armed()) return false;
    return Settled(ApplyInt(js::SettingsKeyMaxValue, a_value));
}

float PapyrusGetMinWeight(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return 0.0F;
    return js::ReadSettingFloat(js::SettingsKeyMinWeight);
}

bool PapyrusSetMinWeight(RE::StaticFunctionTag*, float a_value) { if (!js::latch.Armed()) return false;
    return Settled(ApplyFloat(js::SettingsKeyMinWeight, a_value));
}

float PapyrusGetMaxValueWeight(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return 0.0F;
    return js::ReadSettingFloat(js::SettingsKeyMaxValueWeight);
}

bool PapyrusSetMaxValueWeight(RE::StaticFunctionTag*, float a_value) { if (!js::latch.Armed()) return false;
    return Settled(ApplyFloat(js::SettingsKeyMaxValueWeight, a_value));
}

std::int32_t PapyrusGetMaxItems(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return 0;
    return js::ReadSettingInt(js::SettingsKeyMaxItems);
}

bool PapyrusSetMaxItems(RE::StaticFunctionTag*, std::int32_t a_value) { if (!js::latch.Armed()) return false;
    return Settled(ApplyInt(js::SettingsKeyMaxItems, a_value));
}

std::string PapyrusGetProtectedNames(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return std::string();
    return StoredListToPapyrus(js::ReadSettingText(js::SettingsKeyProtectedNames));
}

bool PapyrusSetProtectedNames(RE::StaticFunctionTag*, std::string a_value) { if (!js::latch.Armed()) return false;
    return Settled(ApplyList(js::SettingsKeyProtectedNames, a_value));
}

std::string PapyrusGetProtectedForms(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return std::string();
    return StoredListToPapyrus(js::ReadSettingText(js::SettingsKeyProtectedForms));
}

bool PapyrusSetProtectedForms(RE::StaticFunctionTag*, std::string a_value) { if (!js::latch.Armed()) return false;
    return Settled(ApplyList(js::SettingsKeyProtectedForms, a_value));
}

std::string PapyrusGetLastSaveError(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return std::string();
    return js::ReadLastSaveError();
}

std::string PapyrusGetLastRunSummary(RE::StaticFunctionTag*) { if (!js::latch.Armed()) return std::string();
    return js::ReadLastRunSummary();
}

// The caller's own contract check: a caller that expected another API records
// the mismatch, and every later effective verdict is then OFF (R29-3). The
// live API is kJsNativeApiVersion (9, v0.8.0); a caller pinned to a retired API
// never reaches this native (Available() already refused it above).
bool PapyrusCheckAPI(RE::StaticFunctionTag*, std::int32_t a_expected) { if (!js::latch.Armed()) return false;
    if (a_expected == kJsNativeApiVersion) return true;
    js::NoteApiMismatch();
    return false;
}

// w-probe-r71 (v0.9.0, E1 latch rule L2): the native batch is one
// resident compound (Install.h kNativeSubs, case j = kNativeSubs[j]);
// RegisterFunction cannot be undone, so a failure leaves the natives bound and
// the plugin latch disarmed (every native above then returns its default).
struct NativeOps {
    RE::BSScript::IVirtualMachine* vm{};
    bool Add(std::size_t j) {
        constexpr std::string_view script = "AGH_JS_Native";
        switch (j) {
        case 0: vm->RegisterFunction("GetAPI", script, PapyrusGetAPI); return true;
        case 1: vm->RegisterFunction("GetStatus", script, PapyrusGetStatus); return true;
        case 2: vm->RegisterFunction("IsEffectivelyEnabled", script, PapyrusIsEffectivelyEnabled); return true;
        case 3: vm->RegisterFunction("GetRuntime", script, PapyrusGetRuntime); return true;
        case 4: vm->RegisterFunction("GetReleaseId", script, PapyrusGetReleaseId); return true;
        case 5: vm->RegisterFunction("GetVersion", script, PapyrusGetVersion); return true;
        case 6: vm->RegisterFunction("GetConfigGeneration", script, PapyrusGetConfigGeneration); return true;
        case 7: vm->RegisterFunction("GetEnabled", script, PapyrusGetEnabled); return true;
        case 8: vm->RegisterFunction("SetEnabled", script, PapyrusSetEnabled); return true;
        case 9: vm->RegisterFunction("GetConfirmBeforeSell", script, PapyrusGetConfirmBeforeSell); return true;
        case 10: vm->RegisterFunction("SetConfirmBeforeSell", script, PapyrusSetConfirmBeforeSell); return true;
        case 11: vm->RegisterFunction("GetSellIngredients", script, PapyrusGetSellIngredients); return true;
        case 12: vm->RegisterFunction("SetSellIngredients", script, PapyrusSetSellIngredients); return true;
        case 13: vm->RegisterFunction("GetSellAnywhere", script, PapyrusGetSellAnywhere); return true;
        case 14: vm->RegisterFunction("SetSellAnywhere", script, PapyrusSetSellAnywhere); return true;
        case 15: vm->RegisterFunction("GetJunkValueOnly", script, PapyrusGetJunkValueOnly); return true;
        case 16: vm->RegisterFunction("SetJunkValueOnly", script, PapyrusSetJunkValueOnly); return true;
        case 17: vm->RegisterFunction("GetJunkMaxValue", script, PapyrusGetJunkMaxValue); return true;
        case 18: vm->RegisterFunction("SetJunkMaxValue", script, PapyrusSetJunkMaxValue); return true;
        case 19: vm->RegisterFunction("GetSellStolen", script, PapyrusGetSellStolen); return true;
        case 20: vm->RegisterFunction("SetSellStolen", script, PapyrusSetSellStolen); return true;
        case 21: vm->RegisterFunction("GetSellWeakWeapons", script, PapyrusGetSellWeakWeapons); return true;
        case 22: vm->RegisterFunction("SetSellWeakWeapons", script, PapyrusSetSellWeakWeapons); return true;
        case 23: vm->RegisterFunction("GetSellWeakArmor", script, PapyrusGetSellWeakArmor); return true;
        case 24: vm->RegisterFunction("SetSellWeakArmor", script, PapyrusSetSellWeakArmor); return true;
        case 25: vm->RegisterFunction("GetSellClothing", script, PapyrusGetSellClothing); return true;
        case 26: vm->RegisterFunction("SetSellClothing", script, PapyrusSetSellClothing); return true;
        case 27: vm->RegisterFunction("GetSellEnchantedLoot", script, PapyrusGetSellEnchantedLoot); return true;
        case 28: vm->RegisterFunction("SetSellEnchantedLoot", script, PapyrusSetSellEnchantedLoot); return true;
        case 29: vm->RegisterFunction("GetEnchantedLootMaxValue", script, PapyrusGetEnchantedLootMaxValue); return true;
        case 30: vm->RegisterFunction("SetEnchantedLootMaxValue", script, PapyrusSetEnchantedLootMaxValue); return true;
        case 31: vm->RegisterFunction("GetKnownEnchantmentsOnly", script, PapyrusGetKnownEnchantmentsOnly); return true;
        case 32: vm->RegisterFunction("SetKnownEnchantmentsOnly", script, PapyrusSetKnownEnchantmentsOnly); return true;
        case 33: vm->RegisterFunction("GetKeepCategory", script, PapyrusGetKeepCategory); return true;
        case 34: vm->RegisterFunction("SetKeepCategory", script, PapyrusSetKeepCategory); return true;
        case 35: vm->RegisterFunction("GetKeepLimit", script, PapyrusGetKeepLimit); return true;
        case 36: vm->RegisterFunction("SetKeepLimit", script, PapyrusSetKeepLimit); return true;
        case 37: vm->RegisterFunction("GetLastKept", script, PapyrusGetLastKept); return true;
        case 38: vm->RegisterFunction("GetAnywhereRatePercent", script, PapyrusGetAnywhereRatePercent); return true;
        case 39: vm->RegisterFunction("GetLastMerchantCheck", script, PapyrusGetLastMerchantCheck); return true;
        case 40: vm->RegisterFunction("GetPerkPrice", script, PapyrusGetPerkPrice); return true;
        case 41: vm->RegisterFunction("SetPerkPrice", script, PapyrusSetPerkPrice); return true;
        case 42: vm->RegisterFunction("RequestRatePreview", script, PapyrusRequestRatePreview); return true;
        case 43: vm->RegisterFunction("GetRatePreview", script, PapyrusGetRatePreview); return true;
        case 44: vm->RegisterFunction("GetLastPriceSource", script, PapyrusGetLastPriceSource); return true;
        case 45: vm->RegisterFunction("GetHotkey", script, PapyrusGetHotkey); return true;
        case 46: vm->RegisterFunction("SetHotkey", script, PapyrusSetHotkey); return true;
        case 47: vm->RegisterFunction("GetMaxValue", script, PapyrusGetMaxValue); return true;
        case 48: vm->RegisterFunction("SetMaxValue", script, PapyrusSetMaxValue); return true;
        case 49: vm->RegisterFunction("GetMinWeight", script, PapyrusGetMinWeight); return true;
        case 50: vm->RegisterFunction("SetMinWeight", script, PapyrusSetMinWeight); return true;
        case 51: vm->RegisterFunction("GetMaxValueWeight", script, PapyrusGetMaxValueWeight); return true;
        case 52: vm->RegisterFunction("SetMaxValueWeight", script, PapyrusSetMaxValueWeight); return true;
        case 53: vm->RegisterFunction("GetMaxItems", script, PapyrusGetMaxItems); return true;
        case 54: vm->RegisterFunction("SetMaxItems", script, PapyrusSetMaxItems); return true;
        case 55: vm->RegisterFunction("GetProtectedNames", script, PapyrusGetProtectedNames); return true;
        case 56: vm->RegisterFunction("SetProtectedNames", script, PapyrusSetProtectedNames); return true;
        case 57: vm->RegisterFunction("GetProtectedForms", script, PapyrusGetProtectedForms); return true;
        case 58: vm->RegisterFunction("SetProtectedForms", script, PapyrusSetProtectedForms); return true;
        case 59: vm->RegisterFunction("GetLastSaveError", script, PapyrusGetLastSaveError); return true;
        case 60: vm->RegisterFunction("GetLastRunSummary", script, PapyrusGetLastRunSummary); return true;
        case 61: vm->RegisterFunction("CheckAPI", script, PapyrusCheckAPI); return true;
        default: return false;
        }
    }
    void Remove(std::size_t) noexcept {}  // resident: never called for kResident sub-calls
};
static_assert(js::e1::kNativeSubs.size() == 62, "NativeOps::Add binds exactly the natives of kNativeSubs");

// The late-failure writer (Install.h LateFailure): the shared gate's line
// through the open receipt writer.
void LogNativeFailure(const std::string& json) { (void)js::LogRawLine(json); }

}  // namespace

namespace js {

// w-probe-r71 (v0.9.0): the VM callback reads the plugin latch first; the
// batch runs through agh::rt::RunCompound and a throw from RegisterFunction
// after SKSEPlugin_Load returned makes the plugin inert with one load_failure
// row naming papyrus/<native>.
bool RegisterPapyrusBridge() {
    auto* papyrus = SKSE::GetPapyrusInterface();
    return papyrus && papyrus->Register([](RE::BSScript::IVirtualMachine* vm) {
        if (!js::latch.Armed()) return false;
        if (!vm) return false;
        NativeOps ops{ vm };
        const auto outcome = agh::rt::RunCompound(std::span<const agh::rt::SubSpec>{ js::e1::kNativeSubs }, ops);
        if (outcome.result != agh::rt::StepResult::kOk) {
            js::e1::LateFailure("papyrus", outcome, js::latch, &LogNativeFailure);
            return false;
        }
        return true;
    });
}

}  // namespace js
