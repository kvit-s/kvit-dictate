// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Suite `utf8` — the text layer the committed-prefix invariant rests on (plan.md §1.4,
// docs/protocol.md §8.2/§8.4).
//
// Two things about how this file is written, because both are deliberate:
//
//   * **Every literal is hex escapes, spliced with adjacent-string concatenation.** Not because
//     the compiler cannot handle UTF-8 source (it is built /utf-8), but because a test about
//     byte offsets that spells its input as "é" is a test whose expected offsets cannot be
//     checked by reading it. The splicing is also a correctness requirement: `\x` escapes are
//     maximal-munch in C++, so "\xCC\x81llo" would be read as one escape "\xCC81l" — writing
//     "\xCC\x81" "llo" is the fix, not a style choice.
//   * **The boundary tests walk every offset**, including the ones that are obviously fine and
//     the one past the end. Fixture 013 exists because a check that samples "interesting"
//     offsets is exactly the check that passes while the predicate is wrong.

#include <string>

#include "kd_test.h"
#include "util/utf8.h"

namespace {

using kd::utf8::kNpos;

// Fixture 012's text: "café ☕" — 63 61 66 | c3 a9 | 20 | e2 98 95, nine bytes.
const std::string kCafe = "caf" "\xC3\xA9" " " "\xE2\x98\x95";

// Fixture 013's text: "é 👨‍👩‍👧 🇩🇪 #️⃣" where the é is decomposed (e + U+0301).
// Clusters: [0,3) [3,4) [4,22) [22,23) [23,31) [31,32) [32,39).
const std::string kMixed =
    "\x65\xCC\x81"                                                              // e + U+0301
    "\x20"                                                                      // space
    "\xF0\x9F\x91\xA8" "\xE2\x80\x8D" "\xF0\x9F\x91\xA9" "\xE2\x80\x8D" "\xF0\x9F\x91\xA7"
    "\x20"                                                                      // space
    "\xF0\x9F\x87\xA9" "\xF0\x9F\x87\xAA"                                       // 🇩🇪
    "\x20"                                                                      // space
    "\x23" "\xEF\xB8\x8F" "\xE2\x83\xA3";                                       // # VS16 keycap

// Checks the boundary predicate at *every* offset from 0 to size()+1 inclusive, so that both a
// false positive and a false negative are caught, and reports the offending offset by number.
void check_boundaries(std::string_view s, const size_t* expected, size_t n_expected) {
    for (size_t off = 0; off <= s.size() + 1; ++off) {
        bool expect = false;
        for (size_t i = 0; i < n_expected; ++i) {
            if (expected[i] == off) {
                expect = true;
                break;
            }
        }
        const bool got = kd::utf8::is_grapheme_boundary(s, off);
        if (got == expect) {
            KD_CHECK(true);
        } else {
            KD_FAIL(std::string("offset ") + std::to_string(off) + ": expected " +
                    (expect ? "a boundary" : "no boundary") + ", got " +
                    (got ? "a boundary" : "no boundary"));
        }
    }
}

std::wstring wide(std::initializer_list<uint16_t> units) {
    // Built unit by unit rather than from a wide literal: L"\xD83D" followed by anything hex-ish
    // munches wrong, and an unpaired surrogate in a source literal is not portable anyway.
    std::wstring w;
    for (uint16_t u : units) w.push_back(static_cast<wchar_t>(u));
    return w;
}

}  // namespace

// ═══ validate() — one test per rejection class, each pinning first_bad_offset ═══════════════

KD_TEST(utf8, validate_accepts_well_formed) {
    KD_CHECK(kd::utf8::validate(""));
    KD_CHECK(kd::utf8::validate("plain ascii"));
    KD_CHECK(kd::utf8::validate(kCafe));
    KD_CHECK(kd::utf8::validate(kMixed));
    KD_CHECK(kd::utf8::validate("\xC2\x80"));          // U+0080, shortest 2-byte form
    KD_CHECK(kd::utf8::validate("\xDF\xBF"));          // U+07FF
    KD_CHECK(kd::utf8::validate("\xE0\xA0\x80"));      // U+0800, shortest 3-byte form
    KD_CHECK(kd::utf8::validate("\xEF\xBF\xBF"));      // U+FFFF — a noncharacter, but well-formed
    KD_CHECK(kd::utf8::validate("\xF0\x90\x80\x80"));  // U+10000, shortest 4-byte form
    KD_CHECK(kd::utf8::validate("\xF4\x8F\xBF\xBF"));  // U+10FFFF, the last code point
}

