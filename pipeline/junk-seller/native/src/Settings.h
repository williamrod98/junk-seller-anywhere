#pragma once
// w-probe-r71, extending the w-probe-r28 settings contract and the
// w-probe-r29 (B-002 of
// a review). The engine-free D-W11 settings contract.
//
// Four things live here, all pure so the offline harness drives exactly the
// code the runtime uses:
//   * js::Settings -- the v0.1 key set and defaults of
//     the design notes section 3.3 / design section 9, and
//     the one-key apply that validates it (unknown key, parse error, out of
//     range -> refused, no state change).
//   * the user settings file round trip (SerializeSettings / ParseSettings,
//     key=value lines, `#` comments, CRLF tolerant).
//   * js::EffectiveState -- the fixed ON/OFF reason set of R29-3.
//   * the atomic save and readback, SaveSettingsToFile (r29, B-002). It takes
//     the target path as a parameter and uses only std::filesystem, so the
//     host harness can drive the write-success, write-failure and
//     readback-mismatch branches in a temporary directory. The path itself,
//     the mutex and the live swap stay with the caller (Probe.cpp).
//
// Nothing here includes an engine header and nothing here touches an RE
// object.
#include "Protection.h"
#include "Selector.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace js {
// Product runtime limits formerly supplied by probe-only INI switches.
inline constexpr unsigned TaskBudgetUs = 2000;
inline constexpr unsigned RingSize = 4096;
inline constexpr unsigned MerchantFallback = 1;
inline constexpr unsigned RequestTtlMs = 1000;
inline constexpr unsigned VisitCap = 262144;
inline constexpr std::int32_t SaleQuantity = 1;

// The product base INI keeps only its [Probe] section; settings live in the
// MCM-backed user file. Any base key, including a retired probe key, refuses.
inline bool IsBaseIniKeyAllowed(std::string_view) noexcept { return false; }

// v0.5.0 (the +5.7 s frame investigation): a developer key of the
// base file. `DevFrameWindow=1` arms the observe-only js_frame_window receipt
// row after each sell-anywhere press (timestamps of the conversion end and of
// the receipt writer's drain, a main-thread task sampler from +5 s to +7 s
// after the key, and the events seen meanwhile). It never changes what a
// press sells. The shipped file carries `DevFrameWindow=0`.
inline constexpr const char* BaseIniKeyDevFrameWindow = "DevFrameWindow";
// v0.8.0 (the barter-price design r2 section 9): the second developer key.
// `DevPerkFallback=1` makes every price snapshot with "Price follows perks" ON
// use the shipped formula with the reason dev_forced (no perk walk), so the
// live L1 can prove the fallback path and its Diagnostics row. It never
// changes what a press sells, only how it is priced. The shipped file carries
// `DevPerkFallback=0`.
inline constexpr const char* BaseIniKeyDevPerkFallback = "DevPerkFallback";

