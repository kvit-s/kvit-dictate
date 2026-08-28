// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Suite `commit_engine` — the safety tests. Everything the product promises about not corrupting
// the user's document is asserted here.
//
// Covers commit_engine.h, plan.md §1.4 (the committed-prefix invariant, the six-step stability
// algorithm, cadence vs eligibility, violation handling, ICER) and docs/protocol.md §8.
//
// Three conventions used throughout:
//
//   * **ManualClock, never a sleep.** Step 5's T-millisecond hold and the cadence timer are the
//     two clocks in this module and both are driven explicitly, so every test is deterministic
//     and runs in microseconds.
//   * **`honour_server_stable_bytes = false` in the tests that exercise local stability**, with
//     `stable_bytes = 0` on every message. That isolates the local computation: with the server
//     cap on and a server that publishes nothing, the min() in step 6 would correctly suppress
//     every commit and the local algorithm would be untested. Two tests turn the cap back on and
//     assert the min() itself.
//   * **The Driver re-derives committed text from the CommitOps alone** and asserts on every op
//     that it lands exactly at the end of what was already injected. That is the invariant
//     stated as an executable property rather than as a comment: an op that overlapped, skipped,
//     or replaced would fail here regardless of which test produced it.

#include "commit_engine.h"

#include <string>
#include <vector>

#include "kd_test.h"
#include "util/clock.h"
#include "util/utf8.h"

namespace {

using kd::Cadence;
using kd::CommitEngine;
using kd::CommitOp;
using kd::ManualClock;
using kd::Millis;
using kd::StabilityParams;
using kd::ViolationKind;

// Local-stability defaults: N=2, M as given, T=0 unless a test cares, cadence immediate, server
// cap off.
StabilityParams local_params(uint32_t m = 3, uint32_t t = 0) {
    StabilityParams p;
    p.hypotheses_n = 2;
    p.retract_clusters_m = m;
    p.hold_ms_t = t;
    p.cadence = Cadence::Immediate;
    p.honour_server_stable_bytes = false;
    return p;
}

class Driver {
public:
    explicit Driver(StabilityParams p, Millis t0 = 0) : clock_(t0), engine_(clock_, p) {
        engine_.begin_session("s7");
        engine_.begin_utterance(1);
    }

    CommitEngine& engine() { return engine_; }
    ManualClock& clock() { return clock_; }

    void advance(Millis ms) { clock_.advance(ms); }

    void begin_utterance(uint64_t id) {
        utterance_ = id;
        injected_.clear();  // CommitOp offsets are per-utterance
        engine_.begin_utterance(id);
    }

    CommitEngine::Result partial(std::string_view text, uint64_t stable_bytes = 0) {
        kd::proto::PartialMsg m;
        m.session_id = "s7";
        m.utterance_id = utterance_;
        m.seq = ++seq_;
        m.text = std::string(text);
        m.stable_bytes = stable_bytes;
        return record(engine_.on_partial(m));
    }

    CommitEngine::Result final_msg(std::string_view text) {
        kd::proto::FinalMsg m;
        m.session_id = "s7";
        m.utterance_id = utterance_;
        m.seq = ++seq_;
        m.text = std::string(text);
        m.audio_pos_ms = 1000;
        return record(engine_.on_final(m));
    }

    CommitEngine::Result tick() { return record(engine_.on_tick()); }

    // Drain ticks until nothing more is produced, so a cadence-delayed prefix is not left
    // stranded by the test itself.
    void settle(Millis to_ms) {
        clock_.set(to_ms);
        for (int i = 0; i < 8; ++i) {
            if (!tick().op.has_value()) break;
        }
    }

    // Committed text as reconstructed from the ops alone.
    const std::string& injected() const { return injected_; }
    const std::vector<CommitOp>& ops() const { return ops_; }

private:
    CommitEngine::Result record(CommitEngine::Result r) {
        if (r.op.has_value()) {
            const CommitOp& op = *r.op;
            // The invariant, as a property: append-only, exactly at the end, never empty.
            KD_CHECK(!op.text.empty());
            KD_CHECK_EQ(op.utterance_offset, static_cast<uint64_t>(injected_.size()));
            KD_CHECK_EQ(op.utterance_id, utterance_);
            KD_CHECK_EQ(op.session_id, std::string("s7"));
            injected_ += op.text;
            ops_.push_back(op);
            // ...and the engine's own view agrees with the reconstruction.
            KD_CHECK_EQ(engine_.committed_text(), std::string_view(injected_));
            // Committed text never ends with whitespace (plan.md §1.4's hold-back rule).
            KD_CHECK(injected_.empty() ||
                     !kd::utf8::is_whitespace(static_cast<char32_t>(
                         static_cast<unsigned char>(injected_.back()))));
        }
        // A rejected message never yields an operation.
        if (r.violation.has_value()) KD_CHECK(!r.op.has_value());
        KD_CHECK_EQ(r.ok(), !r.violation.has_value());
        return r;
    }

    ManualClock clock_;
    CommitEngine engine_;
    std::string injected_;
    std::vector<CommitOp> ops_;
    uint64_t seq_ = 0;
    uint64_t utterance_ = 1;
};

// The emoji pieces, spelled as bytes so the test does not depend on the editor or on the source
// charset surviving a round trip.
const char* const kMan = "\xF0\x9F\x91\xA8";    // U+1F468
const char* const kWoman = "\xF0\x9F\x91\xA9";  // U+1F469
const char* const kGirl = "\xF0\x9F\x91\xA7";   // U+1F467
const char* const kBoy = "\xF0\x9F\x91\xA6";    // U+1F466
const char* const kZwj = "\xE2\x80\x8D";        // U+200D

}  // namespace

