// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// `--backend nemotron` — see backend_nemotron.h.

#define KD_LOG_MODULE "asrd.nemotron"

#include "asrd/backend_nemotron.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <map>

#include "model_download.h"
#include "util/log.h"
#include "util/utf8.h"

namespace kd::asrd {

// ─── The onnxruntime-genai C API, the part of it this file uses ─────────────────────────────
//
// Declared here rather than taken from `ort_genai_c.h`, because nothing links against the DLL
// (backend_nemotron.h). The signatures are copied from that header in the
// Microsoft.ML.OnnxRuntimeGenAI 0.20.0 NuGet package; `OGA_API_CALL` is `_stdcall`, which x64
// ignores. A DLL that lacks any of them is refused by name in `load_api`.

extern "C" {
struct OgaResult;
struct OgaModel;
struct OgaTokenizer;
struct OgaTokenizerStream;
struct OgaGeneratorParams;
struct OgaGenerator;
struct OgaStreamingProcessor;
struct OgaNamedTensors;
}

namespace {

namespace fs = std::filesystem;

struct OgaApi {
    void (*SetTelemetryEnabled)(bool);
    void (*Shutdown)();
    const char* (*ResultGetError)(const OgaResult*);
    void (*DestroyResult)(OgaResult*);
    void (*DestroyString)(const char*);
    OgaResult* (*SetLogBool)(const char*, bool);
    OgaResult* (*SetLogCallback)(void (*)(const char*, size_t));
    OgaResult* (*CreateModel)(const char*, OgaModel**);
    void (*DestroyModel)(OgaModel*);
    OgaResult* (*ModelGetType)(const OgaModel*, const char**);
    OgaResult* (*CreateTokenizer)(const OgaModel*, OgaTokenizer**);
    void (*DestroyTokenizer)(OgaTokenizer*);
    OgaResult* (*CreateTokenizerStream)(const OgaTokenizer*, OgaTokenizerStream**);
    void (*DestroyTokenizerStream)(OgaTokenizerStream*);
    OgaResult* (*TokenizerStreamDecode)(OgaTokenizerStream*, int32_t, const char**);
    OgaResult* (*CreateGeneratorParams)(const OgaModel*, OgaGeneratorParams**);
    void (*DestroyGeneratorParams)(OgaGeneratorParams*);
    OgaResult* (*CreateGenerator)(const OgaModel*, const OgaGeneratorParams*, OgaGenerator**);
    void (*DestroyGenerator)(OgaGenerator*);
    OgaResult* (*Generator_SetInputs)(OgaGenerator*, const OgaNamedTensors*);
    bool (*Generator_IsDone)(OgaGenerator*);
    OgaResult* (*Generator_GenerateNextToken)(OgaGenerator*);
    OgaResult* (*Generator_GetNextTokens)(const OgaGenerator*, const int32_t**, size_t*);
    OgaResult* (*CreateStreamingProcessor)(OgaModel*, OgaStreamingProcessor**);
    void (*DestroyStreamingProcessor)(OgaStreamingProcessor*);
    OgaResult* (*StreamingProcessorProcess)(OgaStreamingProcessor*, const float*, size_t,
                                            OgaNamedTensors**);
    OgaResult* (*StreamingProcessorFlush)(OgaStreamingProcessor*, OgaNamedTensors**);
    OgaResult* (*StreamingProcessorSetOption)(OgaStreamingProcessor*, const char*, const char*);
    void (*DestroyNamedTensors)(OgaNamedTensors*);
};

constexpr const wchar_t* kGenAiDll = L"onnxruntime-genai.dll";
constexpr const wchar_t* kOrtDll = L"onnxruntime.dll";

std::string path_text(const fs::path& p) { return utf8::from_utf16_or_empty(p.wstring()); }

fs::path exe_dir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return fs::path(buf).parent_path();
}

void genai_log(const char* text, size_t length) {
    std::string line(text, length);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    KD_LOG_DEBUG("onnxruntime-genai: %s", line.c_str());
}

// Loaded once per process and never unloaded: a runtime that has been shut down may still be
// initialised again (ort_genai_c.h), and unloading it under a thread it started is a crash.
struct Runtime {
    HMODULE module = nullptr;
    OgaApi api{};
    fs::path dir;
};

std::mutex g_runtime_mu;
Runtime* g_runtime = nullptr;  // leaked on purpose, like the speech helpers' map

Result<const Runtime*> load_runtime(const fs::path& dir) {
    std::lock_guard<std::mutex> lock(g_runtime_mu);
    // The folder asked for is checked first, loaded or not, so a missing DLL is always reported
    // as exactly that.
    std::error_code ec;
    for (const wchar_t* name : {kGenAiDll, kOrtDll}) {
        if (!fs::is_regular_file(dir / name, ec)) {
            return Status{StatusCode::NotFound,
                          "the Nemotron runtime is not installed: " + path_text(dir / name) +
                              " is missing. Reinstall kvit-dictate, or choose another speech "
                              "engine in Settings > Server."};
        }
    }
    if (g_runtime != nullptr) {
        if (g_runtime->dir != dir) {
            return Status{StatusCode::Unsupported,
                          "the Nemotron runtime is already loaded from " + path_text(g_runtime->dir)};
        }
        return static_cast<const Runtime*>(g_runtime);
    }

    // Before the DLL exists in this process: backend_nemotron.h. Its C runtime reads the
    // environment block when it first needs it, which is after this call.
    SetEnvironmentVariableW(L"ORT_DISABLE_TELEMETRY", L"1");

    // By full path, with its own folder first for its dependencies — onnxruntime.dll and the
    // Visual C++ runtime DLLs beside it — and then System32. Never the current directory or PATH.
    const HMODULE module =
        LoadLibraryExW((dir / kGenAiDll).c_str(), nullptr,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module == nullptr) {
        const DWORD err = GetLastError();
        return Status{StatusCode::Unavailable,
                      "the Nemotron runtime in " + path_text(dir) +
                          " could not be loaded (Windows error " + std::to_string(err) +
                          "). A DLL beside it may be missing or from another version.",
                      err};
    }

    auto* rt = new Runtime();
    rt->module = module;
    rt->dir = dir;
    const char* missing = nullptr;
    const auto bind = [&](auto& slot, const char* name) {
        slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(
            reinterpret_cast<void*>(GetProcAddress(module, name)));
        if (slot == nullptr && missing == nullptr) missing = name;
    };
    OgaApi& a = rt->api;
    bind(a.SetTelemetryEnabled, "OgaSetTelemetryEnabled");
    bind(a.Shutdown, "OgaShutdown");
    bind(a.ResultGetError, "OgaResultGetError");
    bind(a.DestroyResult, "OgaDestroyResult");
    bind(a.DestroyString, "OgaDestroyString");
    bind(a.SetLogBool, "OgaSetLogBool");
    bind(a.SetLogCallback, "OgaSetLogCallback");
    bind(a.CreateModel, "OgaCreateModel");
    bind(a.DestroyModel, "OgaDestroyModel");
    bind(a.ModelGetType, "OgaModelGetType");
    bind(a.CreateTokenizer, "OgaCreateTokenizer");
    bind(a.DestroyTokenizer, "OgaDestroyTokenizer");
    bind(a.CreateTokenizerStream, "OgaCreateTokenizerStream");
    bind(a.DestroyTokenizerStream, "OgaDestroyTokenizerStream");
    bind(a.TokenizerStreamDecode, "OgaTokenizerStreamDecode");
    bind(a.CreateGeneratorParams, "OgaCreateGeneratorParams");
    bind(a.DestroyGeneratorParams, "OgaDestroyGeneratorParams");
    bind(a.CreateGenerator, "OgaCreateGenerator");
    bind(a.DestroyGenerator, "OgaDestroyGenerator");
    bind(a.Generator_SetInputs, "OgaGenerator_SetInputs");
    bind(a.Generator_IsDone, "OgaGenerator_IsDone");
    bind(a.Generator_GenerateNextToken, "OgaGenerator_GenerateNextToken");
    bind(a.Generator_GetNextTokens, "OgaGenerator_GetNextTokens");
    bind(a.CreateStreamingProcessor, "OgaCreateStreamingProcessor");
    bind(a.DestroyStreamingProcessor, "OgaDestroyStreamingProcessor");
    bind(a.StreamingProcessorProcess, "OgaStreamingProcessorProcess");
    bind(a.StreamingProcessorFlush, "OgaStreamingProcessorFlush");
    bind(a.StreamingProcessorSetOption, "OgaStreamingProcessorSetOption");
    bind(a.DestroyNamedTensors, "OgaDestroyNamedTensors");
    if (missing != nullptr) {
        // Not freed: see `Runtime`. A DLL this program cannot use is merely left unused.
        delete rt;
        return Status{StatusCode::Unsupported,
                      std::string("the Nemotron runtime in ") + path_text(dir) +
                          " is a version this build cannot use: it has no " + missing};
    }
    a.SetTelemetryEnabled(false);
    if (OgaResult* r = a.SetLogCallback(&genai_log)) a.DestroyResult(r);
    if (OgaResult* r = a.SetLogBool("enabled", kd::log::level_enabled(kd::log::Level::Debug))) {
        a.DestroyResult(r);
    }
    KD_LOG_INFO("loaded the Nemotron runtime from %s (telemetry off)", path_text(dir).c_str());
    g_runtime = rt;
    return static_cast<const Runtime*>(rt);
}

// Turns an OgaResult into a Status, and destroys it.
Status oga_status(const OgaApi& api, OgaResult* r, const char* what) {
    if (r == nullptr) return Status::success();
    const char* e = api.ResultGetError(r);
    std::string msg = std::string(what) + ": " + (e != nullptr ? e : "unknown error");
    api.DestroyResult(r);
    return Status{StatusCode::Unavailable, std::move(msg)};
}

}  // namespace