// The shipped base file is a presence marker, not a settings source. It has
// exactly one [Probe] section and, since v0.5.0, at most one entry after it:
// `DevFrameWindow=0` or `DevFrameWindow=1`; since v0.8.0 also at most one
// `DevPerkFallback=0` or `DevPerkFallback=1`. Every other non-comment line is
// a refusal, including a former numeric, switch, or list key, a second
// DevFrameWindow or DevPerkFallback line and any other value.
// `devFrameWindow` / `devPerkFallback` (optional) receive the keys' values
// (false when absent).
inline bool ParseBaseIni(std::string_view text, bool* devFrameWindow = nullptr,
                         bool* devPerkFallback = nullptr) noexcept {
    bool section = false;
    bool devSeen = false;
    bool dev = false;
    bool perkSeen = false;
    bool perk = false;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find('\n', start);
        auto line = text.substr(start, end == std::string_view::npos ?
                                            std::string_view::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty() && line.front() != ';' && line.front() != '#') {
            if (line == "[Probe]" && !section) section = true;
            else if (section && !devSeen && (line == "DevFrameWindow=0" || line == "DevFrameWindow=1")) {
                devSeen = true;
                dev = line.back() == '1';
            } else if (section && !perkSeen && (line == "DevPerkFallback=0" || line == "DevPerkFallback=1")) {
                perkSeen = true;
                perk = line.back() == '1';
            } else {
                return false;
            }
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    if (section && devFrameWindow) *devFrameWindow = dev;
    if (section && devPerkFallback) *devPerkFallback = perk;
    return section;
}

// The v0.1 keys exactly as requirements section 3.3 and the probe ini spell
// them. `Mode` is display only and is never stored or serialized (R29-1).
inline constexpr const char* SettingsKeyEnabled = "Enabled";
inline constexpr const char* SettingsKeyMode = "Mode";
inline constexpr const char* SettingsKeyConfirmBeforeSell = "ConfirmBeforeSell";
inline constexpr const char* SettingsKeyHotkey = "Hotkey";
inline constexpr const char* SettingsKeyMaxValue = "MaxValue";
inline constexpr const char* SettingsKeyMinWeight = "MinWeight";
inline constexpr const char* SettingsKeyMaxValueWeight = "MaxValueWeight";
inline constexpr const char* SettingsKeyMaxItems = "MaxItems";
inline constexpr const char* SettingsKeyProtectedNames = "ProtectedNames";
inline constexpr const char* SettingsKeyProtectedForms = "ProtectedForms";
// (design-r2 section 4): the ingredient switch. Default OFF, and written
// to the user file only when ON, so every file up to v0.0.3 (which never had
// the key) loads as OFF and a never-opted-in file still loads on v0.0.3.
inline constexpr const char* SettingsKeySellIngredients = "SellIngredients";
// v0.1.0 (the v0.1.0 design r3 sections 1.4 and 7): the sell
// anywhere switch and the three player-facing category switches. Same rule as
// SellIngredients: default OFF, written only when ON, so every older file
// loads as OFF and an update changes nothing until the player opts in.
inline constexpr const char* SettingsKeySellAnywhere = "SellAnywhere";
inline constexpr const char* SettingsKeySellFood = "SellFood";
inline constexpr const char* SettingsKeySellMisc = "SellMisc";
inline constexpr const char* SettingsKeySellGear = "SellGear";
// v0.2.0 (the v0.2 update plan r2 sections 4.1 and 4.5):
// the three v0.1.0 category switches above are RETIRED: a line of theirs is
// read (0 or 1) and ignored, and it is never written again. The v0.2.0 keys
// below are written only when they differ from the shipped default, so an
// untouched v0.1.0 file loads unchanged and a file that holds only defaults
// still loads on v0.1.0.
//   JunkRule        Classic (default) | ValueOnly (audit A-04: a choice, never
//                   a silent widening of an existing player's junk)
//   JunkMaxValue    0..200, default 50 (used by ValueOnly)
//   SellStolen      "Include stolen items", default OFF
//   SellWeakWeapons / SellWeakArmor / SellClothing, default OFF
inline constexpr const char* SettingsKeyJunkRule = "JunkRule";
inline constexpr const char* SettingsKeyJunkMaxValue = "JunkMaxValue";
inline constexpr const char* SettingsKeySellStolen = "SellStolen";
inline constexpr const char* SettingsKeySellWeakWeapons = "SellWeakWeapons";
inline constexpr const char* SettingsKeySellWeakArmor = "SellWeakArmor";
inline constexpr const char* SettingsKeySellClothing = "SellClothing";
// v0.3.0 (the v0.2 update plan r2 sections 4.1, 4.2 and 4.5), the same
// rule (a key only when it differs from the shipped default; no schema move,
// so a v0.2.0 build loads a v0.3.0 file once these lines are removed):
//   SellEnchantedLoot      "Weak enchanted loot", default OFF
//   EnchantedLootMaxValue  0..2000, default 300: the jewelry / clothing bound
//                          for enchantments off the vanilla ladder
//   KnownEnchantmentsOnly  "Known enchants only", default ON (written as 0)
inline constexpr const char* SettingsKeySellEnchantedLoot = "SellEnchantedLoot";
inline constexpr const char* SettingsKeyEnchantedLootMaxValue = "EnchantedLootMaxValue";
inline constexpr const char* SettingsKeyKnownEnchantmentsOnly = "KnownEnchantmentsOnly";
// v0.8.0 (the barter-price design r2 section 4): "Price follows perks",
// default OFF for new installs and updates, written only when ON (the
// SellAnywhere rule), so an update pays exactly what v0.7.0 paid until the
// player turns it on, and a file without the line still loads on v0.7.0.
inline constexpr const char* SettingsKeyPerkPrice = "PerkPrice";
// v0.4.0 (the v0.2 update plan r2 sections 3, 4.1, 4.2 and 4.5): the eight
// keep-limit categories of the Categories page, one switch and one keep per
// kind each, the same rule (a key only when it differs from the shipped
// default, no schema move). Index order = the MCM row order and the
// GetKeepCategory / GetKeepLimit native index. The anywhere ingredient switch
// has its own key: SellIngredients keeps the in-menu path's v0.0.4 meaning.
// v0.5.0 (the v0.2 update plan r2 sections 4.1 and 4.2): four more rows,
// appended (no index moves): restore and cure potions 100 per effect pool,
// skill potions 100 per skill pool, other potions 10 and poisons 10 per kind.
// v0.6.0 (the v0.2 update plan r2 sections 4.1 and 4.2): three more rows,
// appended (no index moves): mid-tier gear, high-tier gear and jewelry, keep
// 10 per kind each (0-100).
struct SettingsKeepCategory {
    const char* switchKey;
    const char* keepKey;
    int keepDefault;
    int keepCeiling;
};
inline constexpr std::size_t SettingsKeepCount = 15;
enum SettingsKeepIndex : std::size_t {
    kKeepIndexIngredients, kKeepIndexCookedFood, kKeepIndexRawFood, kKeepIndexDrinks,
    kKeepIndexOre, kKeepIndexGems, kKeepIndexPelts, kKeepIndexTools,
    kKeepIndexRestorePotions, kKeepIndexSkillPotions, kKeepIndexOtherPotions, kKeepIndexPoisons,
    kKeepIndexMidGear, kKeepIndexHighGear, kKeepIndexJewelry,
};
inline constexpr std::array<SettingsKeepCategory, SettingsKeepCount> SettingsKeepCategories{{
    {"SellIngredientsAnywhere", "KeepIngredients", 500, 2000},
    {"SellCookedFood", "KeepCookedFood", 10, 500},
    {"SellRawFood", "KeepRawFood", 20, 500},
    {"SellDrinks", "KeepDrinks", 10, 500},
    {"SellOre", "KeepOre", 50, 1000},
    {"SellGems", "KeepGems", 50, 1000},
    {"SellPelts", "KeepPelts", 100, 1000},
    {"SellTools", "KeepTools", 1, 20},
    {"SellRestorePotions", "KeepRestorePotions", 100, 1000},
    {"SellSkillPotions", "KeepSkillPotions", 100, 1000},
    {"SellOtherPotions", "KeepOtherPotions", 10, 500},
    {"SellPoisons", "KeepPoisons", 10, 500},
    {"SellMidGear", "KeepMidGear", 10, 100},
    {"SellHighGear", "KeepHighGear", 10, 100},
    {"SellJewelry", "KeepJewelry", 10, 100},
}};
// The table index of a keep-category switch or keep key, or SettingsKeepCount.
inline std::size_t SettingsKeepSwitchIndex(std::string_view key) noexcept {
    for (std::size_t i = 0; i < SettingsKeepCount; ++i) {
        if (key == SettingsKeepCategories[i].switchKey) return i;
    }
    return SettingsKeepCount;
}
inline std::size_t SettingsKeepLimitIndex(std::string_view key) noexcept {
    for (std::size_t i = 0; i < SettingsKeepCount; ++i) {
        if (key == SettingsKeepCategories[i].keepKey) return i;
    }
    return SettingsKeepCount;
}
inline constexpr std::array<int, SettingsKeepCount> SettingsKeepDefaults() noexcept {
    std::array<int, SettingsKeepCount> out{};
    for (std::size_t i = 0; i < SettingsKeepCount; ++i) out[i] = SettingsKeepCategories[i].keepDefault;
    return out;
}
inline constexpr int SettingsEnchantedLootMaxValueDefault = 300;
inline constexpr int SettingsEnchantedLootMaxValueCeiling = 2000;
inline constexpr const char* SettingsJunkRuleClassic = "Classic";
inline constexpr const char* SettingsJunkRuleValueOnly = "ValueOnly";
inline constexpr int SettingsJunkMaxValueDefault = 50;
inline constexpr int SettingsJunkMaxValueCeiling = 200;
// The one Mode value v0.1 accepts (Automatic is v0.3).
inline constexpr const char* SettingsModeName = "Hotkey";

// Receipt rows outlive their Papyrus callers. Return only process-lifetime
// key literals so a row never retains a caller-owned string.
inline const char* SettingsKeyConstant(std::string_view key) noexcept {
    if (key == SettingsKeyEnabled) return SettingsKeyEnabled;
    if (key == SettingsKeyMode) return SettingsKeyMode;
    if (key == SettingsKeyConfirmBeforeSell) return SettingsKeyConfirmBeforeSell;
    if (key == SettingsKeyHotkey) return SettingsKeyHotkey;
    if (key == SettingsKeyMaxValue) return SettingsKeyMaxValue;
    if (key == SettingsKeyMinWeight) return SettingsKeyMinWeight;
    if (key == SettingsKeyMaxValueWeight) return SettingsKeyMaxValueWeight;
    if (key == SettingsKeyMaxItems) return SettingsKeyMaxItems;
    if (key == SettingsKeyProtectedNames) return SettingsKeyProtectedNames;
    if (key == SettingsKeyProtectedForms) return SettingsKeyProtectedForms;
    if (key == SettingsKeySellIngredients) return SettingsKeySellIngredients;
    if (key == SettingsKeySellAnywhere) return SettingsKeySellAnywhere;
    if (key == SettingsKeySellFood) return SettingsKeySellFood;
    if (key == SettingsKeySellMisc) return SettingsKeySellMisc;
    if (key == SettingsKeySellGear) return SettingsKeySellGear;
    if (key == SettingsKeyJunkRule) return SettingsKeyJunkRule;
    if (key == SettingsKeyJunkMaxValue) return SettingsKeyJunkMaxValue;
    if (key == SettingsKeySellStolen) return SettingsKeySellStolen;
    if (key == SettingsKeySellWeakWeapons) return SettingsKeySellWeakWeapons;
    if (key == SettingsKeySellWeakArmor) return SettingsKeySellWeakArmor;
    if (key == SettingsKeySellClothing) return SettingsKeySellClothing;
    if (key == SettingsKeySellEnchantedLoot) return SettingsKeySellEnchantedLoot;
    if (key == SettingsKeyEnchantedLootMaxValue) return SettingsKeyEnchantedLootMaxValue;
    if (key == SettingsKeyKnownEnchantmentsOnly) return SettingsKeyKnownEnchantmentsOnly;
    if (key == SettingsKeyPerkPrice) return SettingsKeyPerkPrice;
    // v0.4.0: the table's own literals (process lifetime).
    if (const auto i = SettingsKeepSwitchIndex(key); i < SettingsKeepCount) return SettingsKeepCategories[i].switchKey;
    if (const auto i = SettingsKeepLimitIndex(key); i < SettingsKeepCount) return SettingsKeepCategories[i].keepKey;
    return "unknown";
}

// Both list keys are semicolon separated in the ini and in the user file (the
// spelling ParseProtectedNames/ParseProtectedForms already use). The Papyrus
// natives join/split their own string with PapyrusListSeparator instead, so
// the file keeps loading an existing ini value unchanged.
inline constexpr char SettingsListSeparator = ';';
inline constexpr char PapyrusListSeparator = '|';

// The v0.1 bounds. MaxItems is bounded by the accepted T-W06 transaction cap,
// never above it (R29-1), and MinWeight/MaxValue/MaxValueWeight refuse a
// negative or non-finite value. The two large caps only exist so a nonsense
// number is a refusal instead of a live threshold.
inline constexpr int SettingsMaxItemsFloor = 1;
inline constexpr int SettingsMaxItemsCeiling = static_cast<int>(SelectorMaxTransactions);
// D2: 40 was the ceiling and the default of every build up to v0.0.3. In
// a file written before v0.0.4 a stored 40 therefore means "the old maximum"
// and is migrated once, at load, to the new ceiling (256); values 1..39 are an
// explicit smaller choice and are kept. The file schema marker below tells the
// two apart: a file that carries it was written by v0.0.4 or later, and there
// 40 is an ordinary value (design ruling, design-QA P3).
inline constexpr int SettingsMaxItemsLegacyCeiling = 40;
// The marker is a COMMENT line on purpose: v0.0.3 skips `#` lines, so a file
// saved by v0.0.4 still loads after a rollback (design-r2 4.3). It is written
// by every save, exactly once, and it is never a settings key.
// v0.2.0 (the v0.2 update plan 4.5): every save writes schema 5; a file
// with the schema-4 marker (v0.0.4 .. v0.1.0) is read exactly the same way.
inline constexpr const char* SettingsSchemaMarker = "# SettingsSchema=5";
inline constexpr const char* SettingsSchemaMarkerV4 = "# SettingsSchema=4";
inline constexpr int SettingsHotkeyCeiling = 255;
inline constexpr int SettingsIntCeiling = 1000000;
inline constexpr float SettingsFloatCeiling = 1000000.0F;

// The refusals ApplySetting can answer with. `readonly` is the display-only
// `Mode` key: it is known, it validates, and it never changes state.
inline constexpr const char* SettingReasonOk = "ok";
inline constexpr const char* SettingReasonUnknownKey = "unknown_key";
inline constexpr const char* SettingReasonParseError = "parse_error";
inline constexpr const char* SettingReasonOutOfRange = "out_of_range";
inline constexpr const char* SettingReasonReadonly = "readonly";

// The fixed effective-state reason tokens of R29-3, in priority order.
inline constexpr const char* EffectiveReasonOk = "OK";
inline constexpr const char* EffectiveReasonDisabled = "DISABLED";
inline constexpr const char* EffectiveReasonRuntimeMismatch = "RUNTIME_MISMATCH";
inline constexpr const char* EffectiveReasonAdapterUnproven = "ADAPTER_UNPROVEN";
inline constexpr const char* EffectiveReasonConfigCorrupt = "CONFIG_CORRUPT";
inline constexpr const char* EffectiveReasonApiMismatch = "API_MISMATCH";

// The live settings of this process. Defaults are requirements section 3.3
// verbatim; `maxItems` defaults to the selector's own cap because the two must
// never disagree.
struct Settings {
    bool enabled{true};
    bool confirmBeforeSell{false};
    // Scancode, 0 = unassigned.
    int hotkey{0};
    int maxValue{10};
    float minWeight{0.5F};
    float maxValueWeight{10.0F};
    int maxItems{SettingsMaxItemsCeiling};
    std::vector<std::string> protectedNames{};
    std::vector<std::string> protectedForms{};
    bool sellIngredients{false};
    // v0.1.0: OFF by default (see the key comment above).
    bool sellAnywhere{false};
    // v0.2.0: the Categories page (see the key comment above). The
    // retired SellFood / SellMisc / SellGear have no field any more.
    bool junkValueOnly{false};
    int junkMaxValue{SettingsJunkMaxValueDefault};
    bool sellStolen{false};
    bool sellWeakWeapons{false};
    bool sellWeakArmor{false};
    bool sellClothing{false};
    // v0.3.0: weak enchanted loot (see the key comment above).
    bool sellEnchantedLoot{false};
    int enchantedLootMaxValue{SettingsEnchantedLootMaxValueDefault};
    bool knownEnchantmentsOnly{true};
    // v0.4.0: the keep-limit categories (SettingsKeepCategories order):
    // every switch OFF, every keep at its table default.
    std::array<bool, SettingsKeepCount> keepOn{};
    std::array<int, SettingsKeepCount> keepLimit{SettingsKeepDefaults()};
    // v0.8.0: "Price follows perks" (see the key comment above).
    bool perkPrice{false};
};

struct SettingResult {
    bool accepted{false};
    const char* reason{SettingReasonUnknownKey};
};

struct EffectiveStateResult {
    bool on{false};
    const char* reason{EffectiveReasonDisabled};
};

// True when every stored field is equal. `Mode` is not part of it by design
// (display only, never stored). The save readback uses exactly this test.
inline bool SameSettings(const Settings& left, const Settings& right) noexcept {
    return left.enabled == right.enabled &&
           left.confirmBeforeSell == right.confirmBeforeSell &&
           left.hotkey == right.hotkey && left.maxValue == right.maxValue &&
           left.minWeight == right.minWeight && left.maxValueWeight == right.maxValueWeight &&
           left.maxItems == right.maxItems && left.protectedNames == right.protectedNames &&
           left.protectedForms == right.protectedForms &&
           left.sellIngredients == right.sellIngredients &&
           left.sellAnywhere == right.sellAnywhere && left.junkValueOnly == right.junkValueOnly &&
           left.junkMaxValue == right.junkMaxValue && left.sellStolen == right.sellStolen &&
           left.sellWeakWeapons == right.sellWeakWeapons && left.sellWeakArmor == right.sellWeakArmor &&
           left.sellClothing == right.sellClothing && left.sellEnchantedLoot == right.sellEnchantedLoot &&
           left.enchantedLootMaxValue == right.enchantedLootMaxValue &&
           left.knownEnchantmentsOnly == right.knownEnchantmentsOnly && left.keepOn == right.keepOn &&
           left.keepLimit == right.keepLimit && left.perkPrice == right.perkPrice;
}

// One list joined with `separator`. An empty list joins to the empty string,
// which parses back to the empty list.
inline std::string JoinSettingsList(const std::vector<std::string>& items, char separator) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i) out += separator;
        out += items[i];
    }
    return out;
}

