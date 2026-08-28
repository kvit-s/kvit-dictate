// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Leveled, thread-safe logging, plus a separately-gated transcript channel.
//
// Specification: util/log.h. Implements plan.md §1.9 (privacy) under the constraint from
// plan.md §1.8 (a slow WH_KEYBOARD_LL callback is silently unhooked by Windows and the hotkey
// dies with no error).
//
// ─── The shape, and why ─────────────────────────────────────────────────────────────────────
//
// A KD_LOG_* call on any thread does exactly this: read one relaxed atomic (the level), format
// into a stack buffer, take an uncontended mutex, memcpy into a pre-allocated ring slot, notify.
// It never opens a file, never grows a container, never waits on a lock that an I/O operation
// could be holding, and never blocks. If the ring is full the record is **dropped and counted** —
// blocking the hook thread to preserve a log line would trade a working hotkey for a complete
// log, which is the wrong trade and is the one plan.md §1.8 forbids.
//
// The ring is a `std::vector<Record>` of fixed-size records sized once at init(). That costs
// `queue_capacity * sizeof(Record)` bytes (≈2 MiB at the defaults) and buys the property that
// no logging call on any thread allocates. Variable-length records would be smaller and would
// allocate on the hook thread; that is the trade being made, deliberately.
//
// ─── Two channels, structurally separate ────────────────────────────────────────────────────
//
// Recognised text never travels through this queue. `write_transcript()` is a different function
// with a different sink, a different file, a different mutex and its own enable flag, and in a
// build without KD_ENABLE_TRANSCRIPT_LOG it refuses to open a file at all — so a release binary
// cannot write a transcript even if a settings file asks it to. That is stronger than the macro
// being compiled out, and both guards are present on purpose: the macro stops the call sites,
// this stops a direct call.

#define KD_LOG_MODULE "log"
#include "util/log.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "util/json.h"
#include "util/utf8.h"

// Only in the .cpp: no header under src/ includes windows.h, which is what keeps the pure
// modules unit-testable.
#include <windows.h>

namespace kd::log {
namespace {

// ═══ Fixed-size records ═════════════════════════════════════════════════════════════════════

// A formatted line longer than this is truncated. 480 bytes is far past any diagnostic line this
// project emits (which carry shapes and numbers, never text — see redact()), and keeping the
// record a round 512 bytes makes the ring's memory cost obvious at a glance.
constexpr size_t kRecordTextMax = 480;

struct Record {
    uint32_t len = 0;
    char text[kRecordTextMax] = {};
};

// How many records the writer moves out of the ring per lock acquisition. Bounded so the mutex
// is never held for a long copy, and so the writer's own scratch buffer is a fixed size.
constexpr size_t kBatchMax = 128;

constexpr size_t kMinQueueCapacity = 64;

// ═══ Process state ══════════════════════════════════════════════════════════════════════════

struct State {
    // Guards the ring and the pending configuration. Never held across an I/O call.
    std::mutex mu;
    std::condition_variable cv;

    std::vector<Record> ring;
    size_t head = 0;
    size_t count = 0;

    bool running = false;  // a writer thread exists and is draining
    bool stopping = false;
    std::thread writer;

    Config pending;
    bool has_pending = false;
    // True once a configuration has actually reached the sinks. Distinguishes the first init()
    // — which must take effect before a single line is emitted — from a reconfiguration, which
    // must not disturb records already queued. See writer_main().
    bool configured = false;

    std::atomic<uint64_t> dropped{0};

