Scriptname AGH_JS_MCM extends SKI_ConfigBase

; w-probe-r29 (design rulings R30-1/R30-2, design section 9 D-W11): the MCM of
; the junk-seller observation probe. Two pages. The Settings page has exactly
; one row per v0.1 key in the order of the design notes
; section 3.3: Enabled, Mode, ConfirmBeforeSell, Hotkey, MaxValue, MinWeight,
; MaxValueWeight, MaxItems, ProtectedNames, ProtectedForms, plus the 
; SellIngredients toggle (design-r2 4.2, default OFF) and, in a second
; column, the v0.1.0 sell-anywhere rows (see below). The Diagnostics
; page is read only: the runtime, the effective ON/OFF with its reason token,
; the atomic config generation and the last run line.
;
; Every user-facing sentence is a $AGH_JS_* translation key of
; Interface/Translations/AGH_JunkSeller_english.txt / _japanese.txt. The only
; literals in this file are the DLL's own fixed value tokens (OK, OFF, Hotkey
; and the gate / effective reason tokens), which by design stay untranslated:
; they are identifiers a user compares against the receipt, not sentences.
;
; Display rule (the family rule the same way in
; pipeline/uncapper/Source/Scripts/AGH_UC_MCM.psc): SkyUI draws a text option
; as label left and value right in one row, so a value holds at most 24 Latin
; or 16 Japanese characters. Every value column below therefore goes through
; ValueColumnText()/ListText()/RuntimeText()/EffectiveText()/LastRunGateText();
; a value the bound would cut is shortened to a marked prefix here (the
; trailing "~" is the fixed truncation marker, not a sentence), and
; OnOptionHighlight returns the complete value into the SkyUI info panel
; whenever the bound did cut it -- the audit A-001/A-002 fix of
; a review. Every longer sentence (the help texts and
; the GetLastRunSummary() line) already lives in that info panel.
;
; Label rule (w-probe-r33, finding A of a live test log and
; ruling R35-1): the label column is narrower than the value column -- R-W25b
; measured the 24-character Japanese hotkey label overrunning its own value --
; so every label key of the two translation files holds at most 10 Japanese or
; 20 Latin characters, and a qualifier that no longer fits moves into that
; row's _HELP sentence, which the info panel already shows. The value column
; keeps the 24/16 bound above, and the last-run row carries the short
; $AGH_JS_ADAPTER_UNAVAILABLE_SHORT token instead of a sentence (finding B: the
; sentence collided with its own label).
;
; No native call happens when the DLL is absent: Available() checks the plugin
; version first, then GetAPI(), then CheckAPI(); the one row that is left is a
; single enabled info row (SkyUI does not fire OnOptionHighlight over a
; disabled row), and OnPageReset returns before any other native call.
;
; Papyrus string rule (design ruling section 53 R53-2/R53-3): a "$" key inside a
; concatenated string is never substituted by the SkyUI "$TOKEN" scan, which
; only resolves a key that stands alone. No $AGH_JS_* key is ever concatenated
; with anything in this file.
;
; v0.1.0 (the v0.1.0 design r3 sections 1.1, 2.5
; and 7): the Settings page gets a second column for sell anywhere: the
; SellAnywhere toggle (OFF by default; switching it ON asks once through
; ShowMessage with a cancel button, and Cancel leaves it OFF), the read-only
; "Rate now" and "Merchant check" rows, and the three player-facing category
; toggles (food, misc items, spare gear; all OFF by default).
;
; v0.2.0 (the v0.2 update plan r2 section 4.2): a third
; page "Categories" between Settings and Diagnostics, sell anywhere only: the
; junk rule toggle "Junk: value only" (OFF = Classic) with its "Junk max
; value" slider (0-200, step 5, default 50; disabled while Classic), "Include
; stolen items", "Weak weapons", "Weak armor" and "Clothing" (all OFF by
; default; a user preset turns them ON through the user file). The three
; v0.1.0 category toggles are retired. The Settings page puts the value /
; weight thresholds under a "Classic junk rule" header, and the Diagnostics
; page shows the five "why kept" counters of the last press.
; Existing saves get the third page through GetVersion() 2 / OnVersionUpdate.
;
; v0.3.0 (the v0.2 update plan r2 section 4.2): the Categories page gains
; "Weak enchanted loot" (OFF by default) with its "Jewelry max value" slider
; (0-2000, step 50, default 300; disabled while the toggle is OFF) and "Known
; enchants only" (ON by default); the Diagnostics "Enchant not known" counter
; is live. The pages are unchanged, so the menu version stays 2.
;
; v0.4.0 (the v0.2 update plan r2 section 4.2, keep-N): the Categories
; page gains a "Keep per kind" block of eight rows, each a toggle left and its
; keep slider right (0 = sell all; the slider is disabled while its toggle is
; OFF): alchemy ingredients, cooked food, raw food, drinks and quest food,
; ore and ingots, gems, pelts, tools, all OFF by default. The rows come from
; the four indexed natives of API 6 (Get/SetKeepCategory, Get/SetKeepLimit);
; each label, help, range and step is a literal per index below (no key is
; concatenated). The pages are unchanged, so the menu version stays 2.
;
; v0.5.0 (the v0.2 update plan r2 section 4.2, potions and poisons): the
; block grows to twelve rows: restore and cure potions (keep per effect pool,
; 0-1000 step 5, default 100), skill potions (keep per skill pool, 0-1000
; step 5, default 100), other potions (keep per kind, 0-500 step 1, default
; 10) and poisons (keep per kind, 0-500 step 1, default 10), all OFF by
; default, through the same four natives at indices 8..11 (API 7).
;
; v0.6.0 (the v0.2 update plan r2 section 4.2, mid / high gear, jewelry):
; the block grows to fifteen rows: mid-tier gear, high-tier gear and jewelry
; (keep per kind, 0-100 step 1, default 10 each), all OFF by default,
; through the same four natives at indices 12..14 (API 8).
;
; v0.8.0 (the barter-price design r2 sections 3.4 and 4):
; the Settings page's sell-anywhere column gains "Price follows perks" (OFF by
; default) under the "Sell anywhere" toggle. "Rate now" is a preview the DLL
; computes on the game thread: OnConfigOpen asks for it (RequestRatePreview),
; the Settings page waits for it at most 20 x 0.05 s before it adds the row,
; and a row that still says "reading..." is updated once, on the next option
; highlight (SkyUI refuses option writes inside OnPageReset and runs no timer
; in menu mode). Toggling "Price follows perks" asks again and updates the row
; the same way. The Diagnostics page gains "Last price source" (your perks /
; vanilla formula / vanilla fallback), kept until the next sale or a load and
; never overwritten by the preview (API 9).
;
; The whole surface below the row handlers comes from AGH_JS_Native, whose
; GetAPI() is 9 for this revision (design-r2 4.2 added
; GetSellIngredients/SetSellIngredients; v0.1.0 added the sell-anywhere
; natives; v0.2.0 the Categories page; API 5, v0.3.0, weak
; enchanted loot; API 6, v0.4.0, the keep-limit categories; API 7,
; v0.5.0, twelve keep rows; API 8, v0.6.0, fifteen keep rows; API
; 9, v0.8.0, the perk price, its preview and the last price source); a Set*
; returns true only after the
; user file was written and read back, so a false makes the row show the value
; the storage still holds and puts GetLastSaveError() into the info panel.

Int unavailableOption = -1
; v0.9.0 (w-probe-r71, E1 the E1 design 2.5):
; the option ids of the two inactive status rows (-1 = not on the current
; page), reset at every page reset.
Int inactiveStatusOption = -1
Int inactiveValueOption = -1
Int inactiveDetailsOption = -1
Int inactiveLogOption = -1
; v0.9.0: the once-per-load HUD line. This MCM quest has no player alias,
; so SkyUI's OnGameReload runs once at quest start, never per load (the SR
; v1.9.0 finding). A game load restores this script's variables from the
; save, where loadScreenOpen is False (OnMenuClose clears it), while a door's
; loading screen keeps the True set by its own OnMenuOpen. So a Loading Menu
; close that finds it False follows a game load.
Bool loadScreenOpen = False
Bool loadWatchOn = False
Int enabledOption = -1
Int modeOption = -1
Int confirmOption = -1
Int hotkeyOption = -1
Int maxValueOption = -1
Int minWeightOption = -1
Int maxValueWeightOption = -1
Int maxItemsOption = -1
Int sellIngredientsOption = -1
Int sellAnywhereOption = -1
Int perkPriceOption = -1
Int anywhereRateOption = -1
Int anywhereCheckOption = -1
Int classicHeaderOption = -1
Int categoriesHeaderOption = -1
Int junkValueOnlyOption = -1
Int junkMaxValueOption = -1
Int sellStolenOption = -1
Int sellWeakWeaponsOption = -1
Int sellWeakArmorOption = -1
Int sellClothingOption = -1
Int sellEnchantedLootOption = -1
Int enchantedLootMaxValueOption = -1
Int knownEnchantmentsOnlyOption = -1
Int[] keptOptions
Int keepHeaderOption = -1
Int[] keepToggleOptions
Int[] keepSliderOptions
Int protectedNamesOption = -1
Int protectedFormsOption = -1
Int runtimeOption = -1
Int effectiveOption = -1
Int generationOption = -1
Int lastRunOption = -1
Int priceSourceOption = -1

