// UTF-8 validation, code-point and grapheme-cluster navigation, and the UTF-16 boundary.
//
// The normative statement of everything here is in util/utf8.h — read that first. This file
// carries the *why* of the implementation choices, not a restatement of the contract.
//
// Implements: plan.md §1.2, §1.4 steps 2-4; docs/protocol.md §8.2 and §8.4.
//
// ─── Two structural decisions worth knowing before reading ──────────────────────────────────
//
// 1. **The grapheme rules are evaluated locally and backwards, not by scanning the string from
//    the beginning.** is_grapheme_boundary(s, off) looks at the code point after `off`, the code
//    point before it, and — only for the two rules that genuinely need history (GB11's
//    `ExtPict Extend* ZWJ ×` and GB12/GB13's regional-indicator parity) — walks backwards over
//    the bounded run those rules define. That keeps the predicate O(run length) instead of
//    O(offset), and, more importantly, it means there is exactly *one* implementation of the
//    break rules. next_grapheme(), count_graphemes(), floor_grapheme() and
//    common_grapheme_prefix() are all defined in terms of this one predicate, so they cannot
//    drift apart from it — which is the failure mode of the usual "fast forward iterator plus a
//    separate boundary check" arrangement, and it would show up as the protocol layer accepting
//    a `stable_bytes` the commit engine would never have produced.
//
//    Worst case is quadratic on a string that is one long run of regional indicators. Dictated
//    text is not that, and the alternative costs correctness insurance we want more than the
//    asymptote.
//
// 2. **Malformed input is never repaired and never guessed at.** No U+FFFD (docs/protocol.md
//    §4.1). Where a predicate cannot decode what it needs, it answers the conservative way:
//    is_grapheme_boundary() says "not a boundary", so a malformed offset can never be blessed as
//    a commit point. Refusing to commit is a delay; committing garbage is a corrupted document.
//
// ─── The tables ─────────────────────────────────────────────────────────────────────────────
//
// These are hand-written range tables, not generated from UnicodeData.txt, and utf8.h's
// kUnicodeVersion says "unspecified" for exactly that reason (docs/protocol.md §14 item 2 is the
// open question). They are deliberately *over*-inclusive where they are wrong: every
// approximation below errs toward merging code points into one cluster rather than splitting
// one, because a merge only delays a commit while a split types a half-formed character into the
// user's document. Do not hand-edit a single code point in here to fix a bug report — the fix is
// to generate both this file and the Python side's table from one source.

#include "util/utf8.h"

