#pragma once
// Engine-free rule-path facts for w-probe-r25 (design ruling section 27, R27-2).
// Two things live here because both are pure and both are driven by the
// offline harness:
//   R27-2 a: the two protection lists the rule reader needs. ProtectedNames is
//     a semicolon-separated display-name list; ProtectedForms is a
//     semicolon-separated FormKey list ("Plugin.esp|0xHHHHHH", or a bare
//     runtime FormID). Parsing is engine-free: the FormKeys are resolved to
//     runtime FormIDs by the caller (Probe.cpp), so the host test drives
//     exactly the code the runtime uses.
//   R27-2 b: the six vendor keyword names that keep a Misc base out of the
//     junk class, plus the engine-free half of that decision.
// Nothing here includes an engine header.
#include "Selector.h"
#include "WBind.h"
#include "receipt/Log.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace js {
// R27-2 a bounds: at most 32 names of at most TextBytes each, and at most 32
// forms. Past either bound the list refuses the load, like every other key.
constexpr unsigned MaxProtectedNames = 32;
constexpr unsigned MaxProtectedForms = 32;
// A plugin file name as a FormKey spells it ("Skyrim.esm").
constexpr unsigned FormPluginBytes = 64;
// A local FormID is the low 24 bits of the runtime FormID: the compile index
// is OR-ed in on top, so a larger local value would corrupt the resolution.
constexpr std::uint32_t MaxLocalFormId = 0xFFFFFF;

// A protected display name is only comparable when the complete GFx string
// fits in the receipt field. The return value is false for a truncated source;
// the caller must keep protection facts unknown and must not compare the copy.
template <std::size_t N>
inline bool CopyProtectedDisplayName(std::array<char, N>& destination,
                                     const char* source) noexcept {
    destination = {};
    if (!source) return false;
    std::size_t i = 0;
    for (; i + 1 < N && source[i]; ++i) destination[i] = source[i];
    destination[i] = '\0';
    return source[i] == '\0';
}

// ASCII whitespace a list item may carry around it. Only the edges are
// trimmed, so a plugin name or a display name keeps its interior spaces.
inline bool ProtectionSpace(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
           value == '\v' || value == '\f';
}

// The display names ProtectedNames= spells. Fixed size and copied by value, so
// the hook thread only reads it after the INI was parsed at load time.
class ProtectedNames {
public:
    void Clear() noexcept { _names = {}; _count = 0; }
    unsigned Count() const noexcept { return _count; }
    const char* At(unsigned index) const noexcept {
        return index < _count ? _names[index].data() : "";
    }
    // One trimmed list item. Empty, oversized, past the cap or a duplicate
    // (compared without case, like the match below) refuses the whole list.
    bool Add(const char* text, std::size_t length) noexcept {
        if (!text || !length || length >= TextBytes || _count >= MaxProtectedNames) return false;
        std::array<char, TextBytes> candidate{};
        for (std::size_t i = 0; i < length; ++i) candidate[i] = text[i];
        for (unsigned i = 0; i < _count; ++i) {
            if (NameEqualsNoCase(_names[i].data(), candidate.data())) return false;
        }
        _names[_count++] = candidate;
        return true;
    }
    // R27-2 a: case-insensitive exact match against the display name the r24
    // reader copied into RuleGfxRead::name. A partial match never protects.
    bool Matches(const char* name) const noexcept {
        if (!name) return false;
        for (unsigned i = 0; i < _count; ++i) {
            if (NameEqualsNoCase(_names[i].data(), name)) return true;
        }
        return false;
    }

private:
    std::array<std::array<char, TextBytes>, MaxProtectedNames> _names{};
    unsigned _count{};
};

// "Plugin.esp|0xHHHHHH" or a bare runtime FormID, as the INI spelled it.
struct ProtectedFormKey {
    bool pluginKnown{};
    std::array<char, FormPluginBytes> plugin{};
    std::uint32_t local{};
    // The item as the INI spelled it, so a key that fails to resolve can name
    // itself in the config row instead of turning the probe off in silence.
    std::array<char, TextBytes> text{};
};

