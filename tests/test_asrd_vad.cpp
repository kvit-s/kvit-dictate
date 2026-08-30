// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// tests/test_asrd_vad.cpp — the endpointer that has been endpointing every dictation this
// project has ever done.
//
// That claim is the reason this file exists rather than being deferred with Silero:
// `config.py`'s `--vad` defaults to `auto`, `--silero-model` defaults to None, no models.json
// entry sets it, and `SileroOnnxScorer` imports onnxruntime inside `load()` — so the energy
// scorer has answered on every run to date. Porting it is not a downgrade, and testing it is
// not testing a fallback.
//
// The assertions are about behaviour, not about numbers: exact probabilities are a property of
// the SNR curve and pinning them would make every threshold tweak a test edit. What is pinned
// is what the session loop depends on — that silence does not open an utterance, that speech
// does, that a brief pause does not end one, that a long one does, and that the same audio
// split differently produces the same items.

#include "kd_test.h"

#include "asrd/vad.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace kd::asrd;

constexpr double kPi = 3.14159265358979323846;

// A tone at `amplitude` (0..1 of full scale). Speech-shaped enough for an energy detector: what
// it measures is level against a tracked floor, and a 300 Hz tone clears the gate the same way
// a vowel does. That is precisely the limitation vad.h records — this detects sound, not speech.
void append_tone(std::vector<int16_t>& out, uint32_t ms, double amplitude, double hz = 300.0) {
    const size_t n = static_cast<size_t>(ms) * kVadSampleRate / 1000;
    const size_t start = out.size();
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(start + i) / kVadSampleRate;
        out.push_back(static_cast<int16_t>(amplitude * 32000.0 * std::sin(2.0 * kPi * hz * t)));
    }
}

void append_silence(std::vector<int16_t>& out, uint32_t ms) {
    out.insert(out.end(), static_cast<size_t>(ms) * kVadSampleRate / 1000, 0);
}

size_t count_of(const std::vector<VadItem>& items, VadItemKind kind) {
    size_t n = 0;
    for (const VadItem& i : items) {
        if (i.kind == kind) ++n;
    }
    return n;
}

size_t gated_samples(const std::vector<VadItem>& items) {
    size_t n = 0;
    for (const VadItem& i : items) {
        if (i.kind == VadItemKind::Audio) n += i.audio.size();
    }
    return n;
}

// A compact, comparable rendering of an item stream, so a chunking mismatch prints as a diff a
// human can read rather than as two counts that differ.
std::string trace(const std::vector<VadItem>& items) {
    std::string out;
    for (const VadItem& i : items) {
        switch (i.kind) {
            case VadItemKind::Audio: out += "a" + std::to_string(i.audio.size()) + " "; break;
            case VadItemKind::SpeechStart: out += "START@" + std::to_string(i.sample) + " "; break;
            case VadItemKind::SpeechEnd: out += "end@" + std::to_string(i.sample) + " "; break;
            case VadItemKind::UtteranceEnd:
                out += "UTT_END@" + std::to_string(i.sample) + "(" + i.reason + ") ";
                break;
            case VadItemKind::Discontinuity: out += "DISC "; break;
        }
    }
    return out;
}

VadConfig test_config() {
    VadConfig cfg;
    // The shipped default is 1500 ms (openai-translator.md §4.4). Shortened here so a case is
    // a second of audio rather than four, and stated rather than silently different.
    cfg.endpoint_silence_ms = 400.0;
    cfg.hangover_ms = 200.0;
    return cfg;
}

}  // namespace

// ─── Configuration ──────────────────────────────────────────────────────────────────────────

KD_TEST(asrd_vad, the_shipped_defaults_are_coherent) {
    KD_CHECK(VadConfig{}.validate().empty());
    KD_CHECK_EQ(VadConfig{}.endpoint_silence_ms, 1500.0);
    // 28 s, not 30: the clock starts at the onset but the audio handed over also carries
    // `preroll_ms` from before it, and a Whisper-family model's native window is 30 s. See
    // `VadConfig::max_utterance_ms`.
    KD_CHECK_EQ(VadConfig{}.max_utterance_ms, 28000.0);
    KD_CHECK(VadConfig{}.soft_max_utterance_ms < VadConfig{}.max_utterance_ms);
}

