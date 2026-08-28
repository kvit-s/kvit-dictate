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

std::string rstrip(const std::string& s) {
    size_t end = s.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) --end;
    return s.substr(0, end);
}

std::string strip(const std::string& s) {
    size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin])) != 0) ++begin;
    size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) --end;
    return s.substr(begin, end - begin);
}

std::vector<std::string> split_words(const std::string& s) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < s.size()) {
        while (at < s.size() && std::isspace(static_cast<unsigned char>(s[at])) != 0) ++at;
        const size_t begin = at;
        while (at < s.size() && std::isspace(static_cast<unsigned char>(s[at])) == 0) ++at;
        if (at > begin) out.emplace_back(s, begin, at - begin);
    }
    return out;
}

// Comparison key for stitching: case- and edge-punctuation-insensitive.
//
// Deliberately crude, and copied crude. A model that recases or repunctuates across a window
// boundary should still stitch, but nothing here tries to be clever about it — an over-clever
// matcher would silently freeze the wrong words, and a crude one that fails falls back to the
// conservative branch.
std::string norm_word(const std::string& w) {
    static constexpr const char* kEdge = ".,;:!?\"'()[]{}\xE2\x80\xA6";  // … in UTF-8
    size_t begin = 0;
    size_t end = w.size();
    const auto is_edge = [](char c) {
        for (const char* p = kEdge; *p != '\0'; ++p) {
            if (*p == c) return true;
        }
        return false;
    };
    while (begin < end && is_edge(w[begin])) ++begin;
    while (end > begin && is_edge(w[end - 1])) --end;
    std::string out = w.substr(begin, end - begin);
    // ASCII-only fold, for the same reason normalizer.cpp gives: a locale-aware fold introduces
    // the Turkish dotless-i problem into a comparison whose whole job is to be predictable.
    for (char& c : out) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x80) c = static_cast<char>(std::tolower(u));
    }
    return out;
}

}  // namespace

std::string join_words(const std::string& a, const std::string& b) {
    const std::string left = rstrip(a);
    const std::string right = strip(b);
    if (left.empty()) return right;
    if (right.empty()) return left;
    return left + " " + right;
}

std::string scrolled_off_words(const std::string& prev_text, const std::string& new_text) {
    const std::vector<std::string> pw = split_words(prev_text);
    const std::vector<std::string> nw = split_words(new_text);
    if (pw.empty()) return {};

    std::vector<std::string> pn;
    std::vector<std::string> nn;
    pn.reserve(pw.size());
    nn.reserve(nw.size());
    for (const std::string& w : pw) pn.push_back(norm_word(w));
    for (const std::string& w : nw) nn.push_back(norm_word(w));

    const size_t limit = std::min(pn.size(), nn.size());
    const auto join_prefix = [&](size_t upto) {
        std::string out;
        for (size_t i = 0; i < upto; ++i) {
            if (i != 0) out += ' ';
            out += pw[i];
        }
        return out;
    };
    for (size_t k = limit; k >= 1; --k) {
        if (std::equal(pn.end() - static_cast<ptrdiff_t>(k), pn.end(), nn.begin())) {
            return join_prefix(pn.size() - k);
        }
    }
    // No overlap found. Freeze everything, which can commit text the next window would have
    // revised — but the alternative deletes words the user actually said, and losing text is
    // worse than freezing it early.
    return join_prefix(pw.size());
}

// ─── PseudoStreamingDriver ──────────────────────────────────────────────────────────────────

PseudoStreamingDriver::PseudoStreamingDriver(uint32_t window_ms, uint32_t hop_ms)
    : window_ms_(window_ms),
      hop_ms_(hop_ms == 0 ? 1 : hop_ms),
      window_samples_(ms_to_samples(window_ms)),
      hop_samples_(ms_to_samples(hop_ms == 0 ? 1 : hop_ms)) {}

void PseudoStreamingDriver::reset() {
    buf_.clear();
    frozen_.clear();
    prev_window_text_.clear();
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
    return pending_samples_ >= hop_samples_;
}

bool PseudoStreamingDriver::take_window(bool force, std::vector<int16_t>& window, bool& slid) {
    if (!force && pending_samples_ < hop_samples_) return false;
    if (!any_audio_) return false;

    if (pending_samples_ >= hop_samples_) {
        pending_samples_ -= hop_samples_;
    } else {
        pending_samples_ = 0;
    }

    // ── The slide, and the condition that must hold before it is allowed ────────────────────
    //
    // Dropping audio out of the window is only survivable because `commit()` freezes its
    // transcript first, and it can only do that by stitching the pre-slide transcript against
    // the post-slide one. **With no pre-slide transcript there is nothing to stitch**, so
    // `scrolled_off_words("", text)` freezes the empty string and the audio is simply gone.
    //
    // That is not hypothetical: it is what finalize-only did. With hop >= window no intermediate
    // hop ever runs, so `prev_window_text_` is always empty — and every utterance longer than
    // `window_ms` silently lost everything before its last window. openai-translator.md §4.5
    // asserts "finalize-only makes hop_ms >= window_ms, so the window never slides", and that
    // is false: it stops *hops* from firing, but the finalize still forces a take.
    //
    // Reported from real use as "a chunk of dictation was never captured", which is exactly what
    // it looks like from the outside and exactly the failure this project says it will not have.
    //
    // So the window slides only when there is a transcript to freeze. The buffer is then bounded
    // by `VadConfig::max_utterance_ms` instead — a cut utterance, logged, rather than a silent
    // hole in the middle of one.
    slid = buf_.size() > window_samples_ && !prev_window_text_.empty();
    if (slid) {
        // Keep the newest window_samples_. Everything older is gone from the model's view, and
        // commit() is about to freeze its words.
        buf_.erase(buf_.begin(),
                   buf_.begin() + static_cast<ptrdiff_t>(buf_.size() - window_samples_));
    }
    window = buf_;
    return true;
}

std::string PseudoStreamingDriver::commit(const std::string& window_text, bool slid) {
    if (slid) {
        frozen_ = join_words(frozen_, scrolled_off_words(prev_window_text_, window_text));
    }
    prev_window_text_ = window_text;
    return join_words(frozen_, window_text);
}

// ─── OpenAiBackend ──────────────────────────────────────────────────────────────────────────

OpenAiBackend::OpenAiBackend(OpenAiBackendConfig cfg)
    : cfg_(std::move(cfg)),
      driver_(cfg_.window_ms,
              // finalize_only turns the hop off entirely by making it at least as long as any
              // window can be, so the only transcription is the one finalize forces: one
              // request per utterance, no live preview.
              cfg_.finalize_only ? std::max(cfg_.hop_ms, cfg_.window_ms) : cfg_.hop_ms) {
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
    bool slid = false;
    {
        std::lock_guard<std::mutex> lock(driver_mutex_);
        if (!driver_.take_window(force, window, slid)) return Status::success();
    }

    // The HTTP round trip is deliberately outside the lock: it is 237-541 ms, and holding the
    // driver's mutex across it would block the connection thread's feed() for exactly as long
    // as the decode thread exists to avoid.
    std::string window_text;
    const Status st = transcribe_batch(window, window_text);
    if (!st.ok()) return st;

    {
        std::lock_guard<std::mutex> lock(driver_mutex_);
        text = driver_.commit(window_text, slid);
    }
    have_text = true;
    return Status::success();
}

}  // namespace kd::asrd
