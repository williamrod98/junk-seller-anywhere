#pragma once
// w-probe-r9 (R9-2): the foreground gate a sale attempt must pass before it
// may be issued. Engine free on purpose, so the whole truth table is
// host-testable without CommonLib or the game.
// w-probe-r10 (R10-1): the `topIsBarter` term is dropped entirely, not
// replaced by a negative form. R-W5 measured `GetTopMostMenu` returning null
// while the BarterMenu was the only and topmost menu (js_w_phase
// topmostMenu="none", openMenus="BarterMenu=1,..."), so the term could never
// pass and is not a usable signal for this probe. The remaining five terms
// already state what "foreground" means here.
#include <string>

namespace js {

// barter is true when the BarterMenu is obtained. The remaining four are
// true when the named menu is open, which is a fail condition for the gate.
// R9-1 removed the game-paused term: the BarterMenu itself pauses the game,
// so requiring !GameIsPaused() made the gate impossible to pass while the
// BarterMenu was open. `paused` is not a term of this gate; it stays in the
// phase snapshot (WPhaseInfo, and w-probe-r10's per-attempt snapshot) as
// information only. R10-1 dropped `topIsBarter` for the same reason: the
// `top` pointer is still recorded (as `topmostMenu`) but never gates.
struct ForegroundGate {
    bool barter{}, modal{}, quantity{}, messageBox{}, console{};
};

inline bool ForegroundGateOk(const ForegroundGate& gate) noexcept {
    return gate.barter && !gate.modal && !gate.quantity &&
           !gate.messageBox && !gate.console;
}

// R9-3 (R10-1 dropped the "top" term): the fixed-order, comma-joined names of
// the failed terms, so a live run can name the blocking term instead of
// inferring it from openMenus. Empty when the gate passed.
inline std::string ForegroundGateReason(const ForegroundGate& gate) noexcept {
    std::string reason;
    auto add = [&reason](const char* term) {
        if (!reason.empty()) reason += ',';
        reason += term;
    };
    if (!gate.barter) add("barter");
    if (gate.modal) add("modal");
    if (gate.quantity) add("quantity");
    if (gate.messageBox) add("messageBox");
    if (gate.console) add("console");
    return reason;
}

}
