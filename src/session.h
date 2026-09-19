// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// The session / utterance state machine. **Pure — no Windows, no I/O, no threads.**
//
// Implements: plan.md §1.6 (session lifetime and the two silence timers), plan.md §1.8
// (activation must not immediately self-stop), and docs/protocol.md §7 (identity and ordering)
// and §10 (lifecycle). It is the module plan.md §2 marks "← unit-tested" and plan.md §7 lists
// first under test breadth.
//
// ─── What this module is, and what it deliberately is not ───────────────────────────────────
//
// It is a **decision function**: given the current state and one event, it returns a verdict, a
// list of messages to send, and a list of observable effects. It sends nothing itself, injects
// nothing, touches no clock but the one handed to it, and owns no thread. That is what makes
// the fixtures in tests/fixtures/protocol/ (`kind: "session"`, cases 014–020) executable
// against it directly: the runner sets `initial`, feeds `messages`, and compares verdicts.
//
// It is **not** the commit engine. This machine decides whether a message is *applicable*
// (right session, right order, right state); commit_engine.h decides what part of an applicable
// message is *safe to type*. Keeping them apart matters because the staleness rule and the
// prefix invariant fail in different ways and must be testable in isolation.
//
// ─── The failure this exists to prevent ─────────────────────────────────────────────────────
//
// docs/protocol.md §7 opens with it, and so does plan.md's risk table: **a late `final` from a
// finished session being injected into whatever window the user has since focused.** Every rule
// here traces back to that. The rule is absolute — drop, never clamp, never queue, never inject
// (fixture 014) — with exactly one exception: `stopped` and `error` for a non-current session
// are recorded for bookkeeping and applied to nothing, because without that the Client could
// never observe the orderly end of a session it has already moved on from.
//
// ─── Threading ──────────────────────────────────────────────────────────────────────────────
//
// SessionMachine is **single-threaded and owns no lock.** The owner (main.cpp) drives it from
// the UI thread only. Inbound protocol messages arrive on the IPC read thread and are posted to
// the UI thread before they reach this class; hotkey and mouse events arrive on the hook thread
// and are posted the same way — plan.md §1.8 requires the low-level hook callback never to
// block, and taking a lock this machine might hold is exactly the kind of block that gets a
// hook silently unhooked by Windows.
//
// The consequence for implementers: **do not add a mutex to make this callable from the IPC
// thread.** The single-thread rule is the design, and it is what makes the state machine
// testable without any concurrency at all.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "injector.h"   // TargetIdentity / IntegrityLevel / InjectStatus — all Windows-free types
#include "protocol.h"
#include "util/clock.h"

namespace kd {

// ─── Verdicts ───────────────────────────────────────────────────────────────────────────────
//
// The spellings of to_string(Verdict) match docs/protocol.md §12.5 exactly, because the fixture
// runner compares them as strings. Changing one changes the wire-adjacent contract with the
// Python suite; do not rename without changing the fixtures and the Python runner together.

enum class Verdict : uint8_t {
    Accept,                 // "accept"
    AcceptAfterGap,         // "accept_after_gap"
    AcceptBookkeepingOnly,  // "accept_bookkeeping_only"  — §7.1, state only, no text, no inject
    DropStaleSession,       // "drop_stale_session"
    DropCancelledSession,   // "drop_cancelled_session"   — §10.4, after we wrote `cancel`
    DropDuplicateSeq,       // "drop_duplicate_seq"
    DropOutOfOrder,         // "drop_out_of_order"
    ProtocolError,          // "protocol_error"           — outcome.error is populated
    Sent,                   // "sent"                     — for `from: "client"` fixture entries
    AbortSession,           // "abort_session"            — for `from: "transport"` entries
    Reconnected,            // "reconnected"