// One list split on `separator`. An empty text is an empty list (not one empty
// item), matching ParseProtectedNames' "empty protects nothing".
inline std::vector<std::string> SplitSettingsList(std::string_view text, char separator) {
    std::vector<std::string> items;
    if (text.empty()) return items;
    std::size_t start = 0;
    for (;;) {
        const std::size_t end = text.find(separator, start);
        const std::string_view piece =
            text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        // Only the edges are trimmed, so an interior space is kept, exactly
        // like Protection.h's own list parser.
        std::size_t first = 0, last = piece.size();
        while (first < last && ProtectionSpace(piece[first])) ++first;
        while (last > first && ProtectionSpace(piece[last - 1])) --last;
        items.emplace_back(piece.substr(first, last - first));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return items;
}

// A strict non-negative/negative integer: an optional sign then digits, with
// no other character and no whitespace.
inline bool ParseSettingInt(std::string_view text, int& out) noexcept {
    if (text.empty()) return false;
    std::size_t index = 0;
    bool negative = false;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        index = 1;
    }
    if (index >= text.size()) return false;
    std::int64_t value = 0;
    for (; index < text.size(); ++index) {
        const char c = text[index];
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
        if (value > (std::numeric_limits<std::int64_t>::max)() / 16) return false;
    }
    if (negative) value = -value;
    if (value < (std::numeric_limits<int>::min)() || value > (std::numeric_limits<int>::max)()) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

// A finite float. strtof is used through a bounded copy so the parse is the
// same on MSVC and on the host compiler, and the whole text must be consumed.
inline bool ParseSettingFloat(std::string_view text, float& out) noexcept {
    if (text.empty() || text.size() >= 64) return false;
    char buffer[64]{};
    for (std::size_t i = 0; i < text.size(); ++i) buffer[i] = text[i];
    char* end = nullptr;
    const float value = std::strtof(buffer, &end);
    if (end != buffer + text.size()) return false;
    if (!std::isfinite(value)) return false;
    out = value;
    return true;
}

// The shortest text that parses back to the same float.
inline std::string FormatSettingFloat(float value) {
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
    return std::string(buffer);
}

// One key=value apply. Every refusal leaves `settings` untouched (the value is
// validated before it is stored), which is what the save path and the parser
// both rely on.
inline SettingResult ApplySetting(Settings& settings, std::string_view key, std::string_view value) {
    // v0.4.0: a keep-category switch (0 / 1) or keep (0..its ceiling).
    if (const auto i = SettingsKeepSwitchIndex(key); i < SettingsKeepCount) {
        if (value != "0" && value != "1") return {false, SettingReasonParseError};
        settings.keepOn[i] = value == "1";
        return {true, SettingReasonOk};
    }
    if (const auto i = SettingsKeepLimitIndex(key); i < SettingsKeepCount) {
        int number = 0;
        if (!ParseSettingInt(value, number)) return {false, SettingReasonParseError};
        if (number < 0 || number > SettingsKeepCategories[i].keepCeiling) return {false, SettingReasonOutOfRange};
        settings.keepLimit[i] = number;
        return {true, SettingReasonOk};
    }
    if (key == SettingsKeyMode) {
        // Display only: the one accepted value changes nothing.
        if (value == SettingsModeName) return {true, SettingReasonOk};
        return {false, SettingReasonReadonly};
    }
    if (key == SettingsKeyEnabled || key == SettingsKeyConfirmBeforeSell ||
        key == SettingsKeySellIngredients || key == SettingsKeySellAnywhere ||
        key == SettingsKeySellFood || key == SettingsKeySellMisc || key == SettingsKeySellGear ||
        key == SettingsKeySellStolen || key == SettingsKeySellWeakWeapons ||
        key == SettingsKeySellWeakArmor || key == SettingsKeySellClothing ||
        key == SettingsKeySellEnchantedLoot || key == SettingsKeyKnownEnchantmentsOnly ||
        key == SettingsKeyPerkPrice) {
        if (value != "0" && value != "1") return {false, SettingReasonParseError};
        const bool on = value == "1";
        if (key == SettingsKeyEnabled) settings.enabled = on;
        else if (key == SettingsKeyConfirmBeforeSell) settings.confirmBeforeSell = on;
        else if (key == SettingsKeySellIngredients) settings.sellIngredients = on;
        else if (key == SettingsKeySellAnywhere) settings.sellAnywhere = on;
        else if (key == SettingsKeySellStolen) settings.sellStolen = on;
        else if (key == SettingsKeySellWeakWeapons) settings.sellWeakWeapons = on;
        else if (key == SettingsKeySellWeakArmor) settings.sellWeakArmor = on;
        else if (key == SettingsKeySellClothing) settings.sellClothing = on;
        else if (key == SettingsKeySellEnchantedLoot) settings.sellEnchantedLoot = on;
        else if (key == SettingsKeyKnownEnchantmentsOnly) settings.knownEnchantmentsOnly = on;
        else if (key == SettingsKeyPerkPrice) settings.perkPrice = on;
        // v0.2.0: SellFood / SellMisc / SellGear are retired: a valid
        // line is accepted and changes nothing.
        return {true, SettingReasonOk};
    }
    if (key == SettingsKeyJunkRule) {
        if (value == SettingsJunkRuleClassic) settings.junkValueOnly = false;
        else if (value == SettingsJunkRuleValueOnly) settings.junkValueOnly = true;
        else return {false, SettingReasonParseError};
        return {true, SettingReasonOk};
    }
    if (key == SettingsKeyJunkMaxValue) {
        int number = 0;
        if (!ParseSettingInt(value, number)) return {false, SettingReasonParseError};
        if (number < 0 || number > SettingsJunkMaxValueCeiling) return {false, SettingReasonOutOfRange};
        settings.junkMaxValue = number;
        return {true, SettingReasonOk};
    }
    if (key == SettingsKeyEnchantedLootMaxValue) {
        int number = 0;
        if (!ParseSettingInt(value, number)) return {false, SettingReasonParseError};
        if (number < 0 || number > SettingsEnchantedLootMaxValueCeiling) return {false, SettingReasonOutOfRange};
        settings.enchantedLootMaxValue = number;
        return {true, SettingReasonOk};
    }
    if (key == SettingsKeyHotkey || key == SettingsKeyMaxValue || key == SettingsKeyMaxItems) {
        int number = 0;
        if (!ParseSettingInt(value, number)) return {false, SettingReasonParseError};
        if (key == SettingsKeyHotkey) {
            if (number < 0 || number > SettingsHotkeyCeiling) return {false, SettingReasonOutOfRange};
            settings.hotkey = number;
        } else if (key == SettingsKeyMaxValue) {
            if (number < 0 || number > SettingsIntCeiling) return {false, SettingReasonOutOfRange};
            settings.maxValue = number;
        } else {
            // R29-1: MaxItems is a transaction unit bound; a value above the
            // accepted selector cap is refused rather than clamped.
            if (number < SettingsMaxItemsFloor || number > SettingsMaxItemsCeiling) {
                return {false, SettingReasonOutOfRange};
            }
            // D2: a plain store. The one-time migration of a legacy 40
            // lives in ParseSettings, so an MCM choice of 40 stays 40.
            settings.maxItems = number;
        }
        return {true, SettingReasonOk};
    }
    if (key == SettingsKeyMinWeight || key == SettingsKeyMaxValueWeight) {
        float number = 0.0F;
        if (!ParseSettingFloat(value, number)) return {false, SettingReasonParseError};
        if (number < 0.0F || number > SettingsFloatCeiling) return {false, SettingReasonOutOfRange};
        if (key == SettingsKeyMinWeight) settings.minWeight = number;
        else settings.maxValueWeight = number;
        return {true, SettingReasonOk};
    }
    if (key == SettingsKeyProtectedNames || key == SettingsKeyProtectedForms) {
        std::vector<std::string> items = SplitSettingsList(value, SettingsListSeparator);
        if (key == SettingsKeyProtectedNames) {
            ProtectedNames scratch;
            std::string normalized(value);
            if (!ParseProtectedNames(normalized.c_str(), scratch)) return {false, SettingReasonParseError};
            settings.protectedNames = std::move(items);
        } else {
            ProtectedFormKeys scratch;
            std::string normalized(value);
            if (!ParseProtectedForms(normalized.c_str(), scratch)) return {false, SettingReasonParseError};
            settings.protectedForms = std::move(items);
        }
        return {true, SettingReasonOk};
    }
    return {false, SettingReasonUnknownKey};
}

// The user file text of `settings`. `Mode` is deliberately absent (R29-1:
// display only, not stored), so parsing the text back into a default Settings
// reproduces every stored field.
inline std::string SerializeSettings(const Settings& settings) {
    std::string out;
    out += "# AGH_JunkSeller user settings (w-probe-r71). One key=value per line.\n";
    out += "# Mode is display-only (Hotkey) and is never stored here.\n";
    out += "# Lists are semicolon separated; an empty value is an empty list.\n";
    out += std::string(SettingsSchemaMarker) + "\n";
    out += std::string(SettingsKeyEnabled) + "=" + (settings.enabled ? "1" : "0") + "\n";
    out += std::string(SettingsKeyConfirmBeforeSell) + "=" +
           (settings.confirmBeforeSell ? "1" : "0") + "\n";
    out += std::string(SettingsKeyHotkey) + "=" + std::to_string(settings.hotkey) + "\n";
    out += std::string(SettingsKeyMaxValue) + "=" + std::to_string(settings.maxValue) + "\n";
    out += std::string(SettingsKeyMinWeight) + "=" + FormatSettingFloat(settings.minWeight) + "\n";
    out += std::string(SettingsKeyMaxValueWeight) + "=" +
           FormatSettingFloat(settings.maxValueWeight) + "\n";
    out += std::string(SettingsKeyMaxItems) + "=" + std::to_string(settings.maxItems) + "\n";
    out += std::string(SettingsKeyProtectedNames) + "=" +
           JoinSettingsList(settings.protectedNames, SettingsListSeparator) + "\n";
    out += std::string(SettingsKeyProtectedForms) + "=" +
           JoinSettingsList(settings.protectedForms, SettingsListSeparator) + "\n";
    // (design-r2 section 4.3): only an explicit opt-in is written; OFF
    // is the absent key, so a rollback to v0.0.3 keeps loading the file.
    if (settings.sellIngredients) out += std::string(SettingsKeySellIngredients) + "=1\n";
    // v0.1.0 (design r3 section 1.4): the same opt-in rule. A rollback
    // to v0.0.4 refuses a file that holds one of these lines until the line
    // is removed (the stop/rollback text says so).
    if (settings.sellAnywhere) out += std::string(SettingsKeySellAnywhere) + "=1\n";
    // v0.2.0 (the v0.2 update plan 4.5): a key only when it differs from
    // the shipped default; the retired v0.1.0 category keys are never written.
    if (settings.junkValueOnly) {
        out += std::string(SettingsKeyJunkRule) + "=" + SettingsJunkRuleValueOnly + "\n";
    }
    if (settings.junkMaxValue != SettingsJunkMaxValueDefault) {
        out += std::string(SettingsKeyJunkMaxValue) + "=" + std::to_string(settings.junkMaxValue) + "\n";
    }
    if (settings.sellStolen) out += std::string(SettingsKeySellStolen) + "=1\n";
    if (settings.sellWeakWeapons) out += std::string(SettingsKeySellWeakWeapons) + "=1\n";
    if (settings.sellWeakArmor) out += std::string(SettingsKeySellWeakArmor) + "=1\n";
    if (settings.sellClothing) out += std::string(SettingsKeySellClothing) + "=1\n";
    // v0.3.0: the same rule for weak enchanted loot.
    if (settings.sellEnchantedLoot) out += std::string(SettingsKeySellEnchantedLoot) + "=1\n";
    if (settings.enchantedLootMaxValue != SettingsEnchantedLootMaxValueDefault) {
        out += std::string(SettingsKeyEnchantedLootMaxValue) + "=" + std::to_string(settings.enchantedLootMaxValue) +
               "\n";
    }
    if (!settings.knownEnchantmentsOnly) out += std::string(SettingsKeyKnownEnchantmentsOnly) + "=0\n";
    // v0.4.0: the same rule for the keep-limit categories, in table order.
    for (std::size_t i = 0; i < SettingsKeepCount; ++i) {
        const auto& category = SettingsKeepCategories[i];
        if (settings.keepOn[i]) out += std::string(category.switchKey) + "=1\n";
        if (settings.keepLimit[i] != category.keepDefault) {
            out += std::string(category.keepKey) + "=" + std::to_string(settings.keepLimit[i]) + "\n";
        }
    }
    // v0.8.0: the same opt-in rule for "Price follows perks".
    if (settings.perkPrice) out += std::string(SettingsKeyPerkPrice) + "=1\n";
    return out;
}

// Keys the v0.1 contract stores, so the parser can reject a file that names
// something else (ApplySetting would answer unknown_key anyway; this list only
// exists so a caller can pre-check).
inline bool IsStoredSettingKey(std::string_view key) noexcept {
    return key == SettingsKeyEnabled || key == SettingsKeyConfirmBeforeSell ||
           key == SettingsKeyHotkey || key == SettingsKeyMaxValue ||
           key == SettingsKeyMinWeight || key == SettingsKeyMaxValueWeight ||
           key == SettingsKeyMaxItems || key == SettingsKeyProtectedNames ||
           key == SettingsKeyProtectedForms || key == SettingsKeySellIngredients ||
           key == SettingsKeySellAnywhere || key == SettingsKeyJunkRule ||
           key == SettingsKeyJunkMaxValue || key == SettingsKeySellStolen ||
           key == SettingsKeySellWeakWeapons || key == SettingsKeySellWeakArmor ||
           key == SettingsKeySellClothing || key == SettingsKeySellEnchantedLoot ||
           key == SettingsKeyEnchantedLootMaxValue || key == SettingsKeyKnownEnchantmentsOnly ||
           key == SettingsKeyPerkPrice ||
           SettingsKeepSwitchIndex(key) < SettingsKeepCount || SettingsKeepLimitIndex(key) < SettingsKeepCount;
}

// Parse one key=value file. `#` and `;` start a comment line, a `[section]`
// line is tolerated so a copied probe ini still loads, CRLF is accepted, and a
// blank line is skipped. On any refusal `out` is left exactly as it was (the
// whole file is applied to a scratch copy), so the caller's live settings can
// never be left half-applied.
inline bool ParseSettings(std::string_view text, Settings& out, std::vector<std::string>& errors) {
    Settings candidate = out;
    bool ok = true;
    // D2 migration state: the schema marker, and whether this file
    // itself stored MaxItems.
    bool schemaMarker = false;
    bool fileMaxItems = false;
    std::size_t lineStart = 0;
    while (lineStart <= text.size()) {
        const std::size_t newline = text.find('\n', lineStart);
        std::string_view line = text.substr(
            lineStart, newline == std::string_view::npos ? std::string_view::npos : newline - lineStart);
        lineStart = newline == std::string_view::npos ? text.size() + 1 : newline + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty() && line[0] == '#' && (line == SettingsSchemaMarker || line == SettingsSchemaMarkerV4)) {
            schemaMarker = true;
            continue;
        }
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') continue;
        const std::size_t equal = line.find('=');
        if (equal == std::string_view::npos) {
            errors.emplace_back("line:" + std::string(line));
            ok = false;
            continue;
        }
        std::string_view key = line.substr(0, equal);
        const std::string_view value = line.substr(equal + 1);
        while (!key.empty() && ProtectionSpace(key.back())) key.remove_suffix(1);
        const auto result = ApplySetting(candidate, key, value);
        if (!result.accepted) {
            errors.emplace_back(std::string(key) + ":" + result.reason);
            ok = false;
        } else if (key == SettingsKeyMaxItems) {
            fileMaxItems = true;
        }
    }
    if (!ok) return false;
    // D2: one-time migration of a file written before v0.0.4. The next
    // save writes the marker, so the migration never runs twice.
    if (!schemaMarker && fileMaxItems && candidate.maxItems == SettingsMaxItemsLegacyCeiling) {
        candidate.maxItems = SettingsMaxItemsCeiling;
    }
    out = std::move(candidate);
    return true;
}