Bool currentEnabled = False
Bool currentConfirmBeforeSell = False
Int currentHotkey = 0
Int currentMaxValue = 0
Float currentMinWeight = 0.0
Float currentMaxValueWeight = 0.0
Int currentMaxItems = 1
Bool currentSellIngredients = False
Bool currentSellAnywhere = False
Bool currentPerkPrice = False
Bool currentJunkValueOnly = False
Int currentJunkMaxValue = 50
Bool currentSellStolen = False
Bool currentSellWeakWeapons = False
Bool currentSellWeakArmor = False
Bool currentSellClothing = False
Bool currentSellEnchantedLoot = False
Int currentEnchantedLootMaxValue = 300
Bool currentKnownEnchantmentsOnly = True
Int[] currentKept
Bool[] currentKeepOn
Int[] currentKeepLimit
Int currentAnywhereRate = -1
; v0.8.0: the "Rate now" row was added while its preview was pending,
; and this open already asked for its preview.
Bool rateRowPending = False
Bool ratePreviewAsked = False
Int currentLastPriceSource = -1
String currentMerchantCheck = ""
String currentProtectedNames = ""
String currentProtectedForms = ""
String currentRuntime = ""
String currentReleaseId = ""
Bool currentEffective = False
String currentReason = ""
Int currentGeneration = 0
String currentLastRun = ""

Int Function ExpectedApi()
    Return 9
EndFunction

; v0.2.0: version 2 of this menu has the Categories page; SkyUI calls
; OnVersionUpdate once on a save that registered an older version.
Int Function GetVersion()
    Return 2
EndFunction

Function SetPages()
    Pages = New String[3]
    Pages[0] = "$AGH_JS_SETTINGS"
    Pages[1] = "$AGH_JS_CATEGORIES"
    Pages[2] = "$AGH_JS_DIAGNOSTICS"
EndFunction

Event OnVersionUpdate(Int aiVersion)
    If aiVersion >= 2
        SetPages()
    EndIf
EndEvent

Bool Function Available()
    If SKSE.GetPluginVersion("AGH_JunkSeller") <= 0
        Return False
    EndIf
    If AGH_JS_Native.GetAPI() != ExpectedApi()
        Return False
    EndIf
    Return AGH_JS_Native.CheckAPI(ExpectedApi())
EndFunction

; v0.9.0 (E1 design 2.5): SKSE lists the DLL but it is not running. Its
; runtime gate refused this game setup, or a start step failed; Papyrus cannot
; tell which, the receipt (SKSE/AGH_JunkSeller.jsonl) names it. A refused DLL
; binds no native (GetAPI reads 0), a failed one returns 0 from GetAPI.
Bool Function IsNativeInactive()
    If SKSE.GetPluginVersion("AGH_JunkSeller") <= 0
        Return False
    EndIf
    Return AGH_JS_Native.GetAPI() == 0
EndFunction

; The two status rows in the two-cell layout (name cell, answer cell), instead
; of the settings, on every page. No option acts on them.
Function AddInactiveRows()
    SetCursorFillMode(LEFT_TO_RIGHT)
    inactiveStatusOption = AddTextOption("$AGH_JS_DLL_STATUS", "")
    inactiveValueOption = AddTextOption("$AGH_JS_DLL_INACTIVE", "")
    inactiveDetailsOption = AddTextOption("$AGH_JS_DETAILS", "")
    inactiveLogOption = AddTextOption("SKSE/AGH_JunkSeller.jsonl", "")
EndFunction

; One cause-neutral HUD line, only while the DLL is loaded but not running.
Function NotifyIfInactive()
    If IsNativeInactive()
        Debug.Notification("$AGH_JS_INACTIVE_HUD")
    EndIf
EndFunction

; SkyUI's config manager sends this at every game load (and periodically) to
; every config whose mod event registration the save carries. The first one on
; this save starts the Loading Menu watch and runs the check once (a new game,
; or the first load of an older save, where no load was watched yet).
Event OnConfigManagerReady(String a_eventName, String a_strArg, Float a_numArg, Form a_sender)
    Parent.OnConfigManagerReady(a_eventName, a_strArg, a_numArg, a_sender)
    If !loadWatchOn
        loadWatchOn = True
        RegisterForMenu("Loading Menu")
        NotifyIfInactive()
    EndIf
EndEvent

Event OnMenuOpen(String menuName)
    If menuName == "Loading Menu"
        loadScreenOpen = True
    EndIf
EndEvent

Event OnMenuClose(String menuName)
    If menuName == "Loading Menu"
        If !loadScreenOpen
            NotifyIfInactive()
        EndIf
        loadScreenOpen = False
    EndIf
EndEvent

Function Refresh()
    If !Available()
        Return
    EndIf
    currentEnabled = AGH_JS_Native.GetEnabled()
    currentConfirmBeforeSell = AGH_JS_Native.GetConfirmBeforeSell()
    currentHotkey = AGH_JS_Native.GetHotkey()
    currentMaxValue = AGH_JS_Native.GetMaxValue()
    currentMinWeight = AGH_JS_Native.GetMinWeight()
    currentMaxValueWeight = AGH_JS_Native.GetMaxValueWeight()
    currentMaxItems = AGH_JS_Native.GetMaxItems()
    currentSellIngredients = AGH_JS_Native.GetSellIngredients()
    currentSellAnywhere = AGH_JS_Native.GetSellAnywhere()
    currentPerkPrice = AGH_JS_Native.GetPerkPrice()
    currentJunkValueOnly = AGH_JS_Native.GetJunkValueOnly()
    currentJunkMaxValue = AGH_JS_Native.GetJunkMaxValue()
    currentSellStolen = AGH_JS_Native.GetSellStolen()
    currentSellWeakWeapons = AGH_JS_Native.GetSellWeakWeapons()
    currentSellWeakArmor = AGH_JS_Native.GetSellWeakArmor()
    currentSellClothing = AGH_JS_Native.GetSellClothing()
    currentSellEnchantedLoot = AGH_JS_Native.GetSellEnchantedLoot()
    currentEnchantedLootMaxValue = AGH_JS_Native.GetEnchantedLootMaxValue()
    currentKnownEnchantmentsOnly = AGH_JS_Native.GetKnownEnchantmentsOnly()
    If currentKept.Length != 5
        currentKept = New Int[5]
    EndIf
    Int keptIndex = 0
    While keptIndex < 5
        currentKept[keptIndex] = AGH_JS_Native.GetLastKept(keptIndex)
        keptIndex += 1
    EndWhile
    ; v0.4.0: the keep-limit categories (twelve since v0.5.0, fifteen
    ; since v0.6.0).
    If currentKeepOn.Length != 15
        currentKeepOn = New Bool[15]
    EndIf
    If currentKeepLimit.Length != 15
        currentKeepLimit = New Int[15]
    EndIf
    Int keepIndex = 0
    While keepIndex < 15
        currentKeepOn[keepIndex] = AGH_JS_Native.GetKeepCategory(keepIndex)
        currentKeepLimit[keepIndex] = AGH_JS_Native.GetKeepLimit(keepIndex)
        keepIndex += 1
    EndWhile
    ; v0.8.0: the preview of this open (-2 while it is not ready).
    currentAnywhereRate = AGH_JS_Native.GetRatePreview()
    currentLastPriceSource = AGH_JS_Native.GetLastPriceSource()
    currentMerchantCheck = AGH_JS_Native.GetLastMerchantCheck()
    currentProtectedNames = AGH_JS_Native.GetProtectedNames()
    currentProtectedForms = AGH_JS_Native.GetProtectedForms()
    currentRuntime = AGH_JS_Native.GetRuntime()
    currentReleaseId = AGH_JS_Native.GetReleaseId()
    currentEffective = AGH_JS_Native.IsEffectivelyEnabled()
    currentReason = AGH_JS_Native.GetStatus()
    currentGeneration = AGH_JS_Native.GetConfigGeneration()
    currentLastRun = AGH_JS_Native.GetLastRunSummary()
EndFunction

