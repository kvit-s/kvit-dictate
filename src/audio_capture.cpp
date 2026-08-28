// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// WASAPI capture and the pure conversion chain behind it.
//
// Specification: audio_capture.h. Implements plan.md §3 Phase 1 item 1 and docs/protocol.md
// §6.1 `discontinuity` / §6.3 audio frames.
//
// The file is in three parts, in dependency order:
//   1. the pure half — AudioConverter, estimate_gap_ms(), the polyphase filter design;
//   2. WavWriter, which is pure apart from opening a file;
//   3. the WASAPI half — COM, IAudioClient, the capture thread, device notifications.
// Only part 3 needs <windows.h>, but MSVC compiles one translation unit at a time and the
// header must stay Windows-free, so the split is by section rather than by file.

#define KD_LOG_MODULE "audio"

#include "audio_capture.h"

// clang-format off
#include <windows.h>
#include <avrt.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
// clang-format on

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "util/log.h"
#include "util/utf8.h"

namespace kd::audio {
namespace {

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Part 1 — the pure half
// ═══════════════════════════════════════════════════════════════════════════════════════════

// Filter design constants. Every one of these is quoted in audio_capture.h with the measured
// response it produces; changing one invalidates that block and the Phase 1 corpus with it.
constexpr double kStopbandDb = 80.0;      // Kaiser design attenuation A
constexpr double kCutoffFraction = 0.475; // fc = this x min(fs_in, 16000); the -6 dB point
constexpr double kTransitionFraction = 0.055;  // df = this x min(fs_in, 16000)
constexpr size_t kMaxTaps = 262144;       // 1 MiB of float; only exotic rates approach it
constexpr size_t kMinTapsPerPhase = 8;

// Modified Bessel function of the first kind, order 0. Series form: the argument here never
// exceeds beta (~7.86), where the series converges in about twenty terms, so the asymptotic
// expansion that a general-purpose implementation needs is not required and would only add a
// branch that is never taken.
double bessel_i0(double x) {
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 64; ++k) {
        const double r = x / (2.0 * k);
        term *= r * r;
        sum += term;
        if (term < 1e-18 * sum) break;
    }
    return sum;
}

double sinc_pi(double x) {
    if (std::fabs(x) < 1e-12) return 1.0;
    const double t = 3.14159265358979323846 * x;
    return std::sin(t) / t;
}

uint32_t gcd_u32(uint32_t a, uint32_t b) noexcept {
    while (b != 0) {
        const uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

// Float -> int16 with saturation.
//
// Scaled by 32768, not 32767. That choice is what makes an int16 device sample survive the
// round trip through the float pipeline unchanged (-32768/32768 = -1.0f scales back to exactly
// -32768) and it maps -1.0f to INT16_MIN, which is the convention every other tool in this
// space uses. The cost is that +1.0f saturates to 32767 rather than reaching 32768, which is
// not representable anyway.
//
// The bound is applied to the *scaled* value before std::lrintf, because lrintf on an
// out-of-range argument is undefined — it does not saturate — and a driver that hands us a
// denormal-storm or a NaN would otherwise produce whatever the FPU felt like.
int16_t to_i16(float x) noexcept {
    if (std::isnan(x)) return 0;  // a NaN from a driver becomes silence, not a random sample
    const float s = x * 32768.0f;
    if (s <= -32768.0f) return static_cast<int16_t>(-32768);
    if (s > 32767.0f) return static_cast<int16_t>(32767);
    return static_cast<int16_t>(std::lrintf(s));
}

size_t bytes_per_sample(SampleFormat f) noexcept {
    switch (f) {
        case SampleFormat::S16: return 2;
        case SampleFormat::S24In32:
        case SampleFormat::S32:
        case SampleFormat::F32: return 4;
        case SampleFormat::Unsupported: break;
    }
    return 0;
}

}  // namespace

const char* to_string(SampleFormat f) noexcept {
    switch (f) {
        case SampleFormat::S16: return "S16";
        case SampleFormat::S24In32: return "S24In32";
        case SampleFormat::S32: return "S32";
        case SampleFormat::F32: return "F32";
        case SampleFormat::Unsupported: return "Unsupported";
    }
    return "Unsupported";
}

uint32_t AudioFormat::bytes_per_frame() const noexcept {
    return static_cast<uint32_t>(bytes_per_sample(format)) * channels;
}

bool AudioFormat::is_valid() const noexcept {
    return sample_rate > 0 && channels > 0 && format != SampleFormat::Unsupported;
}

std::string AudioFormat::to_string() const {
    // Built by hand rather than with std::format: MSVC 14.44 at /std:c++20 does have <format>,
    // but this is called from the capture thread's logging path and a formatter's exceptions and
    // locale lookups buy nothing over three appends.
    std::string s = std::to_string(sample_rate);
    s += " Hz, ";
    s += std::to_string(channels);
    s += " ch, ";
    s += kd::audio::to_string(format);
    return s;
}

uint32_t estimate_gap_ms(uint64_t expected_pos, uint64_t reported_pos,
                         uint32_t sample_rate) noexcept {
    if (sample_rate == 0) return 0;
    if (expected_pos == 0) return 0;          // no previous packet to measure against
    if (reported_pos <= expected_pos) return 0;  // no advance, or a backwards device position
    const uint64_t frames = reported_pos - expected_pos;
    // Split the multiply so a long gap cannot overflow: frames/rate is the whole-second part and
    // the remainder is strictly below rate, so (rem * 1000) stays far inside 64 bits.
    const uint64_t whole = (frames / sample_rate) * 1000ull;
    const uint64_t rem = frames % sample_rate;
    const uint64_t ms = whole + (rem * 1000ull + sample_rate / 2) / sample_rate;
    return ms > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(ms);
}

// ─── AudioConverter ─────────────────────────────────────────────────────────────────────────

struct AudioConverter::Impl {
    AudioFormat in;
    bool configured = false;
    bool passthrough = false;

    // Rational resampling state. See the derivation in advance() below.
    uint32_t L = 1;
    uint32_t M = 1;
    size_t taps_per_phase = 0;
    std::vector<float> phases;  // phases[p * taps_per_phase + m] = h[p + m*L]
    std::vector<float> hist;    // input history; hist[base] is the newest tap for the next output
    int64_t base = 0;
    uint32_t phase = 0;
    double group_delay_ms = 0.0;

    std::vector<float> mono;  // scratch, reused across calls to keep process() allocation-free

    void design(uint32_t fs_in);
    void reset_state();
    void downmix(const void* frames, size_t frame_count);
    void run_filter(std::vector<int16_t>& out);
};

void AudioConverter::Impl::design(uint32_t fs_in) {
    const uint32_t g = gcd_u32(fs_in, kTargetSampleRate);
    L = kTargetSampleRate / g;
    M = fs_in / g;

    const double fs_i = static_cast<double>(fs_in) * static_cast<double>(L);
    const double lo = static_cast<double>(std::min(fs_in, kTargetSampleRate));
    const double fc = kCutoffFraction * lo;
    const double df = kTransitionFraction * lo;

    // Kaiser's length estimate: N ~= (A - 8) / (2.285 * dw), dw = 2*pi*df/fs_i.
    const double dw = 2.0 * 3.14159265358979323846 * df / fs_i;
    double n_est = (kStopbandDb - 8.0) / (2.285 * dw);
    if (!(n_est > 0.0)) n_est = 1.0;

    size_t n = static_cast<size_t>(std::ceil(n_est));
    if (n < kMinTapsPerPhase * L) n = kMinTapsPerPhase * L;
    if (n > kMaxTaps) {
        // Only reachable for rates whose gcd with 16000 is tiny (11025 Hz and friends). The
        // filter is then shorter than the design asks for, which widens the transition band
        // rather than lowering the stopband — audible as a little more energy just under 8 kHz,
        // never as an alias in the middle of the speech band. Recorded rather than hidden.
        KD_LOG_WARN("resampler tap count capped at %zu for %u Hz (wanted %zu); transition band "
                    "is wider than the documented design",
                    kMaxTaps, fs_in, n);
        n = kMaxTaps;
    }
    // Round up to a whole number of phases so every polyphase branch has the same length.
    n = ((n + L - 1) / L) * L;
    taps_per_phase = n / L;

    const double beta = 0.1102 * (kStopbandDb - 8.7);
    const double i0_beta = bessel_i0(beta);
    const double c = 2.0 * fc / fs_i;  // normalised cutoff of the prototype
    const double center = (static_cast<double>(n) - 1.0) / 2.0;

    std::vector<double> h(n);
    double sum = 0.0;
    for (size_t k = 0; k < n; ++k) {
        const double x = c * (static_cast<double>(k) - center);
        const double r = (2.0 * static_cast<double>(k) / (static_cast<double>(n) - 1.0)) - 1.0;
        const double w = bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0_beta;
        h[k] = c * sinc_pi(x) * w;
        sum += h[k];
    }
    // Normalise to a total of L, not 1. The zero-stuffed interpolation path only sees one input
    // sample per L intermediate samples, so a prototype summing to 1 would give a DC gain of
    // 1/L — a silent (L-1)/L attenuation that looks exactly like a quiet microphone.
    const double norm = (sum != 0.0) ? (static_cast<double>(L) / sum) : 1.0;

    phases.assign(static_cast<size_t>(L) * taps_per_phase, 0.0f);
    for (uint32_t p = 0; p < L; ++p) {
        for (size_t m = 0; m < taps_per_phase; ++m) {
            const size_t k = static_cast<size_t>(p) + m * L;
            if (k < n) {
                phases[static_cast<size_t>(p) * taps_per_phase + m] =
                    static_cast<float>(h[k] * norm);
            }
        }
    }

    group_delay_ms = (static_cast<double>(n) - 1.0) / 2.0 / fs_i * 1000.0;
}

void AudioConverter::Impl::reset_state() {
    if (passthrough || taps_per_phase == 0) return;
    // hist[0 .. taps_per_phase-2] is the pre-roll of zeros, so hist[taps_per_phase-1] is input
    // sample 0 and output 0 is centred on it exactly as the design intends.
    hist.assign(taps_per_phase - 1, 0.0f);
    base = static_cast<int64_t>(taps_per_phase) - 1;
    phase = 0;
}

void AudioConverter::Impl::downmix(const void* frames, size_t frame_count) {
    mono.resize(frame_count);
    const uint16_t ch = in.channels;
    const float inv_ch = 1.0f / static_cast<float>(ch);

    // The average, never channel 0: a headset that presents a stereo capture endpoint with
    // silence on one channel would otherwise be 6 dB quiet or entirely silent depending on
    // which channel the driver filled (audio_capture.h, process()).
    switch (in.format) {
        case SampleFormat::S16: {
            const auto* p = static_cast<const int16_t*>(frames);
            for (size_t i = 0; i < frame_count; ++i) {
                float acc = 0.0f;
                for (uint16_t c = 0; c < ch; ++c) acc += static_cast<float>(p[i * ch + c]);
                mono[i] = acc * inv_ch * (1.0f / 32768.0f);
            }
            break;
        }
        case SampleFormat::S24In32:
        case SampleFormat::S32: {
            // 24-in-32 is handled as 32-bit on purpose. WAVEFORMATEXTENSIBLE places the valid
            // bits in the *most* significant bits of the container, so the low 8 bits of a
            // 24-in-32 sample are zero and dividing by 2^31 is exact — no separate case, and no
            // shift that would be wrong if a driver ever disagreed about justification. If a
            // device did right-justify, the symptom would be a uniform ~48 dB of attenuation,
            // which is obvious rather than subtle.
            const auto* p = static_cast<const int32_t*>(frames);
            for (size_t i = 0; i < frame_count; ++i) {
                float acc = 0.0f;
                for (uint16_t c = 0; c < ch; ++c) {
                    acc += static_cast<float>(p[i * ch + c]) * (1.0f / 2147483648.0f);
                }
                mono[i] = acc * inv_ch;
            }
            break;
        }
        case SampleFormat::F32: {
            const auto* p = static_cast<const float*>(frames);
            for (size_t i = 0; i < frame_count; ++i) {
                float acc = 0.0f;
                for (uint16_t c = 0; c < ch; ++c) acc += p[i * ch + c];
                mono[i] = acc * inv_ch;
            }
            break;
        }
        case SampleFormat::Unsupported:
            std::fill(mono.begin(), mono.end(), 0.0f);
            break;
    }
}

void AudioConverter::Impl::run_filter(std::vector<int16_t>& out) {
    // Polyphase decimation, derived once so the index arithmetic below is checkable:
    //
    //   y[n] = sum_k h[k] * xup[n*M - k], where xup[j] = x[j/L] when L divides j, else 0.
    //   Write n*M = base*L + p with p = (n*M) mod L. Then only k congruent to p mod L survive,
    //   so k = p + m*L and the surviving input index is x[base - m].
    //
    // Advancing n by one adds M to n*M, hence: p += M; base += p / L; p %= L.
    hist.insert(hist.end(), mono.begin(), mono.end());

    const size_t tpp = taps_per_phase;
    const auto avail = static_cast<int64_t>(hist.size());
    while (base < avail) {
        const float* row = &phases[static_cast<size_t>(phase) * tpp];
        const float* x = &hist[static_cast<size_t>(base)];
        float acc = 0.0f;
        for (size_t m = 0; m < tpp; ++m) acc += row[m] * x[-static_cast<ptrdiff_t>(m)];
        out.push_back(to_i16(acc));

        phase += M;
        base += phase / L;
        phase %= L;
    }

    // Drop everything older than the oldest tap the next output will read. The clamp cannot
    // trigger with any design this file produces (taps_per_phase is always far larger than M,
    // so base can overshoot the buffer end by at most M), but an unclamped erase() length would
    // be undefined behaviour if it ever did.
    const int64_t keep_from =
        std::min<int64_t>(base - static_cast<int64_t>(tpp) + 1, static_cast<int64_t>(hist.size()));
    if (keep_from > 0) {
        hist.erase(hist.begin(), hist.begin() + static_cast<ptrdiff_t>(keep_from));
        base -= keep_from;
    }
}

AudioConverter::AudioConverter() : impl_(new Impl()) {}
AudioConverter::~AudioConverter() { delete impl_; }

Status AudioConverter::configure(const AudioFormat& in) {
    if (!in.is_valid()) {
        return Status{StatusCode::Unsupported,
                      "unsupported capture format: " + in.to_string()};
    }
    if (in.sample_rate < 8000 || in.sample_rate > 384000) {
        return Status{StatusCode::Unsupported,
                      "capture sample rate out of range: " + in.to_string()};
    }
    if (in.channels > 32) {
        return Status{StatusCode::Unsupported, "too many capture channels: " + in.to_string()};
    }

    impl_->in = in;
    impl_->passthrough = (in.sample_rate == kTargetSampleRate);
    if (impl_->passthrough) {
        // Recognised as a pass-through so a 16 kHz-native device is not degraded by a round trip
        // through the filter. Downmix and integer conversion still happen; both are exact for a
        // mono s16 device, which is the case this exists for.
        impl_->phases.clear();
        impl_->hist.clear();
        impl_->taps_per_phase = 0;
        impl_->L = 1;
        impl_->M = 1;
        impl_->group_delay_ms = 0.0;
    } else {
        impl_->design(in.sample_rate);
    }
    impl_->reset_state();
    impl_->configured = true;
    KD_LOG_DEBUG("converter configured: %s -> 16000 Hz mono s16 (L=%u M=%u taps/phase=%zu)",
                 in.to_string().c_str(), impl_->L, impl_->M, impl_->taps_per_phase);
    return Status::success();
}

const AudioFormat& AudioConverter::input_format() const noexcept { return impl_->in; }

void AudioConverter::process(const void* frames, size_t frame_count, std::vector<int16_t>& out) {
    if (!impl_->configured || frame_count == 0) return;
    if (frames == nullptr) return;

    impl_->downmix(frames, frame_count);
    if (impl_->passthrough) {
        out.reserve(out.size() + frame_count);
        for (float v : impl_->mono) out.push_back(to_i16(v));
        return;
    }
    out.reserve(out.size() + static_cast<size_t>(static_cast<double>(frame_count) * ratio()) + 2);
    impl_->run_filter(out);
}

void AudioConverter::flush(std::vector<int16_t>& out) {
    if (!impl_->configured || impl_->passthrough) return;
    // Push the group delay's worth of zeros through, which walks the last real input sample out
    // to the centre of the filter. Feeding the whole delay line would append a further half
    // window of the filter's ring-down, which is not audio anyone captured.
    const size_t zeros = impl_->taps_per_phase / 2;
    impl_->mono.assign(zeros, 0.0f);
    impl_->run_filter(out);
}

void AudioConverter::reset() { impl_->reset_state(); }

double AudioConverter::ratio() const noexcept {
    if (!impl_->configured || impl_->in.sample_rate == 0) return 0.0;
    return static_cast<double>(kTargetSampleRate) / static_cast<double>(impl_->in.sample_rate);
}

double AudioConverter::group_delay_ms() const noexcept { return impl_->group_delay_ms; }

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Part 2 — WavWriter
// ═══════════════════════════════════════════════════════════════════════════════════════════

namespace {

constexpr uint32_t kRiffSizeLimit = 0xFFFFFF00u;  // leave headroom for the header

void put_u16(std::string& s, uint16_t v) {
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
}

void put_u32(std::string& s, uint32_t v) {
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
    s.push_back(static_cast<char>((v >> 16) & 0xFF));
    s.push_back(static_cast<char>((v >> 24) & 0xFF));
}

// Standard channel masks, so a 6-channel file says "5.1" rather than "six unlabelled channels".
uint32_t default_channel_mask(uint16_t channels) noexcept {
    switch (channels) {
        case 1: return 0x00004;  // FRONT_CENTER
        case 2: return 0x00003;  // FRONT_LEFT | FRONT_RIGHT
        case 4: return 0x00033;  // quad
        case 6: return 0x0003F;  // 5.1
        case 8: return 0x0063F;  // 7.1
        default: return 0;       // 0 is legal and means "unspecified", which is honest
    }
}

}  // namespace

struct WavWriter::Impl {
    std::ofstream file;
    AudioFormat fmt;
    uint64_t frames = 0;
    uint64_t data_bytes = 0;
    std::streamoff data_size_pos = 0;  // byte offset of the data chunk's 32-bit size field
    uint32_t header_bytes = 0;
    bool open = false;
};

WavWriter::WavWriter() : impl_(new Impl()) {}

WavWriter::~WavWriter() {
    // The destructor closes so a thrown-past or forgotten writer still leaves a readable file,
    // but it cannot report a failure — that is what the explicit close() is for.
    (void)close();
    delete impl_;
}

Status WavWriter::open(const std::string& utf8_path, const AudioFormat& fmt) {
    (void)close();

    if (!fmt.is_valid()) {
        return Status{StatusCode::InvalidArgument, "WavWriter: invalid format " + fmt.to_string()};
    }

    std::wstring wpath;
    size_t bad = 0;
    if (!utf8::to_utf16(utf8_path, wpath, &bad)) {
        return Status{StatusCode::InvalidArgument,
                      "WavWriter: path is not well-formed UTF-8 at byte " + std::to_string(bad)};
    }

    impl_->file.open(std::filesystem::path(wpath),
                     std::ios::binary | std::ios::out | std::ios::trunc);
    if (!impl_->file) {
        return Status{StatusCode::NotFound, "WavWriter: cannot open " + utf8_path,
                      static_cast<uint32_t>(GetLastError())};
    }

    uint16_t bits = 0;
    uint16_t valid_bits = 0;
    uint16_t tag = 0;
    switch (fmt.format) {
        case SampleFormat::S16: tag = WAVE_FORMAT_PCM; bits = 16; valid_bits = 16; break;
        case SampleFormat::S32: tag = WAVE_FORMAT_PCM; bits = 32; valid_bits = 32; break;
        case SampleFormat::S24In32: tag = WAVE_FORMAT_PCM; bits = 32; valid_bits = 24; break;
        case SampleFormat::F32: tag = WAVE_FORMAT_IEEE_FLOAT; bits = 32; valid_bits = 32; break;
        case SampleFormat::Unsupported: break;
    }

    // Extensible only when the plain chunk cannot express the truth: more than two channels has
    // no defined speaker order without a channel mask, and 24 valid bits in a 32-bit container
    // has nowhere to record "24" at all.
    const bool extensible = (fmt.channels > 2) || (valid_bits != bits);

    const uint16_t block_align = static_cast<uint16_t>(fmt.bytes_per_frame());
    const uint32_t byte_rate = fmt.sample_rate * block_align;
    const uint32_t fmt_chunk_size = extensible ? 40u : 16u;

    std::string h;
    h.reserve(60);
    h += "RIFF";
    put_u32(h, 0);  // patched on close
    h += "WAVE";
    h += "fmt ";
    put_u32(h, fmt_chunk_size);
    put_u16(h, extensible ? static_cast<uint16_t>(WAVE_FORMAT_EXTENSIBLE) : tag);
    put_u16(h, fmt.channels);
    put_u32(h, fmt.sample_rate);
    put_u32(h, byte_rate);
    put_u16(h, block_align);
    put_u16(h, bits);
    if (extensible) {
        put_u16(h, 22);  // cbSize
        put_u16(h, valid_bits);
        put_u32(h, default_channel_mask(fmt.channels));
        // KSDATAFORMAT_SUBTYPE_<tag>: the well-known {tag}-0000-0010-8000-00aa00389b71 template.
        // Written out by value rather than pulled from <ksmedia.h> — see the note on the same
        // GUIDs in the WASAPI section below.
        put_u32(h, tag);
        put_u16(h, 0x0000);
        put_u16(h, 0x0010);
        const unsigned char tail[8] = {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        h.append(reinterpret_cast<const char*>(tail), sizeof(tail));
    }
    h += "data";
    put_u32(h, 0);  // patched on close

    impl_->data_size_pos = static_cast<std::streamoff>(h.size() - 4);
    impl_->header_bytes = static_cast<uint32_t>(h.size());
    impl_->file.write(h.data(), static_cast<std::streamsize>(h.size()));
    if (!impl_->file) {
        impl_->file.close();
        return Status{StatusCode::OsError, "WavWriter: header write failed for " + utf8_path};
    }

    impl_->fmt = fmt;
    impl_->frames = 0;
    impl_->data_bytes = 0;
    impl_->open = true;
    return Status::success();
}

Status WavWriter::write(const void* frames, size_t frame_count) {
    if (!impl_->open) return Status{StatusCode::Unavailable, "WavWriter: not open"};
    if (frame_count == 0) return Status::success();
    if (frames == nullptr) return Status{StatusCode::InvalidArgument, "WavWriter: null frames"};

    const uint64_t n = static_cast<uint64_t>(frame_count) * impl_->fmt.bytes_per_frame();
    if (impl_->data_bytes + n > kRiffSizeLimit) {
        // Refuse rather than wrap: a RIFF size field that overflows produces a file that opens
        // and plays back as noise, which is the worst of the three possible outcomes.
        return Status{StatusCode::Overloaded,
                      "WavWriter: RIFF 4 GiB limit reached; start a new file"};
    }

    impl_->file.write(static_cast<const char*>(frames), static_cast<std::streamsize>(n));
    if (!impl_->file) return Status{StatusCode::OsError, "WavWriter: data write failed"};

    impl_->data_bytes += n;
    impl_->frames += frame_count;
    return Status::success();
}

Status WavWriter::close() {
    if (!impl_->open) return Status::success();
    impl_->open = false;

    Status result = Status::success();

    // RIFF chunks are word-aligned. Every format this writer accepts has an even frame size, so
    // the pad byte is unreachable today; it is written anyway because "unreachable" is a
    // property of the current SampleFormat set, not of RIFF.
    if ((impl_->data_bytes & 1u) != 0) {
        const char pad = 0;
        impl_->file.write(&pad, 1);
    }

    const uint32_t data_size = static_cast<uint32_t>(impl_->data_bytes);
    const uint32_t riff_size =
        impl_->header_bytes - 8u + data_size + static_cast<uint32_t>(impl_->data_bytes & 1u);

    std::string tmp;
    tmp.reserve(4);

    impl_->file.seekp(4, std::ios::beg);
    put_u32(tmp, riff_size);
    impl_->file.write(tmp.data(), 4);

    tmp.clear();
    impl_->file.seekp(impl_->data_size_pos, std::ios::beg);
    put_u32(tmp, data_size);
    impl_->file.write(tmp.data(), 4);

    if (!impl_->file) result = Status{StatusCode::OsError, "WavWriter: could not patch sizes"};
    impl_->file.close();
    if (!result.is_ok()) return result;
    if (impl_->file.fail()) return Status{StatusCode::OsError, "WavWriter: close failed"};
    return Status::success();
}

bool WavWriter::is_open() const noexcept { return impl_->open; }
uint64_t WavWriter::frames_written() const noexcept { return impl_->frames; }
const AudioFormat& WavWriter::format() const noexcept { return impl_->fmt; }

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Part 3 — the WASAPI half
// ═══════════════════════════════════════════════════════════════════════════════════════════

namespace {

// KSDATAFORMAT_SUBTYPE_PCM and _IEEE_FLOAT, by value.
//
// The alternative is <ksmedia.h> plus ksuser.lib, which CMakeLists.txt links and whose comment
// says to delete that line if the GUIDs are compared by value instead. They are compared by
// value here: ksmedia.h drags in ks.h and a large kernel-streaming surface for two constants
// that are simply the WAVE_FORMAT tag dropped into the well-known media-subtype template
// {xxxxxxxx-0000-0010-8000-00AA00389B71}. That template is documented and fixed.
constexpr GUID kSubtypePcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr GUID kSubtypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

// PKEY_Device_FriendlyName, likewise by value.
//
// <functiondiscoverykeys_devpkey.h> only *declares* it unless INITGUID is defined before the
// include, and no import library defines it — the usual fix is to nominate one translation unit
// as the definition site with <initguid.h>. Doing that here would also emit definitions for
// every other GUID this TU's headers declare, which is how two translation units end up with
// duplicate symbols the first time somebody else includes <mmdeviceapi.h>. A file-local
// constant has none of that reach.
constexpr PROPERTYKEY kPkeyDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

bool guid_equal(const GUID& a, const GUID& b) noexcept {
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

// ─── RAII wrappers ──────────────────────────────────────────────────────────────────────────
//
// Every acquired resource is released on every path. None of these are copyable; all of them
// release in the destructor, so an early return from the middle of the capture thread's setup
// cannot leak a COM reference, an event handle, or an MMCSS registration.

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) {
            reset();
            p_ = o.p_;
            o.p_ = nullptr;
        }
        return *this;
    }

    T** put() noexcept {
        reset();
        return &p_;
    }
    void** put_void() noexcept {
        reset();
        return reinterpret_cast<void**>(&p_);
    }
    [[nodiscard]] T* get() const noexcept { return p_; }
    T* operator->() const noexcept { return p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }
    void reset() noexcept {
        if (p_ != nullptr) {
            p_->Release();
            p_ = nullptr;
        }
    }

private:
    T* p_ = nullptr;
};

class EventHandle {
public:
    EventHandle() = default;
    ~EventHandle() { reset(); }
    EventHandle(const EventHandle&) = delete;
    EventHandle& operator=(const EventHandle&) = delete;

    bool create(bool manual_reset) noexcept {
        reset();
        h_ = CreateEventW(nullptr, manual_reset ? TRUE : FALSE, FALSE, nullptr);
        return h_ != nullptr;
    }
    [[nodiscard]] HANDLE get() const noexcept { return h_; }
    void set() const noexcept {
        if (h_ != nullptr) SetEvent(h_);
    }
    void reset() noexcept {
        if (h_ != nullptr) {
            CloseHandle(h_);
            h_ = nullptr;
        }
    }

private:
    HANDLE h_ = nullptr;
};

struct CoTaskMemDeleter {
    void operator()(void* p) const noexcept {
        if (p != nullptr) CoTaskMemFree(p);
    }
};
using CoMemWfx = std::unique_ptr<WAVEFORMATEX, CoTaskMemDeleter>;
using CoMemWide = std::unique_ptr<WCHAR, CoTaskMemDeleter>;

class PropVar {
public:
    PropVar() { PropVariantInit(&v_); }
    ~PropVar() { PropVariantClear(&v_); }
    PropVar(const PropVar&) = delete;
    PropVar& operator=(const PropVar&) = delete;
    PROPVARIANT* put() noexcept { return &v_; }
    [[nodiscard]] const PROPVARIANT& get() const noexcept { return v_; }

private:
    PROPVARIANT v_{};
};

// COM apartment for one scope.
//
// The three return values matter and are usually collapsed into one: S_OK means we opened the
// apartment and must close it; S_FALSE means the thread already had one of the same kind and we
// must *still* CoUninitialize to balance the reference; RPC_E_CHANGED_MODE means the thread is
// an STA (the UI thread) and the apartment is not ours to close — uninitialising it there would
// tear down somebody else's COM.
class ComApartment {
public:
    ComApartment() {
        hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owns_ = SUCCEEDED(hr_);
    }
    ~ComApartment() {
        if (owns_) CoUninitialize();
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    [[nodiscard]] bool usable() const noexcept {
        return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE;
    }
    [[nodiscard]] HRESULT hr() const noexcept { return hr_; }

private:
    HRESULT hr_ = E_FAIL;
    bool owns_ = false;
};

class MmcssTask {
public:
    MmcssTask() {
        DWORD index = 0;
        h_ = AvSetMmThreadCharacteristicsW(L"Audio", &index);
    }
    ~MmcssTask() {
        if (h_ != nullptr) AvRevertMmThreadCharacteristics(h_);
    }
    MmcssTask(const MmcssTask&) = delete;
    MmcssTask& operator=(const MmcssTask&) = delete;
    [[nodiscard]] bool ok() const noexcept { return h_ != nullptr; }

private:
    HANDLE h_ = nullptr;
};

// Guarantees IAudioClient::Stop() runs even if the loop leaves by an error path.
class ClientRunGuard {
public:
    explicit ClientRunGuard(IAudioClient* c) noexcept : c_(c) {}
    ~ClientRunGuard() {
        if (c_ != nullptr) c_->Stop();
    }
    ClientRunGuard(const ClientRunGuard&) = delete;
    ClientRunGuard& operator=(const ClientRunGuard&) = delete;

private:
    IAudioClient* c_ = nullptr;
};

// Guarantees ReleaseBuffer() matches every successful GetBuffer(). WASAPI leaves the packet
// locked otherwise, and the next GetBuffer returns AUDCLNT_E_OUT_OF_ORDER forever.
class PacketGuard {
public:
    PacketGuard(IAudioCaptureClient* c, UINT32 frames) noexcept : c_(c), frames_(frames) {}
    ~PacketGuard() {
        if (c_ != nullptr) c_->ReleaseBuffer(frames_);
    }
    PacketGuard(const PacketGuard&) = delete;
    PacketGuard& operator=(const PacketGuard&) = delete;

private:
    IAudioCaptureClient* c_ = nullptr;
    UINT32 frames_ = 0;
};

// ─── Format discovery ───────────────────────────────────────────────────────────────────────

// Never assume 44.1/48 kHz stereo float: shared-mode mix formats on this machine alone include
// 2-channel float32 at 48 kHz (most USB headsets), 1-channel int16 at 16 kHz (some webcams),
// and 24-in-32 on interfaces with a real ADC. Anything not in the table is refused by name
// rather than guessed at — a wrong guess about bit depth is either white noise (obvious) or a
// 6 dB level error (not obvious at all).
AudioFormat format_from_wfx(const WAVEFORMATEX* wfx) {
    AudioFormat f;
    if (wfx == nullptr) return f;
    f.sample_rate = wfx->nSamplesPerSec;
    f.channels = wfx->nChannels;

    uint16_t container_bits = wfx->wBitsPerSample;
    uint16_t valid_bits = container_bits;
    bool is_float = false;
    bool is_pcm = false;

    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        valid_bits = ext->Samples.wValidBitsPerSample;
        if (valid_bits == 0) valid_bits = container_bits;
        is_float = guid_equal(ext->SubFormat, kSubtypeIeeeFloat);
        is_pcm = guid_equal(ext->SubFormat, kSubtypePcm);
    } else if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        is_float = true;
    } else if (wfx->wFormatTag == WAVE_FORMAT_PCM) {
        is_pcm = true;
    }

    if (is_float && container_bits == 32) {
        f.format = SampleFormat::F32;
    } else if (is_pcm && container_bits == 16) {
        f.format = SampleFormat::S16;
    } else if (is_pcm && container_bits == 32) {
        f.format = (valid_bits == 24) ? SampleFormat::S24In32 : SampleFormat::S32;
    } else {
        // Packed 24-bit (wBitsPerSample == 24) lands here deliberately. It is not a shared-mode
        // mix format on Windows — the engine always presents a 32-bit container — so supporting
        // it would be untested code guarding an unreachable case.
        f.format = SampleFormat::Unsupported;
    }
    return f;
}

std::string wide_to_utf8(const wchar_t* w) {
    if (w == nullptr) return {};
    return utf8::from_utf16_or_empty(std::wstring_view(w));
}

DeviceInfo describe_device(IMMDevice* dev, bool is_default) {
    DeviceInfo info;
    info.is_default = is_default;
    if (dev == nullptr) return info;

    CoMemWide id;
    {
        WCHAR* raw = nullptr;
        if (SUCCEEDED(dev->GetId(&raw))) {
            id.reset(raw);
            info.id = wide_to_utf8(id.get());
        }
    }

    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.put()))) {
        PropVar v;
        if (SUCCEEDED(props->GetValue(kPkeyDeviceFriendlyName, v.put())) &&
            v.get().vt == VT_LPWSTR) {
            info.friendly_name = wide_to_utf8(v.get().pwszVal);
        }
    }

    ComPtr<IAudioClient> client;
    if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void()))) {
        WAVEFORMATEX* raw = nullptr;
        if (SUCCEEDED(client->GetMixFormat(&raw))) {
            CoMemWfx mix(raw);
            info.mix_format = format_from_wfx(mix.get());
        }
    }
    return info;
}

