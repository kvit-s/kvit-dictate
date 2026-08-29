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

// ─── OpenAiBackend ──────────────────────────────────────────────────────────────────────────

OpenAiBackend::OpenAiBackend(OpenAiBackendConfig cfg)
    : cfg_(std::move(cfg)),
      // finalize_only turns hops off, so the only transcription is the one the finalize forces:
      // one request per utterance, no live preview.
      driver_(cfg_.finalize_only ? 0 : cfg_.hop_ms) {
    while (!cfg_.base_url.empty() && cfg_.base_url.back() == '/') cfg_.base_url.pop_back();
    parse_http_url(cfg_.base_url + kTranscriptionsPath, transcribe_url_);
    parse_http_url(cfg_.base_url + kVadPath, vad_url_);
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
    KD_LOG_INFO("speech service reachable at %s (model %s, %s)", cfg_.base_url.c_str(),
                cfg_.model.c_str(), cfg_.finalize_only ? "finalize-only" : "live preview");
    return Status::success();
}

void OpenAiBackend::reset() {
    std::lock_guard<std::mutex> lock(driver_mutex_);
    driver_.reset();
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

Status OpenAiBackend::transcribe_batch(const std::vector<int16_t>& pcm, std::string& out) {
    out.clear();
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
    fields.emplace_back("response_format", "json");

    std::string content_type;
    const std::string body =
        encode_multipart(fields, "file", "utterance.wav", "audio/wav",
                         wav_bytes(pcm.data(), pcm.size()), content_type);

    ++requests_;
    const Result<HttpResponse> r = http_.post(transcribe_url_, content_type, body,
                                              cfg_.timeout_ms);
    if (!r) return r.status();
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
    return Status::success();
}

Status OpenAiBackend::transcribe(bool force, std::string& text, bool& have_text) {
    have_text = false;
    std::vector<int16_t> window;
    {
        std::lock_guard<std::mutex> lock(driver_mutex_);
        if (!driver_.take_window(force, window)) return Status::success();
    }

    if (window.size() > kAbsurdAudioSamples && !oversize_warned_) {
        // Nothing is dropped and nothing is stitched — this is a diagnostic, not a policy. The
        // buffer is bounded by `VadConfig::max_utterance_ms` and `connection.cpp` refuses to
        // start without that bound, so getting here means an utterance is not being closed and
        // the POST is about to be enormous. That should be visible in the log rather than felt
        // as a mysteriously slow decode.
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
    // What comes back is the answer for the whole utterance so far, which *is* the cumulative
    // hypothesis. There is no prefix to prepend and no seam to join: see the header.
    const Status st = transcribe_batch(window, text);
    if (!st.ok()) {
        text.clear();
        return st;
    }
    have_text = true;
    return Status::success();
}

}  // namespace kd::asrd
