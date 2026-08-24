// Normalizer — see normalizer.h for the design and for why this is a client-side module.
//
// ─── Windows facts this file depends on, written down because each cost a debugging session ──
//
//   * WinHttpCrackUrl needs every pointer in URL_COMPONENTS to be either null with a zero length,
//     or a real buffer with a real length. Setting a length to a non-zero value with a null
//     pointer makes it return the *offsets into the original string* instead, with the lengths in
//     dwXxxLength — which is what this file wants, and it is why the buffers are null here and
//     the substrings are cut by hand afterwards.
//   * WinHttpReceiveResponse must be called before WinHttpQueryHeaders or WinHttpReadData, and
//     WinHttpQueryDataAvailable returning 0 is the only reliable end-of-body signal. A reader
//     that loops on ReadData alone and stops at a short read truncates any body that arrives in
//     more than one TCP segment — which for a paragraph-length rewrite is most of them.
//   * The timeouts set by WinHttpSetTimeouts are per-operation, not per-request. A request that
//     stalls after each of four operations can therefore take four times the configured value,
//     so the total is bounded again by hand: each operation gets a share, and the elapsed time is
//     checked between them. main.cpp's finalize grace depends on this being a real bound.
//   * WinHttpSetOption(WINHTTP_OPTION_CONTEXT_VALUE) is not used and no async callback is
//     installed: this file is synchronous on a thread of its own, which is the shape the rest of
//     the program already uses for the pipe (ipc_client.cpp) and is far easier to reason about
//     than an async state machine for a request that happens once a session.

#define KD_LOG_MODULE "norm"

#include "normalizer.h"

#include <windows.h>
// Must follow windows.h.
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "util/clock.h"
#include "util/json.h"
#include "util/log.h"
#include "util/utf8.h"

namespace kd {
namespace {

bool value_in(const char* const* table, size_t n, const std::string& v) {
    for (size_t i = 0; i < n; ++i) {
        if (v == table[i]) return true;
    }
    return false;
}

std::string join_quoted(const char* const* table, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        if (i != 0) s += i + 1 == n ? " or " : ", ";
        s += '"';
        s += table[i];
        s += '"';
    }
    return s;
}

// ASCII-only lowercase. Deliberately not towlower or any locale-aware form: the protected terms
// are identifiers, paths and flags, all ASCII, and a locale-aware fold introduces the Turkish
// dotless-i problem into a comparison whose whole job is to be predictable. A term containing
// non-ASCII simply compares case-sensitively in those bytes, which is the safe direction — it
// can only make the guard stricter, never laxer.
std::string ascii_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        out.push_back(u < 0x80 ? static_cast<char>(std::tolower(u)) : c);
    }
    return out;
}

}  // namespace

// ─── Configuration ──────────────────────────────────────────────────────────────────────────

Status NormalizerConfig::validate(bool cadence_is_end_of_session) const {
    if (!enabled) return Status::success();

    // The rule normalizer.h spends a section on, enforced in one place. It is checked before the
    // URL so that a user who turns this on under the wrong cadence is told the real reason rather
    // than being sent to fix a URL that was never the problem.
    if (!cadence_is_end_of_session) {
        return Status{StatusCode::InvalidArgument,
                      "settings.normalizer.enabled requires cadence \"end-of-session\": a "
                      "rewrite is not prefix-stable, and under an immediate cadence the text it "
                      "would revise is already in the document (plan.md 1.4)"};
    }
    if (url.empty()) {
        return Status{StatusCode::InvalidArgument,
                      "settings.normalizer.url is empty but the normalizer is enabled"};
    }
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        return Status{StatusCode::InvalidArgument,
                      "settings.normalizer.url must begin with http:// or https://, got \"" +
                          url + "\""};
    }
    if (timeout_ms < 100 || timeout_ms > 30000) {
        return Status{StatusCode::InvalidArgument,
                      "settings.normalizer.timeout_ms must be 100..30000, got " +
                          std::to_string(timeout_ms)};
    }
    if (max_bytes < 64 || max_bytes > 65536) {
        return Status{StatusCode::InvalidArgument,
                      "settings.normalizer.max_bytes must be 64..65536, got " +
                          std::to_string(max_bytes)};
    }

    // The endpoint answers an untrained control value with 400 rather than passing it to the
    // model, which would otherwise invent text for it. Catching it here means the settings file
    // is rejected at load, naming the field, instead of every session failing silently.
    const auto n_styling = sizeof(kNormalizerStylings) / sizeof(*kNormalizerStylings);
    const auto n_structure = sizeof(kNormalizerStructures) / sizeof(*kNormalizerStructures);
    const auto n_context = sizeof(kNormalizerContexts) / sizeof(*kNormalizerContexts);
    if (!value_in(kNormalizerStylings, n_styling, styling)) {
        return Status{StatusCode::InvalidArgument,
                      "settings.normalizer.styling: expected " +
                          join_quoted(kNormalizerStylings, n_styling) + ", got \"" + styling +
                          "\""};
    }
    if (!value_in(kNormalizerStructures, n_structure, structure)) {
        return Status{StatusCode::InvalidArgument,
                      "settings.normalizer.structure: expected " +
                          join_quoted(kNormalizerStructures, n_structure) + ", got \"" +
                          structure + "\""};
    }
    if (!value_in(kNormalizerContexts, n_context, context)) {
        return Status{StatusCode::InvalidArgument,
                      "settings.normalizer.context: expected " +
                          join_quoted(kNormalizerContexts, n_context) + ", got \"" + context +
                          "\""};
    }
    return Status::success();
}