    // Writer-thread-only state. No mutex: nothing else touches these.
    Config active;
    std::ofstream file;
    uint64_t file_bytes = 0;
    uint64_t reported_dropped = 0;
};

// Deliberately leaked.
//
// A function-local `static State` would be destroyed by the CRT at exit, and its destructor would
// join the writer thread. If the process leaves through ExitProcess (which Windows does for you
// when the last message loop ends badly, and which any crash handler may do), every other thread
// is already terminated by the time static destructors run — so the join would hang the process
// during shutdown, in a way that looks like a freeze and reproduces on nobody's machine. The
// state is therefore never destroyed; shutdown() does the join explicitly, and init() registers
// it with atexit() for the ordinary path where all threads are still alive.
State& state() {
    static State* s = new State();
    return *s;
}

std::atomic<uint8_t> g_level{static_cast<uint8_t>(Level::Info)};

// ═══ Formatting ═════════════════════════════════════════════════════════════════════════════

const char* level_field(Level l) noexcept {
    switch (l) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO ";
        case Level::Warn: return "WARN ";
        case Level::Error: return "ERROR";
        case Level::Off: return "OFF  ";
    }
    return "?????";
}

// __FILE__ under MSVC is the path the compiler was handed, which for a CMake build is absolute.
// Logging it whole would put 60 characters of build-machine path on every line.
const char* basename(const char* path) noexcept {
    if (path == nullptr) return "";
    const char* last = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '\\' || *p == '/') last = p + 1;
    }
    return last;
}

// Back off `len` to a UTF-8 sequence boundary so a truncated line is still well-formed UTF-8.
// Not a substitution — nothing is replaced, the tail is simply not there, and the caller appends
// a visible marker so the truncation is never mistaken for the end of the message.
size_t utf8_safe_len(const char* buf, size_t len) noexcept {
    while (len > 0) {
        const unsigned char c = static_cast<unsigned char>(buf[len - 1]);
        if ((c & 0x80u) == 0u) return len;              // ASCII: a boundary follows it
        if ((c & 0xC0u) == 0xC0u) return len - 1;       // a lead byte: drop the partial sequence
        --len;                                          // a continuation byte: keep walking back
    }
    return 0;
}

// Formats the complete line, timestamp included, on the *calling* thread. Doing it here rather
// than on the writer means the timestamp is the moment of the event and the writer does no work
// beyond I/O — which matters because the writer is what falls behind under load.
size_t format_record(char* buf, size_t cap, Level l, const char* module, const char* file,
                     int line, const char* fmt, va_list ap) noexcept {
    SYSTEMTIME st;
    GetLocalTime(&st);  // local, not UTC: this log is read next to Event Viewer and a wall clock

    // SYSTEMTIME fields are WORD; they promote to int through the ellipsis, and MSVC's C4477
    // format check compares against the declared conversion. The casts are for that check, not
    // for correctness.
    const int n = std::snprintf(
        buf, cap, "%04u-%02u-%02u %02u:%02u:%02u.%03u %s t%-5lu [%-8s] %s:%d ",
        static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wMonth),
        static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
        static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond),
        static_cast<unsigned>(st.wMilliseconds), level_field(l),
        static_cast<unsigned long>(GetCurrentThreadId()), module != nullptr ? module : "?",
        basename(file), line);
    if (n < 0) return 0;
    size_t used = static_cast<size_t>(n);
    if (used >= cap) return utf8_safe_len(buf, cap - 1);

    const int m = std::vsnprintf(buf + used, cap - used, fmt != nullptr ? fmt : "", ap);
    if (m < 0) return used;
    if (static_cast<size_t>(m) >= cap - used) {
        // Truncated. Reserve room for the marker so a reader is never left guessing whether the
        // message really ended there.
        static const char kMarker[] = "…[truncated]";
        const size_t marker_len = sizeof(kMarker) - 1;
        size_t keep = utf8_safe_len(buf, cap - 1 - marker_len);
        std::memcpy(buf + keep, kMarker, marker_len);
        return keep + marker_len;
    }
    return used + static_cast<size_t>(m);
}

// ═══ Sinks (writer thread only) ═════════════════════════════════════════════════════════════