Status hresult_status(const char* what, HRESULT hr) {
    std::string msg = what;
    msg += " failed (hr=0x";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08lX", static_cast<unsigned long>(hr));
    msg += buf;
    msg += ")";
    const StatusCode code = (hr == AUDCLNT_E_DEVICE_INVALIDATED ||
                             hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
                                ? StatusCode::Unavailable
                                : StatusCode::OsError;
    return Status{code, std::move(msg), static_cast<uint32_t>(hr)};
}

// ─── Device-change notifications ────────────────────────────────────────────────────────────
//
// Every method here runs on an MMDevice-owned thread, not ours. Two hard rules follow, both of
// them documented by Microsoft and both of them silent-deadlock bugs when broken:
//   * do not block — the audio service is waiting on this call;
//   * do not call back into IMMDeviceEnumerator from inside it.
// So the callbacks set two atomics and signal an event. Everything that needs the enumerator
// (working out which device is now the default, for instance) happens on the capture thread
// after it wakes.
class DeviceNotifier final : public IMMNotificationClient {
public:
    DeviceNotifier(HANDLE signal, std::wstring pinned_id)
        : signal_(signal), pinned_(std::move(pinned_id)) {}

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(refs_.fetch_add(1, std::memory_order_relaxed) + 1);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const long n = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0) delete this;
        return static_cast<ULONG>(n);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role,
                                                     LPCWSTR) override {
        // Only when we are following the default. A pinned device does not care that the user
        // switched their default output, and eMultimedia/eCommunications fire separately for the
        // same physical change — reacting to all three would restart the capture three times.
        if (pinned_.empty() && flow == eCapture && role == eConsole) {
            changed_.store(true, std::memory_order_release);
            if (signal_ != nullptr) SetEvent(signal_);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
        if (matches(id) && state != DEVICE_STATE_ACTIVE) signal_lost();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {
        if (matches(id)) signal_lost();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        return S_OK;
    }

    [[nodiscard]] bool changed() const noexcept {
        return changed_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool lost() const noexcept { return lost_.load(std::memory_order_acquire); }

    // The id of the device actually in use, set by the capture thread once it has opened one.
    // Written before the notifier is registered and read only from the notification thread.
    void set_active_id(std::wstring id) { active_ = std::move(id); }

private:
    bool matches(LPCWSTR id) const {
        if (id == nullptr) return false;
        if (!pinned_.empty()) return pinned_ == id;
        return !active_.empty() && active_ == id;
    }
    void signal_lost() {
        lost_.store(true, std::memory_order_release);
        changed_.store(true, std::memory_order_release);
        if (signal_ != nullptr) SetEvent(signal_);
    }

    std::atomic<long> refs_{1};
    HANDLE signal_ = nullptr;  // not owned
    std::wstring pinned_;
    std::wstring active_;
    std::atomic<bool> changed_{false};
    std::atomic<bool> lost_{false};
};

// Unregisters before releasing. The order matters: releasing a registered notifier leaves the
// audio service holding a pointer into freed memory the next time a device changes.
class NotifierRegistration {
public:
    NotifierRegistration(IMMDeviceEnumerator* e, DeviceNotifier* n) : e_(e), n_(n) {
        registered_ = (e_ != nullptr && n_ != nullptr &&
                       SUCCEEDED(e_->RegisterEndpointNotificationCallback(n_)));
    }
    ~NotifierRegistration() {
        if (registered_) e_->UnregisterEndpointNotificationCallback(n_);
        if (n_ != nullptr) n_->Release();
    }
    NotifierRegistration(const NotifierRegistration&) = delete;
    NotifierRegistration& operator=(const NotifierRegistration&) = delete;
    [[nodiscard]] bool ok() const noexcept { return registered_; }

private:
    IMMDeviceEnumerator* e_ = nullptr;
    DeviceNotifier* n_ = nullptr;
    bool registered_ = false;
};

int64_t qpc_frequency() noexcept {
    static const int64_t f = [] {
        LARGE_INTEGER li{};
        QueryPerformanceFrequency(&li);  // documented never to fail on XP or later
        return li.QuadPart != 0 ? li.QuadPart : 10000000LL;
    }();
    return f;
}

int64_t qpc_now() noexcept {
    LARGE_INTEGER li{};
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

// IAudioCaptureClient reports u64QPCPosition as the performance counter *converted to
// 100-nanosecond units*, not as raw counter ticks. AudioChunk::qpc_first_sample is documented as
// a raw QPC value, so the units are converted back here rather than two different clocks being
// mixed under one field name.
int64_t qpc_from_100ns(uint64_t hns) noexcept {
    const uint64_t f = static_cast<uint64_t>(qpc_frequency());
    const uint64_t whole = (hns / 10000000ull) * f;
    const uint64_t rem = (hns % 10000000ull) * f / 10000000ull;
    return static_cast<int64_t>(whole + rem);
}

}  // namespace

Result<std::vector<DeviceInfo>> enumerate_capture_devices() {
    ComApartment com;
    if (!com.usable()) return hresult_status("CoInitializeEx", com.hr());

    ComPtr<IMMDeviceEnumerator> enumr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), enumr.put_void());
    if (FAILED(hr)) return hresult_status("CoCreateInstance(MMDeviceEnumerator)", hr);

    std::string default_id;
    {
        ComPtr<IMMDevice> def;
        if (SUCCEEDED(enumr->GetDefaultAudioEndpoint(eCapture, eConsole, def.put()))) {
            WCHAR* raw = nullptr;
            if (SUCCEEDED(def->GetId(&raw))) {
                CoMemWide id(raw);
                default_id = wide_to_utf8(id.get());
            }
        }
    }

    ComPtr<IMMDeviceCollection> coll;
    hr = enumr->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, coll.put());
    if (FAILED(hr)) return hresult_status("EnumAudioEndpoints", hr);

    UINT count = 0;
    hr = coll->GetCount(&count);
    if (FAILED(hr)) return hresult_status("IMMDeviceCollection::GetCount", hr);

    std::vector<DeviceInfo> out;
    out.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(coll->Item(i, dev.put()))) continue;
        DeviceInfo info = describe_device(dev.get(), false);
        info.is_default = (!default_id.empty() && info.id == default_id);
        out.push_back(std::move(info));
    }
    return out;
}

