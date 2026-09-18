// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Suite `injector` — the parts of src/injector.cpp that can be asserted without a foreign window.
//
// plan.md §7 gates on three claims this file is responsible for:
//   * "exact Unicode injection in every declared supported target" — the event *sequence* half of
//     that claim is checkable here. Astral-plane characters are the case that breaks silently:
//     SendInput has no notion of a code point, so a surrogate pair must go out as two separate
//     KEYEVENTF_UNICODE events. Emit one and every emoji in the dictation becomes U+FFFD.
//   * "clipboard restored only when we still own the state we set" — exercised end to end against
//     the real clipboard, including the path where someone else changes it under us.
//   * "explicit failure state for unsupported and elevated ones" — the integrity comparison is a
//     pure function over synthetic levels and is tested as one.
//
// What is deliberately NOT covered here, and why:
//   * Injector::inject() itself. Every path through it calls validate_target(), which reads the
//     real foreground window, and then SendInput(), which types into whatever has focus — i.e.
//     into the terminal running the tests. There is no seam that would make that safe: injector.h
//     takes the target as an opaque HWND and the Windows calls are direct. tools/probe and
//     tools/testtarget cover it with a real window; docs/supported-targets.md records the results.
//   * capture_foreground_target(), validate_target(), secure_desktop_active(), password-field and
//     console detection. Same reason: their inputs are the live desktop. A test that captured the
//     test runner's own console window would assert that the code returns what it just read.
//   * process_integrity() against a real elevated process — that needs an elevated process to
//     exist and be ours to poke at. integrity_from_rid() is where the banding logic lives, and it
//     is tested exhaustively instead.
//
// ─── This suite touches the real system clipboard ───────────────────────────────────────────
//
// There is no clipboard seam to mock: the functions under test call OpenClipboard directly, which
// is the whole point of testing them (the bug class being guarded against is a Win32 sequencing
// bug, not a logic bug). Each clipboard test therefore snapshots the developer's clipboard through
// the module's own save path and puts it back afterwards — see ClipboardScope. Formats that
// clipboard_save() cannot preserve (CF_BITMAP, CF_ENHMETAFILE, the GDI/private ranges — see
// format_is_copyable in injector.cpp) are NOT restored: if you had a bitmap on the clipboard when
// you ran this suite, it is gone. That is stated rather than hidden.

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "injector.h"
#include "kd_test.h"
#include "util/utf8.h"

// ════════════════════════════════════════════════════════════════════════════════════════════
//  injector_detail, re-declared verbatim from src/injector.cpp
// ════════════════════════════════════════════════════════════════════════════════════════════
//
// injector.h is a Windows-free header by project rule, and this block was left out of it during
// the parallel implementation pass so that neither file had to be edited by two agents at once.
// The declarations below are a token-identical copy of the block at the top of src/injector.cpp.
//
// **Keep the two copies in step.** A drift here is a link error, not a silent miscompile — the
// signatures are mangled — so the failure mode is loud, but the duplication is still temporary:
// at integration time this belongs either in injector.h (every type below is already
// Windows-free) or in a new src/injector_detail.h.

namespace kd {
namespace injector_detail {

struct SynthKey {
    uint16_t vk = 0;
    uint16_t scan = 0;
    uint32_t flags = 0;
    uintptr_t extra = 0;
};

struct SynthAtom {
    uint32_t first_event = 0;
    uint32_t event_count = 0;
    uint32_t utf8_bytes = 0;
};

inline constexpr uint32_t kStuckShift = 1u << 0;
inline constexpr uint32_t kStuckAlt = 1u << 1;

bool build_text_events(std::string_view utf8, uintptr_t extra, std::vector<SynthKey>& events,
                       std::vector<SynthAtom>& atoms, size_t* bad_offset, const char** reason);

void build_paste_chord(bool ctrl_shift_v, uint32_t stuck_mods, uintptr_t extra,
                       std::vector<SynthKey>& events);

IntegrityLevel integrity_from_rid(uint32_t rid) noexcept;
int integrity_rank(IntegrityLevel l) noexcept;
bool integrity_allows_injection(IntegrityLevel ours, IntegrityLevel target) noexcept;
IntegrityLevel process_integrity(uint32_t pid, uint32_t* os_error) noexcept;
IntegrityLevel current_process_integrity() noexcept;

struct ClipboardBackup;
ClipboardBackup* clipboard_backup_create();
void clipboard_backup_destroy(ClipboardBackup* b) noexcept;
size_t clipboard_backup_format_count(const ClipboardBackup* b) noexcept;
size_t clipboard_backup_lost_format_count(const ClipboardBackup* b) noexcept;

bool clipboard_save(ClipboardBackup* b, ClipboardGuardState& st, uint32_t* os_error);
bool clipboard_set_utf8(std::string_view utf8, ClipboardGuardState& st, uint32_t* os_error);
bool clipboard_set_delayed(std::string_view utf8, std::wstring& out_utf16,
                           ClipboardGuardState& st, uint32_t* os_error);
void clipboard_restore(ClipboardBackup* b, ClipboardGuardState& st, std::wstring_view ours);

void paste_watch_arm(ClipboardBackup* backup, std::wstring text, ClipboardGuardState* report_to,
                     uint32_t sequence_at_set, uint32_t target_pid, uint32_t settle_ms,
                     uint32_t fallback_ms);
ClipboardBackup* paste_watch_adopt();
void paste_watch_cancel(const char* why);
bool paste_watch_run_now(const char* why, bool may_defer);
bool paste_watch_active();

uintptr_t clipboard_owner_window();

}  // namespace injector_detail
}  // namespace kd

namespace {

using kd::ClipboardGuardState;
using kd::IntegrityLevel;
using kd::injector_detail::SynthAtom;
using kd::injector_detail::SynthKey;

// A recognisable non-zero mark. The real one is drawn from BCryptGenRandom per process; all these
// tests need is that whatever is handed in reaches every generated event unchanged, because that
// value is the only thing stopping hotkey.h from treating our own typing as user input.
constexpr uintptr_t kMark = 0xA5A5'1234u;

// ─── Event-shape helpers ────────────────────────────────────────────────────────────────────

bool is_unicode_down(const SynthKey& k, wchar_t unit) {
    // wVk MUST be zero: a non-zero virtual key makes KEYEVENTF_UNICODE a no-op and the target
    // receives whatever that VK maps to on the current layout instead of the character we meant.
    return k.vk == 0 && k.scan == static_cast<uint16_t>(unit) &&
           k.flags == static_cast<uint32_t>(KEYEVENTF_UNICODE);
}

bool is_unicode_up(const SynthKey& k, wchar_t unit) {
    return k.vk == 0 && k.scan == static_cast<uint16_t>(unit) &&
           k.flags == (static_cast<uint32_t>(KEYEVENTF_UNICODE) |
                       static_cast<uint32_t>(KEYEVENTF_KEYUP));
}

bool is_vk_down(const SynthKey& k, uint16_t vk) { return k.vk == vk && k.flags == 0u; }

bool is_vk_up(const SynthKey& k, uint16_t vk) {
    return k.vk == vk && k.flags == static_cast<uint32_t>(KEYEVENTF_KEYUP);
}

struct Built {
    std::vector<SynthKey> events;
    std::vector<SynthAtom> atoms;
    size_t bad = 0;
    const char* reason = nullptr;
    bool ok = false;
};

Built build(std::string_view utf8, uintptr_t extra = kMark) {
    Built b;
    b.ok = kd::injector_detail::build_text_events(utf8, extra, b.events, b.atoms, &b.bad,
                                                  &b.reason);
    return b;
}

// As a std::string so KD_CHECK_EQ compares text rather than the two pointers it would compare for
// a `const char*` against a string literal — and so a failure prints the reason it actually got.
std::string reason_of(const Built& b) { return b.reason == nullptr ? std::string() : b.reason; }

// The invariant every pacing decision rests on: atoms tile the event list exactly, in order, with
// no gap and no overlap. A batch boundary is chosen on atom indices, so a hole here would mean a
// surrogate pair or a key-down/key-up pair split across two SendInput calls.
void check_atoms_tile_events(const Built& b) {
    uint32_t next = 0;
    for (const SynthAtom& a : b.atoms) {
        KD_CHECK_EQ(a.first_event, next);
        KD_CHECK_GT(a.event_count, 0u);
        next = a.first_event + a.event_count;
    }
    KD_CHECK_EQ(static_cast<size_t>(next), b.events.size());
}

uint32_t total_atom_bytes(const Built& b) {
    uint32_t n = 0;
    for (const SynthAtom& a : b.atoms) n += a.utf8_bytes;
    return n;
}

}  // namespace

// ════════════════════════════════════════════════════════════════════════════════════════════
//  Event synthesis — UTF-8 to KEYEVENTF_UNICODE
// ════════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(injector, ascii_is_two_events_per_character) {
    const Built b = build("Hi");
    KD_REQUIRE(b.ok);
    KD_CHECK(b.reason == nullptr);
    KD_CHECK_EQ(b.bad, kd::utf8::kNpos);
    KD_REQUIRE_EQ(b.events.size(), size_t{4});
    KD_REQUIRE_EQ(b.atoms.size(), size_t{2});

    KD_CHECK(is_unicode_down(b.events[0], L'H'));
    KD_CHECK(is_unicode_up(b.events[1], L'H'));
    KD_CHECK(is_unicode_down(b.events[2], L'i'));
    KD_CHECK(is_unicode_up(b.events[3], L'i'));

    KD_CHECK_EQ(b.atoms[0].event_count, 2u);
    KD_CHECK_EQ(b.atoms[0].utf8_bytes, 1u);
    KD_CHECK_EQ(b.atoms[1].utf8_bytes, 1u);
    check_atoms_tile_events(b);
}