; SkyUI's value column holds at most 24 Latin or 16 Japanese characters. A
; Japanese (wide) glyph takes two of those columns, so a value that holds any
; character above 127 is held to 16 characters and an all-ASCII value to 24.
; No value column below is written without one of these three helpers.
Int Function ValueColumnWidth(String asValue)
    Int index = 0
    Int count = StringUtil.GetLength(asValue)
    While index < count
        If StringUtil.AsOrd(StringUtil.GetNthChar(asValue, index)) > 127
            Return 16
        EndIf
        index += 1
    EndWhile
    Return 24
EndFunction

Bool Function ValueColumnFits(String asValue)
    Return StringUtil.GetLength(asValue) <= ValueColumnWidth(asValue)
EndFunction

; The value column form of asValue. A value that does not fit keeps its leading
; text and gets the fixed "~" truncation marker, so a shortened row can never
; read as if it were complete.
String Function ValueColumnText(String asValue)
    Int width = ValueColumnWidth(asValue)
    If StringUtil.GetLength(asValue) <= width
        Return asValue
    EndIf
    Return StringUtil.Substring(asValue, 0, width - 1) + "~"
EndFunction

; The Diagnostics runtime value column. GetRuntime() is
; "<game version> + SKSE <SKSE version>" -- 25 characters for the declared
; 1.6.1170.0 / SKSE 2.2.6.0 -- which is past the bound above, so the column
; shows the game version alone (the runtime check the DLL actually performs)
; and OnOptionHighlight returns the complete string.
;
; (design-r2 section 8, S2e): the version comes from GetVersion()
; (BuildInfo.h) instead of a literal, so a version bump needs no psc edit and
; the static text checks never see a stale number here. The
; Available() re-check is the same "GetAPI first" pattern Refresh() and every
; other native call in this file already follow; it is cheap (one page build)
; and gives a safe fallback when the DLL is missing.
String Function RuntimeText()
    If !Available()
        Return "AGH Junk Seller"
    EndIf
    Return "AGH Junk Seller v" + AGH_JS_Native.GetVersion()
EndFunction

; The Diagnostics value column of the effective row: the fixed OK / OFF token
; pair. The reason behind an OFF is the DLL's own token and goes to the info
; panel (OnOptionHighlight), never into the value column.
String Function EffectiveText()
    If currentEffective
        Return "OK"
    EndIf
    Return "OFF"
EndFunction

; The settings reason is a named field in GetLastRunSummary(), so the MCM can
; distinguish gate=SETTINGS_OFF settings=ADAPTER_UNPROVEN from other gates.
String Function LastRunSettingsText()
    If currentLastRun == "" || currentLastRun == "none"
        Return ""
    EndIf
    Int start = StringUtil.Find(currentLastRun, "settings=")
    If start < 0
        Return ""
    EndIf
    start += 9
    Int stop = StringUtil.Find(currentLastRun, " ", start)
    If stop < 0
        stop = StringUtil.GetLength(currentLastRun)
    EndIf
    If stop <= start
        Return ""
    EndIf
    Return StringUtil.Substring(currentLastRun, start, stop - start)
EndFunction

; The Diagnostics value column of the last-run row: the gate reason token of
; the last run, or $AGH_JS_NONE before the first run. An adapter refusal is
; shown with the translated adapter message in the value column, while the
; complete named summary remains available in the info panel.
String Function LastRunGateText()
    If currentLastRun == "" || currentLastRun == "none"
        Return "$AGH_JS_NONE"
    EndIf
    Int start = StringUtil.Find(currentLastRun, "gate=")
    If start < 0
        Return "$AGH_JS_NONE"
    EndIf
    start += 5
    Int stop = StringUtil.Find(currentLastRun, " ", start)
    If stop < 0
        stop = StringUtil.GetLength(currentLastRun)
    EndIf
    If stop <= start
        Return "$AGH_JS_NONE"
    EndIf
    Return StringUtil.Substring(currentLastRun, start, stop - start)
EndFunction

; w-probe-r33 (finding B): the value column of the last-run row. An adapter
; refusal shows the short $AGH_JS_ADAPTER_UNAVAILABLE_SHORT token -- the
; translated sentence it used to hold was wider than the value column and
; collided with the label -- while the complete sentence and the native's
; named summary stay in the info panel below.
String Function LastRunValueText()
    If currentReason == "AWAITING_MENU"
        Return "$AGH_JS_AWAITING_MENU_SHORT"
    ElseIf currentReason == "ADAPTER_UNPROVEN" || LastRunSettingsText() == "ADAPTER_UNPROVEN"
        Return "$AGH_JS_ADAPTER_UNAVAILABLE_SHORT"
    EndIf
    Return ValueColumnText(LastRunGateText())
EndFunction

; The two list rows show the native's "|"-joined string, or $AGH_JS_NONE when
; the list is empty. The value column is bounded like every other one, and
; OnOptionHighlight returns the complete list whenever the bound cut it (audit
; A-002). Both rows are read only: the natives own the spelling, and the user
; ini is where a list is edited (the help text names that file).
String Function ListText(String asValue)
    If asValue == ""
        Return "$AGH_JS_NONE"
    EndIf
    Return ValueColumnText(asValue)
EndFunction

; v0.1.0: the value column of the "Rate now" row: the percent the DLL
; computed from the live inputs, or $AGH_JS_NONE when it could not read them.
; v0.8.0: $AGH_JS_RATE_PENDING while the preview is not ready (-2).
String Function AnywhereRateText()
    If currentAnywhereRate == -2
        Return "$AGH_JS_RATE_PENDING"
    EndIf
    If currentAnywhereRate < 0
        Return "$AGH_JS_NONE"
    EndIf
    Return ValueColumnText((currentAnywhereRate As String) + " %")
EndFunction

; v0.8.0 (the barter-price design r2 section 3.4 step 2): wait for the
; preview of this request, at most 20 x 0.05 s (menu-mode time).
Function WaitRatePreview()
    Int polls = 0
    currentAnywhereRate = AGH_JS_Native.GetRatePreview()
    While currentAnywhereRate == -2 && polls < 20
        Utility.WaitMenuMode(0.05)
        currentAnywhereRate = AGH_JS_Native.GetRatePreview()
        polls += 1
    EndWhile
EndFunction

; v0.8.0: the one late update of a "Rate now" row added while pending,
; outside OnPageReset (SkyUI refuses option writes inside it).
Function UpdatePendingRate()
    If !rateRowPending || anywhereRateOption < 0
        Return
    EndIf
    currentAnywhereRate = AGH_JS_Native.GetRatePreview()
    If currentAnywhereRate != -2
        rateRowPending = False
        SetTextOptionValue(anywhereRateOption, AnywhereRateText())
    EndIf
EndFunction

; v0.8.0: the value column of the Diagnostics "Last price source" row.
String Function PriceSourceText()
    If currentLastPriceSource == 0
        Return "$AGH_JS_PRICESOURCE_PERKS"
    ElseIf currentLastPriceSource == 1
        Return "$AGH_JS_PRICESOURCE_FORMULA"
    ElseIf currentLastPriceSource == 2
        Return "$AGH_JS_PRICESOURCE_FALLBACK"
    EndIf
    Return "$AGH_JS_NONE"
EndFunction

; v0.1.0: the value column of the "Merchant check" row: "<merchant> /
; <sell anywhere>" gold of the last barter-menu batch, or $AGH_JS_NONE.
String Function MerchantCheckText()
    If currentMerchantCheck == ""
        Return "$AGH_JS_NONE"
    EndIf
    Return ValueColumnText(currentMerchantCheck)
EndFunction

; v0.2.0: the value column of one "why kept" row: the count of the last
; press, or $AGH_JS_NONE before the first one.
String Function KeptText(Int aiIndex)
    If currentKept.Length != 5 || currentKept[aiIndex] < 0
        Return "$AGH_JS_NONE"
    EndIf
    Return ValueColumnText(currentKept[aiIndex] As String)
EndFunction

; The junk max value slider only applies with "Junk: value only" ON.
Int Function JunkMaxValueFlags()
    If currentJunkValueOnly
        Return OPTION_FLAG_NONE
    EndIf
    Return OPTION_FLAG_DISABLED
EndFunction

; v0.3.0: the jewelry max value slider only applies with "Weak enchanted
; loot" ON.
Int Function EnchantedLootMaxValueFlags()
    If currentSellEnchantedLoot
        Return OPTION_FLAG_NONE
    EndIf
    Return OPTION_FLAG_DISABLED
EndFunction

; v0.4.0: a keep slider only applies with its category ON.
Int Function KeepSliderFlags(Int aiIndex)
    If currentKeepOn.Length == 15 && currentKeepOn[aiIndex]
        Return OPTION_FLAG_NONE
    EndIf
    Return OPTION_FLAG_DISABLED
EndFunction

