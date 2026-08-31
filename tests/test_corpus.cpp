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
#include <vector>

#include "corpus.h"
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
