// Suite `edit_buffer` — src/edit_buffer.cpp, the compose buffer's text, caret and selection.
//
// docs/editing.md §11 names the cases this file has to cover, and they are the cases where the
// difference between "works" and "corrupts the user's document" is one byte: the caret clamped to
// the settled region, the tail not addressable, Backspace and the arrows moving by grapheme
// cluster and not by byte, Ctrl+Backspace to the word boundary, selection replacement, the caret
// move and dropped selection on resume, appending while a selection is active, an empty buffer,
// and a buffer that is entirely selected and then typed over.
//
// The grapheme cases are the reason this module exists at all. §3.4: "A Backspace that removes a
// byte, or a combining mark from its base letter, is a corruption the user cannot see until it
// reaches the document."

#include "edit_buffer.h"

#include "kd_test.h"
#include "util/utf8.h"

namespace {

kd::EditCommand cmd(kd::EditOp op, bool extend = false) {
    kd::EditCommand c;
    c.op = op;
    c.extend = extend;
    return c;
}

kd::EditCommand type(std::string_view s) {
    kd::EditCommand c;
    c.op = kd::EditOp::InsertText;
    c.text.assign(s);
    return c;
}

kd::EditCommand place(size_t off, bool extend = false) {
    kd::EditCommand c;
    c.op = extend ? kd::EditOp::ExtendCaret : kd::EditOp::PlaceCaret;
    c.offset = off;
    return c;
}

// U+0065 U+0301 — "e" plus COMBINING ACUTE ACCENT. Three bytes, one grapheme cluster, and the
// canonical way to discover that an editor is working in bytes.
constexpr const char* kCombining = "cafe\xCC\x81";  // "café", decomposed

// U+1F469 U+200D U+1F4BB — WOMAN + ZWJ + LAPTOP. One cluster, eleven bytes, three code points.
constexpr const char* kZwj = "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB";

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  The empty buffer — every entry point has to survive it
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(edit_buffer, an_empty_buffer_absorbs_every_command_without_moving) {
    kd::EditBuffer b;
    for (kd::EditOp op : {kd::EditOp::Backspace, kd::EditOp::Delete, kd::EditOp::DeleteWordBack,
                          kd::EditOp::DeleteWordForward, kd::EditOp::MoveLeft,
                          kd::EditOp::MoveRight, kd::EditOp::MoveUp, kd::EditOp::MoveDown,
                          kd::EditOp::MoveWordLeft, kd::EditOp::MoveWordRight,
                          kd::EditOp::MoveLineStart, kd::EditOp::MoveLineEnd,
                          kd::EditOp::MoveDocStart, kd::EditOp::MoveDocEnd, kd::EditOp::SelectAll,
                          kd::EditOp::Copy, kd::EditOp::Cut, kd::EditOp::Undo}) {
        const kd::EditResult r = b.apply(cmd(op));
        KD_CHECK(!r.changed_text);
        KD_CHECK_EQ(b.caret(), size_t{0});
        KD_CHECK(b.text().empty());
        KD_CHECK(!b.has_selection());
    }
}

KD_TEST(edit_buffer, a_caret_past_the_end_is_clamped_rather_than_trusted) {
    // docs/editing.md §3: the caret clamps to [0, len(settled)] and the unstable tail is not
    // addressable. Because the tail is simply not in this buffer, the clamp *is* the freeze rule.
    kd::EditBuffer b;
    b.set_text("hello");
    b.set_caret(9999, false);
    KD_CHECK_EQ(b.caret(), size_t{5});
    b.apply(place(9999));
    KD_CHECK_EQ(b.caret(), size_t{5});
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Grapheme clusters, not bytes (docs/editing.md §3.4)
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(edit_buffer, backspace_removes_a_whole_cluster_not_a_byte) {
    kd::EditBuffer b;
    b.set_text(kCombining);
    KD_REQUIRE_EQ(b.text().size(), size_t{6});  // c a f e + 2 bytes of U+0301
    KD_CHECK_EQ(b.caret(), size_t{6});

    b.apply(cmd(kd::EditOp::Backspace));
    // The combining mark left with its base letter. A byte-wise Backspace would have left "cafe"
    // followed by a lone U+0301 that reattaches to whatever the user types next.
    KD_CHECK_EQ(b.text(), std::string("caf"));
}

KD_TEST(edit_buffer, backspace_removes_a_whole_zwj_sequence) {
    kd::EditBuffer b;
    b.set_text(std::string("hi ") + kZwj);
    KD_REQUIRE_EQ(b.text().size(), size_t{14});
    b.apply(cmd(kd::EditOp::Backspace));
    KD_CHECK_EQ(b.text(), std::string("hi "));
}

KD_TEST(edit_buffer, the_arrows_move_by_cluster_in_both_directions) {
    kd::EditBuffer b;
    b.set_text(kCombining);
    b.apply(cmd(kd::EditOp::MoveDocStart));
    for (int i = 0; i < 4; ++i) b.apply(cmd(kd::EditOp::MoveRight));
    KD_CHECK_EQ(b.caret(), size_t{6});  // four clusters: c a f e-with-acute
    b.apply(cmd(kd::EditOp::MoveLeft));
    KD_CHECK_EQ(b.caret(), size_t{3});  // back over the whole cluster, not over its last byte
}

KD_TEST(edit_buffer, delete_removes_the_cluster_in_front_of_the_caret) {
    kd::EditBuffer b;
    b.set_text(std::string(kZwj) + "!");
    b.apply(cmd(kd::EditOp::MoveDocStart));
    b.apply(cmd(kd::EditOp::Delete));
    KD_CHECK_EQ(b.text(), std::string("!"));
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Words
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(edit_buffer, ctrl_backspace_goes_to_the_word_boundary) {
    kd::EditBuffer b;
    b.set_text("hello world");
    b.apply(cmd(kd::EditOp::DeleteWordBack));
    // util/utf8.h's retract_to_word_boundary: "hello world" at 11 -> 5, the space held back with
    // the word it followed. docs/editing.md §3.4 names this function for exactly this key.
    KD_CHECK_EQ(b.text(), std::string("hello"));
    b.apply(cmd(kd::EditOp::DeleteWordBack));
    KD_CHECK_EQ(b.text(), std::string(""));
}

KD_TEST(edit_buffer, ctrl_right_stops_after_the_whitespace_that_follows_a_word) {
    kd::EditBuffer b;
    b.set_text("one two three");
    b.apply(cmd(kd::EditOp::MoveDocStart));
    b.apply(cmd(kd::EditOp::MoveWordRight));
    KD_CHECK_EQ(b.caret(), size_t{4});  // past "one" and the space after it
    b.apply(cmd(kd::EditOp::MoveWordRight));
    KD_CHECK_EQ(b.caret(), size_t{8});
    b.apply(cmd(kd::EditOp::MoveWordRight));
    KD_CHECK_EQ(b.caret(), b.size());
    b.apply(cmd(kd::EditOp::MoveWordRight));
    KD_CHECK_EQ(b.caret(), b.size());  // idempotent at the end
}

KD_TEST(edit_buffer, ctrl_delete_removes_forward_by_word) {
    kd::EditBuffer b;
    b.set_text("one two three");
    b.apply(cmd(kd::EditOp::MoveDocStart));
    b.apply(cmd(kd::EditOp::DeleteWordForward));
    KD_CHECK_EQ(b.text(), std::string("two three"));
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Selection
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(edit_buffer, shift_arrow_extends_and_a_bare_arrow_collapses) {
    kd::EditBuffer b;
    b.set_text("abcdef");
    b.apply(cmd(kd::EditOp::MoveLeft, true));
    b.apply(cmd(kd::EditOp::MoveLeft, true));
    KD_CHECK(b.has_selection());
    KD_CHECK_EQ(b.selected_text(), std::string_view("ef"));

    // An unextended arrow against a selection collapses it to that end rather than moving one
    // cluster further — what every edit control does, and what a user correcting a word expects.
    b.apply(cmd(kd::EditOp::MoveRight));
    KD_CHECK(!b.has_selection());
    KD_CHECK_EQ(b.caret(), size_t{6});
}

KD_TEST(edit_buffer, typing_over_a_selection_replaces_it) {
    kd::EditBuffer b;
    b.set_text("hello world");
    b.set_caret(6, false);
    b.set_caret(11, true);
    KD_REQUIRE_EQ(b.selected_text(), std::string_view("world"));
    b.apply(type("there"));
    KD_CHECK_EQ(b.text(), std::string("hello there"));
    KD_CHECK(!b.has_selection());
    KD_CHECK_EQ(b.caret(), size_t{11});
}

KD_TEST(edit_buffer, select_all_then_type_leaves_only_what_was_typed) {
    kd::EditBuffer b;
    b.set_text("a whole session of dictation");
    b.apply(cmd(kd::EditOp::SelectAll));
    KD_CHECK_EQ(b.selection_begin(), size_t{0});
    KD_CHECK_EQ(b.selection_end(), b.size());
    b.apply(type("x"));
    KD_CHECK_EQ(b.text(), std::string("x"));
    KD_CHECK_EQ(b.caret(), size_t{1});
}

KD_TEST(edit_buffer, backspace_over_a_selection_removes_the_selection_only) {
    kd::EditBuffer b;
    b.set_text("hello world");
    b.set_caret(5, false);
    b.set_caret(11, true);
    b.apply(cmd(kd::EditOp::Backspace));
    KD_CHECK_EQ(b.text(), std::string("hello"));
    KD_CHECK_EQ(b.caret(), size_t{5});
}

KD_TEST(edit_buffer, cut_removes_the_selection_and_copy_does_not) {
    kd::EditBuffer b;
    b.set_text("hello world");
    b.set_caret(0, false);
    b.set_caret(5, true);

    KD_CHECK_EQ(b.selected_text(), std::string_view("hello"));
    b.apply(cmd(kd::EditOp::Copy));
    KD_CHECK_EQ(b.text(), std::string("hello world"));  // Ctrl+C changes nothing here

    b.apply(cmd(kd::EditOp::Cut));
    KD_CHECK_EQ(b.text(), std::string(" world"));
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Dictation appends, and only appends (docs/editing.md §1)
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(edit_buffer, appending_while_a_selection_is_active_drops_it_and_lands_at_the_end) {
    // §1's invariant: "After any edit, dictation only ever appends. New speech is never inserted
    // at the caret. It lands at the end of the buffer, and the caret moves there to meet it."
    kd::EditBuffer b;
    b.set_text("the first sentence.");
    b.set_caret(4, false);
    b.set_caret(9, true);
    KD_REQUIRE(b.has_selection());

    b.append(" and the second.");
    KD_CHECK_EQ(b.text(), std::string("the first sentence. and the second."));
    KD_CHECK(!b.has_selection());
    KD_CHECK_EQ(b.caret(), b.size());
}

KD_TEST(edit_buffer, appending_after_an_edit_does_not_reinstate_what_was_deleted) {
    kd::EditBuffer b;
    b.set_text("hello wrold");
    b.apply(cmd(kd::EditOp::DeleteWordBack));
    KD_REQUIRE_EQ(b.text(), std::string("hello"));
    b.append(" world");
    KD_CHECK_EQ(b.text(), std::string("hello world"));
}

KD_TEST(edit_buffer, take_empties_the_buffer_so_nothing_can_flush_it_twice) {
    // main.cpp's flush path takes the text *before* anything can fail, so no path can inject the
    // same session into a document twice.
    kd::EditBuffer b;
    b.set_text("one session");
    const std::string first = b.take();
    KD_CHECK_EQ(first, std::string("one session"));
    KD_CHECK(b.empty());
    KD_CHECK_EQ(b.caret(), size_t{0});
    KD_CHECK_EQ(b.take(), std::string(""));
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Lines — the stated approximation
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(edit_buffer, home_and_end_work_over_logical_lines) {
    kd::EditBuffer b;
    b.set_text("first\nsecond\nthird");
    b.set_caret(9, false);  // inside "second"
    b.apply(cmd(kd::EditOp::MoveLineStart));
    KD_CHECK_EQ(b.caret(), size_t{6});
    b.apply(cmd(kd::EditOp::MoveLineEnd));
    KD_CHECK_EQ(b.caret(), size_t{12});
    b.apply(cmd(kd::EditOp::MoveDocStart));
    KD_CHECK_EQ(b.caret(), size_t{0});
    b.apply(cmd(kd::EditOp::MoveDocEnd));
    KD_CHECK_EQ(b.caret(), b.size());
}

KD_TEST(edit_buffer, up_and_down_keep_their_column_across_a_short_line) {
    kd::EditBuffer b;
    b.set_text("longest line\nab\nanother long line");
    b.set_caret(10, false);  // column 10 on line 0
    b.apply(cmd(kd::EditOp::MoveDown));
    KD_CHECK_EQ(b.caret(), size_t{15});  // clamped to the end of "ab"
    b.apply(cmd(kd::EditOp::MoveDown));
    // The sticky column survives the short line, which is the behaviour whose absence is noticed
    // immediately.
    KD_CHECK_EQ(b.caret(), size_t{26});
}

KD_TEST(edit_buffer, up_and_down_reach_the_ends_of_a_single_line_buffer) {
    // edit_buffer.h's stated convention. Speech carries no newlines and Enter commits rather than
    // inserting one, so one logical line is the case that actually occurs; Up and Down being dead
    // keys there would be worse than the approximation.
    kd::EditBuffer b;
    b.set_text("one line only");
    b.set_caret(4, false);
    b.apply(cmd(kd::EditOp::MoveUp));
    KD_CHECK_EQ(b.caret(), size_t{0});
    b.apply(cmd(kd::EditOp::MoveDown));
    KD_CHECK_EQ(b.caret(), b.size());
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Undo
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(edit_buffer, undo_restores_the_text_and_the_caret) {
    kd::EditBuffer b;
    b.set_text("hello world");
    b.apply(cmd(kd::EditOp::DeleteWordBack));
    KD_REQUIRE_EQ(b.text(), std::string("hello"));
    b.apply(cmd(kd::EditOp::Undo));
    KD_CHECK_EQ(b.text(), std::string("hello world"));
    KD_CHECK_EQ(b.caret(), size_t{11});
}

KD_TEST(edit_buffer, a_command_that_changed_nothing_does_not_consume_an_undo_step) {
    // Backspace at offset zero, Delete at the end, Cut with no selection. Without this the user's
    // next Ctrl+Z appears to do nothing, which reads as undo being broken.
    kd::EditBuffer b;
    b.set_text("abc");
    b.apply(cmd(kd::EditOp::MoveDocStart));
    b.apply(cmd(kd::EditOp::Backspace));
    b.apply(cmd(kd::EditOp::Cut));
    KD_CHECK_EQ(b.undo_depth(), size_t{0});

    b.apply(cmd(kd::EditOp::MoveDocEnd));
    b.apply(cmd(kd::EditOp::Delete));
    KD_CHECK_EQ(b.undo_depth(), size_t{0});

    b.apply(cmd(kd::EditOp::Backspace));
    KD_CHECK_EQ(b.undo_depth(), size_t{1});
    b.apply(cmd(kd::EditOp::Undo));
    KD_CHECK_EQ(b.text(), std::string("abc"));
    KD_CHECK_EQ(b.undo_depth(), size_t{0});
}

KD_TEST(edit_buffer, the_undo_stack_is_bounded) {
    // The buffer holds a whole session, and an unbounded history of it is a memory leak with a
    // keyboard shortcut.
    kd::EditBuffer b;
    b.set_text("");
    for (size_t i = 0; i < kd::EditBuffer::kMaxUndo * 2; ++i) b.apply(type("x"));
    KD_CHECK_EQ(b.undo_depth(), kd::EditBuffer::kMaxUndo);
}

KD_TEST(edit_buffer, dictation_is_not_one_of_the_users_actions_to_undo) {
    kd::EditBuffer b;
    b.set_text("said");
    b.append(" more");
    KD_CHECK_EQ(b.undo_depth(), size_t{0});
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Every offset this module produces is a grapheme boundary
// ═══════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(edit_buffer, the_caret_is_always_on_a_grapheme_boundary) {
    // The property util/utf8.h §1 states for every offset in this program, checked against the
    // one module whose whole job is producing them. A caret placed by a mouse click at an
    // arbitrary pixel is the case that would otherwise land mid-sequence.
    kd::EditBuffer b;
    b.set_text(std::string(kCombining) + " " + kZwj + " tail");
    for (size_t off = 0; off <= b.size(); ++off) {
        b.apply(place(off));
        KD_CHECK(kd::utf8::is_grapheme_boundary(b.text(), b.caret()));
    }
    for (size_t off = 0; off <= b.size(); ++off) {
        b.apply(place(0));
        b.apply(place(off, true));
        KD_CHECK(kd::utf8::is_grapheme_boundary(b.text(), b.caret()));
        KD_CHECK(kd::utf8::is_grapheme_boundary(b.text(), b.selection_end()));
    }
}

KD_TEST(edit_buffer, walking_the_whole_buffer_with_backspace_never_leaves_invalid_utf8) {
    kd::EditBuffer b;
    b.set_text(std::string(kCombining) + " " + kZwj + " tail");
    while (!b.empty()) {
        b.apply(cmd(kd::EditOp::Backspace));
        KD_CHECK(kd::utf8::validate(b.text()));
    }
    KD_CHECK_EQ(b.caret(), size_t{0});
}