// A hex local FormID: "0x12F" and "12F" are both accepted, a sign or any other
// character is not, and the 24-bit local range is enforced.
inline bool ParseProtectedLocalFormId(const char* text, std::size_t length,
                                      std::uint32_t& out) noexcept {
    if (!text || !length) return false;
    std::size_t i = 0;
    if (length >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) i = 2;
    if (i == length) return false;
    std::uint64_t value = 0;
    for (; i < length; ++i) {
        const char c = text[i];
        unsigned digit = 0;
        if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
        else return false;
        value = (value << 4) | digit;
        if (value > MaxLocalFormId) return false;
    }
    if (value == 0) return false;
    out = static_cast<std::uint32_t>(value);
    return true;
}

class ProtectedFormKeys {
public:
    void Clear() noexcept { _keys = {}; _count = 0; }
    unsigned Count() const noexcept { return _count; }
    const ProtectedFormKey& At(unsigned index) const noexcept {
        return _keys[index < _count ? index : 0];
    }
    // One trimmed item. An item with a '|' is plugin name + hex local FormID;
    // an item without one is a raw runtime FormID parsed exactly like
    // ParseBindTargetBase (WBind.h), so the two spellings cannot drift.
    bool Add(const char* text, std::size_t length) noexcept {
        if (!text || !length || length >= TextBytes || _count >= MaxProtectedForms) return false;
        ProtectedFormKey key;
        for (std::size_t i = 0; i < length; ++i) key.text[i] = text[i];
        std::size_t bar = 0;
        while (bar < length && text[bar] != '|') ++bar;
        if (bar == length) {
            std::uint32_t raw = 0;
            if (!ParseBindTargetBase(std::string(text, length), raw)) return false;
            key.local = raw;
        } else {
            if (bar == 0 || bar + 1 >= length || bar >= FormPluginBytes) return false;
            for (std::size_t i = 0; i < bar; ++i) key.plugin[i] = text[i];
            std::uint32_t local = 0;
            if (!ParseProtectedLocalFormId(text + bar + 1, length - bar - 1, local)) return false;
            key.pluginKnown = true;
            key.local = local;
        }
        _keys[_count++] = key;
        return true;
    }

private:
    std::array<ProtectedFormKey, MaxProtectedForms> _keys{};
    unsigned _count{};
};

// hf-fix (w-probe-r55): forms the rule never sells, merged into every
// run's protected set on top of the user's ProtectedForms. The HearthFires
// building materials are plain Misc records: Straw, Glass and Clay carry only
// VendorItemClutter, Goat Horns carries no keyword and Hinge carries only
// BYOHHouseCraftingCategorySmithing, so neither the vendor-keyword term nor the
// thresholds keep them (the test notes: 5 materials, 49,898
// units admitted on a test save). The adoption gifts (the doll and the
// ten child outfits, BYOHAdoptionToyKeyword / BYOHAdoptionClothesKeyword) are
// cheap plain Misc too. All ten materials are listed, including the five the
// rule already excludes today, so a threshold change cannot expose them.
// Local FormIDs and EditorIDs were read from the HearthFires.esm MISC records
// (sha256 fb1910b9..., masters Skyrim.esm and Update.esm; the read is recorded
// in the test notes). A key whose plugin is not loaded resolves
// to 0 and is skipped: an item of an absent plugin cannot be in the inventory,
// so the skip never opens a sale and never stops one.
struct BuiltinProtectedForm {
    const char* plugin;
    std::uint32_t local;
    const char* editorId;
};
inline constexpr const char* HearthFiresPlugin = "HearthFires.esm";
inline constexpr std::array<BuiltinProtectedForm, 21> BuiltinProtectedForms{{
    // Building materials (10).
    {HearthFiresPlugin, 0x00300E, "BYOHMaterialLog"},
    {HearthFiresPlugin, 0x00300F, "BYOHMaterialNails"},
    {HearthFiresPlugin, 0x003011, "BYOHMaterialHinge"},
    {HearthFiresPlugin, 0x003012, "BYOHMaterialLock"},
    {HearthFiresPlugin, 0x003035, "BYOHMaterialFittingsIron"},
    {HearthFiresPlugin, 0x00303F, "BYOHMaterialHornGoat"},
    {HearthFiresPlugin, 0x003043, "BYOHMaterialClay"},
    {HearthFiresPlugin, 0x00306C, "BYOHMaterialStoneBlock"},
    {HearthFiresPlugin, 0x005A68, "BYOHMaterialStraw"},
    {HearthFiresPlugin, 0x005A69, "BYOHMaterialGlass"},
    // Adoption gifts (11): the doll and the child outfits.
    {HearthFiresPlugin, 0x00C1DE, "BYOHChildrensDoll"},
    {HearthFiresPlugin, 0x00C742, "ClothesChild01Male"},
    {HearthFiresPlugin, 0x00C743, "ClothesChild02Male"},
    {HearthFiresPlugin, 0x00C744, "ClothesChild03Male"},
    {HearthFiresPlugin, 0x00C745, "ClothesChild04Male"},
    {HearthFiresPlugin, 0x00C746, "ClothesChild05Male"},
    {HearthFiresPlugin, 0x00D81E, "ClothesChild03Female"},
    {HearthFiresPlugin, 0x00D81F, "ClothesChild02Female"},
    {HearthFiresPlugin, 0x00D820, "ClothesChild04Female"},
    {HearthFiresPlugin, 0x00D821, "ClothesChild01Female"},
    {HearthFiresPlugin, 0x00D822, "ClothesChild05Female"},
}};
// The resolved set holds the user's keys and the built-in ones, so it is sized
// for both: a full user list can never push a built-in form out.
constexpr unsigned MaxProtectedFormSet =
    MaxProtectedForms + static_cast<unsigned>(BuiltinProtectedForms.size());

