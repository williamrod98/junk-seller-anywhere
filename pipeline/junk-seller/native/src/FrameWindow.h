#pragma once
// v0.5.0 (the design notes, "the +5.7 s frame"): the
// engine-free half of an OBSERVE-ONLY receipt. After a big sell-anywhere
// press the live runs r61-r64 measured one long frame (55 ms in r61, 104 ms
// in r64) about 5.7 s after the key, growing with the rows sold, never after
// a small press. This window records what the product and the world do in
// that time, so a live run can name the source; it never changes a sale.
//
// Armed (Probe.cpp, only with the base INI key DevFrameWindow=1) when a
// planned press ends: the key edge, the conversion end, the receipt rows the
// press queued. From then on the writer pump re-queues one SKSE main-thread
// task after the previous one ran; each task is one "tick". The gap between
// two ticks is a TASK-SAMPLING INTERVAL, not a frame duration (audit B-01 of
// the v0.5.0 review): SKSE drains its queue once per frame, but the pump runs
// on the receipt writer thread (a 10 ms loop that also writes rows), so a
// fast frame can be skipped and a delayed pump can show a long interval while
// the frames stay short. It bounds the main-thread frame from above; the
// frame durations themselves are PresentMon's. The ticks from +5 s to +7 s after the key are the sampler window. The
// sinks add the events they see meanwhile (container changes, tracked
// stats, menus, saves). The first tick past +7 s ends the window: one
// js_frame_window row.
//
// Nothing here includes an engine header.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace js {

// The sampler window, as offsets from the key edge (the plan's "+5 s to
// +7 s"), the long-frame bound of section 4.4 (50 ms), and the latest a
// window may end (a tick that never comes back still ends it: a load, a
// menu that pauses the task queue).
inline constexpr std::uint64_t FrameWindowStartUs = 5000000;
inline constexpr std::uint64_t FrameWindowEndUs = 7000000;
inline constexpr std::uint64_t FrameWindowLongFrameUs = 50000;
inline constexpr std::size_t FrameWindowTopGaps = 8;
inline constexpr std::size_t FrameWindowNamedEvents = 8;
inline constexpr std::size_t FrameWindowNameBytes = 24;

enum class FrameEventKind : unsigned { kContainer, kStat, kMenuOpen, kMenuClose, kSave };

inline const char* FrameEventKindName(FrameEventKind kind) noexcept {
    switch (kind) {
    case FrameEventKind::kContainer: return "container";
    case FrameEventKind::kStat: return "stat";
    case FrameEventKind::kMenuOpen: return "menuOpen";
    case FrameEventKind::kMenuClose: return "menuClose";
    case FrameEventKind::kSave: return "save";
    }
    return "container";
}

// One long gap: the offset (ms from the key edge) of the tick that ended it
// and the gap itself.
struct FrameGap {
    std::int64_t atMs{-1};
    std::uint64_t gapUs{};
};

// One named event (a tracked stat, a menu, a save) with its offset.
struct FrameNamedEvent {
    FrameEventKind kind{FrameEventKind::kStat};
    std::int64_t atMs{-1};
    std::array<char, FrameWindowNameBytes> name{};
};

// What a tick reads on the main thread: the player's Speech skill as the
// PlayerSkills block holds it (the receipt's speechLevel, which lagged the
// console in r63 / r64) and as the actor value base holds it, the character
// level and the character XP. -1 / negative = unreadable.
struct FrameTickFacts {
    std::int32_t speechLevel{-1};
    float speechBase{-1.0F};
    std::int32_t playerLevel{-1};
    float playerXp{-1.0F};
};