; v0.4.0: the toggle label key of keep-limit category aiIndex (the
; Settings.h SettingsKeepCategories order; no key is ever concatenated).
String Function KeepToggleLabel(Int aiIndex)
    If aiIndex == 0
        Return "$AGH_JS_CAT_INGREDIENTS"
    ElseIf aiIndex == 1
        Return "$AGH_JS_CAT_COOKED"
    ElseIf aiIndex == 2
        Return "$AGH_JS_CAT_RAW"
    ElseIf aiIndex == 3
        Return "$AGH_JS_CAT_DRINKS"
    ElseIf aiIndex == 4
        Return "$AGH_JS_CAT_ORE"
    ElseIf aiIndex == 5
        Return "$AGH_JS_CAT_GEMS"
    ElseIf aiIndex == 6
        Return "$AGH_JS_CAT_PELTS"
    ElseIf aiIndex == 8
        Return "$AGH_JS_CAT_RESTORE"
    ElseIf aiIndex == 9
        Return "$AGH_JS_CAT_SKILLPOT"
    ElseIf aiIndex == 10
        Return "$AGH_JS_CAT_OTHERPOT"
    ElseIf aiIndex == 11
        Return "$AGH_JS_CAT_POISONS"
    ElseIf aiIndex == 12
        Return "$AGH_JS_CAT_MIDGEAR"
    ElseIf aiIndex == 13
        Return "$AGH_JS_CAT_HIGHGEAR"
    ElseIf aiIndex == 14
        Return "$AGH_JS_CAT_JEWELRY"
    EndIf
    Return "$AGH_JS_CAT_TOOLS"
EndFunction

String Function KeepSliderLabel(Int aiIndex)
    If aiIndex == 0
        Return "$AGH_JS_KEEP_INGREDIENTS"
    ElseIf aiIndex == 1
        Return "$AGH_JS_KEEP_COOKED"
    ElseIf aiIndex == 2
        Return "$AGH_JS_KEEP_RAW"
    ElseIf aiIndex == 3
        Return "$AGH_JS_KEEP_DRINKS"
    ElseIf aiIndex == 4
        Return "$AGH_JS_KEEP_ORE"
    ElseIf aiIndex == 5
        Return "$AGH_JS_KEEP_GEMS"
    ElseIf aiIndex == 6
        Return "$AGH_JS_KEEP_PELTS"
    ElseIf aiIndex == 8
        Return "$AGH_JS_KEEP_RESTORE"
    ElseIf aiIndex == 9
        Return "$AGH_JS_KEEP_SKILLPOT"
    ElseIf aiIndex == 10
        Return "$AGH_JS_KEEP_OTHERPOT"
    ElseIf aiIndex == 11
        Return "$AGH_JS_KEEP_POISONS"
    ElseIf aiIndex == 12
        Return "$AGH_JS_KEEP_MIDGEAR"
    ElseIf aiIndex == 13
        Return "$AGH_JS_KEEP_HIGHGEAR"
    ElseIf aiIndex == 14
        Return "$AGH_JS_KEEP_JEWELRY"
    EndIf
    Return "$AGH_JS_KEEP_TOOLS"
EndFunction

String Function KeepToggleHelp(Int aiIndex)
    If aiIndex == 0
        Return "$AGH_JS_CAT_INGREDIENTS_HELP"
    ElseIf aiIndex == 1
        Return "$AGH_JS_CAT_COOKED_HELP"
    ElseIf aiIndex == 2
        Return "$AGH_JS_CAT_RAW_HELP"
    ElseIf aiIndex == 3
        Return "$AGH_JS_CAT_DRINKS_HELP"
    ElseIf aiIndex == 4
        Return "$AGH_JS_CAT_ORE_HELP"
    ElseIf aiIndex == 5
        Return "$AGH_JS_CAT_GEMS_HELP"
    ElseIf aiIndex == 6
        Return "$AGH_JS_CAT_PELTS_HELP"
    ElseIf aiIndex == 8
        Return "$AGH_JS_CAT_RESTORE_HELP"
    ElseIf aiIndex == 9
        Return "$AGH_JS_CAT_SKILLPOT_HELP"
    ElseIf aiIndex == 10
        Return "$AGH_JS_CAT_OTHERPOT_HELP"
    ElseIf aiIndex == 11
        Return "$AGH_JS_CAT_POISONS_HELP"
    ElseIf aiIndex == 12
        Return "$AGH_JS_CAT_MIDGEAR_HELP"
    ElseIf aiIndex == 13
        Return "$AGH_JS_CAT_HIGHGEAR_HELP"
    ElseIf aiIndex == 14
        Return "$AGH_JS_CAT_JEWELRY_HELP"
    EndIf
    Return "$AGH_JS_CAT_TOOLS_HELP"
EndFunction

String Function KeepSliderHelp(Int aiIndex)
    If aiIndex == 0
        Return "$AGH_JS_KEEP_INGREDIENTS_HELP"
    ElseIf aiIndex == 1
        Return "$AGH_JS_KEEP_COOKED_HELP"
    ElseIf aiIndex == 2
        Return "$AGH_JS_KEEP_RAW_HELP"
    ElseIf aiIndex == 3
        Return "$AGH_JS_KEEP_DRINKS_HELP"
    ElseIf aiIndex == 4
        Return "$AGH_JS_KEEP_ORE_HELP"
    ElseIf aiIndex == 5
        Return "$AGH_JS_KEEP_GEMS_HELP"
    ElseIf aiIndex == 6
        Return "$AGH_JS_KEEP_PELTS_HELP"
    ElseIf aiIndex == 8
        Return "$AGH_JS_KEEP_RESTORE_HELP"
    ElseIf aiIndex == 9
        Return "$AGH_JS_KEEP_SKILLPOT_HELP"
    ElseIf aiIndex == 10
        Return "$AGH_JS_KEEP_OTHERPOT_HELP"
    ElseIf aiIndex == 11
        Return "$AGH_JS_KEEP_POISONS_HELP"
    ElseIf aiIndex == 12
        Return "$AGH_JS_KEEP_MIDGEAR_HELP"
    ElseIf aiIndex == 13
        Return "$AGH_JS_KEEP_HIGHGEAR_HELP"
    ElseIf aiIndex == 14
        Return "$AGH_JS_KEEP_JEWELRY_HELP"
    EndIf
    Return "$AGH_JS_KEEP_TOOLS_HELP"
EndFunction

; v0.4.0: the slider default, ceiling and step of category aiIndex
; (the v0.2 update plan r2 section 4.2; the DLL refuses a value above the
; ceiling, Settings.h SettingsKeepCategories).
Float Function KeepDefault(Int aiIndex)
    If aiIndex == 0
        Return 500.0
    ElseIf aiIndex == 1 || aiIndex == 3 || aiIndex >= 10
        Return 10.0
    ElseIf aiIndex == 2
        Return 20.0
    ElseIf aiIndex == 4 || aiIndex == 5
        Return 50.0
    ElseIf aiIndex == 6 || aiIndex == 8 || aiIndex == 9
        Return 100.0
    EndIf
    Return 1.0
EndFunction

Float Function KeepCeiling(Int aiIndex)
    If aiIndex == 0
        Return 2000.0
    ElseIf aiIndex <= 3
        Return 500.0
    ElseIf aiIndex <= 6
        Return 1000.0
    ElseIf aiIndex == 8 || aiIndex == 9
        Return 1000.0
    ElseIf aiIndex == 10 || aiIndex == 11
        Return 500.0
    ElseIf aiIndex >= 12
        Return 100.0
    EndIf
    Return 20.0
EndFunction

Float Function KeepStep(Int aiIndex)
    If aiIndex == 0
        Return 10.0
    ElseIf aiIndex >= 4 && aiIndex <= 6
        Return 5.0
    ElseIf aiIndex == 8 || aiIndex == 9
        Return 5.0
    EndIf
    Return 1.0
EndFunction

; The label key of one "why kept" row (no key is ever concatenated).
String Function KeptLabel(Int aiIndex)
    If aiIndex == 0
        Return "$AGH_JS_KEPT_LIMIT"
    ElseIf aiIndex == 1
        Return "$AGH_JS_KEPT_PROTECTED"
    ElseIf aiIndex == 2
        Return "$AGH_JS_KEPT_STOLEN"
    ElseIf aiIndex == 3
        Return "$AGH_JS_KEPT_ENCHANT"
    EndIf
    Return "$AGH_JS_KEPT_NEVER"
EndFunction

String Function KeptHelp(Int aiIndex)
    If aiIndex == 0
        Return "$AGH_JS_KEPT_LIMIT_HELP"
    ElseIf aiIndex == 1
        Return "$AGH_JS_KEPT_PROTECTED_HELP"
    ElseIf aiIndex == 2
        Return "$AGH_JS_KEPT_STOLEN_HELP"
    ElseIf aiIndex == 3
        Return "$AGH_JS_KEPT_ENCHANT_HELP"
    EndIf
    Return "$AGH_JS_KEPT_NEVER_HELP"
EndFunction