KD_TEST(asrd_vad, rejects_a_hangover_longer_than_the_endpoint) {
    // Not a taste check: a hangover longer than the endpoint would report the end of speech
    // *after* the utterance it belongs to had already been closed, and the session loop would
    // then attribute a SpeechEnd to an utterance that no longer exists.
    VadConfig cfg;
    cfg.hangover_ms = 2000.0;
    cfg.endpoint_silence_ms = 1000.0;
    KD_CHECK(!cfg.validate().empty());
}

KD_TEST(asrd_vad, rejects_an_inverted_hysteresis) {
    VadConfig cfg;
    cfg.threshold = 0.3;
    cfg.neg_threshold = 0.5;
    KD_CHECK(!cfg.validate().empty());
}

// ─── The scorer ─────────────────────────────────────────────────────────────────────────────

KD_TEST(asrd_vad, dbfs_and_zcr_are_measured_not_guessed) {
    std::vector<int16_t> silence(512, 0);
    double dbfs = 0.0;
    double zcr = 0.0;
    frame_dbfs_zcr(silence.data(), silence.size(), dbfs, zcr);
    KD_CHECK_NEAR(dbfs, -120.0, 0.001);  // the floor
    KD_CHECK_NEAR(zcr, 0.0, 0.001);

    // Full scale square wave: 0 dBFS, and one crossing per sample pair.
    std::vector<int16_t> square;
    for (int i = 0; i < 512; ++i) square.push_back(i % 2 == 0 ? 32767 : -32768);
    frame_dbfs_zcr(square.data(), square.size(), dbfs, zcr);
    KD_CHECK_NEAR(dbfs, 0.0, 0.1);
    KD_CHECK_NEAR(zcr, 1.0, 0.01);

    // Half scale: -6 dBFS.
    std::vector<int16_t> half(512, 0);
    for (size_t i = 0; i < half.size(); ++i) half[i] = i % 2 == 0 ? 16384 : -16384;
    frame_dbfs_zcr(half.data(), half.size(), dbfs, zcr);
    KD_CHECK_NEAR(dbfs, -6.0, 0.1);
}

KD_TEST(asrd_vad, the_absolute_floor_stops_dither_becoming_dictation) {
    // A noise floor tracked down to digital silence makes -70 dBFS dither look like a 50 dB SNR
    // event. `energy_abs_floor_dbfs` is what stops that, and it is the difference between an
    // idle microphone producing nothing and an idle microphone producing "Yeah."
    VadConfig cfg;
    EnergyZcrScorer scorer(cfg);

    std::vector<int16_t> quiet(512, 0);
    for (int i = 0; i < 40; ++i) scorer.score(quiet.data(), quiet.size());

    // ~-72 dBFS: well above a floor tracked to -120, well below the absolute gate.
    std::vector<int16_t> dither(512, 0);
    for (size_t i = 0; i < dither.size(); ++i) dither[i] = i % 2 == 0 ? 8 : -8;
    KD_CHECK_NEAR(scorer.score(dither.data(), dither.size()), 0.0, 0.0001);
}

KD_TEST(asrd_vad, the_warmup_window_reports_nothing) {
    // Stated as a cost rather than hidden: a stream that opens mid-word loses that word's onset,
    // because the scorer is calibrating and answers 0.0 throughout. Silero has no such wart.
    VadConfig cfg;
    EnergyZcrScorer scorer(cfg);
    std::vector<int16_t> loud;
    append_tone(loud, 32, 0.8);
    KD_REQUIRE_EQ(loud.size(), size_t{512});
    KD_CHECK_NEAR(scorer.score(loud.data(), loud.size()), 0.0, 0.0001);
}

// ─── The endpointer ─────────────────────────────────────────────────────────────────────────

KD_TEST(asrd_vad, silence_alone_opens_nothing) {
    const VadConfig cfg = test_config();
    VadEndpointer vad(cfg);
    std::vector<int16_t> audio;
    append_silence(audio, 3000);

    std::vector<VadItem> items;
    vad.process(audio.data(), audio.size(), items);
    KD_CHECK_EQ(count_of(items, VadItemKind::SpeechStart), size_t{0});
    KD_CHECK_EQ(count_of(items, VadItemKind::UtteranceEnd), size_t{0});
    KD_CHECK_EQ(gated_samples(items), size_t{0});
    KD_CHECK(!vad.gate_open());
    KD_CHECK_EQ(vad.utterance_index(), uint64_t{0});
}