KD_TEST(injector, empty_text_produces_nothing) {
    const Built b = build("");
    KD_CHECK(b.ok);
    KD_CHECK(b.events.empty());
    KD_CHECK(b.atoms.empty());
    KD_CHECK(b.reason == nullptr);
    KD_CHECK_EQ(b.bad, kd::utf8::kNpos);
}

KD_TEST(injector, every_event_carries_the_injection_mark) {
    // If any event loses the mark, hotkey.h's filter sees genuine-looking user input and ends the
    // session that produced it — the first character typed kills the dictation.
    const Built b = build("a\xC3\xA9\r\n\xF0\x9F\x98\x80");
    KD_REQUIRE(b.ok);
    KD_REQUIRE(!b.events.empty());
    for (const SynthKey& k : b.events) KD_CHECK_EQ(k.extra, kMark);
}

KD_TEST(injector, bmp_multibyte_characters_record_their_utf8_length) {
    // é is 2 UTF-8 bytes and 1 UTF-16 unit; € is 3 and 1. bytes_injected on a partial write is a
    // UTF-8 prefix length handed to the commit engine, so an off-by-one here corrupts the
    // resumption point rather than just a counter.
    const Built b = build("\xC3\xA9\xE2\x82\xAC");  // U+00E9 U+20AC
    KD_REQUIRE(b.ok);
    KD_REQUIRE_EQ(b.atoms.size(), size_t{2});
    KD_REQUIRE_EQ(b.events.size(), size_t{4});

    KD_CHECK(is_unicode_down(b.events[0], L'é'));
    KD_CHECK(is_unicode_up(b.events[1], L'é'));
    KD_CHECK(is_unicode_down(b.events[2], L'€'));
    KD_CHECK(is_unicode_up(b.events[3], L'€'));

    KD_CHECK_EQ(b.atoms[0].utf8_bytes, 2u);
    KD_CHECK_EQ(b.atoms[1].utf8_bytes, 3u);
    KD_CHECK_EQ(total_atom_bytes(b), 5u);
    check_atoms_tile_events(b);
}

KD_TEST(injector, astral_character_becomes_two_events_per_code_unit) {
    // U+1F600 GRINNING FACE = F0 9F 98 80 in UTF-8, D83D DE00 in UTF-16.
    //
    // This is the test the module's comment calls out: SendInput has no notion of a code point, so
    // ONE KEYEVENTF_UNICODE event per UTF-16 code unit — four events for one character. Emitting
    // a single event with the code point truncated into 16 bits is the natural mistake and it
    // produces a lone surrogate at the far end.
    const Built b = build("\xF0\x9F\x98\x80");
    KD_REQUIRE(b.ok);
    KD_REQUIRE_EQ(b.atoms.size(), size_t{1});
    KD_REQUIRE_EQ(b.events.size(), size_t{4});

    // Nested order — down high, down low, up low, up high — so the two WM_CHARs land adjacent in
    // the target's queue. See the comment in build_text_events for why this is not sequential.
    KD_CHECK(is_unicode_down(b.events[0], L'\xD83D'));
    KD_CHECK(is_unicode_down(b.events[1], L'\xDE00'));
    KD_CHECK(is_unicode_up(b.events[2], L'\xDE00'));
    KD_CHECK(is_unicode_up(b.events[3], L'\xD83D'));

    KD_CHECK_EQ(b.atoms[0].event_count, 4u);
    KD_CHECK_EQ(b.atoms[0].utf8_bytes, 4u);  // a surrogate pair is always 4 UTF-8 bytes
    check_atoms_tile_events(b);
}

KD_TEST(injector, astral_and_bmp_mixed_keeps_byte_accounting_exact) {
    const std::string text = "a\xF0\x9F\x98\x80\xC3\xA9z";  // a, U+1F600, U+00E9, z
    const Built b = build(text);
    KD_REQUIRE(b.ok);
    KD_REQUIRE_EQ(b.atoms.size(), size_t{4});
    KD_CHECK_EQ(b.atoms[0].utf8_bytes, 1u);
    KD_CHECK_EQ(b.atoms[1].utf8_bytes, 4u);
    KD_CHECK_EQ(b.atoms[2].utf8_bytes, 2u);
    KD_CHECK_EQ(b.atoms[3].utf8_bytes, 1u);
    // The sum must equal the input exactly: this is what makes InjectResult::bytes_injected a real
    // prefix length rather than an estimate.
    KD_CHECK_EQ(static_cast<size_t>(total_atom_bytes(b)), text.size());
    KD_CHECK_EQ(b.events.size(), size_t{2 + 4 + 2 + 2});
    check_atoms_tile_events(b);
}

KD_TEST(injector, two_adjacent_astral_characters_stay_separate_atoms) {
    // Back-to-back surrogate pairs are where a naive "is this a high surrogate" scan pairs the low
    // surrogate of the first character with the high of the second.
    const Built b = build("\xF0\x9F\x98\x80\xF0\x9F\x91\x8D");  // U+1F600 U+1F44D
    KD_REQUIRE(b.ok);
    KD_REQUIRE_EQ(b.atoms.size(), size_t{2});
    KD_REQUIRE_EQ(b.events.size(), size_t{8});
    KD_CHECK_EQ(b.atoms[0].event_count, 4u);
    KD_CHECK_EQ(b.atoms[1].event_count, 4u);
    KD_CHECK(is_unicode_down(b.events[0], L'\xD83D'));
    KD_CHECK(is_unicode_up(b.events[3], L'\xD83D'));
    KD_CHECK(is_unicode_down(b.events[4], L'\xD83D'));
    KD_CHECK(is_unicode_down(b.events[5], L'\xDC4D'));
    KD_CHECK(is_unicode_up(b.events[6], L'\xDC4D'));
    KD_CHECK(is_unicode_up(b.events[7], L'\xD83D'));
    check_atoms_tile_events(b);
}

// ─── CR / LF ────────────────────────────────────────────────────────────────────────────────

KD_TEST(injector, crlf_collapses_to_one_return_key) {
    // A real VK_RETURN, not KEYEVENTF_UNICODE U+000D: the unicode form produces a WM_CHAR only,
    // which the many targets that watch for VK_RETURN in WM_KEYDOWN never see.
    const Built b = build("\r\n");
    KD_REQUIRE(b.ok);
    KD_REQUIRE_EQ(b.atoms.size(), size_t{1});
    KD_REQUIRE_EQ(b.events.size(), size_t{2});
    KD_CHECK(is_vk_down(b.events[0], VK_RETURN));
    KD_CHECK(is_vk_up(b.events[1], VK_RETURN));
    // Neither event may carry KEYEVENTF_UNICODE — that flag with a non-zero vk is a no-op.
    KD_CHECK_EQ(b.events[0].flags & static_cast<uint32_t>(KEYEVENTF_UNICODE), 0u);
    KD_CHECK_EQ(b.atoms[0].utf8_bytes, 2u);  // CRLF consumed both bytes
}

KD_TEST(injector, lone_cr_and_lone_lf_are_each_one_return) {
    for (const char* s : {"\r", "\n"}) {
        const Built b = build(s);
        KD_REQUIRE(b.ok);
        KD_REQUIRE_EQ(b.atoms.size(), size_t{1});
        KD_CHECK_EQ(b.atoms[0].utf8_bytes, 1u);
        KD_REQUIRE_EQ(b.events.size(), size_t{2});
        KD_CHECK(is_vk_down(b.events[0], VK_RETURN));
        KD_CHECK(is_vk_up(b.events[1], VK_RETURN));
    }
}

KD_TEST(injector, lf_cr_is_two_returns_not_one) {
    // Only CR-then-LF pairs. The reversed order is two separate line breaks, and collapsing it
    // would silently swallow one.
    const Built b = build("\n\r");
    KD_REQUIRE(b.ok);
    KD_REQUIRE_EQ(b.atoms.size(), size_t{2});
    KD_CHECK_EQ(b.atoms[0].utf8_bytes, 1u);
    KD_CHECK_EQ(b.atoms[1].utf8_bytes, 1u);
    KD_CHECK_EQ(b.events.size(), size_t{4});
}

KD_TEST(injector, crlf_between_text_keeps_offsets_aligned) {
    const std::string text = "a\r\nb";
    const Built b = build(text);
    KD_REQUIRE(b.ok);
    KD_REQUIRE_EQ(b.atoms.size(), size_t{3});
    KD_CHECK_EQ(b.atoms[0].utf8_bytes, 1u);
    KD_CHECK_EQ(b.atoms[1].utf8_bytes, 2u);
    KD_CHECK_EQ(b.atoms[2].utf8_bytes, 1u);
    KD_CHECK_EQ(static_cast<size_t>(total_atom_bytes(b)), text.size());
    KD_CHECK(is_unicode_down(b.events[0], L'a'));
    KD_CHECK(is_vk_down(b.events[2], VK_RETURN));
    KD_CHECK(is_unicode_down(b.events[4], L'b'));
    check_atoms_tile_events(b);
}