Float Function OneDecimal(Float afValue)
    Return Math.Floor(afValue * 10.0 + 0.5) / 10.0
EndFunction

Event OnConfigInit()
    SetPages()
    ModName = "$AGH_JS_TITLE"
    keptOptions = New Int[5]
    currentKept = New Int[5]
    keepToggleOptions = New Int[15]
    keepSliderOptions = New Int[15]
    Refresh()
EndEvent

Event OnGameReload()
    Parent.OnGameReload()
    Refresh()
EndEvent

Event OnConfigOpen()
    ; v0.8.0 (section 3.4 step 1): a fresh preview for this open; an
    ; older one is never shown.
    rateRowPending = False
    If Available()
        AGH_JS_Native.RequestRatePreview()
        ratePreviewAsked = True
    EndIf
    Refresh()
EndEvent

; v0.8.0: the next open asks again.
Event OnConfigClose()
    ratePreviewAsked = False
    rateRowPending = False
EndEvent

Event OnPageReset(String page)
    unavailableOption = -1
    inactiveStatusOption = -1
    inactiveValueOption = -1
    inactiveDetailsOption = -1
    inactiveLogOption = -1
    enabledOption = -1
    modeOption = -1
    confirmOption = -1
    hotkeyOption = -1
    maxValueOption = -1
    minWeightOption = -1
    maxValueWeightOption = -1
    maxItemsOption = -1
    sellIngredientsOption = -1
    sellAnywhereOption = -1
    perkPriceOption = -1
    anywhereRateOption = -1
    anywhereCheckOption = -1
    classicHeaderOption = -1
    categoriesHeaderOption = -1
    junkValueOnlyOption = -1
    junkMaxValueOption = -1
    sellStolenOption = -1
    sellWeakWeaponsOption = -1
    sellWeakArmorOption = -1
    sellClothingOption = -1
    sellEnchantedLootOption = -1
    enchantedLootMaxValueOption = -1
    knownEnchantmentsOnlyOption = -1
    If keptOptions.Length != 5
        keptOptions = New Int[5]
    EndIf
    Int resetIndex = 0
    While resetIndex < 5
        keptOptions[resetIndex] = -1
        resetIndex += 1
    EndWhile
    ; v0.4.0: a save made before this version has no keep arrays yet.
    keepHeaderOption = -1
    If keepToggleOptions.Length != 15
        keepToggleOptions = New Int[15]
    EndIf
    If keepSliderOptions.Length != 15
        keepSliderOptions = New Int[15]
    EndIf
    resetIndex = 0
    While resetIndex < 15
        keepToggleOptions[resetIndex] = -1
        keepSliderOptions[resetIndex] = -1
        resetIndex += 1
    EndWhile
    protectedNamesOption = -1
    protectedFormsOption = -1
    runtimeOption = -1
    effectiveOption = -1
    generationOption = -1
    lastRunOption = -1
    priceSourceOption = -1
    rateRowPending = False
    ; Papyrus dispatches OnConfigOpen and OnPageReset on separate threads, so
    ; the first page of a session can be built before OnConfigOpen's Refresh()
    ; returns. Refreshing here too makes this event self sufficient (the same
    ; fix the uncapper MCM carries for its own page build).
    Refresh()
    SetCursorFillMode(TOP_TO_BOTTOM)
    ; v0.9.0 (E1 design 2.5): loaded but not running: the two status rows only.
    If IsNativeInactive()
        AddInactiveRows()
        Return
    EndIf
    If !Available()
        ; An info row stays enabled, otherwise SkyUI never fires
        ; OnOptionHighlight over it with the mouse.
        unavailableOption = AddTextOption("$AGH_JS_STATUS", "$AGH_JS_SEE_INFO")
        Return
    EndIf
    If page == "" || page == "$AGH_JS_SETTINGS"
        enabledOption = AddToggleOption("$AGH_JS_ENABLED", currentEnabled)
        modeOption = AddTextOption("$AGH_JS_MODE", "Hotkey")
        confirmOption = AddTextOption("$AGH_JS_CONFIRM", "$AGH_JS_CONFIRM_V001")
        hotkeyOption = AddKeyMapOption("$AGH_JS_HOTKEY", currentHotkey, OPTION_FLAG_WITH_UNMAP)
        ; v0.2.0: the three thresholds are the Classic junk rule (the
        ; barter menu always; sell anywhere while "Junk: value only" is OFF).
        classicHeaderOption = AddHeaderOption("$AGH_JS_CLASSIC_HEADER")
        maxValueOption = AddSliderOption("$AGH_JS_MAXVALUE", currentMaxValue As Float)
        minWeightOption = AddSliderOption("$AGH_JS_MINWEIGHT", currentMinWeight, "{1}")
        maxValueWeightOption = AddSliderOption("$AGH_JS_MAXVALUEWEIGHT", currentMaxValueWeight, "{1}")
        maxItemsOption = AddSliderOption("$AGH_JS_MAXITEMS", currentMaxItems As Float)
        sellIngredientsOption = AddToggleOption("$AGH_JS_SELLINGREDIENTS", currentSellIngredients)
        protectedNamesOption = AddTextOption("$AGH_JS_PROTECTEDNAMES", ListText(currentProtectedNames))
        protectedFormsOption = AddTextOption("$AGH_JS_PROTECTEDFORMS", ListText(currentProtectedForms))
        ; v0.1.0: the sell-anywhere column, top of the right column.
        SetCursorPosition(1)
        sellAnywhereOption = AddToggleOption("$AGH_JS_SELLANYWHERE", currentSellAnywhere)
        ; v0.8.0: the perk price switch, then the preview of this open.
        perkPriceOption = AddToggleOption("$AGH_JS_PERKPRICE", currentPerkPrice)
        ; OnConfigOpen runs on its own thread and may not have asked yet: a
        ; preview from an earlier open is never shown.
        If !ratePreviewAsked
            AGH_JS_Native.RequestRatePreview()
            ratePreviewAsked = True
        EndIf
        WaitRatePreview()
        anywhereRateOption = AddTextOption("$AGH_JS_ANYWHERE_RATE", AnywhereRateText())
        rateRowPending = currentAnywhereRate == -2
        anywhereCheckOption = AddTextOption("$AGH_JS_ANYWHERE_CHECK", MerchantCheckText())
    ElseIf page == "$AGH_JS_CATEGORIES"
        ; v0.2.0 (the v0.2 update plan r2 4.2): a toggle left, its slider
        ; right on the same row; rows without a slider sell all.
        SetCursorFillMode(LEFT_TO_RIGHT)
        categoriesHeaderOption = AddHeaderOption("$AGH_JS_CAT_HEADER")
        AddEmptyOption()
        junkValueOnlyOption = AddToggleOption("$AGH_JS_JUNKVALUEONLY", currentJunkValueOnly)
        junkMaxValueOption = AddSliderOption("$AGH_JS_JUNKMAXVALUE", currentJunkMaxValue As Float, "{0}", JunkMaxValueFlags())
        sellStolenOption = AddToggleOption("$AGH_JS_SELLSTOLEN", currentSellStolen)
        AddEmptyOption()
        sellWeakWeaponsOption = AddToggleOption("$AGH_JS_SELLWEAKWEAPONS", currentSellWeakWeapons)
        AddEmptyOption()
        sellWeakArmorOption = AddToggleOption("$AGH_JS_SELLWEAKARMOR", currentSellWeakArmor)
        AddEmptyOption()
        sellClothingOption = AddToggleOption("$AGH_JS_SELLCLOTHING", currentSellClothing)
        AddEmptyOption()
        ; v0.3.0: weak enchanted loot with its jewelry value slider, then
        ; the known-enchantment switch.
        sellEnchantedLootOption = AddToggleOption("$AGH_JS_SELLENCHANTEDLOOT", currentSellEnchantedLoot)
        enchantedLootMaxValueOption = AddSliderOption("$AGH_JS_ENCHANTMAXVALUE", currentEnchantedLootMaxValue As Float, "{0}", EnchantedLootMaxValueFlags())
        knownEnchantmentsOnlyOption = AddToggleOption("$AGH_JS_KNOWNENCHANTONLY", currentKnownEnchantmentsOnly)
        AddEmptyOption()
        ; v0.4.0: the keep-limit categories, a toggle and its keep slider
        ; per row.
        keepHeaderOption = AddHeaderOption("$AGH_JS_KEEP_HEADER")
        AddEmptyOption()
        Int keepRow = 0
        While keepRow < 15
            keepToggleOptions[keepRow] = AddToggleOption(KeepToggleLabel(keepRow), currentKeepOn[keepRow])
            keepSliderOptions[keepRow] = AddSliderOption(KeepSliderLabel(keepRow), currentKeepLimit[keepRow] As Float, "{0}", KeepSliderFlags(keepRow))
            keepRow += 1
        EndWhile
    ElseIf page == "$AGH_JS_DIAGNOSTICS"
        runtimeOption = AddTextOption("$AGH_JS_RUNTIME", RuntimeText())
        effectiveOption = AddTextOption("$AGH_JS_EFFECTIVE", EffectiveText())
        ; w-probe-r33: the generation is a native integer rendered as text, so
        ; it goes through the same value-column bound as every other path.
        generationOption = AddTextOption("$AGH_JS_GENERATION", ValueColumnText(currentGeneration As String))
        ; The gate token is bounded like every other value column; the info
        ; panel below already carries the complete GetLastRunSummary() line.
        lastRunOption = AddTextOption("$AGH_JS_LASTRUN", LastRunValueText())
        ; v0.8.0: how the last sale priced (kept until the next sale or a load).
        priceSourceOption = AddTextOption("$AGH_JS_PRICESOURCE", PriceSourceText())
        ; v0.2.0: why the kinds of the last press stayed.
        AddHeaderOption("$AGH_JS_KEPT_HEADER")
        Int keptIndex = 0
        While keptIndex < 5
            keptOptions[keptIndex] = AddTextOption(KeptLabel(keptIndex), KeptText(keptIndex))
            keptIndex += 1
        EndWhile
    EndIf