// w-probe-r29 (B-002 of a review): the atomic save and
// readback of the user settings file, in one engine-free place so the host
// harness drives exactly the bytes and the failures the runtime sees.
//
// Order (R29-2, unchanged): serialize -> write "<target>.tmp" beside the target
// -> rename over the target -> read the target back and parse it. `saved` is
// true only when the rename completed, and `readbackOk` only when the parsed
// file equals `settings` field by field (SameSettings). `detail` is the text
// GetLastSaveError reports: "none" on success, "save_failed" when the write or
// the rename did not complete, "readback_mismatch" when the file was written
// but did not read back as intended. A failed save never touches `settings`,
// so the caller's live snapshot is the previous one by construction.
inline constexpr const char* SettingsSaveDetailNone = "none";
inline constexpr const char* SettingsSaveDetailFailed = "save_failed";
inline constexpr const char* SettingsSaveDetailReadback = "readback_mismatch";

struct SettingsSaveResult {
    bool saved{false};
    bool readbackOk{false};
    const char* detail{SettingsSaveDetailNone};
};

inline SettingsSaveResult SaveSettingsToFile(const Settings& settings,
                                             const std::filesystem::path& target) {
    SettingsSaveResult result;
    std::filesystem::path temp = target;
    temp += ".tmp";
    const std::string text = SerializeSettings(settings);
    bool wrote = false;
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (out) {
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
            wrote = out.good();
        }
    }
    std::error_code failure;
    if (wrote) {
        std::filesystem::rename(temp, target, failure);
        if (failure) {
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
        }
    } else {
        // No half-written settings file is left behind: the temp path is
        // removed even when the open itself failed.
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        failure = std::make_error_code(std::errc::io_error);
    }
    result.saved = wrote && !failure;
    if (!result.saved) {
        result.detail = SettingsSaveDetailFailed;
        return result;
    }
    bool readbackOk = false;
    {
        std::ifstream back(target, std::ios::binary);
        if (back) {
            const std::string readText{std::istreambuf_iterator<char>(back),
                                       std::istreambuf_iterator<char>()};
            Settings readback;
            std::vector<std::string> errors;
            readbackOk = ParseSettings(readText, readback, errors) &&
                         SameSettings(readback, settings);
        }
    }
    result.readbackOk = readbackOk;
    result.detail = readbackOk ? SettingsSaveDetailNone : SettingsSaveDetailReadback;
    return result;
}

