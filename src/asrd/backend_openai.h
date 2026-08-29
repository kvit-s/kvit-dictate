// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// The one backend v1 has: `POST {service}/v1/audio/transcriptions`, OpenAI-compatible.
// Port of `server/kvit_asr/backends/openai_http.py`, and of the half of
// `server/kvit_asr/backends/base.py` that drives a batch model on a timer — but not of its
// overlapping window, which is deliberately not here. See below.
//
// ─── This is a batch model behind a streaming interface, and there is no window ─────────────
//
// The endpoint takes a recording and returns a string. `PseudoStreamingDriver` buffers the
// utterance and re-transcribes **all of it** every `hop_ms`. The transcript of that recording is
// the cumulative hypothesis, exactly, with nothing computed on top of it: the preview and the
// `final` are the same operation over the same audio, differing only in when they run.
//
// ─── Why there is no sliding window, and no stitcher ────────────────────────────────────────
//
// There used to be both. A window of `window_ms` bounded the decode cost of an unbounded
// utterance; past it the oldest audio scrolled out, and because nothing was left to re-derive
// that audio's transcript from, its words had to be **frozen** into a prefix. The freeze point
// was estimated by word-overlap stitching: the largest k such that the last k words of the
// pre-slide transcript equal the first k words of the post-slide one.
//
// That estimate has exactly one failure mode, and it is not a small one. The match needs an
// exact run of words. A model that revises as it hears more — which is what every batch model
// does as it reaches the end of a sentence: `give` becomes `gives`, a comma appears, a word is
// dropped — breaks the run, no k matches, and the fallback freezes the **entire** pre-slide
// transcript while the new window still holds almost all of the same audio. The result is the
// same speech twice, joined at a visible seam, each copy slightly reworded.
//
// The other branch is worse: freezing nothing deletes words the user actually said. So the
// design's own choice, once its guess failed, was between duplicating text and losing it.
//
// This was reported from real use three times. Twice it was addressed by making the window big
// enough that it could not slide — first `8000` → `max_utterance_ms`, then a 300 ms pre-roll
// correction on top of that — and each time the duplication was one arithmetic slip away from
// coming back, because a heuristic whose only failure mode is corrupting the document was still
// sitting in the path, waiting for the buffer to cross a number.
//
// So it is gone. **The driver never drops audio.** Every take hands over the whole utterance
// from its first sample, so there is no scrolled-off transcript, nothing to freeze, nothing to
// stitch, and no arrangement of hop and utterance length that can duplicate a word. What this
// backend returns is what the model said about one recording.
//
// What the window bought was bounded decode cost, and that bound now comes from where it always
// should have: the endpointer. `VadConfig::max_utterance_ms` cuts an utterance at 30 s — a cut
// the user can see in their text, rather than a stitch they cannot. That bound is **required**,
// not assumed: `connection.cpp` refuses a connection whose endpointer leaves an utterance
// unbounded, because this driver would then buffer it forever. A missing bound is a startup
// failure with a message, not a slow surprise mid-word.
//
// The cost is a POST that grows with the utterance, and it is self-limiting: only one request is
// in flight at a time (§2.1), so a slower decode means fewer previews, never a queue.
//
// Finalize-only (the v1 default) sets `hop_ms = 0`, which turns hops off entirely: one request
// per utterance, no live preview. See §4.3's batch-model caveat.
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
    // How often the utterance so far is re-transcribed for the live preview. There is no window
    // to go with it — every hop decodes the whole utterance — so this is a rate, not a span.
    uint32_t hop_ms = 1000;
    // openai-translator.md §4.3: the v1 default for a batch model. Turns hops off entirely, so
    // the only transcription is the one the finalize forces and no `partial` is ever emitted.
    bool finalize_only = true;
    bool vad_gate = true;
};

// The utterance buffer, with no network in it. Split out so the hop cadence can be tested
// without a server.
//
// **One invariant, and it is the whole design: no sample that has been fed ever leaves the
// buffer before `reset()`.** A take hands over everything since the last reset, so the model
// always sees the utterance from its first sample and the transcript it returns is the
// cumulative hypothesis with nothing added to it. That is what makes duplication structurally
// impossible here rather than merely improbable: there is no second source of text to join.
//
// The buffer is bounded by the caller. Audio is fed only while an utterance is open, and
// `VadConfig::max_utterance_ms` closes one after 30 s; `connection.cpp` refuses to run without
// that bound.
class PseudoStreamingDriver {
public:
    // `hop_ms == 0` disables hops entirely: only a forced take produces a window, which is
    // finalize-only.
    explicit PseudoStreamingDriver(uint32_t hop_ms);

    void reset();

    // Append gated PCM. Returns true when at least one hop is now due.
    bool feed(const int16_t* samples, size_t count);

    // Take the audio to transcribe next — the whole utterance so far — consuming one hop's
    // worth of pending audio. Returns false when there is nothing to transcribe.
    //
    // `force` is finalize: take the buffer even when no hop is due, and even when hops are off.
    bool take_window(bool force, std::vector<int16_t>& window);

    [[nodiscard]] uint64_t audio_ms() const noexcept { return audio_ms_; }
    [[nodiscard]] bool any_audio() const noexcept { return any_audio_; }
    [[nodiscard]] uint32_t hop_ms() const noexcept { return hop_ms_; }
    [[nodiscard]] bool hops_enabled() const noexcept { return hop_samples_ != 0; }

private:
    uint32_t hop_ms_;
    size_t hop_samples_;  // 0 means hops are off

    std::vector<int16_t> buf_;
    size_t pending_samples_ = 0;
    uint64_t audio_ms_ = 0;
    bool any_audio_ = false;
};

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
    bool oversize_warned_ = false;  // likewise: a buffer past any plausible utterance length
    uint64_t requests_ = 0;
    uint64_t vad_skipped_ = 0;
    uint64_t empty_responses_ = 0;
};

}  // namespace kd::asrd
