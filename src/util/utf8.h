#pragma once
//
// UTF-8: validation, code-point iteration, grapheme-cluster boundaries, UTF-16 conversion,
// grapheme-wise longest common prefix, and word-boundary retraction.
//
// Implements: plan.md §1.2 ("Text and offsets are UTF-8 end to end"), plan.md §1.4 steps 2–4
// (longest common prefix on grapheme clusters, retraction by clusters and to a word boundary),
// and docs/protocol.md §8.2/§8.4 (`stable_bytes` must land on a grapheme-cluster boundary).
//
// ─── The two rules this header exists to enforce ────────────────────────────────────────────
//
// 1. **Every offset in this program is a UTF-8 byte offset that lands on a grapheme-cluster
//    boundary.** Not a code-point index, not a UTF-16 code unit index, not a "character"
//    count. docs/protocol.md §8.2 is normative; revision 1 of the plan called this
//    `stable_chars` and was ambiguous between all four, which is the defect this naming fixes.
//
// 2. **This header is the only place the UTF-16 boundary is crossed.** Windows is a UTF-16 API
//    surface (`SendInput` with `KEYEVENTF_UNICODE`, DirectWrite, `CreateFileW`, clipboard
//    `CF_UNICODETEXT`), so the conversion has to happen somewhere. It happens here, at the
//    edge, and nothing above the Win32 call sites stores a std::wstring. Ad-hoc
//    `MultiByteToWideChar` calls sprinkled through injector.cpp and overlay.cpp are how a
//    codebase ends up with three subtly different answers to "what is a character".
//
// ─── Malformed input is never repaired ──────────────────────────────────────────────────────
//
// No U+FFFD substitution, anywhere, ever. docs/protocol.md §4.1 forbids it, and the reason is
// specific to this program: this text is on its way to a keystroke injector aimed at the user's
// document. A replacement character is a silent corruption the user discovers weeks later by
// reading their own notes. Every function here either succeeds on well-formed input or reports
// exactly where the input went wrong.
//
// "Well-formed" means the Unicode definition, not the permissive one: no overlong encodings, no
// encodings of surrogate code points U+D800–U+DFFF, nothing above U+10FFFF, no truncated
// sequences.
//
// ─── The grapheme approximation, declared up front ──────────────────────────────────────────
//
// There is no ICU here and there will not be (no third-party C++ dependencies, plan.md §5.3),
// and a full UAX #29 implementation with generated property tables is a Phase 2 item, not a
// Phase 0 one. What ships now is a deliberate, documented **approximation**. It is defined by
// what it refuses to split, and it refuses to split:
//
//   * a UTF-8 multi-byte sequence;
//   * a UTF-16 surrogate pair (relevant only inside utf16 conversion, but stated for symmetry);
//   * a run of combining marks — any code point with Grapheme_Extend=Yes, plus
//     Grapheme_Cluster_Break=SpacingMark, never begins a cluster;
//   * a ZWJ (U+200D) sequence — UAX #29 rule GB11, which is what holds 👨‍👩‍👧 together;
//   * a regional-indicator pair — UAX #29 rules GB12/GB13, which is what holds 🇩🇪 together;
//   * a variation selector (U+FE00–U+FE0F, U+E0100–U+E01EF) or the combining enclosing keycap
//     U+20E3 away from the base it modifies;
//   * a CR LF pair (GB3).
//
// docs/protocol.md §8.4 states this same list as the normative minimum for a *receiver's*
// boundary check, and fixture `013-offset-splits-combining-sequence` pins exactly these cases.
// So the approximation is not a shortcut below the spec — it is the spec's stated floor.
//
// Hangul syllable composition (GB6–GB8, L/V/T/LV/LVT) **is** modelled — the rules are four
// comparisons against jamo ranges, and the alternative reports a decomposed syllable as three
// clusters. An earlier draft of this header listed it as a known gap; it is not one.
//
// What it gets **wrong**, stated honestly rather than discovered later:
//   * `Prepend` (GB9b) is not modelled. The set is small and script-specific, and the failure
//     is an over-eager split of a Prepend sequence — safe in the same direction as the rest.
//   * Indic conjunct clusters (GB9c, added in Unicode 15.1) are not modelled, with the same
//     conservative failure direction.
//   * The `Grapheme_Extend` / `SpacingMark` / `Extended_Pictographic` tables are hand-written
//     range lists covering the common scripts, not the generated properties. A code point
//     missing from them splits a cluster it should not — again, a delayed commit, not corrupt
//     text — which is why every judgement call in utf8.cpp errs toward over-inclusion.
//   * Emoji modifier sequences (skin tone) are covered incidentally, because the modifiers
//     U+1F3FB–U+1F3FF carry Grapheme_Extend=Yes.
//   * `Extended_Pictographic` is approximated by a small set of code-point ranges rather than a
//     generated table, so GB11 is applied slightly more broadly than UAX #29 specifies. Again
//     conservative: it can merge two adjacent emoji into one cluster, never split one.
//
// **Open question, recorded rather than guessed** (mirrors docs/protocol.md §14 item 2): which
// Unicode version's property tables both endpoints pin. The Python side's `unicodedata` tracks
// the CPython build's Unicode version, which is not under our control and will drift from a
// hand-maintained C++ table. Until that is decided, the ranges below must be generated from one
// source file into both languages, and the version recorded in this header. Nothing in this
// file may be hand-edited to "fix" a single character without regenerating the other side.
//
// ─── Threading ──────────────────────────────────────────────────────────────────────────────
//
// Every function here is pure, reentrant, allocation-free unless it returns a container, and
// safe to call from any thread including the WASAPI capture thread and the low-level keyboard
// hook. No global state, no locks, no Windows headers.
//
// ─── Convention ─────────────────────────────────────────────────────────────────────────────
//
// `std::string_view` in, byte offsets out. Offsets are `size_t` and are always relative to the
// start of the view passed in. `kNpos` is the "no such position" answer; it is never a valid
// offset because 0 always is.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace kd::utf8 {

