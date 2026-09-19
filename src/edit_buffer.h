// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// EditBuffer — the compose buffer's text, caret and selection.
//
// Implements: docs/editing.md §3 (one buffer, two regions), §3.4 (editing is grapheme-wise, not
// byte-wise) and §11 (the logic lives somewhere pure and is tested there).
//
// ─── Why this module exists at all ──────────────────────────────────────────────────────────
//
// docs/editing.md §11: "put the logic somewhere pure and test it there", and immediately after,
// "src/main.cpp has no tests and this feature must not put anything interesting there. Anything
// that would need a test in main.cpp belongs in EditBuffer instead." That is the entire brief.
// No Windows headers, no I/O, no clock, no allocation beyond the string it owns.
//
// ─── What is in the buffer, and what is deliberately not ────────────────────────────────────
//
// The pill renders two regions and this class holds exactly one of them:
//
//     App's EditBuffer                         CommitEngine::Result::pending_text
//     |------------------------------------|  |--------------------------------|
//     settled — the user's, editable           unstable tail — the model's, not editable
//                                caret ^
//
// **The unstable tail is not in here and must never be put in here.** docs/editing.md §3 makes
// that the whole freeze rule: the user may edit only what the model has finished saying, and an
// utterance becomes editable at the moment it finalises and joins this buffer. Because the tail
// is simply absent, the caret clamp — `[0, text().size())` — *is* the freeze rule, with no
// second, hand-maintained notion of "frozen" to drift out of step with the commit engine's.
// §14 records the alternative (let the user edit the tail and suppress revisions that would
// touch it) and why it was rejected.
//
// ─── Offsets ────────────────────────────────────────────────────────────────────────────────
//
// UTF-8 byte offsets on grapheme-cluster boundaries, exactly as util/utf8.h §1 requires of every
// offset in this program. Every entry point clamps with `floor_grapheme`, so a caller cannot
// place a caret inside a multi-byte sequence or between a base letter and its combining mark —
// docs/editing.md §3.4: "A Backspace that removes a byte, or a combining mark from its base
// letter, is a corruption the user cannot see until it reaches the document."
//
// ─── Lines are logical, and that is a stated approximation ──────────────────────────────────
//
// Up/Down and Home/End work over **logical** lines — runs separated by '\n' — not over the
// visually wrapped lines DirectWrite produces in the pill. A Windows-free module cannot know
// where a proportional font wrapped a line, and overlay.h's layout is the only thing that does.
//
// In practice the buffer is one logical line: speech carries no newlines and Enter commits
// rather than inserting one, so the only way a '\n' arrives is a paste. So that the two arrow
// keys are not simply dead there, Up from the first line moves to the start of the buffer and
// Down from the last moves to the end — the convention every single-line edit control follows,
// and the one that makes Up/Down useful in the case that actually occurs.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kd {

// ─── What the user asked for ────────────────────────────────────────────────────────────────
//
// The vocabulary is deliberately small and deliberately *decided elsewhere*: InputFilter turns
// (vk, modifiers) into one of these in the hook callback, and this module applies it on the UI
// thread. docs/editing.md §4.2 draws that line and hotkey.h explains why it cannot move — the
// callback may not allocate, and a printable character cannot be produced without asking the
// keyboard layout, which is a call the hook is not allowed to make.
enum class EditOp : uint8_t {
    None,

    InsertText,         // `text` — a printable character, or a paste the caller already read

    Backspace,
    Delete,
    DeleteWordBack,     // Ctrl+Backspace
    DeleteWordForward,  // Ctrl+Delete

    MoveLeft,
    MoveRight,
    MoveUp,
    MoveDown,
    MoveWordLeft,       // Ctrl+Left
    MoveWordRight,      // Ctrl+Right
    MoveLineStart,      // Home
    MoveLineEnd,        // End
    MoveDocStart,       // Ctrl+Home
    MoveDocEnd,         // Ctrl+End

    SelectAll,          // Ctrl+A

    // Ctrl+C / Ctrl+X. The clipboard is a Windows API and this module has none, so the split is:
    // the caller reads `selected_text()` *before* applying, and Cut is the only one of the two
    // that changes anything here. Ctrl+V never reaches this module at all — the caller reads the
    // clipboard and issues InsertText, so there is one insertion path rather than two.
    Copy,
    Cut,

    Undo,               // Ctrl+Z, over the bounded snapshot stack below

    // The mouse (docs/editing.md §5.2). `offset` carries the hit-tested position, because the
    // hit test needs the DirectWrite layout and therefore belongs to the overlay.
    PlaceCaret,
    ExtendCaret,
};

const char* to_string(EditOp op) noexcept;

struct EditCommand {
    EditOp op = EditOp::None;
    bool extend = false;   // Shift was held, or the mouse is being dragged
    std::string text;      // InsertText only
    size_t offset = 0;     // PlaceCaret / ExtendCaret only
};

// What changed, so the caller knows whether to repaint and what to put on the clipboard.
struct EditResult {
    bool changed_text = false;
    bool changed_caret = false;

    [[nodiscard]] bool changed() const noexcept { return changed_text || changed_caret; }
};

