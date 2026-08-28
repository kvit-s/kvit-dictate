// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// tests/test_audio.cpp — the pure DSP in AudioConverter.
//
// No microphone, no WASAPI, no device. Everything here is a buffer in and a buffer out, because
// a test that needs a working capture device is a test that gets skipped and then rots.
//
// The resampler tests are the ones that matter most, and not for tidiness. plan.md builds the
// real capture path BEFORE recording the benchmark corpus specifically so that the corpus goes
// through the product's own chain. That makes this converter a silent multiplier on every WER,
// CER and technical-term F1 number the bake-off will produce: naive decimation passes a casual
// "does audio come out" check and folds a 12 kHz component down to 4 kHz, right through the
// formant range, degrading every candidate model equally and invisibly. A frequency-response
// assertion is the only thing that separates "resampled" from "resampled correctly".

#include "kd_test.h"

#include "audio_capture.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using kd::audio::AudioConverter;
using kd::audio::AudioFormat;
using kd::audio::SampleFormat;

constexpr double kPi = 3.14159265358979323846;

AudioFormat fmt(uint32_t rate, uint16_t channels, SampleFormat f) {
    AudioFormat a;
    a.sample_rate = rate;
    a.channels = channels;
    a.format = f;
    return a;
}

// A sine at `hz`, `seconds` long, interleaved to `channels` identical copies.
std::vector<float> sine_f32(double hz, uint32_t rate, double seconds, uint16_t channels = 1,
                            double amplitude = 0.5) {
    const size_t frames = static_cast<size_t>(rate * seconds);
    std::vector<float> out(frames * channels);
    for (size_t i = 0; i < frames; ++i) {
        const double v = amplitude * std::sin(2.0 * kPi * hz * static_cast<double>(i) / rate);
        for (uint16_t c = 0; c < channels; ++c) out[i * channels + c] = static_cast<float>(v);
    }
    return out;
}

// Peak amplitude of the steady-state portion, skipping the filter's start-up transient. A FIR
// needs its group delay of samples before its output means anything, and measuring across that
// would report an attenuation the filter does not actually have.
double peak_steady(const std::vector<int16_t>& s, double skip_fraction = 0.35) {
    if (s.empty()) return 0.0;
    const size_t start = static_cast<size_t>(s.size() * skip_fraction);
    const size_t end = s.size() - s.size() / 10;
    int32_t peak = 0;
    for (size_t i = start; i < end; ++i) {
        const int32_t v = s[i] < 0 ? -static_cast<int32_t>(s[i]) : s[i];
        if (v > peak) peak = v;
    }
    return static_cast<double>(peak) / 32767.0;
}

// Goertzel: the energy at one frequency, which is all we need and far less code than an FFT.
double magnitude_at(const std::vector<int16_t>& s, double hz, uint32_t rate) {
    if (s.empty()) return 0.0;
    const double w = 2.0 * kPi * hz / rate;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0;
    double s2 = 0.0;
    for (int16_t sample : s) {
        const double x = static_cast<double>(sample) / 32767.0;
        const double s0 = x + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return std::sqrt(power > 0.0 ? power : 0.0) / (static_cast<double>(s.size()) / 2.0);
}

std::vector<int16_t> convert(const AudioFormat& in, const void* data, size_t frames) {
    AudioConverter c;
    const kd::Status st = c.configure(in);
    if (!st) return {};
    std::vector<int16_t> out;
    c.process(data, frames, out);
    c.flush(out);
    return out;
}

}  // namespace

// ═══ Format handling ════════════════════════════════════════════════════════════════════════

KD_TEST(audio, configure_accepts_the_formats_wasapi_actually_hands_us) {
    // Shared mode usually gives float32 stereo at 44.1 or 48 kHz, but USB interfaces hand out
    // 24-in-32 and some devices int16. Refusing any of these means refusing a real microphone.
    const SampleFormat formats[] = {SampleFormat::S16, SampleFormat::S24In32, SampleFormat::S32,
                                    SampleFormat::F32};
    const uint32_t rates[] = {16000, 22050, 44100, 48000, 96000};
    for (SampleFormat f : formats) {
        for (uint32_t r : rates) {
            AudioConverter c;
            KD_CHECK(static_cast<bool>(c.configure(fmt(r, 2, f))));
        }
    }
}

