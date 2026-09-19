// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// The commit engine. Specification: commit_engine.h, plan.md §1.4, docs/protocol.md §8.
//
// The invariant, which nothing in this file may weaken:
//   Every byte sequence injected into the target must remain a prefix of every later hypothesis,
//   and of the final text, for that utterance. Committed text is never repaired.
//
// ─── How the invariant is enforced structurally, not by care ────────────────────────────────
//
// There is exactly one function in this file that produces bytes to inject — Impl::flush() —
// and exactly one that decides how far the injectable prefix reaches — Impl::candidate_length()
// followed by Impl::matured_prefix(). The cadence decision lives in cadence_allows(), a free
// function that is handed **only clock values**: it cannot see the hypotheses, the candidate, or
// the committed text, so there is no expression it could evaluate that would widen the prefix.
// That is the "cadence can never promote unstable text" rule made unrepresentable rather than
// merely documented — revision 1 of the plan had the rule written down and violated it anyway.
//
// ─── Two places where plan.md §1.4 needed interpretation, both flagged ──────────────────────
//
// 1. **Step 5's dwell is per-prefix, not per-candidate-value.** Read literally ("require the
//    candidate to have been unchanged for T ms"), a candidate that *grows* with every partial
//    would restart the timer on every partial, and a speaker who never pauses would never have
//    a single byte committed. That cannot be the intent — §1.4 exists to get text into the
//    document during speech. So what is timed is each prefix *length*: the engine keeps a small
//    list of (prefix length, first agreed at) checkpoints, and a length matures T ms after the
//    prefix it denotes was first agreed. Growing the candidate adds a checkpoint; it never
//    disturbs the ones below it. A retraction drops every checkpoint above the point of
//    disagreement. tests/test_commit_engine.cpp pins this with a stream of partials arriving
//    faster than T, which a literal implementation would never commit.
//
// 2. **Step 3's "M clusters behind the decode head" is applied to the candidate**, i.e.
//    `retract_graphemes(newest, lcp, M)`, not `min(lcp, len - M)`. Both readings satisfy the
//    stated rule ("at least M behind the head"); this one is the strictly more conservative of
//    the two, it is what src/util/utf8.h's own documentation of retract_graphemes describes, and
//    the failure direction of being too conservative is a delayed commit rather than a corrupted
//    document.

#include "commit_engine.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "util/utf8.h"

namespace kd {

// ─── to_string ──────────────────────────────────────────────────────────────────────────────

const char* to_string(Cadence c) noexcept {
    switch (c) {
        case Cadence::Immediate: return "immediate";
        case Cadence::Periodic: return "periodic";
        case Cadence::EndOfSession: return "end-of-session";
    }
    return "?";
}

const char* to_string(ViolationKind k) noexcept {
    switch (k) {
        case ViolationKind::None: return "none";
        case ViolationKind::StableBytesOutOfRange: return "stable_bytes_out_of_range";
        case ViolationKind::StableBytesNotOnBoundary: return "stable_bytes_not_on_boundary";
        case ViolationKind::StableBytesDecreased: return "stable_bytes_decreased";
        case ViolationKind::StablePrefixMutated: return "stable_prefix_mutated";
        case ViolationKind::CommittedTextContradicted: return "committed_text_contradicted";
    }
    return "?";
}

namespace {

// ─── Small local helpers ────────────────────────────────────────────────────────────────────

constexpr bool is_ascii_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// Split on ASCII whitespace, returning (offset, length) so the original spelling is recoverable.
void split_words(std::string_view s, std::vector<std::pair<size_t, size_t>>& out) {
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && is_ascii_space(s[i])) ++i;
        const size_t start = i;
        while (i < s.size() && !is_ascii_space(s[i])) ++i;
        if (i > start) out.emplace_back(start, i - start);
    }
}