struct FrameWindow {
    bool armed{};
    bool done{};
    const char* end{"none"};  // how the window ended: "complete", "load", "timeout"
    std::uint64_t runToken{};
    std::uint64_t edgeUs{};
    std::uint64_t convertEndUs{};
    // The press, for the row: rows and units sold, instance lists removed,
    // Speech XP calls and points (units x value summed), gold paid.
    std::uint32_t rowsSold{};
    std::int64_t unitsSold{};
    std::int64_t listsSold{};
    std::uint64_t xpCalls{};
    double xpPoints{};
    std::int64_t goldPaid{};
    // The product's own deferred work: the receipt rows the press queued are
    // written by the writer thread; `receiptTarget` is the writer's accepted
    // count at the arm, `receiptDrainAtMs` the first tick that saw them all
    // delivered (-1 = not seen).
    std::uint64_t receiptTarget{};
    std::int64_t receiptDrainAtMs{-1};
    // The one HUD line of the press is shown by an SKSE task the press
    // queued (Probe.cpp SurfaceResultLine); its offset (-1 = not seen).
    std::int64_t lineShownAtMs{-1};
    // The ticks.
    std::uint64_t lastTickUs{};
    std::uint32_t ticks{};
    std::uint32_t ticksInWindow{};
    std::uint32_t longFramesInWindow{};
    std::uint64_t maxGapUs{};           // in the window
    std::int64_t maxGapAtMs{-1};
    std::uint64_t maxGapBeforeUs{};     // from the arm to +5 s (the control)
    std::int64_t maxGapBeforeAtMs{-1};
    std::uint64_t windowSumUs{};
    std::array<FrameGap, FrameWindowTopGaps> top{};  // the longest gaps in the window, longest first
    std::uint32_t topCount{};
    // Speech, character level and XP at the arm and when they first changed.
    FrameTickFacts atArm{};
    FrameTickFacts last{};
    std::int64_t speechLevelChangeAtMs{-1};
    std::int64_t speechBaseChangeAtMs{-1};
    std::int64_t playerLevelChangeAtMs{-1};
    std::int64_t playerXpChangeAtMs{-1};
    // Container changes that name the player (outside the barter menu), from
    // the press's first slice on: during the conversion and after its end.
    std::uint32_t containerDuring{};
    std::uint32_t containerAfter{};
    std::int64_t containerFirstAfterAtMs{-1};
    std::int64_t containerLastAfterAtMs{-1};
    // Tracked stats, menus and saves, in arrival order (bounded).
    std::array<FrameNamedEvent, FrameWindowNamedEvents> named{};
    std::uint32_t namedCount{};
    std::uint32_t namedDropped{};
};

inline std::int64_t FrameOffsetMs(const FrameWindow& window, std::uint64_t nowUs) noexcept {
    if (nowUs < window.edgeUs) return 0;
    return static_cast<std::int64_t>((nowUs - window.edgeUs) / 1000);
}

// Starts a window for one ended press (the caller fills the press facts).
inline void ArmFrameWindow(FrameWindow& window, std::uint64_t runToken, std::uint64_t edgeUs, std::uint64_t endUs,
                           std::uint64_t receiptTarget, const FrameTickFacts& facts) noexcept {
    window = FrameWindow{};
    window.armed = true;
    window.runToken = runToken;
    window.edgeUs = edgeUs;
    window.convertEndUs = endUs;
    window.lastTickUs = endUs;
    window.receiptTarget = receiptTarget;
    window.atArm = facts;
    window.last = facts;
}

// Keeps the FrameWindowTopGaps longest gaps, longest first.
inline void KeepTopFrameGap(FrameWindow& window, FrameGap gap) noexcept {
    std::size_t at = window.topCount;
    while (at > 0 && window.top[at - 1].gapUs < gap.gapUs) --at;
    if (at >= FrameWindowTopGaps) return;
    const std::size_t last = window.topCount < FrameWindowTopGaps ? window.topCount : FrameWindowTopGaps - 1;
    for (std::size_t i = last; i > at; --i) window.top[i] = window.top[i - 1];
    window.top[at] = gap;
    if (window.topCount < FrameWindowTopGaps) ++window.topCount;
}