KD_TEST(audio, configure_rejects_nonsense_rather_than_producing_garbage) {
    AudioConverter c;
    KD_CHECK(!c.configure(fmt(0, 1, SampleFormat::F32)));
    KD_CHECK(!c.configure(fmt(48000, 0, SampleFormat::F32)));
    KD_CHECK(!c.configure(fmt(48000, 1, SampleFormat::Unsupported)));
}

KD_TEST(audio, bytes_per_frame_matches_the_container_not_the_valid_bits) {
    // 24-in-32 is the trap: 24 valid bits in a 32-bit container is FOUR bytes per sample. Reading
    // it as three desynchronises the whole buffer and produces noise that sounds like a broken
    // microphone rather than a broken cast.
    KD_CHECK_EQ(fmt(48000, 2, SampleFormat::S16).bytes_per_frame(), uint32_t{4});
    KD_CHECK_EQ(fmt(48000, 2, SampleFormat::S24In32).bytes_per_frame(), uint32_t{8});
    KD_CHECK_EQ(fmt(48000, 2, SampleFormat::S32).bytes_per_frame(), uint32_t{8});
    KD_CHECK_EQ(fmt(48000, 1, SampleFormat::F32).bytes_per_frame(), uint32_t{4});
}

// ═══ Downmix ════════════════════════════════════════════════════════════════════════════════

KD_TEST(audio, stereo_downmix_averages_rather_than_sums) {
    // Summing clips the moment both channels carry the same loud signal — which is exactly what
    // a centred voice on a stereo mic produces, so it would clip on the common case.
    const uint32_t rate = kd::audio::kTargetSampleRate;
    std::vector<float> in(rate * 2);
    for (size_t i = 0; i < rate; ++i) {
        in[i * 2 + 0] = 0.8f;
        in[i * 2 + 1] = 0.8f;
    }
    const std::vector<int16_t> out =
        convert(fmt(rate, 2, SampleFormat::F32), in.data(), rate);
    KD_REQUIRE_GT(out.size(), size_t{0});
    KD_CHECK_NEAR(peak_steady(out), 0.8, 0.02);
}

KD_TEST(audio, opposed_channels_cancel) {
    const uint32_t rate = kd::audio::kTargetSampleRate;
    std::vector<float> in(rate * 2);
    for (size_t i = 0; i < rate; ++i) {
        in[i * 2 + 0] = 0.7f;
        in[i * 2 + 1] = -0.7f;
    }
    const std::vector<int16_t> out = convert(fmt(rate, 2, SampleFormat::F32), in.data(), rate);
    KD_REQUIRE_GT(out.size(), size_t{0});
    KD_CHECK_NEAR(peak_steady(out), 0.0, 0.02);
}

KD_TEST(audio, multichannel_downmix_does_not_overflow) {
    // 5.1 with every channel at full scale. Averaging keeps this at full scale; summing would
    // reach 6x and wrap to a loud buzz.
    const uint32_t rate = kd::audio::kTargetSampleRate;
    const uint16_t ch = 6;
    std::vector<float> in(static_cast<size_t>(rate) * ch, 0.9f);
    const std::vector<int16_t> out = convert(fmt(rate, ch, SampleFormat::F32), in.data(), rate);
    KD_REQUIRE_GT(out.size(), size_t{0});
    KD_CHECK(peak_steady(out) <= 1.0);
    KD_CHECK_NEAR(peak_steady(out), 0.9, 0.03);
}

// ═══ Sample-format conversion and clipping ══════════════════════════════════════════════════

KD_TEST(audio, float_samples_beyond_full_scale_clamp_instead_of_wrapping) {
    // float32 is not bounded to [-1, 1]; a hot input or an effects chain can exceed it. Casting
    // without clamping wraps +1.5 to a large NEGATIVE int16 — an inaudible-looking bug that
    // sounds like a violent click.
    const uint32_t rate = kd::audio::kTargetSampleRate;
    std::vector<float> in(rate, 1.5f);
    for (size_t i = rate / 2; i < rate; ++i) in[i] = -1.5f;

    const std::vector<int16_t> out = convert(fmt(rate, 1, SampleFormat::F32), in.data(), rate);
    KD_REQUIRE_GT(out.size(), size_t{0});
    for (int16_t s : out) {
        KD_CHECK(s >= -32768);
        KD_CHECK(s <= 32767);
    }
    // The positive half must be near full scale and POSITIVE — the wrap bug shows up here.
    KD_CHECK_GT(out[out.size() / 4], int16_t{30000});
}

