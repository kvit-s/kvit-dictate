// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Server-side stability — see stability.h for which side of the mark this is and why the
// Python tracker, not commit_engine.cpp, is the oracle.

#define KD_LOG_MODULE "asrd.stab"

#include "asrd/stability.h"

#include <algorithm>

#include "util/log.h"
#include "util/utf8.h"

namespace kd::asrd {

const char* to_string(ViolationKind k) noexcept {
    switch (k) {
        case ViolationKind::None:                     return "none";
        case ViolationKind::StableBytesOutOfRange:    return "stable_bytes_out_of_range";
        case ViolationKind::StableBytesNotOnBoundary: return "stable_bytes_not_on_boundary";
        case ViolationKind::StableBytesDecreased:     return "stable_bytes_decreased";
        case ViolationKind::StablePrefixMutated:      return "stable_prefix_mutated";
    }
    return "unknown";
}

std::string Violation::to_wire_message() const {
    // Deliberately no text. §8.3 step 3 does ask a *receiver* to log both texts; this is the
    // sender catching a violation in its own next message, so the only copy that matters is on
    // this side, where the privacy gate (KD_TRANSCRIPT) already exists.
    return std::string("server-side stability violation (") + to_string(kind) +
           "): utterance_id=" + std::to_string(utterance_id) +
           " previous_stable_bytes=" + std::to_string(previous_stable_bytes) +
           " incoming_stable_bytes=" + std::to_string(incoming_stable_bytes) +
           " diverge_offset=" + std::to_string(diverge_offset) + " published_prefix=<" +
           std::to_string(previous_prefix_bytes) + "B redacted> incoming_text=<" +
           std::to_string(incoming_text_bytes) + "B redacted>";
}

size_t first_difference(std::string_view a, std::string_view b) noexcept {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return i;
    }
    return n;
}

StabilityTracker::StabilityTracker(StabilityParams params, const IClock* clock)
    : params_(params), clock_(clock != nullptr ? clock : &owned_clock_) {
    if (params_.hypotheses_n < 1) params_.hypotheses_n = 1;
    window_.reserve(params_.hypotheses_n);
    candidate_since_ms_ = clock_->now_ms();
}

void StabilityTracker::reset(uint64_t utterance_id) {
    window_.clear();
    utterance_id_ = utterance_id;
    high_water_ = 0;
    stable_prefix_.clear();
    candidate_prefix_.clear();
    candidate_since_ms_ = clock_->now_ms();
    last_text_.clear();
    has_last_text_ = false;
}

Violation StabilityTracker::make(ViolationKind kind, std::string_view text, uint64_t incoming,
                                 uint64_t diverge) const {
    Violation v;
    v.kind = kind;
    v.utterance_id = utterance_id_;
    v.previous_stable_bytes = high_water_;
    v.incoming_stable_bytes = incoming;
    v.diverge_offset = diverge;
    v.previous_prefix_bytes = stable_prefix_.size();
    v.incoming_text_bytes = text.size();
    return v;
}

bool StabilityTracker::check(std::string_view text, uint64_t stable_bytes, Violation& v) const {
    // §8.2 requirement 1 — range.
    if (stable_bytes > text.size()) {
        v = make(ViolationKind::StableBytesOutOfRange, text, stable_bytes, text.size());
        return false;
    }

    // §8.2 requirement 3, first half — monotonic.
    if (stable_bytes < high_water_) {
        v = make(ViolationKind::StableBytesDecreased, text, stable_bytes, stable_bytes);
        return false;
    }

    // §8.2 requirement 3, second half — the one with teeth. A non-decreasing `stable_bytes`
    // whose underlying bytes changed has still retracted text the Client may already have typed
    // into the user's document.
    const size_t hw = static_cast<size_t>(high_water_);
    if (text.size() < hw || text.substr(0, hw) != stable_prefix_) {
        v = make(ViolationKind::StablePrefixMutated, text, stable_bytes,
                 first_difference(text, stable_prefix_));
        return false;
    }

    // §8.2 requirement 2 — grapheme-cluster boundary. Last, matching stability.py's order, so a
    // test that provokes two faults at once gets the same answer from both implementations.
    if (!utf8::is_grapheme_boundary(text, static_cast<size_t>(stable_bytes))) {
        v = make(ViolationKind::StableBytesNotOnBoundary, text, stable_bytes, stable_bytes);
        return false;
    }

    v = Violation{};
    return true;
}

