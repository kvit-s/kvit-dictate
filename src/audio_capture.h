// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// WASAPI microphone capture -> 16 kHz mono s16le, with the format conversion split out as a
// pure, testable unit.
//
// Implements: plan.md §1.1 (capture), §3 Phase 1 item 1 (native device-format discovery,
// channel mixing, resampling, buffer discontinuity handling, timestamps, default-device-change
// handling, device selection), and docs/protocol.md §6.1 `discontinuity` / §6.3 audio frames.
//
// ─── Why this module is built first ─────────────────────────────────────────────────────────
//
// plan.md §3 Phase 1 opens with it: revision 1 planned to record the benchmark corpus with
// `bench/record.py` while scheduling the real WASAPI path for Phase 3, which would have meant
// tuning models against a microphone pipeline the product does not use. So the real capture
// module is built first and the corpus recorder drives it. Any change here changes the corpus's
// meaning — treat the conversion chain as versioned data, not as an implementation detail.
//
// ─── The split that makes it testable ───────────────────────────────────────────────────────
//
//   * `AudioConverter` — pure. Downmix, sample-format conversion, resample. No COM, no
//     Windows headers, no device. Fed by unit tests with synthetic buffers at 44.1/48/96 kHz,
//     mono/stereo/7.1, s16/s24/s32/f32. This is where the bugs live and it is where the tests
//     go (plan.md §7 test breadth: "audio conversion and discontinuity").
//   * `AudioCapture` — the WASAPI half. Device enumeration, IAudioClient, the event-driven
//     capture loop, MMCSS, device-change notifications. Not unit-testable and not pretended to
//     be; it is exercised by the probe tool and by manual device tests.
//
// ─── Threading — read this before touching anything ─────────────────────────────────────────
//
// `AudioCapture` owns **one dedicated capture thread**. That thread:
//   * calls CoInitializeEx(COINIT_MULTITHREADED) itself — the object may be constructed on the
//     UI thread, which is STA, and an IAudioCaptureClient must not be used across apartments;
//   * raises its priority through AvSetMmThreadCharacteristicsW(L"Audio", …) (this is why
//     avrt.lib is linked). Without MMCSS, a busy machine produces glitches that look exactly
//     like a bad model;
//   * waits on the IAudioClient event handle and drains every available packet per wake.
//
// **`IAudioSink` callbacks run on that capture thread.** They must not block, must not allocate
// where it can be avoided, must not take a lock the UI thread holds, and must not call back into
// AudioCapture. The intended implementation copies into a bounded ring buffer and returns; the
// IPC writer picks it up. A sink that blocks here produces a glitch, which becomes a
// `discontinuity` message, which the Server may respond to by resetting decoder state — i.e. a
// slow sink degrades transcription quality silently unless it is caught here.
//
// ─── Discontinuity is reported, never hidden ────────────────────────────────────────────────
//
// WASAPI sets AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY when the capture stream has a gap. The two
// silent options — pad with zeroes, or splice the audio together — both hand the decoder a lie
// and produce mysteriously bad transcription that is indistinguishable from a bad model. So a
// gap becomes an `on_discontinuity` callback, which becomes a `discontinuity` control message
// (docs/protocol.md §6.1), and the Server may reset decoder state or force an utterance
// boundary. Padding is not an option this module offers.

#include <cstdint>
#include <string>
#include <vector>

#include "util/result.h"

namespace kd::audio {

// ─── Pure half: format conversion ───────────────────────────────────────────────────────────

enum class SampleFormat : uint8_t {
    S16,    // WAVE_FORMAT_PCM, 16-bit
    S24In32,// 24-bit valid bits inside a 32-bit container — common on USB interfaces
    S32,    // WAVE_FORMAT_PCM, 32-bit
    F32,    // WAVE_FORMAT_IEEE_FLOAT — the usual shared-mode mix format
    Unsupported,
};

const char* to_string(SampleFormat f) noexcept;

struct AudioFormat {
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    SampleFormat format = SampleFormat::Unsupported;