// ─── AudioCapture ───────────────────────────────────────────────────────────────────────────

struct AudioCapture::Impl {
    CaptureConfig cfg;
    IAudioSink* sink = nullptr;

    std::thread thread;
    EventHandle stop_event;    // manual reset: once we are stopping we stay stopping
    EventHandle audio_event;   // auto reset: WASAPI's SetEventHandle target
    EventHandle device_event;  // auto reset: signalled by DeviceNotifier
    EventHandle ready_event;   // manual reset: start() waits on this for init_status
    Status init_status;
    std::atomic<bool> running{false};

    mutable std::mutex device_mu;
    DeviceInfo device;

    // Stats. Atomic because stats() is called from the UI thread while the capture thread
    // updates them; relaxed ordering because these are counters, not a protocol.
    std::atomic<uint64_t> frames_captured{0};
    std::atomic<uint64_t> discontinuities{0};
    std::atomic<uint64_t> glitch_frames{0};
    std::atomic<uint64_t> sink_callbacks{0};
    std::atomic<uint64_t> max_sink_us{0};

    // Capture-thread-only state.
    AudioConverter converter;
    std::vector<int16_t> pending;      // converted samples not yet emitted as a whole chunk
    std::vector<uint8_t> silence_buf;  // zeros for AUDCLNT_BUFFERFLAGS_SILENT packets
    size_t chunk_samples = 320;
    uint64_t expected_device_pos = 0;
    uint64_t emitted_samples = 0;
    int64_t pending_qpc = 0;
    bool have_pending_qpc = false;

