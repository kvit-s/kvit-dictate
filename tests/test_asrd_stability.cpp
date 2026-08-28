// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// tests/test_asrd_stability.cpp — the T2 gate.
//
// ═══ Why a second corpus exists ═════════════════════════════════════════════════════════════
//
// `tests/fixtures/protocol/` cannot catch a divergence here. docs/protocol.md constrains the
// *sequence* of marks a Server may emit — non-decreasing, on a cluster boundary, prefix stable
// — not which mark plan.md §1.4 says it should choose. Two conforming servers can publish
// different numbers for the same hypotheses and both pass all 21 cases, which is exactly the
// risk openai-translator.md §8 names: "porting stability.py introduces a divergence the
// fixtures do not catch."
//
// So `tests/fixtures/stability/` is a second corpus, **recorded from the Python tracker** by
// `server/tools/gen_stability_fixtures.py`. Every expected value in it is what
// `kvit_asr.stability.StabilityTracker` actually answered, not what anybody thought it should.
// `server/tests/test_stability_fixtures.py` runs the same file, so a regression in the oracle
// is caught rather than silently re-baselined on the next regeneration.
//
// **A failure here is a failed port, not a refinement.** The most likely single cause is step
// 3's two readings — subtract M from the common prefix, or cap the candidate at M clusters
// behind the decode head. stability.py:831-836 takes the cap; src/asrd/stability.cpp takes the
// cap; case 003 is the one that notices if that ever stops being true.

#include "kd_test.h"

#include "asrd/stability.h"
#include "util/clock.h"
#include "util/json.h"
#include "util/utf8.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using kd::json::Value;

std::string str_of(const Value* v, const char* dflt = "") {
    std::string_view sv;
    if (v != nullptr && v->get_string(sv)) return std::string(sv);
    return std::string(dflt);
}

bool int_of(const Value* v, int64_t& out) { return v != nullptr && v->get_int(out); }

bool load_json(const std::string& relative, Value& out, std::string& why) {
    const std::string path = kd::test::fixture_path(relative);
    std::string text;
    if (!kd::test::read_file(path, text)) {
        why = "cannot read " + path;
        return false;
    }
    kd::json::Error err;
    kd::json::Options opts;
    // These files carry whole hypotheses rather than one protocol frame, so the 64 KiB wire
    // bound does not apply to them.
    opts.max_bytes = 1u << 20;
    if (!kd::json::parse(text, out, err, opts)) {
        why = path + ": " + std::string(kd::json::to_string(err.code)) + " at " +
              std::to_string(err.offset);
        return false;
    }
    return true;
}

void run_case(const std::string& id, const Value& doc) {
    const Value* pv = doc.find("params");
    kd::asrd::StabilityParams params;
    if (pv != nullptr) {
        int64_t n = 0;
        if (int_of(pv->find("hypotheses_n"), n)) params.hypotheses_n = static_cast<uint32_t>(n);
        if (int_of(pv->find("retract_clusters_m"), n)) {
            params.retract_clusters_m = static_cast<uint32_t>(n);
        }
        if (int_of(pv->find("hold_ms_t"), n)) params.hold_ms_t = static_cast<uint32_t>(n);
    }

    // A manual clock, because the dwell is the one part of this algorithm that is time-driven
    // and a real clock would make case 002 pass or fail according to how loaded the machine is.
    kd::ManualClock clock(0);
    kd::asrd::StabilityTracker tracker(params, &clock);

    const Value* steps = doc.find("steps");
    KD_REQUIRE(steps != nullptr);

    for (size_t i = 0; i < steps->size(); ++i) {
        const Value* sp = steps->value_at(i);
        KD_REQUIRE(sp != nullptr);
        const std::string op = str_of(sp->find("op"));
        const std::string where = id + " step[" + std::to_string(i) + "] " + op;

        if (op == "reset") {
            int64_t utt = 0;
            int_of(sp->find("utterance_id"), utt);
            tracker.reset(static_cast<uint64_t>(utt));
            continue;
        }
        if (op == "advance") {
            int64_t ms = 0;
            KD_REQUIRE(int_of(sp->find("ms"), ms));
            clock.advance(static_cast<kd::Millis>(ms));
            continue;
        }
        if (op == "stop_after_violation") {
            // The program ends here by construction: §8.2 binds the next message against the
            // previously published mark, so a refused utterance has no conforming successor.
            break;
        }

        uint64_t stable = 0;
        kd::asrd::Violation v;
        bool ok = false;
        if (op == "observe") {
            ok = tracker.observe(str_of(sp->find("text")), stable, v);
        } else if (op == "poll") {
            ok = tracker.poll(stable, v);
        } else if (op == "finalize") {
            ok = tracker.finalize(str_of(sp->find("text")), stable, v);
        } else {
            KD_FAIL(where + ": unknown op");
            return;
        }

        const Value* want_violation = sp->find("expect_violation");
        if (want_violation != nullptr) {
            if (ok) {
                KD_FAIL(where + ": Python refused this step with \"" + str_of(want_violation) +
                        "\" and the C++ tracker published stable_bytes=" +
                        std::to_string(stable));
                return;
            }
            KD_CHECK_EQ(std::string(kd::asrd::to_string(v.kind)), str_of(want_violation));
            int64_t want_diverge = 0;
            if (int_of(sp->find("expect_diverge_offset"), want_diverge)) {
                // The load-bearing field of §8.3's log line: zero means the backend revised the
                // *first word*, which is the difference between "tune the stability knobs" and
                // "this backend must never publish a mark before the final".
                KD_CHECK_EQ(v.diverge_offset, static_cast<uint64_t>(want_diverge));
            }
            continue;
        }

        int64_t want_stable = 0;
        KD_REQUIRE(int_of(sp->find("expect_stable_bytes"), want_stable));
        if (!ok) {
            KD_FAIL(where + ": Python published stable_bytes=" + std::to_string(want_stable) +
                    " and the C++ tracker refused with " + std::string(kd::asrd::to_string(v.kind)));
            return;
        }
        if (stable != static_cast<uint64_t>(want_stable)) {
            KD_FAIL(where + ": Python said stable_bytes=" + std::to_string(want_stable) +
                    ", C++ said " + std::to_string(stable) +
                    ". This is a failed port, not a refinement — see the header of this file.");
            continue;
        }
        KD_CHECK(true);

        int64_t want_high = 0;
        if (int_of(sp->find("expect_high_water"), want_high)) {
            KD_CHECK_EQ(tracker.high_water(), static_cast<uint64_t>(want_high));
        }
    }
}

}  // namespace