KD_TEST(asrd_vad, speech_then_a_long_pause_is_one_utterance) {
    const VadConfig cfg = test_config();
    VadEndpointer vad(cfg);
    std::vector<int16_t> audio;
    append_silence(audio, 400);   // calibration
    append_tone(audio, 900, 0.5);
    append_silence(audio, 1200);  // past endpoint_silence_ms

    std::vector<VadItem> items;
    vad.process(audio.data(), audio.size(), items);

    KD_CHECK_EQ(count_of(items, VadItemKind::SpeechStart), size_t{1});
    KD_CHECK_EQ(count_of(items, VadItemKind::UtteranceEnd), size_t{1});
    KD_CHECK_EQ(vad.utterance_index(), uint64_t{1});
    KD_CHECK(!vad.gate_open());
    KD_INFO(trace(items));

    // The endpoint, not the hangover, is what closes it — and it is reported with that reason,
    // because a log reader has to be able to tell a real endpoint from a forced one.
    for (const VadItem& i : items) {
        if (i.kind == VadItemKind::UtteranceEnd) KD_CHECK_EQ(i.reason, std::string("endpoint"));
    }
    // Gated audio covers the speech, and the pre-roll means it starts before the detected onset.
    KD_CHECK_GT(gated_samples(items), size_t{900 * 16});
}

KD_TEST(asrd_vad, a_brief_mid_sentence_pause_does_not_end_the_utterance) {
    // plan.md §1.6, and the entire point of the module: a speaker who pauses to think has not
    // finished the sentence, and a detector that ends the utterance there splits it across two
    // `final`s with a capital letter in the middle.
    const VadConfig cfg = test_config();
    VadEndpointer vad(cfg);
    std::vector<int16_t> audio;
    append_silence(audio, 400);
    append_tone(audio, 600, 0.5);
    append_silence(audio, 300);   // longer than hangover, shorter than the endpoint
    append_tone(audio, 600, 0.5);
    append_silence(audio, 1200);

    std::vector<VadItem> items;
    vad.process(audio.data(), audio.size(), items);
    KD_INFO(trace(items));

    KD_CHECK_EQ(count_of(items, VadItemKind::UtteranceEnd), size_t{1});
    KD_CHECK_EQ(vad.utterance_index(), uint64_t{1});
    // The pause is visible as a SpeechEnd — the hangover fired — and the utterance survived it.
    KD_CHECK_GT(count_of(items, VadItemKind::SpeechEnd), size_t{0});
}

KD_TEST(asrd_vad, two_separated_utterances_are_two_utterances) {
    const VadConfig cfg = test_config();
    VadEndpointer vad(cfg);
    std::vector<int16_t> audio;
    append_silence(audio, 400);
    append_tone(audio, 600, 0.5);
    append_silence(audio, 1400);
    append_tone(audio, 600, 0.5);
    append_silence(audio, 1400);

    std::vector<VadItem> items;
    vad.process(audio.data(), audio.size(), items);
    KD_INFO(trace(items));
    KD_CHECK_EQ(count_of(items, VadItemKind::UtteranceEnd), size_t{2});
    KD_CHECK_EQ(count_of(items, VadItemKind::SpeechStart), size_t{2});
    KD_CHECK_EQ(vad.utterance_index(), uint64_t{2});
}

KD_TEST(asrd_vad, the_same_audio_split_differently_produces_the_same_items) {
    // A component that is only correct at one chunking is correct by accident. The Client sends
    // 20-100 ms audio frames (docs/protocol.md §6.3) and is not obliged to be consistent about
    // it, so this is a property the session loop leans on rather than a nicety.
    const VadConfig cfg = test_config();
    std::vector<int16_t> audio;
    append_silence(audio, 400);
    append_tone(audio, 700, 0.5);
    append_silence(audio, 1200);
    append_tone(audio, 500, 0.6);
    append_silence(audio, 1200);

    std::string reference;
    for (const size_t chunk : {size_t{0}, size_t{160}, size_t{512}, size_t{513}, size_t{4096},
                               size_t{1}}) {
        VadEndpointer vad(cfg);
        std::vector<VadItem> items;
        size_t at = 0;
        while (at < audio.size()) {
            const size_t n = chunk == 0 ? audio.size() : (std::min)(chunk, audio.size() - at);
            vad.process(audio.data() + at, n, items);
            at += n;
        }
        const std::string got = trace(items);
        if (reference.empty()) {
            reference = got;
            KD_INFO("reference: " + reference);
            continue;
        }
        if (got != reference) {
            KD_FAIL("chunk=" + std::to_string(chunk) + " produced a different item stream\n" +
                    "  reference: " + reference + "\n  got:       " + got);
        } else {
            KD_CHECK(true);
        }
    }
}

