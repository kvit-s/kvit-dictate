// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// The session loop — see session_server.h for the state machine, the two threads, and why
// there is no `overload_audio` here.

#define KD_LOG_MODULE "asrd.sess"

#include "asrd/session_server.h"

#include <algorithm>

#include "util/log.h"

namespace kd::asrd {
namespace {

constexpr uint32_t kBytesPerSample = 2;

// A bound on how long the connection thread will wait for a decode already in flight before
// starting the next utterance. Generous, because the thing being waited for is one HTTP request
// whose own timeout is shorter; if this ever expires, the decode thread is wedged and the
// utterance boundary is the least of the problems.
constexpr auto kIdleWait = std::chrono::seconds(60);

proto::Message stopped_message(const std::string& session_id, const char* reason) {
    proto::StoppedMsg m;
    m.session_id = session_id;
    m.reason = std::string(reason);
    return proto::Message::of(std::move(m));
}

proto::Message error_message(proto::ErrorCode code, bool fatal, const std::string& msg,
                             const std::string* session_id) {
    proto::ErrorMsg e;
    e.code = code;
    e.code_raw = proto::to_string(code);
    e.fatal = fatal;
    e.msg = msg;
    if (session_id != nullptr) e.session_id = *session_id;
    return proto::Message::of(std::move(e));
}

}  // namespace

SessionServer::SessionServer(OpenAiBackend& backend, SessionServerConfig cfg, MessageSink& sink)
    : backend_(backend),
      cfg_(std::move(cfg)),
      sink_(sink),
      vad_(cfg_.vad),
      stability_(cfg_.stability) {
    worker_ = std::thread([this] { worker(); });
}

SessionServer::~SessionServer() { close(); }

void SessionServer::close() {
    {
        std::lock_guard<std::mutex> lock(m_);
        if (quit_) return;
        // Abandon anything open. §6.1's `stopped` has nowhere to go once the socket is closing,
        // and the client's own teardown does not wait for it on this path.
        cancelled_ = true;
        terminal_ = true;
        quit_ = true;
    }
    backend_.cancel();
    work_cv_.notify_all();
    idle_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

std::string SessionServer::current_session_id() const {
    std::lock_guard<std::mutex> lock(m_);
    return (has_session_ && !terminal_) ? session_id_ : std::string();
}

bool SessionServer::connection_fatal() const noexcept {
    std::lock_guard<std::mutex> lock(m_);
    return connection_fatal_;
}

uint64_t SessionServer::discarded_audio_bytes() const noexcept {
    std::lock_guard<std::mutex> lock(m_);
    return discarded_audio_bytes_;
}

uint64_t SessionServer::audio_pos_ms_locked() const noexcept {
    return audio_bytes_total_ * 1000u / (proto::kSampleRate * kBytesPerSample);
}

// ─── Work handoff ───────────────────────────────────────────────────────────────────────────

void SessionServer::request_work(bool hop, bool finalize, bool stop) {
    {
        std::lock_guard<std::mutex> lock(m_);
        // A hop that arrives while one is already pending does not queue a second WinHTTP call:
        // it replaces the pending work, and the newer buffer is what gets transcribed.
        pending_hop_ = pending_hop_ || hop;
        pending_finalize_ = pending_finalize_ || finalize;
        pending_stop_ = pending_stop_ || stop;
    }
    work_cv_.notify_one();
}

void SessionServer::wait_for_idle(std::unique_lock<std::mutex>& lock) {
    idle_cv_.wait_for(lock, kIdleWait,
                      [this] { return quit_ || (!busy_ && !pending_finalize_ && !pending_hop_); });
}

// ─── Inbound: control ───────────────────────────────────────────────────────────────────────

bool SessionServer::on_start(const proto::StartMsg& m) {
    {
        std::unique_lock<std::mutex> lock(m_);
        if (has_session_ && !terminal_) {
            // §7.2: sessions do not nest and are not concurrent; the socket is not a
            // multiplexer.
            lock.unlock();
            fail_fatal(proto::ErrorCode::state_violation,
                       "start for \"" + m.session_id + "\" while \"" + session_id_ +
                           "\" is open (docs/protocol.md §7.2)");
            return false;
        }
    }
    if (!proto::is_valid_session_id(m.session_id)) {
        fail_fatal(proto::ErrorCode::bad_message,
                   "session_id is not 1-64 bytes of [A-Za-z0-9_-] (docs/protocol.md §7.1)");
        return false;
    }
    if (m.sample_rate != static_cast<int32_t>(proto::kSampleRate)) {
        fail_fatal(proto::ErrorCode::bad_message,
                   "sample_rate must be 16000 in proto 1, got " + std::to_string(m.sample_rate));
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(m_);
        wait_for_idle(lock);
        has_session_ = true;
        session_id_ = m.session_id;
        terminal_ = false;
        cancelled_ = false;
        stopping_ = false;
        seq_ = 0;
        utterance_id_ = 0;
        utterance_open_ = false;
        audio_bytes_total_ = 0;
        last_text_.clear();
        have_last_text_ = false;
        last_stable_ = 0;
        pending_hop_ = false;
        pending_finalize_ = false;
        pending_stop_ = false;
    }
    // `context` is accepted and ignored: this backend declares context_supported = false in
    // `ready`, which §6.1 requires of a server that cannot use the hints — silently dropping
    // them while claiming support is what would corrupt plan.md §4's comparison.
    vad_.reset();
    backend_.reset();
    backend_.reset_cancel();
    stability_.reset(0);
    KD_LOG_INFO("session_start session_id=%s context=%zu", m.session_id.c_str(),
                m.context.size());
    return true;
}

bool SessionServer::on_stop(const proto::StopMsg& m) {
    {
        std::lock_guard<std::mutex> lock(m_);
        if (!has_session_ || session_id_ != m.session_id || terminal_) {
            // §6.1 / §10.5: idempotent with respect to unknown or already-ended sessions. Not
            // an error — a `stop` sent before any session names no session at all and is legal,
            // and the Client's teardown runs on crash paths where its own state is uncertain.
            const proto::Message reply = stopped_message(m.session_id, "unknown_session");
            sink_.send(reply);
            return true;
        }
        if (stopping_) return true;
        stopping_ = true;
    }

    // Flush the endpointer so the tail of the last word reaches the backend before the final.
    items_.clear();
    vad_.flush(items_);
    for (const VadItem& item : items_) {
        if (item.kind == VadItemKind::Audio && !item.audio.empty()) {
            backend_.feed(item.audio.data(), item.audio.size());
        }
    }

    bool finalize = false;
    {
        std::lock_guard<std::mutex> lock(m_);
        finalize = utterance_open_;
        utterance_open_ = false;
    }
    // `stop` always produces `stopped`, decode or no decode (§10.4). The worker sends it after
    // the final so the Client sees the text before the acknowledgement.
    request_work(false, finalize, true);
    return true;
}

bool SessionServer::on_cancel(const proto::CancelMsg& m) {
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(m_);
        if (!has_session_ || session_id_ != m.session_id || terminal_) {
            const proto::Message reply = stopped_message(m.session_id, "unknown_session");
            sink_.send(reply);
            return true;
        }
        cancelled_ = true;
        first = !terminal_;
        terminal_ = true;
        utterance_open_ = false;
        pending_hop_ = false;
        pending_finalize_ = false;
        pending_stop_ = false;
    }
    // §6.1 / §10.4: `stopped` goes out immediately rather than waiting for the very decode being
    // abandoned. Closing the in-flight request handle is what makes that decode return promptly
    // instead of sitting on the HTTP timeout.
    backend_.cancel();
    if (first) {
        const proto::Message reply = stopped_message(m.session_id, "cancel");
        if (!sink_.send(reply)) {
            std::lock_guard<std::mutex> lock(m_);
            connection_fatal_ = true;
        }
    }
    work_cv_.notify_all();
    return true;
}

bool SessionServer::on_discontinuity(const proto::DiscontinuityMsg& m) {
    {
        std::lock_guard<std::mutex> lock(m_);
        if (!has_session_ || session_id_ != m.session_id || terminal_) return true;
    }
    // §6.1: the Server MUST accept this and MUST NOT treat it as fatal.
    //
    // Ending the utterance is not optional dramatics, it follows from the protocol: resetting
    // decoder state mid-utterance would make the next `partial.text` for that `utterance_id` no
    // longer a superset of the previous one, and §8.1 requires it to be cumulative while §8.2
    // forbids the stable prefix from mutating. So a decoder reset and an utterance boundary
    // happen together, or the wire contract is violated.
    items_.clear();
    vad_.notify_discontinuity(static_cast<uint32_t>(std::max<int64_t>(0, m.gap_ms)),
                              m.reason.value_or(std::string()), items_);
    for (const VadItem& item : items_) {
        if (item.kind == VadItemKind::UtteranceEnd) close_utterance();
    }

    {
        std::lock_guard<std::mutex> lock(m_);
        // The gap is added to the session's audio timeline so `audio_pos_ms` keeps tracking wall
        // time rather than bytes-we-happened-to-receive. `gap_ms == 0` means "duration unknown"
        // (§6.1) and adds nothing.
        audio_bytes_total_ += static_cast<uint64_t>(m.gap_ms) * proto::kSampleRate *
                              kBytesPerSample / 1000u;
    }
    return true;
}

bool SessionServer::on_peer_error(const proto::ErrorMsg& m) {
    // §11.1/§13: act on `fatal`, even for a code this build does not recognise. The Client sends
    // `stability_violation` (fatal:false) when our own `stable_bytes` broke §8.2, which is the
    // ICER-relevant event in plan.md §1.4 — so it is logged at error level with the session id.
    KD_LOG_ERROR("peer_error code=%s fatal=%d session_id=%s msg=%s",
                 m.code_raw.empty() ? proto::to_string(m.code) : m.code_raw.c_str(),
                 m.fatal ? 1 : 0, m.session_id.value_or(std::string("-")).c_str(),
                 m.msg.c_str());
    if (m.fatal) {
        std::lock_guard<std::mutex> lock(m_);
        connection_fatal_ = true;
        return false;
    }
    if (m.session_id.has_value()) {
        proto::CancelMsg c;
        c.session_id = *m.session_id;
        return on_cancel(c);
    }
    return true;
}

// ─── Inbound: audio ─────────────────────────────────────────────────────────────────────────

bool SessionServer::on_audio(const int16_t* samples, size_t count) {
    if (count == 0) return true;  // legal and a no-op (§6.3, fixture 003)

    {
        std::lock_guard<std::mutex> lock(m_);
        if (!has_session_ || terminal_ || cancelled_) {
            // §6.3: losing the race between a `stop` and an in-flight audio frame is a Client
            // bug this rule contains rather than escalates.
            discarded_audio_bytes_ += count * kBytesPerSample;
            return true;
        }
        audio_bytes_total_ += count * kBytesPerSample;
    }

    items_.clear();
    vad_.process(samples, count, items_);

    bool hop_due = false;
    for (const VadItem& item : items_) {
        switch (item.kind) {
            case VadItemKind::SpeechStart:
                open_utterance();
                break;
            case VadItemKind::Audio: {
                bool open = false;
                {
                    std::lock_guard<std::mutex> lock(m_);
                    open = utterance_open_ && !terminal_ && !cancelled_;
                }
                if (open && !item.audio.empty()) {
                    if (backend_.feed(item.audio.data(), item.audio.size())) hop_due = true;
                }
                break;
            }
            case VadItemKind::UtteranceEnd:
                close_utterance();
                hop_due = false;  // the finalize supersedes any hop this chunk earned
                break;
            case VadItemKind::SpeechEnd:
                // The hangover, not the endpoint. plan.md §1.6: a brief mid-sentence pause must
                // not end the utterance, so nothing happens here — the endpoint timer is what
                // decides, and it is still running.
                break;
            case VadItemKind::Discontinuity:
                break;
        }
    }

    if (hop_due && !cfg_.emit_partials) {
        // Finalize-only: the hop cannot fire, because hop_ms was raised to window_ms. Reaching
        // here would mean the driver and the mode disagree.
        hop_due = false;
    }
    if (hop_due) request_work(true, false, false);

    // The tick is load-bearing, not an optimisation: driving it from the audio path keeps the
    // dwell advancing while the speaker is talking, and the Client streams the microphone
    // continuously (silence included), so audio arrival is a finer-grained clock than a T/2
    // timer would be.
    tick();
    return true;
}

void SessionServer::tick() {
    if (!cfg_.publish_stable_partials) {
        // The dwell has nothing to mature: nothing ever fed the tracker, so a poll would
        // evaluate a hypothesis it has never seen.
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_);
        if (terminal_ || cancelled_ || !utterance_open_ || !have_last_text_) return;
        if (busy_ || pending_hop_ || pending_finalize_) return;  // a decode will re-evaluate
        pending_tick_ = true;
    }
    work_cv_.notify_one();
}