    void thread_main();
    HRESULT drain(IAudioCaptureClient* capture);
    void handle_packet(const BYTE* data, UINT32 frames, DWORD flags, UINT64 device_pos,
                       UINT64 qpc_hns);
    void emit_ready_chunks(bool flush_partial);
    void call_sink_audio(const AudioChunk& chunk);
};

void AudioCapture::Impl::call_sink_audio(const AudioChunk& chunk) {
    const int64_t t0 = qpc_now();
    sink->on_audio(chunk);
    const int64_t dt = qpc_now() - t0;
    const uint64_t us =
        (dt > 0) ? static_cast<uint64_t>(dt * 1000000LL / qpc_frequency()) : 0ull;
    sink_callbacks.fetch_add(1, std::memory_order_relaxed);
    // A slow sink is a glitch waiting to happen (audio_capture.h threading note), so the worst
    // case is kept rather than an average — an average hides the one 40 ms stall that dropped a
    // packet.
    uint64_t prev = max_sink_us.load(std::memory_order_relaxed);
    while (us > prev) {
        if (max_sink_us.compare_exchange_weak(prev, us, std::memory_order_relaxed)) break;
    }
}

void AudioCapture::Impl::emit_ready_chunks(bool flush_partial) {
    // The tail flushed out of the resampler at end of session can arrive with no packet behind
    // it (the discontinuity path clears the anchor). Anchoring on now() is the honest answer
    // then: it is a real acquisition time for audio that is at most a filter length old.
    if (!have_pending_qpc && !pending.empty()) {
        pending_qpc = qpc_now();
        have_pending_qpc = true;
    }
    const int64_t freq = qpc_frequency();
    size_t offset = 0;
    while (offset < pending.size()) {
        const size_t remaining = pending.size() - offset;
        size_t n = chunk_samples;
        if (remaining < chunk_samples) {
            if (!flush_partial) break;
            n = remaining;
        }
        AudioChunk chunk;
        chunk.samples = pending.data() + offset;
        chunk.count = n;
        chunk.qpc_first_sample = static_cast<uint64_t>(pending_qpc);
        chunk.session_pos_ms = emitted_samples / (kTargetSampleRate / 1000);
        call_sink_audio(chunk);

        offset += n;
        emitted_samples += n;
        pending_qpc += static_cast<int64_t>(n) * freq / static_cast<int64_t>(kTargetSampleRate);
    }
    if (offset > 0) {
        pending.erase(pending.begin(), pending.begin() + static_cast<ptrdiff_t>(offset));
        if (pending.empty()) have_pending_qpc = false;
    }
}

