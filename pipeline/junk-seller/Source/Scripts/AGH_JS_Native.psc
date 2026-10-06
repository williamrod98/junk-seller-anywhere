Scriptname AGH_JS_Native Hidden

; w-probe-r28 (R29-1/R29-2/R29-3): the D-W11 settings contract of
; AGH_JunkSeller.dll. The declarations here and the RegisterFunction list in
; native/src/PapyrusBridge.cpp stay exact so the PEX can call each surface; the
; StaticFunctionTag base argument is not a PSC-visible parameter.
;
; API 2 (design-r2 4.2): adds GetSellIngredients/SetSellIngredients.
; API 3 (v0.1.0, the v0.1.0 design r3 section 7): adds the sell
; anywhere switch, the three category switches (food, misc, spare gear),
; GetAnywhereRatePercent and GetLastMerchantCheck.
; API 4 (v0.2.0, the v0.2 update plan r2 section 4.2):
; retires the three category switches of API 3 (food, misc, spare gear) and
; adds the Categories page (junk rule, junk max value, include stolen items,
; weak weapons, weak armor, clothing) and GetLastKept.
; API 5 (v0.3.0, the v0.2 update plan r2 section 4.2): adds weak enchanted
; loot (Get/SetSellEnchantedLoot, Get/SetEnchantedLootMaxValue,
; Get/SetKnownEnchantmentsOnly).
; API 6 (v0.4.0, the v0.2 update plan r2 section 4.2): adds the eight
; keep-limit categories through four indexed natives (Get/SetKeepCategory,
; Get/SetKeepLimit).
; API 7 (v0.5.0, the v0.2 update plan r2 section 4.2): the same four
; natives over twelve rows; indices 8..11 are restore potions, skill potions,
; other potions and poisons. No native is added.
; API 8 (v0.6.0, the v0.2 update plan r2 section 4.2): the same four
; natives over fifteen rows; indices 12..14 are mid-tier gear, high-tier gear
; and jewelry. No native is added.
; API 9 (v0.8.0, the barter-price design r2 sections 3.4
; and 4): adds "Price follows perks" (Get/SetPerkPrice), the main-thread
; "Rate now" preview (RequestRatePreview, GetRatePreview) and
; GetLastPriceSource. GetAnywhereRatePercent stays (the vanilla formula).
; A caller still built against API 1 never calls either: it compares GetAPI()
; against its own ExpectedApi() first (AGH_JS_MCM.psc Available()) and stops
; there.
;
; Every Set* refuses (returns false) with no state change when the value is
; unknown, unparseable or out of range, and also when the file could not be
; saved and read back (R29-2). GetLastSaveError names the last such failure
; ("none" when there is none).

; The live native API version (4). A caller asks GetAPI first, so a missing or
; mismatched DLL never reaches another native.
Int Function GetAPI() Global Native

; The effective reason token: OK, DISABLED, RUNTIME_MISMATCH, ADAPTER_UNPROVEN,
; CONFIG_CORRUPT or API_MISMATCH (R29-3).
String Function GetStatus() Global Native

; False whenever GetStatus() is not OK.
Bool Function IsEffectivelyEnabled() Global Native

; The declared runtime, "1.6.1170.0 + SKSE 2.2.6.0".
String Function GetRuntime() Global Native

; The release build id embedded in BuildInfo.h.
String Function GetReleaseId() Global Native

; (design-r2 section 8, S2e): the product version embedded in
; BuildInfo.h, e.g. "0.0.4". The MCM Diagnostics page names it instead of a
; hard-coded literal.
String Function GetVersion() Global Native

; The atomic generation the last successful save produced (R29-2).
Int Function GetConfigGeneration() Global Native

; The CheckAPI of a caller that expected another API sets the API_MISMATCH
; reason; true means this caller's expected API is the live one.
Bool Function CheckAPI(Int aiExpected) Global Native

; The master switch; the plugin default is ON.
Bool Function GetEnabled() Global Native
Bool Function SetEnabled(Bool abValue) Global Native

Bool Function GetConfirmBeforeSell() Global Native
Bool Function SetConfirmBeforeSell(Bool abValue) Global Native

; (design-r2 4.2/4.3): default OFF; ON is written to the user file, OFF
; never is, so a file from a build before API 2 loads as OFF.
Bool Function GetSellIngredients() Global Native
Bool Function SetSellIngredients(Bool abValue) Global Native

; v0.1.0: default OFF; ON is written to the user file, OFF never is.
; SellAnywhere: the same key also sells when no menu is open (direct
; conversion, new gold, no buy-back).
Bool Function GetSellAnywhere() Global Native
Bool Function SetSellAnywhere(Bool abValue) Global Native

; v0.2.0: the Categories page (sell anywhere only). Every switch is OFF
; by default and a key is written only when it differs from the default.
; JunkValueOnly: the junk rule "Value only" (value <= JunkMaxValue, weight and
; ratio dropped); OFF = Classic (the value / weight thresholds of the Settings
; page). JunkMaxValue: 0..200, default 50. SellStolen: "Include stolen items".
; SellWeakWeapons / SellWeakArmor / SellClothing: sold whole.
Bool Function GetJunkValueOnly() Global Native
Bool Function SetJunkValueOnly(Bool abValue) Global Native
Int Function GetJunkMaxValue() Global Native
Bool Function SetJunkMaxValue(Int aiValue) Global Native
Bool Function GetSellStolen() Global Native
Bool Function SetSellStolen(Bool abValue) Global Native
Bool Function GetSellWeakWeapons() Global Native
Bool Function SetSellWeakWeapons(Bool abValue) Global Native
Bool Function GetSellWeakArmor() Global Native
Bool Function SetSellWeakArmor(Bool abValue) Global Native
Bool Function GetSellClothing() Global Native
Bool Function SetSellClothing(Bool abValue) Global Native