EndEvent

Event OnOptionHighlight(Int option)
    If option < 0
        SetInfoText("")
        Return
    EndIf
    If !Available()
        ; v0.9.0: cause-neutral; the receipt names the real reason.
        If inactiveStatusOption >= 0 && (option == inactiveStatusOption || option == inactiveValueOption || option == inactiveDetailsOption || option == inactiveLogOption)
            SetInfoText("$AGH_JS_INACTIVE_INFO")
            Return
        EndIf
        If unavailableOption >= 0 && option == unavailableOption
            SetInfoText("$AGH_JS_DLL_MISSING")
            Return
        EndIf
        SetInfoText("")
        Return
    EndIf
    UpdatePendingRate()
    If enabledOption >= 0 && option == enabledOption
        SetInfoText("$AGH_JS_ENABLED_HELP")
        Return
    EndIf
    If modeOption >= 0 && option == modeOption
        SetInfoText("$AGH_JS_MODE_HELP")
        Return
    EndIf
    If confirmOption >= 0 && option == confirmOption
        SetInfoText("$AGH_JS_CONFIRM_HELP")
        Return
    EndIf
    If hotkeyOption >= 0 && option == hotkeyOption
        SetInfoText("$AGH_JS_HOTKEY_HELP")
        Return
    EndIf
    If maxValueOption >= 0 && option == maxValueOption
        SetInfoText("$AGH_JS_MAXVALUE_HELP")
        Return
    EndIf
    If minWeightOption >= 0 && option == minWeightOption
        SetInfoText("$AGH_JS_MINWEIGHT_HELP")
        Return
    EndIf
    If maxValueWeightOption >= 0 && option == maxValueWeightOption
        SetInfoText("$AGH_JS_MAXVALUEWEIGHT_HELP")
        Return
    EndIf
    If maxItemsOption >= 0 && option == maxItemsOption
        SetInfoText("$AGH_JS_MAXITEMS_HELP")
        Return
    EndIf
    If sellIngredientsOption >= 0 && option == sellIngredientsOption
        SetInfoText("$AGH_JS_SELLINGREDIENTS_HELP")
        Return
    EndIf
    If sellAnywhereOption >= 0 && option == sellAnywhereOption
        SetInfoText("$AGH_JS_SELLANYWHERE_HELP")
        Return
    EndIf
    If perkPriceOption >= 0 && option == perkPriceOption
        SetInfoText("$AGH_JS_PERKPRICE_HELP")
        Return
    EndIf
    If anywhereRateOption >= 0 && option == anywhereRateOption
        SetInfoText("$AGH_JS_ANYWHERE_RATE_HELP")
        Return
    EndIf
    If anywhereCheckOption >= 0 && option == anywhereCheckOption
        SetInfoText("$AGH_JS_ANYWHERE_CHECK_HELP")
        Return
    EndIf
    If junkValueOnlyOption >= 0 && option == junkValueOnlyOption
        SetInfoText("$AGH_JS_JUNKVALUEONLY_HELP")
        Return
    EndIf
    If junkMaxValueOption >= 0 && option == junkMaxValueOption
        SetInfoText("$AGH_JS_JUNKMAXVALUE_HELP")
        Return
    EndIf
    If sellStolenOption >= 0 && option == sellStolenOption
        SetInfoText("$AGH_JS_SELLSTOLEN_HELP")
        Return
    EndIf
    If sellWeakWeaponsOption >= 0 && option == sellWeakWeaponsOption
        SetInfoText("$AGH_JS_SELLWEAKWEAPONS_HELP")
        Return
    EndIf
    If sellWeakArmorOption >= 0 && option == sellWeakArmorOption
        SetInfoText("$AGH_JS_SELLWEAKARMOR_HELP")
        Return
    EndIf
    If sellClothingOption >= 0 && option == sellClothingOption
        SetInfoText("$AGH_JS_SELLCLOTHING_HELP")
        Return
    EndIf
    If sellEnchantedLootOption >= 0 && option == sellEnchantedLootOption
        SetInfoText("$AGH_JS_SELLENCHANTEDLOOT_HELP")
        Return
    EndIf
    If enchantedLootMaxValueOption >= 0 && option == enchantedLootMaxValueOption
        SetInfoText("$AGH_JS_ENCHANTMAXVALUE_HELP")
        Return
    EndIf
    If knownEnchantmentsOnlyOption >= 0 && option == knownEnchantmentsOnlyOption
        SetInfoText("$AGH_JS_KNOWNENCHANTONLY_HELP")
        Return
    EndIf
    If keepHeaderOption >= 0 && option == keepHeaderOption
        SetInfoText("$AGH_JS_KEEP_HEADER_HELP")
        Return
    EndIf
    Int keepHelp = 0
    While keepHelp < keepToggleOptions.Length
        If keepToggleOptions[keepHelp] >= 0 && option == keepToggleOptions[keepHelp]
            SetInfoText(KeepToggleHelp(keepHelp))
            Return
        EndIf
        If keepSliderOptions[keepHelp] >= 0 && option == keepSliderOptions[keepHelp]
            SetInfoText(KeepSliderHelp(keepHelp))
            Return
        EndIf
        keepHelp += 1
    EndWhile
    Int helpIndex = 0
    While helpIndex < keptOptions.Length
        If keptOptions[helpIndex] >= 0 && option == keptOptions[helpIndex]
            SetInfoText(KeptHelp(helpIndex))
            Return
        EndIf
        helpIndex += 1
    EndWhile
    If protectedNamesOption >= 0 && option == protectedNamesOption
        ; A-002: when the bound shortened the value column, the complete list
        ; takes the panel; otherwise the help sentence does.
        If ValueColumnFits(currentProtectedNames)
            SetInfoText("$AGH_JS_PROTECTEDNAMES_HELP")
        Else
            SetInfoText(currentProtectedNames)
        EndIf
        Return
    EndIf
    If protectedFormsOption >= 0 && option == protectedFormsOption
        If ValueColumnFits(currentProtectedForms)
            SetInfoText("$AGH_JS_PROTECTEDFORMS_HELP")
        Else
            SetInfoText(currentProtectedForms)
        EndIf
        Return
    EndIf
    If runtimeOption >= 0 && option == runtimeOption
        ; version-neutral (the UI text check, no hard-coded version literal).
        ; currentReleaseId (GetReleaseId(), BuildInfo.h) already identifies the
        ; build.
        SetInfoText("AGH Junk Seller releaseId=" + currentReleaseId + "; " + currentRuntime)
        Return
    EndIf
    If effectiveOption >= 0 && option == effectiveOption
        ; R30-1: the value column is the fixed OK / OFF pair, so the reason
        ; token from GetStatus() belongs here. It is the DLL's own vocabulary
        ; and stays untranslated, exactly like the value tokens, and it is
        ; passed as the whole string so SkyUI's "$TOKEN" scan never sees it.
        If currentEffective
            SetInfoText("$AGH_JS_EFFECTIVE_HELP")
        Else
            SetInfoText(currentReason)
        EndIf
        Return
    EndIf
    If generationOption >= 0 && option == generationOption
        SetInfoText("$AGH_JS_GENERATION_HELP")
        Return
    EndIf
    If priceSourceOption >= 0 && option == priceSourceOption
        SetInfoText("$AGH_JS_PRICESOURCE_HELP")
        Return
    EndIf
    If lastRunOption >= 0 && option == lastRunOption
        ; w-probe-r33 (finding B): the value column carries the short token now,
        ; so this panel carries the whole meaning of an adapter refusal -- the
        ; translated sentence that says why nothing was sold. A "$" key is only
        ; substituted when it stands alone (design rulings R53-2/R53-3), so the
        ; sentence and the native's named GetLastRunSummary() line are never
        ; concatenated into one string: the sentence is set for the adapter
        ; path and the summary for every other path.
        If currentReason == "AWAITING_MENU"
            SetInfoText("$AGH_JS_AWAITING_MENU")
        ElseIf currentReason == "ADAPTER_UNPROVEN" || LastRunSettingsText() == "ADAPTER_UNPROVEN"
            SetInfoText("$AGH_JS_ADAPTER_UNAVAILABLE")
        Else
            SetInfoText(currentLastRun)
        EndIf
        Return
    EndIf
    SetInfoText("")
