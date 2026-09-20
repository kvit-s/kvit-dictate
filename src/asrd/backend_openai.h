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
// ─── Committing the settled part, using the model's own timestamps ────────────────────────
//
// Buffering the whole utterance is correct but not free: the POST grows with the utterance, and
// past a certain length the early sentences are being re-uploaded and re-decoded every hop for
// no reason. They are not going to change — a batch model revises the *tail*, where it had
// incomplete context, and leaves settled text alone. Measured 2026-08-30 against this endpoint:
// the transcript of the first 11 s of a clip was a character-exact prefix (143/143) of the
// transcript of the whole 21 s.
//
// The obvious move — notice the text has settled, drop the audio behind it — is the trap. Text
// stability tells you *what* settled, not *where in the audio* it ended, and mapping the one to
// the other by matching words is `scrolled_off_words` reinvented, with the same failure and the
// same duplicated dictation at the end of it.
//
// `response_format=verbose_json` removes the guess. The response carries a segment table:
//
//   {"text": "...", "duration": 21.395,
//    "segments": [{"id":0, "start":0.0, "end":15.22, "text":"..."},
//                 {"id":1, "start":15.22, "end":21.39, "text":"..."}]}
//
// Each entry gives the text **and** the time, together, from the model. So committing all but
// the last segment and cutting the audio at that segment's `end` needs no inference at all: the
// text to commit is handed over, and so is the sample offset. Verified end to end on 2026-08-30
// by cutting the clip at 15.22 s and decoding the halves independently — the head decode equalled
// `segments[0].text` exactly, and head + tail reproduced the whole-clip transcript character for
// character, with nothing lost or repeated at the join.
//
// **It is offered, not guaranteed.** The same audio truncated to 13, 16 and 19 s came back as a
// single segment; the boundary at 15.22 s only appeared once 21.4 s had arrived. So the model
// may hand over no cut point for a long time, and `propose_cut` refuses rather than improvises:
// fewer than two segments, a table that does not reconcile with `text`, a degenerate offset —
// all return nothing, and the utterance simply keeps buffering until `VadConfig`'s soft cut or
// its stopwatch ends it. Every refusal costs decode time; none can cost a word.
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

// The endpoint's cap on the `prompt` field, measured 2026-09-20: 4096 bytes is accepted, 4097
// is refused with `form field 'prompt' is too long` and HTTP 400.
//
// The client's own limits are looser — docs/protocol.md §6.1 allows 256 terms of 128 bytes, so
// 32 KB — which means a legal `start.context` can exceed what the server will take. The join
// below truncates rather than letting the request fail, because a vocabulary list one term too
// long must not cost the user the whole utterance.
inline constexpr size_t kMaxPromptBytes = 4096;

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
    // Ask for `verbose_json` and act on the segment table when one comes back. Off falls back to
    // `json` and a plain string, which every OpenAI-compatible service supports — so this is the
    // switch to reach for when pointing at a service whose segmentation is not trustworthy.
    bool segment_cuts = true;
    // A proposed cut must leave at least this much audio behind it and this much in front, or it
    // is not worth the utterance boundary it costs. Guards against a degenerate segment table
    // producing a boundary every hop.
    uint32_t min_cut_head_ms = 4000;
    uint32_t min_cut_tail_ms = 1000;
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

    // Drop the first `cut_samples` and keep the rest, for a caller that has just committed the
    // transcript of exactly that audio.
    //
    // **This is the one operation allowed to remove audio from the buffer, and it is not an
    // exception to the invariant above — it is the invariant restated across a boundary.** The
    // dropped samples are not discarded, they are *finished*: their transcript has already gone
    // out as a `final`. Every sample is still decoded exactly once, in exactly one utterance,
    // and no two pieces of text ever describe the same audio. The caller owes that guarantee;
    // `OpenAiBackend::propose_cut` is what establishes it, from the model's own segment table.
    void retain_tail(size_t cut_samples);

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

// One entry of a `verbose_json` segment table: a span of the recording and its transcript.
struct Segment {
    double start_s = 0.0;
    double end_s = 0.0;
    std::string text;
};

// ─── Vocabulary hints, joined for the wire ──────────────────────────────────────────────────
//
// `start.context` is a list; the endpoint's `prompt` is one string. This joins them with ", "
// and stops before `max_bytes`, on a term boundary — never mid-term, and never mid-UTF-8, since
// the terms are copied whole or not at all.
//
// Dropping the tail of the list rather than the tail of the string is the whole point. A prompt
// cut mid-word is a fragment the model will try to make sense of, and biasing a recogniser
// toward half a word is worse than not biasing it: measured 2026-09-20, a degenerate 4096-byte
// prompt of one repeated term returned an empty transcript for audio that otherwise transcribes
// fine, so a bad prompt does not merely fail to help — it can cost the utterance.
//
// Pure and free-standing, and tested, for the reason `corpus_prune_set` is: it is the kind of
// arithmetic whose off-by-one is invisible in a request log and obvious in a table of cases.
// `dropped` reports how many terms did not fit, so the caller can say so once rather than
// silently sending less vocabulary than the user configured.
[[nodiscard]] std::string join_context_prompt(const std::vector<std::string>& terms,
                                              size_t max_bytes, size_t* dropped = nullptr);