inline constexpr size_t kNpos = static_cast<size_t>(-1);

// The Unicode version the tables in utf8.cpp were generated against. Must match the version
// recorded on the Python side. See the open question above.
inline constexpr const char* kUnicodeVersion = "unspecified-pending-protocol-md-14.2";

// ─── Validation ─────────────────────────────────────────────────────────────────────────────

// True if `s` is well-formed UTF-8 by the strict Unicode definition.
//
// TWO DIFFERENT OFFSETS, because two callers want different things and conflating them is an
// interop bug rather than a matter of taste:
//
//   `first_bad_offset` — the byte that could not begin or continue a valid sequence. This is the
//   diagnostic offset: for "\xE0\x80\xAF" it is 1, because the lead is legal and byte 1 is what
//   makes the sequence overlong. Precise, and what a human debugging an encoding fault wants.
//
//   `bad_sequence_start` — the first byte of the malformed sequence: 0 for that same input. THIS
//   is what docs/protocol.md's `bad_utf8` reports as `at_offset`, as fixture
//   008-invalid-utf8-control pins: a payload ending "caf\xE9\"" reports the offset of the 0xE9
//   lead (72), not of the 0x22 that failed to continue it (73).
//
// The two coincide whenever the lead byte itself is invalid, which is why the distinction is easy
// to miss — it shows up only on a truncated or overlong sequence whose lead is well-formed.
// An earlier version of this comment claimed `first_bad_offset` was the wire offset; it is not,
// and the C++ endpoint reported `at_offset` one byte high until fixture 008 was run against it.
bool validate(std::string_view s, size_t* first_bad_offset = nullptr,
              size_t* bad_sequence_start = nullptr) noexcept;

// ─── Code-point level ───────────────────────────────────────────────────────────────────────

// Number of bytes in the sequence beginning with `lead`, or 0 if `lead` cannot begin one
// (i.e. it is a continuation byte 0x80–0xBF, or one of the never-legal bytes 0xC0, 0xC1,
// 0xF5–0xFF).
int sequence_length(uint8_t lead) noexcept;

// Decode the code point starting at `pos`. Returns false if `pos` is out of range or the
// sequence is malformed; on success writes the code point and its byte length.
bool decode(std::string_view s, size_t pos, char32_t& cp, size_t& len) noexcept;

// True if `off` is 0, s.size(), or the start of a UTF-8 sequence. This is the *weak* check —
// docs/protocol.md §8.2 requirement 2 is explicit that it is necessary but not sufficient, and
// fixture 013 is built entirely from offsets that pass this and fail is_grapheme_boundary().
bool is_code_point_boundary(std::string_view s, size_t off) noexcept;

// Offset of the next / previous code-point boundary. Returns kNpos if there is none (i.e.
// next() at s.size(), prev() at 0) or if the input is malformed at that point.
size_t next_code_point(std::string_view s, size_t off) noexcept;
size_t prev_code_point(std::string_view s, size_t off) noexcept;

// Number of code points. Returns kNpos if `s` is not well-formed.
size_t count_code_points(std::string_view s) noexcept;

// ─── Grapheme-cluster level (the approximation declared above) ──────────────────────────────

// True if `off` is a boundary between extended grapheme clusters — including 0 and s.size().
// False for any offset inside a sequence, inside a cluster, or past the end.
//
// This is the predicate docs/protocol.md §8.2 requirement 2 imposes on `stable_bytes`, and the
// one the `text`-kind fixtures (012, 013) call `is_valid_stable_offset`; see
// kd::proto::is_valid_stable_offset in protocol.h, which is this function plus the range check.
bool is_grapheme_boundary(std::string_view s, size_t off) noexcept;

// Offset of the next / previous grapheme-cluster boundary strictly after / before `off`.
// `off` need not itself be a boundary; if it is not, next_grapheme() returns the first boundary
// after it and prev_grapheme() the first before it. Returns kNpos when there is none.
size_t next_grapheme(std::string_view s, size_t off) noexcept;
size_t prev_grapheme(std::string_view s, size_t off) noexcept;

// Snap `off` down to the nearest grapheme-cluster boundary at or before it. Never moves
// forward: a commit engine that rounded *up* would commit a byte the model has not stabilised.
// Returns 0 when there is no boundary at or before `off` (only possible for malformed input).
size_t floor_grapheme(std::string_view s, size_t off) noexcept;

