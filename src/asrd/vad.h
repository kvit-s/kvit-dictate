// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// Endpointing: frame energy and zero-crossing rate into utterance boundaries.
// Port of `server/kvit_asr/vad.py`'s `EnergyZcrScorer` and `VadEndpointer`.
//
// ─── Silero is not here, and openai-translator.md §4.4 is blunt about what that costs ───────
//
// Nothing, because it was never running. `config.py`'s `--vad` defaults to `auto`,
// `--silero-model` defaults to `None`, no `models.json` entry sets it, and `SileroOnnxScorer`
// imports `onnxruntime` inside `load()` — so on every run this project has ever done, Silero
// failed to load and the energy scorer answered. **That is what has endpointed every dictation
// here.** Porting it is not a downgrade; it is porting the thing that is actually in use.
//
// It is still worse than Silero, in exactly the ways vad.py enumerates and which are repeated
// here rather than left behind with the Python:
//
//   * It detects *sound*, not speech. A door, a keyboard, a cough and a fan step all clear the
//     SNR gate. Silero was trained to reject them.
//   * It needs a calibration window and reports nothing during it, so a stream that opens
//     mid-word loses that word's onset.
//   * Steady broadband noise that starts *after* calibration reads as speech until the slow
//     upward adaptation catches up — of the order of a second.
//   * Zero-crossing rate is a weak feature, used only to bring unvoiced onsets (/s/, /f/)
//     forward a frame or two. It cannot separate a fricative from white noise.
//
// The probability it returns is a monotone squashing of SNR in dB, **not** a calibrated
// probability. It is never a confidence and must never be reported as one.
//
// ─── The other VAD, which is a different mechanism and is not this one ──────────────────────
//
// `models.json` sets `vad_gate=true`, which is a call to the *speech server's* Silero over
// `/v1/audio/vad`. That one is in `backend_openai.h`, it is a batch question about a finished
// buffer, and it cannot drive endpointing. The two are not alternatives: this module decides
// *when an utterance ends*, 32 ms at a time; that one decides whether a finished window is
// worth transcribing at all.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kd::asrd {

inline constexpr uint32_t kVadSampleRate = 16000;

// Every tunable in one place. The defaults are vad.py's, except `endpoint_silence_ms`, which
// openai-translator.md §4.4 raises to 1500 for a batch backend: a remote round trip per
// utterance makes an early endpoint expensive, and a batch model has no decoder state to lose
// by waiting.
struct VadConfig {
    uint32_t sample_rate = kVadSampleRate;
    // 512 samples = 32 ms. Silero's published 16 kHz usage requires exactly this window, and
    // keeping the same hop bounds the position error of every event at one frame.
    uint32_t frame_samples = 512;

    // Trigger at `threshold`, un-trigger at `neg_threshold`. The hysteresis gap is what stops a
    // probability hovering at the boundary producing a burst of one-frame speech segments.
    double threshold = 0.5;
    double neg_threshold = 0.35;

    // A speech run shorter than this never produces an onset: it is a click, a lip smack, a
    // chair. Suppression is on the *onset*, not on the endpoint timer.
    double min_speech_ms = 200.0;
    // How long speech must be absent before SPEECH_END. Below this a pause is invisible.
    double hangover_ms = 250.0;
    // Ends the UTTERANCE. Never the session.
    double endpoint_silence_ms = 1500.0;
    // Audio fed to the decoder ahead of the detected onset.
    double preroll_ms = 300.0;
    // ─── Two ways a long utterance ends, and only one of them is any good ──────────────────
    //
    // A batch model has to be handed a bounded recording, so an utterance that never endpoints
    // has to be ended by something. The question is *where*, and the two fields below are the
    // second and third of three answers, in descending order of quality:
    //
    //   1. The model's own segment boundary, from the timestamps in its response. Exact, and
    //      handled in `backend_openai.cpp` rather than here — it knows where a sentence ended
    //      because it just transcribed it. Not always offered: measured 2026-08-30 against
    //      Qwen3-ASR, a 21 s clip came back as two segments but the same audio at 13/16/19 s
    //      came back as one, so the boundary can lag the pause by several seconds.
    //   2. `soft_max_utterance_ms` — the next *breath*. Past this much audio the hangover
    //      boundary (`hangover_ms` of quiet, far more frequent than a full endpoint) ends the
    //      utterance instead of merely noting a pause. Cuts between words.
    //   3. `max_utterance_ms` — the stopwatch. Cuts wherever the clock lands, mid-word if that
    //      is where it lands, which garbles a word on both sides of the seam.
    //
    // Only 3 existed before 2026-08-30, and it fired during ordinary long-winded dictation
    // rather than in the emergency it was written for. That is the wrong tool doing the common
    // job: `progress.md` §3b already raised `endpoint_silence_ms` 800 → 1500 so that utterance
    // boundaries land on *sentences*, and a stopwatch cut contradicts that outright.