KD_TEST(audio, int16_passthrough_at_the_target_rate_is_lossless) {
    // 16 kHz mono s16 in is already the wire format. Anything other than an identity here means
    // the converter is filtering audio it has no reason to touch.
    const uint32_t rate = kd::audio::kTargetSampleRate;
    std::vector<int16_t> in(rate);
    for (size_t i = 0; i < rate; ++i) {
        in[i] = static_cast<int16_t>(8000.0 * std::sin(2.0 * kPi * 440.0 * static_cast<double>(i) / rate));
    }
    const std::vector<int16_t> out = convert(fmt(rate, 1, SampleFormat::S16), in.data(), rate);
    KD_REQUIRE_EQ(out.size(), in.size());
    bool identical = true;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != out[i]) {
            identical = false;
            break;
        }
    }
    KD_CHECK(identical);
}

// ═══ Resampling — the tests that protect every number in the bake-off ═══════════════════════

KD_TEST(audio, resampler_preserves_speech_band_tones) {
    // 1 kHz and 7 kHz both sit below the 8 kHz Nyquist of the 16 kHz target and both carry real
    // speech information (7 kHz is fricative territory — the difference between "s" and "f").
    for (double hz : {1000.0, 7000.0}) {
        const std::vector<float> in = sine_f32(hz, 48000, 0.5);
        const std::vector<int16_t> out =
            convert(fmt(48000, 1, SampleFormat::F32), in.data(), in.size());
        KD_REQUIRE_GT(out.size(), size_t{0});
        const double peak = peak_steady(out);
        if (peak < 0.4 || peak > 0.6) {
            KD_FAIL("resampling a " + std::to_string(static_cast<int>(hz)) +
                    " Hz tone changed its amplitude to " + std::to_string(peak) +
                    " (expected ~0.5); the passband is not flat");
        } else {
            KD_CHECK(true);
        }
    }
}

KD_TEST(audio, resampler_attenuates_above_nyquist_instead_of_folding_it_back) {
    // THE test. A 12 kHz tone cannot exist at 16 kHz. A correct anti-alias filter removes it; a
    // naive decimator folds it to |16000 - 12000| = 4 kHz, planting a loud artefact in the middle
    // of the speech band. Both produce output; only one produces usable audio, and the difference
    // is invisible without this measurement.
    const std::vector<float> in = sine_f32(12000.0, 48000, 0.5);
    const std::vector<int16_t> out =
        convert(fmt(48000, 1, SampleFormat::F32), in.data(), in.size());
    KD_REQUIRE_GT(out.size(), size_t{0});

    const double alias = magnitude_at(out, 4000.0, kd::audio::kTargetSampleRate);
    const double reference =
        magnitude_at(convert(fmt(48000, 1, SampleFormat::F32),
                             sine_f32(4000.0, 48000, 0.5).data(),
                             sine_f32(4000.0, 48000, 0.5).size()),
                     4000.0, kd::audio::kTargetSampleRate);

    KD_REQUIRE_GT(reference, 0.0);
    const double ratio = alias / reference;
    if (ratio > 0.1) {
        KD_FAIL("a 12 kHz input produced " + std::to_string(ratio) +
                " of full amplitude at 4 kHz — it is aliasing, not filtering. Every WER number "
                "measured through this converter would be depressed by an artefact the models "
                "never had a chance against.");
    } else {
        KD_CHECK(true);
    }
}

