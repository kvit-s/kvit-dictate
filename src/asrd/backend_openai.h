// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// The one backend v1 has: `POST {service}/v1/audio/transcriptions`, OpenAI-compatible.
// Port of `server/kvit_asr/backends/openai_http.py` and the half of
// `server/kvit_asr/backends/base.py` that drives an overlapping window over a batch model.
//
// ─── This is a batch model behind a streaming interface, and that is not free ───────────────
//
// The endpoint takes a recording and returns a string. `PseudoStreamingDriver` re-transcribes
// the whole current window every `hop_ms` and emits the result as the cumulative hypothesis.
// While the utterance is shorter than `window_ms` the window starts at the utterance start, so
// the hypothesis is simply "the transcript of everything said so far" and cumulativity is
// exact. Most dictation utterances endpoint well inside an 8-second window and never leave
// that regime.
//
// Past `window_ms` the oldest audio scrolls out and the model can no longer see it. Its
// transcript must be **frozen** into a prefix, because nothing is left to re-derive it from,
// and the freeze point is estimated by word-overlap stitching. base.py documents honestly when
// that estimate is wrong, and the documentation comes across with the code rather than being
// left behind with it:
//
//   * repeated words ("the the", "very very") make the overlap ambiguous and can freeze one
//     word too few or too many;
//   * a model that repunctuates or recases across the boundary produces no exact overlap at
//     all, and the fallback freezes the *entire* pre-slide transcript — conservative, because
//     dropping it would delete words the user actually said;
//   * a revision that would have crossed the boundary is simply lost.
//
// The frozen prefix is **not** "stable" in plan.md §1.4's sense — no stability computation
// blessed it — it is merely unrecoverable. The stability tracker still runs on top and still
// decides what reaches the document.
//
// Finalize-only (the v1 default) sets `hop_ms >= window_ms`, so the window never slides and
// none of the above can happen. That is deliberate: see §4.3's batch-model caveat.
//
// ─── It invents words over silence, measured rather than assumed ────────────────────────────
//
// Measured against this endpoint on 2026-08-21: 500 ms of digital silence returns "Yeah.",
// 2000 ms returns "Okay.", click transients return "Hi.". No phrase blocklist is implemented,
// for the reason whisper_transformers.py already gives — "thank you" and "okay" are perfectly
// ordinary dictated words, and silently deleting text the user said is the failure this project
// is least willing to accept. The fix is upstream: VAD gating means silence never reaches the
// backend, and `vad_gate` adds a second, better-informed opinion at the one moment that matters.
//
// **The gate fails open.** A missing route (503), a timeout, a malformed answer all mean "go
// ahead". Losing something the user actually said is worse than an occasional invented word,
// and a VAD outage must never be able to silence dictation.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "asrd/http.h"
#include "util/result.h"

namespace kd::asrd {

inline constexpr const char* kDefaultModel = "Qwen/Qwen3-ASR-1.7B";
inline constexpr const char* kDefaultLanguage = "en";
inline constexpr const char* kTranscriptionsPath = "/v1/audio/transcriptions";
inline constexpr const char* kVadPath = "/v1/audio/vad";

struct OpenAiBackendConfig {
    std::string base_url;                    // no trailing slash; validated by the caller
    std::string model = kDefaultModel;
    std::string language = kDefaultLanguage;  // empty means "omit the field, server default"
    uint32_t timeout_ms = 30000;
    uint32_t window_ms = 8000;
    uint32_t hop_ms = 1000;
    // openai-translator.md §4.3: the v1 default for a batch model. Makes hop >= window, so the
    // window never slides and no `partial` is ever emitted.
    bool finalize_only = true;
    bool vad_gate = true;
};

// The overlapping-window driver, with no network in it. Split out so the sliding, freezing and
// stitching can be tested without a server, which is where every one of its bugs lives.
class PseudoStreamingDriver {
public:
    PseudoStreamingDriver(uint32_t window_ms, uint32_t hop_ms);

    void reset();

    // Append gated PCM. Returns true when at least one hop is now due.
    bool feed(const int16_t* samples, size_t count);

    // Take the window that should be transcribed next, consuming one hop's worth of pending
    // audio. `slid` reports whether the window dropped its oldest audio, which is what the
    // freeze in commit() keys off. Returns false when there is nothing to transcribe.
    //
    // `force` is finalize: take the window even when no hop is due.
    bool take_window(bool force, std::vector<int16_t>& window, bool& slid);

    // Turn one window transcript into the cumulative hypothesis for the utterance, applying the
    // freeze-on-slide when `slid`.
    std::string commit(const std::string& window_text, bool slid);