KD_TEST(utf8, validate_rejects_overlong) {
    size_t bad = kNpos;
    KD_CHECK(!kd::utf8::validate("\xC0\xAF", &bad));  // overlong '/'
    KD_CHECK_EQ(bad, size_t{0});                      // 0xC0 can never be a lead byte at all

    bad = kNpos;
    KD_CHECK(!kd::utf8::validate("\xC1\xBF", &bad));
    KD_CHECK_EQ(bad, size_t{0});

    bad = kNpos;
    KD_CHECK(!kd::utf8::validate("\xE0\x80\xAF", &bad));  // overlong 3-byte
    KD_CHECK_EQ(bad, size_t{1});  // the lead is legal; the byte that makes it overlong is at 1

    bad = kNpos;
    KD_CHECK(!kd::utf8::validate("\xE0\x9F\xBF", &bad));  // U+07FF written in three bytes
    KD_CHECK_EQ(bad, size_t{1});

    bad = kNpos;
    KD_CHECK(!kd::utf8::validate("\xF0\x80\x80\xAF", &bad));  // overlong 4-byte
    KD_CHECK_EQ(bad, size_t{1});

    bad = kNpos;
    KD_CHECK(!kd::utf8::validate("\xF0\x8F\xBF\xBF", &bad));  // U+FFFF written in four bytes
    KD_CHECK_EQ(bad, size_t{1});
}

KD_TEST(utf8, validate_rejects_utf8_encoded_surrogates) {
    // CESU-8 / WTF-8 input. Accepting these is how a lone surrogate reaches SendInput.
    size_t bad = kNpos;
    KD_CHECK(!kd::utf8::validate("\xED\xA0\x80", &bad));  // U+D800
    KD_CHECK_EQ(bad, size_t{1});

    bad = kNpos;
    KD_CHECK(!kd::utf8::validate("\xED\xBF\xBF", &bad));  // U+DFFF
    KD_CHECK_EQ(bad, size_t{1});

    bad = kNpos;
    KD_CHECK(!kd::utf8::validate("\xED\xA0\xBD\xED\xB8\x80", &bad));  // surrogate pair as CESU-8
    KD_CHECK_EQ(bad, size_t{1});

    // U+D7FF and U+E000, either side of the surrogate block, must still pass.
    KD_CHECK(kd::utf8::validate("\xED\x9F\xBF"));
    KD_CHECK(kd::utf8::validate("\xEE\x80\x80"));
}

KD_TEST(utf8, validate_rejects_above_max_code_point) {
    size_t bad = kNpos;
    KD_CHECK(!kd::utf8::validate("\xF4\x90\x80\x80", &bad));  // U+110000
    KD_CHECK_EQ(bad, size_t{1});

    bad = kNpos;
    KD_CHECK(!kd::utf8::validate("\xF5\x80\x80\x80", &bad));  // lead is never legal
    KD_CHECK_EQ(bad, size_t{0});

    bad = kNpos;
    KD_CHECK(!kd::utf8::validate("\xF8\x88\x80\x80\x80", &bad));  // 5-byte form, never legal
    KD_CHECK_EQ(bad, size_t{0});

    bad = kNpos;
    KD_CHECK(!kd::utf8::validate("\xFE", &bad));
    KD_CHECK_EQ(bad, size_t{0});
    bad = kNpos;
    KD_CHECK(!kd::utf8::validate("\xFF", &bad));
    KD_CHECK_EQ(bad, size_t{0});
}

KD_TEST(utf8, validate_rejects_truncated_and_stray_continuations) {
    // A sequence cut off by the end of the buffer has no offending byte, so the reported offset
    // is the lead's — which is the offset a caller must rewind to, and what docs/protocol.md
    // §11.3 `bad_utf8` should carry.
    size_t bad = kNpos;
    KD_CHECK(!kd::utf8::validate("abc\xC3", &bad));
    KD_CHECK_EQ(bad, size_t{3});

    bad = kNpos;
    KD_CHECK(!kd::utf8::validate("\xE2\x82", &bad));
    KD_CHECK_EQ(bad, size_t{0});

    bad = kNpos;
    KD_CHECK(!kd::utf8::validate("\xF0\x9F\x92", &bad));
    KD_CHECK_EQ(bad, size_t{0});

    // Continuation byte where a lead belongs.
    bad = kNpos;
    KD_CHECK(!kd::utf8::validate("\x80", &bad));
    KD_CHECK_EQ(bad, size_t{0});
    bad = kNpos;
    KD_CHECK(!kd::utf8::validate("ok\xBFno", &bad));
    KD_CHECK_EQ(bad, size_t{2});

    // Lead byte followed by a non-continuation.
    bad = kNpos;
    KD_CHECK(!kd::utf8::validate("\xE2\x28\xA1", &bad));
    KD_CHECK_EQ(bad, size_t{1});
    bad = kNpos;
    KD_CHECK(!kd::utf8::validate("\xF0\x9F\x92\x28", &bad));
    KD_CHECK_EQ(bad, size_t{3});
}

// ═══ code-point level ═══════════════════════════════════════════════════════════════════════