void emit_to_debugger(std::string_view line) {
    std::wstring wide;
    if (!utf8::to_utf16(line, wide)) {
        // A caller passed bytes that are not UTF-8 — most often a Win32 string in the ANSI code
        // page. Replacing the offending bytes with '?' here does NOT violate utf8.h's
        // no-substitution rule: that rule protects text on its way to the keystroke injector and
        // the user's document. This text goes to a debugger window and nowhere else, and the
        // alternative is losing the diagnostic entirely, which is worse.
        std::string ascii;
        ascii.reserve(line.size());
        for (const char c : line) {
            ascii.push_back(static_cast<unsigned char>(c) < 0x80 ? c : '?');
        }
        ascii += "  [non-UTF-8 bytes replaced]";
        if (!utf8::to_utf16(ascii, wide)) return;
    }
    wide += L"\r\n";
    ::OutputDebugStringW(wide.c_str());
}

void roll_file_if_needed(State& s, size_t incoming) {
    if (!s.file.is_open() || s.active.file_max_bytes == 0) return;
    if (s.file_bytes + incoming <= s.active.file_max_bytes) return;

    s.file.close();

    std::filesystem::path previous = s.active.file;
    previous += ".1";  // exactly one previous generation, as the header states

    std::error_code ec;
    std::filesystem::remove(previous, ec);
    std::filesystem::rename(s.active.file, previous, ec);
    // A failed rename is not worth escalating: the worst case is that the current file keeps
    // growing past its bound until the next roll succeeds. Reporting it would require logging,
    // and we are inside the logger.

    s.file.open(s.active.file, std::ios::binary | std::ios::trunc);
    s.file_bytes = 0;
}

void emit(State& s, std::string_view line) {
    if (s.active.to_debugger) emit_to_debugger(line);

    if (s.active.to_stderr) {
        // One fwrite for the line and one for the terminator would let two threads interleave,
        // but only the writer thread reaches here, so the pair is atomic enough. stderr is
        // unbuffered by default on Windows, which is what a supervised child wants: the parent
        // reads the pipe as the lines are produced rather than in 4 KB gulps at exit.
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fputc('\n', stderr);
    }

    if (s.file.is_open()) {
        roll_file_if_needed(s, line.size() + 2);
        s.file.write(line.data(), static_cast<std::streamsize>(line.size()));
        s.file.write("\r\n", 2);
        s.file_bytes += line.size() + 2;
    }
}

void apply_config(State& s, const Config& cfg) {
    const bool file_changed = (s.active.file != cfg.file);
    s.active = cfg;

    if (file_changed || cfg.file.empty()) {
        if (s.file.is_open()) s.file.close();
        s.file_bytes = 0;
    }
    if (!cfg.file.empty() && !s.file.is_open()) {
        std::error_code ec;
        const auto parent = cfg.file.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);

        s.file.open(cfg.file, std::ios::binary | std::ios::app);
        const auto existing = std::filesystem::file_size(cfg.file, ec);
        s.file_bytes = ec ? 0u : static_cast<uint64_t>(existing);
    }
}

