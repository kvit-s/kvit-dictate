// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// The commit engine: stability -> injection operations. **Pure — no Windows, no I/O.**
//
// Implements: plan.md §1.4 in full — the committed-prefix invariant, the six-step stability
// algorithm, the separation of cadence from eligibility, violation handling, and the ICER
// counters; plus docs/protocol.md §8 (`stable_bytes` semantics and §8.3's required response to
// a violation).
//
// ─── The invariant, quoted because everything else here follows from it ─────────────────────
//
//   > Every byte sequence injected into the target must remain a prefix of every later
//   > hypothesis, and of the final text, for that utterance. Committed text is never repaired.
//
// The reason is not aesthetic. Replaying a revision means sending backspaces into an arbitrary
// application, and backspace is not safe to send blind: in a browser it can navigate, in an IDE
// it can dismiss autocomplete, in a terminal it can eat a prompt. So there is no repair path in
// this module — not a private one, not a debug one. If a hypothesis contradicts what was
// committed, the document is left exactly as it is and the event is counted.
//
// ─── Cadence and eligibility are separate knobs ─────────────────────────────────────────────
//
// Revision 1 of the plan listed `periodic-500ms` commit policies, which commit on a timer
// regardless of stability — directly contradicting the invariant. The review caught it and the
// resolution is structural rather than a comment:
//
//   * **Eligibility** — which prefix is safe to commit — is determined **only** by stability.
//     It is computed in `eligible_prefix()` and nothing else may widen it.
//   * **Cadence** — `Immediate` or `Periodic(N ms)` — controls only **when an already-eligible
//     prefix is flushed**. It can delay a commit; it can never promote unstable text.
//
// The API enforces this: the cadence parameters are consulted only in `on_tick()` and only to
// decide whether to emit a prefix that `eligible_prefix()` had already blessed.
//
// ─── The stability algorithm (plan.md §1.4), step by step ───────────────────────────────────
//
//   1. Keep the last N hypotheses for the current utterance (default N = 2).
//   2. Candidate = longest common prefix over those N, computed on **grapheme clusters**
//      (utf8::common_grapheme_prefix — a byte-wise prefix would commit a base letter whose
//      accent the model is still revising).
//   3. Retract at least M clusters behind the decode head (default M = 3) — the tail of a
//      hypothesis is where revision concentrates.
//   4. Retract further to the last **word boundary**, and hold back trailing whitespace
//      (utf8::retract_to_word_boundary). Never commit a partial word: a revised word ending is
//      the most common revision there is.
//   5. Require the candidate to have been unchanged for T ms (default 200).
//   6. Never exceed the server's own `stable_bytes`, and never go backwards.
//   7. Reset entirely at an utterance boundary.
//
// Step 6 is why `stable_bytes` and the local computation are both present: the server's mark is
// an upper bound the sender promises never to retract, and the local computation is a second,
// independent opinion. The engine commits the **minimum** of the two. A backend that lies about
// stability then costs latency rather than document corruption.
//
// ─── Threading ──────────────────────────────────────────────────────────────────────────────
//
// Single-threaded, no locks, no clock of its own — the owner passes `now_ms` in, or hands it an
// IClock at construction, so step 5's T-millisecond hold is testable without sleeping. Driven
// from the UI thread alongside SessionMachine. The returned CommitOp is executed by injector.h
// on that same thread.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "protocol.h"
#include "util/clock.h"

namespace kd {

// ─── Configuration ──────────────────────────────────────────────────────────────────────────

enum class Cadence : uint8_t {
    Immediate,  // flush an eligible prefix as soon as it exists
    Periodic,   // flush at most every `cadence_ms`; delays only, never promotes

    // Flush nothing until the session ends, then inject the whole thing in one operation.
    //
    // Still a *cadence* in §1.4's sense — it delays a flush and can never widen one — but note
    // where it is implemented, because it is not here. This engine's cadence gate is consulted
    // on the partial path only (`cadence_allows`, one call site); `on_final` deliberately never
    // asks, because a `final` is complete by definition. And since a revising backend publishes
    // `stable_bytes = 0` on every partial (server-side `publish_stable_partials`), *every* op
    // for such a backend is produced by `on_final`. A cadence implemented purely in this file
    // would therefore have no effect whatsoever on the backend it was added for.
    //
    // So the engine keeps emitting ops exactly as it always has, and `App` accumulates them and
    // injects once. The value lives in this enum because it is one concept and belongs in one
    // place; the engine honours the half it owns (never flushing early on the partial path) and
    // `App::flush_deferred_commits` owns the rest.
    EndOfSession,
};

const char* to_string(Cadence c) noexcept;

struct StabilityParams {
    // The experiment matrix in plan.md §4 sweeps exactly these three.
    uint32_t hypotheses_n = 2;         // N ∈ {2, 3}
    uint32_t retract_clusters_m = 3;   // M ∈ {2, 3, 5}
    uint32_t hold_ms_t = 200;          // T ∈ {100, 200, 400} ms