bool StabilityTracker::observe(std::string_view text, uint64_t& stable_bytes, Violation& v) {
    // A ring of the last N hypotheses. N is 2 or 3 in every configuration the plan sweeps, so a
    // vector shifted by one is cheaper than a deque and reads as the algorithm does.
    if (window_.size() == params_.hypotheses_n) {
        window_.erase(window_.begin());
    }
    window_.emplace_back(text);
    last_text_.assign(text);
    has_last_text_ = true;
    return evaluate(text, stable_bytes, v);
}

bool StabilityTracker::poll(uint64_t& stable_bytes, Violation& v) {
    if (!has_last_text_) {
        stable_bytes = high_water_;
        v = Violation{};
        return true;
    }
    // Deliberately does NOT push the text into the window again: a repeated evaluation is not a
    // second opinion, and counting it as one would make N mean "N evaluations" rather than
    // "N hypotheses".
    return evaluate(last_text_, stable_bytes, v);
}

bool StabilityTracker::finalize(std::string_view text, uint64_t& stable_bytes, Violation& v) {
    if (!check(text, text.size(), v)) return false;
    high_water_ = text.size();
    stable_prefix_.assign(text);
    last_text_.assign(text);
    has_last_text_ = true;
    stable_bytes = high_water_;
    return true;
}

bool StabilityTracker::evaluate(std::string_view text, uint64_t& stable_bytes, Violation& v) {
    const Millis now = clock_->now_ms();

    // Step 0, which plan.md does not number because it should never fire: the incoming
    // hypothesis must still contain everything already published. If it does not, this message
    // cannot be sent at all.
    const size_t hw = static_cast<size_t>(high_water_);
    if (text.size() < hw || text.substr(0, hw) != stable_prefix_) {
        v = make(ViolationKind::StablePrefixMutated, text, high_water_,
                 first_difference(text, stable_prefix_));
        return false;
    }

    size_t candidate = 0;
    if (window_.size() >= params_.hypotheses_n) {
        // Step 2: longest common prefix over the window, on grapheme clusters. The N-way form
        // returns a byte length that is a cluster boundary in *every* input, which is the subtle
        // way to get this wrong when two hypotheses diverge inside a cluster.
        std::vector<std::string_view> items;
        items.reserve(window_.size());
        for (const std::string& w : window_) items.emplace_back(w);
        candidate = utf8::common_grapheme_prefix(items.data(), items.size());

        // Step 3, in the second of its two readings: cap the candidate at M clusters behind the
        // decode head, rather than subtracting M from the common prefix. See stability.h.
        const size_t head_cap =
            utf8::retract_graphemes(text, text.size(), params_.retract_clusters_m);
        candidate = std::min(candidate, head_cap);

        // Step 4: back to the last word boundary, holding trailing whitespace back.
        candidate = utf8::retract_to_word_boundary(text, candidate);
    }
    // Fewer than N hypotheses means nothing has been confirmed by a second opinion yet.
    // Publishing here would be publishing the first guess, which is the whole failure mode the
    // algorithm exists to avoid.

    // Step 5: the candidate must have been unchanged for T ms.
    //
    // The dwell is tracked on the candidate *bytes*, not on its length. Two different prefixes
    // of the same byte length are not the same candidate, and treating them as one would let a
    // rewrite masquerade as stability.
    const std::string_view candidate_prefix = text.substr(0, candidate);
    if (candidate_prefix != candidate_prefix_) {
        candidate_prefix_.assign(candidate_prefix);
        candidate_since_ms_ = now;
    }
    const bool matured = (now - candidate_since_ms_) >= params_.hold_ms_t;

    // Step 6: emit as a UTF-8 byte length, monotonically non-decreasing.
    uint64_t result = high_water_;
    if (matured && candidate > result) result = candidate;

    if (!check(text, result, v)) return false;

    high_water_ = result;
    stable_prefix_.assign(text.substr(0, static_cast<size_t>(result)));
    stable_bytes = result;
    return true;
}

}  // namespace kd::asrd