KD_TEST(utf8, sequence_length_classifies_lead_bytes) {
    KD_CHECK_EQ(kd::utf8::sequence_length(0x00), 1);
    KD_CHECK_EQ(kd::utf8::sequence_length(0x7F), 1);
    KD_CHECK_EQ(kd::utf8::sequence_length(0x80), 0);
    KD_CHECK_EQ(kd::utf8::sequence_length(0xBF), 0);
    KD_CHECK_EQ(kd::utf8::sequence_length(0xC0), 0);  // always overlong
    KD_CHECK_EQ(kd::utf8::sequence_length(0xC1), 0);  // always overlong
    KD_CHECK_EQ(kd::utf8::sequence_length(0xC2), 2);
    KD_CHECK_EQ(kd::utf8::sequence_length(0xDF), 2);
    KD_CHECK_EQ(kd::utf8::sequence_length(0xE0), 3);
    KD_CHECK_EQ(kd::utf8::sequence_length(0xEF), 3);
    KD_CHECK_EQ(kd::utf8::sequence_length(0xF0), 4);
    KD_CHECK_EQ(kd::utf8::sequence_length(0xF4), 4);
    KD_CHECK_EQ(kd::utf8::sequence_length(0xF5), 0);
    KD_CHECK_EQ(kd::utf8::sequence_length(0xFF), 0);
}

KD_TEST(utf8, decode_reads_each_sequence_length) {
    char32_t cp = 0;
    size_t len = 0;

    KD_REQUIRE(kd::utf8::decode("a", 0, cp, len));
    KD_CHECK_EQ(static_cast<uint32_t>(cp), uint32_t{0x61});
    KD_CHECK_EQ(len, size_t{1});

    KD_REQUIRE(kd::utf8::decode(kCafe, 3, cp, len));
    KD_CHECK_EQ(static_cast<uint32_t>(cp), uint32_t{0x00E9});
    KD_CHECK_EQ(len, size_t{2});

    KD_REQUIRE(kd::utf8::decode(kCafe, 6, cp, len));
    KD_CHECK_EQ(static_cast<uint32_t>(cp), uint32_t{0x2615});
    KD_CHECK_EQ(len, size_t{3});

    KD_REQUIRE(kd::utf8::decode("\xF0\x9F\x98\x80", 0, cp, len));
    KD_CHECK_EQ(static_cast<uint32_t>(cp), uint32_t{0x1F600});
    KD_CHECK_EQ(len, size_t{4});

    // Mid-sequence and past-the-end both fail rather than resynchronising.
    KD_CHECK(!kd::utf8::decode(kCafe, 4, cp, len));
    KD_CHECK(!kd::utf8::decode(kCafe, kCafe.size(), cp, len));
}

KD_TEST(utf8, code_point_boundaries_and_navigation) {
    // The weak check: true at 4? No — 4 is inside c3 a9. But 7 and 8 are also inside a sequence,
    // which is what fixture 012 pins.
    KD_CHECK(kd::utf8::is_code_point_boundary(kCafe, 0));
    KD_CHECK(kd::utf8::is_code_point_boundary(kCafe, 3));
    KD_CHECK(!kd::utf8::is_code_point_boundary(kCafe, 4));
    KD_CHECK(kd::utf8::is_code_point_boundary(kCafe, 5));
    KD_CHECK(kd::utf8::is_code_point_boundary(kCafe, 6));
    KD_CHECK(!kd::utf8::is_code_point_boundary(kCafe, 7));
    KD_CHECK(!kd::utf8::is_code_point_boundary(kCafe, 8));
    KD_CHECK(kd::utf8::is_code_point_boundary(kCafe, 9));   // end of text
    KD_CHECK(!kd::utf8::is_code_point_boundary(kCafe, 10));  // past the end

    KD_CHECK_EQ(kd::utf8::next_code_point(kCafe, 3), size_t{5});
    KD_CHECK_EQ(kd::utf8::next_code_point(kCafe, 6), size_t{9});
    KD_CHECK_EQ(kd::utf8::next_code_point(kCafe, 9), kNpos);
    KD_CHECK_EQ(kd::utf8::next_code_point(kCafe, 4), kNpos);  // mid-sequence

    KD_CHECK_EQ(kd::utf8::prev_code_point(kCafe, 9), size_t{6});
    KD_CHECK_EQ(kd::utf8::prev_code_point(kCafe, 5), size_t{3});
    KD_CHECK_EQ(kd::utf8::prev_code_point(kCafe, 0), kNpos);
    KD_CHECK_EQ(kd::utf8::prev_code_point(kCafe, 4), kNpos);  // mid-sequence

    KD_CHECK_EQ(kd::utf8::count_code_points(kCafe), size_t{6});
    KD_CHECK_EQ(kd::utf8::count_code_points(""), size_t{0});
    KD_CHECK_EQ(kd::utf8::count_code_points("\xC0\xAF"), kNpos);
}

// ═══ grapheme clusters ══════════════════════════════════════════════════════════════════════

KD_TEST(utf8, boundary_predicate_matches_fixture_012) {
    // Every legal offset in "café ☕". 4, 7 and 8 split sequences; 10 is past the end.
    const size_t expected[] = {0, 1, 2, 3, 5, 6, 9};
    check_boundaries(kCafe, expected, sizeof(expected) / sizeof(expected[0]));
}