    Cadence cadence = Cadence::Immediate;
    uint32_t cadence_ms = 0;           // used only when cadence == Periodic

    // Trust the server's `stable_bytes` as an upper bound (step 6). Always true in production;
    // settable so the harness can measure how much of the safety margin each backend's own
    // stability signal is worth.
    bool honour_server_stable_bytes = true;

    // plan.md §1.4: "A speculative mode (commit ahead of stability, to measure the achievable
    // latency ceiling) may be run in the harness. It is named unsafe, excluded from defaults,
    // and never shipped."
    //
    // The name carries the warning because the flag is the only thing standing between the
    // harness and a mode that knowingly corrupts documents. Anything reading settings must
    // refuse to set it outside a bench run.
    bool speculative_unsafe = false;
};

// ─── Violations ─────────────────────────────────────────────────────────────────────────────

enum class ViolationKind : uint8_t {
    None,
    StableBytesOutOfRange,     // > len(utf8(text))                        (§8.2 req 1)
    StableBytesNotOnBoundary,  // inside a grapheme cluster                (§8.2 req 2)
    StableBytesDecreased,      // went backwards within the utterance      (fixture 017)
    StablePrefixMutated,       // bytes below the published mark changed   (fixture 018)
    CommittedTextContradicted, // a `final`/`partial` is not an extension of what we already
                               // typed into the user's document — the one that actually costs
                               // the user something
};

const char* to_string(ViolationKind k) noexcept;

// Everything §8.3 step 3 requires be logged, gathered in one struct so no call site has to
// remember the list.
struct Violation {
    ViolationKind kind = ViolationKind::None;
    std::string session_id;
    uint64_t utterance_id = 0;
    uint64_t seq_previous = 0;
    uint64_t seq_current = 0;
    size_t diverge_offset = 0;   // first differing UTF-8 byte offset
    std::string committed_text;  // what we already typed (or the previous stable prefix)
    std::string incoming_text;   // what arrived and could not be reconciled
    uint64_t previous_stable_bytes = 0;
    uint64_t incoming_stable_bytes = 0;

    [[nodiscard]] std::string to_log_line() const;

    // The wire error §8.3 step 2 requires: `stability_violation`, fatal:false, session_id set.
    // Non-fatal because the child is not at fault as a *process* — its model produced an
    // inadmissible revision, and the next session may well be fine.
    [[nodiscard]] proto::ProtocolError to_protocol_error() const;
};

// Exposed for testing: the word-level alignment behind Result::salvage. Returns the part of
// `incoming` that follows everything it agrees with in `committed`, comparing words with ASCII
// case folding and edge punctuation removed, and returns all of `incoming` when they agree about
// nothing.
std::string salvage_tail(std::string_view committed, std::string_view incoming);

// ─── Output ─────────────────────────────────────────────────────────────────────────────────

// One append-only injection. There is no delete, no replace, no move — the type system is the
// first line of defence for the invariant. `text` is always UTF-8 and always begins exactly at
// `utterance_offset` bytes into the utterance's committed text.
struct CommitOp {
    std::string text;              // the bytes to inject, never empty
    uint64_t utterance_offset = 0; // where they start within this utterance's committed text
    uint64_t utterance_id = 0;
    std::string session_id;
    bool leading_space = false;    // a new utterance appended after a space (plan.md §1.6:
                                   // "resuming starts a new utterance appended after a space")
};

// ─── ICER (plan.md §1.4) ────────────────────────────────────────────────────────────────────
//
// "Irrevocable committed error rate — committed graphemes that differ from the final
// hypothesis, per 1000 committed." This replaces flicker as the headline safety number: flicker
// measures churn the user never sees and can be high with zero document corruption; ICER
// measures text that is actually wrong in the actual document. Both are recorded; ICER gates.
struct IcerCounters {
    uint64_t committed_graphemes = 0;
    uint64_t erroneous_graphemes = 0;   // committed but absent from / different in the final
    uint64_t irrevocable_events = 0;    // violations, i.e. distinct incidents
    uint64_t flicker_revisions = 0;     // hypothesis changes absorbed by the overlay — measured
                                        // for comparison, explicitly NOT the gating metric

