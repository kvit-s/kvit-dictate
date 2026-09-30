// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// The OpenAI-compatible backend — see backend_openai.h for what pseudo-streaming costs, and for
// why the VAD gate fails open.

#define KD_LOG_MODULE "asrd.backend"

#include "asrd/backend_openai.h"

#include <algorithm>
#include <cctype>

#include "util/json.h"
#include "util/log.h"

namespace kd::asrd {
namespace {

size_t ms_to_samples(uint32_t ms) { return static_cast<size_t>(ms) * 16000u / 1000u; }

uint64_t samples_to_ms(size_t samples) {
    return static_cast<uint64_t>(samples) * 1000u / 16000u;
}

// Two minutes. Not a limit — nothing is ever dropped — just the point past which a buffer says
// more about a broken endpointer than about a talkative user. Absolute rather than derived from
// `VadConfig`, so there is no second number here that can drift out of step with the first.
constexpr size_t kAbsurdAudioSamples = 120u * 16000u;

// Collapse every run of whitespace to one space and trim. Used only to ask whether a segment
// table accounts for the same words as `text`; the committed text itself keeps the model's own
// spacing.
std::string squash_ws(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool pending = false;
    for (const char c : s) {
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            pending = !out.empty();
            continue;
        }
        if (pending) {
            out += ' ';
            pending = false;
        }
        out += c;
    }
    return out;
}

std::string strip(const std::string& s) {
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin])) != 0) ++begin;
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) --end;
    return s.substr(begin, end - begin);
}

}  // namespace

// ─── PseudoStreamingDriver ────────────────────────────────────────────────────────────────

PseudoStreamingDriver::PseudoStreamingDriver(uint32_t hop_ms)
    : hop_ms_(hop_ms), hop_samples_(ms_to_samples(hop_ms)) {}

void PseudoStreamingDriver::reset() {
    buf_.clear();
    pending_samples_ = 0;
    audio_ms_ = 0;
    any_audio_ = false;
}

bool PseudoStreamingDriver::feed(const int16_t* samples, size_t count) {
    if (count == 0) return false;
    buf_.insert(buf_.end(), samples, samples + count);
    pending_samples_ += count;
    audio_ms_ += samples_to_ms(count);
    any_audio_ = true;
    return hop_samples_ != 0 && pending_samples_ >= hop_samples_;
}

bool PseudoStreamingDriver::take_window(bool force, std::vector<int16_t>& window) {
    if (!force && (hop_samples_ == 0 || pending_samples_ < hop_samples_)) return false;
    if (!any_audio_) return false;

    if (hop_samples_ != 0 && pending_samples_ >= hop_samples_) {
        pending_samples_ -= hop_samples_;
    } else {
        pending_samples_ = 0;
    }

    // The whole utterance, from its first sample, every time. `buf_` is never erased from the
    // front and never truncated — that is the invariant the header opens with, and the reason
    // this function cannot produce a duplicated word no matter how long the speaker talks.
    // Bounding the buffer is the endpointer's job, not this one's; trading audio for a bound
    // here is what the stitcher did, and what it cost is on record.
    window = buf_;
    return true;
}

void PseudoStreamingDriver::retain_tail(size_t cut_samples) {
    if (cut_samples == 0) return;
    if (cut_samples >= buf_.size()) {
        // The caller committed everything. Keep the clock honest and start the next stretch
        // empty rather than silently keeping a sample that has already been transcribed.
        buf_.clear();
    } else {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(cut_samples));
    }
    // `audio_ms_` is what the session reports as the utterance's length, so it tracks the
    // buffer rather than everything ever fed. `pending_samples_` is hop bookkeeping and is
    // deliberately untouched: dropping committed audio is not a reason to skip the next hop.
    audio_ms_ = samples_to_ms(buf_.size());
    any_audio_ = !buf_.empty();
}

// ─── OpenAiBackend ──────────────────────────────────────────────────────────────────────────