KD_TEST(utf8, boundary_predicate_matches_fixture_013) {
    // The whole point: every rejected offset here is a valid *code-point* boundary. A UTF-8-only
    // check passes all of them, which is why fixture 012 proves nothing about this one.
    KD_REQUIRE_EQ(kMixed.size(), size_t{39});
    const size_t expected[] = {0, 3, 4, 22, 23, 31, 32, 39};
    check_boundaries(kMixed, expected, sizeof(expected) / sizeof(expected[0]));

    // ...and the offsets fixture 013 names explicitly, restated so a failure says which rule.
    KD_CHECK(!kd::utf8::is_grapheme_boundary(kMixed, 1));   // before the combining acute
    KD_CHECK(kd::utf8::is_code_point_boundary(kMixed, 1));  // but a fine code-point boundary
    KD_CHECK(!kd::utf8::is_grapheme_boundary(kMixed, 8));   // before a ZWJ
    KD_CHECK(!kd::utf8::is_grapheme_boundary(kMixed, 11));  // after a ZWJ (GB11)
    KD_CHECK(!kd::utf8::is_grapheme_boundary(kMixed, 27));  // between paired regional indicators
    KD_CHECK(!kd::utf8::is_grapheme_boundary(kMixed, 33));  // before a variation selector
    KD_CHECK(!kd::utf8::is_grapheme_boundary(kMixed, 36));  // before U+20E3 enclosing keycap

    KD_CHECK_EQ(kd::utf8::count_graphemes(kMixed), size_t{7});
}

KD_TEST(utf8, boundary_predicate_regional_indicator_parity) {
    // Two flags in a row: 🇩🇪🇩🇪. Boundaries at 0, 8 and 16 only. A parity bug that pairs
    // greedily from the left still gets one flag right and this wrong.
    const std::string two_flags =
        "\xF0\x9F\x87\xA9" "\xF0\x9F\x87\xAA" "\xF0\x9F\x87\xA9" "\xF0\x9F\x87\xAA";
    KD_REQUIRE_EQ(two_flags.size(), size_t{16});
    const size_t expected[] = {0, 8, 16};
    check_boundaries(two_flags, expected, sizeof(expected) / sizeof(expected[0]));
    KD_CHECK_EQ(kd::utf8::count_graphemes(two_flags), size_t{2});

    // An odd run: three regional indicators are one flag plus a lone indicator.
    const std::string three = "\xF0\x9F\x87\xA9" "\xF0\x9F\x87\xAA" "\xF0\x9F\x87\xA9";
    const size_t expected3[] = {0, 8, 12};
    check_boundaries(three, expected3, sizeof(expected3) / sizeof(expected3[0]));
}

KD_TEST(utf8, boundary_predicate_crlf_and_controls) {
    const std::string s = "a\r\nb";  // CRLF is one cluster (GB3)
    const size_t expected[] = {0, 1, 3, 4};
    check_boundaries(s, expected, sizeof(expected) / sizeof(expected[0]));
    KD_CHECK_EQ(kd::utf8::count_graphemes(s), size_t{3});

    // GB4/GB5 outrank GB9: a control character is never absorbed into a neighbouring cluster,
    // and "\n" following a combining mark still breaks.
    const std::string t = "e" "\xCC\x81" "\n";
    const size_t expected_t[] = {0, 3, 4};
    check_boundaries(t, expected_t, sizeof(expected_t) / sizeof(expected_t[0]));
}

KD_TEST(utf8, boundary_predicate_emoji_modifier_and_hangul) {
    // 👍 + U+1F3FB (skin tone): one cluster, via Grapheme_Extend.
    const std::string thumbs = "\xF0\x9F\x91\x8D" "\xF0\x9F\x8F\xBB";
    const size_t expected[] = {0, 8};
    check_boundaries(thumbs, expected, sizeof(expected) / sizeof(expected[0]));

    // Decomposed Hangul 각: L U+1100, V U+1161, T U+11A8 — one cluster (GB6-GB8).
    const std::string jamo = "\xE1\x84\x80" "\xE1\x85\xA1" "\xE1\x86\xA8";
    const size_t expected_j[] = {0, 9};
    check_boundaries(jamo, expected_j, sizeof(expected_j) / sizeof(expected_j[0]));
    KD_CHECK_EQ(kd::utf8::count_graphemes(jamo), size_t{1});

    // A precomposed LV syllable followed by a trailing jamo is also one cluster: 가 + U+11A8.
    const std::string lv_t = "\xEA\xB0\x80" "\xE1\x86\xA8";
    KD_CHECK_EQ(kd::utf8::count_graphemes(lv_t), size_t{1});

    // But V after nothing, or L after V, does break.
    const std::string vl = "\xE1\x85\xA1" "\xE1\x84\x80";  // V then L
    KD_CHECK_EQ(kd::utf8::count_graphemes(vl), size_t{2});
}

