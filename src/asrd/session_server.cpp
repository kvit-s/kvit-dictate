// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// The session loop — see session_server.h for the state machine, the two threads, and why
// there is no `overload_audio` here.

#define KD_LOG_MODULE "asrd.sess"

#include "asrd/session_server.h"

#include <algorithm>

#include <cctype>
#include <fstream>
#include "util/json.h"
#include "util/log.h"
#include "util/wav.h"

namespace kd::asrd {
namespace {

constexpr uint32_t kBytesPerSample = 2;

// A bound on how long the connection thread will wait for a decode already in flight before
// starting the next utterance. Generous, because the thing being waited for is one HTTP request
// whose own timeout is shorter; if this ever expires, the decode thread is wedged and the
// utterance boundary is the least of the problems.
constexpr auto kIdleWait = std::chrono::seconds(60);

// ─── What a failed round trip costs, and therefore what it is worth retrying ───────────────
//
// A **hop** is a preview. Its audio is still in the backend's buffer — `PseudoStreamingDriver`
// never erases from the front — so the next hop re-POSTs the same recording plus whatever
// arrived since, about a second later. Retrying one immediately would put a second request on a
// service that has just shown it is struggling, to buy a preview that is about to be superseded
// anyway. So: no retry, skip it, say so once.
//
// A **flush** is the utterance. Nothing comes after it to cover for it, so this is the round
// trip worth paying for. Three attempts a quarter-second apart is short next to the finalize
// grace on the Client (5 s) and long enough to outlast a connection reset.
constexpr uint32_t kFlushAttempts = 3;
constexpr auto kFlushRetryPause = std::chrono::milliseconds(250);

// When the service really is gone, ending the session beats leaving the user talking into
// nothing: their buffer reaches the document instead of being carried by a session that can no
// longer transcribe. Eight consecutive failures is several seconds of hops — far past a reset,
// nowhere near a pause for breath.
constexpr uint32_t kMaxDecodeFailures = 8;

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
    // `context` goes to the backend as the endpoint's `prompt` field, which is what
    // context_supported = true in `ready` now promises. Until 2026-09-20 this was accepted and
    // dropped, correctly declared unsupported — but the endpoint had honoured `prompt` all
    // along, so the terms the user maintained in Settings bought them nothing.
    //
    // Set before `backend_.reset()` so the first hop of the first utterance already carries it.
    backend_.set_context(m.context);
    vad_.reset();
    backend_.reset();
    backend_.reset_cancel();
    stability_.reset(0);
    KD_LOG_INFO("session_start session_id=%s context=%zu prompt_bytes=%zu",
                m.session_id.c_str(), m.context.size(), backend_.context_prompt().size());
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
        // Finalize-only: the hop cannot fire, because the driver is built with hops off.
        // Reaching here would mean the driver and the mode disagree.
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

// Commit the settled head of an utterance that is still being spoken, and carry the rest over.
//
// Called on the decode thread with the utterance still open and the speaker still talking, which
// is what makes it different from every other boundary here: nothing external happened. The
// model reported, in the same response as the text, that the recording contains a boundary it is
// confident about, and `propose_cut` established that `cut.committed_text` is the transcript of
// exactly the first `cut.cut_samples` samples.
//
// So this is an ordinary utterance boundary in every respect the protocol can see — one `final`
// for the old id (§6.2), the id incremented by exactly one (§7.3), the tracker reset (§1.4 step 7)
// — and the only unusual part is that the next utterance begins with audio already in hand
// rather than with silence. No sample is decoded twice and none is dropped, which is the
// property that the sliding window could never offer and that this whole design exists to keep.
//
// Returns false when the session moved on underneath us, in which case nothing was emitted.
bool SessionServer::split_utterance(const ProposedCut& cut, uint64_t utt) {
    {
        std::lock_guard<std::mutex> lock(m_);
        if (terminal_ || cancelled_ || !utterance_open_ || utterance_id_ != utt) return false;
    }

    uint64_t stable = 0;
    Violation v;
    if (!stability_.finalize(cut.committed_text, stable, v)) {
        fail_session(proto::ErrorCode::decode_failed, v.to_wire_message());
        return false;
    }
    emit_final(cut.committed_text, stable);

    // Order matters: the audio goes first, so that a `feed` racing in from the connection thread
    // lands in the new utterance's buffer rather than being counted against the committed head.
    backend_.retain_tail(cut.cut_samples);

    std::lock_guard<std::mutex> lock(m_);
    ++utterance_id_;
    stability_.reset(utterance_id_);
    last_text_.clear();
    have_last_text_ = false;
    last_stable_ = 0;
    KD_LOG_INFO(
        "committed %llu ms of utterance %llu at the model's own segment boundary and carried "
        "the rest into utterance %llu",
        static_cast<unsigned long long>(static_cast<uint64_t>(cut.cut_samples) * 1000u / 16000u),
        static_cast<unsigned long long>(utt), static_cast<unsigned long long>(utterance_id_));
    return true;
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

// ─── When a hypothesis has taken something back ─────────────────────────────────────────────

bool keeps_published_words(std::string_view prev, std::string_view next) {
    // Tokenise on anything that is not a letter or a digit, lowercasing as we go. That folds away
    // exactly the two things this backend revises harmlessly — case and punctuation — and nothing
    // else. An apostrophe is a separator too, so "I'm" is {"i","m"}: a contraction the model later
    // spells out is not a retraction, and one it swallows into a different word is.
    const auto tokens = [](std::string_view v) {
        std::vector<std::string> out;
        std::string cur;
        for (const char c : v) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (std::isalnum(u)) {
                cur.push_back(static_cast<char>(std::tolower(u)));
            } else if (!cur.empty()) {
                out.push_back(std::move(cur));
                cur.clear();
            }
        }
        if (!cur.empty()) out.push_back(std::move(cur));
        return out;
    };

    const std::vector<std::string> a = tokens(prev);
    const std::vector<std::string> b = tokens(next);
    if (a.size() > b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin());
}

void SessionServer::capture_revision(const std::string& text, uint64_t utt, uint64_t seq) {
    if (cfg_.revision_dir.empty()) return;

    std::string prev;
    {
        std::lock_guard<std::mutex> lock(m_);
        if (!have_last_text_) return;
        prev = last_text_;
    }
    if (prev.empty() || keeps_published_words(prev, text)) return;

    const std::vector<int16_t>& window = backend_.last_window();
    if (window.empty()) return;

    std::error_code ec;
    std::filesystem::create_directories(cfg_.revision_dir, ec);
    if (ec) {
        KD_LOG_WARN("could not create %s for a revision capture: %s",
                    cfg_.revision_dir.string().c_str(), ec.message().c_str());
        return;
    }

    std::string sid;
    {
        std::lock_guard<std::mutex> lock(m_);
        sid = session_id_;
    }
    char stem[128];
    std::snprintf(stem, sizeof(stem), "%s-u%llu-s%llu", sid.c_str(),
                  static_cast<unsigned long long>(utt), static_cast<unsigned long long>(seq));

    // The audio first: if only one of the pair survives a full disk, the recording is the half
    // that cannot be reconstructed from anywhere else.
    const std::filesystem::path wav = cfg_.revision_dir / (std::string(stem) + ".wav");
    if (!wav::write_pcm16(wav, window.data(), window.size())) {
        KD_LOG_WARN("could not write %s", wav.string().c_str());
        return;
    }

    json::Value m = json::Value::object();
    m.set("session", json::Value::string(sid));
    m.set("utterance_id", json::Value::number(static_cast<int64_t>(utt)));
    m.set("seq", json::Value::number(static_cast<int64_t>(seq)));
    m.set("audio_samples", json::Value::number(static_cast<int64_t>(window.size())));
    m.set("audio_ms", json::Value::number(static_cast<int64_t>(window.size() * 1000u / 16000u)));
    // Both texts, because the diff is the finding: what the user had already read, and what
    // replaced it. A capture with only the second could not show what was taken away.
    m.set("previous", json::Value::string(prev));
    m.set("revised", json::Value::string(text));

    std::ofstream meta(cfg_.revision_dir / (std::string(stem) + ".json"),
                       std::ios::binary | std::ios::trunc);
    if (meta) meta << json::serialize(m) << "\n";

    KD_LOG_WARN("a hypothesis retracted published words; wrote %s (%llu ms of audio)",
                wav.filename().string().c_str(),
                static_cast<unsigned long long>(window.size() * 1000u / 16000u));
}

// ─── Decoding, and surviving a decode that does not come back ──────────────────────────────

Status SessionServer::decode_once(bool finalize, Transcription& out) {
    const uint32_t attempts = finalize ? kFlushAttempts : 1u;
    Status st = Status::success();
    for (uint32_t attempt = 1; attempt <= attempts; ++attempt) {
        st = backend_.transcribe(finalize, out);
        if (st.ok()) {
            decode_failures_ = 0;
            return st;
        }
        {
            // A `cancel` closes the in-flight request handle, so the error we are holding may
            // simply be that. Retrying it would re-POST audio the user asked to throw away.
            std::lock_guard<std::mutex> guard(m_);
            if (terminal_ || cancelled_) return st;
        }
        if (attempt < attempts) {
            KD_LOG_WARN("the flush did not come back (%s); retrying, attempt %u of %u",
                        st.message().c_str(), attempt + 1, attempts);
            std::unique_lock<std::mutex> lock(m_);
            // Waiting on the condition variable rather than sleeping: a `cancel` or a teardown
            // arriving during the pause must not sit out the whole quarter second.
            work_cv_.wait_for(lock, kFlushRetryPause, [this] { return quit_ || cancelled_ || terminal_; });
            if (quit_ || cancelled_ || terminal_) return st;
        }
    }
    return st;
}

bool SessionServer::on_decode_failure(bool finalize, bool published, const Status& st) {
    ++decode_failures_;

    // The one failure that is not survivable: the utterance is closed, its flush did not come
    // back after every attempt, and no `partial` ever went out under this id — so there is
    // nothing on the Client naming what was said, and nothing here to reconstruct it from. That
    // is finalize-only's every utterance and live preview's first second, and in both the words
    // are simply gone. Saying so is the only honest move; continuing would silently drop speech,
    // which is the failure this project is least willing to accept.
    if (finalize && !published) {
        KD_LOG_ERROR("the flush failed (%s) and no partial was ever published for this "
                     "utterance, so nothing names what was said; failing the session",
                     st.message().c_str());
        return true;
    }

    // Everything else is survivable, and staying alive is worth something: under
    // `Cadence::EndOfSession` the Client turns a `stopped` into a whole session's buffer landing
    // in the document, so a session that ends on a blip ends mid-sentence.
    if (decode_failures_ >= kMaxDecodeFailures) {
        KD_LOG_ERROR("%u decodes in a row failed (last: %s); ending the session so the Client can "
                     "put what it has into the document",
                     decode_failures_, st.message().c_str());
        return true;
    }
    KD_LOG_WARN("%s failed (%s); the session continues — %u consecutive, %u ends it",
                finalize ? "a flush" : "a preview hop", st.message().c_str(), decode_failures_,
                kMaxDecodeFailures);
    return false;
}

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
            Transcription tr;
            const Status st = decode_once(finalize, tr);
            std::string text = tr.text;
            const bool have = tr.have_text;

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
                // A failed round trip is not, by itself, a failed session. See
                // on_decode_failure() for which ones are.
                std::string fallback;
                bool published = false;
                {
                    std::lock_guard<std::mutex> guard(m_);
                    published = have_last_text_;
                    fallback = last_text_;
                }
                if (on_decode_failure(finalize, published, st)) {
                    fail_session(proto::ErrorCode::decode_failed, st.message());
                } else if (finalize) {
                    // The utterance still owes the Client exactly one `final` (§6.2), because a
                    // `partial` went out under this id — that is what `published` said. The last
                    // hypothesis the user actually watched arrive in the pill is the honest
                    // thing to close it with; dropping it would delete words they had read.
                    uint64_t stable = 0;
                    Violation v;
                    if (stability_.finalize(fallback, stable, v)) {
                        KD_LOG_WARN(
                            "the flush for utterance %llu did not come back; closing it with the "
                            "last hypothesis the Client already has (%zu bytes)",
                            static_cast<unsigned long long>(utt), fallback.size());
                        emit_final(fallback, stable);
                    } else {
                        fail_session(proto::ErrorCode::decode_failed, v.to_wire_message());
                    }
                }
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
                    // Before the emit, while `last_text_` is still the hypothesis the user has
                    // and `backend_.last_window()` is still the audio that produced this one.
                    if (changed) capture_revision(text, utt, seq_ + 1);
                    if (changed) emit_partial(text, stable);
                }

                // The settled head, committed at a boundary the model drew itself. Done after
                // the `partial` so the client sees the hypothesis it is about to have confirmed,
                // and only when the hypothesis was publishable in the first place.
                if (tr.have_cut) split_utterance(tr.cut, utt);
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