// Number of grapheme clusters. Returns kNpos if `s` is not well-formed.
size_t count_graphemes(std::string_view s) noexcept;

// ─── Prefix and retraction (plan.md §1.4 steps 2–4) ─────────────────────────────────────────

// Byte length of the longest common prefix of `a` and `b`, truncated to a grapheme-cluster
// boundary. This is step 2 of the stability algorithm and the reason it must not be a plain
// memcmp prefix: two hypotheses that differ only in a combining mark share a byte prefix that
// ends *inside* a cluster, and committing that prefix would type a base letter whose accent the
// model is still revising.
size_t common_grapheme_prefix(std::string_view a, std::string_view b) noexcept;

// The N-way form, for the general case of plan.md §1.4 step 1 (keep the last N hypotheses,
// default N = 2, but N is a tunable in the experiment harness). Returns a **byte length** that
// is a grapheme-cluster boundary in *every* input — not in the first one only, which is the
// subtle way to get this wrong when two hypotheses diverge inside a cluster.
//
// `count == 0` returns 0; `count == 1` returns items[0].size(). A raw pointer and a count rather
// than a container, so the caller may pass a std::vector, a std::array or a stack buffer without
// this header taking a dependency on any of them.
size_t common_grapheme_prefix(const std::string_view* items, size_t count) noexcept;

// Move `off` back by `clusters` grapheme clusters, clamping at 0. This is step 3 — "retract to
// at least M clusters behind the decode head", default M = 3, because the tail of a hypothesis
// is where revision concentrates.
size_t retract_graphemes(std::string_view s, size_t off, size_t clusters) noexcept;

// True for code points this project treats as a word separator: any Unicode whitespace, plus
// the terminal punctuation that reliably ends a word in dictated text
// (. , ; : ! ? … and their fullwidth/CJK equivalents).
//
// Deliberately does not include '.' -adjacent identifier punctuation cases: "llama.cpp" is the
// motivating example in plan.md §4, and a policy that treated the dot inside it as a word end
// would commit "llama." and then have to append "cpp" — which is exactly right for a sentence
// and exactly wrong for an identifier. The implementation therefore treats a separator as a
// separator only when it is followed by whitespace or by end of text; see the note in utf8.cpp.
bool is_word_separator(char32_t cp) noexcept;

// True for code points with Unicode White_Space=Yes.
bool is_whitespace(char32_t cp) noexcept;

// Step 4 of the stability algorithm, plus the trailing-whitespace hold-back rule.
//
// Returns the largest offset r ≤ `offset` such that:
//   * r is a grapheme-cluster boundary, and
//   * r == 0, or the code point ending at r is a word separator (so we never stop inside a
//     word — a revised word ending is the most common revision there is), and
//   * text[0, r) does not end with whitespace (trailing whitespace is held back, so a following
//     punctuation revision does not require rewriting across a committed space — plan.md §1.4).
//
// Worked examples, which are also the unit tests:
//   ("hello world",     11) -> 5   ("hello"; the space is held back)
//   ("hello world",      5) -> 0   (offset is mid-"world"'s predecessor: no separator below it)
//   ("Hi. There",        9) -> 3   ("Hi."; punctuation is kept, the space is held back)
//   ("hello",            5) -> 0   (no separator anywhere: nothing is safe to commit yet)
//   ("hello ",           6) -> 5   (the trailing space is held back)
size_t retract_to_word_boundary(std::string_view text, size_t offset) noexcept;

// Offset of the end of `text[0, offset)` with any trailing whitespace run removed.
size_t trim_trailing_whitespace(std::string_view text, size_t offset) noexcept;

// ─── UTF-16 conversion — the only place the boundary is crossed ─────────────────────────────
//
// Pure C++, not MultiByteToWideChar. Two reasons: it keeps this module unit-testable with no
// Windows headers, and MB_ERR_INVALID_CHARS handling differs subtly between Windows versions
// in ways that would make "well-formed" mean something different here than it means on the
// wire. Malformed input is rejected, never substituted.

// UTF-8 -> UTF-16. Returns false and leaves `out` unspecified if `s` is not well-formed; on
// false, `first_bad_offset` (if non-null) receives the offending byte offset.
bool to_utf16(std::string_view s, std::wstring& out, size_t* first_bad_offset = nullptr);

// UTF-16 -> UTF-8. Returns false if `s` contains an unpaired surrogate. Windows APIs can and do
// hand back unpaired surrogates (a file name written by a broken tool, a clipboard payload from
// another process); rejecting them here is what stops one entering the transcript stream, where
// it would become a `bad_utf8` frame at the far end of the pipe.
bool from_utf16(std::wstring_view s, std::string& out, size_t* first_bad_index = nullptr);

// Convenience wrappers for call sites that have already validated. Return an empty string on
// malformed input; use the checked forms anywhere the input came from outside this process.
std::wstring to_utf16_or_empty(std::string_view s);
std::string from_utf16_or_empty(std::wstring_view s);

}  // namespace kd::utf8