    // "refused" — a **local extension**, not one of the §12.5 verdicts, and no fixture uses it.
    // It is the answer to "the user pressed the hotkey and we are not going to start a session":
    // an elevated or password target (plan.md §1.7), the secure desktop, or a press while the
    // model is still loading (docs §10.2 forbids `start` outside IDLE). It exists so the refusal
    // is a decided state surfaced at the moment of the press, rather than a session that starts
    // and then mysteriously injects nothing. `refusal()` says which.
    Refused,
};

const char* to_string(Verdict v) noexcept;

// ─── Effects ────────────────────────────────────────────────────────────────────────────────
//
// Observable consequences the owner must carry out. docs/protocol.md §12.5 models these as
// prose assertions and says runners "SHOULD assert the ones they can express and MUST NOT fail
// a case for an effect they do not model" — so this enum is the C++ suite's expressible subset,
// not a normative list.

enum class Effect : uint8_t {
    LogGap,                 // a seq gap was accepted; log expected vs received
    ResetAnchorConfidence,  // §7.4 — the real consequence of a gap (plan.md §1.5)
    ResetCommitEngine,      // SESSION boundary only — maps to CommitEngine::reset(), which
                            // is session-scoped. An utterance boundary must NOT emit this:
                            // the engine resets its own utterance state, and reset() would
                            // clear session_has_committed (see session.cpp's new_utterance).
    ApplyText,              // hand this message to the commit engine
    FlushPendingAudio,      // before `stop`: §6.3's Client obligation
    DropQueuedAudio,        // before `cancel`
    SurfaceWarning,         // tray warning state (stability violation, degraded anchor)
    SurfaceFatal,           // tray error state; connection is over
    KillChild,              // local-only failure: spawn/handshake/model-load timeout, peer mismatch
    RespawnChild,           // §10.3 step 5 — new random pipe name, new session_id space
    StartEndpointTimer,     // plan.md §1.6, ~0.8 s — ends an utterance, not the session
    StartSessionTimer,      // plan.md §1.6, 120 s default, `never` permitted
    StopTimers,
};

const char* to_string(Effect e) noexcept;

// ─── Events the machine accepts ─────────────────────────────────────────────────────────────

// Things the user or the local system did. Everything the hotkey module, the mouse hook, the
// timers and the audio path can say.
enum class LocalEvent : uint8_t {
    // The user's start-or-finish gesture. From Idle it produces `start`; from Running it
    // finalises — FlushPendingAudio, then `stop`, then the last `final`.
    //
    // **Renamed from `HotkeyPressed`, and the rename is not cosmetic** (docs/editing.md §8).
    // After §4.5 the hotkey no longer finalises anything while the compose buffer is running:
    // Enter does. An enumerator called `HotkeyPressed` sitting on a path the hotkey never takes
    // is the single most reliable way to make the next reader wrong about what the product does.
    //
    // Both branches are live. Idle -> `start` is still reached by the hotkey, which is the only
    // thing that starts a session. Running -> finalise is reached by Enter when editing is on,
    // and by the hotkey's second press when it is off — `editing.enabled` is forced false for
    // any cadence but end-of-session, so plan.md §1.8's toggle survives wherever the compose
    // buffer does not exist. docs/editing.md §16 expected this branch to become dead code; it
    // does not, and that is why it is kept rather than deleted.
    FinalisePressed,
    EscPressed,           // cancel — discards everything uncommitted
    StopOnInput,          // a real click or keystroke ended the session (plan.md §1.8)
    EndpointSilence,      // utterance boundary; the session continues (plan.md §1.6)
    SessionSilence,       // the session's own timeout expired
    CaptureGlitch,        // WASAPI discontinuity -> emit `discontinuity` (docs §6.1)
    AudioQueueFull,       // §9 — fatal `overload_audio`, never a silent drop
    ControlQueueFull,     // §9 — fatal `overload_control`
    TargetChanged,        // foreground/focus moved; plan.md §1.7 ends the session rather than
                          // continuing into a new target
    StabilityViolation,   // the commit engine rejected a hypothesis (§8.3); abort the session

    // Speech onset from the VAD gate (plan.md §1.6: "Stop feeding at the endpoint, reset decoder
    // state, resume on speech onset"). This is the event that arms and re-arms **both** silence
    // timers, and without it neither of them can be driven by on_tick() — the machine has no
    // other way to learn that the user is still talking. Appended to the enum rather than
    // inserted, because the earlier values are compared as strings by the fixture runner.
    SpeechDetected,

    // ── The compose buffer's two states (docs/editing.md §3.2, §3.3, §9) ────────────────────
    //
    // Appended, like SpeechDetected above and for the same reason: the earlier values are
    // compared as strings by the fixture runner.
    //
    // Neither carries a verdict of its own and neither changes a protocol state. What they change
    // is which timers are armed, which is the smallest thing that can express "the user is
    // present but not speaking".