OpenAiBackend::OpenAiBackend(OpenAiBackendConfig cfg)
    : cfg_(std::move(cfg)),
      // finalize_only turns hops off, so the only transcription is the one the finalize forces:
      // one request per utterance, no live preview.
      driver_(cfg_.finalize_only ? 0 : cfg_.hop_ms) {
    while (!cfg_.base_url.empty() && cfg_.base_url.back() == '/') cfg_.base_url.pop_back();
    parse_http_url(cfg_.base_url + kTranscriptionsPath, transcribe_url_);
    parse_http_url(cfg_.base_url + kVadPath, vad_url_);
    http_.set_bearer_token(cfg_.api_key);
}

OpenAiBackend::~OpenAiBackend() { http_.cancel(); }

Status OpenAiBackend::load() {
    if (transcribe_url_.host.empty()) {
        return Status{StatusCode::InvalidArgument, "--service is not a usable URL: " +
                                                       cfg_.base_url};
    }
    const Status st = HttpClient::probe_tcp(transcribe_url_.host, transcribe_url_.port,
                                            std::min<uint32_t>(cfg_.timeout_ms, 10000));
    if (!st.ok()) {
        return Status{StatusCode::Unavailable,
                      "cannot reach the transcription endpoint at " + cfg_.base_url +
                          " (" + st.message() +
                          "). This translator needs that host to be up; there is no local "
                          "fallback.",
                      st.os_error()};
    }
    if (!cfg_.api_key.empty()) {
        // See the header: a form with no file, so the service can refuse the key without any
        // model being asked anything.
        std::string content_type;
        const std::string body =
            encode_multipart({{"model", cfg_.model}}, {}, {}, {}, {}, content_type);
        const Result<HttpResponse> r = http_.post(transcribe_url_, content_type, body,
                                                  std::min<uint32_t>(cfg_.timeout_ms, 10000));
        if (!r) {
            return Status{StatusCode::Unavailable,
                          "cannot reach the transcription endpoint at " + cfg_.base_url + " (" +
                              r.status().message() + ")",
                          r.status().os_error()};
        }
        if (r->status == 401 || r->status == 403) {
            return Status{StatusCode::Unavailable,
                          "the speech service at " + cfg_.base_url + " refused the API key (HTTP " +
                              std::to_string(r->status) + ": " + r->body.substr(0, 200) + ")"};
        }
    }
    KD_LOG_INFO("speech service reachable at %s (model %s, %s, %s)", cfg_.base_url.c_str(),
                cfg_.model.c_str(), cfg_.finalize_only ? "finalize-only" : "live preview",
                cfg_.api_key.empty() ? "no API key" : "API key accepted");
    return Status::success();
}

void OpenAiBackend::reset() {
    std::lock_guard<std::mutex> lock(driver_mutex_);
    driver_.reset();
}

void OpenAiBackend::set_context(const std::vector<std::string>& terms) {
    size_t dropped = 0;
    prompt_ = join_context_prompt(terms, kMaxPromptBytes, &dropped);
    if (dropped != 0) {
        KD_LOG_WARN("context: %zu of %zu vocabulary terms did not fit in the endpoint's %zu-byte "
                    "prompt and were not sent",
                    dropped, terms.size(), kMaxPromptBytes);
    }
    KD_LOG_INFO("context: %zu terms, %zu bytes on the wire", terms.size() - dropped,
                prompt_.size());
}

bool OpenAiBackend::feed(const int16_t* samples, size_t count) {
    std::lock_guard<std::mutex> lock(driver_mutex_);
    return driver_.feed(samples, count);
}

uint64_t OpenAiBackend::audio_ms() const {
    std::lock_guard<std::mutex> lock(driver_mutex_);
    return driver_.audio_ms();
}

void OpenAiBackend::cancel() { http_.cancel(); }

void OpenAiBackend::reset_cancel() { http_.reset_cancel(); }

