// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Endpointing — see vad.h for what this is, what it is not, and why Silero is absent.

#define KD_LOG_MODULE "asrd.vad"

#include "asrd/vad.h"

#include <algorithm>
#include <cmath>

#include "util/log.h"

namespace kd::asrd {
namespace {

constexpr double kFullScale = 32768.0;
constexpr double kDbfsFloor = -120.0;

uint64_t ms_to_samples(double ms, uint32_t sample_rate) {
    if (ms <= 0.0) return 0;
    return static_cast<uint64_t>(ms * sample_rate / 1000.0);
}

double logistic(double x) {
    if (x >= 0.0) return 1.0 / (1.0 + std::exp(-std::min(x, 60.0)));
    const double e = std::exp(std::max(x, -60.0));
    return e / (1.0 + e);
}

}  // namespace

uint64_t VadConfig::min_speech_samples() const noexcept {
    return ms_to_samples(min_speech_ms, sample_rate);
}
uint64_t VadConfig::hangover_samples() const noexcept {
    return ms_to_samples(hangover_ms, sample_rate);
}
uint64_t VadConfig::endpoint_samples() const noexcept {
    return ms_to_samples(endpoint_silence_ms, sample_rate);
}
uint64_t VadConfig::preroll_samples() const noexcept {
    return ms_to_samples(preroll_ms, sample_rate);
}
uint64_t VadConfig::max_utterance_samples() const noexcept {
    return ms_to_samples(max_utterance_ms, sample_rate);
}
uint64_t VadConfig::soft_max_utterance_samples() const noexcept {
    return ms_to_samples(soft_max_utterance_ms, sample_rate);
}

std::string VadConfig::validate() const {
    if (sample_rate == 0) return "sample_rate must be positive";
    if (frame_samples <= 1) return "frame_samples must be > 1";
    if (!(threshold > 0.0 && threshold <= 1.0)) return "threshold must be in (0, 1]";
    if (!(neg_threshold >= 0.0 && neg_threshold < threshold)) {
        return "neg_threshold must be in [0, threshold)";
    }
    if (min_speech_ms < 0.0 || hangover_ms < 0.0 || preroll_ms < 0.0) {
        return "min_speech_ms, hangover_ms and preroll_ms must be >= 0";
    }
    if (endpoint_silence_ms <= 0.0) return "endpoint_silence_ms must be > 0";
    if (hangover_ms > endpoint_silence_ms) {
        // A hangover longer than the endpoint would report the end of speech *after* the
        // utterance it belongs to had already been closed.
        return "hangover_ms must not exceed endpoint_silence_ms";
    }
    if (max_utterance_ms != 0.0 && max_utterance_ms <= endpoint_silence_ms) {
        return "max_utterance_ms must exceed endpoint_silence_ms, or be 0 to disable";
    }
    if (soft_max_utterance_ms != 0.0) {
        // Below the hangover there is no boundary to wait for, and above the stopwatch the
        // stopwatch wins and the soft cut is dead code that reads as if it were live.
        if (soft_max_utterance_ms <= hangover_ms) {
            return "soft_max_utterance_ms must exceed hangover_ms, or be 0 to disable";
        }
        if (max_utterance_ms != 0.0 && soft_max_utterance_ms > max_utterance_ms) {
            return "soft_max_utterance_ms must not exceed max_utterance_ms";
        }
    }
    return {};
}

// ─── Scorer ─────────────────────────────────────────────────────────────────────────────────

void frame_dbfs_zcr(const int16_t* frame, size_t n, double& dbfs, double& zcr) noexcept {
    if (n == 0) {
        dbfs = kDbfsFloor;
        zcr = 0.0;
        return;
    }
    // int64 accumulator: 512 samples of full-scale int16 is ~5.4e11, well past int32.
    int64_t total = 0;
    size_t crossings = 0;
    bool prev_positive = frame[0] >= 0;
    for (size_t i = 0; i < n; ++i) {
        const int64_t x = frame[i];
        total += x * x;
        const bool positive = frame[i] >= 0;
        if (positive != prev_positive) {
            ++crossings;
            prev_positive = positive;
        }
    }
    const double mean_square = static_cast<double>(total) / static_cast<double>(n);
    dbfs = mean_square <= 0.0
               ? kDbfsFloor
               : std::max(kDbfsFloor, 10.0 * std::log10(mean_square / (kFullScale * kFullScale)));
    zcr = n > 1 ? static_cast<double>(crossings) / static_cast<double>(n - 1) : 0.0;
}

EnergyZcrScorer::EnergyZcrScorer(const VadConfig& cfg) : cfg_(&cfg) {
    warmup_frames_ = static_cast<uint64_t>(std::ceil(
        static_cast<double>(ms_to_samples(cfg.energy_warmup_ms, cfg.sample_rate)) /
        static_cast<double>(cfg.frame_samples)));
    reset();
}

void EnergyZcrScorer::reset() noexcept {
    noise_dbfs_ = cfg_->energy_initial_noise_dbfs;
    frames_seen_ = 0;
    have_warmup_min_ = false;
    warmup_min_ = 0.0;
}

double EnergyZcrScorer::score(const int16_t* frame, size_t n) noexcept {
    const VadConfig& cfg = *cfg_;
    double dbfs = 0.0;
    double zcr = 0.0;
    frame_dbfs_zcr(frame, n, dbfs, zcr);
    ++frames_seen_;

    if (frames_seen_ <= warmup_frames_) {
        // Calibrating. The *quietest* frame so far, not the mean: the mean of a window
        // containing speech is dominated by the speech, and a floor set too high makes the
        // detector deaf — which is worse than one set too low, because `energy_abs_floor_dbfs`
        // catches that direction.
        warmup_min_ = have_warmup_min_ ? std::min(warmup_min_, dbfs) : dbfs;
        have_warmup_min_ = true;
        noise_dbfs_ = warmup_min_;
        return 0.0;
    }

    double snr_db = dbfs - noise_dbfs_;
    if (zcr >= cfg.zcr_min && zcr <= cfg.zcr_max) snr_db += cfg.zcr_boost_db;
    double prob = logistic((snr_db - cfg.energy_snr_db) / cfg.energy_snr_slope_db);
    if (dbfs < cfg.energy_abs_floor_dbfs) {
        // Below the absolute floor nothing is speech, however good the SNR looks. This is what
        // stops a floor tracked down to digital silence turning dither into dictation.
        prob = 0.0;
    }

    // Asymmetric adaptation: drop to a quieter floor fast, rise to a louder one slowly, and only
    // on frames not already judged speech. A symmetric filter lets sustained speech drag the
    // floor up until the speech stops being speech.
    if (dbfs < noise_dbfs_) {
        noise_dbfs_ += cfg.energy_noise_down * (dbfs - noise_dbfs_);
    } else if (prob < cfg.threshold) {
        noise_dbfs_ += cfg.energy_noise_up * (dbfs - noise_dbfs_);
    }
    return prob;
}

// ─── Endpointer ─────────────────────────────────────────────────────────────────────────────

VadEndpointer::VadEndpointer(const VadConfig& cfg) : cfg_(cfg), scorer_(cfg_) {
    // Enough history for the largest pre-roll request: the pre-roll itself, plus the min-speech
    // window that elapses before an onset can be confirmed, plus slack.
    history_cap_ = static_cast<size_t>(cfg_.preroll_samples() + cfg_.min_speech_samples()) +
                   2u * cfg_.frame_samples;
}

void VadEndpointer::emit(std::vector<VadItem>& out, VadItemKind kind, uint64_t sample,
                         const std::string& reason) {
    VadItem item;
    item.kind = kind;
    item.utterance_index = utterance_index_;
    item.sample = sample;
    item.reason = reason;
    out.push_back(std::move(item));
}

void VadEndpointer::close_utterance(std::vector<VadItem>& out, const std::string& reason,
                                    bool have_end, uint64_t end_sample) {
    if (!utterance_open_) return;
    const uint64_t end = have_end ? end_sample : last_speech_end_;
    if (speech_) {
        speech_ = false;
        emit(out, VadItemKind::SpeechEnd, end, reason);
    }
    emit(out, VadItemKind::UtteranceEnd, end, reason);
    utterance_open_ = false;
    last_boundary_ = pos_;
    have_pending_start_ = false;
    pending_samples_ = 0;
}

void VadEndpointer::open_utterance(std::vector<VadItem>& out, uint64_t onset,
                                   const std::string& reason) {
    if (!utterance_open_) {
        ++utterance_index_;
        utterance_open_ = true;
        utterance_start_ = onset;
    }
    emit(out, VadItemKind::SpeechStart, onset, reason);
}

void VadEndpointer::reset_stream_state() {
    buf_.clear();
    history_.clear();
    history_start_ = pos_;
    speech_ = false;
    have_pending_start_ = false;
    pending_start_ = 0;
    pending_samples_ = 0;
    silence_samples_ = 0;
    last_speech_end_ = pos_;
    last_boundary_ = pos_;
    scorer_.reset();
}

void VadEndpointer::reset() {
    reset_stream_state();
    utterance_open_ = false;
    utterance_index_ = 0;
}

void VadEndpointer::process(const int16_t* samples, size_t count, std::vector<VadItem>& out) {
    buf_.insert(buf_.end(), samples, samples + count);
    const size_t n = cfg_.frame_samples;
    size_t consumed = 0;
    while (buf_.size() - consumed >= n) {
        on_frame(buf_.data() + consumed, n, out);
        consumed += n;
    }
    if (consumed != 0) buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(consumed));
}

