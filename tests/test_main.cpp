// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// The kd_test runner. See kd_test.h for how to write a test; this file only runs them.
//
// Behaviour worth knowing:
//   * Tests are sorted by (suite, name) before running, so output order does not depend on the
//     linker's translation-unit order. A test that only passes in a particular order is a test
//     with hidden shared state, and sorting makes that reproducible rather than intermittent.
//   * "No tests matched the filter" exits non-zero. A typo in --suite that silently runs
//     nothing is how a green build stops meaning anything.
//   * Failures are printed as they happen *and* summarised at the end, because a long run's
//     first failure scrolls away.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include "kd_test.h"

namespace {

struct Options {
    std::vector<std::string> suites;  // empty = all
    std::vector<std::string> names;   // substring match; empty = all
    bool list = false;
    bool verbose = false;
    bool help = false;
};

void print_usage() {
    std::printf(
        "kd_tests — kvit-dictate C++ test suite\n"
        "\n"
        "Usage: kd_tests [options]\n"
        "  --suite <name>     run only this suite (repeatable, or comma-separated)\n"
        "  --name <substr>    run only tests whose name contains <substr> (repeatable)\n"
        "  --list             list registered tests and exit\n"
        "  --fixtures <dir>   directory holding tests/fixtures (default: compiled-in path)\n"
        "  --verbose, -v      print every test as it runs\n"
        "  --help, -h         this text\n"
        "\n"
        "Exit code is 0 only if every selected test passed and at least one test ran.\n");
}

void split_commas(std::string_view s, std::vector<std::string>& out) {
    size_t start = 0;
    while (start <= s.size()) {
        const size_t comma = s.find(',', start);
        const size_t end = (comma == std::string_view::npos) ? s.size() : comma;
        if (end > start) out.emplace_back(s.substr(start, end - start));
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
}

bool parse_args(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "kd_tests: %s requires an argument\n", what);
                return nullptr;
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") {
            opt.help = true;
        } else if (a == "--list") {
            opt.list = true;
        } else if (a == "--verbose" || a == "-v") {
            opt.verbose = true;
        } else if (a == "--suite") {
            const char* v = next("--suite");
            if (!v) return false;
            split_commas(v, opt.suites);
        } else if (a == "--name") {
            const char* v = next("--name");
            if (!v) return false;
            opt.names.emplace_back(v);
        } else if (a == "--fixtures") {
            const char* v = next("--fixtures");
            if (!v) return false;
            kd::test::fixture_dir() = v;
        } else {
            std::fprintf(stderr, "kd_tests: unknown option '%.*s'\n", static_cast<int>(a.size()),
                         a.data());
            return false;
        }
    }
    return true;
}

bool selected(const kd::test::TestCase& t, const Options& opt) {
    if (!opt.suites.empty()) {
        const bool hit = std::any_of(opt.suites.begin(), opt.suites.end(),
                                     [&](const std::string& s) { return s == t.suite; });
        if (!hit) return false;
    }
    if (!opt.names.empty()) {
        const bool hit =
            std::any_of(opt.names.begin(), opt.names.end(), [&](const std::string& s) {
                return t.name.find(s) != std::string::npos;
            });
        if (!hit) return false;
    }
    return true;
}

void print_failure(const kd::test::TestCase& t, const kd::test::Failure& f) {
    std::printf("  %s:%d: %s\n", f.file, f.line, f.expression.c_str());
    if (!f.detail.empty()) std::printf("        %s\n", f.detail.c_str());
    (void)t;
}

}  // namespace

namespace kd::test {

int run_all(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) return 2;
    if (opt.help) {
        print_usage();
        return 0;
    }

    std::vector<TestCase> tests = registry();
    std::sort(tests.begin(), tests.end(), [](const TestCase& a, const TestCase& b) {
        if (a.suite != b.suite) return a.suite < b.suite;
        return a.name < b.name;
    });

    if (opt.list) {
        for (const TestCase& t : tests) {
            std::printf("%s.%s\t%s:%d\n", t.suite.c_str(), t.name.c_str(), t.file, t.line);
        }
        std::printf("%zu test(s) registered\n", tests.size());
        return 0;
    }

    int ran = 0;
    int failed = 0;
    int total_checks = 0;
    std::vector<std::string> failed_names;

    const auto wall_start = std::chrono::steady_clock::now();

    for (const TestCase& t : tests) {
        if (!selected(t, opt)) continue;
        ++ran;

        TestResult result;
        current_result() = &result;

        if (opt.verbose) std::printf("[ RUN      ] %s.%s\n", t.suite.c_str(), t.name.c_str());

        const auto t0 = std::chrono::steady_clock::now();
        try {
            t.fn();
        } catch (const AbortTest&) {
            // KD_REQUIRE failed; the failure is already recorded.
        } catch (const std::exception& e) {
            result.record_failure(Failure{t.file, t.line, "unhandled std::exception", e.what()});
        } catch (...) {
            result.record_failure(Failure{t.file, t.line, "unhandled non-std exception", {}});
        }
        const auto t1 = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

        current_result() = nullptr;
        total_checks += result.checks();

        for (const std::string& n : result.notes()) std::printf("  note: %s\n", n.c_str());

        if (result.passed()) {
            if (opt.verbose) {
                std::printf("[       OK ] %s.%s (%lld ms, %d checks)\n", t.suite.c_str(),
                            t.name.c_str(), static_cast<long long>(ms), result.checks());
            }
        } else {
            ++failed;
            failed_names.push_back(t.suite + "." + t.name);
            std::printf("[     FAIL ] %s.%s\n", t.suite.c_str(), t.name.c_str());
            for (const Failure& f : result.failures()) print_failure(t, f);
        }
    }

    const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - wall_start)
                             .count();

    std::printf("\n");
    if (ran == 0) {
        // Deliberately an error: an empty run that reports success is worse than a failure,
        // because it looks like coverage.
        std::fprintf(stderr,
                     "kd_tests: no tests matched the filter (%zu registered). Refusing to "
                     "report success.\n",
                     tests.size());
        return 1;
    }

    std::printf("%d test(s), %d check(s), %d failed, %lld ms\n", ran, total_checks, failed,
                static_cast<long long>(wall_ms));
    if (failed != 0) {
        std::printf("failed:\n");
        for (const std::string& n : failed_names) std::printf("  %s\n", n.c_str());
        return 1;
    }
    std::printf("OK\n");
    return 0;
}

}  // namespace kd::test

int main(int argc, char** argv) { return kd::test::run_all(argc, argv); }