void AudioCapture::Impl::handle_packet(const BYTE* data, UINT32 frames, DWORD flags,
                                       UINT64 device_pos, UINT64 qpc_hns) {
    const AudioFormat& fmt = converter.input_format();

    if ((flags & static_cast<DWORD>(AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)) != 0) {
        // Report the gap; never conceal it. Padding with zeroes or splicing across it both hand
        // the decoder a lie (docs/protocol.md §6.1). Everything already converted is emitted
        // first, so pre-gap and post-gap audio never share one chunk, and the filter state is
        // dropped so pre-gap audio is not smeared into post-gap audio by the delay line.
        emit_ready_chunks(/*flush_partial=*/true);
        converter.flush(pending);
        emit_ready_chunks(/*flush_partial=*/true);
        converter.reset();
        have_pending_qpc = false;

        const uint32_t gap_ms = estimate_gap_ms(expected_device_pos, device_pos, fmt.sample_rate);
        discontinuities.fetch_add(1, std::memory_order_relaxed);
        if (device_pos > expected_device_pos && expected_device_pos != 0) {
            glitch_frames.fetch_add(device_pos - expected_device_pos, std::memory_order_relaxed);
        }
        KD_LOG_WARN("capture discontinuity: %u ms lost (device pos %llu, expected %llu)", gap_ms,
                    static_cast<unsigned long long>(device_pos),
                    static_cast<unsigned long long>(expected_device_pos));
        sink->on_discontinuity(gap_ms, "AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY");
    }

    expected_device_pos = device_pos + frames;
    frames_captured.fetch_add(frames, std::memory_order_relaxed);
    if (frames == 0) return;

    // AUDCLNT_BUFFERFLAGS_SILENT means the buffer contents are undefined and must be treated as
    // silence. Reading it would feed the converter whatever the engine left in the ring.
    const void* src = data;
    if ((flags & static_cast<DWORD>(AUDCLNT_BUFFERFLAGS_SILENT)) != 0 || data == nullptr) {
        const size_t bytes = static_cast<size_t>(frames) * fmt.bytes_per_frame();
        if (silence_buf.size() < bytes) silence_buf.assign(bytes, uint8_t{0});
        src = silence_buf.data();
    }

    if (cfg.tap_native_format) {
        // plan.md §3 Phase 1: the original-format capture is preserved alongside the 16 kHz
        // conversion so a backend error and a resampling bug stay distinguishable. This is the
        // tap; the recorder on the other end writes it with WavWriter.
        sink->on_audio_native(src, frames, fmt);
    }

    if (!have_pending_qpc) {
        int64_t t = 0;
        if ((flags & static_cast<DWORD>(AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)) != 0 || qpc_hns == 0) {
            // Documented as "the device position is unreliable for this packet". Falling back to
            // now() costs the scheduling delay between the glitch and this wake-up, which is the
            // best available answer and is worse than the device timestamp — hence the log line,
            // so a latency measurement built on these packets is not silently trusted.
            t = qpc_now();
            KD_LOG_DEBUG("packet timestamp error; falling back to QueryPerformanceCounter");
        } else {
            t = qpc_from_100ns(qpc_hns);
        }
        // The first converted sample this packet contributes to describes audio captured one
        // filter group delay ago (~2.85 ms). Subtracting it here is what keeps the constant out
        // of Phase 2's latency decomposition.
        const double delay_ticks =
            converter.group_delay_ms() * static_cast<double>(qpc_frequency()) / 1000.0;
        pending_qpc = t - static_cast<int64_t>(delay_ticks);
        have_pending_qpc = true;
    }

    converter.process(src, frames, pending);
    emit_ready_chunks(/*flush_partial=*/false);
}

