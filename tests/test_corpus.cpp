// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// corpus.h's pruning rule. Tested rather than eyeballed for the reason `clamp_scroll_line` is:
// this is arithmetic that deletes the user's recordings, and an off-by-one in it is invisible in
// a directory walk and obvious in a table of cases.
//
// Input convention throughout: `ages` is oldest-first, matching what `prune_store` hands over.

#include <algorithm>
#include <string>
#include <vector>

#include "corpus.h"
#include "history.h"
#include "kd_test.h"

namespace {

bool has(const std::vector<size_t>& v, size_t i) {
    return std::find(v.begin(), v.end(), i) != v.end();
}

}  // namespace

KD_TEST(corpus, a_store_inside_both_bounds_loses_nothing) {
    const std::vector<uint64_t> sizes{10, 10, 10};
    const std::vector<uint32_t> ages{5, 3, 1};
    KD_CHECK(kd::corpus_prune_set(sizes, ages, 100, 30).empty());
}

KD_TEST(corpus, past_the_age_bound_goes_whatever_the_store_weighs) {
    const std::vector<uint64_t> sizes{1, 1, 1};
    const std::vector<uint32_t> ages{90, 40, 2};
    const auto doomed = kd::corpus_prune_set(sizes, ages, 1'000'000, 30);
    KD_REQUIRE_EQ(doomed.size(), size_t{2});
    KD_CHECK(has(doomed, 0));
    KD_CHECK(has(doomed, 1));
    KD_CHECK(!has(doomed, 2));
}

KD_TEST(corpus, over_the_byte_bound_prunes_oldest_first) {
    // 40 bytes held, 25 allowed: the two oldest go and the rest stays.
    const std::vector<uint64_t> sizes{10, 10, 10, 10};
    const std::vector<uint32_t> ages{9, 7, 5, 1};
    const auto doomed = kd::corpus_prune_set(sizes, ages, 25, 0);
    KD_REQUIRE_EQ(doomed.size(), size_t{2});
    KD_CHECK(has(doomed, 0));
    KD_CHECK(has(doomed, 1));
}

KD_TEST(corpus, the_newest_session_is_never_pruned_to_make_room_for_itself) {
    // One session, alone, and larger than the whole bound. Deleting it would delete the run in
    // progress — being briefly over the bound is the better failure, and the only one that does
    // not lose the recording being made right now.
    const std::vector<uint64_t> sizes{500};
    const std::vector<uint32_t> ages{0};
    KD_CHECK(kd::corpus_prune_set(sizes, ages, 10, 0).empty());

    // And with company: everything older can go, the newest still stays.
    const std::vector<uint64_t> big{100, 100, 500};
    const std::vector<uint32_t> old{9, 5, 0};
    const auto doomed = kd::corpus_prune_set(big, old, 10, 0);
    KD_CHECK(!has(doomed, 2));
    KD_CHECK_EQ(doomed.size(), size_t{2});
}

KD_TEST(corpus, a_zero_bound_disables_that_half_rather_than_wiping_the_store) {
    // The reading that matters: a zero in a config file must not be "everything is too old" or
    // "nothing may be kept". Both halves off means nothing is ever pruned.
    const std::vector<uint64_t> sizes{1'000'000, 1'000'000};
    const std::vector<uint32_t> ages{9999, 8888};
    KD_CHECK(kd::corpus_prune_set(sizes, ages, 0, 0).empty());
}

KD_TEST(corpus, a_busy_day_still_prunes_even_though_every_session_shares_an_age) {
    // Ages are whole days, so everything recorded today is age 0. An earlier rule spared every
    // entry tied at the minimum age, which on a day like this spared the entire store and left
    // the byte bound unable to bind until midnight. Only the newest — the last, since the caller
    // hands them over oldest-first — is protected.
    const std::vector<uint64_t> sizes{100, 100, 100, 100};
    const std::vector<uint32_t> ages{0, 0, 0, 0};
    const auto doomed = kd::corpus_prune_set(sizes, ages, 150, 0);
    KD_REQUIRE_EQ(doomed.size(), size_t{3});
    KD_CHECK(!has(doomed, 3));
}

KD_TEST(corpus, the_default_keeps_sessions_whatever_their_age) {
    // 60 days was the old default and it deleted 461 sessions nobody had asked to lose. Only the
    // disk bound may remove anything now, and only when it needs the room.
    const kd::CorpusConfig cfg;
    KD_CHECK_EQ(cfg.max_age_days, uint32_t{0});
    const std::vector<uint64_t> sizes{10, 10};
    const std::vector<uint32_t> ages{400, 1};
    KD_CHECK(kd::corpus_prune_set(sizes, ages, cfg.max_bytes, cfg.max_age_days).empty());
}