void VadEndpointer::flush(std::vector<VadItem>& out) {
    if (utterance_open_ && !buf_.empty()) {
        VadItem item;
        item.kind = VadItemKind::Audio;
        item.utterance_index = utterance_index_;
        item.sample = pos_;
        item.audio = buf_;
        out.push_back(std::move(item));
    }
    pos_ += buf_.size();
    buf_.clear();
    close_utterance(out, "flush", false, 0);
}

void VadEndpointer::notify_discontinuity(uint32_t gap_ms, const std::string& reason,
                                         std::vector<VadItem>& out) {
    const uint64_t at = pos_;
    const std::string why = reason.empty() ? std::string("discontinuity") : reason;
    // `end_sample` is the last moment speech was heard, not the moment the gap was reported —
    // the utterance ended where the audio stopped being trustworthy, which is before the gap.
    close_utterance(out, why, false, 0);
    if (gap_ms > 0) pos_ += ms_to_samples(gap_ms, cfg_.sample_rate);
    emit(out, VadItemKind::Discontinuity, at, why);
    reset_stream_state();
    KD_LOG_INFO(
        "audio discontinuity at sample %llu (gap_ms=%u); VAD state reset and any open utterance "
        "closed",
        static_cast<unsigned long long>(at), gap_ms);
}