    [[nodiscard]] uint32_t bytes_per_frame() const noexcept;
    [[nodiscard]] bool is_valid() const noexcept;
    [[nodiscard]] std::string to_string() const;
};

// The target: docs/protocol.md §6.3, non-negotiable in proto 1.
inline constexpr uint32_t kTargetSampleRate = 16000;
inline constexpr uint16_t kTargetChannels = 1;

// Converts device-native buffers to 16 kHz mono s16le.
//
// **Resampling quality is a correctness matter here, not a nicety.** Linear interpolation from
// 48 kHz to 16 kHz aliases everything above 8 kHz back into the speech band, which an ASR model
// hears as noise it was never trained on — and the resulting accuracy loss would be attributed
// to the model in the bake-off. The implementation must low-pass before decimating.
//
// The common case is exact and cheap: 48000 -> 16000 is an integer 3:1 ratio, so an FIR
// low-pass with its cutoff below 8 kHz followed by decimation by 3 is both correct and fast.
// 44100 -> 16000 is 147:160 and needs a polyphase rational resampler. 16000 -> 16000 is a
// pass-through and must be recognised as one, so that a 16 kHz-native device is not degraded by
// a round trip through the filter.
//
// ─── The filter, decided and recorded (was an open question in the skeleton) ────────────────
//
// One design covers every rate, because a special case per rate is a special case that goes
// untested. The resampler is a **rational polyphase FIR**: L = 16000/gcd, M = fs_in/gcd, an
// interpolate-by-L / decimate-by-M structure whose prototype low-pass runs at the intermediate
// rate fs_i = fs_in x L. Only the L-th of the taps that multiply a non-zero sample are ever
// evaluated, so the cost per output sample is (total taps / L), not (total taps).
//
//   Prototype   windowed sinc, Kaiser window, beta = 0.1102 x (A - 8.7) with A = 80 dB.
//   Cutoff      fc = 0.475 x min(fs_in, 16000)  -> 7600 Hz whenever we are downsampling.
//               fc is the -6 dB point, not the passband edge.
//   Transition  df = 0.055 x min(fs_in, 16000)  -> 880 Hz, i.e. passband edge 7160 Hz and
//               stopband edge 8040 Hz.
//   Length      N = ceil((A - 8) / (2.285 x 2pi x df / fs_i)), rounded up to a multiple of L
//               and capped at 262144 taps.
//
// Measured response of exactly this design (48 kHz -> 16 kHz, N = 274; 44.1 kHz -> 16 kHz,
// N = 40320 — the two agree to within 0.001 dB because the design is specified in Hz, not in
// normalised units):
//
//     1 kHz  -0.00 dB      7.16 kHz  -0.00 dB      8.04 kHz   -80 dB
//     7 kHz  -0.00 dB      7.60 kHz  -6.02 dB      12   kHz   -98 dB
//
// The 12 kHz figure is the one that matters: a naive 3:1 decimator would fold a 12 kHz tone
// back to 4 kHz at full amplitude, squarely inside the speech band, and the resulting accuracy
// loss would be charged to whichever model happened to be under test.
//
// Group delay is (N - 1) / 2 intermediate samples ~= 2.85 ms for every supported rate. It is a
// constant, it is reported by group_delay_ms(), and AudioCapture subtracts it from the QPC
// timestamp it hands the sink, so plan.md §3 Phase 2's latency decomposition is not measuring
// this filter by accident.
//
// **This is now versioned data, not an implementation detail.** Changing any constant above
// changes what the Phase 1 corpus measures. Re-record the corpus or do not change them.
class AudioConverter {
public:
    AudioConverter();
    ~AudioConverter();
    AudioConverter(const AudioConverter&) = delete;
    AudioConverter& operator=(const AudioConverter&) = delete;

    // Configure for a device format. Fails with StatusCode::Unsupported for formats this
    // converter does not handle, rather than guessing — a wrong guess about bit depth produces
    // white noise, which is at least obvious, or a quiet 6 dB error, which is not.
    Status configure(const AudioFormat& in);

    [[nodiscard]] const AudioFormat& input_format() const noexcept;

    // Convert `frame_count` device frames. Appends to `out`; never clears it, so a caller can
    // accumulate a protocol-sized chunk across several WASAPI packets.
    //
    // Channel mixing is an average of all channels, not a copy of channel 0: a headset that
    // presents a stereo capture endpoint with silence on one channel would otherwise be
    // 6 dB quiet or entirely silent depending on which channel it filled.
    void process(const void* frames, size_t frame_count, std::vector<int16_t>& out);