KD_TEST(asrd_vad, the_frame_that_ends_an_utterance_delivers_its_audio_first) {
    // The ordering vad.h documents and vad.py leaves to its consumer. The audio of the frame on
    // which the utterance ends belongs to *that* utterance, so it must reach the backend before
    // the `final` is forced — otherwise the last frame of every utterance is silently dropped.
    const VadConfig cfg = test_config();
    VadEndpointer vad(cfg);
    std::vector<int16_t> audio;
    append_silence(audio, 400);
    append_tone(audio, 600, 0.5);
    append_silence(audio, 1200);

    std::vector<VadItem> items;
    vad.process(audio.data(), audio.size(), items);

    bool seen_end = false;
    size_t audio_after_end = 0;
    for (const VadItem& i : items) {
        if (i.kind == VadItemKind::UtteranceEnd) seen_end = true;
        else if (seen_end && i.kind == VadItemKind::Audio) audio_after_end += i.audio.size();
    }
    KD_CHECK(seen_end);
    KD_CHECK_EQ(audio_after_end, size_t{0});

    // And the onset's SpeechStart precedes the pre-roll it opened the gate for, so a session
    // that opens an utterance on SpeechStart never sees audio for an utterance it has not opened.
    bool seen_start = false;
    size_t audio_before_start = 0;
    for (const VadItem& i : items) {
        if (i.kind == VadItemKind::SpeechStart) { seen_start = true; break; }
        if (i.kind == VadItemKind::Audio) audio_before_start += i.audio.size();
    }
    KD_CHECK(seen_start);
    KD_CHECK_EQ(audio_before_start, size_t{0});
}

KD_TEST(asrd_vad, preroll_reaches_back_before_the_detected_onset) {
    // The detector is late by construction — min_speech_ms must elapse before an onset is
    // confirmed — so the gated audio has to reach back, or every utterance loses its first
    // syllable.
    VadConfig cfg = test_config();
    cfg.preroll_ms = 300.0;
    VadEndpointer vad(cfg);

    std::vector<int16_t> audio;
    append_silence(audio, 400);
    append_tone(audio, 500, 0.5);
    append_silence(audio, 1200);

    std::vector<VadItem> items;
    vad.process(audio.data(), audio.size(), items);

    uint64_t onset = 0;
    uint64_t first_audio = UINT64_MAX;
    for (const VadItem& i : items) {
        if (i.kind == VadItemKind::SpeechStart && onset == 0) onset = i.sample;
        if (i.kind == VadItemKind::Audio && first_audio == UINT64_MAX) first_audio = i.sample;
    }
    KD_REQUIRE(first_audio != UINT64_MAX);
    KD_CHECK(first_audio <= onset);
}

KD_TEST(asrd_vad, flush_closes_an_open_utterance_and_keeps_the_residue) {
    // `stop` arrives mid-word. The sub-frame residue is under 32 ms of audio, but it is the tail
    // of a word, and dropping it clips the last phoneme of the session.
    const VadConfig cfg = test_config();
    VadEndpointer vad(cfg);
    std::vector<int16_t> audio;
    append_silence(audio, 400);
    append_tone(audio, 700, 0.5);

    std::vector<VadItem> items;
    vad.process(audio.data(), audio.size(), items);
    KD_REQUIRE(vad.gate_open());

    // A partial frame's worth, left in the buffer.
    std::vector<int16_t> tail;
    append_tone(tail, 10, 0.5);
    vad.process(tail.data(), tail.size(), items);

    std::vector<VadItem> flushed;
    vad.flush(flushed);
    KD_CHECK_EQ(count_of(flushed, VadItemKind::UtteranceEnd), size_t{1});
    KD_CHECK(!vad.gate_open());
    for (const VadItem& i : flushed) {
        if (i.kind == VadItemKind::UtteranceEnd) KD_CHECK_EQ(i.reason, std::string("flush"));
    }
    KD_CHECK_GT(gated_samples(flushed), size_t{0});
}

