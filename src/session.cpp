// The session / utterance state machine.
//
// Specification: session.h. Implements plan.md §1.6 (the two silence timers), §1.7 (target
// identity and the refusals), §1.8 (activation must not immediately self-stop, second press
// finalises, Esc cancels), and docs/protocol.md §7 (identity and ordering) and §10 (lifecycle).
//
// No <windows.h> here, and none may be added: everything Windows-shaped this module needs comes
// through ITargetProvider, which is what makes the wrong-window and refusal rules — the ones
// plan.md §7 gates on — reachable from a unit test with no window, no hook and no message loop.
//
// ─── The four rules that everything else in this file serves ────────────────────────────────
//
//  1. **Staleness before everything.** A `partial`/`final` whose `session_id` is not the current
//     one is dropped before its `seq` is allowed to touch any high-water mark. Order matters:
//     checking `seq` first would let a message from a dead session advance state for a live one.
//  2. **The two timers are independent.** `endpoint_silence_ms` ends an utterance;
//     `session_silence_ms` ends the session. They are two thresholds read off the same
//     "last speech" timestamp, in that order, and neither one resets the other. A one-minute
//     thinking pause therefore costs an utterance boundary and nothing else.
//  3. **Nothing is injected into a target we cannot still name.** `before_inject()` is the only
//     gate, it runs per injection, and on any change it ends the session with `cancel` rather
//     than continuing.
//  4. **A refusal is a decision, not a discovery.** Elevated / password / secure-desktop targets
//     are refused at the press, because SendInput into a higher-integrity window fails silently
//     and its return value does not identify UIPI as the cause (plan.md §1.7).
//
// ─── What is deliberately NOT here ──────────────────────────────────────────────────────────
//
//  * No text is assembled, committed, or diffed — commit_engine.h owns that. This machine only
//    decides that a message is *applicable* and says so with Effect::ApplyText.
//  * No mutex. See the threading note in session.h: taking a lock this machine might hold, from
//    a low-level hook callback, is how a hook gets silently unhooked by Windows.
//  * No timeout on `stopped`. docs/protocol.md defines no deadline for it, so inventing one here
//    would be a constant nobody could trace to the spec. Recorded in the return report instead;
//    the consequence is that a sidecar that reads `stop` and then wedges leaves the machine in
//    FINALIZING until the transport notices the peer is gone.

#include "session.h"

#include <algorithm>
#include <string>
#include <utility>

namespace kd {
namespace {

// One place that knows how to turn a detected fault into the wire message §11 describes.
proto::Message make_error_message(const proto::ProtocolError& e) {
    proto::ErrorMsg em;
    em.code = e.code;
    em.code_raw = proto::to_string(e.code);
    em.fatal = e.fatal;
    em.msg = e.msg;
    em.session_id = e.session_id;
    em.supported = e.supported;
    return proto::Message::of(std::move(em));
}

// UIPI comparison (plan.md §1.7). Unknown on either side is not evidence of anything, so it does
// not refuse: an unprovable guess would either block legitimate targets or promise a protection
// we cannot deliver. IntegrityLevel's enumerators are ordered low-to-high with Unknown last,
// which is why Unknown must be excluded before the numeric comparison rather than after it.
bool target_outranks_us(IntegrityLevel target, IntegrityLevel self) noexcept {
    if (target == IntegrityLevel::Unknown || self == IntegrityLevel::Unknown) return false;
    return static_cast<uint8_t>(target) > static_cast<uint8_t>(self);
}

}  // namespace

// ─── Spellings ──────────────────────────────────────────────────────────────────────────────
//
// These four functions are a wire-adjacent contract: the Python fixture runner compares the
// verdict strings against docs/protocol.md §12.5, and the transport-event strings against the
// `event` fields in the fixture files. Renaming one is a change to the shared test data, not a
// local tidy-up.

const char* to_string(Verdict v) noexcept {
    switch (v) {
        case Verdict::Accept: return "accept";
        case Verdict::AcceptAfterGap: return "accept_after_gap";
        case Verdict::AcceptBookkeepingOnly: return "accept_bookkeeping_only";
        case Verdict::DropStaleSession: return "drop_stale_session";
        case Verdict::DropCancelledSession: return "drop_cancelled_session";
        case Verdict::DropDuplicateSeq: return "drop_duplicate_seq";
        case Verdict::DropOutOfOrder: return "drop_out_of_order";
        case Verdict::ProtocolError: return "protocol_error";
        case Verdict::Sent: return "sent";
        case Verdict::AbortSession: return "abort_session";
        case Verdict::Reconnected: return "reconnected";
        case Verdict::Refused: return "refused";
    }
    return "unknown";
}

const char* to_string(Effect e) noexcept {
    switch (e) {
        case Effect::LogGap: return "log_gap";
        case Effect::ResetAnchorConfidence: return "reset_anchor_confidence";
        case Effect::ResetCommitEngine: return "reset_commit_engine";
        case Effect::ApplyText: return "apply_text";
        case Effect::FlushPendingAudio: return "flush_pending_audio";
        case Effect::DropQueuedAudio: return "drop_queued_audio";
        case Effect::SurfaceWarning: return "surface_warning";
        case Effect::SurfaceFatal: return "surface_fatal";
        case Effect::KillChild: return "kill_child";
        case Effect::RespawnChild: return "respawn_child";
        case Effect::StartEndpointTimer: return "start_endpoint_timer";
        case Effect::StartSessionTimer: return "start_session_timer";
        case Effect::StopTimers: return "stop_timers";
    }
    return "unknown";
}

const char* to_string(LocalEvent e) noexcept {
    switch (e) {
        case LocalEvent::HotkeyPressed: return "hotkey_pressed";
        case LocalEvent::EscPressed: return "esc_pressed";
        case LocalEvent::StopOnInput: return "stop_on_input";
        case LocalEvent::EndpointSilence: return "endpoint_silence";
        case LocalEvent::SessionSilence: return "session_silence";
        case LocalEvent::CaptureGlitch: return "capture_glitch";
        case LocalEvent::AudioQueueFull: return "audio_queue_full";
        case LocalEvent::ControlQueueFull: return "control_queue_full";
        case LocalEvent::TargetChanged: return "target_changed";
        case LocalEvent::StabilityViolation: return "stability_violation";
        case LocalEvent::SpeechDetected: return "speech_detected";
    }
    return "unknown";
}

const char* to_string(TransportEvent e) noexcept {
    switch (e) {
        case TransportEvent::Connected: return "connected";
        case TransportEvent::PeerValidated: return "peer_validated";
        case TransportEvent::PeerMismatch: return "peer_mismatch";
        case TransportEvent::BrokenPipe: return "broken_pipe";
        case TransportEvent::EofAtFrameBoundary: return "eof_at_frame_boundary";
        case TransportEvent::EofMidFrame: return "eof_mid_frame";
        case TransportEvent::ConnectTimeout: return "connect_timeout";
        case TransportEvent::HandshakeTimeout: return "handshake_timeout";
        case TransportEvent::ModelLoadTimeout: return "model_load_timeout";
        case TransportEvent::ChildRespawned: return "child_respawned";
    }
    return "unknown";
}

// ─── SessionIdGenerator ─────────────────────────────────────────────────────────────────────

std::string SessionIdGenerator::next() {
    // "s<N>" with a counter that never resets — docs §7.1. The counter is deliberately not
    // reseeded on child restart: reusing an id after a restart defeats the staleness check
    // exactly when it matters most (fixture 020).
    std::string s = "s";
    s += std::to_string(next_++);
    return s;
}

// ─── Outcome ────────────────────────────────────────────────────────────────────────────────

bool SessionMachine::Outcome::has_effect(Effect e) const noexcept {
    return std::find(effects.begin(), effects.end(), e) != effects.end();
}

// ─── Impl ───────────────────────────────────────────────────────────────────────────────────

struct SessionMachine::Impl {
    Impl(const IClock& c, SessionConfig config) : clock(c), cfg(std::move(config)) {}

