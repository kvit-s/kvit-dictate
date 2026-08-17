// Tests for the test framework itself.
//
// A test framework nobody tested is a source of false green builds, and this one has exactly
// the parts that go wrong quietly: byte-escaping in failure messages (which is what makes a
// UTF-8 offset bug legible rather than baffling), value stringification, and the fixture-path
// plumbing that every protocol conformance test will depend on.
//
// What is *not* tested here: the failure and abort paths. Exercising them would require a
// failing assertion inside a passing run, which the runner has no notion of. They are covered
// by construction instead — KD_REQUIRE throws AbortTest, and the runner catches it — and by
// the fact that a genuinely broken reporter shows up the first time any real test fails.

#include <cstdint>
#include <string>
#include <string_view>

#include "kd_test.h"

KD_TEST(harness, quote_escapes_non_printable_bytes) {
    KD_CHECK_EQ(kd::test::quote("abc"), std::string("\"abc\""));
    KD_CHECK_EQ(kd::test::quote(""), std::string("\"\""));
    KD_CHECK_EQ(kd::test::quote("a\tb"), std::string("\"a\\tb\""));
    KD_CHECK_EQ(kd::test::quote("a\nb"), std::string("\"a\\nb\""));
    KD_CHECK_EQ(kd::test::quote("say \"hi\""), std::string("\"say \\\"hi\\\"\""));
    KD_CHECK_EQ(kd::test::quote("back\\slash"), std::string("\"back\\\\slash\""));
}

KD_TEST(harness, quote_makes_utf8_bytes_visible) {
    // The reason this matters: "café" and "café" render identically in a terminal and
    // differ by four bytes. A failure message that printed them raw would be unreadable, and
    // half of this project's tests are about exactly that distinction (docs/protocol.md §8.4).
    const std::string precomposed = "caf\xC3\xA9";        // U+00E9
    const std::string decomposed = "cafe\xCC\x81";        // e + U+0301
    KD_CHECK_EQ(kd::test::quote(precomposed), std::string("\"caf\\xC3\\xA9\""));
    KD_CHECK_EQ(kd::test::quote(decomposed), std::string("\"cafe\\xCC\\x81\""));
    KD_CHECK_NE(kd::test::quote(precomposed), kd::test::quote(decomposed));
}

KD_TEST(harness, stringify_covers_the_types_assertions_use) {
    KD_CHECK_EQ(kd::test::stringify(true), std::string("true"));
    KD_CHECK_EQ(kd::test::stringify(false), std::string("false"));
    KD_CHECK_EQ(kd::test::stringify(42), std::string("42"));
    KD_CHECK_EQ(kd::test::stringify(std::string("hi")), std::string("\"hi\""));
    KD_CHECK_EQ(kd::test::stringify(std::string_view("hi")), std::string("\"hi\""));
    KD_CHECK_EQ(kd::test::stringify(static_cast<const char*>(nullptr)), std::string("nullptr"));

    // Bytes print as numbers, not as characters: 0xE9 as 'é' tells you nothing useful about
    // where a UTF-8 sequence went wrong.
    KD_CHECK_EQ(kd::test::stringify(static_cast<unsigned char>(0xE9)), std::string("233"));

    enum class Colour : uint8_t { Red = 3 };
    KD_CHECK_EQ(kd::test::stringify(Colour::Red), std::string("3"));
}

KD_TEST(harness, equal_compares_mixed_string_types) {
    KD_CHECK(kd::test::detail::equal(std::string("a"), "a"));
    KD_CHECK(kd::test::detail::equal("a", std::string("a")));
    KD_CHECK(kd::test::detail::equal("a", "a"));  // both const char*: strcmp, not pointer identity
    KD_CHECK(!kd::test::detail::equal("a", "b"));
    KD_CHECK(kd::test::detail::equal(std::string_view("a"), std::string("a")));
}

KD_TEST(harness, registry_is_populated_and_ordered_by_the_runner) {
    // If this is ever zero the KD_TEST macro's static registration has stopped working, which
    // would otherwise present as a silently empty run — the exact failure the runner's
    // "no tests matched" exit code exists to catch.
    KD_CHECK_GT(kd::test::registry().size(), size_t{0});
}

KD_TEST(harness, fixture_dir_points_at_the_shared_fixtures) {
    // docs/protocol.md §12 requires the C++ and Python suites to read the *same* files. If this
    // path is wrong, the conformance tests silently test nothing, so the index is read here
    // rather than only inside the (not yet written) fixture runner.
    const std::string index = kd::test::fixture_path("protocol/index.json");
    std::string contents;
    KD_REQUIRE(kd::test::read_file(index, contents));
    KD_CHECK_GT(contents.size(), size_t{100});
    KD_CHECK_NE(contents.find("\"cases\""), std::string::npos);
    KD_CHECK_NE(contents.find("001-fragmented-reads"), std::string::npos);
}

KD_TEST(harness, read_file_fails_loudly_on_a_missing_file) {
    std::string contents = "sentinel";
    KD_CHECK(!kd::test::read_file(kd::test::fixture_path("protocol/no-such-case.json"), contents));
    // Unchanged: a missing fixture must not look like an empty one.
    KD_CHECK_EQ(contents, std::string("sentinel"));
}
