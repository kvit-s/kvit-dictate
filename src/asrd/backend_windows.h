// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// `--backend windows-speech`: the on-device Windows AI speech model, through `kvit-speech.exe`.
//
// This is the backend that needs no server. The model is Whisper Small, int8, shipped by
// Microsoft as an optional Windows component and reached through `Microsoft.Windows.AI.Speech`.
// Nothing leaves the machine. `tools/kvit-speech/README.md` is the measurement record; the facts
// that shape this file are:
//
//   * **The WinRT lives in a helper process**, `tools/kvit-speech` (C#), because the API is a
//     Windows App SDK projection and this program has no third-party dependencies. The helper
//     speaks a framed protocol of its own on stdin/stdout — `[u32 len][u8 tag][payload]`, tags
//     `T`/`P`/`Q` in and `R`/`O`/`E` out — whose normative copy is the header of `Program.cs`.
//     It is deliberately not docs/protocol.md: it carries one question, "what do these bytes
//     say?", and sessions, sequence numbers and stability stay on this side.
//
//   * **The helper is expensive to start and cheap to keep.** `TryCreateAsync` measured 2851 ms
//     cold and 919 ms warm, and a whole `ready` took up to 4155 ms. So the helper belongs to the
//     *process*, not to a connection: the first connection starts it, every later one reuses it,
//     and it dies with this process (a job object with kill-on-close, and stdin EOF as a
//     backstop — `ServeAsync` exits on it).
//
//   * **Batch, finalize-only by default.** A decode is ≈ 1350 ms fixed + 0.30× real time on the
//     i7-14700K it was measured on, CPU only. A one-second preview hop would need a 1.7 s decode
//     every second and fall further behind with each, so the default is one decode per
//     utterance. Live preview works and lags, and that is the model, not a setting.
//
//   * **It invents text over silence**: two seconds of digital zero came back as a fluent Welsh
//     sentence. The endpointer upstream normally stops such a window being built; the peak gate
//     here is the second line, and it also saves the 1.4 s a silent decode costs. It answers an
//     empty transcript, which is the correct answer for a window with nothing in it.
//
//   * **No vocabulary.** The API takes no prompt, so `ready.context_supported` is false and the
//     terms a session sends are kept and not used. Saying so is what lets the client grey out a
//     box that would otherwise change nothing.
//
//   * **No timestamps**, so no segment cuts. An utterance is bounded by the endpointer's
//     `max_utterance_ms` alone, which `connection.cpp` already requires.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "asrd/backend.h"
#include "asrd/backend_openai.h"  // PseudoStreamingDriver: the same buffer, the same invariant
#include "util/result.h"

namespace kd::asrd {

// What `ready.model` and `hello_ack.models[0].id` say for this backend.
inline constexpr const char* kWindowsSpeechModel = "windows-speech";

// The helper's frame cap, the same number `Program.cs` enforces on its side.
inline constexpr uint32_t kSpeechMaxFrameBytes = 32u * 1024u * 1024u;

// Where the helper is installed: `kvit-speech\kvit-speech.exe` beside this executable.
//
// Fixed for the same reason `Translator::image_path` is: the client launches this translator by
// absolute path from its own directory, and the translator launches the helper the same way.
// `--speech-helper` exists for a developer build and for the tests; the client never passes it.
std::filesystem::path default_speech_helper_path();

// The loudest sample, as a magnitude. Free and pure so the gate's arithmetic is testable without
// a helper — `-32768` is the case that matters.
uint32_t pcm_peak(const int16_t* samples, size_t count) noexcept;

// One running `kvit-speech.exe`. Thread-safe: `ensure_ready` and `transcribe` may be called from
// any thread, and requests are serialised, because the helper answers one at a time and in order.
class SpeechHelper {
public:
    struct Spec {
        std::filesystem::path exe;
        // Extra arguments, for tests and for `--ensure-model`. Built by this program, never from
        // user input.
        std::vector<std::wstring> args;
        // Cold start is ~3–4 s, but the first start after a reboot may be reading 300 MB of
        // model from a cold disk, and a Copilot+ NPU has its own warm-up. A minute is "broken".
        uint32_t startup_timeout_ms = 60000;
    };