// ─── The corpus ─────────────────────────────────────────────────────────────────────────────

KD_TEST(asrd_stability, agrees_with_the_python_tracker_byte_for_byte) {
    Value index;
    std::string why;
    KD_REQUIRE(load_json("stability/index.json", index, why));
    const Value* cases = index.find("cases");
    KD_REQUIRE(cases != nullptr);
    KD_CHECK_GT(cases->size(), size_t{0});

    for (size_t i = 0; i < cases->size(); ++i) {
        const Value* cp = cases->value_at(i);
        KD_REQUIRE(cp != nullptr);
        const std::string id = str_of(cp->find("id"));
        Value doc;
        std::string load_why;
        if (!load_json("stability/" + str_of(cp->find("file")), doc, load_why)) {
            KD_FAIL(load_why);
            continue;
        }
        // The index and the case file each state the id; they must agree, or a case can be run
        // under the wrong name and a failure points at the wrong file.
        KD_CHECK_EQ(str_of(doc.find("id")), id);
        run_case(id, doc);
    }
    KD_INFO("ran " + std::to_string(cases->size()) + " recorded cases");
}

// ─── Properties the corpus states by example and this file states as invariants ─────────────

KD_TEST(asrd_stability, the_published_mark_is_always_a_grapheme_boundary) {
    // Not a restatement of the corpus: this drives the tracker with hypotheses that end inside
    // clusters, which the recorded cases do not, and asserts the invariant rather than a value.
    static const char* kHypotheses[] = {
        "cafe\xCC\x81 au lait",           // e + combining acute
        "cafe\xCC\x81 au lait pl",
        "cafe\xCC\x81 au lait please",
        "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7 family here",
        "\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7 devanagari cluster",
    };
    kd::ManualClock clock(0);
    kd::asrd::StabilityTracker tracker({}, &clock);
    for (const char* text : kHypotheses) {
        tracker.reset(1);
        for (int round = 0; round < 3; ++round) {
            uint64_t stable = 0;
            kd::asrd::Violation v;
            KD_REQUIRE(tracker.observe(text, stable, v));
            clock.advance(300);
            KD_REQUIRE(tracker.poll(stable, v));
            KD_CHECK(kd::utf8::is_grapheme_boundary(text, static_cast<size_t>(stable)));
        }
    }
}