// ─── The guard ──────────────────────────────────────────────────────────────────────────────

const std::vector<std::string>& builtin_protected_terms() {
    // plan.md §4's list, in step with bench/metrics.py's TERMS table. Canonical spellings only.
    //
    // Keep this list and that one together when either changes: the bench measures what this
    // enforces, and a term the bench scores but the app does not protect is a measurement that
    // does not describe the shipped program.
    static const std::vector<std::string> terms = {
        "llama.cpp",  "whisper.cpp",     "sherpa-onnx",    "onnxruntime", "ONNX",
        "sm_120",     "--n-gpu-layers",  "AppData",        "RTX",         "CUDA",
        "PTX",        "JIT",             "WASAPI",         "int8",        "int4",
        "Blackwell",  "Qwen3",           "kvit-dictate",   "Win+H",       "FIR",
        "UV_CACHE_DIR",
    };
    return terms;
}

const char* to_string(GuardVerdict v) noexcept {
    switch (v) {
        case GuardVerdict::Accept:         return "Accept";
        case GuardVerdict::RejectEmpty:    return "RejectEmpty";
        case GuardVerdict::RejectTermLost: return "RejectTermLost";
        case GuardVerdict::RejectTooShort: return "RejectTooShort";
    }
    return "Unknown";
}

GuardResult guard_verdict(std::string_view raw, std::string_view normalized,
                          const std::vector<std::string>& extra_terms, double min_ratio) {
    GuardResult r;

    // Whitespace-only counts as empty. A model that answers a paragraph with a newline has failed
    // in the way that costs the most, and "the pill went blank" is the report it produces.
    const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    const bool blank = std::all_of(normalized.begin(), normalized.end(),
                                   [&](char c) { return is_space(static_cast<unsigned char>(c)); });
    if (blank) {
        r.verdict = GuardVerdict::RejectEmpty;
        return r;
    }

    // The length floor, checked before the term scan because it is the cheaper test and because a
    // truncated answer will usually trip it first. Normalisation legitimately shortens — every
    // filler removed, every "twenty five dollars" that becomes "$25" — so the floor is generous;
    // it is here to catch the model answering a paragraph with a sentence, which no term list
    // can catch because the terms it kept may all be in the sentence it did return.
    if (!raw.empty()) {
        const double ratio =
            static_cast<double>(normalized.size()) / static_cast<double>(raw.size());
        if (ratio < min_ratio) {
            r.verdict = GuardVerdict::RejectTooShort;
            return r;
        }
    }

    // Case-insensitive substring, so `sm_120` inside `--arch=sm_120` counts as surviving. The
    // alternative — tokenising both sides — would need this module to hold an opinion about word
    // boundaries in `--n-gpu-layers` and `C:\Users\sk\AppData` that the model does not share, and
    // every disagreement would show up as a false rejection of a perfectly good rewrite.
    const std::string raw_l = ascii_lower(raw);
    const std::string norm_l = ascii_lower(normalized);

    const auto check = [&](const std::vector<std::string>& terms) -> bool {
        for (const std::string& term : terms) {
            if (term.empty()) continue;
            const std::string t = ascii_lower(term);
            // Only terms the user actually said are protected. A term absent from both sides is
            // not evidence of anything, and treating it as a loss would reject every rewrite of
            // every sentence that did not happen to mention llama.cpp.
            if (raw_l.find(t) == std::string::npos) continue;
            if (norm_l.find(t) == std::string::npos) {
                r.verdict = GuardVerdict::RejectTermLost;
                r.lost_term = term;
                return false;
            }
        }
        return true;
    };

    if (!check(builtin_protected_terms())) return r;
    if (!check(extra_terms)) return r;

    r.verdict = GuardVerdict::Accept;
    return r;
}