KD_TEST(injector, tab_goes_through_as_a_unicode_character) {
    // Tab is one of the three control characters allowed through, and it goes as a character
    // rather than VK_TAB on purpose: VK_TAB moves focus in a dialog, which is a wrong-window
    // hazard, whereas WM_CHAR 0x09 inserts a tab in the control that already has focus.
    const Built b = build("\t");
    KD_REQUIRE(b.ok);
    KD_REQUIRE_EQ(b.events.size(), size_t{2});
    KD_CHECK(is_unicode_down(b.events[0], L'\t'));
    KD_CHECK(is_unicode_up(b.events[1], L'\t'));
    KD_CHECK_EQ(b.atoms[0].utf8_bytes, 1u);
}

// ─── Refusals ───────────────────────────────────────────────────────────────────────────────

KD_TEST(injector, rejects_forbidden_control_characters) {
    // U+0008 delivered as a character IS the backspace path, smuggled in as text — and injector.h
    // forbids a delete path outright. Rejecting beats filtering: a caller told "sent" about a
    // string that was quietly shortened has no way to notice.
    struct Case {
        const char* text;
        size_t length;  // explicit, so the embedded-NUL case is not cut short at its first byte
        size_t offset;
    };
    const Case cases[] = {
        {"\x08", 1, 0},          // backspace — the delete path, smuggled in as text
        {"a\x7F", 2, 1},         // DEL
        {"ab\x1B", 3, 2},        // ESC dismisses dialogs
        // Split literal on purpose: MSVC consumes hex-escape digits greedily, so "a\x0Cb" is the
        // two bytes 61 CB, not the three this case is about.
        {"a\x0C" "b", 3, 1},     // form feed
        {"\x00x", 2, 0},         // NUL: valid UTF-8, still not something to type
        {"a\x01\x02", 3, 1},     // the first offending byte is reported, not the last
    };
    for (const Case& c : cases) {
        const Built b = build(std::string_view(c.text, c.length));
        KD_CHECK(!b.ok);
        KD_CHECK_EQ(reason_of(b), std::string("forbidden-control"));
        KD_CHECK_EQ(b.bad, c.offset);
        // Cleared, not left holding a partial sequence a careless caller could still send.
        KD_CHECK(b.events.empty());
        KD_CHECK(b.atoms.empty());
    }
}

KD_TEST(injector, rejects_malformed_utf8_before_looking_at_controls) {
    const Built overlong = build("\xC0\xAF");  // overlong encoding of '/'
    KD_CHECK(!overlong.ok);
    KD_CHECK_EQ(reason_of(overlong), std::string("malformed-utf8"));
    KD_CHECK_EQ(overlong.bad, size_t{0});

    const Built truncated = build("a\xE2\x82");  // three-byte sequence cut short
    KD_CHECK(!truncated.ok);
    KD_CHECK_EQ(reason_of(truncated), std::string("malformed-utf8"));
    KD_CHECK_EQ(truncated.bad, size_t{1});

    // CESU-8 encoding of U+D800. It must be refused here rather than reaching to_utf16 and
    // becoming a lone surrogate in a KEYEVENTF_UNICODE event.
    const Built lone_surrogate = build("\xED\xA0\x80");
    KD_CHECK(!lone_surrogate.ok);
    KD_CHECK_EQ(reason_of(lone_surrogate), std::string("malformed-utf8"));
}

// ════════════════════════════════════════════════════════════════════════════════════════════
//  Paste chord
// ════════════════════════════════════════════════════════════════════════════════════════════

namespace {

bool chord_contains_vk(const std::vector<SynthKey>& ev, uint16_t vk) {
    for (const SynthKey& k : ev) {
        if (k.vk == vk) return true;
    }
    return false;
}

}  // namespace

KD_TEST(injector, paste_chord_is_ctrl_v_by_default) {
    std::vector<SynthKey> ev;
    kd::injector_detail::build_paste_chord(false, 0, kMark, ev);
    KD_REQUIRE_EQ(ev.size(), size_t{4});
    KD_CHECK(is_vk_down(ev[0], VK_CONTROL));
    KD_CHECK(is_vk_down(ev[1], static_cast<uint16_t>('V')));
    KD_CHECK(is_vk_up(ev[2], static_cast<uint16_t>('V')));
    KD_CHECK(is_vk_up(ev[3], VK_CONTROL));
    for (const SynthKey& k : ev) {
        KD_CHECK_EQ(k.extra, kMark);
        KD_CHECK_EQ(k.flags & static_cast<uint32_t>(KEYEVENTF_UNICODE), 0u);
    }
}

KD_TEST(injector, paste_chord_is_ctrl_shift_v_for_consoles) {
    // Terminals bind Ctrl+V to something else — conhost and Windows Terminal want Ctrl+Shift+V.
    std::vector<SynthKey> ev;
    kd::injector_detail::build_paste_chord(true, 0, kMark, ev);
    KD_REQUIRE_EQ(ev.size(), size_t{6});
    KD_CHECK(is_vk_down(ev[0], VK_CONTROL));
    KD_CHECK(is_vk_down(ev[1], VK_SHIFT));
    KD_CHECK(is_vk_down(ev[2], static_cast<uint16_t>('V')));
    KD_CHECK(is_vk_up(ev[3], static_cast<uint16_t>('V')));
    KD_CHECK(is_vk_up(ev[4], VK_SHIFT));
    KD_CHECK(is_vk_up(ev[5], VK_CONTROL));
}

KD_TEST(injector, paste_chord_releases_a_stuck_shift_first) {
    // The hotkey chord's own modifiers are often still physically down when the first commit
    // lands. A held Shift turns Ctrl+V into Ctrl+Shift+V, which in a terminal is a *different*
    // paste and in an editor is a different command entirely.
    std::vector<SynthKey> ev;
    kd::injector_detail::build_paste_chord(false, kd::injector_detail::kStuckShift, kMark, ev);
    KD_REQUIRE_EQ(ev.size(), size_t{5});
    KD_CHECK(is_vk_up(ev[0], VK_SHIFT));
    KD_CHECK(is_vk_down(ev[1], VK_CONTROL));
}

KD_TEST(injector, stuck_shift_is_left_alone_when_the_chord_wants_shift) {
    // Releasing it here would immediately be undone by the chord's own Shift-down; sending both
    // would be two contradictory events one microsecond apart.
    std::vector<SynthKey> ev;
    kd::injector_detail::build_paste_chord(true, kd::injector_detail::kStuckShift, kMark, ev);
    KD_REQUIRE_EQ(ev.size(), size_t{6});
    KD_CHECK(is_vk_down(ev[0], VK_CONTROL));  // no leading Shift-up
}

KD_TEST(injector, paste_chord_releases_a_stuck_alt_in_both_modes) {
    std::vector<SynthKey> plain;
    kd::injector_detail::build_paste_chord(false, kd::injector_detail::kStuckAlt, kMark, plain);
    KD_REQUIRE_EQ(plain.size(), size_t{5});
    KD_CHECK(is_vk_up(plain[0], VK_MENU));

    std::vector<SynthKey> console;
    kd::injector_detail::build_paste_chord(true, kd::injector_detail::kStuckAlt, kMark, console);
    KD_REQUIRE_EQ(console.size(), size_t{7});
    KD_CHECK(is_vk_up(console[0], VK_MENU));
}

KD_TEST(injector, paste_chord_releases_both_stuck_modifiers) {
    std::vector<SynthKey> ev;
    kd::injector_detail::build_paste_chord(
        false, kd::injector_detail::kStuckShift | kd::injector_detail::kStuckAlt, kMark, ev);
    KD_REQUIRE_EQ(ev.size(), size_t{6});
    KD_CHECK(is_vk_up(ev[0], VK_SHIFT));
    KD_CHECK(is_vk_up(ev[1], VK_MENU));
    KD_CHECK(is_vk_down(ev[2], VK_CONTROL));
}

KD_TEST(injector, paste_chord_never_touches_the_win_key) {
    // Injecting a bare Win key-up is exactly the event that pops the Start menu — the wrong-window
    // hazard plan.md §1.8's suppression exists to avoid. A held Win costs us one paste; a Start
    // menu stealing focus mid-session costs the user their text.
    for (uint32_t mods = 0; mods <= 3u; ++mods) {
        for (bool shift_v : {false, true}) {
            std::vector<SynthKey> ev;
            kd::injector_detail::build_paste_chord(shift_v, mods, kMark, ev);
            KD_CHECK(!chord_contains_vk(ev, VK_LWIN));
            KD_CHECK(!chord_contains_vk(ev, VK_RWIN));
        }
    }
}

KD_TEST(injector, paste_chord_clears_the_caller_buffer) {
    // The Impl reuses one scratch vector across calls; a build that appended would send the
    // previous injection's text again on the next paste.
    std::vector<SynthKey> ev(9);
    kd::injector_detail::build_paste_chord(false, 0, kMark, ev);
    KD_CHECK_EQ(ev.size(), size_t{4});
}