KD_TEST(asrd_stability, refuses_rather_than_clamps) {
    // §8.3 forbids clamping because it hides the defect ICER exists to measure, and because in
    // this direction it would mean shipping a message whose bytes below the published mark have
    // changed — the failure that corrupts the user's document.
    kd::ManualClock clock(0);
    kd::asrd::StabilityTracker tracker({}, &clock);
    tracker.reset(1);

    uint64_t stable = 0;
    kd::asrd::Violation v;
    KD_REQUIRE(tracker.observe("one two three four five", stable, v));
    KD_REQUIRE(tracker.observe("one two three four five", stable, v));
    clock.advance(300);
    KD_REQUIRE(tracker.poll(stable, v));
    KD_REQUIRE_GT(stable, uint64_t{0});
    const uint64_t published = stable;

    // A synthetic backwards step, straight at check(). Nothing in the tracker's own paths can
    // produce this, which is why it is worth testing directly.
    KD_CHECK(!tracker.check("one two three four five", published - 1, v));
    KD_CHECK(v.kind == kd::asrd::ViolationKind::StableBytesDecreased);

    // Out of range.
    KD_CHECK(!tracker.check("one two", 100, v));
    KD_CHECK(v.kind == kd::asrd::ViolationKind::StableBytesOutOfRange);

    // And the high-water mark is untouched by a refusal: a refused emission is not sent, so
    // nothing about what was published has changed.
    KD_CHECK_EQ(tracker.high_water(), published);
}

KD_TEST(asrd_stability, the_wire_message_carries_no_recognised_text) {
    // plan.md §1.9 and util/log.h: `error.msg` travels to the Client, which logs it verbatim
    // into a file the user keeps, and the Client cannot redact prose it did not compose. So the
    // diagnosis must be numbers only. This is asserted rather than reviewed because the tempting
    // fix for a hard stability bug is to put the two texts in the message.
    kd::ManualClock clock(0);
    kd::asrd::StabilityTracker tracker({}, &clock);
    tracker.reset(7);

    uint64_t stable = 0;
    kd::asrd::Violation v;
    KD_REQUIRE(tracker.observe("the secret passphrase is swordfish", stable, v));
    KD_REQUIRE(tracker.observe("the secret passphrase is swordfish", stable, v));
    clock.advance(300);
    KD_REQUIRE(tracker.poll(stable, v));
    KD_REQUIRE(!tracker.observe("THE secret passphrase is swordfish", stable, v));

    const std::string message = v.to_wire_message();
    KD_CHECK(message.find("swordfish") == std::string::npos);
    KD_CHECK(message.find("passphrase") == std::string::npos);
    KD_CHECK(message.find("stable_prefix_mutated") != std::string::npos);
    KD_CHECK(message.find("utterance_id=7") != std::string::npos);
    KD_CHECK(message.find("diverge_offset=0") != std::string::npos);
    KD_CHECK(message.find("redacted") != std::string::npos);
}

KD_TEST(asrd_stability, reset_clears_the_dwell_as_well_as_the_mark) {
    // plan.md §1.4 step 7. "Entirely" includes the dwell timer, and forgetting it is the subtle
    // way to get this wrong: the next utterance's first candidate would inherit a hold that
    // matured under the previous one and publish on its very first evaluation.
    kd::ManualClock clock(0);
    kd::asrd::StabilityTracker tracker({}, &clock);
    tracker.reset(1);

    uint64_t stable = 0;
    kd::asrd::Violation v;
    KD_REQUIRE(tracker.observe("alpha beta gamma delta", stable, v));
    clock.advance(10000);
    KD_REQUIRE(tracker.observe("alpha beta gamma delta", stable, v));
    KD_CHECK_EQ(stable, uint64_t{0});  // the candidate is 0 ms old, however long ago it started

    tracker.reset(2);
    KD_CHECK_EQ(tracker.high_water(), uint64_t{0});
    KD_CHECK(tracker.stable_prefix().empty());
    KD_CHECK(!tracker.has_text());
    KD_CHECK_EQ(tracker.utterance_id(), uint64_t{2});

    KD_REQUIRE(tracker.observe("alpha beta gamma delta", stable, v));
    KD_REQUIRE(tracker.observe("alpha beta gamma delta", stable, v));
    KD_CHECK_EQ(stable, uint64_t{0});
}

KD_TEST(asrd_stability, poll_before_any_hypothesis_is_a_no_op) {
    kd::ManualClock clock(0);
    kd::asrd::StabilityTracker tracker({}, &clock);
    tracker.reset(1);
    uint64_t stable = 99;
    kd::asrd::Violation v;
    KD_CHECK(tracker.poll(stable, v));
    KD_CHECK_EQ(stable, uint64_t{0});
}

KD_TEST(asrd_stability, poll_does_not_count_as_a_second_opinion) {
    // A repeated evaluation is not a second hypothesis, and counting it as one would make N
    // mean "N evaluations" rather than "N hypotheses" — which would let a single guess become
    // stable simply by being polled twice.
    kd::ManualClock clock(0);
    kd::asrd::StabilityTracker tracker({}, &clock);
    tracker.reset(1);

    uint64_t stable = 0;
    kd::asrd::Violation v;
    KD_REQUIRE(tracker.observe("one two three four", stable, v));
    for (int i = 0; i < 10; ++i) {
        clock.advance(1000);
        KD_REQUIRE(tracker.poll(stable, v));
        KD_CHECK_EQ(stable, uint64_t{0});
    }
}