// ─── Utterance lifecycle ────────────────────────────────────────────────────────────────────

void SessionServer::open_utterance() {
    std::unique_lock<std::mutex> lock(m_);
    if (terminal_ || cancelled_ || utterance_open_) return;
    // One utterance in flight at a time. A new one cannot start on top of a decode still running
    // for the previous one, because the backend's window and the stability tracker are both
    // per-utterance state that the decode is still reading. This is the "it waits for that POST"
    // half of §2.1's one-in-flight rule, and it costs at most one round trip — only reachable
    // when speech resumes within an RTT of an endpoint.
    wait_for_idle(lock);
    if (quit_) return;

    ++utterance_id_;  // §7.3: starts at 1, increments by exactly 1
    utterance_open_ = true;
    last_text_.clear();
    have_last_text_ = false;
    last_stable_ = 0;
    lock.unlock();

    backend_.reset();
}

void SessionServer::close_utterance() {
    {
        std::lock_guard<std::mutex> lock(m_);
        if (!utterance_open_ || terminal_ || cancelled_) return;
        utterance_open_ = false;
    }
    request_work(false, true, false);
}

// ─── Emission ───────────────────────────────────────────────────────────────────────────────

bool SessionServer::emit_text(proto::Message& m) {
    // The cancelled check and the seq increment happen under the same lock, in that order. That
    // is the whole of §10.4's Server obligation: a `cancel` read by the connection thread
    // between the decode finishing and the message being built cannot slip past, because the
    // flag is read after that thread set it and before this thread commits to a sequence number.
    {
        std::lock_guard<std::mutex> lock(m_);
        if (terminal_ || cancelled_) return false;
        ++seq_;
        if (auto* p = m.get_if<proto::PartialMsg>()) p->seq = seq_;
        if (auto* f = m.get_if<proto::FinalMsg>()) f->seq = seq_;
    }
    if (!sink_.send(m)) {
        std::lock_guard<std::mutex> lock(m_);
        connection_fatal_ = true;
        return false;
    }
    return true;
}