namespace kd::utf8 {
namespace {

// ─── Range-table machinery ──────────────────────────────────────────────────────────────────

struct Range {
    char32_t lo;
    char32_t hi;
};

// Binary search over a table that MUST be sorted and non-overlapping. That invariant is checked
// at compile time by the static_asserts under the tables, not at runtime: an unsorted table
// would make the search silently *miss* entries, which shows up as a cluster split in one
// obscure script and nowhere else. A compile-time check is the only kind that finds it.
template <size_t N>
constexpr bool in_ranges(const Range (&table)[N], char32_t cp) noexcept {
    size_t lo = 0;
    size_t hi = N;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (cp < table[mid].lo) {
            hi = mid;
        } else if (cp > table[mid].hi) {
            lo = mid + 1;
        } else {
            return true;
        }
    }
    return false;
}

// Grapheme_Extend=Yes, approximated. Coverage is the scripts a dictation tool plausibly sees
// plus everything docs/protocol.md §8.4 names as the required floor. A code point with
// Grapheme_Extend=Yes that is missing from this table causes an over-eager split, so additions
// are safe and removals are not.
constexpr Range kExtend[] = {
    {0x0300, 0x036F},  // combining diacritical marks (the "é as e + U+0301" case, fixture 013)
    {0x0483, 0x0489},  // Cyrillic combining
    {0x0591, 0x05BD}, {0x05BF, 0x05BF}, {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x05C7, 0x05C7},
    {0x0610, 0x061A}, {0x064B, 0x065F}, {0x0670, 0x0670},
    {0x06D6, 0x06DC}, {0x06DF, 0x06E4}, {0x06E7, 0x06E8}, {0x06EA, 0x06ED},
    {0x0711, 0x0711}, {0x0730, 0x074A}, {0x07A6, 0x07B0}, {0x07EB, 0x07F3}, {0x07FD, 0x07FD},
    {0x0816, 0x0819}, {0x081B, 0x0823}, {0x0825, 0x0827}, {0x0829, 0x082D},
    {0x0900, 0x0902}, {0x093A, 0x093A}, {0x093C, 0x093C}, {0x0941, 0x0948}, {0x094D, 0x094D},
    {0x0951, 0x0957}, {0x0962, 0x0963},
    {0x0981, 0x0981}, {0x09BC, 0x09BC}, {0x09BE, 0x09BE}, {0x09C1, 0x09C4}, {0x09CD, 0x09CD},
    {0x09D7, 0x09D7}, {0x09E2, 0x09E3}, {0x09FE, 0x09FE},
    {0x0A01, 0x0A02}, {0x0A3C, 0x0A3C}, {0x0A41, 0x0A42}, {0x0A47, 0x0A48}, {0x0A4B, 0x0A4D},
    {0x0A51, 0x0A51}, {0x0A70, 0x0A71}, {0x0A75, 0x0A75},
    {0x0A81, 0x0A82}, {0x0ABC, 0x0ABC}, {0x0AC1, 0x0AC5}, {0x0AC7, 0x0AC8}, {0x0ACD, 0x0ACD},
    {0x0AE2, 0x0AE3}, {0x0AFA, 0x0AFF},
    {0x0B01, 0x0B01}, {0x0B3C, 0x0B3C}, {0x0B3E, 0x0B3F}, {0x0B41, 0x0B44}, {0x0B4D, 0x0B4D},
    {0x0B55, 0x0B57}, {0x0B62, 0x0B63},
    {0x0B82, 0x0B82}, {0x0BBE, 0x0BBE}, {0x0BC0, 0x0BC0}, {0x0BCD, 0x0BCD}, {0x0BD7, 0x0BD7},
    {0x0C00, 0x0C00}, {0x0C04, 0x0C04}, {0x0C3E, 0x0C40}, {0x0C46, 0x0C48}, {0x0C4A, 0x0C4D},
    {0x0C55, 0x0C56}, {0x0C62, 0x0C63},
    {0x0C81, 0x0C81}, {0x0CBC, 0x0CBC}, {0x0CBF, 0x0CBF}, {0x0CC2, 0x0CC2}, {0x0CC6, 0x0CC6},
    {0x0CCC, 0x0CCD}, {0x0CD5, 0x0CD6}, {0x0CE2, 0x0CE3},
    {0x0D00, 0x0D01}, {0x0D3B, 0x0D3C}, {0x0D3E, 0x0D3E}, {0x0D41, 0x0D44}, {0x0D4D, 0x0D4D},
    {0x0D57, 0x0D57}, {0x0D62, 0x0D63},
    {0x0D81, 0x0D81}, {0x0DCA, 0x0DCA}, {0x0DCF, 0x0DCF}, {0x0DD2, 0x0DD6}, {0x0DDF, 0x0DDF},
    {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E},  // Thai above/below vowels and tones
    {0x0EB1, 0x0EB1}, {0x0EB4, 0x0EBC}, {0x0EC8, 0x0ECD},  // Lao
    {0x0F18, 0x0F19}, {0x0F35, 0x0F35}, {0x0F37, 0x0F37}, {0x0F39, 0x0F39}, {0x0F71, 0x0F84},
    {0x0F86, 0x0F87}, {0x0F8D, 0x0F97}, {0x0F99, 0x0FBC}, {0x0FC6, 0x0FC6},
    {0x102D, 0x1030}, {0x1032, 0x1037}, {0x1039, 0x103A}, {0x103D, 0x103E}, {0x1058, 0x1059},
    {0x105E, 0x1060}, {0x1071, 0x1074}, {0x1082, 0x1082}, {0x1085, 0x1086}, {0x108D, 0x108D},
    {0x135D, 0x135F}, {0x1712, 0x1714}, {0x1732, 0x1733}, {0x1752, 0x1753}, {0x1772, 0x1773},
    {0x17B4, 0x17B5}, {0x17B7, 0x17BD}, {0x17C6, 0x17C6}, {0x17C9, 0x17D3}, {0x17DD, 0x17DD},
    {0x180B, 0x180D}, {0x1885, 0x1886}, {0x18A9, 0x18A9},
    {0x1920, 0x1922}, {0x1927, 0x1928}, {0x1932, 0x1932}, {0x1939, 0x193B},
    {0x1A17, 0x1A18}, {0x1A1B, 0x1A1B}, {0x1A56, 0x1A56}, {0x1A58, 0x1A5E}, {0x1A60, 0x1A60},
    {0x1A62, 0x1A62}, {0x1A65, 0x1A6C}, {0x1A73, 0x1A7C}, {0x1A7F, 0x1A7F},
    {0x1AB0, 0x1AFF},  // combining diacritical marks extended
    {0x1B00, 0x1B03}, {0x1B34, 0x1B34}, {0x1B36, 0x1B3A}, {0x1B3C, 0x1B3C}, {0x1B42, 0x1B42},
    {0x1B6B, 0x1B73}, {0x1B80, 0x1B81}, {0x1BA2, 0x1BA5}, {0x1BA8, 0x1BA9}, {0x1BAB, 0x1BAD},
    {0x1BE6, 0x1BE6}, {0x1BE8, 0x1BE9}, {0x1BED, 0x1BED}, {0x1BEF, 0x1BF1},
    {0x1C2C, 0x1C33}, {0x1C36, 0x1C37}, {0x1CD0, 0x1CD2}, {0x1CD4, 0x1CE0}, {0x1CE2, 0x1CE8},
    {0x1CED, 0x1CED}, {0x1CF4, 0x1CF4}, {0x1CF8, 0x1CF9},
    {0x1DC0, 0x1DFF},  // combining diacritical marks supplement
    {0x200C, 0x200C},  // ZWNJ (ZWJ is U+200D and is its own class — see GB11)
    {0x20D0, 0x20F0},  // combining marks for symbols; U+20E3 keycap lives here (fixture 013)
    {0x2CEF, 0x2CF1}, {0x2D7F, 0x2D7F}, {0x2DE0, 0x2DFF},
    {0x302A, 0x302F}, {0x3099, 0x309A},  // CJK tone marks; kana voiced-sound marks
    {0xA66F, 0xA672}, {0xA674, 0xA67D}, {0xA69E, 0xA69F}, {0xA6F0, 0xA6F1},
    {0xA802, 0xA802}, {0xA806, 0xA806}, {0xA80B, 0xA80B}, {0xA825, 0xA826}, {0xA82C, 0xA82C},
    {0xA8C4, 0xA8C5}, {0xA8E0, 0xA8F1}, {0xA8FF, 0xA8FF}, {0xA926, 0xA92D}, {0xA947, 0xA951},
    {0xA980, 0xA982}, {0xA9B3, 0xA9B3}, {0xA9B6, 0xA9B9}, {0xA9BC, 0xA9BD}, {0xA9E5, 0xA9E5},
    {0xAA29, 0xAA2E}, {0xAA31, 0xAA32}, {0xAA35, 0xAA36}, {0xAA43, 0xAA43}, {0xAA4C, 0xAA4C},
    {0xAA7C, 0xAA7C}, {0xAAB0, 0xAAB0}, {0xAAB2, 0xAAB4}, {0xAAB7, 0xAAB8}, {0xAABE, 0xAABF},
    {0xAAC1, 0xAAC1}, {0xAAEC, 0xAAED}, {0xAAF6, 0xAAF6}, {0xABE5, 0xABE5}, {0xABE8, 0xABE8},
    {0xABED, 0xABED},
    {0xFB1E, 0xFB1E},
    {0xFE00, 0xFE0F},  // variation selectors (fixture 013's U+FE0F keycap sequence)
    {0xFE20, 0xFE2F},  // combining half marks
    {0xFF9E, 0xFF9F},  // halfwidth kana voiced-sound marks
    {0x101FD, 0x101FD}, {0x102E0, 0x102E0}, {0x10376, 0x1037A},
    {0x10A01, 0x10A0F}, {0x10A38, 0x10A3F}, {0x10AE5, 0x10AE6},
    {0x11001, 0x11001}, {0x11038, 0x11046}, {0x1107F, 0x11081}, {0x110B3, 0x110B6},
    {0x110B9, 0x110BA}, {0x11100, 0x11102}, {0x11127, 0x1112B}, {0x1112D, 0x11134},
    {0x11173, 0x11173}, {0x11180, 0x11181}, {0x111B6, 0x111BE}, {0x111C9, 0x111CC},
    {0x1122F, 0x11231}, {0x11234, 0x11234}, {0x11236, 0x11237}, {0x112DF, 0x112DF},
    {0x112E3, 0x112EA}, {0x11300, 0x11301}, {0x1133B, 0x1133C}, {0x11340, 0x11340},
    {0x11366, 0x1136C}, {0x11370, 0x11374},
    {0x16AF0, 0x16AF4}, {0x16B30, 0x16B36}, {0x16F8F, 0x16F92}, {0x16FE4, 0x16FE4},
    {0x1BC9D, 0x1BC9E},
    {0x1D165, 0x1D169}, {0x1D16D, 0x1D172}, {0x1D17B, 0x1D182}, {0x1D185, 0x1D18B},
    {0x1D1AA, 0x1D1AD}, {0x1D242, 0x1D244},
    {0x1DA00, 0x1DA36}, {0x1DA3B, 0x1DA6C}, {0x1DA75, 0x1DA75}, {0x1DA84, 0x1DA84},
    {0x1E000, 0x1E006}, {0x1E008, 0x1E018}, {0x1E01B, 0x1E021}, {0x1E023, 0x1E024},
    {0x1E026, 0x1E02A}, {0x1E8D0, 0x1E8D6}, {0x1E944, 0x1E94A},
    {0x1F3FB, 0x1F3FF},  // emoji skin-tone modifiers: Grapheme_Extend=Yes, so GB9 covers them
    {0xE0020, 0xE007F},  // tag characters (subdivision flags such as the Scotland flag)
    {0xE0100, 0xE01EF},  // variation selectors supplement
};

// Grapheme_Cluster_Break=SpacingMark, approximated (UAX #29 rule GB9a). Spacing combining marks
// that visibly *follow* their base but still belong to its cluster. Far less complete than
// kExtend; the failure direction is the same (a missing entry splits a cluster it should not,
// and the consequence is a delayed commit, not corrupt text).
constexpr Range kSpacingMark[] = {
    {0x0903, 0x0903}, {0x093B, 0x093B}, {0x093E, 0x0940}, {0x0949, 0x094C}, {0x094E, 0x094F},
    {0x0982, 0x0983}, {0x09BF, 0x09C0}, {0x09C7, 0x09C8}, {0x09CB, 0x09CC},
    {0x0A03, 0x0A03}, {0x0A3E, 0x0A40}, {0x0A83, 0x0A83}, {0x0ABE, 0x0AC0}, {0x0AC9, 0x0AC9},
    {0x0ACB, 0x0ACC}, {0x0B02, 0x0B03}, {0x0B40, 0x0B40}, {0x0B47, 0x0B48}, {0x0B4B, 0x0B4C},
    {0x0BBF, 0x0BBF}, {0x0BC1, 0x0BC2}, {0x0BC6, 0x0BC8}, {0x0BCA, 0x0BCC},
    {0x0C01, 0x0C03}, {0x0C41, 0x0C44}, {0x0C82, 0x0C83}, {0x0CBE, 0x0CBE}, {0x0CC0, 0x0CC1},
    {0x0CC3, 0x0CC4}, {0x0CC7, 0x0CC8}, {0x0CCA, 0x0CCB},
    {0x0D02, 0x0D03}, {0x0D3F, 0x0D40}, {0x0D46, 0x0D48}, {0x0D4A, 0x0D4C},
    {0x0D82, 0x0D83}, {0x0DD0, 0x0DD1}, {0x0DD8, 0x0DDE}, {0x0DF2, 0x0DF3},
    {0x0E33, 0x0E33}, {0x0EB3, 0x0EB3},
    {0x102B, 0x102C}, {0x1031, 0x1031}, {0x1038, 0x1038}, {0x103B, 0x103C}, {0x1056, 0x1057},
    {0x1062, 0x1064}, {0x1067, 0x106D}, {0x1083, 0x1084}, {0x1087, 0x108C}, {0x108F, 0x108F},
    {0x109A, 0x109C},
    {0x17B6, 0x17B6}, {0x17BE, 0x17C5}, {0x17C7, 0x17C8},
    {0x1923, 0x1926}, {0x1929, 0x192B}, {0x1930, 0x1931}, {0x1933, 0x1938},
    {0x1A19, 0x1A1A}, {0x1A55, 0x1A55}, {0x1A57, 0x1A57}, {0x1A61, 0x1A61}, {0x1A63, 0x1A64},
    {0x1A6D, 0x1A72}, {0x1B04, 0x1B04}, {0x1B35, 0x1B35}, {0x1B3B, 0x1B3B}, {0x1B3D, 0x1B41},
    {0x1B43, 0x1B44}, {0x1B82, 0x1B82}, {0x1BA1, 0x1BA1}, {0x1BA6, 0x1BA7}, {0x1BAA, 0x1BAA},
    {0x1BE7, 0x1BE7}, {0x1BEA, 0x1BEC}, {0x1BEE, 0x1BEE}, {0x1BF2, 0x1BF3},
    {0x1C24, 0x1C2B}, {0x1C34, 0x1C35}, {0x1CE1, 0x1CE1}, {0x1CF7, 0x1CF7},
    {0xA823, 0xA824}, {0xA827, 0xA827}, {0xA880, 0xA881}, {0xA8B4, 0xA8C3},
    {0xA952, 0xA953}, {0xA983, 0xA983}, {0xA9B4, 0xA9B5}, {0xA9BA, 0xA9BB}, {0xA9BE, 0xA9C0},
    {0xAA2F, 0xAA30}, {0xAA33, 0xAA34}, {0xAA4D, 0xAA4D}, {0xAAEB, 0xAAEB}, {0xAAEE, 0xAAEF},
    {0xAAF5, 0xAAF5}, {0xABE3, 0xABE4}, {0xABE6, 0xABE7}, {0xABE9, 0xABEA}, {0xABEC, 0xABEC},
    {0x11000, 0x11000}, {0x11002, 0x11002}, {0x11082, 0x11082}, {0x110B0, 0x110B2},
    {0x110B7, 0x110B8}, {0x1112C, 0x1112C}, {0x11182, 0x11182}, {0x111B3, 0x111B5},
    {0x111BF, 0x111C0}, {0x1122C, 0x1122E}, {0x11232, 0x11233}, {0x11235, 0x11235},
    {0x112E0, 0x112E2}, {0x11302, 0x11303}, {0x1133E, 0x1133F}, {0x11341, 0x11344},
    {0x11347, 0x11348}, {0x1134B, 0x1134D},
};

// Extended_Pictographic, approximated by block-sized ranges rather than the generated property.
// Used only by GB11 (ZWJ sequences). Over-inclusion here can merge two adjacent pictographs that
// UAX #29 would separate — but only when a ZWJ sits between them, which is precisely the case
// where merging is what the user means (👨‍👩‍👧 is one family, not three people).
constexpr Range kExtPict[] = {
    {0x00A9, 0x00A9}, {0x00AE, 0x00AE},
    {0x203C, 0x203C}, {0x2049, 0x2049}, {0x2122, 0x2122}, {0x2139, 0x2139},
    {0x2194, 0x21AA}, {0x231A, 0x231B}, {0x2328, 0x2328}, {0x2388, 0x2388}, {0x23CF, 0x23CF},
    {0x23E9, 0x23F3}, {0x23F8, 0x23FA}, {0x24C2, 0x24C2}, {0x25AA, 0x25AB}, {0x25B6, 0x25B6},
    {0x25C0, 0x25C0}, {0x25FB, 0x25FE},
    {0x2600, 0x27BF},  // misc symbols + dingbats, taken whole
    {0x2934, 0x2935}, {0x2B00, 0x2BFF}, {0x3030, 0x3030}, {0x303D, 0x303D},
    {0x3297, 0x3297}, {0x3299, 0x3299},
    {0x1F000, 0x1FAFF}, {0x1FC00, 0x1FFFD},
};

// White_Space=Yes. Short enough to be exact rather than approximate.
constexpr Range kWhitespace[] = {
    {0x0009, 0x000D}, {0x0020, 0x0020}, {0x0085, 0x0085}, {0x00A0, 0x00A0},
    {0x1680, 0x1680}, {0x2000, 0x200A}, {0x2028, 0x2029}, {0x202F, 0x202F},
    {0x205F, 0x205F}, {0x3000, 0x3000},
};

// Terminal punctuation that ends a word in dictated text. Note what is absent: quotes, brackets,
// hyphens, and the apostrophe, none of which reliably end a word ("don't", "well-known",
// '"quoted"'). See is_word_separator() in utf8.h and the llama.cpp note in
// retract_to_word_boundary() below.
constexpr Range kTerminalPunct[] = {
    {0x0021, 0x0021},  // !
    {0x002C, 0x002C},  // ,
    {0x002E, 0x002E},  // .
    {0x003A, 0x003B},  // : ;
    {0x003F, 0x003F},  // ?
    {0x2026, 0x2026},  // …
    {0x203C, 0x203D},  // ‼ ‽
    {0x2047, 0x2049},  // ⁇ ⁈ ⁉
    {0x3001, 0x3002},  // 、 。
    {0xFF01, 0xFF01},  // ！
    {0xFF0C, 0xFF0C},  // ，
    {0xFF0E, 0xFF0E},  // ．
    {0xFF1A, 0xFF1B},  // ： ；
    {0xFF1F, 0xFF1F},  // ？
    {0xFF61, 0xFF61},  // ｡
    {0xFF64, 0xFF64},  // ､
};

// Grapheme_Cluster_Break=Control, approximated: Cc other than CR/LF, plus the format characters
// that are not ZWJ/ZWNJ, plus the line/paragraph separators. Text always breaks either side of
// one of these (GB4/GB5), which is what keeps a control character from being glued onto a word.
constexpr Range kControl[] = {
    {0x0000, 0x0009}, {0x000B, 0x000C}, {0x000E, 0x001F},  // Cc minus CR (0x0D) and LF (0x0A)
    {0x007F, 0x009F},
    {0x00AD, 0x00AD},  // soft hyphen
    {0x061C, 0x061C}, {0x180E, 0x180E},
    {0x200B, 0x200B},                    // ZWSP — Cf, and not an Extend
    {0x200E, 0x200F},                    // LRM/RLM (U+200C ZWNJ and U+200D ZWJ are excluded)
    {0x2028, 0x2029},                    // Zl / Zp
    {0x2060, 0x2064}, {0x2066, 0x206F},  // word joiner, invisible operators, bidi overrides
    {0xFEFF, 0xFEFF},
    {0xFFF9, 0xFFFB},
    {0x13430, 0x1343F}, {0x1BCA0, 0x1BCA3}, {0x1D173, 0x1D17A},
    {0xE0000, 0xE001F}, {0xE0080, 0xE00FF}, {0xE01F0, 0xE0FFF},
};

// The invariant in_ranges() depends on, enforced where it cannot rot: a table edited in the
// wrong place fails the build rather than quietly losing a code point in a binary search.
template <size_t N>
constexpr bool sorted_disjoint(const Range (&table)[N]) noexcept {
    for (size_t i = 0; i < N; ++i) {
        if (table[i].lo > table[i].hi) return false;
        if (i > 0 && table[i].lo <= table[i - 1].hi) return false;
    }
    return true;
}

static_assert(sorted_disjoint(kExtend), "kExtend must be sorted and non-overlapping");
static_assert(sorted_disjoint(kSpacingMark), "kSpacingMark must be sorted and non-overlapping");
static_assert(sorted_disjoint(kExtPict), "kExtPict must be sorted and non-overlapping");
static_assert(sorted_disjoint(kWhitespace), "kWhitespace must be sorted and non-overlapping");
static_assert(sorted_disjoint(kTerminalPunct), "kTerminalPunct must be sorted and non-overlapping");
static_assert(sorted_disjoint(kControl), "kControl must be sorted and non-overlapping");

constexpr char32_t kCR = 0x000D;
constexpr char32_t kLF = 0x000A;
constexpr char32_t kZWJ = 0x200D;

constexpr bool is_regional_indicator(char32_t cp) noexcept {
    return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

// Hangul jamo classes (UAX #29 GB6-GB8). utf8.h's first draft declared these unmodelled; they
// are modelled, because the rules are four comparisons and the alternative reports a decomposed
// syllable as three clusters. docs/protocol.md §8.4 permits a receiver's check to be stricter
// than the stated floor without a proto bump, which is exactly what this is.
enum class Hangul { None, L, V, T, LV, LVT };

constexpr Hangul hangul_class(char32_t cp) noexcept {
    if (cp >= 0x1100 && cp <= 0x115F) return Hangul::L;
    if (cp >= 0xA960 && cp <= 0xA97C) return Hangul::L;
    if (cp >= 0x1160 && cp <= 0x11A7) return Hangul::V;
    if (cp >= 0xD7B0 && cp <= 0xD7C6) return Hangul::V;
    if (cp >= 0x11A8 && cp <= 0x11FF) return Hangul::T;
    if (cp >= 0xD7CB && cp <= 0xD7FB) return Hangul::T;
    if (cp >= 0xAC00 && cp <= 0xD7A3) {
        // Precomposed syllable: those divisible by 28 have no trailing consonant (LV), the rest
        // do (LVT). 28 is the number of T jamo including "none".
        return ((cp - 0xAC00) % 28 == 0) ? Hangul::LV : Hangul::LVT;
    }
    return Hangul::None;
}

constexpr bool is_extend(char32_t cp) noexcept { return in_ranges(kExtend, cp); }
constexpr bool is_spacing_mark(char32_t cp) noexcept { return in_ranges(kSpacingMark, cp); }
constexpr bool is_ext_pict(char32_t cp) noexcept { return in_ranges(kExtPict, cp); }
constexpr bool is_control(char32_t cp) noexcept { return in_ranges(kControl, cp); }

// ─── Strict decoding ────────────────────────────────────────────────────────────────────────

inline uint8_t byte_at(std::string_view s, size_t i) noexcept {
    return static_cast<uint8_t>(s[i]);
}

// The one decoder. On failure `bad` receives the absolute offset of the byte that made the
// sequence invalid; for a sequence truncated by the end of the buffer there is no such byte, so
// `bad` receives the offset of the *lead*, which is the offset a `bad_utf8` error should report
// (docs/protocol.md §11.3) because that is where the caller must resume or reject.
bool decode_strict(std::string_view s, size_t pos, char32_t& cp, size_t& len, size_t& bad) noexcept {
    cp = 0;
    len = 0;
    if (pos >= s.size()) {
        bad = pos;
        return false;
    }
    const uint8_t b0 = byte_at(s, pos);
    const int n = sequence_length(b0);
    if (n == 0) {
        bad = pos;  // continuation byte in lead position, or 0xC0/0xC1/0xF5-0xFF
        return false;
    }
    if (n == 1) {
        cp = b0;
        len = 1;
        return true;
    }
    if (pos + static_cast<size_t>(n) > s.size()) {
        bad = pos;  // truncated at end of buffer — see the note above
        return false;
    }

    // The second byte carries the overlong / surrogate / >U+10FFFF constraints. Checking the
    // range here rather than decoding first and range-checking after is what makes rejection
    // exact: "\xED\xA0\x80" is refused at the byte that made it a surrogate, not at the end.
    const uint8_t b1 = byte_at(s, pos + 1);
    uint8_t lo = 0x80;
    uint8_t hi = 0xBF;
    if (b0 == 0xE0) {
        lo = 0xA0;  // reject 3-byte overlongs
    } else if (b0 == 0xED) {
        hi = 0x9F;  // reject UTF-8-encoded surrogates U+D800-U+DFFF
    } else if (b0 == 0xF0) {
        lo = 0x90;  // reject 4-byte overlongs
    } else if (b0 == 0xF4) {
        hi = 0x8F;  // reject anything above U+10FFFF
    }
    if (b1 < lo || b1 > hi) {
        bad = pos + 1;
        return false;
    }

    char32_t acc = static_cast<char32_t>(b0 & (0xFF >> (n + 1)));
    acc = (acc << 6) | static_cast<char32_t>(b1 & 0x3F);
    for (int i = 2; i < n; ++i) {
        const uint8_t bi = byte_at(s, pos + static_cast<size_t>(i));
        if ((bi & 0xC0) != 0x80) {
            bad = pos + static_cast<size_t>(i);
            return false;
        }
        acc = (acc << 6) | static_cast<char32_t>(bi & 0x3F);
    }
    cp = acc;
    len = static_cast<size_t>(n);
    return true;
}

// Decode the code point that *ends* at `off`. Returns false at offset 0 or on malformed input.
bool decode_prev(std::string_view s, size_t off, char32_t& cp, size_t& start) noexcept {
    if (off == 0 || off > s.size()) return false;
    // A UTF-8 sequence is at most 4 bytes, so the lead is within 4 bytes of the end.
    const size_t limit = off < 4 ? off : 4;
    for (size_t back = 1; back <= limit; ++back) {
        const size_t cand = off - back;
        if ((byte_at(s, cand) & 0xC0) == 0x80) continue;  // continuation byte; keep walking back
        char32_t c = 0;
        size_t len = 0;
        size_t bad = 0;
        if (!decode_strict(s, cand, c, len, bad)) return false;
        if (cand + len != off) return false;  // `off` is inside this sequence, not after it
        cp = c;
        start = cand;
        return true;
    }
    return false;
}

}  // namespace

// ─── Validation ─────────────────────────────────────────────────────────────────────────────

bool validate(std::string_view s, size_t* first_bad_offset, size_t* bad_sequence_start) noexcept {
    size_t i = 0;
    while (i < s.size()) {
        char32_t cp = 0;
        size_t len = 0;
        size_t bad = 0;
        if (!decode_strict(s, i, cp, len, bad)) {
            if (first_bad_offset != nullptr) *first_bad_offset = bad;
            // `i` is by construction the start of the sequence being decoded, so the wire offset
            // needs no back-scan. Back-scanning would not work anyway: the offending byte is not
            // always a continuation byte (a truncated "\xE9\"" fails on '"'), so walking back over
            // continuations from `bad` would stop on the wrong byte.
            if (bad_sequence_start != nullptr) *bad_sequence_start = i;
            return false;
        }
        i += len;
    }
    return true;
}

// ─── Code-point level ───────────────────────────────────────────────────────────────────────

int sequence_length(uint8_t lead) noexcept {
    if (lead < 0x80) return 1;
    if (lead < 0xC2) return 0;  // 0x80-0xBF continuation, 0xC0/0xC1 always overlong
    if (lead < 0xE0) return 2;
    if (lead < 0xF0) return 3;
    if (lead < 0xF5) return 4;
    return 0;  // 0xF5-0xFF would encode above U+10FFFF
}

bool decode(std::string_view s, size_t pos, char32_t& cp, size_t& len) noexcept {
    size_t bad = 0;
    return decode_strict(s, pos, cp, len, bad);
}

bool is_code_point_boundary(std::string_view s, size_t off) noexcept {
    if (off > s.size()) return false;
    if (off == 0 || off == s.size()) return true;
    return (byte_at(s, off) & 0xC0) != 0x80;
}

size_t next_code_point(std::string_view s, size_t off) noexcept {
    char32_t cp = 0;
    size_t len = 0;
    if (!decode(s, off, cp, len)) return kNpos;
    return off + len;
}

size_t prev_code_point(std::string_view s, size_t off) noexcept {
    char32_t cp = 0;
    size_t start = 0;
    if (!decode_prev(s, off, cp, start)) return kNpos;
    return start;
}

size_t count_code_points(std::string_view s) noexcept {
    size_t n = 0;
    size_t i = 0;
    while (i < s.size()) {
        char32_t cp = 0;
        size_t len = 0;
        if (!decode(s, i, cp, len)) return kNpos;
        i += len;
        ++n;
    }
    return n;
}

// ─── Grapheme-cluster level ─────────────────────────────────────────────────────────────────

bool is_grapheme_boundary(std::string_view s, size_t off) noexcept {
    if (off > s.size()) return false;      // past the end is not a position at all
    if (off == 0 || off == s.size()) return true;  // GB1 / GB2
    if (!is_code_point_boundary(s, off)) return false;

    char32_t next = 0;
    size_t next_len = 0;
    if (!decode(s, off, next, next_len)) return false;  // malformed ahead: refuse to bless it

    char32_t prev = 0;
    size_t prev_start = 0;
    if (!decode_prev(s, off, prev, prev_start)) return false;  // malformed behind: same

    // Rule order is UAX #29's, and it matters: GB4/GB5 outrank GB9, so a line feed after a
    // combining mark still breaks rather than being absorbed into the cluster.
    if (prev == kCR && next == kLF) return false;                          // GB3
    if (prev == kCR || prev == kLF || is_control(prev)) return true;       // GB4
    if (next == kCR || next == kLF || is_control(next)) return true;       // GB5

    const Hangul hp = hangul_class(prev);
    const Hangul hn = hangul_class(next);
    if (hp == Hangul::L &&
        (hn == Hangul::L || hn == Hangul::V || hn == Hangul::LV || hn == Hangul::LVT)) {
        return false;  // GB6
    }
    if ((hp == Hangul::LV || hp == Hangul::V) && (hn == Hangul::V || hn == Hangul::T)) {
        return false;  // GB7
    }
    if ((hp == Hangul::LVT || hp == Hangul::T) && hn == Hangul::T) return false;  // GB8

    if (is_extend(next) || next == kZWJ) return false;  // GB9  (skin tones land here)
    if (is_spacing_mark(next)) return false;            // GB9a

    // GB9b (Prepend ×) is not modelled: the Prepend set is tiny, script-specific, and absent
    // from anything this program is likely to dictate. Consequence is an over-eager split of a
    // Prepend sequence — conservative in the safe direction, and recorded here rather than
    // silently assumed.

    // GB11: ExtPict Extend* ZWJ × ExtPict. The backward walk is bounded by the Extend run.
    if (prev == kZWJ && is_ext_pict(next)) {
        size_t p = prev_start;
        for (;;) {
            char32_t c = 0;
            size_t start = 0;
            if (!decode_prev(s, p, c, start)) break;
            if (is_ext_pict(c)) return false;
            if (!is_extend(c)) break;
            p = start;
        }
    }

    // GB12/GB13: an odd number of regional indicators immediately behind `off` means the one at
    // `off` completes a pair (a flag), so there is no boundary. Counting the whole run is the
    // only way to get "🇩🇪🇩🇪" right — the second flag must start a new cluster.
    if (is_regional_indicator(next) && is_regional_indicator(prev)) {
        size_t run = 0;
        size_t p = off;
        for (;;) {
            char32_t c = 0;
            size_t start = 0;
            if (!decode_prev(s, p, c, start)) break;
            if (!is_regional_indicator(c)) break;
            ++run;
            p = start;
        }
        if ((run % 2) == 1) return false;
    }

    return true;  // GB999
}

size_t next_grapheme(std::string_view s, size_t off) noexcept {
    if (off >= s.size()) return kNpos;
    for (size_t r = off + 1; r <= s.size(); ++r) {
        if (is_grapheme_boundary(s, r)) return r;
    }
    return kNpos;  // unreachable: s.size() is always a boundary
}

size_t prev_grapheme(std::string_view s, size_t off) noexcept {
    if (off == 0) return kNpos;
    size_t r = (off <= s.size()) ? off - 1 : s.size();
    while (r > 0 && !is_grapheme_boundary(s, r)) --r;
    return r;  // 0 is always a boundary, so this terminates with a valid answer
}

size_t floor_grapheme(std::string_view s, size_t off) noexcept {
    size_t r = (off <= s.size()) ? off : s.size();
    while (r > 0 && !is_grapheme_boundary(s, r)) --r;
    return r;
}

size_t count_graphemes(std::string_view s) noexcept {
    if (!validate(s)) return kNpos;
    size_t n = 0;
    size_t i = 0;
    while (i < s.size()) {
        const size_t nxt = next_grapheme(s, i);
        if (nxt == kNpos) return kNpos;
        i = nxt;
        ++n;
    }
    return n;
}

// ─── Prefix and retraction (plan.md §1.4 steps 2-4) ─────────────────────────────────────────

size_t common_grapheme_prefix(std::string_view a, std::string_view b) noexcept {
    const std::string_view items[2] = {a, b};
    return common_grapheme_prefix(items, 2);
}

size_t common_grapheme_prefix(const std::string_view* items, size_t count) noexcept {
    if (items == nullptr || count == 0) return 0;

    // Step 1: the byte-wise common prefix, which is an upper bound on the answer.
    size_t p = items[0].size();
    for (size_t i = 1; i < count; ++i) {
        const size_t limit = (items[i].size() < p) ? items[i].size() : p;
        size_t j = 0;
        while (j < limit && items[0][j] == items[i][j]) ++j;
        p = j;
    }

    // Step 2: retract until the offset is a cluster boundary in *every* input. Requiring all of
    // them is the whole point (plan.md §1.4 step 2): "e" and "é" share the byte prefix
    // "e", but offset 1 is a boundary in the first and not in the second, so the shared prefix
    // ends inside a cluster and committing it would type a letter whose accent is still being
    // revised.
    // The byte-wise prefix routinely stops INSIDE a code point: "é" (C3 A9) and "è" (C3 A8) share
    // their lead byte, as do any two emoji from one block, and any two CJK characters from one
    // range. prev_code_point() answers kNpos for an offset that is not a code-point boundary — so
    // starting the walk on one made this function return 0 for every such pair, which meant the
    // engine committed NOTHING for the rest of an utterance containing one. Floor first.
    size_t r = p;
    while (r > 0 && !is_code_point_boundary(items[0], r)) --r;

    for (;;) {
        bool all = true;
        for (size_t i = 0; i < count; ++i) {
            if (!is_grapheme_boundary(items[i], r)) {
                all = false;
                break;
            }
        }
        if (all) return r;
        if (r == 0) return 0;
        const size_t back = prev_code_point(items[0], r);
        if (back == kNpos) return 0;  // malformed input: commit nothing
        r = back;
    }
}

size_t retract_graphemes(std::string_view s, size_t off, size_t clusters) noexcept {
    // Flooring first means a mid-cluster `off` costs the caller the partial cluster *plus* the
    // requested retraction. That is the conservative reading of "at least M clusters behind the
    // decode head" and the only one that cannot over-commit.
    size_t r = floor_grapheme(s, off);
    for (size_t i = 0; i < clusters && r > 0; ++i) {
        const size_t p = prev_grapheme(s, r);
        if (p == kNpos) return 0;
        r = p;
    }
    return r;
}

bool is_whitespace(char32_t cp) noexcept { return in_ranges(kWhitespace, cp); }

bool is_word_separator(char32_t cp) noexcept {
    return is_whitespace(cp) || in_ranges(kTerminalPunct, cp);
}

size_t trim_trailing_whitespace(std::string_view text, size_t offset) noexcept {
    size_t r = (offset <= text.size()) ? offset : text.size();
    for (;;) {
        char32_t cp = 0;
        size_t start = 0;
        if (!decode_prev(text, r, cp, start)) return r;  // at 0, or malformed: stop where we are
        if (!is_whitespace(cp)) return r;
        r = start;
    }
}

size_t retract_to_word_boundary(std::string_view text, size_t offset) noexcept {
    size_t r = (offset <= text.size()) ? offset : text.size();

    // Walk back over code points looking for one that ends a word. Two conditions, and the
    // second is the "llama.cpp" rule from plan.md §4:
    //
    //   * whitespace always ends a word;
    //   * terminal punctuation ends a word only when whitespace or the end of `text` follows it.
    //     Otherwise "llama.cpp" would commit as "llama." while the model is still deciding
    //     whether it said "cpp", "C P P" or "see plus plus" — and once "llama." is in the
    //     document there is no taking it back (the invariant in plan.md §1.4).
    //
    // Note the lookahead uses the whole of `text`, not text[0, offset): what follows the
    // candidate separator is information we have, and pretending we do not have it would commit
    // *more* rather than less.
    for (;;) {
        char32_t cp = 0;
        size_t start = 0;
        if (!decode_prev(text, r, cp, start)) return 0;  // reached 0, or malformed: commit nothing

        bool ends_word = false;
        if (is_whitespace(cp)) {
            ends_word = true;
        } else if (is_word_separator(cp)) {
            if (r == text.size()) {
                ends_word = true;
            } else {
                char32_t after = 0;
                size_t after_len = 0;
                if (decode(text, r, after, after_len) && is_whitespace(after)) ends_word = true;
            }
        }

        if (ends_word && is_grapheme_boundary(text, r)) {
            // Hold back the trailing whitespace run (plan.md §1.4): committing the space means a
            // later ", " revision would have to rewrite across text already in the document.
            // floor_grapheme() after the trim is belt-and-braces — the trim stops on a code-point
            // boundary that is also a cluster boundary in every case we can construct, but the
            // commit engine's correctness must not rest on "every case we can construct".
            return floor_grapheme(text, trim_trailing_whitespace(text, r));
        }
        r = start;
    }
}

// ─── UTF-16 conversion ──────────────────────────────────────────────────────────────────────
//
// Hand-written rather than MultiByteToWideChar/WideCharToMultiByte, as util/utf8.h states. The
// reasons are worth repeating at the code: this module stays free of windows.h and therefore
// unit-testable, and MB_ERR_INVALID_CHARS is not a stable definition of "well-formed" across
// Windows versions — WideCharToMultiByte's WC_ERR_INVALID_CHARS in particular only exists from
// Vista onward and its treatment of unpaired surrogates has changed. "Well-formed" has to mean
// the same thing here as it means on the wire, so it is defined here.

static_assert(sizeof(wchar_t) == 2,
              "This module assumes the Windows UTF-16 wchar_t. On a 32-bit-wchar_t platform the "
              "surrogate handling below is wrong, and silently so.");

bool to_utf16(std::string_view s, std::wstring& out, size_t* first_bad_offset) {
    out.clear();
    out.reserve(s.size());  // upper bound for ASCII, an over-estimate otherwise; never a re-alloc
    size_t i = 0;
    while (i < s.size()) {
        char32_t cp = 0;
        size_t len = 0;
        size_t bad = 0;
        if (!decode_strict(s, i, cp, len, bad)) {
            if (first_bad_offset != nullptr) *first_bad_offset = bad;
            return false;
        }
        if (cp < 0x10000) {
            out.push_back(static_cast<wchar_t>(cp));
        } else {
            const char32_t v = cp - 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
        }
        i += len;
    }
    return true;
}

bool from_utf16(std::wstring_view s, std::string& out, size_t* first_bad_index) {
    out.clear();
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        char32_t cp = static_cast<char32_t>(static_cast<uint16_t>(s[i]));
        if (cp >= 0xDC00 && cp <= 0xDFFF) {
            if (first_bad_index != nullptr) *first_bad_index = i;  // low surrogate with no high
            return false;
        }
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (i + 1 >= s.size()) {
                if (first_bad_index != nullptr) *first_bad_index = i;  // truncated pair
                return false;
            }
            const char32_t lo = static_cast<char32_t>(static_cast<uint16_t>(s[i + 1]));
            if (lo < 0xDC00 || lo > 0xDFFF) {
                if (first_bad_index != nullptr) *first_bad_index = i;
                return false;
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
        } else {
            i += 1;
        }

        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return true;
}

std::wstring to_utf16_or_empty(std::string_view s) {
    std::wstring out;
    if (!to_utf16(s, out)) out.clear();
    return out;
}

std::string from_utf16_or_empty(std::wstring_view s) {
    std::string out;
    if (!from_utf16(s, out)) out.clear();
    return out;
}

}  // namespace kd::utf8