    [[nodiscard]] uint64_t audio_ms() const noexcept { return audio_ms_; }
    [[nodiscard]] bool any_audio() const noexcept { return any_audio_; }
    [[nodiscard]] const std::string& frozen_prefix() const noexcept { return frozen_; }
    [[nodiscard]] uint32_t hop_ms() const noexcept { return hop_ms_; }
    [[nodiscard]] uint32_t window_ms() const noexcept { return window_ms_; }

private:
    uint32_t window_ms_;
    uint32_t hop_ms_;
    size_t window_samples_;
    size_t hop_samples_;

    std::vector<int16_t> buf_;
    std::string frozen_;
    std::string prev_window_text_;
    size_t pending_samples_ = 0;
    uint64_t audio_ms_ = 0;
    bool any_audio_ = false;
};

// Estimate which words left the window between two successive transcripts. Exposed for the
// tests, because this heuristic is where the wrapper can commit a wrong prefix.
//
// `prev_text` covers audio [s0, t0]; `new_text` covers [s1, t1] with s1 > s0 and t1 > t0. The
// words that scrolled off are a prefix of `prev_text`: find the largest k such that the last k
// words of `prev_text` equal the first k words of `new_text`. k == 0 freezes all of `prev_text`
// — the conservative branch, which can commit text the next window would have revised, but the
// alternative deletes words the user actually said.
std::string scrolled_off_words(const std::string& prev_text, const std::string& new_text);

// Join two fragments with exactly one space, tolerating empties.
std::string join_words(const std::string& a, const std::string& b);

// The backend itself. One per connection; its HTTP client is what `cancel()` reaches.
class OpenAiBackend {
public:
    explicit OpenAiBackend(OpenAiBackendConfig cfg);
    ~OpenAiBackend();
    OpenAiBackend(const OpenAiBackend&) = delete;
    OpenAiBackend& operator=(const OpenAiBackend&) = delete;

    // "Load" means "prove the endpoint is reachable": a TCP connect, not a transcription.
    //
    // Worth doing rather than skipping — without it the first failure surfaces mid-utterance
    // with the user already talking. It is also what makes `ready` mean something: asrd does
    // not emit `ready` unless this succeeded, so a green Test button in the client's settings
    // means the translator *and* the speech host answered.
    Status load();

    // Start a new utterance: clears the window, the frozen prefix and the audio clock.
    void reset();

    // Append gated PCM to the window. Cheap; no network. Returns true when a hop is due.
    bool feed(const int16_t* samples, size_t count);

    // Transcribe the current window. `force` finalises. On success `have_text` says whether
    // there was anything to transcribe at all; `text` is the cumulative hypothesis.
    //
    // Blocking, and that is why it is called on the decode thread: a round trip against the
    // measured endpoint is 237–541 ms (plus 50–176 ms for the optional VAD gate), and doing
    // that on the connection thread would stall audio intake for half a second at a time and
    // overflow the AUDIO_QUEUE_MAX_MS budget docs/protocol.md §9 makes fatal.
    Status transcribe(bool force, std::string& text, bool& have_text);

    // Abort whatever is in flight, from any thread.
    void cancel();
    void reset_cancel();

    [[nodiscard]] const std::string& model() const noexcept { return cfg_.model; }
    [[nodiscard]] uint64_t audio_ms() const;
    [[nodiscard]] uint32_t hop_ms() const noexcept { return driver_.hop_ms(); }
    [[nodiscard]] bool finalize_only() const noexcept { return cfg_.finalize_only; }
    [[nodiscard]] uint64_t requests() const noexcept { return requests_; }
    [[nodiscard]] uint64_t vad_skipped() const noexcept { return vad_skipped_; }

private:
    // The server's Silero route. Returns true — "go ahead" — on every failure. See the header.
    bool vad_says_speech(const std::vector<int16_t>& pcm);
    Status transcribe_batch(const std::vector<int16_t>& pcm, std::string& out);

    OpenAiBackendConfig cfg_;
    HttpUrl transcribe_url_{};
    HttpUrl vad_url_{};
    HttpClient http_;

    // `feed()` runs on the connection thread and `transcribe()` on the decode thread, so the
    // driver's buffer has two writers. The mutex covers the driver's own state only; the HTTP
    // call deliberately happens *outside* it, which is the whole reason the two threads exist.
    mutable std::mutex driver_mutex_;
    PseudoStreamingDriver driver_;

    bool vad_unavailable_ = false;  // latched after the first refusal, so it is logged once
    uint64_t requests_ = 0;
    uint64_t vad_skipped_ = 0;
    uint64_t empty_responses_ = 0;
};

}  // namespace kd::asrd