void SessionServer::emit_partial(const std::string& text, uint64_t stable_bytes) {
    proto::PartialMsg p;
    {
        std::lock_guard<std::mutex> lock(m_);
        p.session_id = session_id_;
        p.utterance_id = utterance_id_;
        p.audio_pos_ms = static_cast<int64_t>(audio_pos_ms_locked());
    }
    // `text` is CUMULATIVE for this utterance (docs/protocol.md §8.1). Never a delta.
    p.text = text;
    p.stable_bytes = stable_bytes;
    proto::Message m = proto::Message::of(std::move(p));
    if (emit_text(m)) {
        std::lock_guard<std::mutex> lock(m_);
        last_text_ = text;
        have_last_text_ = true;
        last_stable_ = stable_bytes;
    }
}

void SessionServer::emit_final(const std::string& text, uint64_t stable_bytes) {
    proto::FinalMsg f;
    {
        std::lock_guard<std::mutex> lock(m_);
        f.session_id = session_id_;
        f.utterance_id = utterance_id_;
        f.audio_pos_ms = static_cast<int64_t>(audio_pos_ms_locked());
    }
    f.text = text;
    // Redundant but permitted (§6.2), and it keeps one code path for both message types.
    f.stable_bytes = stable_bytes;
    proto::Message m = proto::Message::of(std::move(f));
    emit_text(m);
}