// Cadence::EndOfSession, on the half of it this engine owns.
//
// The other half is App::flush_deferred_commits, which cannot be reached from here — so what this
// pins is exactly the boundary: the partial path never flushes, and `on_final` still produces its
// op regardless, because a caller that buffers needs something to buffer. An engine that
// suppressed the final op too would leave the text with nowhere to come from and the feature
// would be silently dead.
KD_TEST(commit_engine, end_of_session_suppresses_partial_flushes_but_not_the_final) {
    StabilityParams p = local_params(/*m=*/0, /*t=*/0);
    p.cadence = Cadence::EndOfSession;
    Driver d(p);

    // Enough hypotheses, and enough elapsed time, that an Immediate cadence would certainly have
    // committed by now.
    d.partial("the quick brown fox", 19);
    d.advance(1000);
    d.tick();
    d.partial("the quick brown fox jumps", 25);
    d.advance(1000);
    d.tick();
    KD_CHECK_EQ(d.injected(), "");   // nothing typed, however eligible it became

    // The final still yields the whole utterance for the caller to accumulate.
    const auto r = d.final_msg("the quick brown fox jumps");
    KD_CHECK(r.ok());
    KD_REQUIRE(r.op.has_value());
    KD_CHECK_EQ(r.op->text, "the quick brown fox jumps");
}

// `pending_text` is scoped to the utterance in progress, and a `final` empties it.
//
// Obvious once stated, and the source of a real defect: App used `pending_text` alone as the
// overlay preview, which is right when every utterance is typed into the document as it closes —
// the preview empties exactly because the words moved into the document. Under
// Cadence::EndOfSession nothing moves anywhere, so the same code emptied the pill at every pause
// while the accumulated buffer sat behind it, and the user watched a sentence of their dictation
// apparently vanish. A deferred cadence has to render the buffer, not just this.
KD_TEST(commit_engine, pending_text_is_utterance_scoped_and_a_final_empties_it) {
    Driver d(local_params(/*m=*/3, /*t=*/0));

    const auto mid = d.partial("the quick brown fox jumps", 0);
    KD_CHECK(!mid.pending_text.empty());   // mid-utterance there is something to preview

    const auto end = d.final_msg("the quick brown fox jumps");
    KD_CHECK(end.ok());
    KD_CHECK_EQ(end.pending_text, "");     // nothing unstable is left to show
}

// ─── Spellings ──────────────────────────────────────────────────────────────────────────────

KD_TEST(commit_engine, to_string_spellings) {
    KD_CHECK_EQ(kd::to_string(Cadence::Immediate), "immediate");
    KD_CHECK_EQ(kd::to_string(Cadence::Periodic), "periodic");
    // The settings file writes this spelling; changing it silently orphans every settings.json
    // that already contains it (settings.cpp's kCadences reads the same strings).
    KD_CHECK_EQ(kd::to_string(Cadence::EndOfSession), "end-of-session");
    KD_CHECK_EQ(kd::to_string(ViolationKind::None), "none");
    KD_CHECK_EQ(kd::to_string(ViolationKind::CommittedTextContradicted),
                "committed_text_contradicted");
    KD_CHECK_EQ(kd::to_string(ViolationKind::StableBytesDecreased), "stable_bytes_decreased");
}

// ─── The happy path: monotonic growth over a real revision sequence ─────────────────────────

KD_TEST(commit_engine, monotonic_growth_over_revision_stream) {
    // The stream is the one plan.md §1.2 uses as its example, revised the way a streaming decoder
    // actually revises: the tail grows and is repunctuated, the head does not move.
    Driver d(local_params(/*m=*/3));

    d.partial("can you");
    d.partial("can you change");
    KD_CHECK_EQ(d.engine().committed_text(), "can");

    d.partial("can you change the");
    KD_CHECK_EQ(d.engine().committed_text(), "can you");

    d.partial("can you change the llama");
    KD_CHECK_EQ(d.engine().committed_text(), "can you change");

    d.partial("can you change the llama dot cpp");
    KD_CHECK_EQ(d.engine().committed_text(), "can you change the");

    // Committed length is monotonically non-decreasing throughout.
    uint64_t last = 0;
    for (const CommitOp& op : d.ops()) {
        KD_CHECK_GE(op.utterance_offset, last);
        last = op.utterance_offset + op.text.size();
    }

    const std::string final_text = "can you change the llama.cpp configuration";
    auto r = d.final_msg(final_text);
    KD_CHECK(r.ok());
    KD_CHECK_EQ(d.injected(), final_text);
    KD_CHECK_EQ(d.engine().committed_text(), final_text);

    // Every intermediate commit is a byte prefix of the final text — the invariant, checked
    // against the thing it is stated about.
    std::string sofar;
    for (const CommitOp& op : d.ops()) {
        sofar += op.text;
        KD_CHECK(final_text.compare(0, sofar.size(), sofar) == 0);
    }

    KD_CHECK_EQ(d.engine().counters().irrevocable_events, uint64_t{0});
    KD_CHECK_EQ(d.engine().counters().erroneous_graphemes, uint64_t{0});
    KD_CHECK_NEAR(d.engine().counters().icer_per_1000(), 0.0, 1e-9);
}

