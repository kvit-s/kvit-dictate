// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// EditBuffer — the compose buffer's text, caret and selection.
//
// Specification: edit_buffer.h. Implements docs/editing.md §3, §3.4 and the half of §11 that
// says the interesting logic belongs somewhere with tests.
//
// Nothing in this file includes a Windows header, reads a clock, or allocates outside the one
// string and the undo stack it owns. That is not incidental tidiness: it is what lets
// tests/test_edit_buffer.cpp drive sequences — a selection replaced by a combining mark, a
// Backspace across a ZWJ emoji, an Up arrow off the top of the buffer — that are awkward to
// produce with a real keyboard and impossible to assert about through a UI.

#include "edit_buffer.h"

#include <algorithm>
#include <utility>

#include "util/utf8.h"

namespace kd {

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  EditOp
// ═══════════════════════════════════════════════════════════════════════════════════════════

const char* to_string(EditOp op) noexcept {
    switch (op) {
        case EditOp::None: return "none";
        case EditOp::InsertText: return "insert_text";
        case EditOp::Backspace: return "backspace";
        case EditOp::Delete: return "delete";
        case EditOp::DeleteWordBack: return "delete_word_back";
        case EditOp::DeleteWordForward: return "delete_word_forward";
        case EditOp::MoveLeft: return "move_left";
        case EditOp::MoveRight: return "move_right";
        case EditOp::MoveUp: return "move_up";
        case EditOp::MoveDown: return "move_down";
        case EditOp::MoveWordLeft: return "move_word_left";
        case EditOp::MoveWordRight: return "move_word_right";
        case EditOp::MoveLineStart: return "move_line_start";
        case EditOp::MoveLineEnd: return "move_line_end";
        case EditOp::MoveDocStart: return "move_doc_start";
        case EditOp::MoveDocEnd: return "move_doc_end";
        case EditOp::SelectAll: return "select_all";
        case EditOp::Copy: return "copy";
        case EditOp::Cut: return "cut";
        case EditOp::Undo: return "undo";
        case EditOp::PlaceCaret: return "place_caret";
        case EditOp::ExtendCaret: return "extend_caret";
    }
    return "unknown";
}

namespace {

// The forward half of util/utf8.h's word-boundary pair. `retract_to_word_boundary` exists there
// because plan.md §1.4 needs it; nothing needed the other direction until Ctrl+Right did, and it
// is defined here rather than added to utf8.h because it is an *editing* convention (skip the
// rest of this word, then the whitespace after it) rather than a property of the text.
//
// The two are deliberately not exact mirrors, and the asymmetry is the familiar one: going back
// stops before the whitespace that precedes a word, going forward stops after the whitespace
// that follows one. That is what every editor does, and matching them would make one of the two
// directions feel wrong.
size_t next_word_boundary(std::string_view s, size_t off) noexcept {
    const size_t n = s.size();
    if (off >= n) return n;

    char32_t cp = 0;
    size_t len = 0;

    // Already sitting in whitespace: eat it, and stop at the start of the next word.
    if (utf8::decode(s, off, cp, len) && utf8::is_whitespace(cp)) {
        while (off < n && utf8::decode(s, off, cp, len) && utf8::is_whitespace(cp)) off += len;
        return off;
    }

    // Otherwise: the rest of this word, then the whitespace run that follows it.
    while (off < n && utf8::decode(s, off, cp, len) && !utf8::is_whitespace(cp)) off += len;
    while (off < n && utf8::decode(s, off, cp, len) && utf8::is_whitespace(cp)) off += len;
    return off;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Content
// ═══════════════════════════════════════════════════════════════════════════════════════════

void EditBuffer::clear() noexcept {
    text_.clear();
    caret_ = 0;
    anchor_ = 0;
    desired_column_valid_ = false;
    undo_.clear();
}

std::string EditBuffer::take() {
    std::string out = std::move(text_);
    text_.clear();  // moved-from is valid but unspecified; say what it is
    caret_ = 0;
    anchor_ = 0;
    desired_column_valid_ = false;
    undo_.clear();
    return out;
}

void EditBuffer::append(std::string_view t) {
    if (t.empty()) return;
    text_.append(t);
    caret_ = text_.size();
    anchor_ = caret_;
    desired_column_valid_ = false;
}

void EditBuffer::set_text(std::string_view t) {
    text_.assign(t);
    caret_ = text_.size();
    anchor_ = caret_;
    desired_column_valid_ = false;
    undo_.clear();
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Caret and selection
// ═══════════════════════════════════════════════════════════════════════════════════════════

size_t EditBuffer::selection_begin() const noexcept { return std::min(caret_, anchor_); }
size_t EditBuffer::selection_end() const noexcept { return std::max(caret_, anchor_); }

std::string_view EditBuffer::selected_text() const noexcept {
    const size_t b = selection_begin();
    const size_t e = selection_end();
    return std::string_view(text_).substr(b, e - b);
}

void EditBuffer::set_caret(size_t off, bool extend) noexcept {
    move_to(off, extend, false);
}

void EditBuffer::move_to(size_t off, bool extend, bool keep_desired_column) noexcept {
    // Two clamps, in this order, and both are load-bearing. The first keeps the caret inside the
    // settled region — which, because the unstable tail is not in this buffer at all, is
    // docs/editing.md §3's freeze rule in its entirety. The second puts it on a grapheme
    // boundary: a caret between a base letter and its combining mark is a Backspace away from a
    // corruption the user cannot see (§3.4).
    if (off > text_.size()) off = text_.size();
    caret_ = utf8::floor_grapheme(text_, off);
    if (!extend) anchor_ = caret_;
    if (!keep_desired_column) desired_column_valid_ = false;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Logical lines
// ═══════════════════════════════════════════════════════════════════════════════════════════
//
// See edit_buffer.h: these are '\n'-separated runs, not the visually wrapped lines the pill
// draws. A Windows-free module cannot know where DirectWrite broke a proportional font.

size_t EditBuffer::line_start(size_t off) const noexcept {
    const size_t nl = text_.rfind('\n', off == 0 ? 0 : off - 1);
    if (nl == std::string::npos || off == 0) return 0;
    return nl + 1;
}

size_t EditBuffer::line_end(size_t off) const noexcept {
    const size_t nl = text_.find('\n', off);
    return nl == std::string::npos ? text_.size() : nl;
}

size_t EditBuffer::grapheme_column(size_t off) const noexcept {
    const size_t begin = line_start(off);
    return utf8::count_graphemes(std::string_view(text_).substr(begin, off - begin));
}

size_t EditBuffer::offset_for_column(size_t begin, size_t column) const noexcept {
    const size_t end = line_end(begin);
    size_t at = begin;
    for (size_t i = 0; i < column && at < end; ++i) {
        const size_t next = utf8::next_grapheme(text_, at);
        if (next == utf8::kNpos || next > end) break;
        at = next;
    }
    return at;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Mutation helpers
// ═══════════════════════════════════════════════════════════════════════════════════════════

void EditBuffer::push_undo() {
    undo_.push_back(Snapshot{text_, caret_, anchor_});
    if (undo_.size() > kMaxUndo) {
        // Drop the oldest. The stack is a convenience, not a transaction log: losing the far end
        // of a long session's history is a better failure than growing without bound behind a
        // key the user can hold down.
        undo_.erase(undo_.begin());
    }
}

void EditBuffer::erase_range(size_t from, size_t to) {
    if (to <= from) return;
    text_.erase(from, to - from);
    caret_ = from;
    anchor_ = from;
}

bool EditBuffer::erase_selection() {
    if (!has_selection()) return false;
    erase_range(selection_begin(), selection_end());
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  apply — the one entry point that changes anything
// ═══════════════════════════════════════════════════════════════════════════════════════════

EditResult EditBuffer::apply(const EditCommand& c) {
    EditResult r;

    const std::string before_text = text_;
    const size_t before_caret = caret_;
    const size_t before_anchor = anchor_;
    const size_t undo_before = undo_.size();

    // Up and Down are the only two commands that inherit the sticky column; everything else
    // invalidates it, which move_to() does by default.
    const bool vertical = (c.op == EditOp::MoveUp || c.op == EditOp::MoveDown);
    if (vertical && !desired_column_valid_) {
        desired_column_ = grapheme_column(caret_);
        desired_column_valid_ = true;
    }

    switch (c.op) {
        case EditOp::None:
            break;

        case EditOp::InsertText: {
            if (c.text.empty()) break;
            push_undo();
            erase_selection();
            text_.insert(caret_, c.text);
            caret_ += c.text.size();
            // Not floor_grapheme'd: the insertion point was already a boundary and the inserted
            // run is well-formed, so its end is one too. Flooring here would silently move the
            // caret *backwards* through a combining mark the user has just typed.
            anchor_ = caret_;
            desired_column_valid_ = false;
            break;
        }

        case EditOp::Backspace: {
            push_undo();
            if (!erase_selection()) {
                if (caret_ == 0) break;
                const size_t prev = utf8::prev_grapheme(text_, caret_);
                erase_range(prev == utf8::kNpos ? 0 : prev, caret_);
            }
            desired_column_valid_ = false;
            break;
        }

        case EditOp::Delete: {
            push_undo();
            if (!erase_selection()) {
                if (caret_ >= text_.size()) break;
                const size_t next = utf8::next_grapheme(text_, caret_);
                erase_range(caret_, next == utf8::kNpos ? text_.size() : next);
            }
            desired_column_valid_ = false;
            break;
        }

        case EditOp::DeleteWordBack: {
            push_undo();
            if (!erase_selection()) {
                if (caret_ == 0) break;
                erase_range(utf8::retract_to_word_boundary(text_, caret_), caret_);
            }
            desired_column_valid_ = false;
            break;
        }

        case EditOp::DeleteWordForward: {
            push_undo();
            if (!erase_selection()) {
                if (caret_ >= text_.size()) break;
                erase_range(caret_, next_word_boundary(text_, caret_));
            }
            desired_column_valid_ = false;
            break;
        }

        case EditOp::MoveLeft: {
            // An unextended arrow against a selection collapses it to that end rather than
            // moving one cluster further, which is what every edit control does and what the
            // muscle memory of anyone correcting a word expects.
            if (has_selection() && !c.extend) {
                move_to(selection_begin(), false, false);
                break;
            }
            const size_t prev = caret_ == 0 ? 0 : utf8::prev_grapheme(text_, caret_);
            move_to(prev == utf8::kNpos ? 0 : prev, c.extend, false);
            break;
        }

        case EditOp::MoveRight: {
            if (has_selection() && !c.extend) {
                move_to(selection_end(), false, false);
                break;
            }
            const size_t next =
                caret_ >= text_.size() ? text_.size() : utf8::next_grapheme(text_, caret_);
            move_to(next == utf8::kNpos ? text_.size() : next, c.extend, false);
            break;
        }

        case EditOp::MoveWordLeft:
            move_to(caret_ == 0 ? 0 : utf8::retract_to_word_boundary(text_, caret_), c.extend,
                    false);
            break;

        case EditOp::MoveWordRight:
            move_to(next_word_boundary(text_, caret_), c.extend, false);
            break;

        case EditOp::MoveLineStart:
            move_to(line_start(caret_), c.extend, false);
            break;

        case EditOp::MoveLineEnd:
            move_to(line_end(caret_), c.extend, false);
            break;

        case EditOp::MoveDocStart:
            move_to(0, c.extend, false);
            break;

        case EditOp::MoveDocEnd:
            move_to(text_.size(), c.extend, false);
            break;

        case EditOp::MoveUp: {
            const size_t begin = line_start(caret_);
            if (begin == 0) {
                // No line above. edit_buffer.h's stated convention: go to the start of the
                // buffer, so that in the one-logical-line case that actually occurs the key does
                // something useful instead of nothing.
                move_to(0, c.extend, true);
                break;
            }
            move_to(offset_for_column(line_start(begin - 1), desired_column_), c.extend, true);
            break;
        }

        case EditOp::MoveDown: {
            const size_t end = line_end(caret_);
            if (end >= text_.size()) {
                move_to(text_.size(), c.extend, true);
                break;
            }
            move_to(offset_for_column(end + 1, desired_column_), c.extend, true);
            break;
        }

        case EditOp::SelectAll:
            anchor_ = 0;
            caret_ = text_.size();
            desired_column_valid_ = false;
            break;

        case EditOp::Copy:
            // Nothing here changes. The caller read `selected_text()` before calling, which is
            // the whole of Ctrl+C on this side of the Windows boundary.
            break;

        case EditOp::Cut:
            if (has_selection()) {
                push_undo();
                erase_selection();
            }
            desired_column_valid_ = false;
            break;

        case EditOp::Undo: {
            if (undo_.empty()) break;
            Snapshot s = std::move(undo_.back());
            undo_.pop_back();
            text_ = std::move(s.text);
            caret_ = std::min(s.caret, text_.size());
            anchor_ = std::min(s.anchor, text_.size());
            desired_column_valid_ = false;
            break;
        }

        case EditOp::PlaceCaret:
            move_to(c.offset, false, false);
            break;

        case EditOp::ExtendCaret:
            move_to(c.offset, true, false);
            break;
    }

    r.changed_text = text_ != before_text;
    r.changed_caret = caret_ != before_caret || anchor_ != before_anchor;

    // A push_undo() whose operation turned out to change nothing — Backspace at offset zero,
    // Delete at the end, Cut with no selection — would otherwise leave the stack one deep with a
    // snapshot identical to the present, so the user's next Ctrl+Z would appear to do nothing.
    // Conditioned on this command having pushed, so that Undo itself (which pops) is never a
    // candidate for the pop below.
    if (!r.changed_text && undo_.size() == undo_before + 1) {
        undo_.pop_back();
    }

    return r;
}

}  // namespace kd
