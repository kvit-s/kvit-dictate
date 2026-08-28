// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Tests for src/session.cpp — the session / utterance state machine.
//
// The properties under test are the ones plan.md §7 gates the product on: no wrong-window
// injection, no self-stop on our own injected input, clean recovery from a sidecar crash, and
// the two silence timers staying independent. Every one of them is reachable here with no
// window, no hook, no mic and no sidecar, which is the entire reason the machine takes an IClock
// and an ITargetProvider instead of calling GetForegroundWindow itself.
//
// Suite name: `session` (registered DISABLED in CMakeLists until the integration pass moves it).

#include "kd_test.h"

#include <string>
#include <vector>

#include "hotkey.h"
#include "session.h"

namespace {

using kd::Effect;
using kd::LocalEvent;
using kd::TransportEvent;
using kd::Verdict;
namespace proto = kd::proto;

// ─── A target provider with no Windows in it ────────────────────────────────────────────────
//
// Note that it compares identities itself rather than calling TargetIdentity::same_as(): that
// function lives in injector.cpp, and a test of the session machine should fail only when the
// session machine is wrong.
class FakeTarget final : public kd::ITargetProvider {
public:
    kd::TargetIdentity foreground;
    kd::IntegrityLevel self = kd::IntegrityLevel::Medium;
    bool secure = false;
    bool capture_fails = false;
    int captures = 0;
    int validations = 0;

    FakeTarget() {
        foreground.top_level = 0x1000;
        foreground.focus = 0x1001;
        foreground.thread_id = 42;
        foreground.process_id = 4242;
        foreground.integrity = kd::IntegrityLevel::Medium;
        foreground.process_name = "notepad.exe";
    }

    bool capture_target(kd::TargetIdentity& out) override {
        ++captures;
        if (capture_fails) return false;
        out = foreground;
        return true;
    }

    bool target_still_current(const kd::TargetIdentity& captured) override {
        ++validations;
        return captured.top_level == foreground.top_level &&
               captured.focus == foreground.focus &&
               captured.thread_id == foreground.thread_id;
    }