KD_TEST(utf8, boundary_predicate_refuses_malformed_input) {
    // No repair, no resynchronisation: an offset inside or adjacent to malformed bytes is simply
    // not a commit point.
    const std::string bad = "ab" "\xC3" "cd";  // truncated 2-byte sequence in the middle
    KD_CHECK(kd::utf8::is_grapheme_boundary(bad, 0));
    KD_CHECK(kd::utf8::is_grapheme_boundary(bad, 5));  // end of text is always a boundary
    KD_CHECK(!kd::utf8::is_grapheme_boundary(bad, 2));
    KD_CHECK(!kd::utf8::is_grapheme_boundary(bad, 3));
    KD_CHECK_EQ(kd::utf8::count_graphemes(bad), kNpos);
}

KD_TEST(utf8, grapheme_navigation) {
    KD_CHECK_EQ(kd::utf8::next_grapheme(kMixed, 0), size_t{3});
    KD_CHECK_EQ(kd::utf8::next_grapheme(kMixed, 1), size_t{3});   // from inside a cluster
    KD_CHECK_EQ(kd::utf8::next_grapheme(kMixed, 4), size_t{22});  // over the whole ZWJ sequence
    KD_CHECK_EQ(kd::utf8::next_grapheme(kMixed, 39), kNpos);

    KD_CHECK_EQ(kd::utf8::prev_grapheme(kMixed, 39), size_t{32});
    KD_CHECK_EQ(kd::utf8::prev_grapheme(kMixed, 22), size_t{4});
    KD_CHECK_EQ(kd::utf8::prev_grapheme(kMixed, 2), size_t{0});
    KD_CHECK_EQ(kd::utf8::prev_grapheme(kMixed, 0), kNpos);

    // floor_grapheme never moves forward — rounding up would commit a byte nothing stabilised.
    KD_CHECK_EQ(kd::utf8::floor_grapheme(kMixed, 39), size_t{39});
    KD_CHECK_EQ(kd::utf8::floor_grapheme(kMixed, 38), size_t{32});
    KD_CHECK_EQ(kd::utf8::floor_grapheme(kMixed, 21), size_t{4});
    KD_CHECK_EQ(kd::utf8::floor_grapheme(kMixed, 1), size_t{0});
    KD_CHECK_EQ(kd::utf8::floor_grapheme(kMixed, 999), size_t{39});  // clamps to the end

    KD_CHECK_EQ(kd::utf8::count_graphemes(""), size_t{0});
    KD_CHECK_EQ(kd::utf8::count_graphemes(kCafe), size_t{6});
}

// ═══ plan.md §1.4 step 2 — grapheme-wise longest common prefix ══════════════════════════════

KD_TEST(utf8, common_prefix_stops_before_a_divergent_cluster) {
    // The motivating case from utf8.h: two hypotheses that agree on "e" and disagree on whether
    // an accent follows. The byte prefix is 1; the grapheme prefix must be 0, because committing
    // that byte types an "e" the model is about to turn into "é".
    const std::string with_accent = "e" "\xCC\x81" "llo";
    const std::string plain = "ello";
    KD_CHECK_EQ(kd::utf8::common_grapheme_prefix(with_accent, plain), size_t{0});
    KD_CHECK_EQ(kd::utf8::common_grapheme_prefix(plain, with_accent), size_t{0});

    // Same shape one word in, so the retraction is to a cluster boundary rather than to zero.
    const std::string a = "the cafe" "\xCC\x81" " is";
    const std::string b = "the cafe teria";
    KD_CHECK_EQ(kd::utf8::common_grapheme_prefix(a, b), size_t{7});  // "the caf"

    // An emoji ZWJ sequence that one hypothesis extends: the shared bytes end mid-cluster.
    const std::string fam = "hi " "\xF0\x9F\x91\xA8" "\xE2\x80\x8D" "\xF0\x9F\x91\xA9";
    const std::string man = "hi " "\xF0\x9F\x91\xA8" "!";
    KD_CHECK_EQ(kd::utf8::common_grapheme_prefix(fam, man), size_t{3});
}

KD_TEST(utf8, common_prefix_ordinary_cases) {
    KD_CHECK_EQ(kd::utf8::common_grapheme_prefix("hello world", "hello there"), size_t{6});
    KD_CHECK_EQ(kd::utf8::common_grapheme_prefix("same", "same"), size_t{4});
    KD_CHECK_EQ(kd::utf8::common_grapheme_prefix("prefix", "prefix and more"), size_t{6});
    KD_CHECK_EQ(kd::utf8::common_grapheme_prefix("", "anything"), size_t{0});
    KD_CHECK_EQ(kd::utf8::common_grapheme_prefix("", ""), size_t{0});
    KD_CHECK_EQ(kd::utf8::common_grapheme_prefix("abc", "xyz"), size_t{0});

    // A shared prefix that ends exactly at the end of one input, where the other continues with
    // an extend: the answer is the last boundary common to both, not the shorter length.
    KD_CHECK_EQ(kd::utf8::common_grapheme_prefix("e", "e" "\xCC\x81"), size_t{0});
    KD_CHECK_EQ(kd::utf8::common_grapheme_prefix("ae", "ae" "\xCC\x81"), size_t{1});
}