KD_TEST(asrd_vad, flush_with_nothing_open_emits_nothing) {
    const VadConfig cfg = test_config();
    VadEndpointer vad(cfg);
    std::vector<VadItem> items;
    vad.flush(items);
    KD_CHECK(items.empty());
}

KD_TEST(asrd_vad, a_discontinuity_closes_the_utterance_and_advances_the_clock) {
    // §6.1. Ending the utterance is not optional dramatics: resetting decoder state mid-
    // utterance would make the next partial.text for that id no longer a superset of the
    // previous one, which §8.1 requires and §8.2 forbids breaking.
    const VadConfig cfg = test_config();
    VadEndpointer vad(cfg);
    std::vector<int16_t> audio;
    append_silence(audio, 400);
    append_tone(audio, 700, 0.5);

    std::vector<VadItem> items;
    vad.process(audio.data(), audio.size(), items);
    KD_REQUIRE(vad.gate_open());
    const uint64_t before = vad.position_samples();

    std::vector<VadItem> gap;
    vad.notify_discontinuity(500, "wasapi_glitch", gap);
    KD_CHECK_EQ(count_of(gap, VadItemKind::UtteranceEnd), size_t{1});
    KD_CHECK_EQ(count_of(gap, VadItemKind::Discontinuity), size_t{1});
    KD_CHECK(!vad.gate_open());
    // The lost duration advances the position counter, so this module's sample clock stays
    // aligned with the Client's timeline across the gap.
    KD_CHECK_EQ(vad.position_samples(), before + 500 * 16);
}

KD_TEST(asrd_vad, a_zero_gap_discontinuity_invents_no_duration) {
    // §6.1: gap_ms == 0 means "a gap occurred, duration unknown". Inventing a duration would put
    // every subsequent position quietly out of step with the Client's.
    const VadConfig cfg = test_config();
    VadEndpointer vad(cfg);
    std::vector<int16_t> audio;
    append_silence(audio, 400);
    std::vector<VadItem> items;
    vad.process(audio.data(), audio.size(), items);

    const uint64_t before = vad.position_samples();
    std::vector<VadItem> gap;
    vad.notify_discontinuity(0, "", gap);
    KD_CHECK_EQ(vad.position_samples(), before);
}

KD_TEST(asrd_vad, past_the_soft_limit_the_next_breath_ends_the_utterance) {
    // The cut that ordinary long-winded dictation should get, instead of the stopwatch below.
    //
    // `progress.md` §3b raised `endpoint_silence_ms` 800 → 1500 precisely so that utterance
    // boundaries land on sentences rather than on breaths. A stopwatch cut contradicts that and
    // lands wherever the clock does, which is mid-word about as often as not — and a batch model
    // handed a fragment that starts mid-word invents a plausible beginning for it, so one bad
    // boundary garbles both sides. Past the soft limit the utterance ends at the next hangover
    // boundary instead: not a sentence, but between two words, which is the part that matters.
    VadConfig cfg = test_config();
    cfg.soft_max_utterance_ms = 1000.0;
    cfg.max_utterance_ms = 3000.0;
    VadEndpointer vad(cfg);

    std::vector<int16_t> audio;
    append_silence(audio, 400);
    append_tone(audio, 700, 0.5);
    append_silence(audio, 250);  // a breath: past hangover_ms, well inside endpoint_silence_ms
    append_tone(audio, 700, 0.5);
    append_silence(audio, 250);  // and the one that lands past the soft limit
    append_tone(audio, 700, 0.5);
    append_silence(audio, 250);

    std::vector<VadItem> items;
    vad.process(audio.data(), audio.size(), items);
    KD_INFO(trace(items));

    size_t soft = 0;
    size_t hard = 0;
    size_t endpoint = 0;
    for (const VadItem& i : items) {
        if (i.kind != VadItemKind::UtteranceEnd) continue;
        if (i.reason == "soft_max_utterance") ++soft;
        if (i.reason == "max_utterance") ++hard;
        if (i.reason == "endpoint") ++endpoint;
    }
    KD_CHECK_EQ(soft, size_t{1});
    // The whole point: the stopwatch never gets to fire, and neither does a real endpoint —
    // the speaker never paused long enough for one.
    KD_CHECK_EQ(hard, size_t{0});
    KD_CHECK_EQ(endpoint, size_t{0});

    // And it lands on a breath: the close is reported at the same sample as the SpeechEnd that
    // triggered it, not at the frame the timer happened to be examined on.
    uint64_t last_speech_end = 0;
    bool matched = false;
    for (const VadItem& i : items) {
        if (i.kind == VadItemKind::SpeechEnd) last_speech_end = i.sample;
        if (i.kind == VadItemKind::UtteranceEnd && i.reason == "soft_max_utterance") {
            matched = (i.sample == last_speech_end);
        }
    }
    KD_CHECK(matched);
}