; v0.3.0: weak enchanted loot (sell anywhere only), sold whole.
; SellEnchantedLoot: OFF by default. EnchantedLootMaxValue: 0..2000, default
; 300, the jewelry / clothing bound for enchantments off the vanilla 01-06
; ladder. KnownEnchantmentsOnly: ON by default (an item whose enchantment the
; player has not learned stays); written only when OFF.
Bool Function GetSellEnchantedLoot() Global Native
Bool Function SetSellEnchantedLoot(Bool abValue) Global Native
Int Function GetEnchantedLootMaxValue() Global Native
Bool Function SetEnchantedLootMaxValue(Int aiValue) Global Native
Bool Function GetKnownEnchantmentsOnly() Global Native
Bool Function SetKnownEnchantmentsOnly(Bool abValue) Global Native

; v0.4.0: the keep-limit categories (sell anywhere only), by index:
; 0 alchemy ingredients (keep 0-2000, default 500), 1 cooked food (0-500, 10),
; 2 raw food (0-500, 20), 3 drinks and quest food (0-500, 10; a quest food
; keeps its floor even at 0), 4 ore and ingots (0-1000, 50), 5 gems (0-1000,
; 50), 6 pelts (0-1000, 100), 7 tools (0-20, 1); v0.5.0: 8 restore and
; cure potions (0-1000, 100 per effect pool), 9 skill potions (0-1000, 100 per
; skill pool), 10 other potions (0-500, 10), 11 poisons (0-500, 10); 
; v0.6.0: 12 mid-tier gear (0-100, 10), 13 high-tier gear (0-100, 10), 14
; jewelry (0-100, 10). Every switch is OFF by default; the keep is per kind
; (per pool for 8 and 9) and 0 sells all. An index outside 0..14 reads OFF / 0
; and every Set refuses it.
Bool Function GetKeepCategory(Int aiIndex) Global Native
Bool Function SetKeepCategory(Int aiIndex, Bool abValue) Global Native
Int Function GetKeepLimit(Int aiIndex) Global Native
Bool Function SetKeepLimit(Int aiIndex, Int aiValue) Global Native

; One "why kept" counter of the last sell-anywhere press: 0 at limit,
; 1 protected, 2 stolen (switch OFF), 3 enchantment not known, 4 never sold;
; -1 before the first press.
Int Function GetLastKept(Int aiIndex) Global Native

; round(100 x m') of the vanilla formula from the live inputs; -1 when the
; inputs cannot be read. Since API 9 the MCM shows GetRatePreview instead.
Int Function GetAnywhereRatePercent() Global Native

; v0.8.0: "Price follows perks". OFF by default (new installs and
; updates); ON is written to the user file, OFF never is. ON: sell anywhere
; pays what the player's own price perks earn at a neutral merchant; OFF: the
; vanilla formula.
Bool Function GetPerkPrice() Global Native
Bool Function SetPerkPrice(Bool abValue) Global Native

; v0.8.0: the "Rate now" preview. RequestRatePreview clears the preview
; and asks the game thread for a new one (False when it could not be queued);
; GetRatePreview answers -2 while it is not ready, -1 when the price cannot be
; read, else round(100 x m') of what a press would pay now. A load clears it.
Bool Function RequestRatePreview() Global Native
Int Function GetRatePreview() Global Native

; v0.8.0: how the last sale (sell anywhere or the barter-menu key) priced:
; 0 your perks, 1 the vanilla formula, 2 the vanilla formula because the perk
; prices could not be read; -1 before a sale since the last load.
Int Function GetLastPriceSource() Global Native

; "<merchant gold> / <sell anywhere gold>" of the last barter-menu batch since
; the last load; "" before one.
String Function GetLastMerchantCheck() Global Native

; Scancode; 0 means unassigned.
Int Function GetHotkey() Global Native
Bool Function SetHotkey(Int aiValue) Global Native

Int Function GetMaxValue() Global Native
Bool Function SetMaxValue(Int aiValue) Global Native

Float Function GetMinWeight() Global Native
Bool Function SetMinWeight(Float afValue) Global Native

Float Function GetMaxValueWeight() Global Native
Bool Function SetMaxValueWeight(Float afValue) Global Native

; 1..256 (design-r2 section 5, D2); a value above the accepted selector
; cap is refused, not clamped.
Int Function GetMaxItems() Global Native
Bool Function SetMaxItems(Int aiValue) Global Native

; One `|`-joined string each. The file itself keeps the semicolon spelling.
String Function GetProtectedNames() Global Native
Bool Function SetProtectedNames(String asValue) Global Native

String Function GetProtectedForms() Global Native
Bool Function SetProtectedForms(String asValue) Global Native

; The text of the last save failure ("none" when the last save succeeded).
String Function GetLastSaveError() Global Native

; One line from the last js_rule_run values (gateReason, settingsReason,
; candidatesRead, issued, confirmed, terminal reasons); "none" before the first
; run. The r29 Diagnostics page reads this instead of parsing a receipt.
String Function GetLastRunSummary() Global Native