    const IClock& clock;
    SessionConfig cfg;
    SessionIdGenerator ids;
    ITargetProvider* provider = nullptr;

    // Connection state. A machine starts in SPAWNING: the pipe is created and the child launched
    // before anything can be exchanged (docs §2.4 step 1).
    proto::ConnState state = proto::ConnState::Spawning;
    Millis state_entered_ms = 0;
    Millis loading_seen_ms = 0;  // READY_TIMEOUT_MS restarts on each `loading` (docs §6.2)

    // Session state.
    std::string session_id;        // empty means no session is open, so nothing is current
    uint64_t last_seq = 0;
    uint64_t utterance_id = 0;
    uint64_t last_stable_bytes = 0;
    std::string prev_text;         // the text that established last_stable_bytes
    bool have_prev_text = false;
    bool discarding = false;       // `cancel` written, or the session aborted: drop all its text

    // Timers. Both are thresholds on last_voice_ms — see rule 2 at the top of the file.
    Millis activation_ms = 0;
    Millis last_voice_ms = 0;
    bool utterance_open = false;
    bool utterance_counted = false;
    uint64_t utterances_completed = 0;

    // Target identity and the refusal decided at the press.
    TargetIdentity target;
    InjectStatus refusal = InjectStatus::Ok;

    [[nodiscard]] Millis now() const noexcept { return clock.now_ms(); }
    [[nodiscard]] Millis since(Millis t) const noexcept {
        const Millis n = now();
        return n >= t ? n - t : 0;  // monotonic clock, so this cannot happen; costs one compare
    }

    void enter(proto::ConnState s) {
        state = s;
        state_entered_ms = now();
        if (s == proto::ConnState::Loading) loading_seen_ms = state_entered_ms;
    }

    // Clear everything scoped to one session. The session_id generator's counter is untouched.
    void clear_session() {
        session_id.clear();
        last_seq = 0;
        utterance_id = 0;
        last_stable_bytes = 0;
        prev_text.clear();
        have_prev_text = false;
        discarding = false;
        utterance_open = false;
        utterance_counted = false;
        utterances_completed = 0;
        target = TargetIdentity{};
    }

    // An utterance ended, by endpoint timeout or by `final`. Counted once per utterance: the two
    // paths overlap constantly (the VAD endpoint fires ~0.8 s before the server's `final`
    // arrives) and double-counting would put a leading space in front of nothing.
    void close_utterance() {
        utterance_open = false;
        if (!utterance_counted) {
            utterance_counted = true;
            ++utterances_completed;
        }
    }

    // Fatal teardown: the connection is finished (docs §11.1). Recovery is a new child on a new
    // pipe, never a reuse of this one (§10.3 step 1). The open session is abandoned **without
    // injecting anything further** — committed text stays in the document, and the unstable tail
    // is lost, which §10.3 step 2 names as the honest outcome.
    void die(Outcome& out, proto::ErrorCode code, std::string msg, bool tell_peer) {
        proto::ProtocolError err;
        err.code = code;
        err.fatal = true;
        err.msg = std::move(msg);
        if (!session_id.empty()) err.session_id = session_id;
        if (tell_peer) out.to_send.push_back(make_error_message(err));
        out.error = std::move(err);

        out.effects.push_back(Effect::StopTimers);
        out.effects.push_back(Effect::ResetCommitEngine);
        out.effects.push_back(Effect::ResetAnchorConfidence);
        out.effects.push_back(Effect::SurfaceFatal);
        out.effects.push_back(Effect::KillChild);
        out.effects.push_back(Effect::RespawnChild);

        clear_session();
        enter(proto::ConnState::Dead);
    }