// ─── The engine: one runtime, one model ─────────────────────────────────────────────────────

class NemotronEngine {
public:
    NemotronEngine(const Runtime* rt, OgaModel* model, OgaTokenizer* tokenizer)
        : rt_(rt), model_(model), tokenizer_(tokenizer) {}
    ~NemotronEngine() {
        if (tokenizer_ != nullptr) rt_->api.DestroyTokenizer(tokenizer_);
        if (model_ != nullptr) rt_->api.DestroyModel(model_);
    }
    NemotronEngine(const NemotronEngine&) = delete;
    NemotronEngine& operator=(const NemotronEngine&) = delete;

    [[nodiscard]] const OgaApi& api() const noexcept { return rt_->api; }
    [[nodiscard]] OgaModel* model() const noexcept { return model_; }
    [[nodiscard]] OgaTokenizer* tokenizer() const noexcept { return tokenizer_; }

private:
    const Runtime* rt_;
    OgaModel* model_;
    OgaTokenizer* tokenizer_;
};

namespace {

class StreamImpl final : public NemotronStream {
public:
    StreamImpl(const OgaApi& api) : api_(api) {}
    ~StreamImpl() override {
        if (ts_ != nullptr) api_.DestroyTokenizerStream(ts_);
        if (gen_ != nullptr) api_.DestroyGenerator(gen_);
        if (params_ != nullptr) api_.DestroyGeneratorParams(params_);
        if (proc_ != nullptr) api_.DestroyStreamingProcessor(proc_);
    }