HRESULT AudioCapture::Impl::drain(IAudioCaptureClient* capture) {
    for (;;) {
        UINT32 packet = 0;
        HRESULT hr = capture->GetNextPacketSize(&packet);
        if (FAILED(hr)) return hr;
        if (packet == 0) return S_OK;

        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 device_pos = 0;
        UINT64 qpc_hns = 0;
        hr = capture->GetBuffer(&data, &frames, &flags, &device_pos, &qpc_hns);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) return S_OK;
        if (FAILED(hr)) return hr;

        PacketGuard release(capture, frames);
        handle_packet(data, frames, flags, device_pos, qpc_hns);
    }
}

void AudioCapture::Impl::thread_main() {
    // Every failure path below signals ready_event before returning: start() blocks on it, so a
    // path that forgets would hang the caller rather than report an error.
    bool device_lost = false;
    bool device_changed = false;

    {
        ComApartment com;
        if (!com.usable()) {
            init_status = hresult_status("CoInitializeEx", com.hr());
            ready_event.set();
            return;
        }

        // MMCSS. Best effort: on a machine where the Audiosrv scheduler category is missing this
        // fails and capture still works, just without priority. Never fatal, always logged —
        // without it a busy machine glitches in a way indistinguishable from a bad model.
        MmcssTask mmcss;
        if (!mmcss.ok()) {
            KD_LOG_WARN("AvSetMmThreadCharacteristicsW(Audio) failed (err %lu); capture thread "
                        "runs at normal priority",
                        GetLastError());
        }

        ComPtr<IMMDeviceEnumerator> enumr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), enumr.put_void());
        if (FAILED(hr)) {
            init_status = hresult_status("CoCreateInstance(MMDeviceEnumerator)", hr);
            ready_event.set();
            return;
        }

        const std::wstring pinned = utf8::to_utf16_or_empty(cfg.device_id);
        ComPtr<IMMDevice> dev;
        if (cfg.device_id.empty()) {
            hr = enumr->GetDefaultAudioEndpoint(eCapture, eConsole, dev.put());
            if (FAILED(hr)) {
                init_status = hresult_status("GetDefaultAudioEndpoint(eCapture)", hr);
                ready_event.set();
                return;
            }
        } else {
            hr = enumr->GetDevice(pinned.c_str(), dev.put());
            if (FAILED(hr)) {
                init_status = hresult_status("IMMDeviceEnumerator::GetDevice", hr);
                ready_event.set();
                return;
            }
        }

        DeviceInfo info = describe_device(dev.get(), cfg.device_id.empty());

        ComPtr<IAudioClient> client;
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void());
        if (FAILED(hr)) {
            init_status = hresult_status("IMMDevice::Activate(IAudioClient)", hr);
            ready_event.set();
            return;
        }

        WAVEFORMATEX* raw_mix = nullptr;
        hr = client->GetMixFormat(&raw_mix);
        if (FAILED(hr)) {
            init_status = hresult_status("IAudioClient::GetMixFormat", hr);
            ready_event.set();
            return;
        }
        CoMemWfx mix(raw_mix);

        const AudioFormat fmt = format_from_wfx(mix.get());
        Status cs = converter.configure(fmt);
        if (!cs.is_ok()) {
            init_status = cs;
            ready_event.set();
            return;
        }
        info.mix_format = fmt;

        // Shared mode, event driven. hnsPeriodicity MUST be 0 here — a non-zero periodicity with
        // AUDCLNT_STREAMFLAGS_EVENTCALLBACK is an exclusive-mode construction and returns
        // AUDCLNT_E_INVALID_DEVICE_PERIOD in shared mode.
        REFERENCE_TIME default_period = 0;
        REFERENCE_TIME min_period = 0;
        client->GetDevicePeriod(&default_period, &min_period);
        // Ask for slack: the engine may allocate more but never less, and a buffer only one
        // period deep turns any scheduling hiccup into a discontinuity.
        REFERENCE_TIME buffer_duration = std::max<REFERENCE_TIME>(default_period * 4, 600000);
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                buffer_duration, 0, mix.get(), nullptr);
        if (FAILED(hr)) {
            init_status = hresult_status("IAudioClient::Initialize", hr);
            ready_event.set();
            return;
        }

        hr = client->SetEventHandle(audio_event.get());
        if (FAILED(hr)) {
            init_status = hresult_status("IAudioClient::SetEventHandle", hr);
            ready_event.set();
            return;
        }

        ComPtr<IAudioCaptureClient> capture;
        hr = client->GetService(__uuidof(IAudioCaptureClient), capture.put_void());
        if (FAILED(hr)) {
            init_status = hresult_status("IAudioClient::GetService(IAudioCaptureClient)", hr);
            ready_event.set();
            return;
        }

        auto* notifier = new DeviceNotifier(device_event.get(), pinned);
        notifier->set_active_id(utf8::to_utf16_or_empty(info.id));
        NotifierRegistration registration(enumr.get(), notifier);
        if (!registration.ok()) {
            // Not fatal: capture works, we just will not learn about a default-device change
            // until the endpoint stops feeding us and the watchdog fires.
            KD_LOG_WARN("RegisterEndpointNotificationCallback failed; device changes will only "
                        "be detected by the capture watchdog");
        }

        chunk_samples = static_cast<size_t>(std::max<uint32_t>(cfg.chunk_ms, 1u)) *
                        (kTargetSampleRate / 1000);
        pending.clear();
        pending.reserve(chunk_samples * 4);
        expected_device_pos = 0;
        emitted_samples = 0;
        have_pending_qpc = false;

        hr = client->Start();
        if (FAILED(hr)) {
            init_status = hresult_status("IAudioClient::Start", hr);
            ready_event.set();
            return;
        }
        ClientRunGuard run(client.get());

        {
            std::lock_guard<std::mutex> lock(device_mu);
            device = info;
        }
        KD_LOG_INFO("capture started on \"%s\" (%s)", info.friendly_name.c_str(),
                    fmt.to_string().c_str());

        init_status = Status::success();
        ready_event.set();

        // ─── The loop ───────────────────────────────────────────────────────────────────────
        //
        // The 2 s timeout is a watchdog, not a poll: the event fires every device period (10 ms
        // or so) while the endpoint is alive, so two seconds of silence from it means the device
        // is gone in a way that produced no notification. plan.md §7 requires mic removal to end
        // the session cleanly rather than hang, and an INFINITE wait here is exactly that hang.
        const HANDLE waits[3] = {stop_event.get(), device_event.get(), audio_event.get()};
        for (;;) {
            const DWORD w = WaitForMultipleObjects(3, waits, FALSE, 2000);
            if (w == WAIT_OBJECT_0) break;  // stop()

            if (w == WAIT_OBJECT_0 + 1) {
                device_changed = true;
                device_lost = notifier->lost();
                break;
            }

            if (w == WAIT_TIMEOUT) {
                KD_LOG_ERROR("capture watchdog: no buffer event for 2000 ms; treating the "
                             "endpoint as lost");
                device_changed = true;
                device_lost = true;
                break;
            }

            if (w != WAIT_OBJECT_0 + 2) {
                KD_LOG_ERROR("WaitForMultipleObjects failed (%lu, err %lu)", w, GetLastError());
                device_changed = true;
                device_lost = true;
                break;
            }

            const HRESULT dhr = drain(capture.get());
            if (FAILED(dhr)) {
                device_changed = true;
                device_lost = (dhr == AUDCLNT_E_DEVICE_INVALIDATED);
                KD_LOG_ERROR("capture read failed (hr=0x%08lX)", static_cast<unsigned long>(dhr));
                break;
            }
        }

        // Tail of the session: whatever the converter still holds is real audio the user spoke.
        // Skipped when the device is gone, because then the delay line's contents are the last
        // milliseconds before a fault, not speech worth appending.
        if (!device_lost) {
            converter.flush(pending);
        }
        emit_ready_chunks(/*flush_partial=*/true);
    }  // ClientRunGuard::Stop, notifier unregister, COM uninit all happen here, in order

    if (device_changed) {
        // Reported after the capture has fully stopped, exactly as audio_capture.h promises, so
        // the owner can restart from inside the callback without re-entering a live device.
        DeviceInfo next;
        if (!device_lost) {
            ComApartment com2;
            ComPtr<IMMDeviceEnumerator> e2;
            if (com2.usable() &&
                SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                           __uuidof(IMMDeviceEnumerator), e2.put_void()))) {
                ComPtr<IMMDevice> d2;
                if (SUCCEEDED(e2->GetDefaultAudioEndpoint(eCapture, eConsole, d2.put()))) {
                    next = describe_device(d2.get(), true);
                }
            }
        }
        sink->on_device_change(next, device_lost);
    }

    running.store(false, std::memory_order_release);
    KD_LOG_INFO("capture thread exited (changed=%d lost=%d)", device_changed ? 1 : 0,
                device_lost ? 1 : 0);
}