bool OpenAiBackend::vad_says_speech(const std::vector<int16_t>& pcm) {
    if (vad_url_.host.empty()) return true;
    std::string content_type;
    const std::string body =
        encode_multipart({}, "file", "clip.wav", "audio/wav",
                         wav_bytes(pcm.data(), pcm.size()), content_type);

    const Result<HttpResponse> r = http_.post(vad_url_, content_type, body, cfg_.timeout_ms);
    const auto go_ahead = [&](const std::string& why) {
        if (!vad_unavailable_) {
            vad_unavailable_ = true;
            KD_LOG_WARN(
                "VAD gate unavailable (%s); transcribing without it. Non-speech may come back as "
                "invented words — this endpoint answers 500 ms of silence with \"Yeah.\". "
                "Logged once per connection.",
                why.c_str());
        }
        return true;
    };

    if (!r) return go_ahead(r.status().message());
    if (r->status != 200) return go_ahead("HTTP " + std::to_string(r->status));

    kd::json::Value doc;
    kd::json::Error err;
    kd::json::Options opts;
    opts.max_bytes = 1u << 20;
    if (!kd::json::parse_object(r->body, doc, err, opts)) {
        return go_ahead(std::string("response is not a JSON object: ") +
                        kd::json::to_string(err.code));
    }
    const kd::json::Value* speech = doc.find("speech");
    bool value = true;
    if (speech == nullptr || !speech->get_bool(value)) {
        return go_ahead("response has no boolean 'speech' field");
    }
    return value;
}

Status OpenAiBackend::transcribe_batch(const std::vector<int16_t>& pcm, std::string& out,
                                       std::vector<Segment>& segments) {
    out.clear();
    segments.clear();
    if (pcm.empty()) return Status::success();

    if (cfg_.vad_gate && !vad_unavailable_ && !vad_says_speech(pcm)) {
        // Nothing was said. An empty string is not a failure and must not look like one: the
        // driver treats it as "this window produced no hypothesis", which is exactly right.
        ++vad_skipped_;
        return Status::success();
    }

    std::vector<std::pair<std::string, std::string>> fields;
    fields.emplace_back("model", cfg_.model);
    if (!cfg_.language.empty()) fields.emplace_back("language", cfg_.language);
    // `verbose_json` is the same answer with a segment table attached, and every field `json`
    // returns is still there. A service that does not implement it either ignores the value or
    // returns `text` alone, and both are handled: `segments` simply stays empty and no cut is
    // proposed. Nothing downstream requires it.
    fields.emplace_back("response_format", cfg_.segment_cuts ? "verbose_json" : "json");
    // Vocabulary hints. The endpoint takes them as `prompt`; `context` is a synonym there, and
    // `hotwords` and `initial_prompt` are silently ignored, so the name matters — measured
    // 2026-09-20 against both the Qwen and the R2T2 host. Omitted entirely when empty rather
    // than sent blank, because an empty prompt is a field the server still has to interpret.
    if (!prompt_.empty()) fields.emplace_back("prompt", prompt_);

    std::string content_type;
    const std::string body =
        encode_multipart(fields, "file", "utterance.wav", "audio/wav",
                         wav_bytes(pcm.data(), pcm.size()), content_type);

    ++requests_;
    const Result<HttpResponse> r = http_.post(transcribe_url_, content_type, body,
                                              cfg_.timeout_ms);
    if (!r) return r.status();
    if (r->status == 401 || r->status == 403) {
        // Said in words, because "HTTP 401" is a support question and "the key" is a settings
        // change. `load` checked the key once; this is the key being revoked, or running out of
        // credit, since.
        return Status{StatusCode::Unavailable,
                      std::string(cfg_.api_key.empty()
                                      ? "the speech service wants an API key and none is set"
                                      : "the speech service refused the API key") +
                          " (HTTP " + std::to_string(r->status) + ": " + r->body.substr(0, 400) +
                          ")"};
    }
    if (r->status != 200) {
        return Status{StatusCode::Unavailable,
                      "transcription endpoint returned HTTP " + std::to_string(r->status) + ": " +
                          r->body.substr(0, 400)};
    }

    kd::json::Value doc;
    kd::json::Error err;
    kd::json::Options opts;
    opts.max_bytes = 1u << 20;
    if (!kd::json::parse_object(r->body, doc, err, opts)) {
        return Status{StatusCode::Malformed,
                      "transcription endpoint returned " + std::to_string(r->body.size()) +
                          " bytes that are not a JSON object (" +
                          kd::json::to_string(err.code) + ")"};
    }
    const kd::json::Value* text = doc.find("text");
    std::string_view sv;
    if (text == nullptr || !text->get_string(sv)) {
        return Status{StatusCode::Malformed,
                      "transcription response has no string 'text' field"};
    }
    out = strip(std::string(sv));
    // An empty string is a legitimate answer — silence, or audio below the model's threshold —
    // and must not be treated as an error. Counted so a session that produced nothing at all can
    // be told apart from one that was never asked.
    if (out.empty()) ++empty_responses_;

    // The segment table, when there is one. Absent, malformed or half-typed entries are not an
    // error: they cost a cut, not a transcription, and the transcription is the part the user
    // is waiting for.
    const kd::json::Value* segs = doc.find("segments");
    if (segs != nullptr && segs->is_array()) {
        for (size_t i = 0; i < segs->size(); ++i) {
            const kd::json::Value* e = segs->at(i);
            if (e == nullptr || !e->is_object()) continue;
            const kd::json::Value* vs = e->find("start");
            const kd::json::Value* ve = e->find("end");
            const kd::json::Value* vt = e->find("text");
            Segment seg;
            std::string_view st_sv;
            if (vs == nullptr || !vs->get_double(seg.start_s)) continue;
            if (ve == nullptr || !ve->get_double(seg.end_s)) continue;
            if (vt == nullptr || !vt->get_string(st_sv)) continue;
            seg.text = strip(std::string(st_sv));
            segments.push_back(std::move(seg));
        }
    }
    return Status::success();
}