    Status init(NemotronEngine& e) {
        Status st = oga_status(api_, api_.CreateStreamingProcessor(e.model(), &proc_),
                               "creating the feature extractor");
        if (!st.ok()) return st;
        // backend_nemotron.h: the session's endpointer has already decided what is speech.
        st = oga_status(api_, api_.StreamingProcessorSetOption(proc_, "use_vad", "false"),
                        "switching the model's VAD off");
        if (!st.ok()) return st;
        st = oga_status(api_, api_.CreateGeneratorParams(e.model(), &params_),
                        "creating the decoder parameters");
        if (!st.ok()) return st;
        st = oga_status(api_, api_.CreateGenerator(e.model(), params_, &gen_), "creating the decoder");
        if (!st.ok()) return st;
        return oga_status(api_, api_.CreateTokenizerStream(e.tokenizer(), &ts_),
                          "creating the detokeniser");
    }

    Status process_chunk(const float* samples, std::string& text_out) override {
        OgaNamedTensors* features = nullptr;
        const Status st =
            oga_status(api_, api_.StreamingProcessorProcess(proc_, samples, kNemotronChunkSamples,
                                                            &features),
                       "extracting features");
        if (!st.ok()) return st;
        return decode(features, text_out);
    }

    Status flush(const float* samples, size_t count, std::string& text_out) override {
        if (count > 0) {
            // Less than a chunk, so the extractor keeps it and answers nothing; the flush below
            // pads it to a whole chunk with silence.
            OgaNamedTensors* features = nullptr;
            const Status st = oga_status(
                api_, api_.StreamingProcessorProcess(proc_, samples, count, &features),
                "extracting features");
            if (!st.ok()) return st;
            if (features != nullptr) {
                const Status d = decode(features, text_out);
                if (!d.ok()) return d;
            }
        }
        OgaNamedTensors* features = nullptr;
        const Status st = oga_status(api_, api_.StreamingProcessorFlush(proc_, &features),
                                     "flushing the feature extractor");
        if (!st.ok()) return st;
        return decode(features, text_out);
    }

private:
    Status decode(OgaNamedTensors* features, std::string& text_out) {
        if (features == nullptr) return Status::success();
        Status st = oga_status(api_, api_.Generator_SetInputs(gen_, features), "decoding");
        api_.DestroyNamedTensors(features);
        // A chunk yields a handful of tokens; a bound keeps a confused runtime from looping.
        for (int steps = 0; st.ok() && !api_.Generator_IsDone(gen_) && steps < 512; ++steps) {
            st = oga_status(api_, api_.Generator_GenerateNextToken(gen_), "decoding");
            if (!st.ok()) break;
            const int32_t* tokens = nullptr;
            size_t n = 0;
            st = oga_status(api_, api_.Generator_GetNextTokens(gen_, &tokens, &n), "decoding");
            for (size_t i = 0; st.ok() && i < n; ++i) {
                const char* piece = nullptr;
                st = oga_status(api_, api_.TokenizerStreamDecode(ts_, tokens[i], &piece),
                                "detokenising");
                if (st.ok() && piece != nullptr) text_out += piece;
            }
        }
        return st;
    }

