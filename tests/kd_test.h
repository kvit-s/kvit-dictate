// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// kd_test — a ~300-line self-registering test framework, with no dependencies.
//
// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Everything you need to write a test
// ═══════════════════════════════════════════════════════════════════════════════════════════
//
//     #include "kd_test.h"
//     #include "util/utf8.h"
//
//     KD_TEST(utf8, rejects_overlong_encoding) {
//         size_t bad = 0;
//         KD_CHECK(!kd::utf8::validate("\xC0\xAF", &bad));
//         KD_CHECK_EQ(bad, size_t{0});
//     }
//
// Add the file to `KD_TEST_SOURCES` in CMakeLists.txt and it runs. There is no registration
// list to maintain, no fixture class to derive from, and no main() to write.
//
// ─── Assertions ─────────────────────────────────────────────────────────────────────────────
//
//   KD_CHECK(expr)            record a failure and keep going
//   KD_REQUIRE(expr)          record a failure and abandon this test (the rest would cascade)
//   KD_CHECK_EQ(a, b)         both operands printed on failure
//   KD_REQUIRE_EQ(a, b)
//   KD_CHECK_NE(a, b)
//   KD_CHECK_NEAR(a, b, eps)  for the double-valued metrics (ICER, confidence scores)
//   KD_FAIL("why")            unconditional failure
//   KD_INFO("context")        a note attached to this test's output, pass or fail
//
// Every failure prints file:line, the *source text* of the expression, and — for the
// comparisons — both values. Strings are quoted and non-printable bytes are escaped as \xNN,
// which matters constantly here: half of this project's tests are about UTF-8 byte offsets, and
// a diff of two visually identical strings is useless.
//
// ─── Running ────────────────────────────────────────────────────────────────────────────────
//
//   kd_tests                       run everything
//   kd_tests --suite utf8          run one suite (repeatable, or comma-separated)
//   kd_tests --name offset         run tests whose name contains a substring
//   kd_tests --list                print every registered test and exit
//   kd_tests --verbose             print each test as it runs, and every passing check count
//   kd_tests --fixtures <dir>      override the fixture directory (default: KD_FIXTURE_DIR)
//
// Exit code is 0 only if every test passed and at least one test ran. "No tests matched" is a
// failure, not a success: a filter typo that silently runs nothing is how a green build comes
// to mean nothing.
//
// ─── Design notes, so nobody has to reverse-engineer the choices ────────────────────────────
//
//   * **Header-only registry.** The list lives in a function-local static inside an inline
//     function, so there is no ordering problem between translation units and no separate
//     registration source file. Tests are sorted by suite then name before running, so the
//     output is stable regardless of link order.
//   * **KD_REQUIRE throws** a private type caught by the runner. This is the one place in the
//     project that uses exceptions for control flow; it is confined to the test binary and never
//     crosses into kd_core, whose hot paths are exception-free by policy (util/result.h).
//   * **No death tests, no mocking framework, no parameterised fixtures.** The modules that need
//     seams already expose them as interfaces (IClock, IScreenCapture, ICaretProvider,
//     IAudioSink), which is a better answer than a mocking library and is the reason those
//     interfaces exist.
//   * **Single-threaded.** Tests run one at a time on the main thread. The current-test pointer
//     is a plain global, not thread_local: a test that asserts from a worker thread would be
//     asserting about shared state anyway, and the modules under test are single-threaded by
//     design.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace kd::test {

// ─── Failure records ────────────────────────────────────────────────────────────────────────

struct Failure {
    const char* file = "";
    int line = 0;
    std::string expression;  // the source text of the assertion
    std::string detail;      // "expected: X\n  actual: Y", or a free-form message
};

// Thrown by KD_REQUIRE. Caught by the runner; never escapes the test binary.
struct AbortTest {};

class TestResult {
public:
    void record_check() noexcept { ++checks_; }
    void record_failure(Failure f) { failures_.push_back(std::move(f)); }
    void record_note(std::string n) { notes_.push_back(std::move(n)); }

    [[nodiscard]] int checks() const noexcept { return checks_; }
    [[nodiscard]] const std::vector<Failure>& failures() const noexcept { return failures_; }
    [[nodiscard]] const std::vector<std::string>& notes() const noexcept { return notes_; }
    [[nodiscard]] bool passed() const noexcept { return failures_.empty(); }

private:
    int checks_ = 0;
    std::vector<Failure> failures_;
    std::vector<std::string> notes_;
};

using TestFn = void (*)();

