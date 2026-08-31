// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// The server-side mirror of `src/session.cpp`: one session per connection, start to `stopped`.
// Ports `server/kvit_asr/session.py` and `server/kvit_asr/connection.py`'s routing.
//
// ─── The state machine, which is small because the protocol is ──────────────────────────────
//
//   hello          -> validate proto; hello_ack (with models[]); then load
//                     load fails -> fatal error (not `ready`), orderly close
//                     load ok    -> ready
//   start          -> open session, reset endpointer and stability, utterance_id = 1
//   audio (0x02)   -> feed the endpointer; gated audio joins the utterance window
//     endpoint     -> close utterance, emit final, ++utterance_id
//     hop elapsed  -> decode window, emit partial   (off in finalize-only)
//   stop           -> flush, decode the remainder, final, stopped
//   cancel         -> drop everything, abort in-flight HTTP, stopped(reason=cancel),
//                     no further partial/final
//   discontinuity  -> flush the decoder's state
//
// The handshake half lives in `main.cpp`; this class owns everything from `start` onwards.
//
// ─── Threads ────────────────────────────────────────────────────────────────────────────────
//
// | connection | framing, session state, endpointing, all inbound routing | parked in recv() |
// | decode     | one HTTP request at a time, and the emission that follows it |
//
// The decode thread is not an optimisation. A transcription round trip against the measured
// endpoint is 237-541 ms and the optional VAD gate a further 50-176 ms; doing that on the
// connection thread would stall audio intake for half a second at a time.
//
// **One in flight, always.** A hop, an endpoint or a `stop` that fires while a POST is running
// waits for that POST or replaces the pending work with the newer buffer; a second WinHTTP call
// is never queued. `cancel` and teardown close the in-flight request handle, so `stop` never
// sits on the HTTP timeout before emitting `stopped`.
//
// ─── Why there is no `overload_audio` here ──────────────────────────────────────────────────
//
// docs/protocol.md §9 bounds the Server's *inbound audio queue* and makes an overflow a fatal
// `overload_audio` — never a dropped frame. The Python server needs that bound because its
// decode happens inline on the reader thread, so a slow backend really does back audio up.
//
// This translator has no such queue: the connection thread runs the endpointer (about 31 frames
// per second of audio over 512 integers) and appends gated audio to the backend's window, and
// then returns. Nothing on that path waits for the network, so there is no queue to overflow
// and no bound to enforce. The buffer that *does* grow is the current utterance's audio, and it
// is bounded three ways — the model's own segment boundary, `VadConfig::soft_max_utterance_ms`
// and `max_utterance_ms` — none of which is §9's mechanism and all of which fail as a closed
// utterance rather than as a dropped frame.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "asrd/backend_openai.h"
#include "asrd/stability.h"
#include "asrd/vad.h"
#include "protocol.h"

namespace kd::asrd {

// Where outbound messages go. The connection owns the socket; this keeps the session testable
// against a vector of messages instead of a port.
class MessageSink {
public:
    virtual ~MessageSink() = default;
    // False means the peer is gone. There is nobody left to tell, so the session records that
    // the connection is finished and stops.
    virtual bool send(const proto::Message& m) = 0;
};

struct SessionServerConfig {
    StabilityParams stability;
    VadConfig vad;

    // Whether a `partial` is emitted at all. Off is finalize-only: one `final` per utterance at
    // the endpoint and nothing in between.
    //
    // **Independent of the mark below, and that separation is the whole point.** A `partial`
    // with `stable_bytes = 0` promises nothing — §8.2 constrains only the published prefix, so
    // with no prefix published the model may revise every byte it has ever emitted and stay
    // conforming, and `final` need only begin with a zero-length prefix, which it does. What
    // such a message *is* good for is the thing the user actually sees: the Client greys it out
    // as "still moving" and commits none of it. Preview and mark were one flag until a batch
    // model published 490 bytes, re-punctuated byte 421, and took the session — and the words
    // in flight — down with it. See `publish_stable_partials`.
    bool emit_partials = false;

    // Whether a `partial` may carry a non-zero `stable_bytes` at all.
    //
    // **False for any backend that re-decodes the whole utterance from scratch**, which is every
    // batch model behind a window. Such a model does not extend its previous hypothesis, it
    // replaces it: once it can see where the sentence ended it re-cases the opening word and
    // inserts the punctuation it was not sure of, so `so I was thinking we` becomes `So I was
    // thinking, we`. That revises byte 0 — and §8.2 binds a new hypothesis against the
    // *previously published* mark, so once a non-zero mark is out there, no value of
    // `stable_bytes` makes the next message conforming.
    //
    // Note what this is not: it is not a stability parameter that trades latency for safety.
    // N/M/T delay the mark, and delaying a mark that will later be contradicted at byte 0 only
    // changes when the abort happens. There is no setting of the three knobs that makes a
    // revising backend safe, which is why this is a separate switch and not a tuning.
    bool publish_stable_partials = false;

    // An utterance that decodes to nothing produces no `final`: emitting an empty one would
    // burn a `seq` and a `utterance_id` to say nothing.
    bool emit_empty_finals = false;

