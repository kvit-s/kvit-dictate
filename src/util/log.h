// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// Leveled, thread-safe logging — with transcript logging as a separate, off-by-default,
// retention-bounded channel.
//
// Implements: plan.md §1.9 (privacy and network policy).
//
// ─── The one rule that makes this module different from every other logger ──────────────────
//
// **Recognised text is not a log message.** plan.md §1.9 is explicit: "No transcripts or audio
// persisted by default. Diagnostic logging of recognised text is opt-in, retention-bounded, and
// off in release builds." A logger where `LOG_DEBUG("partial: %s", text)` is one keystroke away
// from shipping is a logger that will eventually write a user's dictated password to disk.
//
// So there are two channels, and they are not the same API:
//
//   * **Diagnostics** — KD_LOG_*. Levels, timestamps, thread ids, module tags. May record
//     *shapes*: byte counts, offsets, sequence numbers, error codes, timings. It is a
//     documented defect to pass recognised text to these macros, and the transcript-redaction
//     helper below exists so there is always an easy correct alternative.
//   * **Transcript** — KD_TRANSCRIPT. Compiled out entirely unless KD_ENABLE_TRANSCRIPT_LOG is
//     defined, *and* disabled at run time unless the user has explicitly opted in through
//     settings, *and* written to a separate file with its own retention bound. Release builds
//     do not define the macro, so the call sites vanish rather than becoming a run-time branch
//     that a mis-set setting could open.
//
// ─── What "retention-bounded" means concretely ──────────────────────────────────────────────
//
// The transcript sink keeps at most `TranscriptPolicy::max_bytes` and
// `TranscriptPolicy::max_age` of history, enforced on every write and on open (so a file left
// behind by a previous run is trimmed before anything is appended to it, not after the process
// happens to write again). The bound is a promise made to the user in the settings UI; it is
// enforced here, once, rather than by each caller.
//
// ─── Threading ──────────────────────────────────────────────────────────────────────────────
//
// Every entry point is thread-safe. The WASAPI capture thread, the IPC I/O thread, the UI
// thread and the hook thread all log.
//
// **The low-level keyboard hook is the hard constraint.** plan.md §1.8: "The callback must never
// block — a slow hook is silently unhooked by Windows and the hotkey dies with no error." So:
//
//   * The sinks are written from a dedicated logging thread. A call to KD_LOG_* formats into a
//     fixed-size record and pushes it onto a bounded lock-free-enough queue; it never touches
//     the file system and never waits on a lock held by an I/O operation.
//   * If the queue is full the record is **dropped and counted**, and the next successful write
//     carries the drop count. Blocking the hook thread to preserve a log line would trade a
//     working hotkey for a complete log, which is the wrong trade.
//   * `level_enabled()` is a relaxed atomic read, so a disabled level costs one load and the
//     arguments are never evaluated (the macros short-circuit).
//
// ─── Sinks ──────────────────────────────────────────────────────────────────────────────────
//
// Diagnostics go to OutputDebugStringW (visible in DebugView / the VS output window) and,
// optionally, to a file. Both are optional and independently configurable; the default in a
// release build is debugger-only, which persists nothing.

#include <cstdarg>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace kd::log {

enum class Level : uint8_t {
    Trace = 0,  // per-buffer, per-frame detail. Off except when chasing a specific fault.
    Debug,      // state transitions, decisions, measured timings
    Info,       // lifecycle: started, model ready, session opened/closed
    Warn,       // recovered or degraded: seq gap, low anchor confidence, device change
    Error,      // a session or connection died; the user will notice
    Off,        // nothing is emitted
};

const char* to_string(Level l) noexcept;
bool parse_level(std::string_view s, Level& out) noexcept;

struct Config {
    Level level = Level::Info;

    // Mirror diagnostics to OutputDebugStringW. Cheap, persists nothing.
    bool to_debugger = true;

    // Mirror diagnostics to stderr. Off for the GUI client, which has no console to write to;
    // ON for `kvit-asrd`, whose whole diagnostic channel is its standard error — supervised, the
    // parent redirects it into the client's log, and standalone it is the terminal the operator
    // started it in (openai-translator.md §4.8). A separate asrd.log would split the first
    // support question across two files, which is why there is not one.
    bool to_stderr = false;

    // Diagnostics file. Empty means "no file", which is the release default: plan.md §1.9's
    // "persisted by default" prohibition is about transcripts, but a diagnostics file that
    // records window titles and process names is close enough to the line that it is opt-in too.
    std::filesystem::path file;

    // Roll the diagnostics file at this size, keeping one previous generation.
    uint64_t file_max_bytes = 4u * 1024u * 1024u;

    // Number of records the background writer may hold. Beyond this, records are dropped and
    // counted — see the threading note above.
    size_t queue_capacity = 4096;
};