// R29-3: the selector thresholds of one settings snapshot, built in exactly
// one place so Probe.cpp and the host test cannot drift apart.
inline SelectorThresholds SelectorThresholdsFromSettings(const Settings& settings) noexcept {
    SelectorThresholds thresholds;
    thresholds.maxValue = settings.maxValue;
    thresholds.minWeight = settings.minWeight;
    thresholds.maxValueWeight = settings.maxValueWeight;
    thresholds.maxItems = settings.maxItems;
    thresholds.sellIngredients = settings.sellIngredients;
    thresholds.junkValueOnly = settings.junkValueOnly;
    thresholds.junkMaxValue = settings.junkMaxValue;
    thresholds.sellStolen = settings.sellStolen;
    thresholds.sellWeakWeapons = settings.sellWeakWeapons;
    thresholds.sellWeakArmor = settings.sellWeakArmor;
    thresholds.sellClothing = settings.sellClothing;
    thresholds.sellEnchantedLoot = settings.sellEnchantedLoot;
    thresholds.enchantedLootMaxValue = settings.enchantedLootMaxValue;
    thresholds.knownEnchantmentsOnly = settings.knownEnchantmentsOnly;
    // v0.4.0: the keep-limit categories.
    thresholds.sellIngredientsAnywhere = settings.keepOn[kKeepIndexIngredients];
    thresholds.sellCookedFood = settings.keepOn[kKeepIndexCookedFood];
    thresholds.sellRawFood = settings.keepOn[kKeepIndexRawFood];
    thresholds.sellDrinks = settings.keepOn[kKeepIndexDrinks];
    thresholds.sellOre = settings.keepOn[kKeepIndexOre];
    thresholds.sellGems = settings.keepOn[kKeepIndexGems];
    thresholds.sellPelts = settings.keepOn[kKeepIndexPelts];
    thresholds.sellTools = settings.keepOn[kKeepIndexTools];
    thresholds.keepIngredients = settings.keepLimit[kKeepIndexIngredients];
    thresholds.keepCookedFood = settings.keepLimit[kKeepIndexCookedFood];
    thresholds.keepRawFood = settings.keepLimit[kKeepIndexRawFood];
    thresholds.keepDrinks = settings.keepLimit[kKeepIndexDrinks];
    thresholds.keepOre = settings.keepLimit[kKeepIndexOre];
    thresholds.keepGems = settings.keepLimit[kKeepIndexGems];
    thresholds.keepPelts = settings.keepLimit[kKeepIndexPelts];
    thresholds.keepTools = settings.keepLimit[kKeepIndexTools];
    // v0.5.0: potions and poisons.
    thresholds.sellRestorePotions = settings.keepOn[kKeepIndexRestorePotions];
    thresholds.sellSkillPotions = settings.keepOn[kKeepIndexSkillPotions];
    thresholds.sellOtherPotions = settings.keepOn[kKeepIndexOtherPotions];
    thresholds.sellPoisons = settings.keepOn[kKeepIndexPoisons];
    thresholds.keepRestorePotions = settings.keepLimit[kKeepIndexRestorePotions];
    thresholds.keepSkillPotions = settings.keepLimit[kKeepIndexSkillPotions];
    thresholds.keepOtherPotions = settings.keepLimit[kKeepIndexOtherPotions];
    thresholds.keepPoisons = settings.keepLimit[kKeepIndexPoisons];
    // v0.6.0: mid-tier gear, high-tier gear and jewelry.
    thresholds.sellMidGear = settings.keepOn[kKeepIndexMidGear];
    thresholds.sellHighGear = settings.keepOn[kKeepIndexHighGear];
    thresholds.sellJewelry = settings.keepOn[kKeepIndexJewelry];
    thresholds.keepMidGear = settings.keepLimit[kKeepIndexMidGear];
    thresholds.keepHighGear = settings.keepLimit[kKeepIndexHighGear];
    thresholds.keepJewelry = settings.keepLimit[kKeepIndexJewelry];
    return thresholds;
}