struct TestCase {
    std::string suite;
    std::string name;
    const char* file = "";
    int line = 0;
    TestFn fn = nullptr;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

// The test currently executing. Set by the runner in test_main.cpp; null outside a test, which
// the assertion helpers treat as "print and abort" rather than dereferencing.
inline TestResult*& current_result() {
    static TestResult* r = nullptr;
    return r;
}

struct Registrar {
    Registrar(const char* suite, const char* name, const char* file, int line, TestFn fn) {
        registry().push_back(TestCase{suite, name, file, line, fn});
    }
};

// ─── Value printing ─────────────────────────────────────────────────────────────────────────

// Quote and escape a byte string so that two visually identical UTF-8 strings are
// distinguishable in a failure message. Printable ASCII passes through; everything else,
// including the high bytes of multi-byte sequences, becomes \xNN.
inline std::string quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c >= 0x20 && c < 0x7F) {
                    out.push_back(static_cast<char>(c));
                } else {
                    char buf[5];
                    std::snprintf(buf, sizeof(buf), "\\x%02X", c);
                    out += buf;
                }
        }
    }
    out.push_back('"');
    return out;
}

namespace detail {

template <typename T, typename = void>
struct is_streamable : std::false_type {};

template <typename T>
struct is_streamable<T, std::void_t<decltype(std::declval<std::ostream&>()
                                             << std::declval<const T&>())>> : std::true_type {};

}  // namespace detail

template <typename T>
std::string stringify(const T& v) {
    using U = std::remove_cv_t<std::remove_reference_t<T>>;
    if constexpr (std::is_same_v<U, bool>) {
        return v ? "true" : "false";
    } else if constexpr (std::is_same_v<U, std::string> || std::is_same_v<U, std::string_view>) {
        return quote(v);
    } else if constexpr (std::is_same_v<U, char> || std::is_same_v<U, signed char> ||
                         std::is_same_v<U, unsigned char>) {
        // Not as a character: these are bytes in this project, and "é" tells you less than 233.
        std::ostringstream os;
        os << static_cast<int>(v);
        return os.str();
    } else if constexpr (std::is_enum_v<U>) {
        std::ostringstream os;
        os << static_cast<std::int64_t>(static_cast<std::underlying_type_t<U>>(v));
        return os.str();
    } else if constexpr (detail::is_streamable<U>::value) {
        std::ostringstream os;
        os << v;
        return os.str();
    } else {
        return "<unprintable>";
    }
}

inline std::string stringify(const char* v) { return v ? quote(v) : std::string("nullptr"); }
inline std::string stringify(char* v) { return v ? quote(v) : std::string("nullptr"); }
inline std::string stringify(std::nullptr_t) { return "nullptr"; }

namespace detail {

// Comparison that does the right thing for the mixed string types this project passes around.
template <typename A, typename B>
bool equal(const A& a, const B& b) {
    return a == b;
}
inline bool equal(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) return a == b;
    return std::strcmp(a, b) == 0;
}

inline void report(bool ok, const char* file, int line, std::string expression, std::string detail,
                   bool fatal) {
    TestResult* r = current_result();
    if (r != nullptr) r->record_check();
    if (ok) return;
    if (r != nullptr) {
        r->record_failure(Failure{file, line, std::move(expression), std::move(detail)});
    } else {
        std::fprintf(stderr, "%s:%d: assertion outside a test: %s\n", file, line,
                     expression.c_str());
    }
    if (fatal) throw AbortTest{};
}

}  // namespace detail

// ─── Fixture access ─────────────────────────────────────────────────────────────────────────
//
// The cross-language conformance fixtures live in tests/fixtures/protocol/ and are shared with
// the Python suite (docs/protocol.md §12: "Neither suite may carry its own private copy").
// CMake defines KD_FIXTURE_DIR as an absolute path so the test binary is runnable from any
// working directory, including ctest's and the debugger's.

// Set by the runner from --fixtures, defaulting to the KD_FIXTURE_DIR compile definition.
inline std::string& fixture_dir() {
    static std::string dir =
#ifdef KD_FIXTURE_DIR
        KD_FIXTURE_DIR;
#else
        "tests/fixtures";
#endif
    return dir;
}

// Reads a file whole, in binary mode. Returns false if it could not be opened — fixture files
// are data the test depends on, so a missing one must fail loudly rather than yield "".
inline bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

inline std::string fixture_path(std::string_view relative) {
    std::string p = fixture_dir();
    if (!p.empty() && p.back() != '/' && p.back() != '\\') p.push_back('/');
    p.append(relative);
    return p;
}

// ─── Runner entry point (implemented in test_main.cpp) ──────────────────────────────────────

int run_all(int argc, char** argv);

}  // namespace kd::test

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Macros
// ═══════════════════════════════════════════════════════════════════════════════════════════

#define KD_TEST_CAT_(a, b) a##b
#define KD_TEST_CAT(a, b) KD_TEST_CAT_(a, b)