// The runtime FormIDs the FormKeys above resolved to, filled once per run.
// Pure, so the host test can drive Contains(); the lookup itself is engine side
// (Probe.cpp passes TESDataHandler::LookupFormID to ResolveProtectedFormSet
// below) and a key that resolves to 0 never reaches this set.
class ProtectedFormSet {
public:
    void Clear() noexcept { _forms = {}; _count = 0; }
    unsigned Count() const noexcept { return _count; }
    void Add(std::uint32_t formId) noexcept {
        if (!formId || _count >= MaxProtectedFormSet) return;
        for (unsigned i = 0; i < _count; ++i) {
            if (_forms[i] == formId) return;
        }
        _forms[_count++] = formId;
    }
    bool Contains(std::uint32_t formId) const noexcept {
        if (!formId) return false;
        for (unsigned i = 0; i < _count; ++i) {
            if (_forms[i] == formId) return true;
        }
        return false;
    }

private:
    std::array<std::uint32_t, MaxProtectedFormSet> _forms{};
    unsigned _count{};
};

// What the built-in half of one resolution did: `added` keys resolved to a
// runtime FormID, `skipped` keys resolved to 0 (their plugin is not loaded).
struct BuiltinProtectionResolution {
    unsigned added{};
    unsigned skipped{};
};

// `lookup(local, plugin)` answers the runtime FormID of a plugin's local
// FormID, or 0 when the plugin is not loaded (TESDataHandler::LookupFormID at
// runtime, a table in the host test). Never fails: a skipped key is counted.
template <class Lookup>
inline BuiltinProtectionResolution AddBuiltinProtectedForms(ProtectedFormSet& out,
                                                            Lookup&& lookup) {
    BuiltinProtectionResolution result;
    for (const auto& form : BuiltinProtectedForms) {
        const std::uint32_t formId = lookup(form.local, form.plugin);
        if (!formId) {
            ++result.skipped;
            continue;
        }
        out.Add(formId);
        ++result.added;
    }
    return result;
}

// One run's protected set: the built-in forms first (a missing plugin is
// skipped, never a refusal), then the user's keys, which stay fail-closed as
// before: a list the builder refused (`keys` holds fewer items than the
// setting, `expectedKeys`) or a key that resolves to 0 answers false, and the
// caller leaves the run's protection facts unknown (facts_unreadable).
template <class Lookup>
inline bool ResolveProtectedFormSet(const ProtectedFormKeys& keys, std::size_t expectedKeys,
                                    Lookup&& lookup, ProtectedFormSet& out,
                                    BuiltinProtectionResolution& builtin) {
    out.Clear();
    builtin = AddBuiltinProtectedForms(out, lookup);
    if (keys.Count() != expectedKeys) return false;
    for (unsigned i = 0; i < keys.Count(); ++i) {
        const auto& key = keys.At(i);
        const std::uint32_t formId = key.pluginKnown ? lookup(key.local, key.plugin.data()) : key.local;
        if (!formId) return false;
        out.Add(formId);
    }
    return true;
}