    // The first edit key closed the microphone. The Client hands the user the tail the pill was
    // drawing and abandons the rest of that round trip (§3.3, App::settle_pending_tail) — the
    // answer to "stop, I want to fix that" cannot be "wait". What this machine owes the Server is
    // the other half: a `discontinuity`, not merely FlushPendingAudio, because its hypotheses are
    // cumulative and a Server left mid-utterance repeats every settled word on the resume.
    // session.cpp's handler has the whole reason.
    //
    // It also **pauses the session-silence timer**, and §9 is emphatic that this is a decision
    // rather than a preference. That timer measures how long a session survives *with no speech*;
    // during editing there is no microphone to be silent into, so leaving it armed would end the
    // session — typing a half-finished buffer into the document — because the user spent two
    // minutes reading what they had dictated. progress.md §8 records what happened the last time
    // "silence" and "wall-clock since activation" were confused.
    EditingBegan,

    // The hotkey, pressed while editing: the microphone reopens and the session-silence timer is
    // armed again from now.
    DictationResumed,
};

const char* to_string(LocalEvent e) noexcept;

// Things the transport did. The fixture files use these spellings in `from: "transport"`
// entries ("broken_pipe", …).
enum class TransportEvent : uint8_t {
    Connected,        // ConnectNamedPipe completed
    PeerValidated,    // GetNamedPipeClientProcessId matched the spawned child
    PeerMismatch,     // it did not — disconnect, kill, report peer_mismatch (§2.4)
    BrokenPipe,       // ERROR_BROKEN_PIPE / ERROR_PIPE_NOT_CONNECTED / child exit (§10.3)
    EofAtFrameBoundary,  // orderly (fixture 010)
    EofMidFrame,      // truncated_frame (fixture 011)
    ConnectTimeout,   // CONNECT_TIMEOUT_MS -> spawn_timeout
    HandshakeTimeout, // HELLO_TIMEOUT_MS
    ModelLoadTimeout, // READY_TIMEOUT_MS, reset by each `loading`

    // "child_respawned" — fixture 020 uses this spelling and there was no enumerator for it.
    // It means §10.3 step 5 has completed: the old child was reaped, a new pipe with a new random
    // name exists, and a new child is connected on it. The machine answers `reconnected` and
    // re-sends `hello`. It deliberately collapses SPAWNING/VALIDATING, which fixture 020 does not
    // exercise; the production path still walks Connected -> PeerValidated -> Handshake.
    ChildRespawned,
};

const char* to_string(TransportEvent e) noexcept;

// ─── Configuration ──────────────────────────────────────────────────────────────────────────

struct SessionConfig {
    // plan.md §1.6, the two timers a naive implementation merges. They are unrelated and are
    // named separately here so that nobody re-merges them.
    uint32_t endpoint_silence_ms = 800;      // ends an UTTERANCE; the session continues
    uint32_t session_silence_ms = 120000;    // how long the session survives with no speech
    bool session_silence_never = false;      // `never` is a legitimate setting (plan.md §1.6),
                                             // safe because stop-on-input already covers the
                                             // real "I'm done" moments (plan.md §1.8)

    // plan.md §1.8: the chord's own key-ups, and the injected Ctrl tap that suppresses the
    // Start menu, arrive *after* activation and would otherwise stop the session immediately.
    // The rule is implemented in hotkey.h's InputFilter; this is how long the machine additionally
    // refuses stop-on-input after activation, as a belt-and-braces guard for events that reach
    // it by another path.
    uint32_t activation_grace_ms = 250;

    // Vocabulary hints sent with `start` (docs §6.1). Empty means the field is omitted, which is
    // NOT the same as sending an empty array — plan.md §4 compares context-enabled and
    // context-free runs separately.
    std::vector<std::string> context;
    bool send_context = false;