class EditBuffer {
public:
    EditBuffer() = default;

    // ── Content ─────────────────────────────────────────────────────────────────────────────

    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] bool empty() const noexcept { return text_.empty(); }
    [[nodiscard]] size_t size() const noexcept { return text_.size(); }

    void clear() noexcept;

    // Take the text and leave the buffer empty, in one step. `flush_deferred_commits` needs
    // exactly this: the text has to leave the buffer *before* anything can fail, so that no path
    // can inject the same session twice (main.cpp's `text.swap(deferred_)` had the same shape and
    // the same comment).
    [[nodiscard]] std::string take();

    // What happens to the caret when the model's text lands at the end of the buffer.
    //
    // `Follow` is docs/editing.md §1's invariant, in one word:
    //
    //   > **After any edit, dictation only ever appends.** New speech is never inserted at the
    //   > caret. It lands at the end of the buffer, and the caret moves there to meet it.
    //
    // The caret move is safe when the text arrives because the user asked for it: §3.2 closes the
    // microphone at the first edit key, so speech reaches the buffer only after the explicit
    // resume gesture that means "I am going to talk now". An earlier revision of the design kept
    // the mic open and snapped the caret on speech onset, which moved the caret out from under a
    // user mid-correction; §14 records the retraction.
    //
    // `Keep` is for the one arrival that is *not* the user asking: the `final` for the utterance
    // that was open when editing began (§3.3). That text is the whole reason they pressed the key
    // — it lands a few hundred milliseconds into their edit, by which time they are already
    // aiming at a word — and dragging the caret to the end of it would be §14's retracted
    // failure arriving by a different road. So the text lands and the caret stays where the user
    // put it. Nothing else has to move with it: appending only grows the tail, so every existing
    // offset — caret, anchor, and the rewriter's — still means what it meant.
    enum class Caret : uint8_t {
        Follow,  // to the end, behind the new text — the user asked to dictate
        Keep,    // exactly where it is — a late `final` arriving mid-edit
    };

    // Dictation. See `Caret` above for which of the two this is and why.
    void append(std::string_view t, Caret caret = Caret::Follow);

    // Replace the whole buffer. Caret to the end, selection dropped.
    void set_text(std::string_view t);

    // ── Caret and selection ─────────────────────────────────────────────────────────────────

    [[nodiscard]] size_t caret() const noexcept { return caret_; }
    [[nodiscard]] size_t anchor() const noexcept { return anchor_; }
    [[nodiscard]] bool has_selection() const noexcept { return caret_ != anchor_; }
    [[nodiscard]] size_t selection_begin() const noexcept;
    [[nodiscard]] size_t selection_end() const noexcept;
    [[nodiscard]] std::string_view selected_text() const noexcept;

    // Clamped into the buffer and floored to a grapheme boundary. `extend` keeps the existing
    // selection anchor (Shift+click, Shift+arrow); without it the selection collapses.
    void set_caret(size_t off, bool extend) noexcept;

    // ── The one entry point that changes anything ───────────────────────────────────────────

    EditResult apply(const EditCommand& c);

    // ── Undo ────────────────────────────────────────────────────────────────────────────────
    //
    // A bounded stack of whole-buffer snapshots (docs/editing.md §13 stage 4). Bounded because
    // the buffer holds a whole session and an unbounded history of it is a memory leak with a
    // keyboard shortcut; whole-buffer because the alternative — an operation log — is a second
    // model of the text to keep in step with the first, for a buffer measured in kilobytes.
    //
    // `append()` deliberately does **not** push a snapshot: the model's own output is not one of
    // the user's actions, and undoing it would put the buffer somewhere the microphone is about
    // to overwrite anyway.
    static constexpr size_t kMaxUndo = 64;
    [[nodiscard]] size_t undo_depth() const noexcept { return undo_.size(); }

private:
    struct Snapshot {
        std::string text;
        size_t caret = 0;
        size_t anchor = 0;
    };

    void push_undo();
    // Removes the selection if there is one and returns true. The shared first half of typing,
    // Backspace, Delete and Cut — all four mean "the selected text goes away first".
    bool erase_selection();
    void erase_range(size_t from, size_t to);
    void move_to(size_t off, bool extend, bool keep_desired_column) noexcept;

    [[nodiscard]] size_t line_start(size_t off) const noexcept;
    [[nodiscard]] size_t line_end(size_t off) const noexcept;
    [[nodiscard]] size_t grapheme_column(size_t off) const noexcept;
    [[nodiscard]] size_t offset_for_column(size_t line_begin, size_t column) const noexcept;

    std::string text_;
    size_t caret_ = 0;
    size_t anchor_ = 0;  // the other end of the selection; == caret_ when there is none

    // Sticky column for Up/Down, in grapheme clusters from the start of the logical line. Reset
    // by every command that is not Up or Down, so a run of Up/Down keeps its column across a
    // short line instead of collapsing onto it — the behaviour every text editor has and whose
    // absence is noticed immediately.
    size_t desired_column_ = 0;
    bool desired_column_valid_ = false;

    std::vector<Snapshot> undo_;
};

}  // namespace kd