void writer_main() {
    State& s = state();

    // One scratch batch for the process lifetime. The writer must not allocate per drain: under
    // the overload conditions where logging matters most, per-record allocation is what turns a
    // slow log into a stalled one.
    std::vector<Record> batch(kBatchMax);
    std::string line;

    for (;;) {
        size_t n = 0;
        bool finished = false;
        Config to_apply;
        bool apply = false;
        bool apply_first = false;

        {
            std::unique_lock<std::mutex> lk(s.mu);
            s.cv.wait_for(lk, std::chrono::milliseconds(200), [&s] {
                return s.stopping || s.count > 0 || s.has_pending;
            });

            // The *first* configuration is applied before anything is emitted. Deferring it
            // like a reconfiguration would put the process's opening lines on the default sinks
            // — which for kvit-asrd, whose only sink is stderr, meant its "starting" line going
            // nowhere while every later line arrived. The promise below is about a *re*-init and
            // is not weakened by this.
            if (s.has_pending && !s.configured) {
                to_apply = s.pending;
                apply_first = true;
                s.has_pending = false;
                s.configured = true;
            }

            n = (std::min)(s.count, kBatchMax);
            const size_t cap = s.ring.size();
            for (size_t i = 0; i < n; ++i) {
                const Record& src = s.ring[(s.head + i) % cap];
                batch[i].len = src.len;
                std::memcpy(batch[i].text, src.text, src.len);
            }
            if (cap != 0) s.head = (s.head + n) % cap;
            s.count -= n;

            // The header promises that records already queued when init() is called again are
            // still written under the *old* sinks. That promise is kept here, by applying a
            // pending configuration only once the ring is empty.
            if (s.has_pending && s.count == 0) {
                to_apply = s.pending;
                apply = true;
                s.has_pending = false;
            }
            finished = s.stopping && s.count == 0;
        }

        // Outside the lock, like every other sink touch: apply_config() can open a file.
        if (apply_first) apply_config(s, to_apply);

        for (size_t i = 0; i < n; ++i) {
            emit(s, std::string_view(batch[i].text, batch[i].len));
        }

        // "The next successful write carries the drop count" (log.h). Emitted after the batch so
        // it names records that were already lost rather than predicting a loss.
        const uint64_t dropped_now = s.dropped.load(std::memory_order_relaxed);
        if (dropped_now > s.reported_dropped && n > 0) {
            line = "*** " + std::to_string(dropped_now - s.reported_dropped) +
                   " diagnostic record(s) dropped: the log queue was full (total " +
                   std::to_string(dropped_now) + ")";
            s.reported_dropped = dropped_now;
            emit(s, line);
        }

        if (n > 0 && s.file.is_open()) s.file.flush();

        if (apply) apply_config(s, to_apply);
        if (finished) break;
    }

    if (s.file.is_open()) {
        s.file.flush();
        s.file.close();
    }
}

// ═══ Enqueue ════════════════════════════════════════════════════════════════════════════════

// Returns false when there is no writer thread to drain the ring, in which case the caller
// writes synchronously. Pre-init logging is rare, low-volume, and never on the hook thread
// (the hooks are installed long after init()), so a synchronous debugger write there is safe.
bool enqueue(const char* text, size_t len) {
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mu);
    if (!s.running || s.ring.empty()) return false;

    if (s.count == s.ring.size()) {
        s.dropped.fetch_add(1, std::memory_order_relaxed);
        return true;  // handled: dropped and counted, per plan.md §1.8
    }
    Record& slot = s.ring[(s.head + s.count) % s.ring.size()];
    slot.len = static_cast<uint32_t>(len);
    std::memcpy(slot.text, text, len);
    ++s.count;
    s.cv.notify_one();
    return true;
}

// ═══ Transcript sink ════════════════════════════════════════════════════════════════════════

// Retention bounds are clamped: `max_bytes` also bounds the in-memory retention window below, so
// a settings file asking for 4 GiB must not be honoured literally.
constexpr uint64_t kTranscriptMinBytes = 4u * 1024u;
constexpr uint64_t kTranscriptMaxBytes = 16u * 1024u * 1024u;

// Trimming rewrites the file, so it is done with hysteresis rather than on every write: once the
// bound is passed, retention is cut back to (1 - 1/8) of it. Otherwise every record past the
// bound would rewrite the whole file.
constexpr uint64_t kTrimHysteresisDivisor = 8;

struct TranscriptState {
    std::mutex mu;
    TranscriptPolicy policy;
    bool open = false;

    // The retained window, mirrored in memory so a trim is a rewrite from RAM rather than a
    // read-modify-write of the file. Bounded by the clamped max_bytes.
    std::deque<std::pair<int64_t, std::string>> lines;  // (epoch seconds, line without newline)
    uint64_t bytes = 0;
};

TranscriptState& transcript_state() {
    static TranscriptState* t = new TranscriptState();  // leaked for the same reason as State
    return *t;
}

std::atomic<bool> g_transcript_enabled{false};

int64_t now_epoch_seconds() noexcept {
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::system_clock::now().time_since_epoch())
                                    .count());
}

uint64_t clamped_max_bytes(const TranscriptPolicy& p) noexcept {
    return (std::min)((std::max)(p.max_bytes, kTranscriptMinBytes), kTranscriptMaxBytes);
}

