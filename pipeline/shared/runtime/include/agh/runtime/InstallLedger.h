#pragma once
// agh/runtime/InstallLedger.h -- pure install ledger for SKSEPlugin_Load (E1 design r3).
//
// Shipped shared header, moved unchanged in behaviour from the
// design prototype (the E1 design r3.1 section 1.6, ruling
// D1). ASCII only, no CommonLib, host-compiled.
//
// Ruling D1: every failure path of SKSEPlugin_Load writes a receipt row and
// returns true, and leaves the plugin INERT: no hook, event sink, trampoline
// write or Papyrus registration done before the failure stays active.
//
// A DLL describes its load as a constexpr plan (one StepSpec per step, in
// order) and binds one Ops object to it. RunInstall() is the whole body of
// SKSEPlugin_Load:
//   - every step runs inside try/catch; a false result, a thrown exception or
//     an exception after the last step is one failure path;
//   - on failure it disarms the latch, undoes every reversible step already
//     done (newest first), writes ONE receipt row naming the failed step and
//     the reason, and returns true;
//   - steps that SKSE cannot undo (Papyrus Register, RegisterListener, a
//     started thread) are "resident": the callback they install checks the
//     latch first, so a disarmed plugin's callbacks do nothing. Each such step
//     names its safe resident state in the plan, and ValidatePlan() refuses a
//     plan that omits it.
// The Fault parameter is the host-test seam: it forces a failure (false or
// throw) after step k, for every k. Shipped DLLs never pass it.
//
// r3: a step that makes MORE than one registration call (SW's
// four AddEventSink calls, a batch of Papyrus natives, a set of sinks inside
// a listener) is a COMPOUND step. Its Ops::Do runs RunCompound() below, which
// is transactional: on a false result or a throw from sub-call j it removes
// sub-calls j, j-1, ..., 0 in that order (the failing one first, because it
// may have taken effect before it failed; each Remove is a no-op when its
// registration is absent, as BSTEventSource::RemoveEventSink is), leaves the
// compound's commit latch disarmed and returns a failure naming the sub-call.
// Nothing catches a sub-call failure and continues: a per-sub-call catch that
// logs and moves on (today's Watch.cpp:549-556) is forbidden and the latch
// gate (pipeline/shared/runtime/tests/latch_gate.py) fails such a site. A compound that runs
// after SKSEPlugin_Load returned (a retry in a listener, a Papyrus callback)
// reports its failure through LateFail(), the same disarm + undo + receipt
// path as RunInstall's.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace agh::rt {

enum class StepKind : std::uint8_t {
    kCheck,    // decides only; no state outside the DLL's own memory
    kReceipt,  // opens or writes the DLL's own receipt; kept as evidence
    kInstall,  // registers something with SKSE or the game, before the latch
    kBranch,   // a designed disabled mode: may end the plan early as a success
    kArm,      // the single latch store; cannot fail
    kArmed,    // runs only after the latch is set
};

enum class Undo : std::uint8_t {
    kNone,        // nothing to undo (checks, receipt rows, pure writes)
    kReversible,  // Ops::Undo(i) removes it (event sink -> RemoveEventSink)
    kResident,    // cannot be undone; its callback checks the latch first
};

struct StepSpec {
    std::string_view name;
    StepKind kind;
    Undo undo;
    std::string_view resident;  // safe resident state; required for kResident and kReceipt
};

enum class StepResult : std::uint8_t { kOk, kFail, kStopDisabled };

enum class PlanError : std::uint8_t {
    kNone,
    kEmpty,
    kArmCount,             // not exactly one kArm step
    kCheckAfterInstall,    // a decision runs after something was installed
    kReceiptAfterInstall,  // an install would have no receipt writer if it failed
    kInstallAfterArm,      // an install or branch after the latch
    kArmedBeforeArm,       // an armed-only step before the latch
    kMissingResidentState, // a resident or receipt step without its safe state
    kBadUndo,              // check/branch/arm with an undo, or install without one
};

[[nodiscard]] constexpr PlanError ValidatePlan(std::span<const StepSpec> plan) noexcept
{
    if (plan.empty()) { return PlanError::kEmpty; }
    std::size_t arms = 0;
    bool installed = false;
    bool armed = false;
    for (const auto& s : plan) {
        switch (s.kind) {
        case StepKind::kCheck:
            if (installed) { return PlanError::kCheckAfterInstall; }
            if (armed) { return PlanError::kInstallAfterArm; }
            if (s.undo != Undo::kNone) { return PlanError::kBadUndo; }
            break;
        case StepKind::kReceipt:
            if (installed || armed) { return PlanError::kReceiptAfterInstall; }
            if (s.undo != Undo::kNone) { return PlanError::kBadUndo; }
            if (s.resident.empty()) { return PlanError::kMissingResidentState; }
            break;
        case StepKind::kInstall:
            if (armed) { return PlanError::kInstallAfterArm; }
            if (s.undo == Undo::kNone) { return PlanError::kBadUndo; }
            installed = true;
            break;
        case StepKind::kBranch:
            if (armed) { return PlanError::kInstallAfterArm; }
            if (s.undo != Undo::kNone) { return PlanError::kBadUndo; }
            break;
        case StepKind::kArm:
            ++arms;
            armed = true;
            if (s.undo != Undo::kNone) { return PlanError::kBadUndo; }
            break;
        case StepKind::kArmed:
            if (!armed) { return PlanError::kArmedBeforeArm; }
            break;
        }
        if (s.undo == Undo::kResident && s.resident.empty()) { return PlanError::kMissingResidentState; }
    }
    if (arms != 1) { return PlanError::kArmCount; }
    // Every receipt step must precede every install (checked above); at least
    // one receipt must exist before the first install so a later failure is
    // written through the DLL's own writer.
    bool receipt = false;
    for (const auto& s : plan) {
        if (s.kind == StepKind::kReceipt) { receipt = true; }
        if (s.kind == StepKind::kInstall && !receipt) { return PlanError::kReceiptAfterInstall; }
    }
    return PlanError::kNone;
}