// ─── Request and response codecs ────────────────────────────────────────────────────────────

std::string build_request_body(const NormalizerConfig& cfg, std::string_view text) {
    json::Value o = json::Value::object();
    o.set("text", json::Value::string(std::string(text)));
    o.set("styling", json::Value::string(cfg.styling));
    o.set("structure", json::Value::string(cfg.structure));
    o.set("context", json::Value::string(cfg.context));
    // Asking for JSON explicitly rather than relying on the endpoint's default, so that a change
    // of default on the server side cannot turn every response into an unparseable success.
    o.set("response_format", json::Value::string("json"));
    return json::serialize(o);
}

Result<std::string> parse_response_body(std::string_view body) {
    json::Value root;
    json::Error err;
    json::Options opts;
    // The response is one rewrite of a buffer this side capped at NormalizerConfig::max_bytes.
    // A body far past that is not a rewrite, it is an error page or a different service.
    opts.max_bytes = 1u << 20;
    if (!json::parse_object(body, root, err, opts)) {
        return Status{StatusCode::Malformed,
                      std::string("normalizer response is not a JSON object: ") +
                          json::to_string(err.code)};
    }
    const json::Value* t = root.find("text");
    if (t == nullptr) {
        return Status{StatusCode::Malformed, "normalizer response has no \"text\" member"};
    }
    std::string_view out;
    if (!t->get_string(out)) {
        return Status{StatusCode::Malformed, "normalizer response \"text\" is not a string"};
    }
    return std::string(out);
}

// ─── The client ─────────────────────────────────────────────────────────────────────────────

namespace {

struct ParsedUrl {
    std::wstring host;
    std::wstring path;  // path + query, as WinHttpOpenRequest wants it
    uint16_t port = 0;
    bool secure = false;
};

bool crack_url(const std::string& url, ParsedUrl& out) {
    std::wstring wide;
    if (!utf8::to_utf16(url, wide)) return false;

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    // Null buffers with non-zero lengths: WinHttpCrackUrl then reports offsets into `wide`
    // rather than copying, which is what this wants. See the note at the top of this file.
    uc.dwSchemeLength = static_cast<DWORD>(-1);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &uc)) return false;
    if (uc.lpszHostName == nullptr || uc.dwHostNameLength == 0) return false;

    out.host.assign(uc.lpszHostName, uc.dwHostNameLength);
    out.port = uc.nPort;
    out.secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

    out.path.clear();
    if (uc.lpszUrlPath != nullptr && uc.dwUrlPathLength != 0) {
        out.path.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    }
    if (uc.lpszExtraInfo != nullptr && uc.dwExtraInfoLength != 0) {
        out.path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    }
    if (out.path.empty()) out.path = L"/";
    return true;
}

// One RAII handle. WinHTTP handles must be closed in the order request → connect → session, and
// a plain member-by-member destruction order gives exactly that when they are declared in the
// opposite order — but the request handle is per-call, so it gets its own scope guard.
class WinHttpHandle {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET h) : h_(h) {}
    ~WinHttpHandle() { reset(); }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    WinHttpHandle(WinHttpHandle&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    WinHttpHandle& operator=(WinHttpHandle&& o) noexcept {
        if (this != &o) {
            reset();
            h_ = o.h_;
            o.h_ = nullptr;
        }
        return *this;
    }

    void reset(HINTERNET h = nullptr) {
        if (h_ != nullptr) WinHttpCloseHandle(h_);
        h_ = h;
    }
    [[nodiscard]] HINTERNET get() const noexcept { return h_; }
    explicit operator bool() const noexcept { return h_ != nullptr; }

private:
    HINTERNET h_ = nullptr;
};

Status os_failure(const char* what) {
    const DWORD e = GetLastError();
    return Status{StatusCode::Unavailable, std::string(what) + " failed", e};
}

}  // namespace

struct Normalizer::Impl {
    std::mutex mu;
    std::condition_variable cv;
    std::thread worker;

    NormalizerConfig cfg;
    Callbacks cb;

    bool running = false;
    bool stopping = false;

    // The pending request. At most one; a newer one replaces it (see normalizer.h, Threading).
    bool have_pending = false;
    uint64_t pending_id = 0;
    std::string pending_text;