// ════════════════════════════════════════════════════════════════════════════════════════════
//  Integrity levels — the explicit-unsupported decision, over synthetic values
// ════════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(injector, integrity_from_rid_rounds_down_to_the_band) {
    using kd::injector_detail::integrity_from_rid;
    KD_CHECK(integrity_from_rid(0) == IntegrityLevel::Untrusted);
    KD_CHECK(integrity_from_rid(SECURITY_MANDATORY_UNTRUSTED_RID) == IntegrityLevel::Untrusted);
    KD_CHECK(integrity_from_rid(SECURITY_MANDATORY_LOW_RID) == IntegrityLevel::Low);
    KD_CHECK(integrity_from_rid(SECURITY_MANDATORY_MEDIUM_RID) == IntegrityLevel::Medium);
    KD_CHECK(integrity_from_rid(SECURITY_MANDATORY_MEDIUM_PLUS_RID) == IntegrityLevel::MediumPlus);
    KD_CHECK(integrity_from_rid(SECURITY_MANDATORY_HIGH_RID) == IntegrityLevel::High);
    KD_CHECK(integrity_from_rid(SECURITY_MANDATORY_SYSTEM_RID) == IntegrityLevel::System);

    // The RIDs are band starts, not an enumeration — AppContainer and protected-process labels sit
    // between them. A value inside a band belongs to that band.
    KD_CHECK(integrity_from_rid(SECURITY_MANDATORY_LOW_RID + 1) == IntegrityLevel::Low);
    KD_CHECK(integrity_from_rid(SECURITY_MANDATORY_MEDIUM_RID + 0x50) == IntegrityLevel::Medium);
    KD_CHECK(integrity_from_rid(SECURITY_MANDATORY_MEDIUM_PLUS_RID + 0x50) ==
             IntegrityLevel::MediumPlus);
    KD_CHECK(integrity_from_rid(SECURITY_MANDATORY_HIGH_RID + 0x100) == IntegrityLevel::High);
    // Above the system band (protected process labels) must not wrap back down to something we
    // would then happily inject into.
    KD_CHECK(integrity_from_rid(SECURITY_MANDATORY_SYSTEM_RID + 0x1000) == IntegrityLevel::System);
    KD_CHECK(integrity_from_rid(0xFFFFFFFFu) == IntegrityLevel::System);
    // Just below a boundary stays in the lower band.
    KD_CHECK(integrity_from_rid(SECURITY_MANDATORY_MEDIUM_RID - 1) == IntegrityLevel::Low);
    KD_CHECK(integrity_from_rid(SECURITY_MANDATORY_HIGH_RID - 1) == IntegrityLevel::MediumPlus);
}

KD_TEST(injector, integrity_rank_is_strictly_ordered_and_unknown_is_negative) {
    using kd::injector_detail::integrity_rank;
    const IntegrityLevel ordered[] = {IntegrityLevel::Untrusted, IntegrityLevel::Low,
                                      IntegrityLevel::Medium,    IntegrityLevel::MediumPlus,
                                      IntegrityLevel::High,      IntegrityLevel::System};
    for (size_t i = 1; i < std::size(ordered); ++i) {
        KD_CHECK_GT(integrity_rank(ordered[i]), integrity_rank(ordered[i - 1]));
    }
    KD_CHECK_GE(integrity_rank(IntegrityLevel::Untrusted), 0);
    // Negative, so integrity_allows_injection can fail closed on it with a single sign test.
    KD_CHECK_LT(integrity_rank(IntegrityLevel::Unknown), 0);
}

KD_TEST(injector, integrity_allows_injection_only_downwards_or_level) {
    using kd::injector_detail::integrity_allows_injection;

    // Same level: the ordinary case.
    KD_CHECK(integrity_allows_injection(IntegrityLevel::Medium, IntegrityLevel::Medium));
    KD_CHECK(integrity_allows_injection(IntegrityLevel::High, IntegrityLevel::High));

    // Downwards is fine.
    KD_CHECK(integrity_allows_injection(IntegrityLevel::High, IntegrityLevel::Medium));
    KD_CHECK(integrity_allows_injection(IntegrityLevel::Medium, IntegrityLevel::Low));
    KD_CHECK(integrity_allows_injection(IntegrityLevel::System, IntegrityLevel::Untrusted));

    // Upwards is UIPI. SendInput returns success and the input goes nowhere, which is why this is
    // decided here instead of being inferred from a return value afterwards.
    KD_CHECK(!integrity_allows_injection(IntegrityLevel::Medium, IntegrityLevel::High));
    KD_CHECK(!integrity_allows_injection(IntegrityLevel::Medium, IntegrityLevel::MediumPlus));
    KD_CHECK(!integrity_allows_injection(IntegrityLevel::MediumPlus, IntegrityLevel::High));
    KD_CHECK(!integrity_allows_injection(IntegrityLevel::High, IntegrityLevel::System));

    // Unknown fails closed in both directions: not being able to open the target's token is itself
    // evidence of a privilege boundary, not an excuse to guess.
    KD_CHECK(!integrity_allows_injection(IntegrityLevel::Unknown, IntegrityLevel::Medium));
    KD_CHECK(!integrity_allows_injection(IntegrityLevel::High, IntegrityLevel::Unknown));
    KD_CHECK(!integrity_allows_injection(IntegrityLevel::Unknown, IntegrityLevel::Unknown));
}

KD_TEST(injector, check_supported_reports_elevated_targets_explicitly) {
    // The whole contract of InjectStatus::ElevatedUnsupported: a *named* state, decided before the
    // user has spoken a word, never a generic "injection seemed not to work".
    // check_supported() consults the live desktop before anything else, so if this runs while the
    // machine is locked every answer below is SecureDesktop and asserting on them would be
    // asserting the wrong thing. Say so and stop, rather than fail or silently pass.
    if (kd::secure_desktop_active()) {
        KD_INFO("not asserted: the secure desktop is active, which short-circuits check_supported");
        return;
    }

    kd::InjectConfig cfg;
    cfg.refuse_elevated_targets = true;
    cfg.refuse_password_fields = true;
    kd::Injector inj(cfg);

    kd::TargetIdentity t;
    t.top_level = 0x1234;  // never dereferenced on this path
    t.integrity = IntegrityLevel::System;  // above anything this test process can be
    KD_CHECK(inj.check_supported(t) == kd::InjectStatus::ElevatedUnsupported);

    // A password field is refused by policy, and that refusal is checked before integrity.
    kd::TargetIdentity pw;
    pw.top_level = 0x1234;
    pw.integrity = IntegrityLevel::Unknown;
    pw.is_password_field = true;
    KD_CHECK(inj.check_supported(pw) == kd::InjectStatus::PasswordField);

    // An invalid identity is a programming error, not an unsupported target.
    KD_CHECK(inj.check_supported(kd::TargetIdentity{}) == kd::InjectStatus::Failed);

    // Turning the guards off is a visible setting, and it must actually turn them off.
    cfg.refuse_elevated_targets = false;
    cfg.refuse_password_fields = false;
    inj.set_config(cfg);
    KD_CHECK(inj.check_supported(t) == kd::InjectStatus::Ok);
    KD_CHECK(inj.check_supported(pw) == kd::InjectStatus::Ok);
}

KD_TEST(injector, target_identity_compares_the_focused_control_too) {
    // Tabbing between fields inside one window is a target change: text meant for a search box does
    // not belong in the document behind it.
    kd::TargetIdentity a;
    a.top_level = 0x10;
    a.focus = 0x20;
    a.thread_id = 7;
    a.process_id = 9;

    kd::TargetIdentity b = a;
    KD_CHECK(a.same_as(b));

    b.focus = 0x21;
    KD_CHECK(!a.same_as(b));

    b = a;
    b.top_level = 0x11;
    KD_CHECK(!a.same_as(b));

    b = a;
    b.thread_id = 8;
    KD_CHECK(!a.same_as(b));

    b = a;
    b.process_id = 10;
    KD_CHECK(!a.same_as(b));

    // Fields that are *descriptions* of the target, not its identity, must not make it a different
    // target — otherwise a title change or a late policy lookup would abort a live session.
    b = a;
    b.process_name = "something.exe";
    b.is_console = true;
    b.integrity = IntegrityLevel::Low;
    KD_CHECK(a.same_as(b));
}

// ════════════════════════════════════════════════════════════════════════════════════════════
//  Clipboard save / set / restore — plan.md §7's ownership gate
// ════════════════════════════════════════════════════════════════════════════════════════════

namespace {

// Opening the clipboard fails while another process holds it, and on a developer machine something
// often does (clipboard managers, Office, RDP bridges). The module retries for ~63 ms internally;
// this adds a few outer attempts so a busy machine produces a slow test rather than a red one.
constexpr int kOuterAttempts = 5;

// Dispatch whatever is waiting, and why a test suite has to.
//
// Once a paste is armed, our own clipboard data is delay-rendered: a reader asking for it sends
// WM_RENDERFORMAT to the owner window and **waits**, holding the clipboard open for as long as
// it waits. A thread that is not pumping therefore blocks every other application's clipboard
// access, including the next line of the test. The application pumps continuously and never
// notices; a test body that runs clipboard calls back to back is the pathological case, so the
// ladders below pump between attempts.
void drain_messages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != FALSE) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool open_for_test() {
    HWND owner = reinterpret_cast<HWND>(kd::injector_detail::clipboard_owner_window());
    if (owner == nullptr) return false;
    for (int i = 0; i < 40; ++i) {
        if (OpenClipboard(owner) != FALSE) return true;
        drain_messages();
        Sleep(2);
    }
    return false;
}