    bool secure_desktop_active() override { return secure; }
    kd::IntegrityLevel self_integrity() override { return self; }
};

// ─── Message builders ───────────────────────────────────────────────────────────────────────

proto::Message partial_msg(const std::string& sid, uint64_t utt, uint64_t seq,
                           const std::string& text, uint64_t stable) {
    proto::PartialMsg p;
    p.session_id = sid;
    p.utterance_id = utt;
    p.seq = seq;
    p.text = text;
    p.stable_bytes = stable;
    return proto::Message::of(p);
}

proto::Message final_msg(const std::string& sid, uint64_t utt, uint64_t seq,
                         const std::string& text) {
    proto::FinalMsg f;
    f.session_id = sid;
    f.utterance_id = utt;
    f.seq = seq;
    f.text = text;
    f.audio_pos_ms = 4820;
    return proto::Message::of(f);
}

proto::Message stopped_msg(const std::string& sid) {
    proto::StoppedMsg s;
    s.session_id = sid;
    return proto::Message::of(s);
}

// Walk the real handshake rather than restore()-ing into IDLE: the transitions are part of what
// is being tested, and a test that starts from a hand-written state can pass over a machine that
// can never reach it.
void bring_to_idle(kd::SessionMachine& m) {
    KD_REQUIRE_EQ(m.on_transport(TransportEvent::Connected).verdict, Verdict::Accept);
    const auto hello = m.on_transport(TransportEvent::PeerValidated);
    KD_REQUIRE_EQ(hello.to_send.size(), size_t{1});
    KD_REQUIRE_EQ(hello.to_send[0].type(), proto::MessageType::Hello);

    proto::HelloAckMsg ack;
    ack.proto = proto::kProtoVersion;
    KD_REQUIRE_EQ(m.on_message(proto::Message::of(ack)).verdict, Verdict::Accept);

    proto::ReadyMsg ready;
    ready.model = "example/streaming-en";
    ready.warm_ms = 2140;
    ready.mode = proto::StreamingMode::NativeStreaming;
    KD_REQUIRE_EQ(m.on_message(proto::Message::of(ready)).verdict, Verdict::Accept);
    KD_REQUIRE_EQ(m.state(), proto::ConnState::Idle);
}

// Start a session and return its id, asserting that a `start` frame was produced.
std::string start_session(kd::SessionMachine& m) {
    const auto out = m.on_local(LocalEvent::FinalisePressed);
    KD_REQUIRE_EQ(out.verdict, Verdict::Sent);
    KD_REQUIRE_EQ(out.to_send.size(), size_t{1});
    KD_REQUIRE_EQ(out.to_send[0].type(), proto::MessageType::Start);
    KD_REQUIRE_EQ(m.state(), proto::ConnState::Running);
    return m.current_session_id();
}

bool sends(const kd::SessionMachine::Outcome& o, proto::MessageType t) {
    for (const auto& m : o.to_send) {
        if (m.type() == t) return true;
    }
    return false;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  plan.md §1.6 — the two timers a naive implementation merges
// ═══════════════════════════════════════════════════════════════════════════════════════════

// Both silence timers hang off `last_voice_ms`, whose only writer is `LocalEvent::SpeechDetected`.
//
// That event was raised from nowhere in src/ — every call site was in this file — so the value
// stayed as seeded at `start` and the timers measured wall-clock since activation instead of
// silence. The endpoint timer, additionally guarded by `utterance_open` (set in the same
// unreachable handler), never fired at all; the session timer has no such guard and so imposed a
// hard 120 s cap on every dictation however much the user was saying. `App` now derives the event
// from the sidecar's own `partial`/`final` text, which is the component that owns the VAD.
//
// These two pin both halves: silence still ends the session, and speech still prevents it.
KD_TEST(session, speech_keeps_the_session_alive_past_the_silence_timer) {
    kd::ManualClock clock(1000);
    kd::SessionConfig cfg;  // 120 s session timer
    kd::SessionMachine m(clock, cfg);
    bring_to_idle(m);
    start_session(m);

    // Three minutes of steady talking, reported the way App reports it.
    for (int i = 0; i < 6; ++i) {
        clock.advance(30000);
        m.on_local(LocalEvent::SpeechDetected);
        KD_CHECK_EQ(m.on_tick().verdict, Verdict::Accept);
        KD_CHECK_EQ(m.state(), proto::ConnState::Running);
    }
    KD_CHECK_EQ(m.state(), proto::ConnState::Running);   // still dictating at 3 minutes
}

KD_TEST(session, silence_past_the_session_timer_still_ends_it) {
    kd::ManualClock clock(1000);
    kd::SessionConfig cfg;
    kd::SessionMachine m(clock, cfg);
    bring_to_idle(m);
    start_session(m);

    // One burst of speech, then nothing for over two minutes.
    m.on_local(LocalEvent::SpeechDetected);
    clock.advance(60000);
    KD_CHECK_EQ(m.on_tick().verdict, Verdict::Accept);
    KD_CHECK_EQ(m.state(), proto::ConnState::Running);   // a one-minute pause is not the end

    clock.advance(61000);                                 // now past 120 s since the last word
    const auto t = m.on_tick();
    KD_CHECK_EQ(t.verdict, Verdict::Sent);
    KD_CHECK(sends(t, proto::MessageType::Stop));
    KD_CHECK_EQ(m.state(), proto::ConnState::Finalizing);
}

KD_TEST(session, thinking_pause_ends_the_utterance_and_not_the_session) {
    kd::ManualClock clock(1000);
    kd::SessionConfig cfg;  // 800 ms endpoint, 120 s session — the shipped defaults
    kd::SessionMachine m(clock, cfg);
    bring_to_idle(m);
    start_session(m);

    m.on_local(LocalEvent::SpeechDetected);
    KD_CHECK(m.utterance_open());
    KD_CHECK(!m.next_utterance_needs_leading_space());  // nothing spoken before this

    // 500 ms of quiet: neither timer has anything to say.
    clock.advance(500);
    auto t = m.on_tick();
    KD_CHECK(t.to_send.empty());
    KD_CHECK(m.utterance_open());

    // 900 ms: the endpoint fires. The UTTERANCE ends; the SESSION does not.
    clock.advance(400);
    t = m.on_tick();
    KD_CHECK(t.to_send.empty());
    KD_CHECK(!m.utterance_open());
    KD_CHECK_EQ(m.utterances_completed(), uint64_t{1});
    KD_CHECK_EQ(m.state(), proto::ConnState::Running);
    KD_CHECK(m.next_utterance_needs_leading_space());

    // A full minute of thinking. Nothing is sent, the session stays open, the mic stays live.
    for (int i = 0; i < 60; ++i) {
        clock.advance(1000);
        const auto tick = m.on_tick();
        KD_REQUIRE(tick.to_send.empty());
    }
    KD_CHECK_EQ(m.state(), proto::ConnState::Running);
    KD_CHECK_EQ(m.utterances_completed(), uint64_t{1});

    // Resuming starts a NEW utterance, appended after a space (plan.md §1.6).
    m.on_local(LocalEvent::SpeechDetected);
    KD_CHECK(m.utterance_open());
    KD_CHECK(m.next_utterance_needs_leading_space());
    KD_CHECK_EQ(m.state(), proto::ConnState::Running);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Editing pauses the session-silence timer — docs/editing.md §9
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(session, editing_disarms_the_session_silence_timer) {
    kd::ManualClock clock(1000);
    kd::SessionMachine m(clock, kd::SessionConfig{});
    bring_to_idle(m);
    start_session(m);

    m.on_local(LocalEvent::SpeechDetected);
    KD_CHECK_EQ(m.on_local(LocalEvent::EditingBegan).verdict, Verdict::Accept);

    clock.advance(60000);
    KD_CHECK_EQ(m.on_tick().verdict, Verdict::Accept);
    KD_CHECK_EQ(m.state(), proto::ConnState::Running);
}

KD_TEST(session, a_session_that_spends_longer_than_the_timer_editing_does_not_end) {
    // docs/editing.md §11 names this one as "the one that will actually break", and §9 says why
    // it matters: the timer measures how long a session survives *with no speech*, and during
    // editing there is no microphone to be silent into. Leaving it armed would type a
    // half-finished buffer into the document because the user spent two minutes reading what
    // they had dictated. progress.md §8 records what happened the last time "silence" and
    // "wall-clock since activation" were confused: a hard two-minute cap on every dictation,
    // with a green test suite.
    kd::ManualClock clock(1000);
    kd::SessionConfig cfg;  // 120 s
    kd::SessionMachine m(clock, cfg);
    bring_to_idle(m);
    start_session(m);

    m.on_local(LocalEvent::SpeechDetected);
    m.on_local(LocalEvent::EditingBegan);

    // Five minutes of editing — well past `session_silence_ms`, ticked the way App ticks.
    for (int i = 0; i < 300; ++i) {
        clock.advance(1000);
        const auto t = m.on_tick();
        KD_REQUIRE(t.to_send.empty());
    }
    KD_CHECK_EQ(m.state(), proto::ConnState::Running);
}

KD_TEST(session, resuming_re_arms_the_session_timer_from_now) {
    kd::ManualClock clock(1000);
    kd::SessionMachine m(clock, kd::SessionConfig{});
    bring_to_idle(m);
    start_session(m);

    m.on_local(LocalEvent::SpeechDetected);
    m.on_local(LocalEvent::EditingBegan);
    clock.advance(300000);  // five minutes of editing

    // Armed from *now*, not from whenever speech was last heard. Charging the editing time
    // against the timer would end the session moments after the user resumed — which is the
    // failure that makes the resume gesture useless.
    KD_CHECK_EQ(m.on_local(LocalEvent::DictationResumed).verdict, Verdict::Accept);
    clock.advance(60000);
    KD_CHECK_EQ(m.on_tick().verdict, Verdict::Accept);
    KD_CHECK_EQ(m.state(), proto::ConnState::Running);

    // ...and it is armed, not disabled. Two more minutes of real silence still ends it.
    clock.advance(61000);
    const auto t = m.on_tick();
    KD_CHECK_EQ(t.verdict, Verdict::Sent);
    KD_CHECK(sends(t, proto::MessageType::Stop));
}

KD_TEST(session, an_open_utterance_is_flushed_when_editing_begins) {
    // docs/editing.md §3.3: the user pressed an arrow key to go and fix something they just
    // said, and the thing they just said had better be editable by the time they get there. So
    // the in-flight utterance is flushed rather than abandoned — the endpoint path, reached by a
    // gesture instead of by a timer.
    kd::ManualClock clock(1000);
    kd::SessionMachine m(clock, kd::SessionConfig{});
    bring_to_idle(m);
    start_session(m);

    m.on_local(LocalEvent::SpeechDetected);
    KD_REQUIRE(m.utterance_open());

    const auto out = m.on_local(LocalEvent::EditingBegan);
    KD_CHECK(out.has_effect(Effect::FlushPendingAudio));
    KD_CHECK(!m.utterance_open());
    KD_CHECK_EQ(m.utterances_completed(), uint64_t{1});

    // No `stop`, no `cancel`: the session is still the user's and the microphone closing is not
    // the session ending.
    KD_CHECK(out.to_send.empty());
    KD_CHECK_EQ(m.state(), proto::ConnState::Running);
}

KD_TEST(session, editing_with_no_open_utterance_flushes_nothing) {
    kd::ManualClock clock(1000);
    kd::SessionMachine m(clock, kd::SessionConfig{});
    bring_to_idle(m);
    start_session(m);

    const auto out = m.on_local(LocalEvent::EditingBegan);
    KD_CHECK(!out.has_effect(Effect::FlushPendingAudio));
    KD_CHECK_EQ(out.verdict, Verdict::Accept);
}

KD_TEST(session, the_editing_events_are_inert_outside_a_running_session) {
    // Both arrive from the hook thread by way of a posted message, so both can land after the
    // session they belong to has ended — a `stopped` that overtook them, or an abort. Accepting
    // and doing nothing is the only answer that does not make the timing a bug.
    kd::ManualClock clock(1000);
    kd::SessionMachine m(clock, kd::SessionConfig{});
    const proto::ConnState before = m.state();
    KD_CHECK_EQ(m.on_local(LocalEvent::EditingBegan).verdict, Verdict::Accept);
    KD_CHECK_EQ(m.on_local(LocalEvent::DictationResumed).verdict, Verdict::Accept);
    KD_CHECK_EQ(m.state(), before);

    bring_to_idle(m);
    KD_CHECK_EQ(m.on_local(LocalEvent::EditingBegan).verdict, Verdict::Accept);
    KD_CHECK_EQ(m.on_local(LocalEvent::DictationResumed).verdict, Verdict::Accept);
    KD_CHECK_EQ(m.state(), proto::ConnState::Idle);
}

KD_TEST(session, a_session_that_ended_while_editing_does_not_disarm_the_next_ones_timer) {
    // The shape progress.md §8 is a catalogue of: state cleared on one of the two paths a session
    // can end through. `editing` is session-scoped, so clear_session() has to clear it — or the
    // next session runs with no session-silence timer at all and nobody notices, because the only
    // symptom is a session that never times out.
    kd::ManualClock clock(1000);
    kd::SessionMachine m(clock, kd::SessionConfig{});
    bring_to_idle(m);
    const std::string sid = start_session(m);

    m.on_local(LocalEvent::SpeechDetected);
    m.on_local(LocalEvent::EditingBegan);
    m.on_local(LocalEvent::FinalisePressed);
    m.on_message(stopped_msg(sid));
    KD_REQUIRE_EQ(m.state(), proto::ConnState::Idle);

    start_session(m);
    m.on_local(LocalEvent::SpeechDetected);
    clock.advance(121000);
    const auto t = m.on_tick();
    KD_CHECK_EQ(t.verdict, Verdict::Sent);
    KD_CHECK(sends(t, proto::MessageType::Stop));
}

KD_TEST(session, the_finalise_event_still_spells_itself_for_the_fixture_runner) {
    // session.h warns that the fixture runner compares these as strings. The rename in
    // docs/editing.md §8 changed one, and no fixture under tests/fixtures/ uses it — but the
    // spelling is part of the cross-language contract's vocabulary either way, so it is pinned
    // here rather than left to be discovered by a fixture that starts using it.
    KD_CHECK_EQ(kd::to_string(LocalEvent::FinalisePressed), "finalise_pressed");
    KD_CHECK_EQ(kd::to_string(LocalEvent::EditingBegan), "editing_began");
    KD_CHECK_EQ(kd::to_string(LocalEvent::DictationResumed), "dictation_resumed");
}

KD_TEST(session, an_utterance_boundary_does_not_reset_the_commit_engine) {
    // ResetCommitEngine is SESSION-scoped: the owner maps it onto CommitEngine::reset(), which
    // calls clear_session(). Emitting it at an utterance boundary wiped `session_has_committed`,
    // the only input to CommitOp::leading_space — so the event that is supposed to produce the
    // space between two utterances was the event that made it impossible, and Notepad3 received
    // "…the default modelopen src/protocol.h…".
    //
    // The engine resets its own utterance state when it sees the id change (see
    // commit_engine's leading_space_survives_an_unannounced_utterance_change), so nothing is
    // lost by staying quiet here.
    kd::ManualClock clock(1000);
    kd::SessionMachine m(clock, kd::SessionConfig{});
    bring_to_idle(m);
    const std::string sid = start_session(m);

    auto out = m.on_message(partial_msg(sid, 1, 1, "can you change the default model", 12));
    KD_REQUIRE_EQ(out.verdict, kd::Verdict::Accept);

    // Same utterance: no reset either way.
    out = m.on_message(partial_msg(sid, 1, 2, "can you change the default model", 20));
    KD_CHECK(!out.has_effect(Effect::ResetCommitEngine));

    // The utterance boundary itself.
    out = m.on_message(partial_msg(sid, 2, 3, "open src/protocol.h", 4));
    KD_REQUIRE_EQ(out.verdict, kd::Verdict::Accept);
    KD_CHECK(!out.has_effect(Effect::ResetCommitEngine));
    // The text still has to reach the engine — that is what carries the new utterance_id.
    KD_CHECK(out.has_effect(Effect::ApplyText));
}

KD_TEST(session, session_silence_ends_the_session_with_stop) {
    kd::ManualClock clock(1000);
    kd::SessionConfig cfg;
    cfg.session_silence_ms = 120000;
    kd::SessionMachine m(clock, cfg);
    bring_to_idle(m);
    const std::string sid = start_session(m);

    m.on_local(LocalEvent::SpeechDetected);

    clock.advance(119999);
    auto t = m.on_tick();
    KD_CHECK(t.to_send.empty());
    KD_CHECK_EQ(m.state(), proto::ConnState::Running);

    clock.advance(1);
    t = m.on_tick();
    KD_CHECK_EQ(t.verdict, Verdict::Sent);
    KD_CHECK(sends(t, proto::MessageType::Stop));
    // §6.3's Client obligation: no audio for a session may follow its terminating control frame.
    KD_CHECK(t.has_effect(Effect::FlushPendingAudio));
    KD_CHECK_EQ(m.state(), proto::ConnState::Finalizing);
    KD_CHECK_EQ(m.current_session_id(), sid);
}

KD_TEST(session, session_silence_never_keeps_the_session_open) {
    kd::ManualClock clock(1000);
    kd::SessionConfig cfg;
    cfg.session_silence_never = true;  // a legitimate setting (plan.md §1.6)
    kd::SessionMachine m(clock, cfg);
    bring_to_idle(m);
    start_session(m);
    m.on_local(LocalEvent::SpeechDetected);

    for (int i = 0; i < 20; ++i) {  // ten minutes
        clock.advance(30000);
        const auto t = m.on_tick();
        KD_REQUIRE(t.to_send.empty());
    }
    KD_CHECK_EQ(m.state(), proto::ConnState::Running);
    // The endpoint timer still ran: silence ends utterances even when it never ends the session.
    KD_CHECK(!m.utterance_open());
    KD_CHECK_EQ(m.utterances_completed(), uint64_t{1});
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  docs/protocol.md §7.1 — session-id discipline
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(session, late_final_from_a_stale_session_is_dropped) {
    kd::ManualClock clock(1000);
    kd::SessionMachine m(clock, kd::SessionConfig{});
    bring_to_idle(m);
    const std::string sid = start_session(m);
    KD_REQUIRE_NE(sid, std::string("s0"));

    // The failure this whole module exists to prevent: a `final` from a finished session
    // arriving after the user has moved on. Dropped — not clamped, not queued, not injected.
    auto out = m.on_message(final_msg("s0", 3, 13, "Stale text."));
    KD_CHECK_EQ(out.verdict, Verdict::DropStaleSession);
    KD_CHECK(!out.has_effect(Effect::ApplyText));
    KD_CHECK(out.to_send.empty());
    KD_CHECK_EQ(m.last_seq(), uint64_t{0});  // a stale seq may not move the high-water mark

    out = m.on_message(partial_msg("s0", 3, 12, "stale text", 5));
    KD_CHECK_EQ(out.verdict, Verdict::DropStaleSession);
    KD_CHECK(!out.has_effect(Effect::ApplyText));

    // The live one still applies.
    out = m.on_message(partial_msg(sid, 1, 1, "live text", 4));
    KD_CHECK_EQ(out.verdict, Verdict::Accept);
    KD_CHECK(out.has_effect(Effect::ApplyText));
    KD_CHECK_EQ(m.last_seq(), uint64_t{1});
}

KD_TEST(session, stopped_and_error_for_a_stale_session_are_bookkeeping_only) {
    kd::ManualClock clock(1000);
    kd::SessionMachine m(clock, kd::SessionConfig{});
    bring_to_idle(m);
    const std::string sid = start_session(m);

    proto::ErrorMsg em;
    em.code = proto::ErrorCode::decode_failed;
    em.code_raw = "decode_failed";
    em.fatal = false;
    em.msg = "backend raised";
    em.session_id = std::string("s0");
    auto out = m.on_message(proto::Message::of(em));
    KD_CHECK_EQ(out.verdict, Verdict::AcceptBookkeepingOnly);
    KD_CHECK(!out.has_effect(Effect::ApplyText));
    KD_CHECK_EQ(m.state(), proto::ConnState::Running);  // our session is untouched
    KD_CHECK_EQ(m.current_session_id(), sid);

    out = m.on_message(stopped_msg("s0"));
    KD_CHECK_EQ(out.verdict, Verdict::AcceptBookkeepingOnly);
    KD_CHECK_EQ(m.state(), proto::ConnState::Running);
    KD_CHECK_EQ(m.current_session_id(), sid);
}

// The child's diagnosis has to survive into `Outcome::error`, because on this path it is the only
// diagnosis there is.
//
// A sidecar that catches its *own* would-be §8.2 violation refuses to send the offending frame and
// reports `decode_failed` instead — §11.3 has no server-sent code for "I caught myself". That
// makes this branch, not the §8.2 check further down, the one a real abort actually comes through.
// It used to set no `error` at all, so the entire incident reached the log as the fixed string
// `committed text was contradicted` and nothing else: which rule, which offset and which session
// were all discarded at this line. Two dictations were lost to it and the log could not tell it
// apart from a refusal.
KD_TEST(session, a_non_fatal_error_for_the_live_session_carries_its_diagnosis_out) {
    kd::ManualClock clock(1000);
    kd::SessionMachine m(clock, kd::SessionConfig{});
    bring_to_idle(m);
    const std::string sid = start_session(m);

    proto::ErrorMsg em;
    em.code = proto::ErrorCode::decode_failed;
    em.code_raw = "decode_failed";
    em.fatal = false;
    em.msg = "server-side stability violation (stable_prefix_mutated): utterance_id=1 "
             "previous_stable_bytes=33 diverge_offset=0";
    em.session_id = sid;

    const auto out = m.on_message(proto::Message::of(em));
    KD_CHECK_EQ(out.verdict, Verdict::Accept);
    KD_CHECK(out.has_effect(Effect::SurfaceWarning));

    // The point of the test. KD_REQUIRE, not KD_CHECK: everything below dereferences the
    // optional, and a non-fatal check would let a regression here reach `out.error->code` on an
    // empty optional — which under a Debug MSVC runtime is a modal assertion dialog, i.e. a test
    // run that hangs forever instead of failing. Found the honest way, by removing the fix.
    KD_REQUIRE(out.error.has_value());
    KD_CHECK_EQ(out.error->code, proto::ErrorCode::decode_failed);
    KD_CHECK(!out.error->fatal);          // §11.1: the session dies, the connection lives
    KD_CHECK_EQ(out.error->msg, em.msg);  // verbatim; main.cpp logs it
    KD_REQUIRE(out.error->session_id.has_value());
    KD_CHECK_EQ(*out.error->session_id, sid);

    // Non-fatal means the connection survives, so none of the fatal teardown fires.
    KD_CHECK(!out.has_effect(Effect::KillChild));
    KD_CHECK(!out.has_effect(Effect::SurfaceFatal));
}

KD_TEST(session, duplicate_and_out_of_order_seq_change_nothing) {
    kd::ManualClock clock(1000);
    kd::SessionMachine m(clock, kd::SessionConfig{});
    kd::SessionSnapshot snap;
    snap.state = proto::ConnState::Running;
    snap.current_session_id = "s7";
    snap.last_seq = 11;
    snap.utterance_id = 3;
    m.restore(snap);

    auto out = m.on_message(partial_msg("s7", 3, 12, "can you change", 8));
    KD_CHECK_EQ(out.verdict, Verdict::Accept);

    out = m.on_message(partial_msg("s7", 3, 12, "can you change the", 8));
    KD_CHECK_EQ(out.verdict, Verdict::DropDuplicateSeq);
    KD_CHECK(!out.has_effect(Effect::ApplyText));

    out = m.on_message(partial_msg("s7", 3, 11, "can", 0));
    KD_CHECK_EQ(out.verdict, Verdict::DropOutOfOrder);
    KD_CHECK(!out.has_effect(Effect::ApplyText));
    KD_CHECK_EQ(m.last_seq(), uint64_t{12});

    out = m.on_message(partial_msg("s7", 3, 13, "can you change the llama", 15));
    KD_CHECK_EQ(out.verdict, Verdict::Accept);
    KD_CHECK_EQ(m.last_seq(), uint64_t{13});
}

KD_TEST(session, seq_gap_is_accepted_and_resets_anchor_confidence) {
    kd::ManualClock clock(1000);
    kd::SessionMachine m(clock, kd::SessionConfig{});
    kd::SessionSnapshot snap;
    snap.state = proto::ConnState::Running;
    snap.current_session_id = "s7";
    snap.last_seq = 12;
    snap.utterance_id = 3;
    m.restore(snap);

    const auto out = m.on_message(partial_msg("s7", 3, 15, "can you change the llama dot cpp", 24));
    KD_CHECK_EQ(out.verdict, Verdict::AcceptAfterGap);
    KD_CHECK_EQ(out.gap_expected_seq, uint64_t{13});
    KD_CHECK_EQ(out.gap_received_seq, uint64_t{15});
    KD_CHECK(out.has_effect(Effect::LogGap));
    KD_CHECK(out.has_effect(Effect::ResetAnchorConfidence));
    KD_CHECK(out.has_effect(Effect::ApplyText));  // the newest hypothesis is still applied
}

KD_TEST(session, stable_bytes_decrease_aborts_the_session_without_clamping) {
    kd::ManualClock clock(1000);
    kd::SessionMachine m(clock, kd::SessionConfig{});
    kd::SessionSnapshot snap;
    snap.state = proto::ConnState::Running;
    snap.current_session_id = "s7";
    snap.last_seq = 11;
    snap.utterance_id = 3;
    m.restore(snap);

    KD_REQUIRE_EQ(m.on_message(partial_msg("s7", 3, 12, "can you change the", 8)).verdict,
                  Verdict::Accept);

    const auto out = m.on_message(partial_msg("s7", 3, 13, "can you change the llama", 4));
    KD_CHECK_EQ(out.verdict, Verdict::ProtocolError);
    KD_REQUIRE(out.error.has_value());
    KD_CHECK_EQ(out.error->code, proto::ErrorCode::stability_violation);
    KD_CHECK(!out.error->fatal);  // the session dies, the connection does not (§11.1)
    KD_REQUIRE(out.error->session_id.has_value());
    KD_CHECK_EQ(*out.error->session_id, std::string("s7"));
    KD_CHECK(sends(out, proto::MessageType::Error));
    KD_CHECK(!out.has_effect(Effect::ApplyText));
    KD_CHECK_EQ(m.state(), proto::ConnState::Finalizing);

    // "Stop applying anything for that session" (§8.3 step 2) — including the `final` that would
    // otherwise arrive and look perfectly well formed.
    const auto after = m.on_message(final_msg("s7", 3, 14, "can you change the llama"));
    KD_CHECK_EQ(after.verdict, Verdict::DropCancelledSession);
    KD_CHECK(!after.has_effect(Effect::ApplyText));
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  plan.md §1.7 — target identity and the refusals
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(session, focus_change_before_injection_ends_the_session_with_nothing_injected) {
    kd::ManualClock clock(1000);
    FakeTarget targets;
    kd::SessionMachine m(clock, kd::SessionConfig{});
    m.set_target_provider(&targets);
    bring_to_idle(m);
    const std::string sid = start_session(m);
    KD_CHECK_EQ(m.target().top_level, targets.foreground.top_level);

    // A hypothesis arrives and is applicable...
    const auto hyp = m.on_message(partial_msg(sid, 1, 1, "can you change the", 8));
    KD_REQUIRE_EQ(hyp.verdict, Verdict::Accept);
    KD_REQUIRE(hyp.has_effect(Effect::ApplyText));

    // ...and between the hypothesis and the injection, the user alt-tabs. Tabbing to another
    // control inside the same window counts too, which is why `focus` is part of the identity.
    targets.foreground.top_level = 0x2000;
    targets.foreground.focus = 0x2001;

    const auto gate = m.before_inject();
    KD_CHECK(!gate.allowed);
    KD_CHECK_EQ(gate.status, kd::InjectStatus::TargetChanged);
    KD_CHECK(sends(gate.outcome, proto::MessageType::Cancel));
    KD_CHECK(gate.outcome.has_effect(Effect::DropQueuedAudio));
    KD_CHECK_EQ(m.state(), proto::ConnState::Cancelling);
    KD_CHECK_EQ(m.refusal(), kd::InjectStatus::TargetChanged);
    KD_CHECK_EQ(targets.validations, 1);

    // Nothing further is applied for this session — the `final` in flight is discarded rather
    // than typed into the window the user has since focused.
    const auto late = m.on_message(final_msg(sid, 1, 2, "can you change the llama.cpp config"));
    KD_CHECK_EQ(late.verdict, Verdict::DropCancelledSession);
    KD_CHECK(!late.has_effect(Effect::ApplyText));

    // And the gate stays shut.
    const auto gate2 = m.before_inject();
    KD_CHECK(!gate2.allowed);
}

KD_TEST(session, unchanged_target_passes_the_gate_on_every_injection) {
    kd::ManualClock clock(1000);
    FakeTarget targets;
    kd::SessionMachine m(clock, kd::SessionConfig{});
    m.set_target_provider(&targets);
    bring_to_idle(m);
    start_session(m);

    for (int i = 0; i < 3; ++i) {
        const auto gate = m.before_inject();
        KD_REQUIRE(gate.allowed);
        KD_CHECK_EQ(gate.status, kd::InjectStatus::Ok);
        KD_CHECK(gate.outcome.to_send.empty());
    }
    // Validated per injection, not once per session.
    KD_CHECK_EQ(targets.validations, 3);
    KD_CHECK_EQ(m.state(), proto::ConnState::Running);
}

KD_TEST(session, secure_desktop_appearing_mid_session_shuts_the_gate) {
    kd::ManualClock clock(1000);
    FakeTarget targets;
    kd::SessionMachine m(clock, kd::SessionConfig{});
    m.set_target_provider(&targets);
    bring_to_idle(m);
    start_session(m);

    targets.secure = true;  // a UAC prompt or the lock screen appeared
    const auto gate = m.before_inject();
    KD_CHECK(!gate.allowed);
    KD_CHECK_EQ(gate.status, kd::InjectStatus::SecureDesktop);
    KD_CHECK(sends(gate.outcome, proto::MessageType::Cancel));
    KD_CHECK_EQ(m.state(), proto::ConnState::Cancelling);
}

KD_TEST(session, elevated_target_is_refused_at_session_start) {
    kd::ManualClock clock(1000);
    FakeTarget targets;
    targets.self = kd::IntegrityLevel::Medium;
    targets.foreground.integrity = kd::IntegrityLevel::High;  // an elevated console, say

    kd::SessionMachine m(clock, kd::SessionConfig{});
    m.set_target_provider(&targets);
    bring_to_idle(m);

    // The refusal happens at the press. Not after a sentence has been spoken, not inferred from
    // an injection that "seemed not to work" — SendInput into a higher-integrity window is
    // silently ignored and its return value does not identify UIPI as the cause.
    const auto out = m.on_local(LocalEvent::FinalisePressed);
    KD_CHECK_EQ(out.verdict, Verdict::Refused);
    KD_CHECK(out.to_send.empty());  // no `start` frame: the mic never opens
    KD_CHECK(out.has_effect(Effect::SurfaceWarning));
    KD_CHECK_EQ(m.state(), proto::ConnState::Idle);
    KD_CHECK(m.current_session_id().empty());
    KD_CHECK_EQ(m.refusal(), kd::InjectStatus::ElevatedUnsupported);
    KD_CHECK_EQ(targets.captures, 1);

    // Lowering the target below us makes it work, which is the proof that the integrity
    // comparison — and not something incidental — is what refused.
    targets.foreground.integrity = kd::IntegrityLevel::Medium;
    const auto ok = m.on_local(LocalEvent::FinalisePressed);
    KD_CHECK_EQ(ok.verdict, Verdict::Sent);
    KD_CHECK_EQ(m.refusal(), kd::InjectStatus::Ok);
}

KD_TEST(session, unknown_integrity_does_not_refuse) {
    // The honest limit: if we cannot read either integrity level we cannot prove elevation, and
    // refusing on an unprovable guess would block ordinary targets for no reason.
    kd::ManualClock clock(1000);
    FakeTarget targets;
    targets.self = kd::IntegrityLevel::Medium;
    targets.foreground.integrity = kd::IntegrityLevel::Unknown;

    kd::SessionMachine m(clock, kd::SessionConfig{});
    m.set_target_provider(&targets);
    bring_to_idle(m);
    KD_CHECK_EQ(m.on_local(LocalEvent::FinalisePressed).verdict, Verdict::Sent);
}

KD_TEST(session, password_field_and_secure_desktop_are_refused_at_session_start) {
    kd::ManualClock clock(1000);
    FakeTarget targets;
    targets.foreground.is_password_field = true;

    kd::SessionMachine m(clock, kd::SessionConfig{});
    m.set_target_provider(&targets);
    bring_to_idle(m);

    auto out = m.on_local(LocalEvent::FinalisePressed);
    KD_CHECK_EQ(out.verdict, Verdict::Refused);
    KD_CHECK_EQ(m.refusal(), kd::InjectStatus::PasswordField);
    KD_CHECK(out.to_send.empty());

    targets.foreground.is_password_field = false;
    targets.secure = true;
    out = m.on_local(LocalEvent::FinalisePressed);
    KD_CHECK_EQ(out.verdict, Verdict::Refused);
    KD_CHECK_EQ(m.refusal(), kd::InjectStatus::SecureDesktop);
    KD_CHECK(out.to_send.empty());
    KD_CHECK_EQ(m.state(), proto::ConnState::Idle);

    // An unidentifiable foreground window is refused too: injecting into a window we cannot name
    // is exactly the failure the identity rules exist to prevent.
    targets.secure = false;
    targets.capture_fails = true;
    out = m.on_local(LocalEvent::FinalisePressed);
    KD_CHECK_EQ(out.verdict, Verdict::Refused);
    KD_CHECK_EQ(m.refusal(), kd::InjectStatus::Failed);
}

KD_TEST(session, target_changed_event_cancels_rather_than_finalising) {
    // The event path (a foreground-change notification) must reach the same conclusion as the
    // per-injection gate: `cancel`, not `stop`. A `stop` invites a `final` we would then be asked
    // to type into a window that no longer has focus.
    kd::ManualClock clock(1000);
    kd::SessionMachine m(clock, kd::SessionConfig{});
    bring_to_idle(m);
    const std::string sid = start_session(m);

    const auto out = m.on_local(LocalEvent::TargetChanged);
    KD_CHECK_EQ(out.verdict, Verdict::Sent);
    KD_CHECK(sends(out, proto::MessageType::Cancel));
    KD_CHECK(!sends(out, proto::MessageType::Stop));
    KD_CHECK(out.has_effect(Effect::DropQueuedAudio));
    KD_CHECK_EQ(m.state(), proto::ConnState::Cancelling);

    KD_CHECK_EQ(m.on_message(final_msg(sid, 1, 1, "anything at all")).verdict,
                Verdict::DropCancelledSession);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  plan.md §1.8 — activation must not immediately self-stop
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(session, stop_on_input_during_the_activation_grace_is_ignored) {
    kd::ManualClock clock(1000);
    kd::SessionConfig cfg;
    cfg.activation_grace_ms = 250;
    kd::SessionMachine m(clock, cfg);
    bring_to_idle(m);
    start_session(m);

    // The chord's own key-ups and the injected VK_CONTROL tap that suppresses the Start menu
    // arrive after the session starts. Whatever slips past hotkey.h's InputFilter dies here.
    clock.advance(10);
    auto out = m.on_local(LocalEvent::StopOnInput);
    KD_CHECK(out.to_send.empty());
    KD_CHECK_EQ(m.state(), proto::ConnState::Running);

    clock.advance(200);  // 210 ms in, still inside the window
    out = m.on_local(LocalEvent::StopOnInput);
    KD_CHECK(out.to_send.empty());
    KD_CHECK_EQ(m.state(), proto::ConnState::Running);

    // Past the window, a real keystroke ends the session — the grace period is a guard, not a
    // suppression of stop-on-input.
    clock.advance(100);
    out = m.on_local(LocalEvent::StopOnInput);
    KD_CHECK_EQ(out.verdict, Verdict::Sent);
    KD_CHECK(sends(out, proto::MessageType::Stop));
    KD_CHECK_EQ(m.state(), proto::ConnState::Finalizing);
}

KD_TEST(session, chord_keyups_and_injected_input_never_reach_the_session) {
    // The composition that matters: InputFilter (hotkey.h) is the primary rule, and the session
    // only ever sees what it passes. Both halves are asserted here, because "the session ignores
    // our own injected input" is a property of the pair, and plan.md §7 gates on it
    // ("Hooks survive long sessions and ignore self-injected input").
    constexpr uint32_t kVkLWin = 0x5B;
    constexpr uint32_t kVkY = 0x59;
    constexpr uint32_t kVkControl = 0x11;
    constexpr uintptr_t kMark = 0xD1C7A7E;

    kd::ManualClock clock(1000);
    kd::SessionMachine m(clock, kd::SessionConfig{});
    bring_to_idle(m);

    kd::InputFilter filter;
    filter.set_hotkey(kd::HotkeySpec{});  // Win+Y, the default
    filter.set_injection_mark(kMark);

    kd::InputEvent e;
    e.time_ms = 1000;
    e.kind = kd::InputEvent::Kind::KeyDown;
    e.vk = kVkLWin;
    (void)filter.on_event(e);
    e.vk = kVkY;
    const kd::FilterOutcome activation = filter.on_event(e);
    KD_REQUIRE(activation.activate);

    start_session(m);
    filter.note_session_started(1000);

    // Everything that arrives after activation and is ours, at times well past any grace window
    // so that only the filter can be what saves the session.
    std::vector<kd::InputEvent> ours;
    kd::InputEvent up_y;
    up_y.kind = kd::InputEvent::Kind::KeyUp;
    up_y.vk = kVkY;
    up_y.time_ms = 5000;
    ours.push_back(up_y);

    kd::InputEvent up_win = up_y;
    up_win.vk = kVkLWin;
    ours.push_back(up_win);

    kd::InputEvent tap_down;  // the Start-menu suppression tap we inject ourselves
    tap_down.kind = kd::InputEvent::Kind::KeyDown;
    tap_down.vk = kVkControl;
    tap_down.injected = true;
    tap_down.extra_info = kMark;
    tap_down.time_ms = 5001;
    ours.push_back(tap_down);

    kd::InputEvent tap_up = tap_down;
    tap_up.kind = kd::InputEvent::Kind::KeyUp;
    ours.push_back(tap_up);

    kd::InputEvent typed;  // a character the injector is typing into the target
    typed.kind = kd::InputEvent::Kind::KeyDown;
    typed.vk = 0x41;  // 'A'
    typed.injected = true;
    typed.extra_info = kMark;
    typed.time_ms = 6000;
    ours.push_back(typed);

    clock.advance(5000);  // far outside the activation grace window
    for (const kd::InputEvent& ev : ours) {
        const kd::FilterOutcome fo = filter.on_event(ev);
        KD_CHECK(!fo.stop_on_input);
        if (fo.stop_on_input) m.on_local(LocalEvent::StopOnInput);
    }
    KD_CHECK_EQ(m.state(), proto::ConnState::Running);

    // A genuine keystroke from the user is not ours and does stop the session.
    kd::InputEvent real;
    real.kind = kd::InputEvent::Kind::KeyDown;
    real.vk = 0x42;  // 'B'
    real.time_ms = 7000;
    const kd::FilterOutcome fo = filter.on_event(real);
    KD_CHECK(fo.stop_on_input);
    if (fo.stop_on_input) {
        const auto out = m.on_local(LocalEvent::StopOnInput);
        KD_CHECK(sends(out, proto::MessageType::Stop));
    }
    KD_CHECK_EQ(m.state(), proto::ConnState::Finalizing);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  plan.md §1.8 — second press finalises, Esc cancels; the tail's fate is never ambiguous
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(session, second_press_finalises_and_esc_cancels_produce_different_text) {
    kd::ManualClock clock(1000);

    // Two machines fed exactly the same server traffic, differing only in how the user ended the
    // session. What lands in the document is the difference.
    std::string finalised;
    std::string cancelled;

    {
        kd::SessionMachine m(clock, kd::SessionConfig{});
        bring_to_idle(m);
        const std::string sid = start_session(m);
        auto out = m.on_message(partial_msg(sid, 1, 1, "can you change the", 8));
        if (out.has_effect(Effect::ApplyText)) finalised = "can you ";

        out = m.on_local(LocalEvent::FinalisePressed);  // second press
        KD_CHECK_EQ(out.verdict, Verdict::Sent);
        KD_CHECK(sends(out, proto::MessageType::Stop));
        KD_CHECK_EQ(m.state(), proto::ConnState::Finalizing);

        // FINALIZING applies the last final — that is the entire point of `stop`. Note that the
        // final's first 8 bytes are identical to the published stable prefix: a `final` that
        // recapitalised them would be a §8.2 violation, not a finalisation.
        out = m.on_message(final_msg(sid, 1, 2, "can you change the llama.cpp config"));
        KD_CHECK_EQ(out.verdict, Verdict::Accept);
        KD_CHECK(out.has_effect(Effect::ApplyText));
        if (out.has_effect(Effect::ApplyText)) finalised = "can you change the llama.cpp config";

        KD_CHECK_EQ(m.on_message(stopped_msg(sid)).verdict, Verdict::Accept);
        KD_CHECK_EQ(m.state(), proto::ConnState::Idle);
        KD_CHECK(m.current_session_id().empty());
    }

    {
        kd::SessionMachine m(clock, kd::SessionConfig{});
        bring_to_idle(m);
        const std::string sid = start_session(m);
        auto out = m.on_message(partial_msg(sid, 1, 1, "can you change the", 8));
        if (out.has_effect(Effect::ApplyText)) cancelled = "can you ";

        out = m.on_local(LocalEvent::EscPressed);
        KD_CHECK_EQ(out.verdict, Verdict::Sent);
        KD_CHECK(sends(out, proto::MessageType::Cancel));
        KD_CHECK(out.has_effect(Effect::DropQueuedAudio));
        KD_CHECK_EQ(m.state(), proto::ConnState::Cancelling);

        // §10.4: the `final` was written before the `cancel` was read. It is discarded regardless
        // of content, and `stopped` is still expected.
        out = m.on_message(final_msg(sid, 1, 2, "can you change the llama.cpp config"));
        KD_CHECK_EQ(out.verdict, Verdict::DropCancelledSession);
        KD_CHECK(!out.has_effect(Effect::ApplyText));
        if (out.has_effect(Effect::ApplyText)) cancelled = "can you change the llama.cpp config";

        out = m.on_message(partial_msg(sid, 2, 3, "and also", 0));
        KD_CHECK_EQ(out.verdict, Verdict::DropCancelledSession);

        KD_CHECK_EQ(m.on_message(stopped_msg(sid)).verdict, Verdict::Accept);
        KD_CHECK_EQ(m.state(), proto::ConnState::Idle);
    }

    // The committed prefix survives both; only the tail's fate differs, and it differs
    // deterministically.
    KD_CHECK_EQ(cancelled, std::string("can you "));
    KD_CHECK_EQ(finalised, std::string("can you change the llama.cpp config"));
    KD_CHECK_NE(finalised, cancelled);
}

KD_TEST(session, esc_during_finalizing_still_cancels) {
    kd::ManualClock clock(1000);
    kd::SessionMachine m(clock, kd::SessionConfig{});
    bring_to_idle(m);
    const std::string sid = start_session(m);

    m.on_local(LocalEvent::FinalisePressed);
    KD_REQUIRE_EQ(m.state(), proto::ConnState::Finalizing);

    const auto out = m.on_local(LocalEvent::EscPressed);
    KD_CHECK(sends(out, proto::MessageType::Cancel));
    KD_CHECK_EQ(m.state(), proto::ConnState::Cancelling);
    KD_CHECK_EQ(m.on_message(final_msg(sid, 1, 1, "discarded")).verdict,
                Verdict::DropCancelledSession);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  docs/protocol.md §10.3 — sidecar crash
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(session, sidecar_crash_ends_the_session_and_never_reuses_a_session_id) {
    kd::ManualClock clock(1000);
    kd::SessionMachine m(clock, kd::SessionConfig{});
    bring_to_idle(m);
    const std::string dead_sid = start_session(m);
    KD_REQUIRE_EQ(m.on_message(partial_msg(dead_sid, 1, 1, "half a sentence", 5)).verdict,
                  Verdict::Accept);

    // The child dies mid-session.
    const auto crash = m.on_transport(TransportEvent::BrokenPipe);
    KD_CHECK_EQ(crash.verdict, Verdict::AbortSession);
    KD_CHECK(!crash.has_effect(Effect::ApplyText));  // nothing further is injected, ever
    KD_CHECK(crash.has_effect(Effect::ResetCommitEngine));
    KD_CHECK(crash.has_effect(Effect::SurfaceFatal));  // never a silent restart mid-session
    KD_CHECK(crash.has_effect(Effect::RespawnChild));
    KD_CHECK(crash.to_send.empty());  // there is no peer left to tell
    KD_CHECK_EQ(m.state(), proto::ConnState::Dead);
    KD_CHECK(m.current_session_id().empty());
    KD_REQUIRE(crash.error.has_value());
    KD_CHECK(crash.error->fatal);

    // A ghost from the dead session arriving before the handle is closed is dropped.
    KD_CHECK_EQ(m.on_message(final_msg(dead_sid, 1, 2, "ghost")).verdict, Verdict::DropStaleSession);

    // §10.3 step 5: reap, respawn on a new random pipe name, new session_id space.
    const auto back = m.on_transport(TransportEvent::ChildRespawned);
    KD_CHECK_EQ(back.verdict, Verdict::Reconnected);
    KD_CHECK(sends(back, proto::MessageType::Hello));
    KD_CHECK_EQ(m.state(), proto::ConnState::Handshake);

    proto::HelloAckMsg ack;
    ack.proto = proto::kProtoVersion;
    KD_REQUIRE_EQ(m.on_message(proto::Message::of(ack)).verdict, Verdict::Accept);
    proto::ReadyMsg ready;
    ready.model = "example/streaming-en";
    ready.mode = proto::StreamingMode::NativeStreaming;
    KD_REQUIRE_EQ(m.on_message(proto::Message::of(ready)).verdict, Verdict::Accept);
    KD_REQUIRE_EQ(m.state(), proto::ConnState::Idle);

    const std::string new_sid = start_session(m);
    KD_CHECK_NE(new_sid, dead_sid);  // the counter never resets — fixture 020

    // The pre-crash session's ghost is still dropped on the new connection.
    KD_CHECK_EQ(m.on_message(partial_msg(dead_sid, 1, 3, "ghost of the dead session", 5)).verdict,
                Verdict::DropStaleSession);
    KD_CHECK_EQ(m.on_message(partial_msg(new_sid, 1, 1, "new session", 0)).verdict,
                Verdict::Accept);
}

KD_TEST(session, fatal_error_from_the_server_kills_the_connection) {
    kd::ManualClock clock(1000);
    kd::SessionMachine m(clock, kd::SessionConfig{});
    bring_to_idle(m);
    const std::string sid = start_session(m);

    proto::ErrorMsg em;
    em.code = proto::ErrorCode::model_load_failed;
    em.code_raw = "model_load_failed";
    em.fatal = true;
    em.msg = "CUDA out of memory";
    em.session_id = sid;
    const auto out = m.on_message(proto::Message::of(em));
    KD_CHECK(out.has_effect(Effect::SurfaceFatal));
    KD_CHECK(out.has_effect(Effect::KillChild));
    KD_CHECK_EQ(m.state(), proto::ConnState::Dead);
    KD_CHECK(m.current_session_id().empty());
}

KD_TEST(session, audio_queue_overflow_is_loud_and_fatal) {
    // §9: silently dropping or delaying audio produces mysteriously bad transcription. The only
    // legal responses are to keep up or to fail loudly.
    kd::ManualClock clock(1000);
    kd::SessionMachine m(clock, kd::SessionConfig{});
    bring_to_idle(m);
    start_session(m);

    const auto out = m.on_local(LocalEvent::AudioQueueFull);
    KD_CHECK_EQ(out.verdict, Verdict::ProtocolError);
    KD_REQUIRE(out.error.has_value());
    KD_CHECK_EQ(out.error->code, proto::ErrorCode::overload_audio);
    KD_CHECK(out.error->fatal);
    KD_CHECK(sends(out, proto::MessageType::Error));  // told to the peer, then torn down
    KD_CHECK_EQ(m.state(), proto::ConnState::Dead);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Handshake, deadlines, and the rest of §10.2
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(session, hotkey_before_ready_does_not_produce_a_start_frame) {
    // §10.2: `start` may only be sent from IDLE. A press during LOADING shows the loading state.
    kd::ManualClock clock(1000);
    kd::SessionMachine m(clock, kd::SessionConfig{});
    m.on_transport(TransportEvent::Connected);
    m.on_transport(TransportEvent::PeerValidated);
    proto::HelloAckMsg ack;
    ack.proto = proto::kProtoVersion;
    m.on_message(proto::Message::of(ack));
    KD_REQUIRE_EQ(m.state(), proto::ConnState::Loading);

    const auto out = m.on_local(LocalEvent::FinalisePressed);
    KD_CHECK_EQ(out.verdict, Verdict::Refused);
    KD_CHECK(out.to_send.empty());
    KD_CHECK_EQ(m.state(), proto::ConnState::Loading);
}

KD_TEST(session, hello_ack_with_the_wrong_proto_is_fatal) {
    kd::ManualClock clock(1000);
    kd::SessionMachine m(clock, kd::SessionConfig{});
    m.on_transport(TransportEvent::Connected);
    m.on_transport(TransportEvent::PeerValidated);

    proto::HelloAckMsg ack;
    ack.proto = proto::kProtoVersion + 1;  // version selection is the Client's (§5)
    const auto out = m.on_message(proto::Message::of(ack));
    KD_CHECK_EQ(out.verdict, Verdict::ProtocolError);
    KD_REQUIRE(out.error.has_value());
    KD_CHECK_EQ(out.error->code, proto::ErrorCode::proto_unsupported);
    KD_CHECK_EQ(m.state(), proto::ConnState::Dead);
}

KD_TEST(session, handshake_and_model_load_deadlines_fire) {
    {
        kd::ManualClock clock(1000);
        kd::SessionMachine m(clock, kd::SessionConfig{});
        m.on_transport(TransportEvent::Connected);
        m.on_transport(TransportEvent::PeerValidated);
        clock.advance(proto::kHelloTimeoutMs - 1);
        KD_CHECK_EQ(m.state(), proto::ConnState::Handshake);
        m.on_tick();
        KD_CHECK_EQ(m.state(), proto::ConnState::Handshake);
        clock.advance(1);
        const auto out = m.on_tick();
        KD_REQUIRE(out.error.has_value());
        KD_CHECK_EQ(out.error->code, proto::ErrorCode::handshake_timeout);
        KD_CHECK_EQ(m.state(), proto::ConnState::Dead);
    }
    {
        // READY_TIMEOUT_MS restarts on each `loading` — a cold model load can take minutes.
        kd::ManualClock clock(1000);
        kd::SessionMachine m(clock, kd::SessionConfig{});
        m.on_transport(TransportEvent::Connected);
        m.on_transport(TransportEvent::PeerValidated);
        proto::HelloAckMsg ack;
        ack.proto = proto::kProtoVersion;
        m.on_message(proto::Message::of(ack));

        clock.advance(proto::kReadyTimeoutMs - 1000);
        proto::LoadingMsg lm;
        lm.model = "example/streaming-en";
        lm.stage = std::string("weights");
        KD_REQUIRE_EQ(m.on_message(proto::Message::of(lm)).verdict, Verdict::Accept);

        clock.advance(proto::kReadyTimeoutMs - 1);
        m.on_tick();
        KD_CHECK_EQ(m.state(), proto::ConnState::Loading);
        clock.advance(1);
        const auto out = m.on_tick();
        KD_REQUIRE(out.error.has_value());
        KD_CHECK_EQ(out.error->code, proto::ErrorCode::model_load_timeout);
        KD_CHECK_EQ(m.state(), proto::ConnState::Dead);
    }
}

KD_TEST(session, capture_glitch_is_reported_rather_than_papered_over) {
    kd::ManualClock clock(1000);
    kd::SessionMachine m(clock, kd::SessionConfig{});
    bring_to_idle(m);
    const std::string sid = start_session(m);

    const auto out = m.on_capture_glitch(35, "AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY");
    KD_CHECK_EQ(out.verdict, Verdict::Sent);
    KD_REQUIRE_EQ(out.to_send.size(), size_t{1});
    KD_REQUIRE_EQ(out.to_send[0].type(), proto::MessageType::Discontinuity);
    const auto* d = out.to_send[0].get_if<proto::DiscontinuityMsg>();
    KD_REQUIRE(d != nullptr);
    KD_CHECK_EQ(d->session_id, sid);
    KD_CHECK_EQ(d->gap_ms, int64_t{35});
    KD_CHECK_EQ(m.state(), proto::ConnState::Running);  // not fatal on either side

    // Reported after the session ended: no session to attribute it to, and not an error.
    m.on_local(LocalEvent::EscPressed);
    m.on_message(stopped_msg(sid));
    const auto late = m.on_capture_glitch(35, "");
    KD_CHECK(late.to_send.empty());
}

KD_TEST(session, id_generator_never_reuses_and_verdict_spellings_are_stable) {
    kd::SessionIdGenerator ids;
    KD_CHECK_EQ(ids.next(), std::string("s1"));
    KD_CHECK_EQ(ids.next(), std::string("s2"));
    KD_CHECK_EQ(ids.peek(), uint64_t{3});
    KD_CHECK(proto::is_valid_session_id("s1"));

    // docs/protocol.md §12.5 — the Python fixture runner compares these as strings.
    KD_CHECK_EQ(kd::to_string(Verdict::Accept), "accept");
    KD_CHECK_EQ(kd::to_string(Verdict::AcceptAfterGap), "accept_after_gap");
    KD_CHECK_EQ(kd::to_string(Verdict::AcceptBookkeepingOnly), "accept_bookkeeping_only");
    KD_CHECK_EQ(kd::to_string(Verdict::DropStaleSession), "drop_stale_session");
    KD_CHECK_EQ(kd::to_string(Verdict::DropCancelledSession), "drop_cancelled_session");
    KD_CHECK_EQ(kd::to_string(Verdict::DropDuplicateSeq), "drop_duplicate_seq");
    KD_CHECK_EQ(kd::to_string(Verdict::DropOutOfOrder), "drop_out_of_order");
    KD_CHECK_EQ(kd::to_string(Verdict::ProtocolError), "protocol_error");
    KD_CHECK_EQ(kd::to_string(Verdict::Sent), "sent");
    KD_CHECK_EQ(kd::to_string(Verdict::AbortSession), "abort_session");
    KD_CHECK_EQ(kd::to_string(Verdict::Reconnected), "reconnected");

    // The fixture files spell transport events this way in `from: "transport"` entries.
    KD_CHECK_EQ(kd::to_string(TransportEvent::BrokenPipe), "broken_pipe");
    KD_CHECK_EQ(kd::to_string(TransportEvent::ChildRespawned), "child_respawned");

    KD_CHECK_EQ(kd::to_string(Effect::LogGap), "log_gap");
    KD_CHECK_EQ(kd::to_string(Effect::ResetAnchorConfidence), "reset_anchor_confidence");
}