    // The id below which every answer is stale. cancel_all() raises it past everything issued so
    // far, which is why a request already inside a blocking WinHTTP call needs no abort: its
    // answer is dropped on arrival by the id check, and correctness never depends on the timing
    // of the cancellation.
    uint64_t valid_from = 1;
    uint64_t next_id = 1;

    std::atomic<uint64_t> sent{0};
    std::atomic<uint64_t> superseded{0};

    SteadyClock clock;

    void run();
    [[nodiscard]] ::kd::Result<std::string> post(const NormalizerConfig& c, const std::string& text,
                                                 uint64_t budget_ms);
};

Normalizer::Normalizer() : impl_(std::make_unique<Impl>()) {}

Normalizer::~Normalizer() { stop(); }

Status Normalizer::start(const NormalizerConfig& cfg, Callbacks cb) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->running) return Status::success();

    // A URL that will not crack is permanent: it will not start working, and every session would
    // otherwise spend the timeout discovering that again.
    ParsedUrl parsed;
    if (cfg.enabled && !crack_url(cfg.url, parsed)) {
        return Status{StatusCode::InvalidArgument,
                      "settings.normalizer.url could not be parsed: \"" + cfg.url + "\""};
    }

    impl_->cfg = cfg;
    impl_->cb = std::move(cb);
    impl_->running = true;
    impl_->stopping = false;
    impl_->worker = std::thread([this] { impl_->run(); });

    if (cfg.enabled) {
        KD_LOG_INFO("normalizer enabled: %s (timeout %u ms, max %zu bytes, %s/%s/%s)",
                    cfg.url.c_str(), cfg.timeout_ms, cfg.max_bytes, cfg.styling.c_str(),
                    cfg.structure.c_str(), cfg.context.c_str());
    }
    return Status::success();
}

void Normalizer::stop() {
    std::thread w;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        if (!impl_->running) return;
        impl_->stopping = true;
        impl_->have_pending = false;
        // Past everything issued, so an answer arriving during teardown is dropped rather than
        // delivered to a callback whose owner is being destroyed.
        impl_->valid_from = impl_->next_id;
        w = std::move(impl_->worker);
    }
    impl_->cv.notify_all();
    if (w.joinable()) w.join();

    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->running = false;
}

void Normalizer::set_config(const NormalizerConfig& cfg) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->cfg = cfg;
}

uint64_t Normalizer::request(std::string_view text) {
    std::unique_lock<std::mutex> lock(impl_->mu);
    if (!impl_->running || impl_->stopping || !impl_->cfg.enabled) return 0;
    if (text.empty()) return 0;

    if (text.size() > impl_->cfg.max_bytes) {
        // Not chunked, and said out loud rather than sent. normalizer.h explains why: the model's
        // window is ~1000 tokens and a confidently truncated rewrite is worse than no rewrite.
        KD_LOG_WARN("buffer is %zu bytes, past normalizer.max_bytes=%zu; left raw",
                    text.size(), impl_->cfg.max_bytes);
        return 0;
    }

    if (impl_->have_pending) {
        impl_->superseded.fetch_add(1, std::memory_order_relaxed);
        KD_LOG_DEBUG("superseding pending normalize #%llu",
                     static_cast<unsigned long long>(impl_->pending_id));
    }

    const uint64_t id = impl_->next_id++;
    impl_->have_pending = true;
    impl_->pending_id = id;
    impl_->pending_text.assign(text);
    lock.unlock();

    impl_->cv.notify_one();
    return id;
}

void Normalizer::cancel_all() {
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->have_pending = false;
        impl_->valid_from = impl_->next_id;
    }
    impl_->cv.notify_all();
}

uint64_t Normalizer::requests_sent() const noexcept {
    return impl_->sent.load(std::memory_order_relaxed);
}

uint64_t Normalizer::requests_superseded() const noexcept {
    return impl_->superseded.load(std::memory_order_relaxed);
}