    // Per 1000 committed graphemes. Returns 0 when nothing has been committed — a rate with no
    // denominator is not 100%, and reporting it as such would poison the bake-off table.
    [[nodiscard]] double icer_per_1000() const noexcept;
};

// ─── The engine ─────────────────────────────────────────────────────────────────────────────

class CommitEngine {
public:
    struct Result {
        // The text to inject now, if any. Absent means "nothing is eligible yet", which is the
        // common case and is not an error.
        std::optional<CommitOp> op;

        // Populated when the incoming message violated §8.2 or contradicted committed text.
        // When set, `op` is always absent: **the engine never emits an operation derived from a
        // message it rejected.**
        std::optional<Violation> violation;

        // Best-effort recovery, set only alongside `violation` and never instead of `op`.
        //
        // Before this existed, a contradicted hypothesis cost the user the whole rest of the
        // utterance: main.cpp logged the violation and returned, and every word after the
        // divergence was silently dropped. That is the failure plan.md §1.4 ranks worst of all —
        // text the user said, gone, with no way to know it was ever there.
        //
        // This carries the words of the incoming hypothesis that follow everything it still
        // agrees with, so they can be appended after the committed text. It is deliberately NOT
        // `op`: `op` means "derived from a message the engine accepted", and this is derived from
        // one it rejected. The distinction is the point — a caller that wants the guarantee reads
        // `op` and a caller that wants the user's words reads this, and neither can be mistaken
        // for the other.
        //
        // The invariant is untouched. This appends and never rewrites: the contradicted text
        // already in the document stays exactly as it is, wrong and visible, which is what §1.4
        // requires. The alignment is word-level and crude (see salvage_tail), so the seam can
        // duplicate or drop a word; when the two texts agree about nothing it appends everything,
        // because visible duplication is recoverable by the user and silent loss is not.
        std::optional<CommitOp> salvage;

        // The unstable tail — everything above the eligible prefix. This is what the overlay
        // renders as pending text (plan.md §1.5); it is where revisions are absorbed.
        std::string pending_text;

        [[nodiscard]] bool ok() const noexcept { return !violation.has_value(); }
    };

    CommitEngine(const IClock& clock, StabilityParams params);
    ~CommitEngine();
    CommitEngine(const CommitEngine&) = delete;
    CommitEngine& operator=(const CommitEngine&) = delete;

    // ── Lifecycle ───────────────────────────────────────────────────────────────────────────

    // A new session. Clears everything including the committed text and the hypothesis window.
    void begin_session(std::string_view session_id);

    // A new utterance. plan.md §1.4 step 7: the stability state resets entirely at an utterance
    // boundary, and docs/protocol.md §8.2's high-water mark resets to 0 with it. Committed text
    // for previous utterances stays in the ICER accounting.
    void begin_utterance(uint64_t utterance_id);

    // End of session or connection. Does NOT retract anything: committed text stays in the
    // user's document, which is the invariant, and §10.3 step 2 says the unstable tail is lost
    // and that is the honest outcome.
    void end_session();

    void reset();

    // ── Feeding it ──────────────────────────────────────────────────────────────────────────
    //
    // Both take an already-de-staled, already-ordered message: SessionMachine has confirmed the
    // session_id is current and the seq is acceptable. Feeding a stale message here is a bug
    // that this class cannot detect, and is the reason the two modules are separate.

    Result on_partial(const proto::PartialMsg& m);

    // A `final` is entirely stable by definition (docs §6.2), so it commits the whole remaining
    // text — unless it contradicts what was already committed, in which case it produces a
    // CommittedTextContradicted violation and no op. That is the case plan.md §1.4's "violation
    // handling" paragraph is about, and it is the one that feeds ICER.
    Result on_final(const proto::FinalMsg& m);

    // Cadence flush. With Cadence::Immediate this never produces anything the on_partial call
    // did not already produce; with Cadence::Periodic it is what actually emits. It is also
    // what makes step 5's T-millisecond hold fire when no new hypothesis has arrived.
    Result on_tick();

    // ── Inspecting it ───────────────────────────────────────────────────────────────────────

    // Everything committed for the current utterance, in order. Read by the anchor module's
    // commit-differencing source (plan.md §1.5 source C) to know what change to look for.
    [[nodiscard]] std::string_view committed_text() const noexcept;
    [[nodiscard]] uint64_t committed_bytes() const noexcept;

    // The eligible prefix length under the current hypothesis window, in UTF-8 bytes. Exposed
    // for tests and for the bench harness; production goes through Result::op.
    [[nodiscard]] uint64_t eligible_prefix() const noexcept;

    [[nodiscard]] const IcerCounters& counters() const noexcept;
    void reset_counters() noexcept;

    [[nodiscard]] const StabilityParams& params() const noexcept;
    void set_params(const StabilityParams& p);  // bench harness sweeps these between runs

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace kd