AudioCapture::AudioCapture() : impl_(new Impl()) {}

AudioCapture::~AudioCapture() {
    stop();
    delete impl_;
}

Status AudioCapture::start(const CaptureConfig& cfg, IAudioSink* sink) {
    if (sink == nullptr) return Status{StatusCode::InvalidArgument, "AudioCapture: null sink"};
    if (cfg.chunk_ms == 0 || cfg.chunk_ms > 1000) {
        return Status{StatusCode::InvalidArgument,
                      "AudioCapture: chunk_ms must be 1..1000 (docs/protocol.md §6.3 recommends "
                      "20-100)"};
    }
    stop();  // idempotent; also joins a thread that exited on its own after a device loss

    impl_->cfg = cfg;
    impl_->sink = sink;
    impl_->init_status = Status::success();

    if (!impl_->stop_event.create(true) || !impl_->audio_event.create(false) ||
        !impl_->device_event.create(false) || !impl_->ready_event.create(true)) {
        return Status{StatusCode::OsError, "AudioCapture: CreateEventW failed",
                      static_cast<uint32_t>(GetLastError())};
    }

    impl_->running.store(true, std::memory_order_release);
    Impl* impl = impl_;
    try {
        impl_->thread = std::thread([impl] { impl->thread_main(); });
    } catch (const std::system_error& e) {
        impl_->running.store(false, std::memory_order_release);
        return Status{StatusCode::Internal,
                      std::string("AudioCapture: could not start capture thread: ") + e.what()};
    }

    // Wait for the thread to finish opening the device so start() can return a real error
    // instead of a promise.
    //
    // **Known limitation, stated rather than papered over:** this wait is INFINITE. Every call
    // before the signal is a bounded WASAPI/driver call, but a wedged audio driver that never
    // returns from IAudioClient::Initialize would hang the caller here, and there is no safe
    // way to abandon the thread — it holds a pointer to this Impl. A bounded wait followed by a
    // join would hang in exactly the same place, one line lower, while pretending otherwise.
    WaitForSingleObject(impl_->ready_event.get(), INFINITE);

    if (!impl_->init_status.is_ok()) {
        Status s = impl_->init_status;
        stop();
        return s;
    }
    return Status::success();
}

void AudioCapture::stop() {
    impl_->stop_event.set();
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->running.store(false, std::memory_order_release);

    impl_->stop_event.reset();
    impl_->audio_event.reset();
    impl_->device_event.reset();
    impl_->ready_event.reset();
    impl_->pending.clear();
    impl_->sink = nullptr;
}

bool AudioCapture::is_running() const noexcept {
    return impl_->running.load(std::memory_order_acquire);
}

DeviceInfo AudioCapture::current_device() const {
    std::lock_guard<std::mutex> lock(impl_->device_mu);
    return impl_->device;
}

AudioCapture::Stats AudioCapture::stats() const noexcept {
    Stats s;
    s.frames_captured = impl_->frames_captured.load(std::memory_order_relaxed);
    s.discontinuities = impl_->discontinuities.load(std::memory_order_relaxed);
    s.glitch_frames_estimated = impl_->glitch_frames.load(std::memory_order_relaxed);
    s.sink_callbacks = impl_->sink_callbacks.load(std::memory_order_relaxed);
    s.max_sink_callback_us = impl_->max_sink_us.load(std::memory_order_relaxed);
    return s;
}

}  // namespace kd::audio