std::string join_context_prompt(const std::vector<std::string>& terms, size_t max_bytes,
                                size_t* dropped) {
    static constexpr std::string_view kSep = ", ";
    std::string out;
    size_t skipped = 0;
    for (const std::string& term : terms) {
        const std::string t = strip(term);
        if (t.empty()) continue;  // an empty term biases nothing and costs two separator bytes
        const size_t want = out.empty() ? t.size() : out.size() + kSep.size() + t.size();
        if (want > max_bytes) {
            // Keep going rather than stopping: a short term later in the list still fits, and
            // the user wrote it down for a reason. Order is not priority here.
            ++skipped;
            continue;
        }
        if (!out.empty()) out += kSep;
        out += t;
    }
    if (dropped != nullptr) *dropped = skipped;
    return out;
}

bool propose_segment_cut(const OpenAiBackendConfig& cfg, size_t buffered_samples,
                         const std::string& text, const std::vector<Segment>& segments,
                         ProposedCut& out) {
    out = {};
    // Nothing to commit unless the model itself drew a line inside this recording.
    if (segments.size() < 2) return false;

    // The table must be a partition of the recording, in order, with no negative spans. A table
    // that is not is a table whose `end` cannot be trusted as a sample offset.
    double prev_end = -1.0;
    for (const Segment& s : segments) {
        if (!(s.end_s >= s.start_s)) return false;
        if (!(s.start_s >= prev_end - 0.001)) return false;
        prev_end = s.end_s;
    }

    // ...and it must account for the same words as `text`. If the segments say something the
    // transcript does not, committing part of them commits text the user did not hear the model
    // say. This is the check that makes the rest of it safe rather than merely plausible.
    std::string joined;
    for (const Segment& s : segments) {
        if (s.text.empty()) continue;
        if (!joined.empty()) joined += ' ';
        joined += s.text;
    }
    if (squash_ws(joined) != squash_ws(text)) return false;

    // Commit all but the last segment: the last is the one the model is still revising, which is
    // the whole premise. Its `start` is the cut, and is also the previous segment's `end`.
    const size_t keep_from = segments.size() - 1;
    const double cut_s = segments[keep_from].start_s;
    if (!(cut_s > 0.0)) return false;

    const size_t cut_samples = static_cast<size_t>(cut_s * 16000.0 + 0.5);
    if (cut_samples == 0 || cut_samples >= buffered_samples) return false;
    if (cut_samples < ms_to_samples(cfg.min_cut_head_ms)) return false;
    if (buffered_samples - cut_samples < ms_to_samples(cfg.min_cut_tail_ms)) return false;

    std::string committed;
    for (size_t i = 0; i < keep_from; ++i) {
        if (segments[i].text.empty()) continue;
        if (!committed.empty()) committed += ' ';
        committed += segments[i].text;
    }
    if (committed.empty()) return false;

    out.cut_samples = cut_samples;
    out.committed_text = std::move(committed);
    return true;
}