// Every record starts `{"ts":<seconds>` — see write_transcript(). Recovering the timestamp is
// therefore a prefix scan, not a JSON parse, which keeps trimming cheap and keeps a corrupt
// line from costing anything more than being dropped.
bool line_timestamp(std::string_view line, int64_t& out) noexcept {
    constexpr std::string_view kPrefix = "{\"ts\":";
    if (line.size() < kPrefix.size() + 1 || line.compare(0, kPrefix.size(), kPrefix) != 0) {
        return false;
    }
    const char* first = line.data() + kPrefix.size();
    const char* last = line.data() + line.size();
    const auto r = std::from_chars(first, last, out);
    return r.ec == std::errc{} && r.ptr != first;
}

void rewrite_transcript_file(TranscriptState& t) {
    std::ofstream f(t.policy.file, std::ios::binary | std::ios::trunc);
    if (!f) return;
    for (const auto& entry : t.lines) {
        f.write(entry.second.data(), static_cast<std::streamsize>(entry.second.size()));
        f.write("\r\n", 2);
    }
}

// Enforces both bounds on the in-memory window and reports whether the file needs rewriting.
bool trim_window(TranscriptState& t, bool force_rewrite_threshold) {
    const uint64_t limit = clamped_max_bytes(t.policy);
    const int64_t cutoff =
        now_epoch_seconds() - static_cast<int64_t>(t.policy.max_age_days) * 86400;

    const uint64_t target =
        force_rewrite_threshold ? limit - limit / kTrimHysteresisDivisor : limit;

    uint64_t removed = 0;
    while (!t.lines.empty() &&
           (t.bytes > target || t.lines.front().first < cutoff)) {
        const uint64_t sz = static_cast<uint64_t>(t.lines.front().second.size()) + 2u;
        t.bytes -= (std::min)(t.bytes, sz);
        removed += sz;
        t.lines.pop_front();
    }
    return removed > 0;
}

// Load the existing file into the retention window, dropping whatever the bounds exclude. Doing
// this on *open* rather than on the next write is what the header means by "a file left behind
// by a previous run is trimmed before anything is appended to it": a machine that was off for a
// month must not have last month's transcript sitting on disk after the app starts.
void load_and_trim(TranscriptState& t) {
    t.lines.clear();
    t.bytes = 0;

    std::error_code ec;
    if (!std::filesystem::exists(t.policy.file, ec)) return;

    const uint64_t limit = clamped_max_bytes(t.policy);
    const auto size = std::filesystem::file_size(t.policy.file, ec);
    if (ec) return;

    std::ifstream f(t.policy.file, std::ios::binary);
    if (!f) return;

    bool partial_first_line = false;
    if (static_cast<uint64_t>(size) > limit) {
        f.seekg(static_cast<std::streamoff>(static_cast<uint64_t>(size) - limit), std::ios::beg);
        partial_first_line = true;
    }

    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (partial_first_line) {
            partial_first_line = false;  // the seek landed mid-record; that record is unusable
            continue;
        }
        int64_t ts = 0;
        if (!line_timestamp(line, ts)) continue;  // not one of ours, or corrupt: do not keep it
        t.bytes += static_cast<uint64_t>(line.size()) + 2u;
        t.lines.emplace_back(ts, std::move(line));
        line.clear();
    }

    if (trim_window(t, false)) rewrite_transcript_file(t);
}

}  // namespace

// ═══ Public API ═════════════════════════════════════════════════════════════════════════════

const char* to_string(Level l) noexcept {
    switch (l) {
        case Level::Trace: return "trace";
        case Level::Debug: return "debug";
        case Level::Info: return "info";
        case Level::Warn: return "warn";
        case Level::Error: return "error";
        case Level::Off: return "off";
    }
    return "unknown";
}