KD_TEST(utf8, common_prefix_n_way) {
    const std::string_view three[] = {"abcdef", "abcxx", "abz"};
    KD_CHECK_EQ(kd::utf8::common_grapheme_prefix(three, 3), size_t{2});

    const std::string_view one[] = {"only"};
    KD_CHECK_EQ(kd::utf8::common_grapheme_prefix(one, 1), size_t{4});
    KD_CHECK_EQ(kd::utf8::common_grapheme_prefix(one, 0), size_t{0});
    KD_CHECK_EQ(kd::utf8::common_grapheme_prefix(nullptr, 3), size_t{0});

    // One divergent cluster among N poisons the whole answer, as it must: the prefix has to be a
    // boundary in *every* hypothesis, not in the first two.
    const std::string_view mixed[] = {"cafe" "\xCC\x81" " x", "cafe" "\xCC\x81" " y", "cafe z"};
    KD_CHECK_EQ(kd::utf8::common_grapheme_prefix(mixed, 3), size_t{3});
}

// ═══ plan.md §1.4 step 3 — retraction by clusters ═══════════════════════════════════════════

KD_TEST(utf8, retract_graphemes_counts_clusters_not_bytes) {
    KD_CHECK_EQ(kd::utf8::retract_graphemes("hello", 5, 0), size_t{5});
    KD_CHECK_EQ(kd::utf8::retract_graphemes("hello", 5, 3), size_t{2});
    KD_CHECK_EQ(kd::utf8::retract_graphemes("hello", 5, 5), size_t{0});
    KD_CHECK_EQ(kd::utf8::retract_graphemes("hello", 5, 99), size_t{0});  // clamps, never wraps
    KD_CHECK_EQ(kd::utf8::retract_graphemes("", 0, 3), size_t{0});

    // Over multi-byte clusters, three clusters back from the end of kMixed is offset 23, not
    // three bytes and not three code points.
    KD_CHECK_EQ(kd::utf8::retract_graphemes(kMixed, 39, 1), size_t{32});
    KD_CHECK_EQ(kd::utf8::retract_graphemes(kMixed, 39, 3), size_t{23});
    KD_CHECK_EQ(kd::utf8::retract_graphemes(kMixed, 39, 6), size_t{3});

    // A mid-cluster starting offset is floored first, so the caller loses the partial cluster in
    // addition to the requested retraction. Conservative by design.
    KD_CHECK_EQ(kd::utf8::retract_graphemes(kMixed, 38, 1), size_t{31});
}

// ═══ plan.md §1.4 step 4 — word-boundary retraction and the whitespace hold-back ════════════

KD_TEST(utf8, retract_to_word_boundary_header_examples) {
    // The five worked examples from util/utf8.h, verbatim.
    KD_CHECK_EQ(kd::utf8::retract_to_word_boundary("hello world", 11), size_t{5});
    KD_CHECK_EQ(kd::utf8::retract_to_word_boundary("hello world", 5), size_t{0});
    KD_CHECK_EQ(kd::utf8::retract_to_word_boundary("Hi. There", 9), size_t{3});
    KD_CHECK_EQ(kd::utf8::retract_to_word_boundary("hello", 5), size_t{0});
    KD_CHECK_EQ(kd::utf8::retract_to_word_boundary("hello ", 6), size_t{5});
}

KD_TEST(utf8, retract_to_word_boundary_never_splits_an_identifier) {
    // plan.md §4's motivating example. The dot in "llama.cpp" is followed by a letter, so it is
    // not a word end — committing "llama." while the model is still deciding between "cpp",
    // "C P P" and "see plus plus" is exactly the irrevocable error ICER measures.
    KD_CHECK_EQ(kd::utf8::retract_to_word_boundary("the llama.cp", 12), size_t{3});
    KD_CHECK_EQ(kd::utf8::retract_to_word_boundary("change the llama.cpp config", 27), size_t{20});

    // The same dot at the end of the text, or followed by a space, does end a word.
    KD_CHECK_EQ(kd::utf8::retract_to_word_boundary("Hi.", 3), size_t{3});
    KD_CHECK_EQ(kd::utf8::retract_to_word_boundary("Hi. T", 5), size_t{3});
}