    // Emit whatever the filter's delay line still holds. Call at the end of a session so the
    // last few milliseconds of speech are not stuck inside the resampler.
    void flush(std::vector<int16_t>& out);

    // Discard filter state. Call on a discontinuity: continuing to filter across a gap smears
    // pre-gap audio into post-gap audio, which is the same lie as padding, just quieter.
    void reset();

    // Samples produced per input frame, for buffer sizing. Approximate for rational ratios.
    [[nodiscard]] double ratio() const noexcept;

    // The FIR's group delay, in milliseconds of the 16 kHz output timeline. Zero when the
    // converter is in pass-through. Exposed so a timestamp can be corrected by it rather than
    // silently carrying it: an output sample emitted "now" describes audio captured this long
    // ago, and a latency decomposition that did not subtract it would attribute a constant
    // 2.85 ms of this module to the ASR backend.
    [[nodiscard]] double group_delay_ms() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

// Estimate how much audio a capture glitch swallowed, from two WASAPI device positions.
//
// `expected_pos` is the device position immediately after the previous packet (that packet's
// reported position plus its frame count); `reported_pos` is the position GetBuffer reported
// for the packet flagged AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY. Both count device frames at
// `sample_rate`, so this is arithmetic, not a Windows call — which is why it is out here where
// it can be tested.
//
// Returns 0 when no gap can be established: no previous packet (expected_pos == 0), a position
// that did not advance, a position that went backwards (a driver bug we report rather than
// paper over with a negative number), or sample_rate == 0. docs/protocol.md §6.1 defines 0 as
// the legal "a discontinuity occurred, duration unknown" value, so 0 is an answer here and not
// an error. Result is rounded to nearest and saturates at UINT32_MAX.
uint32_t estimate_gap_ms(uint64_t expected_pos, uint64_t reported_pos,
                         uint32_t sample_rate) noexcept;

// ─── Windows half: capture ──────────────────────────────────────────────────────────────────

struct DeviceInfo {
    std::string id;            // the WASAPI endpoint id, stable across reboots — what settings store
    std::string friendly_name; // for the UI only; changes when the user renames a device
    bool is_default = false;
    AudioFormat mix_format;    // the device's shared-mode mix format, as discovered
};

struct CaptureConfig {
    // Empty means "the default capture device", and means it dynamically: the capture follows
    // default-device changes. A non-empty id pins one device and reports an error if it goes
    // away, which is what a benchmark run wants and what a daily driver does not.
    std::string device_id;

    // Milliseconds of audio per on_audio() callback, after conversion. docs/protocol.md §6.3
    // recommends 20–100 ms per audio frame; this is the knob that sets it.
    uint32_t chunk_ms = 20;

    // plan.md §3 Phase 1: "Preserve original-format captures alongside the 16 kHz conversion, so
    // backend errors stay distinguishable from resampling bugs." When true, on_audio_native() is
    // called with the untouched device buffer as well. Off in production — it doubles the
    // callback cost for a diagnostic nobody is reading.
    bool tap_native_format = false;
};

// One converted chunk.
struct AudioChunk {
    const int16_t* samples = nullptr;  // 16 kHz mono; valid only for the duration of the call
    size_t count = 0;

    // QueryPerformanceCounter value corresponding to the *first* sample, derived from the
    // WASAPI device position. Deliberately raw QPC rather than kd::Millis: this timestamp is
    // correlated with device positions and with injection timings for the latency decomposition
    // in plan.md §3 Phase 2 item 4, and a millisecond clock would round away the thing being
    // measured.
    uint64_t qpc_first_sample = 0;

    // Position in the session's audio timeline, in milliseconds — the same timeline
    // `audio_pos_ms` refers to on the wire.
    uint64_t session_pos_ms = 0;
};

class IAudioSink {
public:
    virtual ~IAudioSink() = default;

    // Called on the capture thread. See the threading note at the top: copy and return.
    virtual void on_audio(const AudioChunk& chunk) = 0;