// Defines and registers a test. The function is `static`, so the same suite/name pair in two
// translation units links cleanly (and shows up twice in --list, which is the visible symptom
// of an accidental copy-paste).
#define KD_TEST(suite, name)                                                                  \
    static void KD_TEST_CAT(kd_test_fn_, KD_TEST_CAT(suite, KD_TEST_CAT(_, name)))();         \
    static ::kd::test::Registrar KD_TEST_CAT(kd_test_reg_,                                    \
                                             KD_TEST_CAT(suite, KD_TEST_CAT(_, name)))(       \
        #suite, #name, __FILE__, __LINE__,                                                    \
        &KD_TEST_CAT(kd_test_fn_, KD_TEST_CAT(suite, KD_TEST_CAT(_, name))));                 \
    static void KD_TEST_CAT(kd_test_fn_, KD_TEST_CAT(suite, KD_TEST_CAT(_, name)))()

#define KD_CHECK(expr) \
    ::kd::test::detail::report(static_cast<bool>(expr), __FILE__, __LINE__, #expr, {}, false)

#define KD_REQUIRE(expr) \
    ::kd::test::detail::report(static_cast<bool>(expr), __FILE__, __LINE__, #expr, {}, true)

#define KD_TEST_CMP_(a, b, op_ok, op_text, fatal)                                            \
    do {                                                                                     \
        auto&& kd_lhs_ = (a);                                                                \
        auto&& kd_rhs_ = (b);                                                                \
        const bool kd_ok_ = (op_ok);                                                          \
        std::string kd_detail_;                                                              \
        if (!kd_ok_) {                                                                        \
            kd_detail_ = "  lhs = " + ::kd::test::stringify(kd_lhs_) +                       \
                         "\n        rhs = " + ::kd::test::stringify(kd_rhs_);                \
        }                                                                                    \
        ::kd::test::detail::report(kd_ok_, __FILE__, __LINE__, #a " " op_text " " #b,        \
                                   std::move(kd_detail_), (fatal));                          \
    } while (0)

#define KD_CHECK_EQ(a, b) \
    KD_TEST_CMP_(a, b, ::kd::test::detail::equal(kd_lhs_, kd_rhs_), "==", false)
#define KD_REQUIRE_EQ(a, b) \
    KD_TEST_CMP_(a, b, ::kd::test::detail::equal(kd_lhs_, kd_rhs_), "==", true)
#define KD_CHECK_NE(a, b) \
    KD_TEST_CMP_(a, b, !::kd::test::detail::equal(kd_lhs_, kd_rhs_), "!=", false)
#define KD_REQUIRE_NE(a, b) \
    KD_TEST_CMP_(a, b, !::kd::test::detail::equal(kd_lhs_, kd_rhs_), "!=", true)
#define KD_CHECK_LT(a, b) KD_TEST_CMP_(a, b, (kd_lhs_ < kd_rhs_), "<", false)
#define KD_CHECK_LE(a, b) KD_TEST_CMP_(a, b, (kd_lhs_ <= kd_rhs_), "<=", false)
#define KD_CHECK_GT(a, b) KD_TEST_CMP_(a, b, (kd_lhs_ > kd_rhs_), ">", false)
#define KD_CHECK_GE(a, b) KD_TEST_CMP_(a, b, (kd_lhs_ >= kd_rhs_), ">=", false)

// Fatal variants of the ordering comparisons. They exist for the same reason KD_REQUIRE_EQ does:
// when a later assertion would dereference or index using the compared value, continuing past a
// failed ordering check turns one clear failure into a crash that names the wrong line.
#define KD_REQUIRE_LT(a, b) KD_TEST_CMP_(a, b, (kd_lhs_ < kd_rhs_), "<", true)
#define KD_REQUIRE_LE(a, b) KD_TEST_CMP_(a, b, (kd_lhs_ <= kd_rhs_), "<=", true)
#define KD_REQUIRE_GT(a, b) KD_TEST_CMP_(a, b, (kd_lhs_ > kd_rhs_), ">", true)
#define KD_REQUIRE_GE(a, b) KD_TEST_CMP_(a, b, (kd_lhs_ >= kd_rhs_), ">=", true)

#define KD_CHECK_NEAR(a, b, eps)                                                              \
    KD_TEST_CMP_(a, b, (std::fabs(static_cast<double>(kd_lhs_) - static_cast<double>(kd_rhs_)) \
                        <= static_cast<double>(eps)),                                         \
                 "~=", false)

#define KD_FAIL(msg) ::kd::test::detail::report(false, __FILE__, __LINE__, "KD_FAIL", (msg), false)

#define KD_REQUIRE_FAIL(msg) \
    ::kd::test::detail::report(false, __FILE__, __LINE__, "KD_REQUIRE_FAIL", (msg), true)

#define KD_INFO(msg)                                                          \
    do {                                                                      \
        if (::kd::test::TestResult* kd_r_ = ::kd::test::current_result()) {   \
            kd_r_->record_note((msg));                                        \
        }                                                                     \
    } while (0)