    // plan.md §1.7's refusals, decided at session start rather than after a failed injection.
    // They mirror InjectConfig's flags of the same name and settings.h must keep the two in step;
    // they are duplicated here because the decision has to be made **before** a session exists,
    // and the injector is not consulted until there is text to type.
    bool refuse_elevated_targets = true;
    bool refuse_password_fields = true;
};

// ─── The one collaborator this machine has ──────────────────────────────────────────────────
//
// Everything the state machine needs to know about the OS-level target, behind an interface so
// that the machine stays pure and every refusal and focus race is reachable from a unit test.
// Production wires this to injector.h's capture_foreground_target() / validate_target() /
// secure_desktop_active(); tests wire it to a fake and no window ever has to exist.
//
// A null provider means "no gate available": the machine then captures no identity and allows
// injection. That is the right behaviour for the fixture runner, which drives only the protocol
// rules — and the wrong behaviour for production, so main.cpp MUST install one.
class ITargetProvider {
public:
    virtual ~ITargetProvider() = default;

    // Called once, at hotkey press. False means the foreground target could not be identified,
    // which is a refusal — injecting into a window we cannot name is the failure this module
    // exists to prevent.
    virtual bool capture_target(TargetIdentity& out) = 0;

    // Called immediately before **every** injection, never once per session (plan.md §1.7).
    virtual bool target_still_current(const TargetIdentity& captured) = 0;

    // UAC prompt or lock screen. Refused outright, at start and before every injection: the
    // secure desktop can appear in the middle of a session.
    virtual bool secure_desktop_active() = 0;

    // Our own integrity level, for the UIPI comparison. Compared numerically against the
    // target's; Unknown on either side means "cannot prove elevation", and the machine then does
    // not refuse — an unprovable guess in either direction is worse than the honest limit.
    virtual IntegrityLevel self_integrity() = 0;
};

// ─── Session identity ───────────────────────────────────────────────────────────────────────

// docs/protocol.md §7.1: Client-generated, `[A-Za-z0-9_-]{1,64}`, unique for the lifetime of the
// Client process **including across child restarts**. Reusing an id after a restart would defeat
// the staleness check exactly when it matters most (fixture 020), so the counter never resets —
// not on child restart, not on model change, not on reconnect.
class SessionIdGenerator {
public:
    explicit SessionIdGenerator(uint64_t start = 1) noexcept : next_(start) {}
    std::string next();                                  // "s1", "s2", …
    [[nodiscard]] uint64_t peek() const noexcept { return next_; }

private:
    uint64_t next_;
};

// ─── The machine ────────────────────────────────────────────────────────────────────────────

// A snapshot, matching the fixture `initial` block field for field (docs §12.5) so a test can
// construct a machine mid-session without replaying a trace.
struct SessionSnapshot {
    proto::ConnState state = proto::ConnState::Idle;
    std::string current_session_id;
    uint64_t last_seq = 0;
    uint64_t utterance_id = 0;
    uint64_t last_stable_bytes = 0;
    bool cancelled = false;  // `cancel` written, `stopped` not yet received (§10.4)
};

class SessionMachine {
public:
    struct Outcome {
        Verdict verdict = Verdict::Accept;

        // Messages the owner must write, in order, before processing anything else.
        std::vector<proto::Message> to_send;

        std::vector<Effect> effects;

        // Populated when verdict == ProtocolError, or when a fatal local condition was
        // detected. For a non-fatal error the machine has already filled in `session_id`,
        // which §11.1 requires.
        std::optional<proto::ProtocolError> error;

        // Set on AcceptAfterGap, for the log line §7.4 asks for.
        uint64_t gap_expected_seq = 0;
        uint64_t gap_received_seq = 0;

        [[nodiscard]] bool has_effect(Effect e) const noexcept;
    };

    // The gate `before_inject()` returns. `allowed == false` means **do not call the injector**;
    // `outcome` carries whatever the refusal produced (a `cancel` to write, effects to apply) and
    // must be processed like any other Outcome.
    struct InjectGate {
        bool allowed = false;
        InjectStatus status = InjectStatus::Ok;  // why not, when !allowed
        Outcome outcome;
    };

    // The clock must outlive the machine. Tests pass a ManualClock; production passes
    // SteadyClock::instance().
    SessionMachine(const IClock& clock, SessionConfig cfg);

    // The target provider must outlive the machine. Not a constructor argument so that the
    // fixture runner can build a machine with no Windows collaborators at all.
    void set_target_provider(ITargetProvider* provider) noexcept;