EndEvent

Event OnOptionSliderOpen(Int option)
    If !Available()
        Return
    EndIf
    If maxValueOption >= 0 && option == maxValueOption
        SetSliderDialogStartValue(currentMaxValue As Float)
        SetSliderDialogDefaultValue(10.0)
        SetSliderDialogRange(0.0, 1000.0)
        SetSliderDialogInterval(1.0)
        Return
    EndIf
    If minWeightOption >= 0 && option == minWeightOption
        SetSliderDialogStartValue(currentMinWeight)
        SetSliderDialogDefaultValue(0.5)
        SetSliderDialogRange(0.0, 10.0)
        SetSliderDialogInterval(0.1)
        Return
    EndIf
    If maxValueWeightOption >= 0 && option == maxValueWeightOption
        SetSliderDialogStartValue(currentMaxValueWeight)
        SetSliderDialogDefaultValue(10.0)
        SetSliderDialogRange(0.0, 100.0)
        SetSliderDialogInterval(0.1)
        Return
    EndIf
    If maxItemsOption >= 0 && option == maxItemsOption
        ; design-r2 section 5 (D2): the ceiling and default move from 40
        ; to 256 with SelectorMaxTransactions.
        SetSliderDialogStartValue(currentMaxItems As Float)
        SetSliderDialogDefaultValue(256.0)
        SetSliderDialogRange(1.0, 256.0)
        SetSliderDialogInterval(1.0)
        Return
    EndIf
    If junkMaxValueOption >= 0 && option == junkMaxValueOption
        ; v0.2.0: 0-200, step 5, default 50 (the v0.2 update plan 4.2).
        SetSliderDialogStartValue(currentJunkMaxValue As Float)
        SetSliderDialogDefaultValue(50.0)
        SetSliderDialogRange(0.0, 200.0)
        SetSliderDialogInterval(5.0)
        Return
    EndIf
    If enchantedLootMaxValueOption >= 0 && option == enchantedLootMaxValueOption
        ; v0.3.0: 0-2000, step 50, default 300 (the v0.2 update plan 4.2).
        SetSliderDialogStartValue(currentEnchantedLootMaxValue As Float)
        SetSliderDialogDefaultValue(300.0)
        SetSliderDialogRange(0.0, 2000.0)
        SetSliderDialogInterval(50.0)
        Return
    EndIf
    ; v0.4.0: the keep sliders (range minimum 0 = sell all).
    Int keepOpen = 0
    While keepOpen < keepSliderOptions.Length
        If keepSliderOptions[keepOpen] >= 0 && option == keepSliderOptions[keepOpen]
            SetSliderDialogStartValue(currentKeepLimit[keepOpen] As Float)
            SetSliderDialogDefaultValue(KeepDefault(keepOpen))
            SetSliderDialogRange(0.0, KeepCeiling(keepOpen))
            SetSliderDialogInterval(KeepStep(keepOpen))
            Return
        EndIf
        keepOpen += 1
    EndWhile
EndEvent

; The refuse-keeps-old-value rule: a Set* returns true only after the write and
; the readback, so a false re-reads the value the storage still holds and shows
; the failure token. The row is redrawn from the storage either way.
Event OnOptionSliderAccept(Int option, Float value)
    If !Available()
        Return
    EndIf
    If maxValueOption >= 0 && option == maxValueOption
        Int wanted = Math.Floor(value + 0.5) As Int
        If wanted < 0
            wanted = 0
        EndIf
        If AGH_JS_Native.SetMaxValue(wanted)
            currentMaxValue = AGH_JS_Native.GetMaxValue()
        Else
            currentMaxValue = AGH_JS_Native.GetMaxValue()
            SetInfoText(AGH_JS_Native.GetLastSaveError())
        EndIf
        SetSliderOptionValue(maxValueOption, currentMaxValue As Float)
        Return
    EndIf
    If minWeightOption >= 0 && option == minWeightOption
        Float wantedWeight = OneDecimal(value)
        If AGH_JS_Native.SetMinWeight(wantedWeight)
            currentMinWeight = AGH_JS_Native.GetMinWeight()
        Else
            currentMinWeight = AGH_JS_Native.GetMinWeight()
            SetInfoText(AGH_JS_Native.GetLastSaveError())
        EndIf
        SetSliderOptionValue(minWeightOption, currentMinWeight, "{1}")
        Return
    EndIf
    If maxValueWeightOption >= 0 && option == maxValueWeightOption
        Float wantedRatio = OneDecimal(value)
        If AGH_JS_Native.SetMaxValueWeight(wantedRatio)
            currentMaxValueWeight = AGH_JS_Native.GetMaxValueWeight()
        Else
            currentMaxValueWeight = AGH_JS_Native.GetMaxValueWeight()
            SetInfoText(AGH_JS_Native.GetLastSaveError())
        EndIf
        SetSliderOptionValue(maxValueWeightOption, currentMaxValueWeight, "{1}")
        Return
    EndIf
    If maxItemsOption >= 0 && option == maxItemsOption
        Int wantedItems = Math.Floor(value + 0.5) As Int
        ; The DLL refuses anything above the selector's own cap of 256 (
        ; design-r2 section 5); the slider range above stops at 256, so this
        ; is the safety net.
        If AGH_JS_Native.SetMaxItems(wantedItems)
            currentMaxItems = AGH_JS_Native.GetMaxItems()
        Else
            currentMaxItems = AGH_JS_Native.GetMaxItems()
            SetInfoText(AGH_JS_Native.GetLastSaveError())
        EndIf
        SetSliderOptionValue(maxItemsOption, currentMaxItems As Float)
        Return
    EndIf
    If junkMaxValueOption >= 0 && option == junkMaxValueOption
        Int wantedJunk = Math.Floor(value + 0.5) As Int
        If AGH_JS_Native.SetJunkMaxValue(wantedJunk)
            currentJunkMaxValue = AGH_JS_Native.GetJunkMaxValue()
        Else
            currentJunkMaxValue = AGH_JS_Native.GetJunkMaxValue()
            SetInfoText(AGH_JS_Native.GetLastSaveError())
        EndIf
        SetSliderOptionValue(junkMaxValueOption, currentJunkMaxValue As Float, "{0}")
        Return
    EndIf
    If enchantedLootMaxValueOption >= 0 && option == enchantedLootMaxValueOption
        Int wantedEnchant = Math.Floor(value + 0.5) As Int
        If AGH_JS_Native.SetEnchantedLootMaxValue(wantedEnchant)
            currentEnchantedLootMaxValue = AGH_JS_Native.GetEnchantedLootMaxValue()
        Else
            currentEnchantedLootMaxValue = AGH_JS_Native.GetEnchantedLootMaxValue()
            SetInfoText(AGH_JS_Native.GetLastSaveError())
        EndIf
        SetSliderOptionValue(enchantedLootMaxValueOption, currentEnchantedLootMaxValue As Float, "{0}")
        Return
    EndIf
    Int keepAccept = 0
    While keepAccept < keepSliderOptions.Length
        If keepSliderOptions[keepAccept] >= 0 && option == keepSliderOptions[keepAccept]
            Int wantedKeep = Math.Floor(value + 0.5) As Int
            If wantedKeep < 0
                wantedKeep = 0
            EndIf
            If AGH_JS_Native.SetKeepLimit(keepAccept, wantedKeep)
                currentKeepLimit[keepAccept] = AGH_JS_Native.GetKeepLimit(keepAccept)
            Else
                currentKeepLimit[keepAccept] = AGH_JS_Native.GetKeepLimit(keepAccept)
                SetInfoText(AGH_JS_Native.GetLastSaveError())
            EndIf
            SetSliderOptionValue(keepSliderOptions[keepAccept], currentKeepLimit[keepAccept] As Float, "{0}")
            Return
        EndIf
        keepAccept += 1
    EndWhile
EndEvent