KD_TEST(audio, output_length_tracks_the_rate_ratio) {
    struct Case { uint32_t rate; };
    const Case cases[] = {{48000}, {44100}, {22050}, {96000}, {16000}};
    for (const Case& c : cases) {
        const std::vector<float> in = sine_f32(440.0, c.rate, 1.0);
        const std::vector<int16_t> out =
            convert(fmt(c.rate, 1, SampleFormat::F32), in.data(), in.size());
        const double expected =
            static_cast<double>(in.size()) * kd::audio::kTargetSampleRate / c.rate;
        // Within a few milliseconds: a FIR's tail and the fractional-position bookkeeping both
        // legitimately move the boundary by a handful of samples.
        const double drift = std::fabs(static_cast<double>(out.size()) - expected);
        if (drift > kd::audio::kTargetSampleRate * 0.01) {
            KD_FAIL("resampling from " + std::to_string(c.rate) + " Hz produced " +
                    std::to_string(out.size()) + " samples, expected about " +
                    std::to_string(static_cast<int64_t>(expected)));
        } else {
            KD_CHECK(true);
        }
    }
}

KD_TEST(audio, chunked_input_produces_the_same_audio_as_one_big_buffer) {
    // WASAPI delivers whatever the engine happens to have, in packets that vary run to run. If
    // the converter's answer depends on how the input was split, every recording is subtly
    // different from every other and the corpus is not reproducible. This is the property that
    // makes the resampler stateful rather than a pure function of each packet.
    const uint32_t rate = 48000;
    const std::vector<float> in = sine_f32(1000.0, rate, 0.25);

    const std::vector<int16_t> whole =
        convert(fmt(rate, 1, SampleFormat::F32), in.data(), in.size());

    AudioConverter c;
    KD_REQUIRE(static_cast<bool>(c.configure(fmt(rate, 1, SampleFormat::F32))));
    std::vector<int16_t> chunked;
    const size_t sizes[] = {480, 441, 1024, 7, 4096};
    size_t pos = 0;
    size_t k = 0;
    while (pos < in.size()) {
        const size_t n = std::min(sizes[k % 5], in.size() - pos);
        c.process(in.data() + pos, n, chunked);
        pos += n;
        ++k;
    }
    c.flush(chunked);

    KD_REQUIRE_GT(whole.size(), size_t{0});
    // Lengths may differ by a sample or two at the tail depending on where flush() lands.
    const size_t n = std::min(whole.size(), chunked.size());
    KD_CHECK_GT(n, whole.size() * 9 / 10);
    int32_t worst = 0;
    for (size_t i = 0; i < n; ++i) {
        const int32_t d = std::abs(static_cast<int32_t>(whole[i]) - chunked[i]);
        if (d > worst) worst = d;
    }
    if (worst > 8) {
        KD_FAIL("splitting the input changed the output by up to " + std::to_string(worst) +
                " LSB — the converter is not carrying its filter state across calls");
    } else {
        KD_CHECK(true);
    }
}

KD_TEST(audio, reset_clears_filter_state) {
    // After reset() the converter must behave as if freshly configured; otherwise the tail of the
    // previous utterance bleeds into the head of the next one.
    const uint32_t rate = 48000;
    const std::vector<float> loud = sine_f32(1000.0, rate, 0.1, 1, 0.9);
    const std::vector<float> quiet = sine_f32(1000.0, rate, 0.1, 1, 0.05);

    AudioConverter c;
    KD_REQUIRE(static_cast<bool>(c.configure(fmt(rate, 1, SampleFormat::F32))));
    std::vector<int16_t> scratch;
    c.process(loud.data(), loud.size(), scratch);
    c.reset();

    std::vector<int16_t> after;
    c.process(quiet.data(), quiet.size(), after);
    c.flush(after);
    KD_REQUIRE_GT(after.size(), size_t{0});
    KD_CHECK(peak_steady(after) < 0.2);
}

KD_TEST(audio, silence_in_silence_out) {
    // Trivially true and worth pinning: a filter with a mis-initialised history emits a click at
    // the start of every utterance, which a VAD then reports as speech onset.
    const uint32_t rate = 48000;
    const std::vector<float> in(rate / 10, 0.0f);
    const std::vector<int16_t> out = convert(fmt(rate, 1, SampleFormat::F32), in.data(), in.size());
    KD_REQUIRE_GT(out.size(), size_t{0});
    int32_t peak = 0;
    for (int16_t s : out) {
        const int32_t v = std::abs(static_cast<int32_t>(s));
        if (v > peak) peak = v;
    }
    KD_CHECK(peak <= 1);
}