struct TranscriptPolicy {
    // Master switch. Defaults to false and stays false unless the user opts in, per settings.
    bool enabled = false;

    std::filesystem::path file;

    // Retention bounds, enforced on open and on every write.
    uint64_t max_bytes = 1u * 1024u * 1024u;
    uint32_t max_age_days = 7;
};

// Process-wide initialisation. Safe to call once, early, from the UI thread before any other
// thread is started. Calling it again reconfigures; records already queued are still written
// under the old sinks, which is a deliberate simplification and is noted so nobody debugs it.
void init(const Config& cfg);

// Reconfigure the level at run time (settings dialog). Thread-safe.
void set_level(Level l) noexcept;
[[nodiscard]] Level level() noexcept;
[[nodiscard]] bool level_enabled(Level l) noexcept;

// Flush and stop the writer thread. Must be called before the process exits, or the tail of the
// queue is lost — which matters exactly when it hurts, since the interesting records are the
// last ones before a crash-adjacent shutdown.
void shutdown();

// Number of diagnostic records dropped because the queue was full, since init(). Surfaced in
// the settings dialog's diagnostics panel: a nonzero value means the log is not the whole story.
[[nodiscard]] uint64_t dropped_records() noexcept;

// The formatting entry point behind the macros. `module` is a short static tag ("ipc", "audio",
// "commit"); `fmt` is printf-style. Never called directly.
void write(Level l, const char* module, const char* file, int line, const char* fmt, ...);
void write_v(Level l, const char* module, const char* file, int line, const char* fmt, va_list ap);

// ─── Transcript channel ─────────────────────────────────────────────────────────────────────

// Apply the policy. Disabling it closes the file; it does not delete it (deleting a file the
// user asked for would be its own surprise), but the retention bound still applies on the next
// open.
void set_transcript_policy(const TranscriptPolicy& p);
[[nodiscard]] bool transcript_enabled() noexcept;

// Write one transcript record. A no-op unless the policy is enabled. `kind` is "partial",
// "final", "committed", or "violation"; `text` is the recognised text.
void write_transcript(std::string_view session_id, uint64_t utterance_id, uint64_t seq,
                      std::string_view kind, std::string_view text);

// The safe way to mention recognised text in a *diagnostic* line: returns a description that
// carries the shape and none of the content — e.g. `"<62 bytes, 41 clusters, stable=31>"`.
// Use this instead of the text itself in every KD_LOG_* call.
std::string redact(std::string_view text, uint64_t stable_bytes = 0);

}  // namespace kd::log

// ─── Macros ─────────────────────────────────────────────────────────────────────────────────
//
// Each translation unit defines KD_LOG_MODULE before including this header:
//     #define KD_LOG_MODULE "audio"
// Falling back to "?" rather than failing the build keeps a quick experiment cheap while making
// an unnamed module obvious in the output.

#ifndef KD_LOG_MODULE
#define KD_LOG_MODULE "?"
#endif

#define KD_LOG_AT(lvl, ...)                                                     \
    do {                                                                        \
        if (::kd::log::level_enabled(lvl)) {                                    \
            ::kd::log::write((lvl), KD_LOG_MODULE, __FILE__, __LINE__, __VA_ARGS__); \
        }                                                                       \
    } while (0)

#define KD_LOG_TRACE(...) KD_LOG_AT(::kd::log::Level::Trace, __VA_ARGS__)
#define KD_LOG_DEBUG(...) KD_LOG_AT(::kd::log::Level::Debug, __VA_ARGS__)
#define KD_LOG_INFO(...) KD_LOG_AT(::kd::log::Level::Info, __VA_ARGS__)
#define KD_LOG_WARN(...) KD_LOG_AT(::kd::log::Level::Warn, __VA_ARGS__)
#define KD_LOG_ERROR(...) KD_LOG_AT(::kd::log::Level::Error, __VA_ARGS__)

// Transcript logging is compiled out unless explicitly enabled at build time. CMake defines
// KD_ENABLE_TRANSCRIPT_LOG only for Debug configurations; a release binary physically cannot
// write a transcript, which is a stronger guarantee than a run-time flag and is what plan.md
// §1.9's "off in release builds" asks for.
#ifdef KD_ENABLE_TRANSCRIPT_LOG
#define KD_TRANSCRIPT(session, utt, seq, kind, text) \
    ::kd::log::write_transcript((session), (utt), (seq), (kind), (text))
#else
#define KD_TRANSCRIPT(session, utt, seq, kind, text) \
    do {                                             \
        (void)sizeof((session));                     \
        (void)sizeof((utt));                         \
        (void)sizeof((seq));                         \
        (void)sizeof((kind));                        \
        (void)sizeof((text));                        \
    } while (0)
#endif