void Normalizer::Impl::run() {
    for (;;) {
        NormalizerConfig c;
        std::string text;
        uint64_t id = 0;
        {
            std::unique_lock<std::mutex> lock(mu);
            cv.wait(lock, [this] { return stopping || have_pending; });
            if (stopping) return;

            id = pending_id;
            text = std::move(pending_text);
            pending_text.clear();
            have_pending = false;
            c = cfg;
        }

        const uint64_t began = clock.now_ms();
        sent.fetch_add(1, std::memory_order_relaxed);
        ::kd::Result<std::string> answer = post(c, text, c.timeout_ms);
        const uint64_t elapsed = clock.now_ms() - began;

        Normalizer::Result out;
        out.id = id;
        out.raw = std::move(text);
        out.elapsed_ms = elapsed;
        if (answer.is_ok()) {
            out.normalized = *answer;
        } else {
            out.status = answer.status();
        }

        std::function<void(Normalizer::Result)> sink;
        {
            std::lock_guard<std::mutex> lock(mu);
            // The id check normalizer.h promises: an answer to a request that has since been
            // cancelled or superseded is dropped here and never reaches the UI thread, so a late
            // rewrite cannot overwrite text the user has since edited.
            if (stopping || id < valid_from) {
                KD_LOG_DEBUG("dropping stale normalize #%llu (%llu ms)",
                             static_cast<unsigned long long>(id),
                             static_cast<unsigned long long>(elapsed));
                continue;
            }
            sink = cb.on_result;
        }
        if (sink) sink(std::move(out));
    }
}

::kd::Result<std::string> Normalizer::Impl::post(const NormalizerConfig& c,
                                                 const std::string& text, uint64_t budget_ms) {
    ParsedUrl url;
    if (!crack_url(c.url, url)) {
        return Status{StatusCode::InvalidArgument, "normalizer url could not be parsed"};
    }

    const uint64_t deadline = clock.now_ms() + budget_ms;
    // Each WinHTTP timeout is per-operation, so the per-operation share is a quarter of the
    // budget and the total is re-checked between operations. Without both, a server that stalls
    // at every step takes four times as long as configured — see the note at the top.
    const auto share = static_cast<int>(budget_ms / 4 + 1);
    const auto remaining = [&]() -> int64_t {
        return static_cast<int64_t>(deadline) - static_cast<int64_t>(clock.now_ms());
    };

    WinHttpHandle session(WinHttpOpen(L"kvit-dictate/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return os_failure("WinHttpOpen");
    WinHttpSetTimeouts(session.get(), share, share, share, share);

    WinHttpHandle connect(
        WinHttpConnect(session.get(), url.host.c_str(), url.port, 0));
    if (!connect) return os_failure("WinHttpConnect");
    if (remaining() <= 0) return Status{StatusCode::Timeout, "normalizer timed out connecting"};

    WinHttpHandle req(WinHttpOpenRequest(
        connect.get(), L"POST", url.path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, url.secure ? WINHTTP_FLAG_SECURE : 0u));
    if (!req) return os_failure("WinHttpOpenRequest");

    const std::string body = build_request_body(c, text);
    static constexpr wchar_t kHeaders[] = L"Content-Type: application/json\r\n";

    if (!WinHttpSendRequest(req.get(), kHeaders, static_cast<DWORD>(-1),
                            const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                            static_cast<DWORD>(body.size()), 0)) {
        return os_failure("WinHttpSendRequest");
    }
    if (!WinHttpReceiveResponse(req.get(), nullptr)) return os_failure("WinHttpReceiveResponse");
    if (remaining() <= 0) {
        return Status{StatusCode::Timeout, "normalizer timed out awaiting the response"};
    }

    DWORD status_code = 0;
    DWORD len = sizeof(status_code);
    if (!WinHttpQueryHeaders(req.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &len,
                             WINHTTP_NO_HEADER_INDEX)) {
        return os_failure("WinHttpQueryHeaders");
    }
    if (status_code != 200) {
        // 400 is the endpoint refusing an untrained control value. validate() should have caught
        // it at load, so reaching here means the server's vocabulary and this build's tables have
        // diverged — worth a distinct message, because the fix is a settings change, not a retry.
        return Status{StatusCode::Malformed,
                      "normalizer answered HTTP " + std::to_string(status_code) +
                          (status_code == 400
                               ? " (an untrained styling/structure/context value?)"
                               : "")};
    }

    std::string out;
    for (;;) {
        if (remaining() <= 0) {
            return Status{StatusCode::Timeout, "normalizer timed out reading the response"};
        }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.get(), &avail)) {
            return os_failure("WinHttpQueryDataAvailable");
        }
        // Zero available is the only reliable end-of-body signal; a short read is not one.
        if (avail == 0) break;
        if (out.size() + avail > (1u << 20)) {
            return Status{StatusCode::Malformed, "normalizer response exceeds 1 MiB"};
        }
        const size_t at = out.size();
        out.resize(at + avail);
        DWORD read = 0;
        if (!WinHttpReadData(req.get(), out.data() + at, avail, &read)) {
            return os_failure("WinHttpReadData");
        }
        out.resize(at + read);
        if (read == 0) break;
    }

    return parse_response_body(out);
}

}  // namespace kd