KD_TEST(utf8, retract_to_word_boundary_edges) {
    KD_CHECK_EQ(kd::utf8::retract_to_word_boundary("", 0), size_t{0});
    KD_CHECK_EQ(kd::utf8::retract_to_word_boundary("word", 0), size_t{0});

    // No separator anywhere: nothing is safe to commit, and the answer is 0 rather than "as much
    // as we can get away with".
    KD_CHECK_EQ(kd::utf8::retract_to_word_boundary("supercalifragilistic", 20), size_t{0});

    // All whitespace: the hold-back rule leaves nothing.
    KD_CHECK_EQ(kd::utf8::retract_to_word_boundary("   ", 3), size_t{0});
    KD_CHECK_EQ(kd::utf8::retract_to_word_boundary("\r\n\t ", 4), size_t{0});

    // A whole run of trailing whitespace is held back, not just the last space.
    KD_CHECK_EQ(kd::utf8::retract_to_word_boundary("hello   ", 8), size_t{5});
    KD_CHECK_EQ(kd::utf8::retract_to_word_boundary("hello,   world", 14), size_t{6});

    // Multi-byte text: the returned offset is a byte offset on a cluster boundary.
    KD_CHECK_EQ(kd::utf8::retract_to_word_boundary("caf" "\xC3\xA9" " bar", 9), size_t{5});
    // ...and the boundary is a *cluster* boundary, so a space carrying a combining mark (yes,
    // that is legal) is not a place to stop.
    const std::string spooky = "ab" " " "\xCC\x81" "cd";  // space + combining acute
    KD_CHECK_EQ(kd::utf8::retract_to_word_boundary(spooky, spooky.size()), size_t{0});

    // Offsets past the end clamp rather than reading out of bounds.
    KD_CHECK_EQ(kd::utf8::retract_to_word_boundary("hello world", 999), size_t{5});
}

KD_TEST(utf8, trim_trailing_whitespace_and_classifiers) {
    KD_CHECK_EQ(kd::utf8::trim_trailing_whitespace("hello   ", 8), size_t{5});
    KD_CHECK_EQ(kd::utf8::trim_trailing_whitespace("hello", 5), size_t{5});
    KD_CHECK_EQ(kd::utf8::trim_trailing_whitespace("   ", 3), size_t{0});
    KD_CHECK_EQ(kd::utf8::trim_trailing_whitespace("", 0), size_t{0});
    KD_CHECK_EQ(kd::utf8::trim_trailing_whitespace("hello   ", 6), size_t{5});  // respects offset
    KD_CHECK_EQ(kd::utf8::trim_trailing_whitespace("a" "\xC2\xA0", 3), size_t{1});  // NBSP
    KD_CHECK_EQ(kd::utf8::trim_trailing_whitespace("a" "\xE3\x80\x80", 4), size_t{1});  // U+3000

    KD_CHECK(kd::utf8::is_whitespace(U' '));
    KD_CHECK(kd::utf8::is_whitespace(U'\t'));
    KD_CHECK(kd::utf8::is_whitespace(U'\n'));
    KD_CHECK(kd::utf8::is_whitespace(0x00A0));
    KD_CHECK(kd::utf8::is_whitespace(0x3000));
    KD_CHECK(!kd::utf8::is_whitespace(U'a'));
    KD_CHECK(!kd::utf8::is_whitespace(0x200B));  // ZWSP is not White_Space=Yes

    KD_CHECK(kd::utf8::is_word_separator(U'.'));
    KD_CHECK(kd::utf8::is_word_separator(U','));
    KD_CHECK(kd::utf8::is_word_separator(U'!'));
    KD_CHECK(kd::utf8::is_word_separator(U'?'));
    KD_CHECK(kd::utf8::is_word_separator(U';'));
    KD_CHECK(kd::utf8::is_word_separator(U':'));
    KD_CHECK(kd::utf8::is_word_separator(0x2026));  // …
    KD_CHECK(kd::utf8::is_word_separator(0x3002));  // 。
    KD_CHECK(kd::utf8::is_word_separator(U' '));
    // Not separators: an apostrophe splits "don't", a hyphen splits "well-known", quotes and
    // brackets attach to the word they surround.
    KD_CHECK(!kd::utf8::is_word_separator(U'\''));
    KD_CHECK(!kd::utf8::is_word_separator(U'-'));
    KD_CHECK(!kd::utf8::is_word_separator(U'"'));
    KD_CHECK(!kd::utf8::is_word_separator(U')'));
    KD_CHECK(!kd::utf8::is_word_separator(U'a'));
}

// ═══ the UTF-16 boundary ════════════════════════════════════════════════════════════════════

KD_TEST(utf8, utf16_round_trip_including_astral) {
    const std::string inputs[] = {"", "plain ascii", kCafe, kMixed,
                                  "\xF0\x9F\x98\x80" " and " "\xF4\x8F\xBF\xBF"};
    for (const std::string& in : inputs) {
        std::wstring w;
        KD_REQUIRE(kd::utf8::to_utf16(in, w));
        std::string back;
        KD_REQUIRE(kd::utf8::from_utf16(w, back));
        KD_CHECK_EQ(back, in);
        // No repair, anywhere: a U+FFFD in the output would mean a code point was swallowed.
        KD_CHECK_EQ(back.find("\xEF\xBF\xBD"), std::string::npos);
    }
}