// The arm latch. Every resident callback (messaging listener, Papyrus
// registration callback, natives, event sinks, hook thunks, worker threads)
// reads Armed() as its FIRST statement and returns its inactive value (a
// hook thunk: the original call) when it is false. The latch gate
// (pipeline/shared/runtime/tests/latch_gate.py) proves this statically for every registration
// site it finds (design section 1.6, design r2 ruling 1).
class Latch {
public:
    [[nodiscard]] bool Armed() const noexcept { return armed_.load(std::memory_order_acquire); }
    void Arm() noexcept { armed_.store(true, std::memory_order_release); }
    void Disarm() noexcept { armed_.store(false, std::memory_order_release); }
    // r3: the first failure (at load or later) claims the one receipt; a
    // second LateFail() of the same plugin disarms again but writes nothing.
    [[nodiscard]] bool ClaimFailure() noexcept { return !failed_.exchange(true, std::memory_order_acq_rel); }
    [[nodiscard]] bool Failed() const noexcept { return failed_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> armed_{ false };
    std::atomic<bool> failed_{ false };
};

enum class FaultMode : std::uint8_t { kNone, kFail, kThrow };
struct Fault {
    std::size_t after{ 0 };  // inject after this many steps ran (0 = before the first, plan.size() = after the last)
    FaultMode mode{ FaultMode::kNone };
};

struct InstallOutcome {
    bool loadReturn{ true };          // always true (G-07: never SKSE's "Exit game?" box)
    bool failed{ false };
    bool disabled{ false };           // a kBranch step chose the designed disabled mode
    std::size_t failedIndex{ 0 };     // index of the failing step; plan.size() = the tail after the last step
    std::string_view failedStep{};
    std::string_view reason{};
    std::string_view failedSub{};     // r3: the sub-call of a compound step that failed ("" if none)
    std::size_t undone{ 0 };
    std::size_t done{ 0 };            // r3: steps completed; a DLL keeps it for LateFail()
};

inline constexpr std::string_view kTailStep = "tail";
inline constexpr std::string_view kReasonFailed = "STEP_FAILED";
inline constexpr std::string_view kReasonException = "EXCEPTION";
inline constexpr std::string_view kReasonForced = "FORCED_FAULT";

struct ForcedFault {};  // thrown by the seam in kThrow mode

// ---------------------------------------------------------------------------
// Compound steps (r3).
//
// A compound step is one plan step made of several registration sub-calls.
// Each sub-call is reversible (an event sink: RemoveEventSink) or resident
// (a Papyrus RegisterFunction: SKSE cannot unregister it; the native reads
// the plugin latch first, so the caller's disarm makes it inert).
enum class SubUndo : std::uint8_t {
    kReversible,  // SubOps::Remove(j) takes it back; Remove of an absent registration is a no-op
    kResident,    // cannot be taken back; its callback reads the plugin latch first
};

struct SubSpec {
    std::string_view name;
    SubUndo undo;
};

struct CompoundOutcome {
    StepResult result{ StepResult::kOk };
    std::size_t failedSub{ 0 };        // index of the failing sub-call
    std::string_view failedSubName{};
    std::string_view reason{};         // kReasonFailed or kReasonException
    std::size_t removed{ 0 };          // Remove() calls made (the failing sub-call included)
    std::size_t residentLeft{ 0 };     // resident sub-calls that ran (inert only through the plugin latch)
};

// SubOps contract:
//   bool Add(std::size_t j);              may throw; false or a throw = this sub-call failed,
//                                         with or without having taken effect
//   void Remove(std::size_t j) noexcept;  reversible sub-calls only; a no-op when absent
//
// `commit` (optional) is the compound's own latch: every callback installed by
// the compound reads it (after the plugin latch) as part of its first
// statement, so a partly registered compound delivers nothing. It is disarmed
// on entry and armed only after the last sub-call succeeded.
template <class SubOps>
CompoundOutcome RunCompound(std::span<const SubSpec> subs, SubOps& ops, Latch* commit = nullptr) noexcept
{
    CompoundOutcome outcome;
    if (commit) { commit->Disarm(); }
    std::size_t current = 0;
    const auto rollback = [&](std::string_view reason) noexcept {
        for (std::size_t m = current + 1; m-- > 0;) {
            if (subs[m].undo == SubUndo::kReversible) {
                ops.Remove(m);
                ++outcome.removed;
            } else {
                ++outcome.residentLeft;
            }
        }
        outcome.result = StepResult::kFail;
        outcome.failedSub = current;
        outcome.failedSubName = subs[current].name;
        outcome.reason = reason;
        return outcome;
    };
    for (current = 0; current < subs.size(); ++current) {
        bool ok = false;
        try {
            ok = ops.Add(current);
        } catch (...) {
            return rollback(kReasonException);
        }
        if (!ok) { return rollback(kReasonFailed); }
    }
    if (commit) { commit->Arm(); }
    return outcome;
}

// Ops contract:
//   StepResult Do(std::size_t i);                                   may throw
//   void Undo(std::size_t i) noexcept;                              reversible steps only
//   void Receipt(std::string_view step, std::string_view reason, std::string_view sub) noexcept;
//        writes one failure row: through the DLL's writer once a kReceipt step
//        ran, else a one-line synchronous append to the same receipt path.
//        `sub` names the failed sub-call of a compound step ("" otherwise);
//        a compound Do() stores its CompoundOutcome so Receipt can name it.
//   SubFailure FailedSub() const noexcept;     {} unless the last Do() was a failed compound;
//        its reason (EXCEPTION when the sub-call threw) replaces STEP_FAILED in the row.
struct SubFailure {
    std::string_view name{};
    std::string_view reason{};
};
namespace detail {
template <class Ops>
void FailInert(std::span<const StepSpec> plan, std::size_t done, Ops& ops, Latch& latch, std::string_view step,
    std::string_view reason, std::string_view sub, InstallOutcome& outcome) noexcept
{
    latch.Disarm();
    for (std::size_t j = done; j-- > 0;) {
        if (plan[j].undo == Undo::kReversible) {
            ops.Undo(j);
            ++outcome.undone;
        }
    }
    outcome.failed = true;
    outcome.failedStep = step;
    outcome.reason = reason;
    outcome.failedSub = sub;
    if (latch.ClaimFailure()) { ops.Receipt(step, reason, sub); }
}
}  // namespace detail

template <class Ops>
InstallOutcome RunInstall(std::span<const StepSpec> plan, Ops& ops, Latch& latch, Fault fault = {}) noexcept
{
    InstallOutcome outcome;
    std::size_t done = 0;  // steps that completed
    const auto fail = [&](std::size_t index, std::string_view reason) noexcept {
        outcome.failedIndex = index;
        outcome.done = done;
        SubFailure sub{};
        if (index < plan.size() && reason == kReasonFailed) { sub = ops.FailedSub(); }
        detail::FailInert(plan, done, ops, latch, index < plan.size() ? plan[index].name : kTailStep,
            sub.reason.empty() ? reason : sub.reason, sub.name, outcome);
        return outcome;
    };
    std::size_t current = 0;
    try {
        for (current = 0; current < plan.size(); ++current) {
            if (fault.mode != FaultMode::kNone && fault.after == current) {
                if (fault.mode == FaultMode::kThrow) { throw ForcedFault{}; }
                return fail(current, kReasonForced);
            }
            const auto& step = plan[current];
            if (step.kind == StepKind::kArm) {
                latch.Arm();
                ++done;
                continue;
            }
            const StepResult r = ops.Do(current);
            if (r == StepResult::kFail) { return fail(current, kReasonFailed); }
            ++done;
            if (r == StepResult::kStopDisabled && step.kind == StepKind::kBranch) {
                latch.Arm();  // the designed disabled mode keeps what ran so far active
                outcome.disabled = true;
                outcome.done = done;
                return outcome;
            }
        }
        if (fault.mode != FaultMode::kNone && fault.after == plan.size()) {
            if (fault.mode == FaultMode::kThrow) { throw ForcedFault{}; }
            return fail(plan.size(), kReasonForced);
        }
    } catch (...) {
        // The outer catch of SKSEPlugin_Load: also a receipt + inert + true.
        return fail(current, kReasonException);
    }
    outcome.done = done;
    return outcome;
}

// r3: a failure AFTER SKSEPlugin_Load returned (a compound retried in the
// latched listener, a Papyrus registration callback whose native batch
// failed). Same path as a load-time failure: disarm, undo every reversible
// plan step that completed at load (newest first), one receipt. `done` is
// InstallOutcome::done of the plugin's RunInstall. Idempotent: a second
// call disarms again and writes no second receipt.
template <class Ops>
InstallOutcome LateFail(std::span<const StepSpec> plan, std::size_t done, Ops& ops, Latch& latch,
    std::string_view step, std::string_view reason, std::string_view sub) noexcept
{
    InstallOutcome outcome;
    outcome.failedIndex = plan.size();
    outcome.done = done;
    detail::FailInert(plan, done < plan.size() ? done : plan.size(), ops, latch, step, reason, sub, outcome);
    return outcome;
}

}  // namespace agh::rt
