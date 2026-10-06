#pragma once
// w-probe-r28 (R29-1/R29-2/R29-3): the AGH_JS_Native native surface.
//
// PapyrusBridge.cpp registers one function per declaration below on the SKSE
// Papyrus interface, for the script name AGH_JS_Native;
// Source/Scripts/AGH_JS_Native.psc holds the same list. The bodies live in
// Probe.cpp next to the live settings state, so this header only declares them
// and stays engine free: Probe.cpp includes it and Probe.cpp is the only
// translation unit that owns the settings mutex.
#include "Settings.h"
#include <cstdint>
#include <string>

namespace js {

// One Set* result. `accepted` is ApplySetting's own verdict, `saved` and
// `readbackOk` are the file round trip of R29-2, and `reason` is always one of
// the ApplySetting reason tokens. A save failure is not a sixth reason: the
// saved/readbackOk flags say it happened and GetLastSaveError carries the text.
struct SettingSaveOutcome {
    bool accepted{};
    bool saved{};
    bool readbackOk{};
    const char* reason{SettingReasonUnknownKey};
};

// Applies one key, saves the whole settings file, reads it back. Called on the
// Papyrus VM thread. The settings mutex is held only for the one snapshot and
// the one swap: the js_settings_apply row is written with it released, because
// NewRow reads the live snapshot itself.
SettingSaveOutcome ApplyAndSaveSetting(const char* key, const char* value);

// The live value of one key, read as the one snapshot. A key that is not in the
// v0.1 set answers the type's own default and never changes state.
bool ReadSettingBool(const char* key);
int ReadSettingInt(const char* key);
float ReadSettingFloat(const char* key);
// The live text of one list key, in the file's own spelling: the two protection
// lists stay semicolon separated here, and the native getters translate to the
// Papyrus `|` spelling. An unknown key answers the empty string.
std::string ReadSettingText(const char* key);

// The runtime/version this module loaded under (the test Plugin.cpp decides on).
const char* GetRuntimeName() noexcept;
const char* BuildReleaseId() noexcept;
// The atomic generation the save path bumps once a readback matched (R29-2).
std::uint64_t GetConfigGeneration() noexcept;
// The cached R29-3 verdict and its reason token.
bool IsEffectivelyEnabled() noexcept;
const char* GetStatusReason() noexcept;
// "none" when no save has failed; otherwise the last save failure text.
std::string ReadLastSaveError();
// One line built from the last js_rule_run values, "none" before the first run.
std::string ReadLastRunSummary();
// v0.1.0 (design r3 2.5): the sell-anywhere rate, round(100 x m') from
// the live inputs (-1 when they cannot be read), and the last merchant check
// "<merchant> / <anywhere>" (empty before an in-menu batch since the last load).
std::int32_t ReadAnywhereRatePercent();
std::string ReadLastMerchantCheck();
// v0.8.0 (the barter-price design r2 section 3.4): the main-thread "Rate
// now" preview. RequestAnywhereRatePreview queues one UI task (false when none
// could be queued); ReadAnywhereRatePreview answers -2 while the latest
// request is not published, -1 when its price could not be read, else
// round(100 x m') of a price snapshot taken like a press's. ReadLastPriceSource
// is the last sale's js::PriceSource (0 perks, 1 formula, 2 fallback), -1
// before one since the last load.
bool RequestAnywhereRatePreview();
std::int32_t ReadAnywhereRatePreview();
std::int32_t ReadLastPriceSource();
// v0.2.0: one "why kept" counter of the last planned
// sell-anywhere press: 0 at limit, 1 protected, 2 stolen with the switch OFF,
// 3 enchantment not known, 4 never sold; -1 before one or for another index.
std::int32_t ReadLastKept(std::int32_t index);
// Records the CheckAPI mismatch; every later effective verdict is then OFF.
void NoteApiMismatch();

// Registers every AGH_JS_Native function once, from Probe.cpp's OnMessage on
// kDataLoaded. False means the SKSE Papyrus interface was unavailable; the
// plugin stays resident and the probe stays OFF.
[[nodiscard]] bool RegisterPapyrusBridge();

}  // namespace js