    const OgaApi& api_;
    OgaStreamingProcessor* proc_ = nullptr;
    OgaGeneratorParams* params_ = nullptr;
    OgaGenerator* gen_ = nullptr;
    OgaTokenizerStream* ts_ = nullptr;
};

std::mutex g_engines_mu;
std::map<std::wstring, std::shared_ptr<NemotronEngine>>& engines() {
    static auto* m = new std::map<std::wstring, std::shared_ptr<NemotronEngine>>();
    return *m;
}

}  // namespace

fs::path default_nemotron_runtime_dir() { return exe_dir() / L"nemotron"; }

Result<std::shared_ptr<NemotronEngine>> shared_nemotron_engine(const NemotronConfig& cfg) {
    const fs::path runtime_dir = cfg.runtime_dir.empty() ? default_nemotron_runtime_dir()
                                                         : cfg.runtime_dir;
    const fs::path model_dir = cfg.model_dir.empty() ? default_nemotron_model_dir() : cfg.model_dir;

    // Serialised: two connections must not both download 728 MB or both load the model.
    std::lock_guard<std::mutex> lock(g_engines_mu);
    std::shared_ptr<NemotronEngine>& slot = engines()[model_dir.wstring()];
    if (slot) return slot;

    Result<const Runtime*> rt = load_runtime(runtime_dir);
    if (!rt.is_ok()) return rt.status();
    const OgaApi& api = (*rt)->api;

    const CatalogModel& catalog = nemotron_en_catalog_model();
    if (Status have = verify_model_dir(catalog, model_dir, false); !have.ok()) {
        if (!cfg.install_model) {
            return Status{StatusCode::NotFound,
                          "the Nemotron speech model is not downloaded yet (" + have.message() +
                              " in " + path_text(model_dir) +
                              "). Open Settings > Server and press \"Download Nemotron model\": " +
                              std::to_string(catalog.total_bytes() / 1000000) +
                              " MB, once, from Microsoft's model catalog."};
        }
        std::atomic<bool> never{false};
        uint64_t last_logged = 0;
        const Status st = download_catalog_model(
            catalog, model_dir,
            [&last_logged](uint64_t done, uint64_t total) {
                if (done == total || done - last_logged >= 50u * 1000u * 1000u) {
                    last_logged = done;
                    KD_LOG_INFO("downloading the Nemotron model: %llu of %llu MB",
                                static_cast<unsigned long long>(done / 1000000),
                                static_cast<unsigned long long>(total / 1000000));
                }
            },
            never);
        if (!st.ok()) {
            return Status{st.code(), "the Nemotron model could not be downloaded: " + st.message(),
                          st.os_error()};
        }
    }

    const auto t0 = std::chrono::steady_clock::now();
    OgaModel* model = nullptr;
    const std::string model_path = path_text(model_dir);
    Status st = oga_status(api, api.CreateModel(model_path.c_str(), &model),
                           "loading the Nemotron model");
    if (!st.ok()) return st;
    const char* type = nullptr;
    st = oga_status(api, api.ModelGetType(model, &type), "reading the model type");
    const std::string type_text = type != nullptr ? type : "";
    if (type != nullptr) api.DestroyString(type);
    if (st.ok() && type_text != "nemotron_speech") {
        st = Status{StatusCode::Malformed,
                    "the model in " + model_path + " is a \"" + type_text +
                        "\" model, not nemotron_speech"};
    }
    OgaTokenizer* tokenizer = nullptr;
    if (st.ok()) {
        st = oga_status(api, api.CreateTokenizer(model, &tokenizer), "loading the tokenizer");
    }
    if (!st.ok()) {
        api.DestroyModel(model);
        return st;
    }
    KD_LOG_INFO("loaded the Nemotron model from %s in %lld ms", model_path.c_str(),
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - t0)
                                           .count()));
    slot = std::make_shared<NemotronEngine>(*rt, model, tokenizer);
    return slot;
}