// A cut the model proposed for itself. `committed_text` is the transcript of exactly the first
// `cut_samples` samples — not an estimate of it, not a prefix matched against it — so a caller
// may emit that text as final and keep the rest of the audio with no overlap between the two.
struct ProposedCut {
    size_t cut_samples = 0;
    std::string committed_text;
};

struct Transcription {
    // The cumulative hypothesis for everything currently buffered.
    std::string text;
    bool have_text = false;
    // Set only when the response's own segment table supports it, and never on a finalize.
    bool have_cut = false;
    ProposedCut cut;
};

// Decide whether a response's segment table supports committing a prefix of the recording.
//
// Free rather than a member, and pure: it is the one place where text and audio are allowed to
// be related to each other, so every refusal in it needs to be pinned by a test that does not
// need a server. Returns false — keep buffering, commit nothing — on anything it cannot fully
// account for. A refusal costs decode time; being wrong here costs the user words.
bool propose_segment_cut(const OpenAiBackendConfig& cfg, size_t buffered_samples,
                         const std::string& text, const std::vector<Segment>& segments,
                         ProposedCut& out);

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

    // The session's vocabulary hints (docs/protocol.md §6.1), sent as `prompt` on every
    // transcription. Set once at `session_start`; empty restores the unbiased default.
    //
    // Session-scoped rather than config-scoped because that is what the protocol makes it: the
    // terms arrive with `start`, and a connection may serve sessions with different ones.
    void set_context(const std::vector<std::string>& terms);
    [[nodiscard]] const std::string& context_prompt() const noexcept { return prompt_; }

    // Append gated PCM to the window. Cheap; no network. Returns true when a hop is due.
    bool feed(const int16_t* samples, size_t count);

    // Transcribe the current buffer. `force` finalises.
    //
    // Blocking, and that is why it is called on the decode thread: a round trip against the
    // measured endpoint is 237–541 ms (plus 50–176 ms for the optional VAD gate), and doing
    // that on the connection thread would stall audio intake for half a second at a time and
    // overflow the AUDIO_QUEUE_MAX_MS budget docs/protocol.md §9 makes fatal.
    Status transcribe(bool force, Transcription& out);

    // Abort whatever is in flight, from any thread.
    void cancel();
    void reset_cancel();

    [[nodiscard]] const std::string& model() const noexcept { return cfg_.model; }
    [[nodiscard]] uint64_t audio_ms() const;
    [[nodiscard]] uint32_t hop_ms() const noexcept { return driver_.hop_ms(); }

    // Keep a copy of the audio each `transcribe` actually posts, so a caller that later decides
    // the answer was interesting can write down what produced it.
    //
    // Opt-in because it is a copy of the whole utterance on every hop, and because only one
    // caller wants it: the revision capture in session_server.cpp. Off, this costs nothing.
    void set_capture_window(bool on) { capture_window_ = on; }
    // Valid until the next `transcribe`. Empty when capture is off or nothing has been posted.
    [[nodiscard]] const std::vector<int16_t>& last_window() const noexcept { return last_window_; }
    [[nodiscard]] bool finalize_only() const noexcept { return cfg_.finalize_only; }
    [[nodiscard]] uint64_t requests() const noexcept { return requests_; }
    [[nodiscard]] uint64_t vad_skipped() const noexcept { return vad_skipped_; }
    [[nodiscard]] uint64_t segment_cuts_taken() const noexcept { return segment_cuts_; }

    // Drop the committed head and keep the tail, after a `ProposedCut` has been acted on.
    void retain_tail(size_t cut_samples);

private:
    // The server's Silero route. Returns true — "go ahead" — on every failure. See the header.
    bool vad_says_speech(const std::vector<int16_t>& pcm);
    Status transcribe_batch(const std::vector<int16_t>& pcm, std::string& out,
                            std::vector<Segment>& segments);

    OpenAiBackendConfig cfg_;
    std::string prompt_;  // joined `start.context`; empty means the field is omitted
    HttpUrl transcribe_url_{};
    HttpUrl vad_url_{};
    HttpClient http_;

    // `feed()` runs on the connection thread and `transcribe()` on the decode thread, so the
    // driver's buffer has two writers. The mutex covers the driver's own state only; the HTTP
    // call deliberately happens *outside* it, which is the whole reason the two threads exist.
    mutable std::mutex driver_mutex_;
    PseudoStreamingDriver driver_;
    bool capture_window_ = false;
    std::vector<int16_t> last_window_;

    bool vad_unavailable_ = false;  // latched after the first refusal, so it is logged once
    bool oversize_warned_ = false;  // likewise: a buffer past any plausible utterance length
    bool no_segments_warned_ = false;  // likewise: verbose_json asked for, plain text returned
    uint64_t requests_ = 0;
    uint64_t vad_skipped_ = 0;
    uint64_t segment_cuts_ = 0;
    uint64_t empty_responses_ = 0;
};

}  // namespace kd::asrd