// One main-thread tick at `nowUs` with the facts it read and the writer's
// delivered count. A gap is attributed to the tick that ends it. Returns
// true when the window is over (the first tick past +7 s): the caller writes
// the row once and disarms.
inline bool ObserveFrameTick(FrameWindow& window, std::uint64_t nowUs, const FrameTickFacts& facts,
                             std::uint64_t delivered) noexcept {
    if (!window.armed || window.done) return window.done;
    const std::uint64_t gap = nowUs > window.lastTickUs ? nowUs - window.lastTickUs : 0;
    window.lastTickUs = nowUs;
    ++window.ticks;
    const std::int64_t at = FrameOffsetMs(window, nowUs);
    const std::uint64_t offset = nowUs > window.edgeUs ? nowUs - window.edgeUs : 0;
    if (window.receiptDrainAtMs < 0 && delivered >= window.receiptTarget) window.receiptDrainAtMs = at;
    if (window.speechLevelChangeAtMs < 0 && facts.speechLevel != window.atArm.speechLevel) window.speechLevelChangeAtMs = at;
    if (window.speechBaseChangeAtMs < 0 && facts.speechBase != window.atArm.speechBase) window.speechBaseChangeAtMs = at;
    if (window.playerLevelChangeAtMs < 0 && facts.playerLevel != window.atArm.playerLevel) window.playerLevelChangeAtMs = at;
    if (window.playerXpChangeAtMs < 0 && facts.playerXp != window.atArm.playerXp) window.playerXpChangeAtMs = at;
    window.last = facts;
    if (offset < FrameWindowStartUs) {
        if (gap > window.maxGapBeforeUs) {
            window.maxGapBeforeUs = gap;
            window.maxGapBeforeAtMs = at;
        }
        return false;
    }
    if (offset > FrameWindowEndUs) {
        // The gap that crosses the window's end still belongs to it (a long
        // frame at +6.9 s ends past +7 s).
        if (offset - gap <= FrameWindowEndUs) {
            ++window.ticksInWindow;
            window.windowSumUs += gap;
            if (gap >= FrameWindowLongFrameUs) ++window.longFramesInWindow;
            if (gap > window.maxGapUs) {
                window.maxGapUs = gap;
                window.maxGapAtMs = at;
            }
            KeepTopFrameGap(window, FrameGap{at, gap});
        }
        window.done = true;
        window.end = "complete";
        return true;
    }
    ++window.ticksInWindow;
    window.windowSumUs += gap;
    if (gap >= FrameWindowLongFrameUs) ++window.longFramesInWindow;
    if (gap > window.maxGapUs) {
        window.maxGapUs = gap;
        window.maxGapAtMs = at;
    }
    KeepTopFrameGap(window, FrameGap{at, gap});
    return false;
}

// Ends a window that can no longer complete (a load, no tick for too long).
inline void EndFrameWindow(FrameWindow& window, const char* reason) noexcept {
    if (!window.armed || window.done) return;
    window.done = true;
    window.end = reason;
}

// A container change naming the player, seen by the sink at `nowUs`.
inline void ObserveFrameContainer(FrameWindow& window, std::uint64_t nowUs) noexcept {
    if (!window.armed || window.done) return;
    const std::int64_t at = FrameOffsetMs(window, nowUs);
    ++window.containerAfter;
    if (window.containerFirstAfterAtMs < 0) window.containerFirstAfterAtMs = at;
    window.containerLastAfterAtMs = at;
}

// A named event (a tracked stat, a menu, a save) at `nowUs`; the name is cut
// to the buffer. Past the bound the event is only counted.
inline void ObserveFrameNamed(FrameWindow& window, FrameEventKind kind, const char* name, std::uint64_t nowUs) noexcept {
    if (!window.armed || window.done) return;
    if (window.namedCount >= FrameWindowNamedEvents) {
        ++window.namedDropped;
        return;
    }
    auto& event = window.named[window.namedCount++];
    event.kind = kind;
    event.atMs = FrameOffsetMs(window, nowUs);
    event.name.fill('\0');
    if (name) {
        std::size_t n = 0;
        while (name[n] && n + 1 < FrameWindowNameBytes) {
            const char c = name[n];
            event.name[n] = (c >= 32 && c < 127 && c != '"' && c != '\\') ? c : '?';
            ++n;
        }
    }
}

}  // namespace js
