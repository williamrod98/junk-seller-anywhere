#pragma once
// Engine free name rules. Three users share them: the call trace exclude list
// (r4 / probe-js02 anomaly 2), the per name counters the receipt publishes, and
// the Papyrus event gate. Nothing here includes an engine header, so the offline
// harness drives exactly the code the runtime uses.
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
namespace js {
constexpr unsigned NameBytes = 64;
// At most this many names may be excluded from the trace ring by the INI.
constexpr unsigned MaxExcludedNames = 16;
// At most this many distinct names are counted by name. A name past the cap is
// counted in the overflow instead of silently vanishing.
constexpr unsigned MaxNameCounters = 24;

template <std::size_t N>
inline void CopyText(std::array<char, N>& destination, const char* source) noexcept {
    destination = {};
    if (!source) return;
    std::size_t i = 0;
    for (; i + 1 < N && source[i]; ++i) destination[i] = source[i];
    destination[i] = '\0';
}

inline char LowerAscii(char value) noexcept {
    return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
}

// Case insensitive ASCII compare, null safe. BSFixedString compares without
// case, so a gate on an event name must not be stricter than the sender.
inline bool NameEqualsNoCase(const char* left, const char* right) noexcept {
    if (!left || !right) return false;
    std::size_t i = 0;
    for (; left[i] && right[i]; ++i) {
        if (LowerAscii(left[i]) != LowerAscii(right[i])) return false;
    }
    return left[i] == right[i];
}

// Exact compare against a fixed field. Scaleform dispatches by exact name, so
// the exclude list is case sensitive on purpose: a near miss must stay visible
// in the trace instead of being silently swallowed.
template <std::size_t N>
inline bool NameEquals(const std::array<char, N>& field, const char* name) noexcept {
    if (!name) return false;
    std::size_t i = 0;
    for (; i + 1 < N && field[i] && name[i]; ++i) {
        if (field[i] != name[i]) return false;
    }
    return field[i] == '\0' && name[i] == '\0';
}

// Exact compare of two C strings, null safe. Scaleform dispatches by exact
// name, so anything that counts a dispatch has to compare the same way.
inline bool NameEqualsExact(const char* left, const char* right) noexcept {
    if (!left || !right) return false;
    std::size_t i = 0;
    for (; left[i] && right[i]; ++i) {
        if (left[i] != right[i]) return false;
    }
    return left[i] == right[i];
}

// A Scaleform method name as the INI may spell it: letters, digits, underscore.
inline bool NameCharUsable(char value) noexcept {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '_';
}

// The names the trace ring refuses. Fixed size and copied by value, so the hook
// thread only reads it after the INI was parsed at load time.
class NameFilter {
public:
    bool Excluded(const char* name) const noexcept {
        if (!name) return false;
        for (unsigned i = 0; i < _count; ++i) {
            if (NameEquals(_names[i], name)) return true;
        }
        return false;
    }
    bool Add(const char* name, std::size_t length) noexcept {
        if (!name || !length || length >= NameBytes || _count >= MaxExcludedNames) return false;
        for (std::size_t i = 0; i < length; ++i) {
            if (!NameCharUsable(name[i])) return false;
        }
        std::array<char, NameBytes> candidate{};
        for (std::size_t i = 0; i < length; ++i) candidate[i] = name[i];
        if (Excluded(candidate.data())) return false;  // duplicates refuse the load
        _names[_count++] = candidate;
        return true;
    }
    void Clear() noexcept { _names = {}; _count = 0; }
    unsigned Count() const noexcept { return _count; }
    const char* At(unsigned index) const noexcept {
        return index < _count ? _names[index].data() : "";
    }

private:
    std::array<std::array<char, NameBytes>, MaxExcludedNames> _names{};
    unsigned _count{};
};

// Strict list parser: comma separated, no spaces, no empty item, no duplicate,
// at most MaxExcludedNames names. An empty string is a valid list of zero names
// and turns the filter off. Anything else refuses the INI, like every other key.
inline bool ParseNameList(const char* text, NameFilter& out) noexcept {
    out.Clear();
    if (!text || !text[0]) return true;
    std::size_t begin = 0, i = 0;
    for (;; ++i) {
        if (text[i] && text[i] != ',') continue;
        if (!out.Add(text + begin, i - begin)) return false;
        if (!text[i]) return true;
        begin = i + 1;
    }
}

// Counts by name, published in HEALTH and on the barter close row. Producers run
// on the hook thread; a slot is reserved with one compare exchange and only
// becomes visible to a reader after its name is written. A genuine race between
// two threads adding the same new name can split it across two slots, which the
// reader sums; nothing is lost and no lock is taken on an engine thread.
class NameCounters {
public:
    void Add(const char* name) noexcept {
        if (!name) return;
        const auto published = _size.load(std::memory_order_acquire);
        if (Bump(name, 0, published)) return;
        unsigned index = published;
        while (index < MaxNameCounters) {
            if (_size.compare_exchange_weak(index, index + 1, std::memory_order_acq_rel)) {
                CopyText(_entries[index].name, name);
                _entries[index].ready.store(true, std::memory_order_release);
                _entries[index].count.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (Bump(name, published, index)) return;
        }
        _overflow.fetch_add(1, std::memory_order_relaxed);
    }
    unsigned Size() const noexcept {
        const auto size = _size.load(std::memory_order_acquire);
        return size < MaxNameCounters ? size : MaxNameCounters;
    }
    bool Ready(unsigned index) const noexcept {
        return index < MaxNameCounters && _entries[index].ready.load(std::memory_order_acquire);
    }
    const char* NameAt(unsigned index) const noexcept {
        return index < MaxNameCounters ? _entries[index].name.data() : "";
    }
    std::uint64_t CountAt(unsigned index) const noexcept {
        return index < MaxNameCounters ? _entries[index].count.load(std::memory_order_relaxed) : 0;
    }
    std::uint64_t Overflow() const noexcept { return _overflow.load(std::memory_order_relaxed); }
    std::uint64_t Total() const noexcept {
        std::uint64_t total = _overflow.load(std::memory_order_relaxed);
        for (unsigned i = 0; i < Size(); ++i) total += CountAt(i);
        return total;
    }
    // Harness only: the runtime never clears a counter.
    void Reset() noexcept {
        for (auto& entry : _entries) {
            entry.ready.store(false, std::memory_order_relaxed);
            entry.count.store(0, std::memory_order_relaxed);
            entry.name = {};
        }
        _overflow.store(0, std::memory_order_relaxed);
        _size.store(0, std::memory_order_release);
    }

private:
    struct Entry {
        std::array<char, NameBytes> name{};
        std::atomic<std::uint64_t> count{};
        std::atomic<bool> ready{};
    };
    bool Bump(const char* name, unsigned first, unsigned last) noexcept {
        const unsigned end = last < MaxNameCounters ? last : MaxNameCounters;
        for (unsigned i = first; i < end; ++i) {
            if (!_entries[i].ready.load(std::memory_order_acquire)) continue;
            if (NameEquals(_entries[i].name, name)) {
                _entries[i].count.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        return false;
    }
    std::array<Entry, MaxNameCounters> _entries{};
    std::atomic<unsigned> _size{};
    std::atomic<std::uint64_t> _overflow{};
};

// r4 / probe-js02 anomaly 1. SKSE has two mod event mechanisms and only one of
// them has a ModCallbackEvent to dispatch:
//   Form.SendModEvent(eventName, strArg, numArg) -> SKSEModCallbackEvent, which
//     is what BSTEventSink<SKSE::ModCallbackEvent> receives (the struct carries
//     exactly eventName / strArg / numArg / sender);
//   ModEvent.Create + Push* + Send -> an arbitrary argument list delivered
//     straight to the Papyrus RegisterForModEvent registrations. There is no
//     ModCallbackEvent for it, so no plugin sink can ever see it.
// r3 used the second one, which is why js_papyrus stayed at zero rows while the
// quest was demonstrably alive.
constexpr const char* PapyrusEventName = "AGH_JunkSeller";
inline bool PapyrusEventMatches(const char* eventName) noexcept {
    return NameEqualsNoCase(eventName, PapyrusEventName);
}
}