Event OnOptionSelect(Int option)
    If !Available()
        Return
    EndIf
    If enabledOption >= 0 && option == enabledOption
        Bool wantedEnabled = !currentEnabled
        If AGH_JS_Native.SetEnabled(wantedEnabled)
            currentEnabled = AGH_JS_Native.GetEnabled()
        Else
            currentEnabled = AGH_JS_Native.GetEnabled()
            SetInfoText(AGH_JS_Native.GetLastSaveError())
        EndIf
        SetToggleOptionValue(enabledOption, currentEnabled)
        Refresh()
        Return
    EndIf
    If confirmOption >= 0 && option == confirmOption
        ; v0.0.2 exposes this stored setting without allowing an unavailable list.
        SetInfoText("$AGH_JS_CONFIRM_HELP")
        Return
    EndIf
    If sellIngredientsOption >= 0 && option == sellIngredientsOption
        Bool wantedSellIngredients = !currentSellIngredients
        If AGH_JS_Native.SetSellIngredients(wantedSellIngredients)
            currentSellIngredients = AGH_JS_Native.GetSellIngredients()
        Else
            currentSellIngredients = AGH_JS_Native.GetSellIngredients()
            SetInfoText(AGH_JS_Native.GetLastSaveError())
        EndIf
        SetToggleOptionValue(sellIngredientsOption, currentSellIngredients)
        Return
    EndIf
    If sellAnywhereOption >= 0 && option == sellAnywhereOption
        Bool wantedSellAnywhere = !currentSellAnywhere
        ; v0.1.0 (design r3 1.1): switching it ON asks once; Cancel
        ; leaves it OFF. Switching it OFF needs no question.
        If wantedSellAnywhere && !ShowMessage("$AGH_JS_SELLANYWHERE_CONFIRM", True, "$Accept", "$Cancel")
            Return
        EndIf
        If AGH_JS_Native.SetSellAnywhere(wantedSellAnywhere)
            currentSellAnywhere = AGH_JS_Native.GetSellAnywhere()
        Else
            currentSellAnywhere = AGH_JS_Native.GetSellAnywhere()
            SetInfoText(AGH_JS_Native.GetLastSaveError())
        EndIf
        SetToggleOptionValue(sellAnywhereOption, currentSellAnywhere)
        Return
    EndIf
    If perkPriceOption >= 0 && option == perkPriceOption
        ; v0.8.0 (section 3.4 step 3): the switch asks for a new preview and
        ; the "Rate now" row follows it (bounded wait, then one update).
        If AGH_JS_Native.SetPerkPrice(!currentPerkPrice)
            currentPerkPrice = AGH_JS_Native.GetPerkPrice()
        Else
            currentPerkPrice = AGH_JS_Native.GetPerkPrice()
            SetInfoText(AGH_JS_Native.GetLastSaveError())
        EndIf
        SetToggleOptionValue(perkPriceOption, currentPerkPrice)
        If anywhereRateOption >= 0
            AGH_JS_Native.RequestRatePreview()
            WaitRatePreview()
            rateRowPending = currentAnywhereRate == -2
            SetTextOptionValue(anywhereRateOption, AnywhereRateText())
        EndIf
        Return
    EndIf
    If junkValueOnlyOption >= 0 && option == junkValueOnlyOption
        If AGH_JS_Native.SetJunkValueOnly(!currentJunkValueOnly)
            currentJunkValueOnly = AGH_JS_Native.GetJunkValueOnly()
        Else
            currentJunkValueOnly = AGH_JS_Native.GetJunkValueOnly()
            SetInfoText(AGH_JS_Native.GetLastSaveError())
        EndIf
        SetToggleOptionValue(junkValueOnlyOption, currentJunkValueOnly)
        If junkMaxValueOption >= 0
            SetOptionFlags(junkMaxValueOption, JunkMaxValueFlags())
        EndIf
        Return
    EndIf
    If sellStolenOption >= 0 && option == sellStolenOption
        If AGH_JS_Native.SetSellStolen(!currentSellStolen)
            currentSellStolen = AGH_JS_Native.GetSellStolen()
        Else
            currentSellStolen = AGH_JS_Native.GetSellStolen()
            SetInfoText(AGH_JS_Native.GetLastSaveError())
        EndIf
        SetToggleOptionValue(sellStolenOption, currentSellStolen)
        Return
    EndIf
    If sellWeakWeaponsOption >= 0 && option == sellWeakWeaponsOption
        If AGH_JS_Native.SetSellWeakWeapons(!currentSellWeakWeapons)
            currentSellWeakWeapons = AGH_JS_Native.GetSellWeakWeapons()
        Else
            currentSellWeakWeapons = AGH_JS_Native.GetSellWeakWeapons()
            SetInfoText(AGH_JS_Native.GetLastSaveError())
        EndIf
        SetToggleOptionValue(sellWeakWeaponsOption, currentSellWeakWeapons)
        Return
    EndIf
    If sellWeakArmorOption >= 0 && option == sellWeakArmorOption
        If AGH_JS_Native.SetSellWeakArmor(!currentSellWeakArmor)
            currentSellWeakArmor = AGH_JS_Native.GetSellWeakArmor()
        Else
            currentSellWeakArmor = AGH_JS_Native.GetSellWeakArmor()
            SetInfoText(AGH_JS_Native.GetLastSaveError())
        EndIf
        SetToggleOptionValue(sellWeakArmorOption, currentSellWeakArmor)
        Return
    EndIf
    If sellClothingOption >= 0 && option == sellClothingOption
        If AGH_JS_Native.SetSellClothing(!currentSellClothing)
            currentSellClothing = AGH_JS_Native.GetSellClothing()
        Else
            currentSellClothing = AGH_JS_Native.GetSellClothing()
            SetInfoText(AGH_JS_Native.GetLastSaveError())
        EndIf
        SetToggleOptionValue(sellClothingOption, currentSellClothing)
        Return
    EndIf
    If sellEnchantedLootOption >= 0 && option == sellEnchantedLootOption
        If AGH_JS_Native.SetSellEnchantedLoot(!currentSellEnchantedLoot)
            currentSellEnchantedLoot = AGH_JS_Native.GetSellEnchantedLoot()
        Else
            currentSellEnchantedLoot = AGH_JS_Native.GetSellEnchantedLoot()
            SetInfoText(AGH_JS_Native.GetLastSaveError())
        EndIf
        SetToggleOptionValue(sellEnchantedLootOption, currentSellEnchantedLoot)
        If enchantedLootMaxValueOption >= 0
            SetOptionFlags(enchantedLootMaxValueOption, EnchantedLootMaxValueFlags())
        EndIf
        Return
    EndIf
    If knownEnchantmentsOnlyOption >= 0 && option == knownEnchantmentsOnlyOption
        If AGH_JS_Native.SetKnownEnchantmentsOnly(!currentKnownEnchantmentsOnly)
            currentKnownEnchantmentsOnly = AGH_JS_Native.GetKnownEnchantmentsOnly()
        Else
            currentKnownEnchantmentsOnly = AGH_JS_Native.GetKnownEnchantmentsOnly()
            SetInfoText(AGH_JS_Native.GetLastSaveError())
        EndIf
        SetToggleOptionValue(knownEnchantmentsOnlyOption, currentKnownEnchantmentsOnly)
        Return
    EndIf
    Int keepSelect = 0
    While keepSelect < keepToggleOptions.Length
        If keepToggleOptions[keepSelect] >= 0 && option == keepToggleOptions[keepSelect]
            If AGH_JS_Native.SetKeepCategory(keepSelect, !currentKeepOn[keepSelect])
                currentKeepOn[keepSelect] = AGH_JS_Native.GetKeepCategory(keepSelect)
            Else
                currentKeepOn[keepSelect] = AGH_JS_Native.GetKeepCategory(keepSelect)
                SetInfoText(AGH_JS_Native.GetLastSaveError())
            EndIf
            SetToggleOptionValue(keepToggleOptions[keepSelect], currentKeepOn[keepSelect])
            If keepSliderOptions[keepSelect] >= 0
                SetOptionFlags(keepSliderOptions[keepSelect], KeepSliderFlags(keepSelect))
            EndIf
            Return
        EndIf
        keepSelect += 1
    EndWhile
EndEvent

; The keymap row. SkyUI sends a negative key code for its own unmap entry
; (OPTION_FLAG_WITH_UNMAP above), and the native's 0 is this product's
; unassigned value, so an unmap writes 0 through the same SetHotkey path.
Event OnOptionKeyMapChange(Int option, Int keyCode, String conflictControl, String conflictName)
    If !Available()
        Return
    EndIf
    If hotkeyOption >= 0 && option == hotkeyOption
        If keyCode < 0
            keyCode = 0
        EndIf
        If AGH_JS_Native.SetHotkey(keyCode)
            currentHotkey = AGH_JS_Native.GetHotkey()
        Else
            currentHotkey = AGH_JS_Native.GetHotkey()
            SetInfoText(AGH_JS_Native.GetLastSaveError())
        EndIf
        SetKeyMapOptionValue(hotkeyOption, currentHotkey)
    EndIf
EndEvent
