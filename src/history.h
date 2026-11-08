// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// The dictation history: every submission, read back out of the corpus as one page.
//
// ─── Why it is built from the corpus and not kept separately ────────────────────────────────
//
// corpus.h already writes, per session, the target process, a wall clock and — in the `end`
// event's `delivered` — exactly the text that was submitted. A second store holding the same
// text would be a second thing to bound, a second thing to switch off, and a second answer to
// "what did I say" that could disagree with the first. So the history is a *view*: it exists
// only where the corpus does, and it is gone the moment a session is.
//
// ─── Why a page and not a window ────────────────────────────────────────────────────────────
//
// What the user asked for is to look through what was submitted. That is reading, searching
// and copying a few hundred paragraphs, plus playing back the one that came out wrong — which
// a browser already does better than anything worth writing in Win32. The page is regenerated
// each time it is asked for, so it is a snapshot; it has no server and reaches nothing but the
// files beside it.
//
// The page is written to the corpus root, next to the session folders, so each entry plays its
// recording by a relative path and the folder stays self-describing if it is copied elsewhere.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "util/result.h"

namespace kd {

struct HistoryEntry {
    std::string session;  // the session's folder name under the corpus root
    std::string started;  // local wall clock of the start event, "2026-11-08T00:55:39"
    std::string target;   // the process the dictation went to, e.g. "Code.exe"; may be empty
    std::string text;     // what was delivered
    std::string reason;   // why the session ended; "unfinished" when it never said
    uint64_t audio_ms = 0;
    // False when the session has no `end` event — still recording, or the process died. `text`
    // is then the recogniser's finals so far, which is the closest thing to what was meant.
    bool finished = false;
};

// One session's `events.jsonl`, as text. Pure, and forgiving: a line that does not parse is
// skipped rather than costing the entry, because a session killed mid-write leaves a torn last
// line and the lines before it are still true.
[[nodiscard]] HistoryEntry parse_history_entry(std::string session, std::string_view events);

// Every session under `root`, newest first. Folders without an `events.jsonl` (`revisions/`,
// anything a person put there) are not sessions and are skipped.
[[nodiscard]] std::vector<HistoryEntry> read_history(const std::filesystem::path& root);

// The page. Pure: `generated` is the local time to print as the snapshot's age. Sessions with
// no text are counted, not listed — a hotkey pressed and released is not a dictation.
[[nodiscard]] std::string render_history_html(const std::vector<HistoryEntry>& entries,
                                              std::string_view generated);

// Read, render, and write `root/history.html` (through a temporary file, so a page open in the
// browser is never half-replaced). Returns the page's path.
[[nodiscard]] Result<std::filesystem::path> write_history_page(const std::filesystem::path& root);

}  // namespace kd
