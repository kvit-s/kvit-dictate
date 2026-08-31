// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// The dictation corpus: what was said, what the model heard, and what the rewriter made of it,
// kept aligned on one clock so a different model can be asked the same question later.
//
// ─── What this is for, and why it is not the transcript log ─────────────────────────────────
//
// util/log.h's transcript sink answers "what did the model say" for debugging one incident. This
// answers a different question — "would a *different* model have said something better" — and
// that needs the audio, because a transcript without the recording it came from cannot be
// re-decoded by anything.
//
// bench/ has had `metrics.py`, `run_matrix.py` and an empty `corpus/` since before this file
// existed. The evaluation half was built and never fed; this is the capture half.
//
// ─── Session-scoped, not utterance-scoped, and that is deliberate ───────────────────────────
//
// The obvious shape is one file per utterance. It is the wrong one: the Client does not learn
// where an utterance ended until the `final` arrives, and by then the audio has already been
// sent. Segmenting here would mean buffering against a boundary this side does not own, and
// getting it subtly wrong in a way that only shows up as a corpus that disagrees with the
// sessions it came from.
//
// So one continuous recording per session, plus an event log carrying every `partial` and
// `final` with the `audio_pos_ms` the protocol already puts on them (docs/protocol.md §8.1).
// Utterance boundaries are then *derived* offline, exactly, from the same numbers the session
// used — and the recording keeps the pauses, the false starts and the room, which is what makes
// it worth re-decoding at all.
//
// ─── Privacy: off unless asked, and it stays that way ───────────────────────────────────────
//
// plan.md §1.9 prohibits persisting transcripts or audio by default. This is exactly what that
// rule is about — durable, accumulating, audio included — so unlike `privacy.crash_recovery`
// (settings.h, which holds only undelivered text and deletes it on delivery) this one defaults
// **off** and is not turned on by anything but the user saying so.
//
// Three properties keep an enabled corpus honest rather than merely allowed:
//
//   * it is bounded, by bytes and by age, and prunes oldest-first on every session;
//   * it is visible — the tray says so for as long as it is recording;
//   * it is skippable per session, because in a fortnight of collection somebody will dictate a
//     password, and "delete it afterwards" is not a control anyone reaches in time.
//
// ─── Crash behaviour ────────────────────────────────────────────────────────────────────────
//
// A WAV header carries the sample count, so a recording whose process died has a header claiming
// zero bytes and plays as silence. The sizes are therefore rewritten in place every
// `flush_sizes()` — the caller does this on the tick it already runs — so a killed session
// leaves a file that is short rather than one that is unreadable. Tonight's lesson, applied
// somewhere it was cheap to apply.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "util/json.h"
#include "util/result.h"

namespace kd {

struct CorpusConfig {
    // plan.md §1.9. Nothing here happens unless this was set by hand.
    bool enabled = false;

    // Where sessions are written. Empty means `%LOCALAPPDATA%\kvit-dictate\corpus`, which is
    // correct but is on the system drive; a collection run of any length belongs somewhere with
    // room, which is why this is configurable at all.
    std::filesystem::path dir;

    // Oldest-first pruning bounds. Audio is 32 KB per second of open microphone, so a bound in
    // bytes is the one that actually binds — 8 GiB is about 70 hours.
    uint64_t max_bytes = 8ull * 1024 * 1024 * 1024;
    uint32_t max_age_days = 60;
};

// One session's recording. Not thread-safe: `append_audio` is called from the capture thread and
// everything else from the UI thread, so the caller must keep those apart — in practice the
// capture thread's chunks are already funnelled through `App::on_audio`, which is the only
// writer while a session is open.
class Corpus {
public:
    Corpus();
    ~Corpus();
    Corpus(const Corpus&) = delete;
    Corpus& operator=(const Corpus&) = delete;

    // Begin a session. Creates `<dir>/<YYYYMMDD-HHMMSS>/` with `audio.wav` and `events.jsonl`,
    // and prunes the store to `cfg`'s bounds first — before writing, so the bound is a bound
    // rather than a high-water mark. A failure is logged and leaves `active()` false; a corpus
    // that cannot be written must never stop a dictation.
    Status open(const CorpusConfig& cfg, std::string_view session_id, const json::Value& header);

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] const std::filesystem::path& session_dir() const noexcept;
    [[nodiscard]] uint64_t audio_ms() const noexcept;

    // 16 kHz mono s16, exactly what was put on the wire — post-resampler and post-gate, so a
    // replay is a fair comparison rather than an approximation of one.
    void append_audio(const int16_t* samples, size_t count);

    // One JSON object per line. `type` becomes the object's "t" and the millisecond offset into
    // this session's audio becomes its "ms", so every event indexes the recording.
    void event(std::string_view type, json::Value body);

    // Rewrite the WAV's two size fields in place. Cheap, and what makes a killed session's
    // recording playable — see the header.
    void flush_sizes();

    // Finish: a last `end` event, correct sizes, files closed. Idempotent.
    void close(std::string_view reason, const json::Value& tail);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ─── The pruning rule, pure ─────────────────────────────────────────────────────────────────
//
// Given each session directory's size and age in days, oldest-first, which ones must go for the
// store to sit inside both bounds? Returns indices into `sizes`/`ages`.
//
// Separated out and tested for the reason `layout_overlay` and `clamp_scroll_line` were: this is
// arithmetic that deletes the user's data, and an off-by-one in it is invisible in a directory
// walk and obvious in a table of cases. `max_bytes == 0` or `max_age_days == 0` disables that
// half of the rule rather than deleting everything, which is the reading that cannot destroy a
// corpus by way of a zero in a config file.
[[nodiscard]] std::vector<size_t> corpus_prune_set(const std::vector<uint64_t>& sizes,
                                                   const std::vector<uint32_t>& ages,
                                                   uint64_t max_bytes, uint32_t max_age_days);

}  // namespace kd