void VadEndpointer::on_frame(const int16_t* frame, size_t n, std::vector<VadItem>& out) {
    const VadConfig& cfg = cfg_;
    const uint64_t start = pos_;
    const uint64_t end = start + n;
    pos_ = end;

    history_.insert(history_.end(), frame, frame + n);
    if (history_.size() > history_cap_) {
        const size_t excess = history_.size() - history_cap_;
        history_.erase(history_.begin(), history_.begin() + static_cast<ptrdiff_t>(excess));
        history_start_ += excess;
    }

    // The safety valve runs first, so the frame that opens the replacement utterance is
    // attributed to it rather than to the one being cut. Its events go out before this frame's
    // audio, which is why they are appended straight to `out` while the scoring decisions
    // below are buffered.
    const uint64_t max_samples = cfg.max_utterance_samples();
    if (utterance_open_ && max_samples != 0 && start - utterance_start_ >= max_samples) {
        emit(out, VadItemKind::UtteranceEnd, start, "max_utterance");
        utterance_open_ = false;
        last_boundary_ = start;
        KD_LOG_WARN(
            "utterance %llu exceeded max_utterance_ms=%.0f and was cut at sample %llu; this is a "
            "safety valve, not an endpoint — check the VAD threshold or the input level",
            static_cast<unsigned long long>(utterance_index_), cfg.max_utterance_ms,
            static_cast<unsigned long long>(start));
        if (speech_) {
            // Mid-speech cut: open the replacement immediately so the decoder feed stays
            // continuous. Re-qualifying through min_speech_ms would punch a hole in it.
            open_utterance(out, start, "max_utterance");
        }
    }

    const bool gate_was_open = utterance_open_;
    const double prob = scorer_.score(frame, n);
    last_prob_ = prob;
    bool onset_this_frame = false;

    // Events produced by this frame's own decision, held back so the frame's audio can be
    // placed on the correct side of them. See VadItem's comment.
    std::vector<VadItem> frame_items;

    const double threshold = speech_ ? cfg.neg_threshold : cfg.threshold;
    const bool speaking = prob >= threshold;

    if (speaking) {
        // Any speaking frame resets the endpoint timer, including one inside a run too short to
        // qualify as an onset. min_speech_ms suppresses spurious *onsets*, not the timer: the
        // alternative — a sub-minimum blip that does not reset it — lets the endpoint fire in
        // the middle of a word that has just started, splitting it across two utterances.
        silence_samples_ = 0;
        if (speech_) {
            last_speech_end_ = end;
        } else {
            if (!have_pending_start_) {
                have_pending_start_ = true;
                pending_start_ = start;
                pending_samples_ = 0;
            }
            pending_samples_ += n;
            if (pending_samples_ >= cfg.min_speech_samples()) {
                // Backdate the onset to where the run began — that is where speech actually
                // started; the detector is merely late. Clamp to the last utterance boundary so
                // an onset can never be reported before the end of the previous utterance.
                const uint64_t onset = std::max(pending_start_, last_boundary_);
                speech_ = true;
                last_speech_end_ = end;
                have_pending_start_ = false;
                pending_samples_ = 0;
                open_utterance(frame_items, onset, "");
                onset_this_frame = true;
            }
        }
    } else {
        have_pending_start_ = false;
        pending_samples_ = 0;
        silence_samples_ += n;
        if (speech_ && silence_samples_ >= cfg.hangover_samples()) {
            speech_ = false;
            emit(frame_items, VadItemKind::SpeechEnd, last_speech_end_, "");

            // The soft cut. This breath is not an endpoint — the speaker is mid-thought and
            // `endpoint_silence_ms` has not elapsed — but the utterance is long enough that the
            // alternative is the stopwatch, and the stopwatch cuts mid-word. Ending it here puts
            // the boundary between two words instead, which is the whole difference: a batch
            // model handed a fragment starting mid-word invents a plausible beginning for it.
            //
            // Nothing is lost by ending early. The audio after this point opens the next
            // utterance and is transcribed there; every sample is still decoded exactly once.
            if (utterance_open_ && cfg.soft_max_utterance_ms != 0.0 &&
                start - utterance_start_ >= cfg.soft_max_utterance_samples()) {
                emit(frame_items, VadItemKind::UtteranceEnd, last_speech_end_,
                     "soft_max_utterance");
                utterance_open_ = false;
                last_boundary_ = end;
                KD_LOG_INFO(
                    "utterance %llu passed soft_max_utterance_ms=%.0f and was closed at the next "
                    "breath (sample %llu) rather than left to the stopwatch",
                    static_cast<unsigned long long>(utterance_index_), cfg.soft_max_utterance_ms,
                    static_cast<unsigned long long>(last_speech_end_));
            }
        }
        if (utterance_open_ && !speech_ && silence_samples_ >= cfg.endpoint_samples()) {
            // plan.md §1.6's endpoint: this ends the UTTERANCE. The session is untouched and
            // this module has no way to touch it.
            emit(frame_items, VadItemKind::UtteranceEnd, last_speech_end_, "endpoint");
            utterance_open_ = false;
            last_boundary_ = end;
        }
    }

    // Audio routing. Exactly one of these applies: an onset that opened the gate replays the
    // pre-roll and this frame together *after* its SpeechStart, otherwise an already-open gate
    // forwards this frame *before* any end-of-utterance event — including the frame on which
    // the utterance ended, whose tail belongs to it.
    if (onset_this_frame && !gate_was_open) {
        out.insert(out.end(), std::make_move_iterator(frame_items.begin()),
                   std::make_move_iterator(frame_items.end()));
        const uint64_t first = std::max(
            utterance_start_ >= cfg.preroll_samples() ? utterance_start_ - cfg.preroll_samples()
                                                      : 0,
            history_start_);
        const size_t offset = static_cast<size_t>(first - history_start_);
        VadItem item;
        item.kind = VadItemKind::Audio;
        item.utterance_index = utterance_index_;
        item.sample = first;
        item.audio.assign(history_.begin() + static_cast<ptrdiff_t>(offset), history_.end());
        out.push_back(std::move(item));
    } else if (gate_was_open) {
        VadItem item;
        item.kind = VadItemKind::Audio;
        item.utterance_index = utterance_index_;
        item.sample = start;
        item.audio.assign(frame, frame + n);
        out.push_back(std::move(item));
        out.insert(out.end(), std::make_move_iterator(frame_items.begin()),
                   std::make_move_iterator(frame_items.end()));
    } else {
        out.insert(out.end(), std::make_move_iterator(frame_items.begin()),
                   std::make_move_iterator(frame_items.end()));
    }
}

}  // namespace kd::asrd