// Puts text on the clipboard without going through the module — this is how the tests play the
// part of "the user pressed Ctrl+C" or "a clipboard manager rewrote it".
bool set_raw_text(const std::wstring& text) {
    if (!open_for_test()) return false;
    bool ok = false;
    if (EmptyClipboard() != FALSE) {
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h != nullptr) {
            void* dst = GlobalLock(h);
            if (dst != nullptr) {
                std::memcpy(dst, text.c_str(), bytes);
                GlobalUnlock(h);
                if (SetClipboardData(CF_UNICODETEXT, h) != nullptr) {
                    ok = true;
                } else {
                    GlobalFree(h);
                }
            } else {
                GlobalFree(h);
            }
        }
    }
    CloseClipboard();
    return ok;
}

bool clear_raw() {
    if (!open_for_test()) return false;
    const bool ok = EmptyClipboard() != FALSE;
    CloseClipboard();
    return ok;
}

// `present` distinguishes "the clipboard holds an empty string" from "the clipboard holds no text
// format at all"; the restore-onto-empty test needs the difference.
bool get_raw_text(std::wstring& out, bool& present) {
    out.clear();
    present = false;
    if (!open_for_test()) return false;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h != nullptr) {
        const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h));
        if (p != nullptr) {
            out.assign(p);
            present = true;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return true;
}

// Snapshots the developer's real clipboard for the duration of one test and puts it back.
//
// The restore deliberately re-stamps sequence_at_set with the current sequence number before
// calling clipboard_restore: the ownership check is the thing under test, and here we are the
// owner of the intent — this is the test putting the machine back the way it found it, not the
// module deciding whether it may.
class ClipboardScope {
public:
    ClipboardScope() : backup_(kd::injector_detail::clipboard_backup_create()) {
        for (int i = 0; i < kOuterAttempts; ++i) {
            if (kd::injector_detail::clipboard_save(backup_, state_, nullptr)) {
                ok_ = true;
                break;
            }
            drain_messages();
            Sleep(5);
        }
    }
    ~ClipboardScope() {
        if (!ok_) {
            kd::injector_detail::clipboard_backup_destroy(backup_);
            return;
        }
        ClipboardGuardState st = state_;
        // Empty, and it is never read: re-stamping the sequence below makes `moved` false, so
        // the content check is skipped. That is the point — this is the harness putting the
        // machine back the way it found it, not the module deciding whether it may.
        for (int i = 0; i < kOuterAttempts; ++i) {
            st.sequence_at_set = static_cast<uint32_t>(GetClipboardSequenceNumber());
            kd::injector_detail::clipboard_restore(backup_, st, {});
            if (st.restored) break;
            drain_messages();
            Sleep(5);
        }
        kd::injector_detail::clipboard_backup_destroy(backup_);
    }
    ClipboardScope(const ClipboardScope&) = delete;
    ClipboardScope& operator=(const ClipboardScope&) = delete;

    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    kd::injector_detail::ClipboardBackup* backup_ = nullptr;
    ClipboardGuardState state_;
    bool ok_ = false;
};

// Owns a backup for one test body.
class Backup {
public:
    Backup() : p_(kd::injector_detail::clipboard_backup_create()) {}
    ~Backup() { kd::injector_detail::clipboard_backup_destroy(p_); }
    Backup(const Backup&) = delete;
    Backup& operator=(const Backup&) = delete;
    kd::injector_detail::ClipboardBackup* get() const noexcept { return p_; }

private:
    kd::injector_detail::ClipboardBackup* p_;
};

bool save_with_retry(kd::injector_detail::ClipboardBackup* b, ClipboardGuardState& st) {
    for (int i = 0; i < kOuterAttempts; ++i) {
        if (kd::injector_detail::clipboard_save(b, st, nullptr)) return true;
        drain_messages();
        Sleep(5);
    }
    return false;
}

bool set_with_retry(std::string_view utf8, ClipboardGuardState& st) {
    for (int i = 0; i < kOuterAttempts; ++i) {
        if (kd::injector_detail::clipboard_set_utf8(utf8, st, nullptr)) return true;
        drain_messages();
        Sleep(5);
    }
    return false;
}

bool copy_with_retry(std::string_view utf8) {
    for (int i = 0; i < kOuterAttempts; ++i) {
        if (kd::set_clipboard_text(utf8)) return true;
        drain_messages();
        Sleep(5);
    }
    return false;
}

}  // namespace

KD_TEST(injector, clipboard_round_trip_restores_the_previous_contents) {
    ClipboardScope scope;
    KD_REQUIRE(scope.ok());

    KD_REQUIRE(set_raw_text(L"the user's own copy"));

    Backup backup;
    ClipboardGuardState st;
    KD_REQUIRE(save_with_retry(backup.get(), st));
    KD_CHECK(st.had_previous);
    KD_CHECK_GT(kd::injector_detail::clipboard_backup_format_count(backup.get()), size_t{0});

    // Non-ASCII on purpose: this is the CF_UNICODETEXT round trip, and a UTF-8/UTF-16 slip here
    // would show up as mojibake in the pasted text rather than as a failure.
    const char* ours = "dictated \xC3\xA9 text \xF0\x9F\x98\x80";
    KD_REQUIRE(set_with_retry(ours, st));
    KD_CHECK_NE(st.sequence_at_set, 0u);
    KD_CHECK(!st.restored);
    KD_CHECK(!st.skipped_because_changed);

    std::wstring now;
    bool present = false;
    KD_REQUIRE(get_raw_text(now, present));
    KD_CHECK(present);
    KD_CHECK(now == L"dictated é text \U0001F600");

    kd::injector_detail::clipboard_restore(backup.get(), st, L"dictated é text 😀");
    KD_CHECK(st.restored);
    KD_CHECK(!st.skipped_because_changed);

    KD_REQUIRE(get_raw_text(now, present));
    KD_CHECK(present);
    KD_CHECK(now == L"the user's own copy");
}

KD_TEST(injector, clipboard_restore_is_skipped_when_the_sequence_moved) {
    // THE gate. plan.md §1.7: "silently overwriting a user's copy is worse than leaving our text
    // there". Between our set and our restore the user presses Ctrl+C; restoring the pre-injection
    // snapshot at that point destroys what they just copied.
    ClipboardScope scope;
    KD_REQUIRE(scope.ok());

    KD_REQUIRE(set_raw_text(L"contents from before the injection"));

    Backup backup;
    ClipboardGuardState st;
    KD_REQUIRE(save_with_retry(backup.get(), st));
    KD_REQUIRE(st.had_previous);

    KD_REQUIRE(set_with_retry("our dictated text", st));
    const uint32_t seq_at_set = st.sequence_at_set;
    KD_CHECK_NE(seq_at_set, 0u);

    // Someone else — the user, a clipboard manager, the target app — changes it under us.
    KD_REQUIRE(set_raw_text(L"THE USER COPIED THIS"));
    KD_CHECK_NE(static_cast<uint32_t>(GetClipboardSequenceNumber()), seq_at_set);

    kd::injector_detail::clipboard_restore(backup.get(), st, L"our dictated text");

    KD_CHECK(st.skipped_because_changed);
    KD_CHECK(!st.restored);

    std::wstring now;
    bool present = false;
    KD_REQUIRE(get_raw_text(now, present));
    KD_CHECK(present);
    // The user's copy survives untouched. Both wrong answers are checked for explicitly, because
    // "it is not the user's text" is the failure and either substitute would be a bug.
    KD_CHECK(now == L"THE USER COPIED THIS");
    KD_CHECK(now != L"contents from before the injection");
    KD_CHECK(now != L"our dictated text");
}

KD_TEST(injector, a_sequence_bump_that_left_our_text_alone_still_restores) {
    // The case that made the guard useless on a real machine. A clipboard manager (Ditto here,
    // Windows' own Win+V history elsewhere) wakes on our write, reads it, and re-publishes it —
    // and Windows synthesises CF_TEXT/CF_OEMTEXT/CF_LOCALE from CF_UNICODETEXT besides. Every
    // one of those bumps the sequence number while leaving our own string sitting there.
    // Measured 2026-09-09: +6 on every single clipboard injection, so the guard fired every time
    // and the user's clipboard was never once put back.
    //
    // The sequence number now decides only whether to look. What decides the answer is whether
    // the clipboard still holds our text.
    ClipboardScope scope;
    KD_REQUIRE(scope.ok());

    KD_REQUIRE(set_raw_text(L"the user's own copy"));

    Backup backup;
    ClipboardGuardState st;
    KD_REQUIRE(save_with_retry(backup.get(), st));
    KD_REQUIRE(st.had_previous);

    KD_REQUIRE(set_with_retry("our dictated text", st));
    const uint32_t seq_at_set = st.sequence_at_set;

    // Re-publish the *same* string, exactly as a clipboard manager does. A different handle, the
    // same characters — which is why the check compares content and not the handle.
    KD_REQUIRE(set_raw_text(L"our dictated text"));
    KD_CHECK_NE(static_cast<uint32_t>(GetClipboardSequenceNumber()), seq_at_set);

    kd::injector_detail::clipboard_restore(backup.get(), st, L"our dictated text");
    KD_CHECK(st.restored);
    KD_CHECK(!st.skipped_because_changed);

    std::wstring now;
    bool present = false;
    KD_REQUIRE(get_raw_text(now, present));
    KD_CHECK(present);
    KD_CHECK(now == L"the user's own copy");
}