Result<std::unique_ptr<NemotronStream>> create_nemotron_stream(NemotronEngine& engine) {
    auto s = std::make_unique<StreamImpl>(engine.api());
    const Status st = s->init(engine);
    if (!st.ok()) return st;
    return std::unique_ptr<NemotronStream>(std::move(s));
}

void release_nemotron_engines() {
    std::map<std::wstring, std::shared_ptr<NemotronEngine>> taken;
    {
        std::lock_guard<std::mutex> lock(g_engines_mu);
        taken.swap(engines());
    }
    taken.clear();  // the last references, if no backend still holds one
    std::lock_guard<std::mutex> lock(g_runtime_mu);
    if (g_runtime != nullptr) g_runtime->api.Shutdown();
}

// ─── The backend ────────────────────────────────────────────────────────────────────────────

NemotronBackend::NemotronBackend(NemotronConfig cfg) : cfg_(std::move(cfg)) {}

NemotronBackend::~NemotronBackend() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        quit_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

Status NemotronBackend::load() {
    if (engine_) return Status::success();
    Result<std::shared_ptr<NemotronEngine>> engine = shared_nemotron_engine(cfg_);
    if (!engine.is_ok()) return engine.status();
    engine_ = *engine;
    // One stream up front, so a runtime that cannot decode fails the Test rather than the first
    // dictation. The worker makes its own.
    Result<std::unique_ptr<NemotronStream>> probe = create_nemotron_stream(*engine_);
    if (!probe.is_ok()) return probe.status();
    worker_ = std::thread([this] { worker(); });
    return Status::success();
}

void NemotronBackend::reset() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        ++generation_;
        pending_.clear();
        text_.clear();
        hop_version_ = text_version_;
        flush_wanted_ = false;
        flushed_ = false;
        worker_error_ = Status::success();
        window_.clear();
    }
    cv_.notify_all();
}

void NemotronBackend::set_context(const std::vector<std::string>& terms) {
    if (!terms.empty() && !context_ignored_logged_) {
        context_ignored_logged_ = true;
        KD_LOG_INFO("the session sent %zu vocabulary terms; the Nemotron model takes no "
                    "vocabulary, so they are not used",
                    terms.size());
    }
}