    // Past this much audio, the next hangover boundary ends the utterance. 0 disables it, which
    // leaves the stopwatch as the only answer.
    double soft_max_utterance_ms = 20000.0;

    // The stopwatch, and now genuinely a safety valve: with 2 in place it is reachable only by
    // audio with no quiet gap at all in 28 seconds — a stuck detector, a television, a fan the
    // threshold is calibrated wrong for. 0 disables it, and the OpenAI backend refuses to run
    // that way, because it buffers a whole utterance and never drops a sample.
    //
    // 28 s, not 30: the clock starts at the *onset*, but the audio handed over also carries
    // `preroll_ms` from before it, so a cut utterance reaches the model as roughly
    // `max_utterance_ms + preroll_ms`. Whisper-family models have a 30 s native window, and
    // 30 000 + 300 was over it. (The 300 ms also used to be exactly the amount by which a decode
    // window was undersized, which put the same speech in a user's document twice. Nothing
    // depends on that sum any more, but the arithmetic surprises people, so it is written down.)
    double max_utterance_ms = 28000.0;

    // Energy/ZCR scorer.
    double energy_snr_db = 10.0;
    double energy_snr_slope_db = 3.0;
    double energy_abs_floor_dbfs = -55.0;
    double energy_warmup_ms = 200.0;
    double energy_initial_noise_dbfs = -75.0;
    double energy_noise_down = 0.5;
    double energy_noise_up = 0.05;
    double zcr_min = 0.15;
    double zcr_max = 0.45;
    double zcr_boost_db = 4.0;

    [[nodiscard]] uint64_t min_speech_samples() const noexcept;
    [[nodiscard]] uint64_t hangover_samples() const noexcept;
    [[nodiscard]] uint64_t endpoint_samples() const noexcept;
    [[nodiscard]] uint64_t preroll_samples() const noexcept;
    [[nodiscard]] uint64_t max_utterance_samples() const noexcept;
    [[nodiscard]] uint64_t soft_max_utterance_samples() const noexcept;

    // Returns an empty string when the configuration is coherent, and the reason otherwise.
    // Same checks as `VadConfig.__post_init__`, including the one that is not obvious: a
    // hangover longer than the endpoint would report the end of speech *after* the utterance it
    // belongs to had already been closed.
    [[nodiscard]] std::string validate() const;
};

// RMS level in dBFS and zero-crossing rate (crossings per sample) for one frame.
void frame_dbfs_zcr(const int16_t* frame, size_t n, double& dbfs, double& zcr) noexcept;

// Frame energy against an adaptive noise floor, plus zero-crossing rate. See the header comment
// for the list of ways this is worse than Silero.
class EnergyZcrScorer {
public:
    explicit EnergyZcrScorer(const VadConfig& cfg);
    void reset() noexcept;
    double score(const int16_t* frame, size_t n) noexcept;
    [[nodiscard]] double noise_floor_dbfs() const noexcept { return noise_dbfs_; }

private:
    const VadConfig* cfg_;
    uint64_t warmup_frames_ = 0;
    uint64_t frames_seen_ = 0;
    double noise_dbfs_ = 0.0;
    double warmup_min_ = 0.0;
    bool have_warmup_min_ = false;
};

// ─── Output ─────────────────────────────────────────────────────────────────────────────────

enum class VadItemKind : uint8_t {
    // A run of gated audio, already pre-rolled, belonging to `utterance_index`. The session
    // appends it to the backend's window.
    Audio,
    SpeechStart,
    SpeechEnd,     // the hangover, not the endpoint
    UtteranceEnd,  // plan.md §1.6's endpoint. Ends the utterance; the session continues.
    Discontinuity,
};