// The dot inside an identifier is not a word end (src/util/utf8.h, plan.md §4). If it were, the
// engine would commit "the llama." and then have to append "cpp" — right for a sentence, wrong
// for the exact case this product exists to get right.
KD_TEST(commit_engine, identifier_dot_is_not_a_word_boundary) {
    Driver d(local_params(/*m=*/3));
    d.partial("the llama.cpp config x");
    d.partial("the llama.cpp config y");
    KD_CHECK_EQ(d.engine().committed_text(), "the llama.cpp");
}

// ─── The violation that costs the user something ────────────────────────────────────────────

KD_TEST(commit_engine, final_contradicting_committed_text_is_never_repaired) {
    // The realistic shape: partials are lowercase and unpunctuated, and the backend capitalises
    // the sentence in the final. That single byte at offset 0 is unapplicable — we have already
    // typed "can".
    Driver d(local_params(/*m=*/3));
    d.partial("can you");
    d.partial("can you change");
    KD_REQUIRE_EQ(d.engine().committed_text(), "can");

    const std::string before = std::string(d.engine().committed_text());
    auto r = d.final_msg("Can you change");

    KD_REQUIRE(r.violation.has_value());
    KD_CHECK(!r.op.has_value());
    KD_CHECK(!r.ok());
    KD_CHECK_EQ(static_cast<int>(r.violation->kind),
                static_cast<int>(ViolationKind::CommittedTextContradicted));
    KD_CHECK_EQ(r.violation->diverge_offset, size_t{0});
    KD_CHECK_EQ(r.violation->committed_text, "can");
    KD_CHECK_EQ(r.violation->incoming_text, "Can you change");
    KD_CHECK_EQ(r.violation->session_id, "s7");
    KD_CHECK_EQ(r.violation->utterance_id, uint64_t{1});

    // The document is preserved: not repaired, not extended, not truncated.
    KD_CHECK_EQ(d.engine().committed_text(), before);
    KD_CHECK_EQ(d.injected(), before);

    // ICER: three committed graphemes ("can"), all three wrong.
    KD_CHECK_EQ(d.engine().counters().irrevocable_events, uint64_t{1});
    KD_CHECK_EQ(d.engine().counters().committed_graphemes, uint64_t{3});
    KD_CHECK_EQ(d.engine().counters().erroneous_graphemes, uint64_t{3});
    KD_CHECK_NEAR(d.engine().counters().icer_per_1000(), 1000.0, 1e-9);

    // The wire error required by docs/protocol.md §8.3 step 2.
    const kd::proto::ProtocolError e = r.violation->to_protocol_error();
    KD_CHECK_EQ(static_cast<int>(e.code), static_cast<int>(kd::proto::ErrorCode::stability_violation));
    KD_CHECK(!e.fatal);
    KD_REQUIRE(e.session_id.has_value());
    KD_CHECK_EQ(*e.session_id, "s7");

    // The log line carries both texts and the offset (§8.3 step 3).
    const std::string line = r.violation->to_log_line();
    KD_CHECK(line.find("committed_text_contradicted") != std::string::npos);
    KD_CHECK(line.find("diverge_offset=0") != std::string::npos);
    KD_CHECK(line.find("Can you change") != std::string::npos);
    KD_CHECK(line.find('\n') == std::string::npos);  // one record, one line

    // The session is aborted, and stays aborted: a caller that ignored the first violation still
    // cannot get an operation out of the engine. The incident count does not inflate.
    auto after = d.partial("Can you change the");
    KD_CHECK(!after.op.has_value());
    KD_CHECK(after.violation.has_value());
    KD_CHECK_EQ(d.engine().counters().irrevocable_events, uint64_t{1});
    // Not even a new utterance reopens it — §8.3 aborts the session, not the utterance.
    d.begin_utterance(2);
    auto after2 = d.partial("something else entirely");
    KD_CHECK(!after2.op.has_value());
    KD_CHECK(after2.violation.has_value());
}

KD_TEST(commit_engine, later_partial_contradicting_committed_text_is_reported) {
    Driver d(local_params(/*m=*/3));
    d.partial("the quick brown");
    d.partial("the quick brown fox");
    KD_REQUIRE_EQ(d.engine().committed_text(), "the quick");

    auto r = d.partial("the quack brown fox jumps");
    KD_REQUIRE(r.violation.has_value());
    KD_CHECK_EQ(static_cast<int>(r.violation->kind),
                static_cast<int>(ViolationKind::CommittedTextContradicted));
    KD_CHECK_EQ(r.violation->diverge_offset, size_t{6});  // "the qu|ick" vs "the qu|ack"
    KD_CHECK_EQ(d.engine().committed_text(), "the quick");
    // "the qu" is right, "ick" is wrong: three committed graphemes differ from the hypothesis.
    KD_CHECK_EQ(d.engine().counters().erroneous_graphemes, uint64_t{3});
    KD_CHECK_EQ(d.engine().counters().irrevocable_events, uint64_t{1});
}

// ─── §8.2 violations: never clamped ─────────────────────────────────────────────────────────