// ─── history.h ──────────────────────────────────────────────────────────────────────────────

namespace {

// The shape Corpus writes: compact JSON, one object per line, "t" after the body's own keys.
const char* const kSession =
    R"({"model":"m","target":"Code.exe","t":"start","ms":0,"wall":"2026-11-08T00:55:39"})" "\n"
    R"({"utt":1,"seq":1,"audio_pos_ms":900,"stable":0,"text":"hel","t":"partial","ms":900,"wall":"x"})" "\n"
    R"({"utt":1,"seq":2,"audio_pos_ms":2000,"stable":null,"text":"Hello there.","t":"final","ms":2000,"wall":"x"})" "\n"
    R"({"delivered":"Hello there, <b>you</b> & me.","delivered_bytes":28,"reason":"stopped","audio_ms":29392,"t":"end","ms":29392,"wall":"2026-11-08T00:56:08"})" "\n";

}  // namespace

KD_TEST(corpus, history_takes_the_delivered_text_and_the_start_line) {
    const kd::HistoryEntry e = kd::parse_history_entry("20261108-005539-s175", kSession);
    KD_CHECK(e.finished);
    KD_CHECK_EQ(e.text, std::string("Hello there, <b>you</b> & me."));
    KD_CHECK_EQ(e.target, std::string("Code.exe"));
    KD_CHECK_EQ(e.started, std::string("2026-11-08T00:55:39"));
    KD_CHECK_EQ(e.reason, std::string("stopped"));
    KD_CHECK_EQ(e.audio_ms, uint64_t{29392});
}

KD_TEST(corpus, history_of_a_killed_session_falls_back_to_its_finals) {
    // No end line, a torn last line, and no start line: the finals are still what was said, and
    // the folder name still says when.
    const std::string events =
        R"({"utt":1,"seq":2,"audio_pos_ms":2000,"stable":null,"text":"First.","t":"final","ms":2000,"wall":"x"})" "\n"
        R"({"utt":2,"seq":5,"audio_pos_ms":5000,"stable":null,"text":"Second.","t":"final","ms":5000,"wall":"x"})" "\n"
        R"({"utt":3,"seq":1,"audio_pos_ms":6)";
    const kd::HistoryEntry e = kd::parse_history_entry("20260908-134356-s15", events);
    KD_CHECK(!e.finished);
    KD_CHECK_EQ(e.text, std::string("First. Second."));
    KD_CHECK_EQ(e.reason, std::string("unfinished"));
    KD_CHECK_EQ(e.started, std::string("2026-09-08T13:43:56"));
    KD_CHECK_EQ(e.audio_ms, uint64_t{5000});
}

KD_TEST(corpus, the_history_page_escapes_text_and_groups_by_day) {
    std::vector<kd::HistoryEntry> entries;
    entries.push_back(kd::parse_history_entry("20261108-005539-s175", kSession));
    kd::HistoryEntry older;
    older.session = "20261107-090000-s3";
    older.started = "2026-11-07T09:00:00";
    older.target = "notepad.exe";
    older.text = "Yesterday's note.";
    older.finished = true;
    entries.push_back(older);
    kd::HistoryEntry silent;  // a hotkey pressed and released
    silent.session = "20261107-080000-s2";
    silent.started = "2026-11-07T08:00:00";
    silent.finished = true;
    entries.push_back(silent);

    const std::string page = kd::render_history_html(entries, "2026-11-08T01:30:00");
    // The dictated markup is text, not markup.
    KD_CHECK(page.find("Hello there, &lt;b&gt;you&lt;/b&gt; &amp; me.") != std::string::npos);
    KD_CHECK(page.find("<b>you</b>") == std::string::npos);
    KD_CHECK(page.find("Yesterday&#39;s note.") != std::string::npos);
    // One heading per day, with its weekday; 8 November 2026 is a Sunday.
    KD_CHECK(page.find("<h2>Sunday 8 November 2026</h2>") != std::string::npos);
    KD_CHECK(page.find("<h2>Saturday 7 November 2026</h2>") != std::string::npos);
    KD_CHECK(page.find("Sunday 8 November 2026") < page.find("Saturday 7 November 2026"));
    // The app is named without ".exe", and the empty session is counted rather than listed.
    KD_CHECK(page.find("<span class=\"app\">Code</span>") != std::string::npos);
    KD_CHECK(page.find("2 dictations, 7 Nov 2026 &ndash; 8 Nov 2026") != std::string::npos);
    KD_CHECK(page.find("1 session with nothing said not listed") != std::string::npos);
    KD_CHECK(page.find("data-dir=\"20261107-080000-s2\"") == std::string::npos);
}