KD_TEST(asrd_vad, the_soft_limit_waits_for_a_breath_and_does_not_invent_one) {
    // Speech with no gap in it at all gets no soft cut, however long it runs. That is what the
    // stopwatch is still for, and why it cannot be deleted: the soft cut has nothing to cut on.
    VadConfig cfg = test_config();
    cfg.soft_max_utterance_ms = 500.0;
    cfg.max_utterance_ms = 0.0;  // disabled, so a soft cut is the only thing that could fire
    VadEndpointer vad(cfg);

    std::vector<int16_t> audio;
    append_silence(audio, 400);
    append_tone(audio, 4000, 0.5);  // never pauses

    std::vector<VadItem> items;
    vad.process(audio.data(), audio.size(), items);
    KD_CHECK_EQ(count_of(items, VadItemKind::UtteranceEnd), size_t{0});
}

KD_TEST(asrd_vad, the_soft_limit_must_be_coherent_with_the_two_timers_around_it) {
    VadConfig cfg;
    KD_CHECK(cfg.validate().empty());
    // Below the hangover there is no boundary to wait for.
    cfg.soft_max_utterance_ms = cfg.hangover_ms;
    KD_CHECK(!cfg.validate().empty());
    // Above the stopwatch it is dead code that reads as if it were live.
    cfg = VadConfig{};
    cfg.soft_max_utterance_ms = cfg.max_utterance_ms + 1.0;
    KD_CHECK(!cfg.validate().empty());
    // 0 disables it, which is coherent — it just leaves the stopwatch as the only answer.
    cfg = VadConfig{};
    cfg.soft_max_utterance_ms = 0.0;
    KD_CHECK(cfg.validate().empty());
}

KD_TEST(asrd_vad, the_max_utterance_valve_cuts_and_says_so) {
    // The last resort, and now genuinely a safety valve: with the soft cut in place this is
    // reachable only by audio with no quiet gap at all, which is a stuck detector rather than a
    // talkative user. The reason distinguishes it from a real endpoint in the log, because the
    // fix is different — check the threshold or the input level, not the endpoint timer.
    VadConfig cfg = test_config();
    cfg.soft_max_utterance_ms = 0.0;  // this test is about the stopwatch on its own
    cfg.max_utterance_ms = 1000.0;
    VadEndpointer vad(cfg);

    std::vector<int16_t> audio;
    append_silence(audio, 400);
    append_tone(audio, 3000, 0.5);  // never pauses

    std::vector<VadItem> items;
    vad.process(audio.data(), audio.size(), items);
    KD_INFO(trace(items));

    KD_CHECK_GT(count_of(items, VadItemKind::UtteranceEnd), size_t{1});
    bool saw_valve = false;
    for (const VadItem& i : items) {
        if (i.kind == VadItemKind::UtteranceEnd && i.reason == "max_utterance") saw_valve = true;
    }
    KD_CHECK(saw_valve);
    // The replacement utterance opens immediately, so the decoder feed stays continuous rather
    // than punching a hole while the onset re-qualifies.
    KD_CHECK_GT(vad.utterance_index(), uint64_t{1});
}

KD_TEST(asrd_vad, reset_keeps_the_position_clock) {
    // Between sessions. The clock is deliberately not restarted: `VadItem::sample` is measured
    // in it, and a consumer that assumed it restarted would put every subsequent offset a
    // session's worth of audio out.
    const VadConfig cfg = test_config();
    VadEndpointer vad(cfg);
    std::vector<int16_t> audio;
    append_silence(audio, 640);
    std::vector<VadItem> items;
    vad.process(audio.data(), audio.size(), items);

    const uint64_t pos = vad.position_samples();
    KD_CHECK_GT(pos, uint64_t{0});
    vad.reset();
    KD_CHECK_EQ(vad.position_samples(), pos);
    KD_CHECK_EQ(vad.utterance_index(), uint64_t{0});
    KD_CHECK(!vad.gate_open());
}