bool parse_level(std::string_view s, Level& out) noexcept {
    const auto eq = [s](std::string_view lit) noexcept {
        if (s.size() != lit.size()) return false;
        for (size_t i = 0; i < s.size(); ++i) {
            char a = s[i];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (a != lit[i]) return false;
        }
        return true;
    };
    if (eq("trace")) { out = Level::Trace; return true; }
    if (eq("debug")) { out = Level::Debug; return true; }
    if (eq("info")) { out = Level::Info; return true; }
    if (eq("warn") || eq("warning")) { out = Level::Warn; return true; }
    if (eq("error")) { out = Level::Error; return true; }
    if (eq("off") || eq("none")) { out = Level::Off; return true; }
    return false;
}

void set_level(Level l) noexcept {
    g_level.store(static_cast<uint8_t>(l), std::memory_order_relaxed);
}

Level level() noexcept { return static_cast<Level>(g_level.load(std::memory_order_relaxed)); }

bool level_enabled(Level l) noexcept {
    // Off is the highest enumerator, so a naive `l >= current` would report Level::Off as
    // enabled when the level is Off. That would be harmless today and a live bug the first time
    // someone writes KD_LOG_AT(Level::Off, ...).
    if (l == Level::Off) return false;
    return static_cast<uint8_t>(l) >= g_level.load(std::memory_order_relaxed);
}

uint64_t dropped_records() noexcept {
    return state().dropped.load(std::memory_order_relaxed);
}

void init(const Config& cfg) {
    State& s = state();
    set_level(cfg.level);

    bool start_thread = false;
    {
        std::lock_guard<std::mutex> lock(s.mu);
        // The ring is only ever (re)sized while it is empty. Resizing a ring that holds records
        // would either discard them or require rotating them into place, and a reconfiguration
        // is not a reason to lose the lines that led up to it — so a re-init while records are
        // queued keeps the existing capacity. Stated because it is a real, if minor, surprise.
        const size_t capacity = (std::max)(cfg.queue_capacity, kMinQueueCapacity);
        if (s.count == 0 && s.ring.size() != capacity) {
            s.ring.assign(capacity, Record{});
            s.head = 0;
        }
        s.pending = cfg;
        s.has_pending = true;
        if (!s.running) {
            s.stopping = false;
            s.running = true;
            start_thread = true;
        }
    }

    if (start_thread) {
        s.writer = std::thread(&writer_main);

        // Registered once, on the ordinary exit path only. atexit handlers run from exit() /
        // return-from-main, where every thread is still alive and a join is safe; ExitProcess
        // does not run them, which is exactly the case where a join would hang.
        static std::once_flag once;
        std::call_once(once, [] { std::atexit(&shutdown); });
    }
    s.cv.notify_one();
}

void shutdown() {
    State& s = state();
    std::thread writer;
    {
        std::lock_guard<std::mutex> lock(s.mu);
        if (!s.running) return;
        s.running = false;  // late writes fall back to the synchronous debugger path
        s.stopping = true;
        writer = std::move(s.writer);
    }
    s.cv.notify_all();
    if (writer.joinable()) writer.join();
}

void write(Level l, const char* module, const char* file, int line, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    write_v(l, module, file, line, fmt, ap);
    va_end(ap);
}

void write_v(Level l, const char* module, const char* file, int line, const char* fmt,
             va_list ap) {
    if (!level_enabled(l)) return;

    // On the stack, so this path allocates nothing on any thread — including the low-level
    // keyboard hook, where an allocation under a contended CRT heap lock is the exact stall
    // plan.md §1.8 warns about.
    char buf[kRecordTextMax];
    const size_t len = format_record(buf, sizeof(buf), l, module, file, line, fmt, ap);
    if (len == 0) return;

    if (enqueue(buf, len)) return;

    // No writer thread yet (pre-init) or after shutdown(). Synchronous, debugger only: never
    // touch a file from here, because "before init()" is also "before the configured paths are
    // known".
    emit_to_debugger(std::string_view(buf, len));
}

// ═══ Transcript channel ═════════════════════════════════════════════════════════════════════