void SessionServer::send_stopped(const char* reason) {
    std::string sid;
    {
        std::lock_guard<std::mutex> lock(m_);
        if (terminal_) return;
        terminal_ = true;
        sid = session_id_;
    }
    const proto::Message m = stopped_message(sid, reason);
    if (!sink_.send(m)) {
        std::lock_guard<std::mutex> lock(m_);
        connection_fatal_ = true;
    }
}

void SessionServer::fail_session(proto::ErrorCode code, const std::string& msg) {
    // A session-scoped error: abort this session, keep the connection (§11.1, fatal:false).
    // `session_id` is REQUIRED on a non-fatal error.
    //
    // **Discrepancy, reported rather than resolved locally** (docs/protocol.md §14): §11.3 has
    // no code for "the Server detected that its own next message would violate §8.2".
    // `stability_violation` exists but is documented as sent *by the Client* about the Server.
    // `decode_failed` is the closest conforming fit — Server-sent, fatal:false, session-scoped —
    // and it is what this uses, with the violation detail in `msg`. The alternative, sending a
    // message we know to be inadmissible and letting the Client raise `stability_violation`, is
    // worse: by then the offending bytes may already be in the user's document.
    std::string sid;
    {
        std::lock_guard<std::mutex> lock(m_);
        if (terminal_) return;
        terminal_ = true;
        sid = session_id_;
    }
    KD_LOG_ERROR("session failed: %s (%s)", msg.c_str(), proto::to_string(code));
    const proto::Message err = error_message(code, false, msg, &sid);
    const proto::Message stopped = stopped_message(sid, "error");
    if (!sink_.send(err) || !sink_.send(stopped)) {
        std::lock_guard<std::mutex> lock(m_);
        connection_fatal_ = true;
    }
}