    // A gap in the capture stream. `gap_ms` is the best estimate available (0 means "a gap
    // occurred, duration unknown", which docs/protocol.md §6.1 defines as legal). `reason` is a
    // short static string, e.g. "AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY".
    virtual void on_discontinuity(uint32_t gap_ms, const char* reason) = 0;

    // The default capture device changed, or the pinned device was removed. The capture has
    // already stopped by the time this is called; restarting is the owner's decision, because
    // it is a UX question (plan.md §7 requires clean recovery from mic removal).
    virtual void on_device_change(const DeviceInfo& new_device, bool device_lost) = 0;

    // Only called when CaptureConfig::tap_native_format is set. Same thread, same rules.
    virtual void on_audio_native(const void* frames, size_t frame_count, const AudioFormat& fmt) {
        (void)frames;
        (void)frame_count;
        (void)fmt;
    }
};

// Enumerate capture endpoints. Callable from any thread; initialises and uninitialises COM for
// the duration of the call if the calling thread has not already done so.
Result<std::vector<DeviceInfo>> enumerate_capture_devices();

class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();
    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    // Starts the capture thread. `sink` must outlive the capture, and must be prepared for
    // callbacks to begin before start() returns.
    //
    // The mic is open only between start() and stop() — plan.md §1.9 makes that a privacy
    // promise, and the tray state is driven from it, so nothing else may hold the device open.
    Status start(const CaptureConfig& cfg, IAudioSink* sink);

    // Blocks until the capture thread has exited and no further callback can occur. Safe to
    // call when not started.
    void stop();

    [[nodiscard]] bool is_running() const noexcept;

    // The device actually in use, valid while running.
    [[nodiscard]] DeviceInfo current_device() const;

    // Diagnostics for the overload gate in plan.md §7 and for the bench harness.
    struct Stats {
        uint64_t frames_captured = 0;
        uint64_t discontinuities = 0;
        uint64_t glitch_frames_estimated = 0;
        uint64_t sink_callbacks = 0;
        uint64_t max_sink_callback_us = 0;  // a slow sink is a glitch waiting to happen
    };
    [[nodiscard]] Stats stats() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

// ─── WAV output, for the bench recorder ─────────────────────────────────────────────────────
//
// plan.md §3 Phase 1 wants two files per take: the 16 kHz mono s16 stream the product actually
// sends, and the untouched device-format stream beside it. That is what makes "the backend got
// this wrong" separable from "the resampler got this wrong" — with only the converted file, a
// bad take and a bad filter look identical.
//
// Deliberately minimal: RIFF/WAVE, one `fmt ` chunk, one `data` chunk, sizes patched on close.
// It writes WAVE_FORMAT_EXTENSIBLE when it has to (more than two channels, or 24 valid bits in
// a 32-bit container, where the plain 16-byte `fmt ` chunk has nowhere to put
// wValidBitsPerSample) and a plain chunk otherwise, because a plain chunk is what every tool
// reads without complaint.
//
// **RIFF sizes are 32-bit.** A take cannot exceed 4 GiB, which at 48 kHz / 2 ch / 32-bit is
// about 5.6 hours. write() fails with StatusCode::Overloaded at the boundary rather than
// wrapping the size field and producing a file that plays back as noise.
//
// Not thread-safe: one writer, one thread. The bench recorder owns it on the thread that drains
// the capture queue, never on the WASAPI callback thread — file I/O in an IAudioSink callback
// is exactly the blocking that audio_capture.h's threading note forbids.
class WavWriter {
public:
    WavWriter();
    ~WavWriter();
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    // `utf8_path` is UTF-8, converted to UTF-16 here — this is a Windows API boundary and
    // util/utf8.h owns the crossing.
    Status open(const std::string& utf8_path, const AudioFormat& fmt);

    // `frames` is interleaved device-format data, `frame_count` frames of fmt.bytes_per_frame().
    Status write(const void* frames, size_t frame_count);

    // Patches the RIFF and data sizes and closes. Called by the destructor too, but the
    // destructor cannot report a failed flush, so call it explicitly where the result matters.
    Status close();

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] uint64_t frames_written() const noexcept;
    [[nodiscard]] const AudioFormat& format() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace kd::audio
