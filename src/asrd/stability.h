// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// Server-side `stable_bytes`: plan.md §1.4's six steps, from the **producer's** side.
//
// ─── Which side of the mark this is ─────────────────────────────────────────────────────────
//
// `server/kvit_asr/stability.py` is the producer — hypotheses in, `stable_bytes` out — and this
// file is its port. `src/commit_engine.cpp` is the *consumer*: it treats the published mark as
// an upper bound and commits the minimum of that and its own independent computation. The two
// are not the same algorithm run in opposite directions, and `CommitEngine` is emphatically not
// the oracle for this file. The oracle is the Python tracker, byte for byte, and
// `tests/test_asrd_stability.cpp` is where that is asserted.
//
// ─── What is reused rather than rewritten ───────────────────────────────────────────────────
//
// Steps 2–4 are `src/util/utf8.h`:
//
//   step 2  utf8::common_grapheme_prefix(items, count)   — the N-way form, already written
//   step 3  utf8::retract_graphemes
//   step 4  utf8::retract_to_word_boundary, utf8::trim_trailing_whitespace
//
// which is roughly 350 of stability.py's 874 lines that do not need porting at all. What is
// here is the tracker around them.
//
// ─── The step-3 reading, which has two and only one is right ────────────────────────────────
//
// "Retract to at least M clusters behind the decode head" can mean *subtract M from the common
// prefix* or *cap the candidate at M clusters behind the head*. `stability.py:831-836` flags the
// ambiguity and takes the second; this file takes the second. They differ in latency, not in
// safety — but they differ, and a property test that disagrees with Python here is a failed
// port, not a refinement.
//
// ─── Step 6 is not advisory ─────────────────────────────────────────────────────────────────
//
// stability.py's own docstring, which transfers verbatim:
//
//   > The server must NEVER emit a `stable_bytes` that decreases, or one whose prefix bytes
//   > differ from what it already sent. […] the Client's required response is to *preserve the
//   > document* and never repair, which means the wrong bytes stay in the user's file forever.
//   > Catching it here costs one comparison; catching it on the Client costs a corrupted
//   > document.
//
// So `check()` **refuses** rather than clamps. docs/protocol.md §8.3 forbids clamping because it
// hides exactly the defect ICER exists to measure.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "util/clock.h"

namespace kd::asrd {

// The three knobs plan.md §4 sweeps. Owned by the server; the client's `Settings::stability`
// stays as `CommitEngine`'s independent second opinion and is a different set of numbers for a
// different computation (openai-translator.md §4.3).
struct StabilityParams {
    uint32_t hypotheses_n = 2;      // N — how many hypotheses must agree
    uint32_t retract_clusters_m = 3;  // M — clusters held back from the decode head
    uint32_t hold_ms_t = 200;       // T — how long a candidate must stand unchanged
};

// Spellings match `kd::ViolationKind` in src/commit_engine.h and `ViolationKind` in
// stability.py, so a log line from either end of the wire reads the same.
enum class ViolationKind : uint8_t {
    None = 0,
    StableBytesOutOfRange,
    StableBytesNotOnBoundary,
    StableBytesDecreased,
    StablePrefixMutated,
};

const char* to_string(ViolationKind k) noexcept;

// Everything docs/protocol.md §8.3 step 3 asks a log line to carry. Returned rather than thrown:
// this project does not use exceptions on the decode path (see util/result.h).
struct Violation {
    ViolationKind kind = ViolationKind::None;
    uint64_t utterance_id = 0;
    uint64_t previous_stable_bytes = 0;
    uint64_t incoming_stable_bytes = 0;
    uint64_t diverge_offset = 0;
    size_t previous_prefix_bytes = 0;
    size_t incoming_text_bytes = 0;

    [[nodiscard]] bool is_violation() const noexcept { return kind != ViolationKind::None; }