void SessionServer::fail_fatal(proto::ErrorCode code, const std::string& msg) {
    // A connection-scoped error (§11.1, fatal:true): the sender writes nothing more. No
    // `stopped` follows — fatal:true means the connection is finished, and §6.2's `stopped` is
    // a session-teardown acknowledgement that has nowhere left to go.
    std::string sid;
    bool have_sid = false;
    {
        std::lock_guard<std::mutex> lock(m_);
        terminal_ = true;
        connection_fatal_ = true;
        have_sid = has_session_;
        sid = session_id_;
    }
    KD_LOG_ERROR("connection failed: %s (%s)", msg.c_str(), proto::to_string(code));
    const proto::Message err = error_message(code, true, msg, have_sid ? &sid : nullptr);
    sink_.send(err);
}

// ─── The decode thread ──────────────────────────────────────────────────────────────────────

void SessionServer::worker() {
    std::unique_lock<std::mutex> lock(m_);
    for (;;) {
        work_cv_.wait(lock, [this] {
            return quit_ || pending_hop_ || pending_finalize_ || pending_stop_ || pending_tick_;
        });
        if (quit_) return;

        const bool finalize = pending_finalize_;
        const bool hop = pending_hop_;
        const bool stop = pending_stop_;
        const bool ticked = pending_tick_;
        pending_hop_ = false;
        pending_finalize_ = false;
        pending_stop_ = false;
        pending_tick_ = false;

        if (cancelled_) {
            // Nothing may follow a `cancel`, including the `stopped` for a `stop` that lost the
            // race with it — on_cancel already sent one.
            idle_cv_.notify_all();
            continue;
        }

        const uint64_t utt = utterance_id_;
        busy_ = true;
        lock.unlock();

        if (stability_.utterance_id() != utt) stability_.reset(utt);

        if (hop || finalize) {
            std::string text;
            bool have = false;
            const Status st = backend_.transcribe(finalize, text, have);

            bool stale = false;
            {
                std::lock_guard<std::mutex> guard(m_);
                stale = terminal_ || cancelled_ || utterance_id_ != utt;
            }
            if (stale) {
                // The utterance moved on, or the session ended, while the POST was in flight.
                // Publishing now would attribute this text to the wrong utterance_id.
                KD_LOG_DEBUG("dropping a decode result for utterance %llu; the session moved on",
                             static_cast<unsigned long long>(utt));
            } else if (!st.ok()) {
                // §11.3: a backend fault with a session open is `decode_failed`, non-fatal —
                // the model misbehaved on this utterance, the process is fine, and the next
                // session may well work.
                fail_session(proto::ErrorCode::decode_failed, st.message());
            } else if (finalize) {
                bool published = false;
                {
                    std::lock_guard<std::mutex> guard(m_);
                    published = have_last_text_;
                }
                if (!have) text.clear();
                if (!text.empty() || cfg_.emit_empty_finals || published) {
                    // `published` means a `partial` already went out under this id, so §6.2's
                    // "exactly one final per utterance_id" obliges us to close it even if the
                    // backend's flush came back empty.
                    uint64_t stable = 0;
                    Violation v;
                    if (!stability_.finalize(text, stable, v)) {
                        fail_session(proto::ErrorCode::decode_failed, v.to_wire_message());
                    } else {
                        emit_final(text, stable);
                    }
                } else {
                    // Nothing was said and nothing was published. Give the id back, so the next
                    // utterance is n rather than n+1: an id the Client never saw and never will
                    // is a hole it would have to explain to itself.
                    std::lock_guard<std::mutex> guard(m_);
                    if (utterance_id_ > 0) --utterance_id_;
                }
            } else if (have) {
                uint64_t stable = 0;
                bool ok = true;
                Violation v;
                if (cfg_.hostile_stability) {
                    // openai-translator.md §4.7. Publishes the whole hypothesis as stable, which
                    // a revising backend contradicts on its very next message. That is the
                    // point: §8.3's client-side handling has to be exercised end to end.
                    stable = text.size();
                    KD_LOG_ERROR(
                        "--hostile-stability: publishing stable_bytes=%llu unchecked. This build "
                        "is deliberately non-conforming and must never be shipped to a user.",
                        static_cast<unsigned long long>(stable));
                } else if (cfg_.publish_stable_partials) {
                    ok = stability_.observe(text, stable, v);
                }
                if (!ok) {
                    fail_session(proto::ErrorCode::decode_failed, v.to_wire_message());
                } else {
                    bool changed = true;
                    {
                        std::lock_guard<std::mutex> guard(m_);
                        changed = !have_last_text_ || text != last_text_ ||
                                  stable != last_stable_;
                    }
                    if (changed) emit_partial(text, stable);
                }
            }

            if (finalize) {
                backend_.reset();
                std::lock_guard<std::mutex> guard(m_);
                stability_.reset(utterance_id_);
                last_text_.clear();
                have_last_text_ = false;
                last_stable_ = 0;
            }
        } else if (ticked && cfg_.publish_stable_partials) {
            uint64_t stable = 0;
            Violation v;
            if (!stability_.poll(stable, v)) {
                fail_session(proto::ErrorCode::decode_failed, v.to_wire_message());
            } else {
                std::string text;
                bool advance = false;
                {
                    std::lock_guard<std::mutex> guard(m_);
                    advance = have_last_text_ && stable > last_stable_;
                    text = last_text_;
                }
                // At most one `partial`, and only when `stable_bytes` actually advanced — an
                // identical re-send would consume a `seq` to say nothing.
                if (advance) emit_partial(text, stable);
            }
        }

        if (stop) send_stopped("stop");

        lock.lock();
        busy_ = false;
        idle_cv_.notify_all();
    }
}

}  // namespace kd::asrd