    explicit SpeechHelper(Spec spec);
    ~SpeechHelper();
    SpeechHelper(const SpeechHelper&) = delete;
    SpeechHelper& operator=(const SpeechHelper&) = delete;

    // Start the helper if it is not running and wait for its `R`. Idempotent: a helper that is
    // up and ready returns at once. A helper that died since is started again, once per call.
    Status ensure_ready();

    // Transcribe `count` samples. Blocks for the decode.
    //
    // `cancel` is polled while waiting. A cancelled request returns at once with an error, and
    // its answer — which the helper still owes, since a decode cannot be interrupted — is
    // consumed and discarded before the next request is sent, so answers never drift out of
    // step with questions. Exceeding `timeout_ms` kills the helper: an answer that late is a
    // wedged process, and the next call starts a fresh one.
    Status transcribe(const int16_t* samples, size_t count, uint32_t timeout_ms,
                      const std::atomic<bool>& cancel, std::string& out);

    // Ask it to quit, then make sure it has. Idempotent.
    void stop();

    // The `R` payload of the running helper, for the log. Empty before the first ready.
    [[nodiscard]] std::string ready_info() const;
    // How many times a process has been launched. The tests use it to prove reuse.
    [[nodiscard]] uint64_t launches() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The process-wide helper for `spec`, started lazily on first use and kept until
// `release_speech_helpers()` or process exit. Keyed by executable and arguments, so tests that
// launch differently-behaved fakes do not share one.
std::shared_ptr<SpeechHelper> shared_speech_helper(const SpeechHelper::Spec& spec);
// Stop every helper `shared_speech_helper` has started. `main` calls it on the way out; the
// tests call it between cases.
void release_speech_helpers();

struct WindowsSpeechConfig {
    std::filesystem::path helper;  // empty means default_speech_helper_path()
    bool finalize_only = true;
    uint32_t hop_ms = 1000;        // a rate, as in OpenAiBackendConfig; only used with preview on
    uint32_t startup_timeout_ms = 60000;
    // A 30 s utterance — the endpointer's cap — decodes in about 10 s on a fast desktop CPU. A
    // slower laptop is plausibly three times that; past a minute the helper is wedged.
    uint32_t decode_timeout_ms = 60000;
    // Peak magnitude below which a window is not decoded at all (see the header). 200 is
    // ≈ -44 dBFS: well below the peaks of quiet speech, well above a silent room.
    uint32_t silence_peak_threshold = 200;
    // Let the helper download the model through Windows Update if it is not installed. Off by
    // default: plan.md §1.9 — nothing is downloaded unless the user asked for it.
    bool install_model = false;
};

class WindowsSpeechBackend final : public Backend {
public:
    WindowsSpeechBackend(WindowsSpeechConfig cfg, std::shared_ptr<SpeechHelper> helper);
    ~WindowsSpeechBackend() override;

    // The spec `shared_speech_helper` should be asked for, given this configuration.
    static SpeechHelper::Spec helper_spec(const WindowsSpeechConfig& cfg);

    Status load() override;
    void reset() override;
    void set_context(const std::vector<std::string>& terms) override;
    [[nodiscard]] const std::string& context_prompt() const noexcept override { return no_prompt_; }
    bool feed(const int16_t* samples, size_t count) override;
    Status transcribe(bool force, Transcription& out) override;
    void cancel() override { cancelled_.store(true); }
    void reset_cancel() override { cancelled_.store(false); }
    void retain_tail(size_t cut_samples) override;
    void set_capture_window(bool on) override { capture_window_ = on; }
    [[nodiscard]] const std::vector<int16_t>& last_window() const noexcept override {
        return last_window_;
    }

    [[nodiscard]] uint64_t requests() const noexcept { return requests_; }
    [[nodiscard]] uint64_t silence_skipped() const noexcept { return silence_skipped_; }

private:
    WindowsSpeechConfig cfg_;
    std::shared_ptr<SpeechHelper> helper_;
    const std::string no_prompt_;  // always empty: this model takes no vocabulary

    mutable std::mutex driver_mutex_;
    PseudoStreamingDriver driver_;
    std::atomic<bool> cancelled_{false};
    bool capture_window_ = false;
    std::vector<int16_t> last_window_;
    bool context_ignored_logged_ = false;

    uint64_t requests_ = 0;
    uint64_t silence_skipped_ = 0;
};

}  // namespace kd::asrd