// Comparison key for salvage: ASCII case-folded, edge punctuation removed.
//
// Deliberately crude, and crude in the same way the Python base class's `_norm_word` is: the
// revision this has to see through is "you" against "you?" and "how" against "How", which is what
// a batch model produces when a later request tells it where the sentence ended. Anything
// cleverer would silently align the wrong words. Non-ASCII compares verbatim — folding it
// properly needs a case table this project does not carry, and a failed match falls into the
// conservative branch rather than a wrong one.
std::string salvage_key(std::string_view w) {
    static constexpr std::string_view kEdge = ".,;:!?\"'()[]{}";
    size_t b = 0;
    size_t e = w.size();
    while (b < e && kEdge.find(w[b]) != std::string_view::npos) ++b;
    while (e > b && kEdge.find(w[e - 1]) != std::string_view::npos) --e;
    std::string out(w.substr(b, e - b));
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// One-line, single-quoted, control-character-free rendering, so a violation record cannot break
// the log format with a newline the model happened to emit.
std::string escape_for_log(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    char buf[5];
                    std::snprintf(buf, sizeof(buf), "\\x%02X", c);
                    out += buf;
                } else {
                    // High bytes pass through: the log file is UTF-8 and a mangled multi-byte
                    // sequence in a diagnostic is worse than the character itself.
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

size_t graphemes_or_zero(std::string_view s) noexcept {
    const size_t n = utf8::count_graphemes(s);
    return n == utf8::kNpos ? size_t{0} : n;
}

// First differing byte offset, or min(size) if one is a byte prefix of the other.
size_t first_difference(std::string_view a, std::string_view b) noexcept {
    const size_t n = std::min(a.size(), b.size());
    size_t i = 0;
    while (i < n && a[i] == b[i]) ++i;
    return i;
}

// **Byte** prefix, deliberately. The invariant is about the byte sequence already delivered to
// the target: if `whole` begins with those exact bytes, appending the remainder reproduces
// `whole` exactly, and nothing needs repairing. Grapheme reasoning belongs on the *commit*
// side (never emit an offset that splits a cluster), not on the detection side — a hypothesis
// that adds a combining mark to a letter we already typed is an extension, not a contradiction.
bool starts_with_bytes(std::string_view whole, std::string_view prefix) noexcept {
    return whole.size() >= prefix.size() &&
           whole.compare(0, prefix.size(), prefix) == 0;
}

ViolationKind kind_of(proto::StabilityCheck c) noexcept {
    switch (c) {
        case proto::StabilityCheck::Ok: return ViolationKind::None;
        case proto::StabilityCheck::OutOfRange: return ViolationKind::StableBytesOutOfRange;
        case proto::StabilityCheck::NotGraphemeBoundary: return ViolationKind::StableBytesNotOnBoundary;
        case proto::StabilityCheck::Decreased: return ViolationKind::StableBytesDecreased;
        case proto::StabilityCheck::PrefixMutated: return ViolationKind::StablePrefixMutated;
    }
    return ViolationKind::None;
}

// ─── THE CADENCE SEAM ───────────────────────────────────────────────────────────────────────
//
// Everything cadence is allowed to know. Note what is *not* in this signature: no text, no
// hypothesis window, no candidate length, no committed length, no engine state at all. A future
// edit that wanted cadence to influence *which* prefix is committed would have to add a
// parameter here, which is a change a reviewer cannot miss. plan.md §9 records that revision 1
// shipped a `periodic-500ms` policy that committed on a timer regardless of stability; this
// signature is the reason that cannot be reintroduced by accident.
//
// Returns whether the clock permits a flush now. It can only ever say "not yet".
bool cadence_allows(const StabilityParams& p, Millis now, Millis last_flush) noexcept {
    if (p.cadence == Cadence::Immediate) return true;
    // Nothing flushes early. The engine still *produces* the op — `on_final` does not consult
    // this gate — and App holds it until the session ends. See Cadence::EndOfSession.
    if (p.cadence == Cadence::EndOfSession) return false;
    if (p.cadence_ms == 0) return true;
    return now - last_flush >= static_cast<Millis>(p.cadence_ms);
}

// A prefix length that has been continuously agreed since `since`.
struct Checkpoint {
    size_t len = 0;
    Millis since = 0;
};

}  // namespace

// ─── salvage_tail ───────────────────────────────────────────────────────────────────────────
//
// See CommitEngine::Result::salvage for what this is for and why it is not `op`.
std::string salvage_tail(std::string_view committed, std::string_view incoming) {
    std::vector<std::pair<size_t, size_t>> cw;
    std::vector<std::pair<size_t, size_t>> iw;
    split_words(committed, cw);
    split_words(incoming, iw);

    // No words at all in the incoming text. The "agree about nothing" branch below would return
    // it verbatim, which for a whitespace-only hypothesis means appending whitespace to the
    // user's document for no reason.
    if (iw.empty()) return std::string();

    size_t p = 0;
    while (p < cw.size() && p < iw.size() &&
           salvage_key(committed.substr(cw[p].first, cw[p].second)) ==
               salvage_key(incoming.substr(iw[p].first, iw[p].second))) {
        ++p;
    }

    // They agree about nothing. Append all of it rather than dropping it: duplicated words are
    // visible and the user can delete them, whereas words silently discarded are gone and the
    // user never learns they were said. The Python driver's `_scrolled_off_words` makes the same
    // call at its own seam, for the same reason.
    if (p == 0) return std::string(incoming);
    if (p >= iw.size()) return std::string();  // incoming adds nothing after the agreement
    return std::string(incoming.substr(iw[p].first));
}

// ─── IcerCounters ───────────────────────────────────────────────────────────────────────────

double IcerCounters::icer_per_1000() const noexcept {
    // A rate with no denominator is not 100% — reporting it as such would poison the bake-off
    // table in plan.md §4, where ICER is the gating metric and a run that committed nothing
    // would otherwise look like the worst backend in the matrix.
    if (committed_graphemes == 0) return 0.0;
    return (static_cast<double>(erroneous_graphemes) * 1000.0) /
           static_cast<double>(committed_graphemes);
}

// ─── Violation ──────────────────────────────────────────────────────────────────────────────

std::string Violation::to_log_line() const {
    // NOTE FOR CALLERS: this line contains recognised text. docs/protocol.md §8.3 step 3 requires
    // both texts be logged, and plan.md §1.9 requires recognised text never reach the diagnostic
    // channel. Route it to KD_TRANSCRIPT(kind="violation"), never to KD_LOG_*; use
    // kd::log::redact() if a diagnostic line needs to mention that a violation happened.
    std::string s = "stability_violation kind=";
    s += to_string(kind);
    s += " session=";
    s += session_id;
    s += " utterance=";
    s += std::to_string(utterance_id);
    s += " seq=";
    s += std::to_string(seq_previous);
    s += "->";
    s += std::to_string(seq_current);
    s += " diverge_offset=";
    s += std::to_string(diverge_offset);
    s += " prev_stable=";
    s += std::to_string(previous_stable_bytes);
    s += " incoming_stable=";
    s += std::to_string(incoming_stable_bytes);
    s += " committed=\"";
    s += escape_for_log(committed_text);
    s += "\" incoming=\"";
    s += escape_for_log(incoming_text);
    s += "\"";
    return s;
}

proto::ProtocolError Violation::to_protocol_error() const {
    proto::ProtocolError e;
    e.code = proto::ErrorCode::stability_violation;
    e.fatal = false;  // §11.1: non-fatal codes REQUIRE session_id, set below.
    e.session_id = session_id;
    e.at_offset = static_cast<uint64_t>(diverge_offset);

    // Prose only, and deliberately **without** either text. §8.3 wants both texts logged
    // locally; putting them back on the wire would send the user's dictated words across the
    // pipe a second time for no diagnostic gain — the peer is the party that produced them.
    e.msg = "stability violation: ";
    e.msg += to_string(kind);
    e.msg += " at byte ";
    e.msg += std::to_string(diverge_offset);
    e.msg += " (utterance ";
    e.msg += std::to_string(utterance_id);
    e.msg += ", seq ";
    e.msg += std::to_string(seq_previous);
    e.msg += "->";
    e.msg += std::to_string(seq_current);
    e.msg += "); committed text preserved, nothing repaired";
    return e;
}

// ─── Impl ───────────────────────────────────────────────────────────────────────────────────

struct CommitEngine::Impl {
    const IClock* clock = nullptr;
    StabilityParams params;

    std::string session_id;

    // Per utterance. Everything below the counters is cleared at an utterance boundary
    // (plan.md §1.4 step 7).
    uint64_t utterance_id = 0;
    bool utterance_open = false;

    std::vector<std::string> window;  // last N hypotheses, oldest first
    std::string newest;               // window.back(), or empty
    uint64_t newest_stable = 0;       // the server's stable_bytes for `newest`

    std::string candidate;            // the current stability candidate, as bytes
    std::vector<Checkpoint> checkpoints;  // ascending len; `since` non-decreasing with len

    std::string committed;            // this utterance's committed text, in order
    Millis last_flush_ms = 0;         // reference point for Cadence::Periodic

    // §8.2 high-water mark for the current utterance.
    std::string prev_text;
    uint64_t prev_stable = 0;
    uint64_t prev_seq = 0;

    // Session-level.
    bool session_has_committed = false;  // drives CommitOp::leading_space (plan.md §1.6)
    std::optional<Violation> latched;
    bool latched_scope_utterance = false;

    IcerCounters counters;

    // ── state transitions ───────────────────────────────────────────────────────────────────

    void clear_utterance(uint64_t id) {
        utterance_id = id;
        utterance_open = true;
        window.clear();
        newest.clear();
        newest_stable = 0;
        candidate.clear();
        checkpoints.clear();
        committed.clear();
        prev_text.clear();
        prev_stable = 0;
        prev_seq = 0;
        last_flush_ms = clock != nullptr ? clock->now_ms() : Millis{0};
        // Only the speculative harness mode un-latches here; see latch().
        if (latched.has_value() && latched_scope_utterance) latched.reset();
    }

    void clear_session() {
        session_id.clear();
        clear_utterance(0);
        utterance_open = false;
        utterance_id = 0;
        session_has_committed = false;
        latched.reset();
        latched_scope_utterance = false;
    }

    // ── eligibility (stability only — nothing here may consult the cadence knobs) ───────────

    // Steps 1–4 and the step-6 cap. Returns a byte length into `newest`.
    [[nodiscard]] size_t candidate_length() const {
        if (newest.empty() || window.empty()) return 0;

        // ── SPECULATIVE, UNSAFE, NEVER SHIPPED ──────────────────────────────────────────────
        // plan.md §1.4 permits this only "to measure the achievable latency ceiling" in the
        // bench harness. It knowingly commits text the model has not stabilised, which means it
        // knowingly corrupts the user's document; it exists so the matrix in plan.md §4 has a
        // lower bound on commit lag to compare the safe policies against. It is excluded from
        // every default (StabilityParams::speculative_unsafe defaults to false) and
        // Settings::validate() rejects a settings file that sets it, so it is unreachable from
        // the UI. Do not "fix" the safe path by borrowing anything from this branch.
        if (params.speculative_unsafe) {
            return utf8::floor_grapheme(newest, newest.size());
        }

        // Step 1: the last N hypotheses. Fewer than N means there is no agreement to observe
        // yet — one hypothesis agreeing with itself is not evidence of stability — so nothing is
        // eligible. That is the conservative direction: a delayed commit, never a wrong one.
        const size_t n = params.hypotheses_n == 0 ? size_t{1} : static_cast<size_t>(params.hypotheses_n);
        if (window.size() < n) return 0;

        // Step 2: longest common prefix over the window, on grapheme clusters. The common prefix
        // of a set equals the minimum of the pairwise common prefixes against any one member, so
        // the newest hypothesis serves as the reference and every offset below is an offset into
        // it.
        size_t lcp = newest.size();
        for (size_t i = window.size() - n; i < window.size(); ++i) {
            lcp = std::min(lcp, utf8::common_grapheme_prefix(newest, window[i]));
        }
        lcp = utf8::floor_grapheme(newest, lcp);

        // Step 3: retract M clusters. See the note at the top of this file about which of the
        // two readings of "M clusters behind the decode head" this is.
        size_t off = utf8::retract_graphemes(newest, lcp, static_cast<size_t>(params.retract_clusters_m));

        // Step 6, first half: never exceed the server's own mark. The server promises never to
        // retract below `stable_bytes`, and our local computation is an independent second
        // opinion; committing the minimum means a backend that lies about stability costs
        // latency rather than document corruption.
        if (params.honour_server_stable_bytes) {
            off = std::min(off, static_cast<size_t>(newest_stable));
            off = utf8::floor_grapheme(newest, off);
        }

        // Step 4, applied exactly once and last: retract to the last word boundary and hold back
        // trailing whitespace. It must be last because it is not idempotent — its contract
        // (src/util/utf8.h) is "last separator-ending boundary at or below `offset`, then trim
        // trailing whitespace", so feeding its own output back in would retract a second word.
        return utf8::retract_to_word_boundary(newest, off);
    }

    // Step 5. Records that `cand` (a prefix of `newest`) is the candidate as of `now`, keeping
    // the per-prefix dwell timers described at the top of this file.
    void note_candidate(std::string_view cand, Millis now) {
        const size_t agree = first_difference(candidate, cand);
        const size_t len = cand.size();

        // If this exact length was already an agreed prefix, it keeps its original start time:
        // a candidate that shrinks back onto ground it already held has not restarted its dwell.
        Millis since = now;
        if (len <= agree) {
            for (const Checkpoint& cp : checkpoints) {
                if (cp.len >= len) {  // `since` is non-decreasing in len, so this is the earliest
                    since = cp.since;
                    break;
                }
            }
        }

        std::vector<Checkpoint> kept;
        kept.reserve(checkpoints.size() + 1);
        for (const Checkpoint& cp : checkpoints) {
            // Survivors must still be prefixes of the new candidate.
            if (cp.len < len && cp.len <= agree) kept.push_back(cp);
        }
        kept.push_back(Checkpoint{len, since});
        checkpoints.swap(kept);
        candidate.assign(cand);
    }

    // The largest checkpointed prefix whose dwell has elapsed. This is the whole of the
    // eligibility answer: steps 1–6 with the clock consulted only for step 5's T-millisecond
    // hold, never for cadence.
    [[nodiscard]] size_t matured_prefix(Millis now) const noexcept {
        if (params.speculative_unsafe) return candidate.size();  // unsafe mode: no dwell either
        size_t best = 0;
        for (const Checkpoint& cp : checkpoints) {
            if (now - cp.since >= static_cast<Millis>(params.hold_ms_t) && cp.len > best) {
                best = cp.len;
            }
        }
        return best;
    }

    // ── flushing (the only producer of bytes to inject) ─────────────────────────────────────

    [[nodiscard]] CommitOp make_op(size_t target) {
        CommitOp op;
        op.text = newest.substr(committed.size(), target - committed.size());
        op.utterance_offset = static_cast<uint64_t>(committed.size());
        op.utterance_id = utterance_id;
        op.session_id = session_id;
        // plan.md §1.6: "resuming starts a new utterance appended after a space". The space is
        // not part of the utterance's committed text and does not move utterance_offset — the
        // injector emits it, the offsets stay in the utterance's own coordinate system.
        op.leading_space = (op.utterance_offset == 0) && session_has_committed;

        committed.append(op.text);
        counters.committed_graphemes += static_cast<uint64_t>(graphemes_or_zero(op.text));
        session_has_committed = true;

        // Checkpoints at or below the committed mark can never produce another op.
        std::vector<Checkpoint> kept;
        kept.reserve(checkpoints.size());
        for (const Checkpoint& cp : checkpoints) {
            if (cp.len > committed.size()) kept.push_back(cp);
        }
        checkpoints.swap(kept);
        return op;
    }

    // Emit bytes [committed, target) of the newest hypothesis. The only caller-supplied target
    // is on_final's, and a `final` is entirely stable by definition (docs/protocol.md §6.2) —
    // which is why it is the one path that does not go through matured_prefix().
    std::optional<CommitOp> commit_to(size_t target, Millis now) {
        if (target <= committed.size()) return std::nullopt;

        // Defensive: both of these are guaranteed by the checks in on_partial/on_final, and if
        // either ever failed the honest response is to inject nothing rather than to slice a
        // string at an offset whose meaning we no longer trust.
        if (target > newest.size()) return std::nullopt;
        if (!starts_with_bytes(newest, committed)) return std::nullopt;

        last_flush_ms = now;
        return make_op(target);
    }

    std::optional<CommitOp> flush(Millis now) {
        const size_t target = matured_prefix(now);
        if (target <= committed.size()) return std::nullopt;

        // The cadence gate. Note it is asked *after* the eligible target is known, is handed
        // nothing but clock values, and can only answer "not yet" — it never touches `target`.
        if (!params.speculative_unsafe && !cadence_allows(params, now, last_flush_ms)) {
            return std::nullopt;
        }
        return commit_to(target, now);
    }

    // ── violations ──────────────────────────────────────────────────────────────────────────

    Violation latch(ViolationKind kind, std::string_view incoming, uint64_t incoming_stable,
                    uint64_t seq, size_t diverge) {
        Violation v;
        v.kind = kind;
        v.session_id = session_id;
        v.utterance_id = utterance_id;
        v.seq_previous = prev_seq;
        v.seq_current = seq;
        v.diverge_offset = diverge;
        // "What we already typed" is the honest text to report: the previous stable prefix is
        // interesting to the protocol, but the committed text is what the user is looking at.
        v.committed_text = committed;
        v.incoming_text = std::string(incoming);
        v.previous_stable_bytes = prev_stable;
        v.incoming_stable_bytes = incoming_stable;

        ++counters.irrevocable_events;

        // docs/protocol.md §8.3 step 2: abort the *session*, not the connection. So the latch is
        // session-scoped and an utterance boundary does not clear it — a Server that produced one
        // inadmissible revision does not get to escape the abort by starting a new utterance.
        //
        // The one exception is the speculative harness mode, where contradictions are the
        // measurement rather than a fault: there the latch is utterance-scoped so a bench run
        // keeps producing ICER numbers past the first (expected) violation.
        latched = v;
        latched_scope_utterance = params.speculative_unsafe;
        return v;
    }
};

// ─── CommitEngine ───────────────────────────────────────────────────────────────────────────

CommitEngine::CommitEngine(const IClock& clock, StabilityParams params) : impl_(new Impl()) {
    impl_->clock = &clock;
    impl_->params = params;
    impl_->last_flush_ms = clock.now_ms();
}

CommitEngine::~CommitEngine() { delete impl_; }

void CommitEngine::begin_session(std::string_view session_id) {
    impl_->clear_session();
    impl_->session_id.assign(session_id);
    impl_->last_flush_ms = impl_->clock->now_ms();
}

void CommitEngine::begin_utterance(uint64_t utterance_id) { impl_->clear_utterance(utterance_id); }

void CommitEngine::abandon_utterance() {
    // Deliberately does NOT touch `committed` or the counters: committed text stays in the
    // user's document (that is the invariant), and docs/protocol.md §10.3 step 2 says the
    // unstable tail is lost, which is the honest outcome. Only the machinery that could produce
    // a *further* op is torn down.
    //
    // `utterance_open` is what makes this stick rather than merely tidy. on_tick() returns an
    // empty Result while it is false, so a caller that has taken the tail for itself stops being
    // offered it back on every cadence tick — and step 5's dwell, which matures on the clock
    // alone and needs no new hypothesis to move it, can no longer promote a single byte of it.
    Impl& s = *impl_;
    s.utterance_open = false;
    s.window.clear();
    s.newest.clear();
    s.newest_stable = 0;
    s.candidate.clear();
    s.checkpoints.clear();
}

void CommitEngine::end_session() { abandon_utterance(); }

void CommitEngine::reset() {
    // Counters survive: they are per-backend/per-configuration accumulations across sessions
    // (plan.md §4), and reset_counters() is the way to clear them between matrix cells.
    IcerCounters keep = impl_->counters;
    impl_->clear_session();
    impl_->counters = keep;
}

// ─── Feeding it ─────────────────────────────────────────────────────────────────────────────

CommitEngine::Result CommitEngine::on_partial(const proto::PartialMsg& m) {
    Impl& s = *impl_;
    Result r;

    if (s.latched.has_value()) {
        // Sticky: the session is aborted, but reporting `ok()` here would let a caller that
        // ignored the first violation quietly resume injecting. No counter moves — one incident
        // is one incident.
        r.violation = s.latched;
        return r;
    }

    if (s.session_id.empty()) s.session_id = m.session_id;
    if (!s.utterance_open || s.utterance_id != m.utterance_id) {
        // An utterance boundary the caller did not announce. Resetting is plan.md §1.4 step 7
        // and is the safe reading: mixing hypotheses from two utterances in one stability window
        // would compute a common prefix across a boundary that has no meaning.
        s.clear_utterance(m.utterance_id);
    }

    const Millis now = s.clock->now_ms();

    // ── Check 1: does this hypothesis contradict text already in the user's document? ───────
    //
    // Checked before the §8.2 checks because it is the one that has already cost the user
    // something. A fixture-018-style mutation below the published mark that we had also
    // committed is reported as CommittedTextContradicted rather than StablePrefixMutated: both
    // are true, and the one naming the document is the more useful of the two.
    if (!starts_with_bytes(m.text, s.committed)) {
        const size_t diverge = first_difference(s.committed, m.text);
        // ICER: "committed graphemes that differ from the final hypothesis". Everything from the
        // divergence onward is wrong in the document and cannot be taken back.
        const size_t floor = utf8::floor_grapheme(s.committed, diverge);
        s.counters.erroneous_graphemes +=
            static_cast<uint64_t>(graphemes_or_zero(std::string_view(s.committed).substr(floor)));
        r.violation = s.latch(ViolationKind::CommittedTextContradicted, m.text, m.stable_bytes,
                              m.seq, diverge);
        // No `op`. Not now, not later: there is no repair path in this module, and `op` carries a
        // guarantee a rejected message cannot honour.
        //
        // `salvage` is the separate, weaker thing — the words of this hypothesis that follow
        // everything it still agrees with, so the user keeps the rest of their sentence instead
        // of losing it at the point of contradiction. It appends and never rewrites, so the
        // document invariant is exactly as before; what changes is that the words after the
        // divergence stop being silently discarded.
        if (!s.committed.empty()) {
            std::string tail = salvage_tail(s.committed, m.text);
            if (!tail.empty()) {
                CommitOp op;
                op.text = std::move(tail);
                op.utterance_offset = static_cast<uint64_t>(s.committed.size());
                op.utterance_id = s.utterance_id;
                op.session_id = s.session_id;
                // The committed text ends mid-sentence, so the salvaged tail needs a separator.
                op.leading_space = true;
                r.salvage = std::move(op);
            }
        }
        return r;
    }

    // ── Check 2: docs/protocol.md §8.2, all three requirements ──────────────────────────────
    //
    // Clamping an out-of-range or non-monotonic value is forbidden rather than discouraged
    // (§8.3): clamping hides exactly the defect ICER exists to measure.
    size_t diverge = 0;
    const proto::StabilityCheck chk =
        proto::check_stability(s.prev_text, s.prev_stable, m.text, m.stable_bytes, &diverge);
    if (chk != proto::StabilityCheck::Ok) {
        r.violation = s.latch(kind_of(chk), m.text, m.stable_bytes, m.seq, diverge);
        return r;
    }

    // ── Bookkeeping ─────────────────────────────────────────────────────────────────────────

    // Flicker: churn the user never sees, because it happens above the committed prefix and the
    // overlay absorbs it. Recorded for comparison with ICER; explicitly not the gating metric.
    if (!s.newest.empty() && !starts_with_bytes(m.text, s.newest)) {
        ++s.counters.flicker_revisions;
    }

    const size_t keep = s.params.hypotheses_n == 0 ? size_t{1} : static_cast<size_t>(s.params.hypotheses_n);
    s.window.push_back(m.text);
    while (s.window.size() > keep) s.window.erase(s.window.begin());

    s.newest = m.text;
    s.newest_stable = m.stable_bytes;
    s.prev_text = m.text;
    s.prev_stable = m.stable_bytes;
    s.prev_seq = m.seq;

    // ── Eligibility, then cadence. In that order, in that many statements. ──────────────────
    const size_t cand = s.candidate_length();
    s.note_candidate(std::string_view(s.newest).substr(0, cand), now);
    r.op = s.flush(now);

    // DEVIATION FROM THE HEADER COMMENT, stated rather than left to be discovered: commit_engine.h
    // describes pending_text as "everything above the eligible prefix". It is everything above
    // the **committed** prefix. Under Cadence::Periodic, or while step 5's dwell is running,
    // text can be eligible and not yet injected; rendering the overlay from the eligible mark
    // would make that text invisible in both places at once — a visible hole between the
    // document and the preview. `committed + pending == the hypothesis` is the only definition
    // that keeps the user seeing all of what the model said.
    r.pending_text = m.text.substr(std::min(s.committed.size(), m.text.size()));
    return r;
}

CommitEngine::Result CommitEngine::on_final(const proto::FinalMsg& m) {
    Impl& s = *impl_;
    Result r;

    if (s.latched.has_value()) {
        r.violation = s.latched;
        return r;
    }

    if (s.session_id.empty()) s.session_id = m.session_id;
    if (!s.utterance_open || s.utterance_id != m.utterance_id) s.clear_utterance(m.utterance_id);

    const Millis now = s.clock->now_ms();
    const uint64_t full = static_cast<uint64_t>(m.text.size());

    // §6.2: a `final` is entirely stable, so its effective stable_bytes is len(utf8(text)). If
    // the sender supplied the optional field it MUST equal that; a mismatch is a sender defect
    // and §8.3's handling applies (preserve, report, count) rather than "take the smaller one".
    // The kind is StableBytesOutOfRange in both directions, which is a slight stretch of the
    // name in the `< len` case; adding a sixth ViolationKind for it would buy nothing a reader
    // of the log line does not already get from the two texts.
    if (m.stable_bytes.has_value() && *m.stable_bytes != full) {
        r.violation = s.latch(ViolationKind::StableBytesOutOfRange, m.text, *m.stable_bytes, m.seq,
                              static_cast<size_t>(std::min<uint64_t>(*m.stable_bytes, full)));
        return r;
    }

    // The case plan.md §1.4's violation-handling paragraph is written about, and the one that
    // feeds ICER: a final that is not an extension of what we already typed.
    if (!starts_with_bytes(m.text, s.committed)) {
        const size_t diverge = first_difference(s.committed, m.text);
        const size_t floor = utf8::floor_grapheme(s.committed, diverge);
        s.counters.erroneous_graphemes +=
            static_cast<uint64_t>(graphemes_or_zero(std::string_view(s.committed).substr(floor)));
        r.violation =
            s.latch(ViolationKind::CommittedTextContradicted, m.text, full, m.seq, diverge);
        return r;
    }

    size_t dv = 0;
    const proto::StabilityCheck chk =
        proto::check_stability(s.prev_text, s.prev_stable, m.text, full, &dv);
    if (chk != proto::StabilityCheck::Ok) {
        r.violation = s.latch(kind_of(chk), m.text, full, m.seq, dv);
        return r;
    }

    s.newest = m.text;
    s.newest_stable = full;
    s.prev_text = m.text;
    s.prev_stable = full;
    s.prev_seq = m.seq;

    // Everything except trailing whitespace, which stays held back even here: the next utterance
    // is appended after a space (plan.md §1.6, CommitOp::leading_space), and committing this
    // one's trailing space too would double it.
    const size_t target = utf8::trim_trailing_whitespace(m.text, m.text.size());
    s.note_candidate(std::string_view(s.newest).substr(0, target), now);

    // Neither the dwell nor the cadence applies here, and for different reasons. The T-ms hold
    // (step 5) exists to see whether a hypothesis is about to be revised; a `final` is the
    // statement that it will not be. Cadence exists to pace injection; an utterance has exactly
    // one final, and delaying it by up to `cadence_ms` would strand text in the overlay until
    // some later tick the caller may never make. Cadence still only ever *delayed* this text —
    // it never chose it, which is the property that matters.
    r.op = s.commit_to(target, now);

    r.pending_text = m.text.substr(std::min(s.committed.size(), m.text.size()));
    return r;
}

CommitEngine::Result CommitEngine::on_tick() {
    Impl& s = *impl_;
    Result r;
    // A tick is not a message. If the session is already aborted there is nothing new to report
    // and re-reporting the latched violation on every timer tick would turn one incident into a
    // stream of them; the caller was told when the message arrived.
    if (s.latched.has_value() || !s.utterance_open) return r;

    const Millis now = s.clock->now_ms();

    // Note what is NOT here: no recomputation of the candidate. No hypothesis has arrived, so
    // eligibility cannot have widened — only step 5's dwell and the cadence clock can have
    // matured. This is the whole of what a tick is for.
    r.op = s.flush(now);

    r.pending_text = s.newest.substr(std::min(s.committed.size(), s.newest.size()));
    return r;
}

// ─── Inspecting it ──────────────────────────────────────────────────────────────────────────

std::string_view CommitEngine::committed_text() const noexcept { return impl_->committed; }

uint64_t CommitEngine::committed_bytes() const noexcept {
    return static_cast<uint64_t>(impl_->committed.size());
}

uint64_t CommitEngine::eligible_prefix() const noexcept {
    const Impl& s = *impl_;
    // Everything except cadence — steps 1–6 including the T-millisecond dwell. Two engines
    // configured with different cadences and fed the same stream report the same value here at
    // the same instant; that is the property tests/test_commit_engine.cpp asserts, and it is
    // what "cadence can never promote unstable text" means operationally.
    const size_t matured = s.matured_prefix(s.clock->now_ms());
    // Never report less than what is already in the document: an uncommitted candidate may
    // legitimately retract, but committed text may not.
    return static_cast<uint64_t>(std::max(matured, s.committed.size()));
}

const IcerCounters& CommitEngine::counters() const noexcept { return impl_->counters; }

void CommitEngine::reset_counters() noexcept { impl_->counters = IcerCounters{}; }

const StabilityParams& CommitEngine::params() const noexcept { return impl_->params; }

void CommitEngine::set_params(const StabilityParams& p) {
    // Swept between bench runs. Changing N mid-utterance leaves the window short of the new N
    // for a few hypotheses, which suppresses commits until it refills — conservative, and the
    // harness changes parameters between runs anyway.
    impl_->params = p;
}

}  // namespace kd