KD_TEST(injector, clipboard_set_records_the_sequence_after_closing) {
    // The sequence number has to be read after CloseClipboard: read it while still open and it is
    // the value from before our own change, which would make the ownership check compare our text
    // against a stale number and restore over a clipboard we no longer own.
    ClipboardScope scope;
    KD_REQUIRE(scope.ok());

    ClipboardGuardState st;
    KD_REQUIRE(set_with_retry("sequence probe", st));
    KD_CHECK_EQ(st.sequence_at_set, static_cast<uint32_t>(GetClipboardSequenceNumber()));
}

KD_TEST(injector, clipboard_set_resets_the_guard_state) {
    // The Impl reuses one ClipboardGuardState across injections; stale flags would be reported as
    // this injection's diagnostics.
    ClipboardScope scope;
    KD_REQUIRE(scope.ok());

    ClipboardGuardState st;
    st.restored = true;
    st.skipped_because_changed = true;
    st.sequence_at_set = 0xDEADBEEF;
    KD_REQUIRE(set_with_retry("fresh", st));
    KD_CHECK(!st.restored);
    KD_CHECK(!st.skipped_because_changed);
    KD_CHECK_NE(st.sequence_at_set, 0xDEADBEEFu);
}

KD_TEST(injector, clipboard_restore_onto_an_empty_clipboard_leaves_it_empty) {
    // "Restored" for an empty backup means empty — not "left our text there". A user who had
    // nothing on the clipboard should still have nothing.
    ClipboardScope scope;
    KD_REQUIRE(scope.ok());

    KD_REQUIRE(clear_raw());

    Backup backup;
    ClipboardGuardState st;
    KD_REQUIRE(save_with_retry(backup.get(), st));
    KD_CHECK(!st.had_previous);
    KD_CHECK_EQ(kd::injector_detail::clipboard_backup_format_count(backup.get()), size_t{0});

    KD_REQUIRE(set_with_retry("text that must not survive the restore", st));
    kd::injector_detail::clipboard_restore(backup.get(), st,
                                           L"text that must not survive the restore");
    KD_CHECK(st.restored);
    KD_CHECK(!st.skipped_because_changed);

    std::wstring now;
    bool present = false;
    KD_REQUIRE(get_raw_text(now, present));
    KD_CHECK(!present);
}

KD_TEST(injector, clipboard_restore_without_a_save_does_nothing) {
    // The early-return paths in inject_via_clipboard call restore on a backup that may never have
    // been filled. Emptying the clipboard there would be a data-loss bug in an error path.
    ClipboardScope scope;
    KD_REQUIRE(scope.ok());

    KD_REQUIRE(set_raw_text(L"untouched"));

    Backup backup;  // created, never saved into
    ClipboardGuardState st;
    st.sequence_at_set = static_cast<uint32_t>(GetClipboardSequenceNumber());
    kd::injector_detail::clipboard_restore(backup.get(), st, L"untouched");
    KD_CHECK(!st.restored);
    KD_CHECK(!st.skipped_because_changed);

    std::wstring now;
    bool present = false;
    KD_REQUIRE(get_raw_text(now, present));
    KD_CHECK(present);
    KD_CHECK(now == L"untouched");

    // And a null backup must be survivable rather than a crash in an error path.
    ClipboardGuardState st2;
    kd::injector_detail::clipboard_restore(nullptr, st2, L"untouched");
    KD_CHECK(!st2.restored);
}

KD_TEST(injector, clipboard_save_preserves_multiple_formats) {
    // The save path snapshots every HGLOBAL-backed format, not just CF_UNICODETEXT. Windows
    // synthesises CF_TEXT, CF_OEMTEXT and CF_LOCALE from CF_UNICODETEXT, so a clipboard holding
    // one real text format enumerates as several — which is enough to prove the loop walks them
    // all rather than stopping at the first.
    ClipboardScope scope;
    KD_REQUIRE(scope.ok());

    KD_REQUIRE(set_raw_text(L"multi-format probe"));

    Backup backup;
    ClipboardGuardState st;
    KD_REQUIRE(save_with_retry(backup.get(), st));
    const size_t kept = kd::injector_detail::clipboard_backup_format_count(backup.get());
    KD_CHECK_GE(kept, size_t{1});
    KD_INFO("formats preserved: " + std::to_string(kept) + ", not preservable: " +
            std::to_string(kd::injector_detail::clipboard_backup_lost_format_count(backup.get())));

    KD_REQUIRE(set_with_retry("ours", st));
    kd::injector_detail::clipboard_restore(backup.get(), st, L"ours");
    KD_REQUIRE(st.restored);

    std::wstring now;
    bool present = false;
    KD_REQUIRE(get_raw_text(now, present));
    KD_CHECK(present);
    KD_CHECK(now == L"multi-format probe");
}

// ════════════════════════════════════════════════════════════════════════════════════════════
//  Settings plumbing and per-app policy
// ════════════════════════════════════════════════════════════════════════════════════════════

KD_TEST(injector, inject_method_round_trips_through_its_string_form) {
    using kd::InjectMethod;
    InjectMethod m = InjectMethod::Clipboard;
    KD_CHECK(kd::inject_method_from_string(kd::to_string(InjectMethod::SendInput), m));
    KD_CHECK(m == InjectMethod::SendInput);
    KD_CHECK(kd::inject_method_from_string(kd::to_string(InjectMethod::Clipboard), m));
    KD_CHECK(m == InjectMethod::Clipboard);

    // Case-insensitive, and the aliases a settings file might plausibly contain.
    KD_CHECK(kd::inject_method_from_string("SENDINPUT", m));
    KD_CHECK(m == InjectMethod::SendInput);
    KD_CHECK(kd::inject_method_from_string("paste", m));
    KD_CHECK(m == InjectMethod::Clipboard);

    // An unknown value must be rejected, not silently defaulted: a typo in a setting that quietly
    // picks a method is how a user ends up with the wrong injection path and no explanation.
    m = InjectMethod::SendInput;
    KD_CHECK(!kd::inject_method_from_string("uia", m));
    KD_CHECK(!kd::inject_method_from_string("", m));
    KD_CHECK(m == InjectMethod::SendInput);  // unchanged on failure
}

KD_TEST(injector, status_and_level_strings_are_all_distinct) {
    // These names appear in the tray, the overlay and the logs. Two states sharing a name makes an
    // unsupported target indistinguishable from a failure in a bug report.
    const kd::InjectStatus statuses[] = {
        kd::InjectStatus::Ok,           kd::InjectStatus::TargetChanged,
        kd::InjectStatus::ElevatedUnsupported, kd::InjectStatus::SecureDesktop,
        kd::InjectStatus::PasswordField, kd::InjectStatus::ClipboardUnavailable,
        kd::InjectStatus::PasteChordRejected, kd::InjectStatus::PartialWrite,
        kd::InjectStatus::Failed};
    for (size_t i = 0; i < std::size(statuses); ++i) {
        KD_CHECK_NE(kd::to_string(statuses[i]), nullptr);
        for (size_t j = i + 1; j < std::size(statuses); ++j) {
            KD_CHECK_NE(kd::to_string(statuses[i]), kd::to_string(statuses[j]));
        }
    }

    const IntegrityLevel levels[] = {IntegrityLevel::Untrusted, IntegrityLevel::Low,
                                     IntegrityLevel::Medium,    IntegrityLevel::MediumPlus,
                                     IntegrityLevel::High,      IntegrityLevel::System,
                                     IntegrityLevel::Unknown};
    for (size_t i = 0; i < std::size(levels); ++i) {
        for (size_t j = i + 1; j < std::size(levels); ++j) {
            KD_CHECK_NE(kd::to_string(levels[i]), kd::to_string(levels[j]));
        }
    }
}