// R27-2 a: semicolon separated, each item trimmed, no empty item, no
// duplicate, at most MaxProtectedNames items of fewer than TextBytes bytes.
// An empty string is a valid list of zero names (nothing protected).
inline bool ParseProtectedNames(const char* text, ProtectedNames& out) noexcept {
    out.Clear();
    if (!text || !text[0]) return true;
    std::size_t i = 0;
    for (;;) {
        while (text[i] && ProtectionSpace(text[i])) ++i;
        std::size_t end = i;
        while (text[end] && text[end] != ';') ++end;
        std::size_t last = end;
        while (last > i && ProtectionSpace(text[last - 1])) --last;
        if (!out.Add(text + i, last - i)) return false;
        if (!text[end]) return true;
        i = end + 1;
    }
}

// R27-2 a: the same list shape for FormKeys, at most MaxProtectedForms items.
inline bool ParseProtectedForms(const char* text, ProtectedFormKeys& out) noexcept {
    out.Clear();
    if (!text || !text[0]) return true;
    std::size_t i = 0;
    for (;;) {
        while (text[i] && ProtectionSpace(text[i])) ++i;
        std::size_t end = i;
        while (text[end] && text[end] != ';') ++end;
        std::size_t last = end;
        while (last > i && ProtectionSpace(text[last - 1])) --last;
        if (!out.Add(text + i, last - i)) return false;
        if (!text[end]) return true;
        i = end + 1;
    }
}

// R27-2 a / A-01 fix (a review): the protection facts
// are a precondition of the whole probe, not only of the rule reader. An
// unresolvable ProtectedForms key left protectionFactsKnown false -- which does
// exclude every rule candidate as facts_unreadable -- while the probe still
// registered and the BindIssue hand-off, which never consults that flag, could
// still dispatch a sale. Registration is now refused until the facts are
// positively known, and one decision with one reason name is shared by the
// lifecycle row, the register row and the input sink.
struct ProtectionGate {
    bool probeAllowed{};
    const char* reason{"PROTECTION_PENDING"};
};
// `resolved` is false only while the data handler is not populated yet, which
// the next lifecycle message retries; a resolved-but-unknown refusal is final.
inline ProtectionGate DecideProtectionGate(bool resolved, bool factsKnown) noexcept {
    if (factsKnown) return {true, "PROTECTION_OK"};
    return {false, resolved ? "PROTECTION_UNRESOLVED" : "PROTECTION_PENDING"};
}

// R27-2 b: a Misc base is junk unless it carries one of these vendor keywords,
// checked with the keyword form's HasKeywordString
// (vendor/commonlibsse-ng/include/RE/B/BGSKeywordForm.h:35) with the names
// exact. Any of them means the item is a vendor-classified thing rather than
// clutter, so the rule path leaves it kUnknown and Selector.h's own type term
// excludes it. VendorItemClutter is deliberately absent: a clutter Misc item
// stays junk.
inline constexpr std::array<const char*, 6> MiscNonJunkKeywords{{
    "VendorItemGem",
    "VendorItemOreIngot",
    "VendorItemAnimalHide",
    "VendorItemTool",
    "VendorItemAnimalPart",
    "VendorItemDaedricArtifact",
}};

// The index of the first keyword in MiscNonJunkKeywords that `carries` answers
// true for, or -1 when none matched. The runtime passes a lambda that calls
// the engine's HasKeywordString; the host test passes a set lookup, so the
// keyword names and the decision are driven offline.
template <class Predicate>
inline int FirstNonJunkKeywordIndex(Predicate&& carries) noexcept {
    for (std::size_t i = 0; i < MiscNonJunkKeywords.size(); ++i) {
        if (carries(MiscNonJunkKeywords[i])) return static_cast<int>(i);
    }
    return -1;
}

// The classification R27-2 b pins: no keyword matched means plain Misc, which
// this stage may issue as junk; any match means the base is not junk and the
// selector's type term excludes it.
inline CandidateType MiscTypeForKeywordIndex(int keywordIndex) noexcept {
    return keywordIndex < 0 ? CandidateType::kJunk : CandidateType::kUnknown;
}
}