bool NemotronBackend::feed(const int16_t* samples, size_t count) {
    bool wake = false;
    bool hop = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        pending_.insert(pending_.end(), samples, samples + count);
        if (capture_) window_.insert(window_.end(), samples, samples + count);
        wake = pending_.size() >= kNemotronChunkSamples;
        if (!cfg_.finalize_only && text_version_ != hop_version_) {
            hop_version_ = text_version_;
            hop = true;
        }
    }
    if (wake) cv_.notify_all();
    return hop;
}

Status NemotronBackend::transcribe(bool force, Transcription& out) {
    out = {};
    std::unique_lock<std::mutex> lock(mu_);
    if (!engine_) return Status{StatusCode::Internal, "the Nemotron backend was not loaded"};
    if (force) {
        flush_wanted_ = true;
        flushed_ = false;
        cv_.notify_all();
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg_.flush_timeout_ms);
        while (!flushed_ && worker_error_.ok()) {
            if (cancelled_.load()) {
                return Status{StatusCode::Unavailable, "the decode was cancelled"};
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return Status{StatusCode::Timeout,
                              "the Nemotron model did not finish the utterance within " +
                                  std::to_string(cfg_.flush_timeout_ms) + " ms"};
            }
            cv_.wait_for(lock, std::chrono::milliseconds(50));
        }
    }
    if (!worker_error_.ok()) return worker_error_;
    if (capture_) last_window_ = window_;

    const size_t first = text_.find_first_not_of(" \t\r\n");
    const size_t last = text_.find_last_not_of(" \t\r\n");
    out.text = first == std::string::npos ? std::string() : text_.substr(first, last - first + 1);
    // A hop with nothing recognised yet publishes nothing; a finalize always answers.
    out.have_text = force || !out.text.empty();
    return Status::success();
}

void NemotronBackend::cancel() {
    cancelled_.store(true);
    cv_.notify_all();
}

void NemotronBackend::retain_tail(size_t /*cut_samples*/) {
    // Never reached: this backend proposes no cuts (it has no timestamps).
}

void NemotronBackend::set_capture_window(bool on) {
    std::lock_guard<std::mutex> lock(mu_);
    capture_ = on;
}

void NemotronBackend::worker() {
    std::unique_ptr<NemotronStream> stream;
    uint64_t stream_generation = UINT64_MAX;
    bool stream_flushed = false;
    std::vector<float> chunk;

    const auto take = [&](size_t n) {
        chunk.resize(n);
        for (size_t i = 0; i < n; ++i) chunk[i] = static_cast<float>(pending_[i]) / 32768.0f;
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<ptrdiff_t>(n));
    };

    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
        cv_.wait(lock, [&] {
            return quit_ || stream_generation != generation_ ||
                   (worker_error_.ok() && (pending_.size() >= kNemotronChunkSamples ||
                                           (flush_wanted_ && !flushed_)));
        });
        if (quit_) return;

        if (stream_generation != generation_) {
            // A new utterance: a fresh stream, made now so the first chunk does not wait for it.
            const uint64_t g = generation_;
            lock.unlock();
            stream.reset();
            Result<std::unique_ptr<NemotronStream>> made = create_nemotron_stream(*engine_);
            lock.lock();
            stream_generation = g;
            stream_flushed = false;
            if (made.is_ok()) {
                stream = std::move(*made);
            } else if (generation_ == g) {
                worker_error_ = made.status();
                cv_.notify_all();
            }
            continue;
        }

        const uint64_t g = generation_;
        std::string delta;
        Status st;
        bool was_flush = false;
        if (pending_.size() >= kNemotronChunkSamples) {
            take(kNemotronChunkSamples);
            lock.unlock();
            st = stream->process_chunk(chunk.data(), delta);
            chunks_decoded_.fetch_add(1);
        } else {
            take(pending_.size());
            was_flush = true;
            lock.unlock();
            if (!stream_flushed) st = stream->flush(chunk.data(), chunk.size(), delta);
            stream_flushed = true;
        }
        lock.lock();
        if (generation_ != g) continue;  // reset while decoding: the answer is for nobody
        if (!st.ok()) {
            KD_LOG_WARN("the Nemotron model failed: %s", st.message().c_str());
            worker_error_ = st;
        } else if (!delta.empty()) {
            text_ += delta;
            ++text_version_;
        }
        if (was_flush) flushed_ = true;
        cv_.notify_all();
    }
}

}  // namespace kd::asrd