void set_transcript_policy(const TranscriptPolicy& p) {
    TranscriptState& t = transcript_state();
    std::lock_guard<std::mutex> lock(t.mu);

    t.policy = p;

#ifndef KD_ENABLE_TRANSCRIPT_LOG
    // plan.md §1.9: "off in release builds". The macro in log.h removes the call sites; this
    // removes the sink, so a settings file carrying `transcript.enabled = true` from a debug
    // session cannot resurrect it in a release build.
    t.policy.enabled = false;
#endif

    if (!t.policy.enabled || t.policy.file.empty()) {
        // Disabling closes the channel; it does not delete the file. Deleting a file the user
        // asked us to create would be its own surprise (log.h says so explicitly).
        t.open = false;
        t.lines.clear();
        t.bytes = 0;
        g_transcript_enabled.store(false, std::memory_order_relaxed);
        return;
    }

    std::error_code ec;
    const auto parent = t.policy.file.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    load_and_trim(t);
    t.open = true;
    g_transcript_enabled.store(true, std::memory_order_relaxed);
}

bool transcript_enabled() noexcept {
#ifndef KD_ENABLE_TRANSCRIPT_LOG
    return false;
#else
    return g_transcript_enabled.load(std::memory_order_relaxed);
#endif
}

void write_transcript(std::string_view session_id, uint64_t utterance_id, uint64_t seq,
                      std::string_view kind, std::string_view text) {
#ifndef KD_ENABLE_TRANSCRIPT_LOG
    // Hard-disabled, not merely unconfigured. Parameters are consumed so /W4 stays quiet without
    // unnamed parameters, which would make the release signature differ from the debug one.
    (void)session_id;
    (void)utterance_id;
    (void)seq;
    (void)kind;
    (void)text;
    return;
#else
    if (!g_transcript_enabled.load(std::memory_order_relaxed)) return;

    TranscriptState& t = transcript_state();
    std::lock_guard<std::mutex> lock(t.mu);
    if (!t.open) return;

    // JSONL, built through the in-repo serializer so that a transcript containing quotes,
    // newlines or control characters cannot break the line structure the trimmer depends on.
    // "ts" is emitted first because the trimmer recovers it with a prefix scan.
    const int64_t ts = now_epoch_seconds();
    json::Value rec = json::Value::object();
    rec.set("ts", json::Value::number(ts));
    rec.set("session_id", json::Value::string(std::string(session_id)));
    rec.set("utterance_id", json::Value::number(static_cast<int64_t>(utterance_id)));
    rec.set("seq", json::Value::number(static_cast<int64_t>(seq)));
    rec.set("kind", json::Value::string(std::string(kind)));
    rec.set("text", json::Value::string(std::string(text)));
    std::string line = json::serialize(rec);

    // Synchronous, unlike the diagnostic channel. This path is only ever reached from the IPC
    // message thread in a debug build with the option explicitly enabled — never from the hook
    // or the WASAPI thread — and a transcript with holes in it is useless for the ICER analysis
    // (plan.md §1.4) it exists to support.
    {
        std::ofstream f(t.policy.file, std::ios::binary | std::ios::app);
        if (f) {
            f.write(line.data(), static_cast<std::streamsize>(line.size()));
            f.write("\r\n", 2);
        }
    }

    t.bytes += static_cast<uint64_t>(line.size()) + 2u;
    t.lines.emplace_back(ts, std::move(line));

    const uint64_t limit = clamped_max_bytes(t.policy);
    const int64_t cutoff =
        now_epoch_seconds() - static_cast<int64_t>(t.policy.max_age_days) * 86400;
    if (t.bytes > limit || (!t.lines.empty() && t.lines.front().first < cutoff)) {
        if (trim_window(t, true)) rewrite_transcript_file(t);
    }
#endif
}

std::string redact(std::string_view text, uint64_t stable_bytes) {
    const size_t clusters = utf8::count_graphemes(text);
    std::string out = "<";
    out += std::to_string(text.size());
    out += " bytes, ";
    if (clusters == utf8::kNpos) {
        out += "not well-formed UTF-8";
    } else {
        out += std::to_string(clusters);
        out += " clusters";
    }
    out += ", stable=";
    out += std::to_string(stable_bytes);
    out += ">";
    return out;
}

}  // namespace kd::log