KD_TEST(commit_engine, stable_bytes_decrease_is_a_violation_not_a_clamp) {
    StabilityParams p = local_params(/*m=*/0);
    p.honour_server_stable_bytes = true;
    Driver d(p);

    d.partial("hello world", 6);
    auto r = d.partial("hello world", 3);

    KD_REQUIRE(r.violation.has_value());
    KD_CHECK_EQ(static_cast<int>(r.violation->kind),
                static_cast<int>(ViolationKind::StableBytesDecreased));
    KD_CHECK(!r.op.has_value());
    KD_CHECK_EQ(r.violation->previous_stable_bytes, uint64_t{6});
    KD_CHECK_EQ(r.violation->incoming_stable_bytes, uint64_t{3});
    KD_CHECK_EQ(d.engine().counters().irrevocable_events, uint64_t{1});

    // A clamping implementation would have carried on and eventually committed. This one is dead.
    d.advance(10'000);
    KD_CHECK(!d.tick().op.has_value());
}

KD_TEST(commit_engine, stable_prefix_mutation_is_a_violation) {
    StabilityParams p = local_params(/*m=*/0);
    p.honour_server_stable_bytes = true;
    Driver d(p);

    d.partial("hello world", 6);
    auto r = d.partial("hellO world", 6);  // byte 4 changed below the published mark

    KD_REQUIRE(r.violation.has_value());
    KD_CHECK_EQ(static_cast<int>(r.violation->kind),
                static_cast<int>(ViolationKind::StablePrefixMutated));
    KD_CHECK_EQ(r.violation->diverge_offset, size_t{4});
    KD_CHECK(!r.op.has_value());
}

KD_TEST(commit_engine, stable_bytes_out_of_range_is_a_violation) {
    StabilityParams p = local_params(/*m=*/0);
    p.honour_server_stable_bytes = true;
    Driver d(p);

    auto r = d.partial("abc", 99);
    KD_REQUIRE(r.violation.has_value());
    KD_CHECK_EQ(static_cast<int>(r.violation->kind),
                static_cast<int>(ViolationKind::StableBytesOutOfRange));
}

// ─── Step 6: the min() against the server's own mark ────────────────────────────────────────

KD_TEST(commit_engine, server_stable_bytes_caps_the_local_computation) {
    // Same stream, two engines: one that honours the server's mark and one that does not. The
    // honouring engine commits the minimum of the two opinions, which is the whole point — a
    // backend that lies about stability then costs latency, not document corruption.
    StabilityParams capped = local_params(/*m=*/0);
    capped.honour_server_stable_bytes = true;
    Driver with_cap(capped);
    Driver without_cap(local_params(/*m=*/0));

    for (Driver* d : {&with_cap, &without_cap}) {
        d->partial("hello world abc", 0);
        d->partial("hello world abc", 0);
    }
    // The server has published nothing as stable, so the capped engine may commit nothing.
    KD_CHECK_EQ(with_cap.engine().committed_text(), "");
    KD_CHECK_EQ(without_cap.engine().committed_text(), "hello world");

    // Now the server publishes "hello " as stable. min(local=11, server=6) = 6, then step 4
    // holds the space back.
    with_cap.partial("hello world abc", 6);
    KD_CHECK_EQ(with_cap.engine().committed_text(), "hello");
}

// ─── Cadence delays; it can never promote ───────────────────────────────────────────────────

KD_TEST(commit_engine, cadence_only_changes_timing_never_content) {
    // This is the property whose absence was revision 1's blocking defect (plan.md §9): a
    // `periodic-500ms` policy that committed on a timer regardless of stability. Three engines,
    // one hypothesis stream, identical clocks — the committed BYTES must be identical and only
    // the number and timing of the operations may differ.
    StabilityParams immediate = local_params(/*m=*/0);

    StabilityParams p500 = immediate;
    p500.cadence = Cadence::Periodic;
    p500.cadence_ms = 500;

    StabilityParams p1000 = immediate;
    p1000.cadence = Cadence::Periodic;
    p1000.cadence_ms = 1000;

    Driver a(immediate), b(p500), c(p1000);
    Driver* all[] = {&a, &b, &c};

    const char* stream[] = {
        "hello",
        "hello world",
        "hello world how",
        "hello world how are",
        "hello world how are you",
        "hello world how are you today",
    };

    Millis t = 0;
    for (const char* text : stream) {
        for (Driver* d : all) {
            d->clock().set(t);
            d->partial(text);
        }
        // Eligibility is a function of the stability state and the clock alone, so all three
        // engines agree on it at every instant regardless of cadence. This is the assertion that
        // says cadence cannot promote: it is not merely that the outcome matches at the end, it
        // is that the eligible prefix is the same number the whole way through.
        KD_CHECK_EQ(a.engine().eligible_prefix(), b.engine().eligible_prefix());
        KD_CHECK_EQ(a.engine().eligible_prefix(), c.engine().eligible_prefix());

        // A delayed engine is always behind, never ahead, and always on the same text.
        KD_CHECK_LE(b.injected().size(), a.injected().size());
        KD_CHECK_LE(c.injected().size(), b.injected().size());
        KD_CHECK(a.injected().compare(0, b.injected().size(), b.injected()) == 0);
        KD_CHECK(a.injected().compare(0, c.injected().size(), c.injected()) == 0);
        t += 100;
    }

    for (Driver* d : all) d->settle(3000);

    KD_CHECK_EQ(a.injected(), "hello world how are");
    KD_CHECK_EQ(b.injected(), a.injected());
    KD_CHECK_EQ(c.injected(), a.injected());

    // ...and the timing genuinely differed, or the test above proved nothing.
    KD_CHECK_GT(a.ops().size(), c.ops().size());
    KD_CHECK_EQ(c.ops().size(), size_t{1});
}

// ─── Step 5: the T-millisecond dwell ────────────────────────────────────────────────────────

KD_TEST(commit_engine, dwell_is_enforced_with_the_injected_clock) {
    Driver d(local_params(/*m=*/0, /*t=*/200));

    d.partial("hello");        // t=0, one hypothesis: nothing agreed yet
    d.partial("hello world");  // t=0, candidate is still 0 ("hello" has no word boundary below it)
    KD_CHECK_EQ(d.engine().committed_text(), "");

    d.advance(100);
    d.partial("hello world how");  // t=100: candidate becomes "hello"
    KD_CHECK_EQ(d.engine().committed_text(), "");
    KD_CHECK_EQ(d.engine().eligible_prefix(), uint64_t{0});

    // The dwell runs from t=100, when "hello" BECAME the candidate — not from t=0, when it first
    // appeared in a hypothesis. Steps 1-4 decide what the candidate is; step 5 then times it.
    d.advance(199);  // t=299 — one millisecond short
    KD_CHECK(!d.tick().op.has_value());
    KD_CHECK_EQ(d.engine().eligible_prefix(), uint64_t{0});

    d.advance(1);  // t=300: the candidate has been unchanged for exactly T
    KD_CHECK_EQ(d.engine().eligible_prefix(), uint64_t{5});
    auto r = d.tick();
    KD_REQUIRE(r.op.has_value());
    KD_CHECK_EQ(r.op->text, "hello");
    KD_CHECK_EQ(d.engine().committed_text(), "hello");
}

KD_TEST(commit_engine, dwell_does_not_starve_continuous_speech) {
    // The interpretation test. Step 5 says "require the candidate to have been unchanged for
    // T ms". Read as "restart the timer whenever the candidate value changes", a speaker who
    // never pauses produces a candidate that grows with every partial and therefore never
    // commits a single byte — which cannot be the intent of a section whose purpose is getting
    // text into the document during speech. The dwell is per prefix: growing the candidate adds
    // a new timer and leaves the ones below it running.
    Driver d(local_params(/*m=*/0, /*t=*/200));

    const char* stream[] = {"hello", "hello world", "hello world how", "hello world how are",
                            "hello world how are you"};
    for (const char* text : stream) {
        d.partial(text);
        d.advance(100);  // partials arrive twice as fast as T
    }

    // The final advance() moves the clock past the dwell of the prefix agreed one partial ago,
    // but a clock advance is not an event: nothing is flushed until the engine is asked. Without
    // this tick the test would assert on state one commit stale.
    d.tick();

    KD_CHECK(!d.injected().empty());
    KD_CHECK_EQ(d.engine().committed_text(), "hello world");
}

// ─── Step 4: never a partial word, never a trailing space ───────────────────────────────────

KD_TEST(commit_engine, partial_word_is_never_committed) {
    // The last word grows one character at a time, which is exactly how a streaming decoder
    // emits it. At no point may "w", "wo", "wor" or "worl" reach the document.
    Driver d(local_params(/*m=*/3));

    const char* stream[] = {"hello w", "hello wo", "hello wor", "hello worl", "hello world"};
    for (const char* text : stream) {
        d.partial(text);
        const std::string_view committed = d.engine().committed_text();
        KD_CHECK(committed.empty() || committed == "hello");
    }
    KD_CHECK_EQ(d.engine().committed_text(), "hello");

    auto r = d.final_msg("hello world");
    KD_REQUIRE(r.op.has_value());
    KD_CHECK_EQ(r.op->text, " world");
    KD_CHECK_EQ(d.injected(), "hello world");
}

KD_TEST(commit_engine, trailing_whitespace_is_held_back) {
    // With M=0 the word-boundary step is the only thing standing between the candidate and the
    // trailing space, so this tests exactly the hold-back rule: the boundary is at 12, the
    // commit stops at 11.
    Driver d(local_params(/*m=*/0));
    d.partial("hello world ");
    d.partial("hello world ");
    KD_CHECK_EQ(d.engine().committed_text(), "hello world");

    // Still held back at the final: the next utterance is appended after a space (plan.md §1.6)
    // and committing this one's trailing space too would double it.
    auto r = d.final_msg("hello world ");
    KD_CHECK(!r.op.has_value());
    KD_CHECK_EQ(d.engine().committed_text(), "hello world");
}

// ─── Step 7: the utterance boundary ─────────────────────────────────────────────────────────

KD_TEST(commit_engine, utterance_boundary_resets_everything) {
    Driver d(local_params(/*m=*/0));
    d.partial("hello world abc");
    d.partial("hello world abc");
    KD_REQUIRE_EQ(d.engine().committed_text(), "hello world");
    const uint64_t committed_graphemes = d.engine().counters().committed_graphemes;

    d.begin_utterance(2);
    KD_CHECK_EQ(d.engine().committed_text(), "");
    KD_CHECK_EQ(d.engine().committed_bytes(), uint64_t{0});
    KD_CHECK_EQ(d.engine().eligible_prefix(), uint64_t{0});
    // ICER accounting spans the session; only the stability state resets.
    KD_CHECK_EQ(d.engine().counters().committed_graphemes, committed_graphemes);

    // The stability window is empty again, so one hypothesis is not enough to commit anything —
    // if the previous utterance's hypotheses had survived, this would commit immediately.
    d.partial("second one here");
    KD_CHECK_EQ(d.engine().committed_text(), "");

    d.partial("second one here");
    KD_REQUIRE_EQ(d.ops().size(), size_t{2});
    const CommitOp& op = d.ops().back();
    KD_CHECK_EQ(op.utterance_offset, uint64_t{0});
    KD_CHECK_EQ(op.utterance_id, uint64_t{2});
    // plan.md §1.6: "resuming starts a new utterance appended after a space".
    KD_CHECK(op.leading_space);
    KD_CHECK_EQ(op.text, "second one");

    // The very first commit of a session carries no leading space.
    KD_CHECK(!d.ops().front().leading_space);
}

KD_TEST(commit_engine, leading_space_survives_an_unannounced_utterance_change) {
    // `utterance_boundary_resets_everything` above drives `begin_utterance()`, which **no owner
    // calls**: src/main.cpp only ever calls on_partial / on_final / on_tick / reset(). So the
    // boundary the product actually takes is the id-change-detected-by-the-engine path, and this
    // pins that it preserves `session_has_committed`.
    //
    // Honest scope: this test passes with or without the fix that accompanied it — the engine was
    // never the broken half. The defect was SessionMachine emitting the session-scoped
    // ResetCommitEngine at an utterance boundary, and `session.an_utterance_boundary_does_not_
    // reset_the_commit_engine` is the test with teeth for it. This one exists because the fix now
    // *depends* on the behaviour below, which nothing else pinned.
    Driver d(local_params(/*m=*/0));
    d.partial("hello world abc");
    d.partial("hello world abc");
    KD_REQUIRE_EQ(d.engine().committed_text(), "hello world");
    KD_REQUIRE(!d.ops().front().leading_space);  // first commit of a session: no space

    // New utterance id, never announced through begin_utterance() — the engine detects it.
    CommitOp op{};
    bool got = false;
    for (int i = 0; i < 2; ++i) {
        kd::proto::PartialMsg m;
        m.session_id = "s7";
        m.utterance_id = 2;
        m.seq = 90 + i;
        m.text = "second one here";
        const CommitEngine::Result r = d.engine().on_partial(m);
        KD_REQUIRE(r.ok());
        if (r.op.has_value()) {
            op = *r.op;
            got = true;
        }
    }
    KD_REQUIRE(got);
    KD_CHECK_EQ(op.utterance_id, uint64_t{2});
    KD_CHECK_EQ(op.utterance_offset, uint64_t{0});
    // plan.md §1.6: "resuming starts a new utterance appended after a space".
    KD_CHECK(op.leading_space);
}

KD_TEST(commit_engine, an_unannounced_utterance_id_change_resets_the_window) {
    // SessionMachine is supposed to call begin_utterance(). If it does not, mixing hypotheses
    // across a boundary would compute a common prefix over two unrelated texts, so the engine
    // treats the id change itself as the boundary.
    Driver d(local_params(/*m=*/0));
    d.partial("hello world abc");
    d.partial("hello world abc");
    KD_REQUIRE_EQ(d.engine().committed_text(), "hello world");

    kd::proto::PartialMsg m;
    m.session_id = "s7";
    m.utterance_id = 2;  // no begin_utterance() call
    m.seq = 99;
    m.text = "hello world abc";
    auto r = d.engine().on_partial(m);
    KD_CHECK(r.ok());
    KD_CHECK(!r.op.has_value());
    KD_CHECK_EQ(d.engine().committed_text(), "");
}

// ─── Grapheme safety ────────────────────────────────────────────────────────────────────────
//
// Note on what these can and cannot isolate: step 4 (retract to a word boundary) is a strictly
// stronger constraint than step 2 (grapheme-wise common prefix) for any text containing a word
// separator, because a separator-ending offset is always a cluster boundary. So a test cannot
// construct a case where only step 2 saves the document. What it can — and must — assert is the
// property both steps exist to guarantee: the committed length is a grapheme-cluster boundary of
// every hypothesis involved, and no fragment of a cluster ever reaches the document.

KD_TEST(commit_engine, divergence_inside_a_combining_sequence_never_splits_the_cluster) {
    const std::string with_accent = std::string("the word is cafe") + "\xCC\x81";  // U+0301
    const std::string without = "the word is cafe";

    Driver d(local_params(/*m=*/0));
    d.partial(with_accent);
    d.partial(without);  // the model drops the accent: the byte prefix agrees to 16, the
                         // grapheme prefix only to 15

    const std::string_view committed = d.engine().committed_text();
    KD_CHECK_EQ(committed, "the word is");
    KD_CHECK(kd::utf8::is_grapheme_boundary(with_accent, committed.size()));
    KD_CHECK(kd::utf8::is_grapheme_boundary(without, committed.size()));

    // The other direction — the accent arriving after the bare letter was already a hypothesis —
    // must behave identically.
    Driver e(local_params(/*m=*/0));
    e.partial(without);
    e.partial(with_accent);
    KD_CHECK_EQ(e.engine().committed_text(), "the word is");

    // And appending a combining mark to a letter we have NOT committed is an ordinary extension,
    // not a contradiction.
    auto r = e.final_msg(with_accent);
    KD_CHECK(r.ok());
    KD_CHECK_EQ(e.injected(), with_accent);
}

KD_TEST(commit_engine, divergence_inside_an_emoji_zwj_sequence_never_splits_the_cluster) {
    // "hi 👨‍👩‍👧 there" vs "hi 👨‍👩‍👦 there": the byte-wise common prefix runs 20 bytes deep, which is
    // three bytes into the final emoji of the ZWJ sequence. Committing that would put half a
    // codepoint in the user's document.
    const std::string girl =
        std::string("hi ") + kMan + kZwj + kWoman + kZwj + kGirl + " there";
    const std::string boy = std::string("hi ") + kMan + kZwj + kWoman + kZwj + kBoy + " there";

    Driver d(local_params(/*m=*/0));
    d.partial(girl);
    d.partial(boy);

    const std::string_view committed = d.engine().committed_text();
    KD_CHECK_EQ(committed, "hi");
    KD_CHECK(kd::utf8::is_grapheme_boundary(girl, committed.size()));
    KD_CHECK(kd::utf8::is_grapheme_boundary(boy, committed.size()));
    KD_CHECK(committed.find('\xF0') == std::string_view::npos);  // no emoji fragment

    // Once the model settles, the whole cluster goes in at once.
    auto r = d.final_msg(boy);
    KD_REQUIRE(r.op.has_value());
    KD_CHECK_EQ(d.injected(), boy);
}

// ─── Degenerate inputs ──────────────────────────────────────────────────────────────────────

KD_TEST(commit_engine, empty_and_whitespace_only_hypotheses_commit_nothing) {
    Driver d(local_params(/*m=*/0));

    auto r0 = d.partial("");
    KD_CHECK(r0.ok());
    KD_CHECK(!r0.op.has_value());
    KD_CHECK_EQ(r0.pending_text, "");

    d.partial("");
    KD_CHECK_EQ(d.engine().committed_text(), "");
    KD_CHECK_EQ(d.engine().eligible_prefix(), uint64_t{0});

    d.partial("   ");
    d.partial("   ");
    KD_CHECK_EQ(d.engine().committed_text(), "");

    // Even a final made only of whitespace commits nothing: trailing whitespace is held back and
    // that is all there is.
    auto rf = d.final_msg("   ");
    KD_CHECK(rf.ok());
    KD_CHECK(!rf.op.has_value());
    KD_CHECK_EQ(d.engine().committed_text(), "");

    // A rate with no denominator is 0, not 100 (commit_engine.h).
    KD_CHECK_NEAR(d.engine().counters().icer_per_1000(), 0.0, 1e-9);
    KD_CHECK_EQ(d.engine().counters().committed_graphemes, uint64_t{0});
}

KD_TEST(commit_engine, single_cluster_hypothesis) {
    Driver d(local_params(/*m=*/0));
    d.partial("a");
    d.partial("a");
    // One word with no boundary after it: not safe to commit, because the next partial may make
    // it "an".
    KD_CHECK_EQ(d.engine().committed_text(), "");
    KD_CHECK_EQ(d.partial("a").pending_text, "a");

    auto r = d.final_msg("a");
    KD_REQUIRE(r.op.has_value());
    KD_CHECK_EQ(r.op->text, "a");
    KD_CHECK_EQ(d.engine().counters().committed_graphemes, uint64_t{1});
}

KD_TEST(commit_engine, pending_text_and_committed_text_together_are_the_hypothesis) {
    Driver d(local_params(/*m=*/0));
    d.partial("hello world abc");
    auto r = d.partial("hello world abc");
    KD_CHECK_EQ(std::string(d.engine().committed_text()) + r.pending_text, "hello world abc");
    KD_CHECK_EQ(r.pending_text, " abc");
}

// ─── Lifecycle ──────────────────────────────────────────────────────────────────────────────

KD_TEST(commit_engine, end_session_does_not_retract_committed_text) {
    Driver d(local_params(/*m=*/0));
    d.partial("hello world abc");
    d.partial("hello world abc");
    KD_REQUIRE_EQ(d.engine().committed_text(), "hello world");

    d.engine().end_session();
    // docs/protocol.md §10.3: committed text stays in the user's document; the unstable tail is
    // lost and that is the honest outcome.
    KD_CHECK_EQ(d.engine().committed_text(), "hello world");
    d.advance(10'000);
    KD_CHECK(!d.tick().op.has_value());
}

KD_TEST(commit_engine, reset_clears_state_but_keeps_counters) {
    Driver d(local_params(/*m=*/0));
    d.partial("hello world abc");
    d.partial("hello world abc");
    const uint64_t g = d.engine().counters().committed_graphemes;
    KD_REQUIRE_GT(g, uint64_t{0});

    d.engine().reset();
    KD_CHECK_EQ(d.engine().committed_text(), "");
    KD_CHECK_EQ(d.engine().counters().committed_graphemes, g);

    d.engine().reset_counters();
    KD_CHECK_EQ(d.engine().counters().committed_graphemes, uint64_t{0});
    KD_CHECK_EQ(d.engine().counters().irrevocable_events, uint64_t{0});
}

// ─── Flicker is recorded, but it is not the gating metric ───────────────────────────────────

KD_TEST(commit_engine, flicker_counts_revisions_absorbed_by_the_overlay) {
    Driver d(local_params(/*m=*/3));
    d.partial("i want to buy two");
    d.partial("i want to buy too");  // rewritten tail, entirely above the committed prefix
    d.partial("i want to buy to a shop");

    KD_CHECK_GE(d.engine().counters().flicker_revisions, uint64_t{2});
    // Churn the user never saw: zero document corruption.
    KD_CHECK_EQ(d.engine().counters().irrevocable_events, uint64_t{0});
    KD_CHECK_NEAR(d.engine().counters().icer_per_1000(), 0.0, 1e-9);
}

// ─── The speculative mode, which is never shipped ───────────────────────────────────────────

KD_TEST(commit_engine, speculative_unsafe_is_off_by_default) {
    // plan.md §1.4: "named unsafe, excluded from defaults, and never shipped". Settings::validate()
    // additionally rejects a settings file that sets it (src/settings.h), so the only way to
    // reach this mode is a bench harness constructing StabilityParams in code.
    KD_CHECK(!StabilityParams{}.speculative_unsafe);
    StabilityParams p;
    CommitEngine e(kd::SteadyClock::instance(), p);
    KD_CHECK(!e.params().speculative_unsafe);
}

KD_TEST(commit_engine, speculative_unsafe_commits_ahead_of_stability_and_is_measurable) {
    StabilityParams p = local_params(/*m=*/3, /*t=*/200);
    p.speculative_unsafe = true;
    Driver d(p);

    // One hypothesis, no dwell, no word boundary: it commits a partial word immediately. That is
    // the definition of the mode and the reason it must never ship.
    auto r = d.partial("hello wor");
    KD_REQUIRE(r.op.has_value());
    KD_CHECK_EQ(r.op->text, "hello wor");

    // The safe configuration, same input, commits nothing.
    Driver safe(local_params(/*m=*/3, /*t=*/200));
    KD_CHECK(!safe.partial("hello wor").op.has_value());

    d.partial("hello world");  // an extension: fine
    KD_CHECK_EQ(d.injected(), "hello world");

    auto bad = d.partial("hello worm");  // ...and now the document is wrong, irrevocably
    KD_REQUIRE(bad.violation.has_value());
    KD_CHECK_EQ(static_cast<int>(bad.violation->kind),
                static_cast<int>(ViolationKind::CommittedTextContradicted));
    KD_CHECK_EQ(d.engine().committed_text(), "hello world");  // still not repaired
    KD_CHECK_EQ(d.engine().counters().erroneous_graphemes, uint64_t{2});  // "ld"
    KD_CHECK_EQ(d.engine().counters().committed_graphemes, uint64_t{11});
    KD_CHECK_NEAR(d.engine().counters().icer_per_1000(), 2000.0 / 11.0, 1e-9);

    // In this mode — and only in this mode — the latch is utterance-scoped, so a bench run keeps
    // producing ICER numbers past the first (expected) violation instead of stopping at it.
    d.begin_utterance(2);
    auto next = d.partial("new utterance");
    KD_CHECK(next.ok());
    KD_CHECK(next.op.has_value());
}

// ═══ salvage_tail — keeping the user's words after a contradiction ══════════════════════════
//
// A contradicted hypothesis used to cost the user the whole rest of the utterance: the engine
// reported the violation, main.cpp returned, and every word after the divergence was dropped
// with no trace. That is the failure plan.md §1.4 ranks worst — text the user said, gone,
// with no way to know it was ever there.
//
// The document is still never repaired. `salvage` only ever *appends*, so the contradicted text
// stays exactly where it is, wrong and visible. These tests pin the alignment that decides what
// gets appended.

KD_TEST(commit_engine, salvage_appends_only_what_follows_the_agreement) {
    // The measured case, from the first real session against a batch ASR model: the final
    // request sees the whole utterance for the first time and inserts sentence punctuation into
    // text that was committed seconds earlier.
    const std::string committed = "How are you I really";
    const std::string incoming = "How are you? I really wanna know.";
    KD_CHECK_EQ(kd::salvage_tail(committed, incoming), std::string("wanna know."));
}

KD_TEST(commit_engine, salvage_sees_through_case_and_edge_punctuation) {
    // The two revisions a batch model actually makes. Neither is a different word, so neither
    // may break the alignment.
    KD_CHECK_EQ(kd::salvage_tail("how are you", "How are you, and more"), std::string("and more"));
    KD_CHECK_EQ(kd::salvage_tail("one two three", "one two three! four"), std::string("four"));
}

KD_TEST(commit_engine, salvage_is_empty_when_the_incoming_text_adds_nothing) {
    // Agreement that runs to the end of the incoming hypothesis: there is nothing after it to
    // append, and appending the agreement itself would duplicate the document's text.
    KD_CHECK(kd::salvage_tail("one two three", "one two three").empty());
    KD_CHECK(kd::salvage_tail("one two three", "One, two, three!").empty());
    KD_CHECK(kd::salvage_tail("one two three", "one two").empty());
}

KD_TEST(commit_engine, salvage_keeps_everything_when_nothing_matches) {
    // No agreement at all — a genuinely different transcription. Duplication is visible and the
    // user can delete it; silent loss is invisible and permanent, so this deliberately errs
    // toward the recoverable failure.
    KD_CHECK_EQ(kd::salvage_tail("how are you", "what time is it"),
                std::string("what time is it"));
}

KD_TEST(commit_engine, salvage_diverging_midway_appends_from_the_divergence) {
    // A real word changed rather than punctuation. Everything from that word on is appended,
    // which duplicates the changed word's replacement rather than dropping the tail.
    KD_CHECK_EQ(kd::salvage_tail("i really wanna know", "i really want to know"),
                std::string("want to know"));
}

KD_TEST(commit_engine, salvage_handles_empty_and_whitespace_inputs) {
    KD_CHECK(kd::salvage_tail("anything", "").empty());
    KD_CHECK(kd::salvage_tail("anything", "   ").empty());
    KD_CHECK_EQ(kd::salvage_tail("", "some words"), std::string("some words"));
}

KD_TEST(commit_engine, salvage_never_returns_a_prefix_of_the_committed_text) {
    // The property that makes appending safe: whatever comes back must not re-state text the
    // document already holds, because the caller appends it verbatim.
    const std::string committed = "the quick brown fox";
    for (const char* incoming : {"the quick brown fox jumps over", "The quick, brown fox! jumps",
                                 "the quick brown fox", "the quick"}) {
        const std::string tail = kd::salvage_tail(committed, incoming);
        KD_CHECK(tail.empty() || committed.find(tail) != 0);
    }
}