    // openai-translator.md §4.7 / `server/tests/test_hostile_stability.py`. Forces an illegal
    // `stable_bytes` onto the wire so §8.3's handling can be exercised end to end. Logged at
    // ERROR on every use, and deliberately **not reachable from backend options** — a backend
    // must never be able to authorise its own protocol violations.
    bool hostile_stability = false;
};

// One session per connection (§7.2). Sessions do not nest and are not concurrent; the socket is
// not a multiplexer.
class SessionServer {
public:
    SessionServer(OpenAiBackend& backend, SessionServerConfig cfg, MessageSink& sink);
    ~SessionServer();
    SessionServer(const SessionServer&) = delete;
    SessionServer& operator=(const SessionServer&) = delete;

    // Every one of these runs on the connection thread.
    //
    // Each returns false when the *connection* is finished — a fatal error has been written and
    // the caller must stop reading (§11.1). A session-scoped failure returns true: the session
    // is over, the connection is not.
    bool on_start(const proto::StartMsg& m);
    bool on_stop(const proto::StopMsg& m);
    bool on_cancel(const proto::CancelMsg& m);
    bool on_discontinuity(const proto::DiscontinuityMsg& m);
    bool on_audio(const int16_t* samples, size_t count);
    // A peer `error` (§11.1/§13). A non-fatal one naming the open session abandons that session;
    // a fatal one ends the connection.
    bool on_peer_error(const proto::ErrorMsg& m);

    // Advance the T-millisecond stability hold when no new hypothesis has arrived. The owner
    // must call this on a timer — roughly every T/2 ms — for the duration of the session. See
    // StabilityTracker::poll for the silent failure that happens otherwise.
    void tick();

    // Abandon whatever is open and stop the decode thread. Idempotent.
    void close();

    [[nodiscard]] std::string current_session_id() const;
    [[nodiscard]] bool connection_fatal() const noexcept;
    [[nodiscard]] uint64_t discarded_audio_bytes() const noexcept;

private:
    void worker();
    // One decode, with the retries a *flush* is worth and a hop is not. Decode thread only.
    // Resets the consecutive-failure count on success, so the two below are one mechanism.
    [[nodiscard]] Status decode_once(bool finalize, Transcription& out);
    // Book-keeping for a round trip that did not come back. Returns true when the session has
    // to be failed — which is the *last* resort, not the first. Decode thread only.
    [[nodiscard]] bool on_decode_failure(bool finalize, bool published, const Status& st);
    void request_work(bool hop, bool finalize, bool stop);
    // Wait for the decode thread to go idle, bounded. Called before an utterance boundary so a
    // new utterance cannot start on top of a decode still running for the previous one.
    void wait_for_idle(std::unique_lock<std::mutex>& lock);

    void open_utterance();
    void close_utterance();
    // Close the current utterance *at a point inside it* and open the next one on the audio
    // that follows, instead of on silence. Decode thread only. See the definition.
    bool split_utterance(const ProposedCut& cut, uint64_t utt);
    bool emit_text(proto::Message& m);
    void emit_partial(const std::string& text, uint64_t stable_bytes);
    void emit_final(const std::string& text, uint64_t stable_bytes);
    void send_stopped(const char* reason);
    void fail_session(proto::ErrorCode code, const std::string& msg);
    void fail_fatal(proto::ErrorCode code, const std::string& msg);
    [[nodiscard]] uint64_t audio_pos_ms_locked() const noexcept;

    OpenAiBackend& backend_;
    SessionServerConfig cfg_;
    MessageSink& sink_;

    VadEndpointer vad_;              // connection thread only
    StabilityTracker stability_;     // decode thread only

    mutable std::mutex m_;
    std::condition_variable work_cv_;   // connection -> decode
    std::condition_variable idle_cv_;   // decode -> connection
    std::thread worker_;

    bool quit_ = false;
    bool busy_ = false;
    bool pending_hop_ = false;
    bool pending_finalize_ = false;
    bool pending_stop_ = false;
    bool pending_tick_ = false;

    bool has_session_ = false;
    std::string session_id_;
    bool terminal_ = false;
    bool cancelled_ = false;
    bool stopping_ = false;
    bool connection_fatal_ = false;

    uint64_t seq_ = 0;
    uint64_t utterance_id_ = 0;
    bool utterance_open_ = false;
    uint64_t audio_bytes_total_ = 0;
    uint64_t discarded_audio_bytes_ = 0;

    std::string last_text_;
    bool have_last_text_ = false;
    uint64_t last_stable_ = 0;

    // How many round trips in a row have failed. Decode thread only, so it needs no lock.
    //
    // This exists because one failure used to end the session. A `decode_failed` is written as
    // "the model misbehaved on this utterance", and for a model that is handed a recording and
    // returns a string that reading is right — but the failure this actually sees is almost
    // always the *transport*: a dropped keep-alive, a connection reset, a service that was busy
    // for a moment. The speaker is mid-sentence and none of that is about their words.
    //
    // Ending the session there is not a graceful degradation, because under
    // `Cadence::EndOfSession` the Client's `stopped` handler flushes a whole session's buffer
    // into the document. Measured on 2026-08-31: a single `WinHttpReceiveResponse failed` typed
    // 1659 bytes into the user's editor mid-word, four minutes into a dictation.
    uint32_t decode_failures_ = 0;

    std::vector<VadItem> items_;  // scratch for on_audio, so it does not allocate per frame
};

}  // namespace kd::asrd