KD_TEST(injector, app_policy_lookup_is_case_insensitive_on_the_exe_name) {
    // Keyed on the executable name, lowercased — not the window title, which changes with the
    // document, and not the HWND class, which every Electron app shares.
    std::vector<kd::AppPolicy> policies;
    kd::AppPolicy term;
    term.process_name = "WindowsTerminal.exe";
    term.method = kd::InjectMethod::Clipboard;
    term.use_ctrl_shift_v = true;
    policies.push_back(term);

    kd::AppPolicy blocked;
    blocked.process_name = "keepass.exe";
    blocked.supported = false;
    policies.push_back(blocked);

    const kd::AppPolicy* found = kd::find_app_policy(policies, "windowsterminal.exe");
    KD_REQUIRE(found != nullptr);
    KD_CHECK(found->use_ctrl_shift_v);
    KD_CHECK(found->method == kd::InjectMethod::Clipboard);

    KD_CHECK(kd::find_app_policy(policies, "WINDOWSTERMINAL.EXE") == found);

    const kd::AppPolicy* refused = kd::find_app_policy(policies, "KeePass.exe");
    KD_REQUIRE(refused != nullptr);
    KD_CHECK(!refused->supported);

    // No policy means "fall back to the global setting", which is a null, not a default-constructed
    // policy the caller might mistake for a real one.
    KD_CHECK(kd::find_app_policy(policies, "notepad.exe") == nullptr);
    KD_CHECK(kd::find_app_policy(policies, "") == nullptr);
    KD_CHECK(kd::find_app_policy({}, "notepad.exe") == nullptr);
}

KD_TEST(injector, injection_mark_is_stable_and_non_zero) {
    // Per-process random, drawn once. Zero would mean "no mark" to the hook filter, and a value
    // that changed between calls would leave earlier events unrecognised — either way our own
    // typing would be read as user input and stop the session producing it.
    const uintptr_t a = kd::injection_mark().extra_info;
    const uintptr_t b = kd::injection_mark().extra_info;
    KD_CHECK_NE(a, uintptr_t{0});
    KD_CHECK_EQ(a, b);

    // And it reaches the events, which is the only thing that makes it useful.
    const Built built = build("x", a);
    KD_REQUIRE(built.ok);
    KD_REQUIRE(!built.events.empty());
    KD_CHECK_EQ(built.events[0].extra, a);
}

KD_TEST(injector, config_survives_a_round_trip_through_the_injector) {
    kd::InjectConfig cfg;
    cfg.method = kd::InjectMethod::Clipboard;
    cfg.chars_per_batch = 7;
    cfg.inter_batch_delay_us = 250;
    cfg.paste_settle_ms = 33;
    cfg.refuse_password_fields = false;
    cfg.refuse_elevated_targets = false;

    kd::Injector inj(cfg);
    KD_CHECK(inj.config().method == kd::InjectMethod::Clipboard);
    KD_CHECK_EQ(inj.config().chars_per_batch, 7u);
    KD_CHECK_EQ(inj.config().inter_batch_delay_us, 250u);
    KD_CHECK_EQ(inj.config().paste_settle_ms, 33u);
    KD_CHECK(!inj.config().refuse_password_fields);

    cfg.chars_per_batch = 0;  // "one batch" — the fastest targets accept it
    inj.set_config(cfg);
    KD_CHECK_EQ(inj.config().chars_per_batch, 0u);

    // Never used yet, so the diagnostics must read as "nothing happened" rather than as a stale
    // success from a previous Injector.
    KD_CHECK(!inj.last_clipboard_state().restored);
    KD_CHECK(!inj.last_clipboard_state().skipped_because_changed);
    KD_CHECK_EQ(inj.last_clipboard_state().sequence_at_set, 0u);
}

KD_TEST(injector, empty_text_injection_is_a_no_op_success) {
    // The commit engine legitimately produces an empty commit when the stable prefix has not
    // advanced. That must not touch the clipboard, call SendInput, or even look at the target —
    // which is what makes this one safe to run without a foreign window.
    kd::InjectConfig cfg;
    cfg.method = kd::InjectMethod::Clipboard;
    kd::Injector inj(cfg);

    kd::TargetIdentity t;
    t.top_level = 0x1234;
    t.integrity = IntegrityLevel::System;  // would be refused if it got as far as the check

    const kd::InjectResult r = inj.inject(t, "");
    KD_CHECK(r.status == kd::InjectStatus::Ok);
    KD_CHECK_EQ(r.bytes_injected, size_t{0});
    KD_CHECK_EQ(r.os_error, 0u);
}


// ════════════════════════════════════════════════════════════════════════════════════════════
//  The deferred restore — delayed rendering and the "the target took it" signal
// ════════════════════════════════════════════════════════════════════════════════════════════
//
// What these can and cannot reach. The signal under test is "a *different* process asked Windows
// for our text", and a test has no different process to be. So the tests below play the target
// by reading the clipboard from this one, and set the watch's target pid accordingly: equal to
// our own pid where the case is "the target read it", and a pid that is not ours where the case
// is "somebody else did". That exercises every branch of the attribution except the one thing no
// unit test can produce — a real Electron paste — which is measured in docs/supported-targets.md
// instead.

namespace {

// Runs the message loop until `done` or the budget runs out. The restore lives on a WM_TIMER
// hung on the clipboard owner window, so nothing in this mechanism happens in a test that does
// not pump — which is itself the property App::shutdown() had to be taught about.
template <typename F>
bool pump_until(F done, DWORD budget_ms) {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        drain_messages();
        if (done()) return true;
        if (GetTickCount64() - start > budget_ms) return false;
        Sleep(1);
    }
}

// Arms the watch the way inject_via_clipboard does, minus the chord: snapshot, publish
// delay-rendered, hand both to the watch. Returns false if the clipboard was unreachable, which
// on a developer machine is a busy clipboard rather than a bug.
//
// The snapshot is created here and **belongs to the watch** from the moment it is armed — the
// same ownership transfer inject_via_clipboard makes with backup.release(). A test that also
// held it in a RAII wrapper would free it twice, so `out_backup` hands the pointer back only
// for the one test that has to recognise it again.
bool arm_paste(ClipboardGuardState& st, std::string_view text, uint32_t target_pid,
               uint32_t settle_ms, uint32_t fallback_ms,
               kd::injector_detail::ClipboardBackup** out_backup = nullptr) {
    kd::injector_detail::ClipboardBackup* b = kd::injector_detail::clipboard_backup_create();
    if (out_backup != nullptr) *out_backup = b;
    if (!save_with_retry(b, st)) {
        kd::injector_detail::clipboard_backup_destroy(b);
        return false;
    }
    std::wstring ours;
    bool set = false;
    for (int i = 0; i < kOuterAttempts; ++i) {
        if (kd::injector_detail::clipboard_set_delayed(text, ours, st, nullptr)) {
            set = true;
            break;
        }
        drain_messages();
        Sleep(5);
    }
    if (!set) {
        kd::injector_detail::clipboard_backup_destroy(b);
        return false;
    }
    kd::injector_detail::paste_watch_arm(b, std::move(ours), &st, st.sequence_at_set, target_pid,
                                         settle_ms, fallback_ms);
    return true;
}

// Every test here arms a process-wide watch; leaving one armed would have the next test's
// clipboard yanked out from under it by a timer with nothing to do with it.
struct WatchGuard {
    ~WatchGuard() { kd::injector_detail::paste_watch_cancel("test teardown"); }
};

}  // namespace

KD_TEST(injector, delayed_text_is_rendered_only_when_somebody_asks_for_it) {
    // The mechanism in one test: nothing is on the clipboard until a reader wants it, and what
    // the reader gets is exactly what was published — including the astral character, which is
    // the thing a UTF-16 slip in render_block() would mangle.
    ClipboardScope scope;
    KD_REQUIRE(scope.ok());

    KD_REQUIRE(set_raw_text(L"the user's own copy"));

    ClipboardGuardState st;
    WatchGuard guard;  // declared last, so it is destroyed first and never writes into a dead st
    KD_REQUIRE(arm_paste(st, "dictated \xC3\xA9 text \xF0\x9F\x98\x80",
                         GetCurrentProcessId(), 10, 60000));
    KD_CHECK(st.restore_pending);
    KD_CHECK(!st.restored);
    KD_CHECK(!st.target_read_it);

    // Published, but as an offer rather than as data.
    KD_CHECK(IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE);
    KD_CHECK_EQ(reinterpret_cast<uintptr_t>(GetClipboardOwner()),
                kd::injector_detail::clipboard_owner_window());

    std::wstring now;
    bool present = false;
    KD_REQUIRE(get_raw_text(now, present));
    KD_CHECK(present);
    KD_CHECK(now == L"dictated é text \U0001F600");
}

KD_TEST(injector, nothing_restores_while_nobody_reads) {
    // The failure this replaced was a stopwatch: 60 ms after the chord, the clipboard went back
    // whether or not the target had got there. Time alone must no longer be able to do that.
    ClipboardScope scope;
    KD_REQUIRE(scope.ok());

    KD_REQUIRE(set_raw_text(L"the user's own copy"));

    ClipboardGuardState st;
    WatchGuard guard;  // declared last, so it is destroyed first and never writes into a dead st
    KD_REQUIRE(arm_paste(st, "our dictated text", GetCurrentProcessId(), 10, 60000));

    // Two and a half times the old settle window, and nothing may have happened yet. (A
    // clipboard manager reading our text during this is fine and is not the target: this asserts
    // only that no restore ran.)
    KD_CHECK(!pump_until([] { return !kd::injector_detail::paste_watch_active(); }, 150));
    KD_CHECK(st.restore_pending);
    KD_CHECK(!st.restored);
}