// The live `Settings` as the lists the rule path matches against. Pure, so the
// host test drives the same conversion Probe.cpp uses.
inline ProtectedNames BuildProtectedNames(const Settings& settings) noexcept {
    ProtectedNames names;
    for (const auto& item : settings.protectedNames) {
        if (!names.Add(item.data(), item.size())) return ProtectedNames{};
    }
    return names;
}

inline ProtectedFormKeys BuildProtectedFormKeys(const Settings& settings) noexcept {
    ProtectedFormKeys keys;
    for (const auto& item : settings.protectedForms) {
        if (!keys.Add(item.data(), item.size())) return ProtectedFormKeys{};
    }
    return keys;
}

// R29-3: one reason, fixed priority after OK. `runtimeOk` is the plugin's
// runtime/version check (Plugin.cpp:18-19, decided before the module loads),
// `adapterProven` is the r10 sale gate's own readiness (Probe.cpp:1791-1793
// feeding WIssue.h:493-494), `apiMismatch` is set by the CheckAPI native.
inline EffectiveStateResult EffectiveState(const Settings& settings, bool runtimeOk,
                                           bool adapterProven, bool configCorrupt,
                                           bool apiMismatch) noexcept {
    if (!settings.enabled) return {false, EffectiveReasonDisabled};
    if (!runtimeOk) return {false, EffectiveReasonRuntimeMismatch};
    if (configCorrupt) return {false, EffectiveReasonConfigCorrupt};
    if (!adapterProven) return {false, EffectiveReasonAdapterUnproven};
    if (apiMismatch) return {false, EffectiveReasonApiMismatch};
    return {true, EffectiveReasonOk};
}

// The fixed reason set the parser pins (RULE-12).
inline bool IsEffectiveReason(std::string_view reason) noexcept {
    return reason == EffectiveReasonOk || reason == EffectiveReasonDisabled ||
           reason == EffectiveReasonRuntimeMismatch || reason == EffectiveReasonAdapterUnproven ||
           reason == EffectiveReasonConfigCorrupt || reason == EffectiveReasonApiMismatch;
}

}  // namespace js