Status OpenAiBackend::transcribe(bool force, Transcription& out) {
    out = {};
    std::vector<int16_t> window;
    {
        std::lock_guard<std::mutex> lock(driver_mutex_);
        if (!driver_.take_window(force, window)) return Status::success();
    }

    // Before the POST, because the point of keeping it is to have the input that produced the
    // output — and `window` is moved from by nothing below, so this is the same buffer.
    if (capture_window_) last_window_ = window;

    if (window.size() > kAbsurdAudioSamples && !oversize_warned_) {
        // Nothing is dropped and nothing is stitched — this is a diagnostic, not a policy. The
        // buffer is bounded by `VadConfig`'s soft cut and its stopwatch, and `connection.cpp`
        // refuses to start without the latter, so getting here means an utterance is not being
        // closed and the POST is about to be enormous. That should be visible in the log rather
        // than felt as a mysteriously slow decode.
        oversize_warned_ = true;
        KD_LOG_WARN(
            "the utterance buffer has reached %llu ms, past any plausible dictation; the "
            "endpointer is not closing utterances. Nothing is dropped. Logged once per "
            "connection.",
            static_cast<unsigned long long>(samples_to_ms(window.size())));
    }

    // The HTTP round trip is deliberately outside the lock: it is 237-541 ms, and holding the
    // driver's mutex across it would block the connection thread's feed() for exactly as long
    // as the decode thread exists to avoid.
    //
    // What comes back is the answer for the whole buffer, which *is* the cumulative hypothesis.
    // There is no prefix to prepend and no seam to join: see the header.
    std::vector<Segment> segments;
    const Status st = transcribe_batch(window, out.text, segments);
    if (!st.ok()) {
        out.text.clear();
        return st;
    }
    out.have_text = true;

    // A finalize is about to end the utterance anyway, so a cut would buy nothing and would put
    // two `final`s where the protocol expects one.
    if (!force && cfg_.segment_cuts && !out.text.empty()) {
        if (segments.empty() && !no_segments_warned_) {
            no_segments_warned_ = true;
            KD_LOG_INFO(
                "the service returned no segment table for verbose_json, so the utterance will "
                "be committed whole and bounded by the endpointer instead. Not an error. "
                "Logged once per connection.");
        }
        out.have_cut = propose_segment_cut(cfg_, window.size(), out.text, segments, out.cut);
    }
    return Status::success();
}

void OpenAiBackend::retain_tail(size_t cut_samples) {
    std::lock_guard<std::mutex> lock(driver_mutex_);
    driver_.retain_tail(cut_samples);
    ++segment_cuts_;
}

}  // namespace kd::asrd