KD_TEST(injector, a_read_by_the_target_is_what_schedules_the_restore) {
    // The whole point: the read is what starts the clock, and when the clock runs out the user's
    // clipboard is back.
    //
    // Read immediately, on purpose. Only the *first* render request reaches us — after we answer
    // one, the data is real and Windows serves everyone else without a word — so a clipboard
    // manager that gets there before this line takes the signal this test is about. It needs a
    // message-loop turn to do that and we do not give it one, which is enough on this machine
    // and is the same race the mechanism itself documents.
    ClipboardScope scope;
    KD_REQUIRE(scope.ok());

    KD_REQUIRE(set_raw_text(L"the user's own copy"));

    ClipboardGuardState st;
    WatchGuard guard;  // declared last, so it is destroyed first and never writes into a dead st
    // A fallback far beyond the test's patience: a restore here happened because of the read,
    // which is the claim being made.
    KD_REQUIRE(arm_paste(st, "our dictated text", GetCurrentProcessId(), 10, 60000));

    std::wstring now;
    bool present = false;
    KD_REQUIRE(get_raw_text(now, present));  // the target pastes
    KD_CHECK(now == L"our dictated text");
    KD_CHECK(st.target_read_it);

    KD_REQUIRE(pump_until([] { return !kd::injector_detail::paste_watch_active(); }, 2000));
    KD_CHECK(st.restored);
    KD_CHECK(!st.restore_pending);
    KD_CHECK(!st.skipped_because_changed);

    KD_REQUIRE(get_raw_text(now, present));
    KD_CHECK(present);
    KD_CHECK(now == L"the user's own copy");
}

KD_TEST(injector, a_read_by_somebody_else_does_not_schedule_the_restore) {
    // A clipboard manager reading our write is not the target pasting it. Attributing its read
    // to the target would restore early, which is the bug the whole mechanism exists to fix.
    ClipboardScope scope;
    KD_REQUIRE(scope.ok());

    KD_REQUIRE(set_raw_text(L"the user's own copy"));

    ClipboardGuardState st;
    WatchGuard guard;  // declared last, so it is destroyed first and never writes into a dead st
    // A pid that is not ours, so this process reading stands in for the clipboard manager.
    const uint32_t not_us = GetCurrentProcessId() ^ 0x4000u;
    KD_REQUIRE(arm_paste(st, "our dictated text", not_us, 10, 60000));

    std::wstring now;
    bool present = false;
    KD_REQUIRE(get_raw_text(now, present));
    KD_CHECK(now == L"our dictated text");  // it is still served — we do not withhold the data
    KD_CHECK(!st.target_read_it);

    // And the watch is still waiting for its fallback rather than counting down to a restore.
    KD_CHECK(!pump_until([] { return !kd::injector_detail::paste_watch_active(); }, 200));
    KD_CHECK(st.restore_pending);
}

KD_TEST(injector, the_fallback_restores_a_paste_nobody_ever_reads) {
    // Ctrl+V bound to something else, or ignored: no render request ever arrives. The user's
    // clipboard still has to come back, and bounded by something a person would not notice as
    // "it kept my clipboard".
    ClipboardScope scope;
    KD_REQUIRE(scope.ok());

    KD_REQUIRE(set_raw_text(L"the user's own copy"));

    ClipboardGuardState st;
    WatchGuard guard;  // declared last, so it is destroyed first and never writes into a dead st
    KD_REQUIRE(arm_paste(st, "text nobody asks for", GetCurrentProcessId(), 10, 30));

    KD_REQUIRE(pump_until([] { return !kd::injector_detail::paste_watch_active(); }, 2000));
    KD_CHECK(st.restored);
    KD_CHECK(!st.target_read_it);  // honestly reported: the chord may have gone nowhere

    std::wstring now;
    bool present = false;
    KD_REQUIRE(get_raw_text(now, present));
    KD_CHECK(present);
    KD_CHECK(now == L"the user's own copy");
}

KD_TEST(injector, a_second_paste_carries_the_first_snapshot_forward) {
    // main.cpp injects twice in a row routinely — the deferred flush, then the suffix. Taking a
    // fresh snapshot for the second one would snapshot our own dictation and then faithfully
    // restore *that*, leaving the user's clipboard gone for good.
    ClipboardScope scope;
    KD_REQUIRE(scope.ok());

    KD_REQUIRE(set_raw_text(L"the user's own copy"));

    ClipboardGuardState st1;
    ClipboardGuardState st2;
    WatchGuard guard;  // declared last, so it is destroyed first and never writes into a dead st
    kd::injector_detail::ClipboardBackup* first = nullptr;
    KD_REQUIRE(arm_paste(st1, "the first utterance", GetCurrentProcessId(), 10, 60000, &first));

    kd::injector_detail::ClipboardBackup* adopted = kd::injector_detail::paste_watch_adopt();
    KD_CHECK_EQ(reinterpret_cast<uintptr_t>(adopted), reinterpret_cast<uintptr_t>(first));
    KD_CHECK(!st1.restore_pending);
    KD_CHECK(!kd::injector_detail::paste_watch_active());

    // The second paste arms with the snapshot it inherited, and restoring it puts back what the
    // user had before the *first* one.
    std::wstring ours;
    bool published = false;
    for (int i = 0; i < kOuterAttempts && !published; ++i) {
        published = kd::injector_detail::clipboard_set_delayed("the second utterance", ours, st2,
                                                               nullptr);
        if (!published) {
            drain_messages();
            Sleep(5);
        }
    }
    KD_REQUIRE(published);
    kd::injector_detail::paste_watch_arm(adopted, std::move(ours), &st2, st2.sequence_at_set,
                                         GetCurrentProcessId(), 10, 30);

    KD_REQUIRE(pump_until([] { return !kd::injector_detail::paste_watch_active(); }, 2000));
    KD_CHECK(st2.restored);

    std::wstring now;
    bool present = false;
    KD_REQUIRE(get_raw_text(now, present));
    KD_CHECK(present);
    KD_CHECK(now == L"the user's own copy");
    KD_CHECK(now != L"the first utterance");
}

KD_TEST(injector, copying_the_last_dictation_abandons_a_pending_restore) {
    // The recovery path: a user whose paste went astray presses Copy, and a restore armed
    // moments earlier must not take it away again a second later.
    ClipboardScope scope;
    KD_REQUIRE(scope.ok());

    KD_REQUIRE(set_raw_text(L"the user's own copy"));

    ClipboardGuardState st;
    WatchGuard guard;  // declared last, so it is destroyed first and never writes into a dead st
    KD_REQUIRE(arm_paste(st, "the dictation", GetCurrentProcessId(), 10, 30));

    KD_REQUIRE(copy_with_retry("the dictation"));
    KD_CHECK(!kd::injector_detail::paste_watch_active());
    KD_CHECK(!st.restore_pending);

    // Pump well past the fallback the watch was armed with: nothing may reclaim the clipboard.
    KD_CHECK(!pump_until([] { return kd::injector_detail::paste_watch_active(); }, 200));

    std::wstring now;
    bool present = false;
    KD_REQUIRE(get_raw_text(now, present));
    KD_CHECK(present);
    KD_CHECK(now == L"the dictation");
}

KD_TEST(injector, a_copy_during_the_paste_is_left_alone) {
    // The ownership gate, now on the deferred path: the user presses Ctrl+C while the paste is
    // outstanding, and the restore must leave their copy where it is rather than overwrite it
    // with a snapshot taken before they made it.
    ClipboardScope scope;
    KD_REQUIRE(scope.ok());

    KD_REQUIRE(set_raw_text(L"contents from before the injection"));

    ClipboardGuardState st;
    WatchGuard guard;  // declared last, so it is destroyed first and never writes into a dead st
    KD_REQUIRE(arm_paste(st, "our dictated text", GetCurrentProcessId(), 10, 30));

    KD_REQUIRE(set_raw_text(L"THE USER COPIED THIS"));

    KD_REQUIRE(pump_until([] { return !kd::injector_detail::paste_watch_active(); }, 2000));
    KD_CHECK(st.skipped_because_changed);
    KD_CHECK(!st.restored);

    std::wstring now;
    bool present = false;
    KD_REQUIRE(get_raw_text(now, present));
    KD_CHECK(present);
    KD_CHECK(now == L"THE USER COPIED THIS");
}

KD_TEST(injector, the_restore_can_be_forced_for_a_process_on_its_way_out) {
    // App::shutdown() calls this: after the message loop stops, the timer never fires again and
    // the user would be left holding our dictation.
    ClipboardScope scope;
    KD_REQUIRE(scope.ok());

    KD_REQUIRE(set_raw_text(L"the user's own copy"));

    ClipboardGuardState st;
    WatchGuard guard;  // declared last, so it is destroyed first and never writes into a dead st
    KD_REQUIRE(arm_paste(st, "text mid-paste at shutdown", GetCurrentProcessId(),
                         10, 60000));

    KD_CHECK(kd::finish_pending_clipboard_restore());
    KD_CHECK(st.restored);
    KD_CHECK(!st.restore_pending);
    KD_CHECK(!kd::injector_detail::paste_watch_active());
    // Nothing outstanding, nothing to do, and it says so rather than pretending.
    KD_CHECK(!kd::finish_pending_clipboard_restore());

    std::wstring now;
    bool present = false;
    KD_REQUIRE(get_raw_text(now, present));
    KD_CHECK(present);
    KD_CHECK(now == L"the user's own copy");
}