    // End the session by discarding the uncommitted tail: `cancel`, then wait for `stopped`.
    // Used by Esc and by every target-safety refusal — once we can no longer name the window,
    // finalising would mean typing the tail into whatever the user has since focused.
    void cancel_session(Outcome& out) {
        proto::CancelMsg c;
        c.session_id = session_id;
        out.to_send.push_back(proto::Message::of(std::move(c)));
        out.effects.push_back(Effect::DropQueuedAudio);
        discarding = true;
        enter(proto::ConnState::Cancelling);
    }

    // End the session by finalising: flush the audio we owe (docs §6.3's Client obligation —
    // no audio for a session may follow its terminating control frame), then `stop`. The
    // still-in-flight `final` is applied, which is the entire point of `stop`.
    void finalise_session(Outcome& out) {
        out.effects.push_back(Effect::FlushPendingAudio);
        proto::StopMsg s;
        s.session_id = session_id;
        out.to_send.push_back(proto::Message::of(std::move(s)));
        enter(proto::ConnState::Finalizing);
    }

    [[nodiscard]] bool session_open() const noexcept {
        return !session_id.empty() && (state == proto::ConnState::Running ||
                                       state == proto::ConnState::Finalizing ||
                                       state == proto::ConnState::Cancelling);
    }
};

// ─── Construction ───────────────────────────────────────────────────────────────────────────

SessionMachine::SessionMachine(const IClock& clock, SessionConfig cfg)
    : impl_(new Impl(clock, std::move(cfg))) {
    impl_->state_entered_ms = impl_->clock.now_ms();
}

SessionMachine::~SessionMachine() { delete impl_; }

void SessionMachine::set_target_provider(ITargetProvider* provider) noexcept {
    impl_->provider = provider;
}

void SessionMachine::set_config(const SessionConfig& cfg) { impl_->cfg = cfg; }

const SessionConfig& SessionMachine::config() const noexcept { return impl_->cfg; }

// ─── Inbound messages ───────────────────────────────────────────────────────────────────────

SessionMachine::Outcome SessionMachine::on_message(const proto::Message& m) {
    Outcome out;
    Impl& s = *impl_;
    const proto::MessageType t = m.type();

    // A message that arrives after the connection is finished cannot be applied to anything: the
    // handle is gone and the session it names no longer exists. `stopped`/`error` are still worth
    // recording, for the same reason §7.1 gives — otherwise the orderly end of a session we have
    // moved on from is unobservable.
    if (s.state == proto::ConnState::Dead) {
        out.verdict = (t == proto::MessageType::Stopped || t == proto::MessageType::Error)
                          ? Verdict::AcceptBookkeepingOnly
                          : Verdict::DropStaleSession;
        return out;
    }

    // Direction. `error` is the only bidirectional message; anything else in the Client->Server
    // set arriving *from* the Server means the peer is confused about which role it holds, and
    // §11.3 calls that state_violation.
    if (!proto::is_server_message(t)) {
        s.die(out, proto::ErrorCode::state_violation,
              std::string("server sent a client-only message: ") + proto::to_string(t), true);
        out.verdict = Verdict::ProtocolError;
        return out;
    }

    // ── §7.1 staleness, before anything else touches state ──────────────────────────────────
    if (const std::string* sid = m.session_id()) {
        const bool current = !s.session_id.empty() && *sid == s.session_id;
        if (!current) {
            if (m.carries_text()) {
                // The rule this module exists for. Not clamped, not queued, not injected.
                out.verdict = Verdict::DropStaleSession;
                return out;
            }
            // `stopped` / `error` for a session we have moved on from: recorded, applied to
            // nothing. The one exception in §7.1 — and a *fatal* error is still fatal, because
            // `fatal` is scoped to the connection (§11.1), not to the session that carried it.
            out.verdict = Verdict::AcceptBookkeepingOnly;
            if (const auto* em = m.get_if<proto::ErrorMsg>(); em != nullptr && em->fatal) {
                s.die(out, em->code, em->msg, false);
            }
            return out;
        }
    }

    switch (t) {
        case proto::MessageType::HelloAck: {
            if (s.state != proto::ConnState::Handshake) {
                s.die(out, proto::ErrorCode::state_violation,
                      "hello_ack outside HANDSHAKE", true);
                out.verdict = Verdict::ProtocolError;
                return out;
            }
            const auto* ack = m.get_if<proto::HelloAckMsg>();
            if (ack == nullptr || ack->proto != proto::kProtoVersion) {
                // §5: version selection is the Client's. A Server answering with a different
                // number has changed the contract unilaterally, so this is fatal here even though
                // the frame itself was well formed.
                s.die(out, proto::ErrorCode::proto_unsupported,
                      "hello_ack carried proto " +
                          std::to_string(ack != nullptr ? ack->proto : 0) + ", offered " +
                          std::to_string(proto::kProtoVersion),
                      false);
                out.verdict = Verdict::ProtocolError;
                return out;
            }
            s.enter(proto::ConnState::Loading);
            out.verdict = Verdict::Accept;
            return out;
        }

        case proto::MessageType::Loading: {
            if (s.state != proto::ConnState::Loading) {
                s.die(out, proto::ErrorCode::state_violation, "loading outside LOADING", true);
                out.verdict = Verdict::ProtocolError;
                return out;
            }
            s.loading_seen_ms = s.now();  // READY_TIMEOUT_MS restarts on each one (docs §6.2)
            out.verdict = Verdict::Accept;
            return out;
        }

        case proto::MessageType::Ready: {
            // "ready MUST be sent exactly once per connection" (§6.2). A second one means the
            // peer thinks it can change model in place, which plan.md §1.1 explicitly forbids —
            // a model change is a new child.
            if (s.state != proto::ConnState::Loading) {
                s.die(out, proto::ErrorCode::state_violation, "ready outside LOADING", true);
                out.verdict = Verdict::ProtocolError;
                return out;
            }
            s.enter(proto::ConnState::Idle);
            out.verdict = Verdict::Accept;
            return out;
        }

        case proto::MessageType::Stopped: {
            // Terminal for the session, whatever ended it. Everything session-scoped resets,
            // including the id — after this, nothing is current and any straggler is stale.
            s.clear_session();
            s.enter(proto::ConnState::Idle);
            out.effects.push_back(Effect::ResetCommitEngine);
            out.effects.push_back(Effect::StopTimers);
            out.verdict = Verdict::Accept;
            return out;
        }

        case proto::MessageType::Error: {
            const auto* em = m.get_if<proto::ErrorMsg>();
            if (em == nullptr) {
                s.die(out, proto::ErrorCode::internal_error, "error message with no body", false);
                out.verdict = Verdict::ProtocolError;
                return out;
            }
            if (em->fatal) {
                // §13: act on the flag, not on the code — an unrecognised code is still a
                // well-formed error, and refusing it would lose the diagnostic being delivered.
                s.die(out, em->code, em->msg, false);
                out.verdict = Verdict::Accept;
                return out;
            }
            // Non-fatal: the named session is aborted, the connection survives (§11.1). Text
            // already injected stays; nothing further is applied for this session.
            s.discarding = true;
            s.enter(proto::ConnState::Finalizing);
            out.effects.push_back(Effect::SurfaceWarning);
            out.verdict = Verdict::Accept;
            return out;
        }

        case proto::MessageType::Partial:
        case proto::MessageType::Final:
            break;  // below — the long one

        default:
            s.die(out, proto::ErrorCode::state_violation,
                  std::string("unexpected message: ") + proto::to_string(t), true);
            out.verdict = Verdict::ProtocolError;
            return out;
    }

    // ── partial / final for the current session ─────────────────────────────────────────────

    if (s.state != proto::ConnState::Running && s.state != proto::ConnState::Finalizing &&
        s.state != proto::ConnState::Cancelling) {
        s.die(out, proto::ErrorCode::state_violation, "text message with no session open", true);
        out.verdict = Verdict::ProtocolError;
        return out;
    }

    // §10.4: after *writing* `cancel` — or after aborting the session on a violation — every
    // subsequent partial/final for it is discarded regardless of content, until `stopped`. The
    // race is unavoidable: the `final` was written before the `cancel` was read.
    if (s.discarding) {
        out.verdict = Verdict::DropCancelledSession;
        return out;
    }

    const auto* p = m.get_if<proto::PartialMsg>();
    const auto* f = m.get_if<proto::FinalMsg>();
    const uint64_t seq = p != nullptr ? p->seq : f->seq;
    const uint64_t utt = p != nullptr ? p->utterance_id : f->utterance_id;
    const std::string& text = p != nullptr ? p->text : f->text;
    // A `final` is entirely stable by definition (§6.2); its optional stable_bytes is redundant
    // and MUST equal the text length, so the length is the honest value to carry forward.
    const uint64_t stable = p != nullptr ? p->stable_bytes : static_cast<uint64_t>(f->text.size());

    // ── §7.4 ordering, before §8 stability ──────────────────────────────────────────────────
    //
    // A duplicate or out-of-order message changes nothing at all — in particular it may not move
    // the stability high-water mark, which is why this runs first.
    const proto::SeqVerdict sv = proto::classify_seq(s.last_seq, seq);
    if (sv == proto::SeqVerdict::DropDuplicate) {
        out.verdict = Verdict::DropDuplicateSeq;
        return out;
    }
    if (sv == proto::SeqVerdict::DropOutOfOrder) {
        out.verdict = Verdict::DropOutOfOrder;
        return out;
    }

    // ── Utterance boundary (§7.3, §8.2) ─────────────────────────────────────────────────────
    const bool new_utterance = (utt != s.utterance_id);
    uint64_t prev_stable_for_check = s.last_stable_bytes;
    bool have_prev_for_check = s.have_prev_text;
    if (new_utterance) {
        // The high-water mark resets to 0 at every utterance change, and the commit engine's
        // stability window resets with it (plan.md §1.4 step 7).
        prev_stable_for_check = 0;
        have_prev_for_check = false;
        // **No ResetCommitEngine here, deliberately.** That effect is session-scoped — the owner
        // maps it onto `CommitEngine::reset()`, which calls `clear_session()` — and firing it at
        // an utterance boundary destroyed two things that must survive one:
        //
        //   * `session_has_committed`, which is the *only* input to `CommitOp::leading_space`. So
        //     the very event that is supposed to produce the space between utterances was the
        //     event that made it impossible: "…the default modelopen src/protocol.h…". Observed
        //     in a real session after the first fix for this had already been made and tested —
        //     the test drove `begin_utterance()`, which no owner calls.
        //   * a session-scoped violation latch, which §8.3 does not un-latch at an utterance.
        //
        // The utterance-scoped reset this comment is about is not needed from here at all:
        // `CommitEngine::on_partial`/`on_final` compare the incoming `utterance_id` against their
        // own and call `clear_utterance()` themselves, with the id, which this effect cannot
        // carry.
    }

    // ── §8.2 stability ──────────────────────────────────────────────────────────────────────
    size_t diverge = 0;
    proto::StabilityCheck sc;
    if (have_prev_for_check) {
        sc = proto::check_stability(s.prev_text, prev_stable_for_check, text, stable, &diverge);
    } else {
        // No previous text to compare against — either a fresh utterance, or a snapshot restored
        // by a fixture, which carries `last_stable_bytes` but not the text that established it.
        // The range and boundary halves are still checkable, and so is monotonicity; only the
        // "bytes below the mark are unchanged" half is not, and claiming to have checked it would
        // be worse than saying so here.
        sc = proto::check_stability(std::string_view{}, 0, text, stable, &diverge);
        if (sc == proto::StabilityCheck::Ok && stable < prev_stable_for_check) {
            sc = proto::StabilityCheck::Decreased;
        }
    }

    if (sc != proto::StabilityCheck::Ok) {
        // §8.3: not "clamp and continue". Clamping would paper over a backend bug while the
        // user's document silently diverges, and it would hide the events ICER exists to count.
        std::string detail;
        uint64_t at_offset = 0;
        switch (sc) {
            case proto::StabilityCheck::Decreased:
                detail = "stable_bytes decreased " + std::to_string(prev_stable_for_check) +
                         " -> " + std::to_string(stable) + " in utterance " + std::to_string(utt);
                break;
            case proto::StabilityCheck::PrefixMutated:
                detail = "stable prefix mutated at byte " + std::to_string(diverge);
                at_offset = static_cast<uint64_t>(diverge);
                break;
            case proto::StabilityCheck::OutOfRange:
                detail = "stable_bytes " + std::to_string(stable) + " exceeds text length " +
                         std::to_string(text.size());
                break;
            case proto::StabilityCheck::NotGraphemeBoundary:
                detail = "stable_bytes " + std::to_string(stable) +
                         " is not on a grapheme-cluster boundary";
                break;
            case proto::StabilityCheck::Ok:
                break;  // unreachable; enumerated so a new value is a compiler warning
        }

        proto::ProtocolError err;
        err.code = proto::ErrorCode::stability_violation;
        err.fatal = false;  // the child is not at fault as a *process* (§8.3 step 2)
        err.msg = detail;
        err.session_id = s.session_id;
        err.at_offset = at_offset;
        out.to_send.push_back(make_error_message(err));
        out.error = std::move(err);
        out.effects.push_back(Effect::SurfaceWarning);

        // Abort the session, keep the connection. `discarding` is what makes "stop applying
        // anything for that session" true for every later message, including the `final`.
        s.discarding = true;
        s.enter(proto::ConnState::Finalizing);
        out.verdict = Verdict::ProtocolError;
        return out;
    }

    // ── Accepted ────────────────────────────────────────────────────────────────────────────
    if (new_utterance) {
        s.utterance_id = utt;
        s.utterance_counted = false;
    }
    if (sv == proto::SeqVerdict::AcceptAfterGap) {
        // Not fatal: the message in hand is the newest hypothesis and discarding it would be
        // worse. The consequence is the anchor reset — commit differencing infers the caret from
        // a change produced by text we believe we injected, and a missed hypothesis makes that
        // belief unsound (plan.md §1.5).
        out.verdict = Verdict::AcceptAfterGap;
        out.gap_expected_seq = s.last_seq + 1;
        out.gap_received_seq = seq;
        out.effects.push_back(Effect::LogGap);
        out.effects.push_back(Effect::ResetAnchorConfidence);
    } else {
        out.verdict = Verdict::Accept;
    }

    s.last_seq = seq;
    s.last_stable_bytes = stable;
    s.prev_text = text;
    s.have_prev_text = true;
    if (f != nullptr) s.close_utterance();

    out.effects.push_back(Effect::ApplyText);
    return out;
}

// ─── Local events ───────────────────────────────────────────────────────────────────────────

SessionMachine::Outcome SessionMachine::on_local(LocalEvent e) {
    Outcome out;
    Impl& s = *impl_;

    switch (e) {
        case LocalEvent::HotkeyPressed: {
            if (s.state == proto::ConnState::Running) {
                // The second press: finalise. Commit the stable prefix and discard the unstable
                // tail *after* a final decode — which is `stop`, not `cancel` (plan.md §1.8).
                s.finalise_session(out);
                out.verdict = Verdict::Sent;
                return out;
            }
            if (s.state == proto::ConnState::Finalizing || s.state == proto::ConnState::Cancelling) {
                // Already ending. Idempotent by design: teardown also runs on crash paths.
                out.verdict = Verdict::Accept;
                return out;
            }
            if (s.state != proto::ConnState::Idle) {
                // §10.2: `start` may only be sent from IDLE. A press while the model is loading
                // shows the loading state; it does not produce a frame.
                s.refusal = InjectStatus::Failed;
                out.effects.push_back(Effect::SurfaceWarning);
                out.verdict = Verdict::Refused;
                return out;
            }

            // ── The refusals, decided here rather than discovered later (plan.md §1.7) ──────
            TargetIdentity captured;
            if (s.provider != nullptr) {
                InjectStatus refusal = InjectStatus::Ok;
                if (s.provider->secure_desktop_active()) {
                    // UAC prompt or lock screen. Nothing we send reaches it, and the mic should
                    // not be open across it either.
                    refusal = InjectStatus::SecureDesktop;
                } else if (!s.provider->capture_target(captured) || !captured.is_valid()) {
                    refusal = InjectStatus::Failed;
                } else if (s.cfg.refuse_password_fields && captured.is_password_field) {
                    refusal = InjectStatus::PasswordField;
                } else if (s.cfg.refuse_elevated_targets &&
                           target_outranks_us(captured.integrity, s.provider->self_integrity())) {
                    // UIPI. SendInput into a higher-integrity window is silently ignored and the
                    // return value does not identify UIPI as the cause, so this is the last
                    // moment it can be reported honestly.
                    refusal = InjectStatus::ElevatedUnsupported;
                }
                if (refusal != InjectStatus::Ok) {
                    s.refusal = refusal;
                    out.effects.push_back(Effect::SurfaceWarning);
                    out.verdict = Verdict::Refused;
                    return out;  // no session, no `start`, no mic
                }
            }

            // clear_session() resets the identity too, so the captured target is installed after
            // it, not before — the ordering matters and has been wrong here once already.
            s.clear_session();
            s.refusal = InjectStatus::Ok;
            s.target = captured;  // default-constructed (is_valid() == false) with no provider
            s.session_id = s.ids.next();
            s.activation_ms = s.now();
            s.last_voice_ms = s.activation_ms;

            proto::StartMsg start;
            start.session_id = s.session_id;
            start.sample_rate = static_cast<int32_t>(proto::kSampleRate);
            start.channels = static_cast<int32_t>(proto::kChannels);
            start.format = "s16le";
            if (s.cfg.send_context) {
                start.context = s.cfg.context;
                start.context_present = true;  // distinct from "did not send the field" (§6.1)
            }
            out.to_send.push_back(proto::Message::of(std::move(start)));

            out.effects.push_back(Effect::ResetCommitEngine);
            out.effects.push_back(Effect::StartEndpointTimer);
            out.effects.push_back(Effect::StartSessionTimer);
            s.enter(proto::ConnState::Running);
            out.verdict = Verdict::Sent;
            return out;
        }

        case LocalEvent::EscPressed: {
            // Cancel: discard everything uncommitted. Legal from FINALIZING too (§10.2) — the
            // user may hit Esc after the second press and before `stopped`.
            if (s.state != proto::ConnState::Running && s.state != proto::ConnState::Finalizing) {
                out.verdict = Verdict::Accept;
                return out;
            }
            s.cancel_session(out);
            out.verdict = Verdict::Sent;
            return out;
        }

        case LocalEvent::StopOnInput: {
            if (s.state != proto::ConnState::Running) {
                out.verdict = Verdict::Accept;
                return out;
            }
            // plan.md §1.8. The chord's own key-ups and the injected VK_CONTROL tap that
            // suppresses the Start menu arrive *after* the session starts. hotkey.h's InputFilter
            // is the primary guard (it ignores key-ups for keys down at activation and everything
            // flagged LLKHF_INJECTED / carrying our InjectionMark); this window is the second
            // one, for events that reach the machine by another path. It is not a substitute:
            // a grace window alone would still self-stop on a chord released slowly.
            if (s.since(s.activation_ms) < static_cast<Millis>(s.cfg.activation_grace_ms)) {
                out.verdict = Verdict::Accept;
                return out;
            }
            s.finalise_session(out);
            out.verdict = Verdict::Sent;
            return out;
        }

        case LocalEvent::EndpointSilence: {
            // Ends an UTTERANCE. The session continues — this is the timer plan.md §1.6 warns
            // against merging with the session timer.
            if (s.state != proto::ConnState::Running) {
                out.verdict = Verdict::Accept;
                return out;
            }
            s.close_utterance();
            // Deliberately no ResetCommitEngine here. The local VAD endpoint is a *hint* that the
            // utterance is over; the authoritative boundary is the server's `final` and the next
            // utterance_id, and resetting the engine on the hint would discard the stability
            // state that the still-in-flight `final` is about to be checked against.
            out.verdict = Verdict::Accept;
            return out;
        }

        case LocalEvent::SessionSilence: {
            if (s.state != proto::ConnState::Running) {
                out.verdict = Verdict::Accept;
                return out;
            }
            s.close_utterance();
            s.finalise_session(out);
            out.verdict = Verdict::Sent;
            return out;
        }

        case LocalEvent::SpeechDetected: {
            if (s.state != proto::ConnState::Running) {
                out.verdict = Verdict::Accept;
                return out;
            }
            s.last_voice_ms = s.now();  // re-arms BOTH timers, which is the only thing that does
            if (!s.utterance_open) {
                s.utterance_open = true;
                out.effects.push_back(Effect::StartEndpointTimer);
            }
            out.verdict = Verdict::Accept;
            return out;
        }

        case LocalEvent::CaptureGlitch:
            return on_capture_glitch(0, {});

        case LocalEvent::AudioQueueFull: {
            // §9: never a silent drop, never "every other frame". An explicit overload that ends
            // the connection is diagnosable; mysteriously bad transcription is not.
            s.die(out, proto::ErrorCode::overload_audio,
                  "outbound audio queue exceeded AUDIO_QUEUE_MAX_MS", true);
            out.verdict = Verdict::ProtocolError;
            return out;
        }

        case LocalEvent::ControlQueueFull: {
            s.die(out, proto::ErrorCode::overload_control,
                  "inbound control queue exceeded CONTROL_QUEUE_MAX", true);
            out.verdict = Verdict::ProtocolError;
            return out;
        }

        case LocalEvent::TargetChanged: {
            if (!s.session_open()) {
                out.verdict = Verdict::Accept;
                return out;
            }
            // plan.md §1.7: "Any change ends the session rather than continuing into a new
            // target." Cancel rather than stop: a `stop` would invite a `final` we would then be
            // asked to inject, and the window it was meant for no longer has focus.
            s.refusal = InjectStatus::TargetChanged;
            s.cancel_session(out);
            out.effects.push_back(Effect::SurfaceWarning);
            out.verdict = Verdict::Sent;
            return out;
        }

        case LocalEvent::StabilityViolation: {
            // Raised by the commit engine when a hypothesis contradicts text already in the
            // user's document (CommittedTextContradicted). Same handling as the §8.2 checks
            // above: preserve the document, abort the session, keep the connection.
            if (!s.session_open()) {
                out.verdict = Verdict::Accept;
                return out;
            }
            proto::ProtocolError err;
            err.code = proto::ErrorCode::stability_violation;
            err.fatal = false;
            err.msg = "committed text contradicted by a later hypothesis";
            err.session_id = s.session_id;
            out.to_send.push_back(make_error_message(err));
            out.error = std::move(err);
            out.effects.push_back(Effect::SurfaceWarning);
            s.discarding = true;
            s.enter(proto::ConnState::Finalizing);
            out.verdict = Verdict::Sent;
            return out;
        }
    }

    out.verdict = Verdict::Accept;
    return out;
}

// ─── Transport events ───────────────────────────────────────────────────────────────────────

SessionMachine::Outcome SessionMachine::on_transport(TransportEvent e) {
    Outcome out;
    Impl& s = *impl_;

    switch (e) {
        case TransportEvent::Connected: {
            if (s.state != proto::ConnState::Spawning) {
                // A connection on a pipe we are not waiting on. nMaxInstances is 1, so this
                // should be impossible; treat it as fatal rather than as something to absorb.
                s.die(out, proto::ErrorCode::internal_error,
                      "connection completed outside SPAWNING", false);
                out.verdict = Verdict::ProtocolError;
                return out;
            }
            s.enter(proto::ConnState::Validating);
            out.verdict = Verdict::Accept;
            return out;
        }

        case TransportEvent::PeerValidated: {
            if (s.state != proto::ConnState::Validating) {
                s.die(out, proto::ErrorCode::internal_error,
                      "peer validated outside VALIDATING", false);
                out.verdict = Verdict::ProtocolError;
                return out;
            }
            s.enter(proto::ConnState::Handshake);
            proto::HelloMsg hello;
            hello.proto = proto::kProtoVersion;
            hello.client = "kvit-dictate/0.1";
            out.to_send.push_back(proto::Message::of(std::move(hello)));
            out.verdict = Verdict::Accept;
            return out;
        }

        case TransportEvent::PeerMismatch: {
            // docs §2.4 step 4: disconnect immediately, kill the child, report locally. Never
            // sent — the peer on the other end is not the one we spawned.
            s.die(out, proto::ErrorCode::peer_mismatch,
                  "GetNamedPipeClientProcessId did not match the spawned child", false);
            out.verdict = Verdict::AbortSession;
            return out;
        }

        case TransportEvent::BrokenPipe: {
            // §10.3. There is no wire code for "the peer is gone" because there is no peer to
            // tell; internal_error is the closest honest label for the log and the tray, and the
            // message says what actually happened.
            s.die(out, proto::ErrorCode::internal_error, "sidecar stopped (broken pipe)", false);
            out.verdict = Verdict::AbortSession;
            return out;
        }

        case TransportEvent::EofMidFrame: {
            s.die(out, proto::ErrorCode::truncated_frame, "end of stream inside a frame", false);
            out.verdict = Verdict::AbortSession;
            return out;
        }

        case TransportEvent::EofAtFrameBoundary: {
            // Orderly shutdown is not an error (fixture 010) — but if a session was open, the
            // sidecar went away in the middle of it and that is worth surfacing.
            const bool had_session = s.session_open();
            s.die(out, proto::ErrorCode::internal_error, "sidecar closed the connection", false);
            if (!had_session) {
                out.effects.erase(std::remove(out.effects.begin(), out.effects.end(),
                                              Effect::SurfaceFatal),
                                  out.effects.end());
            }
            out.verdict = Verdict::AbortSession;
            return out;
        }

        case TransportEvent::ConnectTimeout: {
            s.die(out, proto::ErrorCode::spawn_timeout,
                  "no client connected within CONNECT_TIMEOUT_MS", false);
            out.verdict = Verdict::AbortSession;
            return out;
        }

        case TransportEvent::HandshakeTimeout: {
            s.die(out, proto::ErrorCode::handshake_timeout,
                  "no hello_ack within HELLO_TIMEOUT_MS", false);
            out.verdict = Verdict::AbortSession;
            return out;
        }

        case TransportEvent::ModelLoadTimeout: {
            s.die(out, proto::ErrorCode::model_load_timeout,
                  "no ready within READY_TIMEOUT_MS", false);
            out.verdict = Verdict::AbortSession;
            return out;
        }

        case TransportEvent::ChildRespawned: {
            // §10.3 step 5 has completed: new child, new random pipe name, new session_id space.
            // The id counter is NOT reset — that is the whole point of fixture 020.
            s.clear_session();
            s.refusal = InjectStatus::Ok;
            s.enter(proto::ConnState::Handshake);
            proto::HelloMsg hello;
            hello.proto = proto::kProtoVersion;
            hello.client = "kvit-dictate/0.1";
            out.to_send.push_back(proto::Message::of(std::move(hello)));
            out.verdict = Verdict::Reconnected;
            return out;
        }
    }

    out.verdict = Verdict::Accept;
    return out;
}

// ─── Time ───────────────────────────────────────────────────────────────────────────────────

SessionMachine::Outcome SessionMachine::on_tick() {
    Outcome out;
    Impl& s = *impl_;

    switch (s.state) {
        case proto::ConnState::Spawning:
            if (s.since(s.state_entered_ms) >= static_cast<Millis>(proto::kConnectTimeoutMs)) {
                s.die(out, proto::ErrorCode::spawn_timeout,
                      "no client connected within CONNECT_TIMEOUT_MS", false);
                out.verdict = Verdict::AbortSession;
            }
            return out;

        case proto::ConnState::Handshake:
            if (s.since(s.state_entered_ms) >= static_cast<Millis>(proto::kHelloTimeoutMs)) {
                s.die(out, proto::ErrorCode::handshake_timeout,
                      "no hello_ack within HELLO_TIMEOUT_MS", false);
                out.verdict = Verdict::AbortSession;
            }
            return out;

        case proto::ConnState::Loading:
            if (s.since(s.loading_seen_ms) >= static_cast<Millis>(proto::kReadyTimeoutMs)) {
                s.die(out, proto::ErrorCode::model_load_timeout,
                      "no ready within READY_TIMEOUT_MS", false);
                out.verdict = Verdict::AbortSession;
            }
            return out;

        case proto::ConnState::Running:
            break;

        default:
            // VALIDATING has no deadline of its own (GetNamedPipeClientProcessId is a local
            // call); FINALIZING and CANCELLING have no `stopped` deadline in the spec — see the
            // note at the top of this file. IDLE and DEAD have nothing to time out.
            return out;
    }

    // ── The two timers, in the order that keeps them independent ────────────────────────────
    //
    // Both are thresholds on the same "last speech" timestamp, and neither resets the other.
    // Endpoint first: at 0.8 s it closes the utterance and the session carries on, so a one
    // minute thinking pause costs exactly one utterance boundary. Session silence is measured
    // from the last *speech*, not from the last utterance boundary, because that is what
    // plan.md §1.6 defines it as: "how long the session survives with no speech at all".
    const Millis quiet = s.since(s.last_voice_ms);

    if (s.utterance_open && quiet >= static_cast<Millis>(s.cfg.endpoint_silence_ms)) {
        s.close_utterance();
        out.verdict = Verdict::Accept;
        // No message: an utterance boundary is a Server-side notion (§7.3), signalled to us by
        // `final`. We only stop feeding the decoder, which the audio path does off this event.
    }

    // `never` is a legitimate setting (plan.md §1.6), safe because stop-on-input already covers
    // essentially every real "I'm done" moment.
    if (!s.cfg.session_silence_never && s.cfg.session_silence_ms > 0 &&
        quiet >= static_cast<Millis>(s.cfg.session_silence_ms)) {
        s.close_utterance();
        s.finalise_session(out);
        out.verdict = Verdict::Sent;
    }

    return out;
}

SessionMachine::Outcome SessionMachine::on_capture_glitch(uint32_t gap_ms,
                                                          std::string_view reason) {
    Outcome out;
    Impl& s = *impl_;
    if (s.state != proto::ConnState::Running || s.session_id.empty()) {
        // Nothing to attribute the gap to. Not an error: a glitch can be reported by the capture
        // thread just after the session ended.
        out.verdict = Verdict::Accept;
        return out;
    }
    // plan.md §1.6 / docs §6.1: padding with zeroes or splicing the audio both hand the decoder a
    // lie and produce bad transcription indistinguishable from a bad model. Say what happened.
    proto::DiscontinuityMsg d;
    d.session_id = s.session_id;
    d.gap_ms = static_cast<int64_t>(gap_ms);
    if (!reason.empty()) d.reason = std::string(reason);
    out.to_send.push_back(proto::Message::of(std::move(d)));
    out.verdict = Verdict::Sent;
    return out;
}

// ─── The per-injection gate ─────────────────────────────────────────────────────────────────

SessionMachine::InjectGate SessionMachine::before_inject() {
    InjectGate gate;
    Impl& s = *impl_;

    // FINALIZING still injects: applying the last `final` is the entire point of `stop`.
    // CANCELLING never does, and neither does anything else.
    if (s.state != proto::ConnState::Running && s.state != proto::ConnState::Finalizing) {
        gate.allowed = false;
        gate.status = InjectStatus::Failed;
        gate.outcome.verdict = Verdict::Refused;
        return gate;
    }

    if (s.provider == nullptr) {
        // No gate available. Documented in session.h: correct for the fixture runner, wrong for
        // production, which is why main.cpp installs one.
        gate.allowed = true;
        gate.status = InjectStatus::Ok;
        gate.outcome.verdict = Verdict::Accept;
        return gate;
    }

    if (s.provider->secure_desktop_active()) {
        gate.status = InjectStatus::SecureDesktop;
    } else if (!s.provider->target_still_current(s.target)) {
        gate.status = InjectStatus::TargetChanged;
    } else {
        gate.allowed = true;
        gate.status = InjectStatus::Ok;
        gate.outcome.verdict = Verdict::Accept;
        return gate;
    }

    // Refused. End the session by discarding the tail — see LocalEvent::TargetChanged for why
    // this is `cancel` and not `stop`. Nothing has been injected on this path, and nothing will
    // be: the caller must honour `allowed == false`.
    s.refusal = gate.status;
    s.cancel_session(gate.outcome);
    gate.outcome.effects.push_back(Effect::SurfaceWarning);
    gate.outcome.verdict = Verdict::Sent;
    return gate;
}

// ─── Inspection ─────────────────────────────────────────────────────────────────────────────

proto::ConnState SessionMachine::state() const noexcept { return impl_->state; }
const std::string& SessionMachine::current_session_id() const noexcept { return impl_->session_id; }
uint64_t SessionMachine::last_seq() const noexcept { return impl_->last_seq; }
uint64_t SessionMachine::utterance_id() const noexcept { return impl_->utterance_id; }
const TargetIdentity& SessionMachine::target() const noexcept { return impl_->target; }
InjectStatus SessionMachine::refusal() const noexcept { return impl_->refusal; }
bool SessionMachine::utterance_open() const noexcept { return impl_->utterance_open; }
uint64_t SessionMachine::utterances_completed() const noexcept {
    return impl_->utterances_completed;
}

bool SessionMachine::next_utterance_needs_leading_space() const noexcept {
    // plan.md §1.6: every utterance after the first in a session is appended after a space.
    return impl_->utterances_completed > 0;
}

SessionSnapshot SessionMachine::snapshot() const {
    SessionSnapshot s;
    s.state = impl_->state;
    s.current_session_id = impl_->session_id;
    s.last_seq = impl_->last_seq;
    s.utterance_id = impl_->utterance_id;
    s.last_stable_bytes = impl_->last_stable_bytes;
    s.cancelled = impl_->discarding;
    return s;
}

void SessionMachine::restore(const SessionSnapshot& s) {
    Impl& i = *impl_;
    i.state = s.state;
    i.state_entered_ms = i.now();
    i.loading_seen_ms = i.state_entered_ms;
    i.session_id = s.current_session_id;
    i.last_seq = s.last_seq;
    i.utterance_id = s.utterance_id;
    i.last_stable_bytes = s.last_stable_bytes;
    i.prev_text.clear();
    // A snapshot carries the high-water mark but not the text that established it, so the
    // "bytes below the mark are unchanged" half of §8.2 cannot be evaluated for the first message
    // after a restore. on_message() says so where it matters rather than pretending otherwise.
    i.have_prev_text = false;
    i.discarding = s.cancelled;
    i.activation_ms = i.state_entered_ms;
    i.last_voice_ms = i.state_entered_ms;
    i.utterance_open = false;
    i.utterance_counted = false;
    i.utterances_completed = 0;
}

void SessionMachine::restart_connection() {
    Impl& i = *impl_;
    i.clear_session();
    i.refusal = InjectStatus::Ok;
    i.enter(proto::ConnState::Spawning);
    // The SessionIdGenerator counter is deliberately untouched (docs §7.1, fixture 020).
}

SessionIdGenerator& SessionMachine::id_generator() noexcept { return impl_->ids; }

}  // namespace kd