    // The one form of this that is safe to put on the wire: every identifying number, no
    // recognised text. `error.msg` is logged verbatim by the Client into a file the user keeps,
    // and plan.md §1.9 keeps transcripts out of diagnostic channels. `diverge_offset` is the
    // load-bearing field — zero means the backend revised the *first word*, which is the
    // signature of a model that re-decodes rather than extends.
    [[nodiscard]] std::string to_wire_message() const;
};

// Turns a stream of cumulative hypotheses into a conforming `stable_bytes` sequence.
//
// One tracker per utterance in progress; `reset()` at every utterance boundary (plan.md §1.4
// step 7 — docs/protocol.md §8.2's high-water mark resets with it).
//
//     StabilityTracker t(params, clock);
//     t.reset(1);
//     uint64_t stable = 0;
//     if (!t.observe("can you change the", stable, v)) fail(v);   // publish a partial
//     if (!t.poll(stable, v)) fail(v);                            // the T-ms hold may have run
//     if (!t.finalize("Can you change the llama.cpp config", stable, v)) fail(v);
//
// Not thread-safe: single-owner, and that owner is the decode thread.
class StabilityTracker {
public:
    explicit StabilityTracker(StabilityParams params = {}, const IClock* clock = nullptr);

    // plan.md §1.4 step 7: reset entirely at an utterance boundary. Entirely means entirely —
    // the hypothesis window, the high-water mark, the published prefix and the dwell timer.
    // Carrying any of it across a boundary would let a mark established for utterance n
    // constrain utterance n+1, which §8.2 explicitly does not do.
    void reset(uint64_t utterance_id = 0);

    // Take one new cumulative hypothesis (docs/protocol.md §8.1 — never a delta) and produce the
    // `stable_bytes` to publish with it. False means the hypothesis contradicts what was already
    // published: that is not recoverable for this utterance, because §8.2 binds the new message
    // against the *previously published* mark, so no value of `stable_bytes` makes the message
    // conforming and it must not be sent at all.
    bool observe(std::string_view text, uint64_t& stable_bytes, Violation& v);

    // Re-evaluate the dwell for the last hypothesis without adding evidence.
    //
    // **The owner must drive this on a timer**, roughly every T/2 ms. Step 5 is a clock
    // condition and observe() only samples the clock when a hypothesis happens to arrive, which
    // has a consequence that cost a full test suite to rediscover on the Python side: while
    // someone is still talking the candidate advances on almost every hypothesis, each advance
    // restarts the dwell, and a loop that never ticks publishes `stable_bytes = 0` for ever.
    // The failure is silent — every message is well formed and the invariant is trivially
    // satisfied by never committing anything.
    bool poll(uint64_t& stable_bytes, Violation& v);

    // Close the utterance with its complete text. A `final` is entirely stable by definition
    // (§6.2), so the answer is len(utf8(text)) — but §8.2's last sentence still applies and is
    // checked rather than assumed: utf8(final.text) MUST begin with the highest prefix already
    // published for this utterance.
    bool finalize(std::string_view text, uint64_t& stable_bytes, Violation& v);

    // The gate on the path out to the wire. Public because a caller assembling a message by
    // another path — a backend that computes its own mark — should be able to run the same
    // check, and because a synthetic backwards step is worth testing directly.
    bool check(std::string_view text, uint64_t stable_bytes, Violation& v) const;

    [[nodiscard]] uint64_t high_water() const noexcept { return high_water_; }
    [[nodiscard]] const std::string& stable_prefix() const noexcept { return stable_prefix_; }
    [[nodiscard]] uint64_t utterance_id() const noexcept { return utterance_id_; }
    [[nodiscard]] bool has_text() const noexcept { return has_last_text_; }
    [[nodiscard]] const std::string& last_text() const noexcept { return last_text_; }
    [[nodiscard]] const StabilityParams& params() const noexcept { return params_; }

private:
    bool evaluate(std::string_view text, uint64_t& stable_bytes, Violation& v);
    [[nodiscard]] Violation make(ViolationKind kind, std::string_view text, uint64_t incoming,
                                 uint64_t diverge) const;

    StabilityParams params_;
    SteadyClock owned_clock_;
    const IClock* clock_;

    std::vector<std::string> window_;  // the last N hypotheses, oldest first
    uint64_t utterance_id_ = 0;
    uint64_t high_water_ = 0;
    std::string stable_prefix_;
    std::string candidate_prefix_;
    Millis candidate_since_ms_ = 0;
    std::string last_text_;
    bool has_last_text_ = false;
};

// Byte offset of the first difference between two byte strings — §8.3 step 3's log field.
size_t first_difference(std::string_view a, std::string_view b) noexcept;

}  // namespace kd::asrd