    // Replace the configuration. The mirror of CommitEngine::set_params, and for the same two
    // callers: the settings dialog, and a bench sweep between runs.
    //
    // The timers are read as they fire rather than latched at session start, so a change during a
    // live session applies to that session. That is the intended behaviour — a user who has just
    // shortened the endpoint silence means *now* — and it is safe because the machine keeps no
    // derived state from the config. It does not touch connection or session state: nothing here
    // can end a session, and a new machine (which would start in SPAWNING and reject the next
    // message from the connected child) is never the right way to change a timer.
    void set_config(const SessionConfig& cfg);
    [[nodiscard]] const SessionConfig& config() const noexcept;

    // ── Driving it ──────────────────────────────────────────────────────────────────────────

    // An inbound control message from the Server. Applies §7.1 staleness, §7.4 ordering, §10.2
    // state rules, in that order — the order matters, because a stale message must be dropped
    // before its `seq` is allowed to move the current session's high-water mark.
    Outcome on_message(const proto::Message& m);

    // A local event. `on_local(FinalisePressed)` from Idle produces the `start` message and the
    // new session_id; from Running it produces FlushPendingAudio + `stop`.
    Outcome on_local(LocalEvent e);

    // A transport event.
    Outcome on_transport(TransportEvent e);

    // Time-driven transitions (the two silence timers, and the protocol deadlines). Call it at
    // least every 100 ms while a connection exists. Returns an Outcome with an empty verdict
    // set when nothing fired.
    Outcome on_tick();

    // Capture-glitch detail: the machine needs the gap length to build `discontinuity`.
    Outcome on_capture_glitch(uint32_t gap_ms, std::string_view reason);

    // Revalidate the target immediately before an injection (plan.md §1.7: "Both are validated
    // before every injection. Any change ends the session rather than continuing into a new
    // target."). On any change this ends the session with `cancel` semantics — the uncommitted
    // tail is discarded rather than typed into a window we can no longer name — and returns
    // allowed = false. Committed text stays where it is; there is no repair path anywhere here.
    InjectGate before_inject();

    // ── Inspecting it ───────────────────────────────────────────────────────────────────────

    [[nodiscard]] proto::ConnState state() const noexcept;
    [[nodiscard]] const std::string& current_session_id() const noexcept;
    [[nodiscard]] uint64_t last_seq() const noexcept;
    [[nodiscard]] uint64_t utterance_id() const noexcept;
    [[nodiscard]] SessionSnapshot snapshot() const;

    // The identity captured at hotkey press. Empty (is_valid() == false) when no session is open
    // or no provider is installed.
    [[nodiscard]] const TargetIdentity& target() const noexcept;

    // Why the last hotkey press was refused. InjectStatus::Ok when the last press started a
    // session. Held rather than only returned, because the tray and overlay show it.
    [[nodiscard]] InjectStatus refusal() const noexcept;

    // ── The two silence timers, separately observable (plan.md §1.6) ────────────────────────
    //
    // They are exposed apart from each other on purpose: an implementation that merged them
    // would still satisfy any test written against the session alone, and merging them is the
    // specific mistake plan.md §1.6 warns about.

    // True between speech onset and the endpoint-silence timeout. False does NOT mean the
    // session ended.
    [[nodiscard]] bool utterance_open() const noexcept;

    // Utterances that have ended in this session, by endpoint timeout or by `final`.
    [[nodiscard]] uint64_t utterances_completed() const noexcept;

    // plan.md §1.6: "resuming starts a new utterance appended after a space". True once anything
    // has already been spoken in this session, and it is what feeds CommitOp::leading_space.
    [[nodiscard]] bool next_utterance_needs_leading_space() const noexcept;

    // Test seam: install a snapshot wholesale. Only fixtures and unit tests use this; production
    // reaches every state by replaying real events, which is the point.
    void restore(const SessionSnapshot& s);

    // Reset to Spawning with a fresh child. Keeps the SessionIdGenerator's counter — §7.1
    // requires ids never to be reused across restarts (fixture 020).
    void restart_connection();

    [[nodiscard]] SessionIdGenerator& id_generator() noexcept;

private:
    struct Impl;
    Impl* impl_;

public:
    ~SessionMachine();
    SessionMachine(const SessionMachine&) = delete;
    SessionMachine& operator=(const SessionMachine&) = delete;
};

}  // namespace kd
