// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// `--backend nemotron`: NVIDIA Nemotron Speech Streaming 0.6B (English) on this machine's CPU.
//
// The model is the one VS Code's dictation and Copilot CLI's `/voice` download: Microsoft's
// Foundry Local catalog entry `nemotron-speech-streaming-en-0.6b-generic-cpu:3`, ONNX, int4,
// 728 MB. docs/copilot-speech-model.md is the measurement record; the facts that shape this file
// are:
//
//   * **The runtime is onnxruntime-genai, loaded at run time.** `onnxruntime-genai.dll`,
//     `onnxruntime.dll` and the Visual C++ runtime DLLs they need sit in `nemotron\` beside this
//     executable and are opened with `LoadLibraryExW` by full path. Nothing links against them,
//     so a build or an install without them still runs every other backend, and a missing DLL
//     is a sentence in Settings rather than a process that will not start.
//
//   * **Its telemetry is switched off before it exists.** `onnxruntime-genai.dll` contains
//     Microsoft's 1DS uploader. Its header says `ORT_DISABLE_TELEMETRY=1`, set before
//     initialisation, "prevents the uploader, events, and persistent device identifier from being
//     created for the process lifetime", so this process sets it before the DLL is loaded, and
//     calls `OgaSetTelemetryEnabled(false)` after. The Foundry Local runtime, which cannot be
//     silenced that far, is not used at all; model_download.h fetches the files itself.
//
//   * **Natively streaming, and decoded as the audio arrives.** The model takes 560 ms chunks;
//     one costs about 75 ms on an i7-14700K. A worker thread decodes each chunk as soon as it is
//     complete, whether or not live preview is on, so when the speaker stops only the last,
//     partial chunk is left: the final arrives about a tenth of a second after the endpoint,
//     however long the utterance was. `transcribe` only reads what the worker has produced.
//
//   * **Its text only grows.** Greedy RNN-T emits each token once and never revises it, so a
//     preview never re-cases or re-punctuates what it already showed. `publish_stable_partials`
//     is still off (connection.cpp): the last token of a partial can be half a word.
//
//   * **The model's own VAD is off.** The session's endpointer already decides what is speech,
//     and on the corpus the model's Silero gate dropped a word the endpointer had kept.
//
//   * **No vocabulary and no timestamps.** `ready.context_supported` is false, and an utterance
//     is bounded by the endpointer's `max_utterance_ms`.
//
//   * **The model belongs to the process.** Loading takes ~0.7 s and ~900 MB, so the first
//     connection loads it and every later one reuses it (`shared_nemotron_engine`).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "asrd/backend.h"
#include "util/result.h"

namespace kd::asrd {

// What `ready.model` and `hello_ack.models[0].id` say for this backend.
inline constexpr const char* kNemotronModel = "nemotron-speech-streaming-en-0.6b";

// The chunk the int4 export was built for: genai_config.json `chunk_samples`, 560 ms at 16 kHz.
inline constexpr size_t kNemotronChunkSamples = 8960;

// `nemotron\` beside this executable. The model folder is `default_nemotron_model_dir()`
// (model_download.h), shared with the Settings page that downloads it.
std::filesystem::path default_nemotron_runtime_dir();

struct NemotronConfig {
    std::filesystem::path runtime_dir;  // empty means default_nemotron_runtime_dir()
    std::filesystem::path model_dir;    // empty means default_nemotron_model_dir()
    // Download the model if it is not there. Off by default: plan.md §1.9 — nothing is
    // downloaded unless the user asked for it, and only Settings' Test button asks.
    bool install_model = false;
    // Whether partials are wanted. Decoding runs either way; this only decides whether `feed`
    // asks the session for a hop when new text has appeared.
    bool finalize_only = false;
    // How long a final may take once the speaker has stopped. One chunk is ~75 ms; ten seconds
    // is a wedged runtime.
    uint32_t flush_timeout_ms = 10000;
};

// The loaded runtime and model. Thread-safe: streams may be created and used from any thread.
class NemotronEngine;

// One utterance's decoding state: a feature extractor, a decoder and a detokeniser.
class NemotronStream {
public:
    virtual ~NemotronStream() = default;
    // Exactly `kNemotronChunkSamples` samples in [-1, 1]. Appends what was recognised.
    virtual Status process_chunk(const float* samples, std::string& text_out) = 0;
    // The end of the utterance: up to one chunk's worth of remaining samples, padded with
    // silence by the runtime. After this the stream is finished.
    virtual Status flush(const float* samples, size_t count, std::string& text_out) = 0;
};

// The process-wide engine for `cfg`, loaded on first use: DLLs, then the model (downloaded first
// when `cfg.install_model` and it is missing). Fails with a sentence the user can act on.
Result<std::shared_ptr<NemotronEngine>> shared_nemotron_engine(const NemotronConfig& cfg);
Result<std::unique_ptr<NemotronStream>> create_nemotron_stream(NemotronEngine& engine);
// Destroy every engine and shut the runtime down. `main` calls it on the way out, after the
// last connection: the runtime's globals must not be left to static destruction.
void release_nemotron_engines();

class NemotronBackend final : public Backend {
public:
    // The engine is loaded by `load()`, so a missing runtime or model is a fatal `error`
    // instead of `ready`, in the user's words.
    explicit NemotronBackend(NemotronConfig cfg);
    ~NemotronBackend() override;
    NemotronBackend(const NemotronBackend&) = delete;
    NemotronBackend& operator=(const NemotronBackend&) = delete;

    Status load() override;
    void reset() override;
    void set_context(const std::vector<std::string>& terms) override;
    [[nodiscard]] const std::string& context_prompt() const noexcept override { return no_prompt_; }
    bool feed(const int16_t* samples, size_t count) override;
    Status transcribe(bool force, Transcription& out) override;
    void cancel() override;
    void reset_cancel() override { cancelled_.store(false); }
    void retain_tail(size_t cut_samples) override;
    void set_capture_window(bool on) override;
    [[nodiscard]] const std::vector<int16_t>& last_window() const noexcept override {
        return last_window_;
    }

    [[nodiscard]] uint64_t chunks_decoded() const noexcept { return chunks_decoded_.load(); }

private:
    void worker();

    NemotronConfig cfg_;
    std::shared_ptr<NemotronEngine> engine_;
    const std::string no_prompt_;
    bool context_ignored_logged_ = false;
    std::atomic<bool> cancelled_{false};
    std::atomic<uint64_t> chunks_decoded_{0};

    // Everything below `mu_` is shared between the connection thread (`feed`, `reset`), the
    // decode thread (`transcribe`) and the worker.
    std::mutex mu_;
    std::condition_variable cv_;
    bool quit_ = false;
    uint64_t generation_ = 0;        // one per utterance; `reset` advances it
    std::vector<int16_t> pending_;   // fed, not yet given to the model
    std::string text_;               // cumulative, for `generation_`
    uint64_t text_version_ = 0;      // advances whenever `text_` grows
    uint64_t hop_version_ = 0;       // the version the last hop request covered
    bool flush_wanted_ = false;
    bool flushed_ = false;
    Status worker_error_;            // the current utterance's, cleared by `reset`
    bool capture_ = false;
    std::vector<int16_t> window_;    // the whole utterance, when capturing
    std::vector<int16_t> last_window_;

    std::thread worker_;
};

}  // namespace kd::asrd