// **Items are ordered**, and the ordering is the interesting part.
//
// vad.py returns events and audio segments as two separate tuples and leaves the consumer to
// interleave them, which is fine for a bench script and wrong for a session loop: the audio of
// the frame on which an utterance ends belongs to *that* utterance and must reach the backend
// before the `final` is forced. So this port emits one ordered stream instead, with the audio
// of each frame placed on the correct side of that frame's events.
struct VadItem {
    VadItemKind kind = VadItemKind::Audio;
    uint64_t utterance_index = 0;
    uint64_t sample = 0;  // where it happened, in the endpointer's own sample clock
    std::string reason;   // "endpoint", "flush", "max_utterance", "discontinuity", or empty
    std::vector<int16_t> audio;  // Audio only
};

// Turns a stream of frame scores into utterance boundaries and a gated decoder feed.
//
//     SILENCE --(scores >= threshold for min_speech_ms)--> SPEECH   emits SpeechStart
//     SPEECH  --(scores <  neg_threshold for hangover_ms)-> SILENCE emits SpeechEnd
//     SILENCE --(endpoint_silence_ms since last speech)---> closed  emits UtteranceEnd
//
// An utterance opens at the first SpeechStart and survives every SpeechEnd until the endpoint
// timer expires — which is what makes a brief mid-sentence pause harmless and is the entire
// point of the module (plan.md §1.6).
//
// The same stream split into different chunk sizes produces identical items. A component that
// is only correct at one chunking is correct by accident.
class VadEndpointer {
public:
    explicit VadEndpointer(const VadConfig& cfg);

    // Consume s16le audio of any length; frames are reassembled internally. Appends to `out`.
    void process(const int16_t* samples, size_t count, std::vector<VadItem>& out);

    // End of stream: close any open utterance so nothing is left un-finalised. The sub-frame
    // residue is emitted if the gate is open — under one frame of audio, but it is the tail of
    // a word, and dropping it clips the last phoneme of the session.
    void flush(std::vector<VadItem>& out);

    // docs/protocol.md §6.1 `discontinuity`. Ends the open utterance and resets everything: a
    // decoder reset and an utterance boundary must happen together, because resetting mid-
    // utterance would make the next `partial.text` for that id no longer a superset of the
    // previous one, which §8.1 and §8.2 between them forbid.
    //
    // `gap_ms == 0` means "duration unknown" (§6.1) and advances nothing; inventing a duration
    // would put every subsequent position quietly out of step with the Client's timeline.
    void notify_discontinuity(uint32_t gap_ms, const std::string& reason,
                              std::vector<VadItem>& out);

    // Full reset between sessions, keeping the position clock. Emits nothing.
    void reset();

    [[nodiscard]] bool gate_open() const noexcept { return utterance_open_; }
    [[nodiscard]] uint64_t utterance_index() const noexcept { return utterance_index_; }
    [[nodiscard]] uint64_t position_samples() const noexcept { return pos_; }
    [[nodiscard]] double last_probability() const noexcept { return last_prob_; }
    [[nodiscard]] const VadConfig& config() const noexcept { return cfg_; }

private:
    void on_frame(const int16_t* frame, size_t n, std::vector<VadItem>& out);
    void emit(std::vector<VadItem>& out, VadItemKind kind, uint64_t sample,
              const std::string& reason);
    void close_utterance(std::vector<VadItem>& out, const std::string& reason, bool have_end,
                         uint64_t end_sample);
    void open_utterance(std::vector<VadItem>& out, uint64_t onset, const std::string& reason);
    void reset_stream_state();

    VadConfig cfg_;
    EnergyZcrScorer scorer_;

    std::vector<int16_t> buf_;      // samples not yet forming a whole frame
    std::vector<int16_t> history_;  // recent samples, for pre-roll
    size_t history_cap_ = 0;
    uint64_t history_start_ = 0;
    uint64_t pos_ = 0;

    bool speech_ = false;  // the SPEECH/SILENCE state of the frame decision
    bool utterance_open_ = false;
    uint64_t utterance_index_ = 0;
    uint64_t utterance_start_ = 0;
    bool have_pending_start_ = false;
    uint64_t pending_start_ = 0;
    uint64_t pending_samples_ = 0;
    uint64_t silence_samples_ = 0;
    uint64_t last_speech_end_ = 0;
    uint64_t last_boundary_ = 0;
    double last_prob_ = 0.0;
};

}  // namespace kd::asrd