KD_TEST(utf8, utf16_encodes_surrogate_pairs) {
    std::wstring w;
    KD_REQUIRE(kd::utf8::to_utf16("\xF0\x9F\x98\x80", w));  // U+1F600
    KD_REQUIRE_EQ(w.size(), size_t{2});
    KD_CHECK_EQ(static_cast<uint32_t>(static_cast<uint16_t>(w[0])), uint32_t{0xD83D});
    KD_CHECK_EQ(static_cast<uint32_t>(static_cast<uint16_t>(w[1])), uint32_t{0xDE00});

    // The astral boundary in both directions: U+FFFF is one unit, U+10000 is two.
    KD_REQUIRE(kd::utf8::to_utf16("\xEF\xBF\xBF", w));
    KD_CHECK_EQ(w.size(), size_t{1});
    KD_REQUIRE(kd::utf8::to_utf16("\xF0\x90\x80\x80", w));
    KD_CHECK_EQ(w.size(), size_t{2});

    std::string s;
    KD_REQUIRE(kd::utf8::from_utf16(wide({0xD83D, 0xDE00}), s));
    KD_CHECK_EQ(s, std::string("\xF0\x9F\x98\x80"));
}

KD_TEST(utf8, utf16_rejects_malformed_in_both_directions) {
    std::wstring w;
    size_t bad = kNpos;
    KD_CHECK(!kd::utf8::to_utf16("ok " "\xED\xA0\x80", w, &bad));  // surrogate encoded in UTF-8
    KD_CHECK_EQ(bad, size_t{4});
    bad = kNpos;
    KD_CHECK(!kd::utf8::to_utf16("ok " "\xC0\xAF", w, &bad));  // overlong
    KD_CHECK_EQ(bad, size_t{3});

    // Unpaired surrogates coming back from a Windows API. Rejected here so they never become a
    // bad_utf8 frame at the far end of the pipe.
    std::string s;
    size_t idx = kNpos;
    KD_CHECK(!kd::utf8::from_utf16(wide({0x0041, 0xD83D}), s, &idx));  // high surrogate at EOF
    KD_CHECK_EQ(idx, size_t{1});
    idx = kNpos;
    KD_CHECK(!kd::utf8::from_utf16(wide({0xD83D, 0x0041}), s, &idx));  // high not followed by low
    KD_CHECK_EQ(idx, size_t{0});
    idx = kNpos;
    KD_CHECK(!kd::utf8::from_utf16(wide({0xDE00}), s, &idx));  // lone low surrogate
    KD_CHECK_EQ(idx, size_t{0});
    idx = kNpos;
    KD_CHECK(!kd::utf8::from_utf16(wide({0xDE00, 0xD83D}), s, &idx));  // reversed pair
    KD_CHECK_EQ(idx, size_t{0});
}

KD_TEST(utf8, utf16_convenience_wrappers_return_empty_on_bad_input) {
    KD_CHECK(kd::utf8::to_utf16_or_empty("\xC0\xAF").empty());
    KD_CHECK(kd::utf8::from_utf16_or_empty(wide({0xD83D})).empty());
    KD_CHECK_EQ(kd::utf8::to_utf16_or_empty("ab").size(), size_t{2});
    KD_CHECK_EQ(kd::utf8::from_utf16_or_empty(wide({0x0061, 0x00E9})), std::string("a" "\xC3\xA9"));
}

// ─── Regression: the byte-wise prefix can stop inside a code point ───────────────────────────

KD_TEST(utf8, common_prefix_when_inputs_diverge_mid_code_point) {
    // The bug this pins: common_grapheme_prefix() began its retraction walk at the byte-wise
    // common prefix, which for these pairs lands on a continuation byte. prev_code_point()
    // answers kNpos there, and the function returned 0 — so the commit engine committed nothing
    // at all for the rest of any utterance containing such a pair. Emoji made it visible, but
    // every one of these is ordinary dictated text.
    //
    // Each pair shares a leading ASCII run and then diverges inside a multi-byte code point.
    struct Case {
        const char* a;
        const char* b;
        size_t want;  // bytes of shared *grapheme* prefix
    };
    const Case cases[] = {
        {"caf\xC3\xA9", "caf\xC3\xA8", 3},                          // é vs è, differ in byte 2 of 2
        {"the \xD0\xB8", "the \xD0\xB9", 4},                        // и vs й (Cyrillic)
        {"x \xE6\x97\xA5", "x \xE6\x97\xA5\xE6\x9C\xAC", 5},        // shared CJK, then divergence
        {"go \xE4\xB8\x8A", "go \xE4\xB8\x8B", 3},                  // 上 vs 下: differ in byte 3 of 3
    };
    for (const Case& c : cases) {
        const size_t got = kd::utf8::common_grapheme_prefix(c.a, c.b);
        KD_CHECK_EQ(got, c.want);
        // Whatever it returns must be a boundary in both inputs — never a fragment.
        KD_CHECK(kd::utf8::is_grapheme_boundary(c.a, got));
        KD_CHECK(kd::utf8::is_grapheme_boundary(c.b, got));
    }
}
