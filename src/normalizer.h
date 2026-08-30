// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// Normalizer — the compose buffer's text, rewritten as written prose by a remote model.
//
// Implements: docs/normalizer.md. Related: docs/editing.md §2 (compose-then-commit, which is the
// only reason this can exist at all) and plan.md §1.4 (the committed-prefix invariant, which is
// the reason it may never run under any other cadence).
//
// ─── What this does, and the one sentence that bounds it ────────────────────────────────────
//
// It POSTs the pill's settled text to an HTTP endpoint that runs a small text model
// (superwhisper/s1-mini, a 0.6B Qwen3-0.6B fine-tune) and gets back the same speech written as
// text: fillers dropped, false starts resolved to whatever the speaker landed on, punctuation
// and capitalisation applied, spoken numbers and dates and emails in written form.
//
// **It rewrites what is in the pill. It never touches what is in the document.** Everything
// below follows from that one line.
//
// ─── Why this is not a backend, and not in the sidecar ──────────────────────────────────────
//
// The obvious home is `server/kvit_asr/`, next to the ASR backends, where the model→interpreter
// map already lives and where nothing in C++ has to learn what a model is. It is the wrong home,
// for a reason that is not a matter of taste:
//
//   **The sidecar does not know what the user has in the pill.** It knows the `final`s it sent.
//   docs/editing.md §3.2 closes the microphone at the first edit key and lets the hotkey reopen
//   it, so by the time a session ends the buffer may contain the user's own corrections,
//   deletions and pastes — none of which crossed the pipe. A normalizer fed the sidecar's own
//   transcript would rewrite text the user had already fixed and silently discard the fix.
//
// So the request is made from the side that owns the buffer, and the protocol is untouched: no
// new message type (docs/protocol.md §4.3 makes an unknown `t` a fatal `unknown_message`, so a
// new one costs a proto version), no new field, no fixtures, no second implementation to keep in
// agreement. This module talks to a different service over a different transport and the sidecar
// never learns it exists.
//
// ─── Why it may only run under Cadence::EndOfSession ────────────────────────────────────────
//
// A rewriter's output is **not prefix-stable**, and the whole program is built on the assumption
// that text only ever grows. Watch a correction cross an utterance boundary:
//
//     raw:  "...send the report by friday"              → "...send the report by Friday."
//     raw:  "...send the report by friday no wait thursday" → "...send the report by Thursday."
//
// `Friday.` was in the first answer and is gone from the second. Under Cadence::Immediate those
// bytes are already in the user's document, and taking them back means sending backspaces into
// an arbitrary application — the single thing plan.md §1.4 exists to refuse. Under
// Cadence::EndOfSession nothing has been committed, the retraction happens in the pill where the
// user can see it, and §1.4 is untouched. `NormalizerConfig::validate()` enforces this and
// Settings::normalise() forces `enabled` off under any other cadence, exactly as it already does
// for `editing.enabled`.
//
// ─── One committed chunk per request ────────────────────────────────────────────────────────
//
// A rewriter is not idempotent: feed it its own output and it keeps editing, and the drift is
// toward shorter and more confident, which is the direction that loses words. So text that has
// been through the model is never sent to it again. The caller (`App::pump_normalize`) sends
// **one committed chunk per request**, in order, one at a time, and splices each answer into the
// pill where its chunk lies.
//
// The first form of this sent the whole raw buffer every time and replaced the pill wholesale,
// keeping a separate raw baseline so the model never saw its own output. It is not what shipped,
// because of what it did to a speaker who does not stop: every answer was superseded by the next
// utterance (`requests_superseded`), so in a continuous dictation the rewrite landed exactly
// once, at the end. What the user watched accumulate for the length of the session was raw text,
// and the pill flipped to prose the instant before they committed it — the one moment they are
// no longer reading it.
//
// A chunk is safe to rewrite alone because it is *closed*: commit_engine.h hands over text that
// is several sentences behind the live tail and can never be revised, so a correction the
// speaker made — "no wait, Thursday" — is already resolved inside the chunk that carries it. The
// whole-buffer form's one real advantage, resolving a correction the VAD split across the
// boundary between two utterances, is the narrow residue of that; it is given up knowingly, and
// what is bought with it is that every chunk arrives rewritten at the moment it arrives at all.
//
// The rest follows: the pill only ever grows, earlier sentences stop churning under the reader,
// a guard rejection costs one utterance rather than the session's whole rewrite, and each
// request is one utterance long instead of growing without bound toward `max_bytes`.
//
// ─── The term guard, which is the reason this is safe to enable ─────────────────────────────
//
// plan.md §4 is a list of the strings Win+H gets wrong and this program exists to get right:
// `llama.cpp`, `sm_120`, `--n-gpu-layers`, `C:\Users\sk\AppData`. A 0.6B model whose own card
// claims 94.8% *token* accuracy is precisely the kind of thing that renders `sm_120` as `sm 120`
// — that exact string is already in bench/metrics.py's `wrong_forms` table as a known-bad
// rendering, written down before this model existed.
//
// So a rewrite is not accepted on trust. `guard_verdict()` compares raw against normalized and
// **rejects the whole rewrite** if any protected term present in the raw text is absent from the
// normalized one. Whole, not partial: a rewrite is one act of judgement by the model and
// splicing half of it back would produce text that neither the model nor the user composed. On
// rejection the raw text stands, the pill does not change, and the log names the term.
//
// The guard is a pure function over two strings and a term list. It has no Windows headers, no
// clock and no I/O, and it is where the tests live — everything interesting about this feature
// that can be decided without a network is decided there.
//
// ─── Failure is never fatal ─────────────────────────────────────────────────────────────────
//
// A missing, slow, overloaded or broken normalizer must never cost the user a word. Every
// failure path — DNS, connect, timeout, non-200, malformed JSON, a guard rejection — leaves the
// raw text exactly as it was and logs why. The user's own machine hears them regardless of
// whether some other machine is up.
//
// ─── Threading ──────────────────────────────────────────────────────────────────────────────
//
// One worker thread per Normalizer, created at `start()` and joined at `stop()`. It owns the
// WinHTTP handles and is the only thread that blocks on the network.
//
//   * `request()` is called on the **UI thread**. It copies the text, stamps it with a
//     monotonically increasing `id`, hands it to the worker and returns immediately. It never
//     blocks and never touches a handle.
//   * `Callbacks::on_result` is invoked on the **worker thread**. Like IpcClient's callbacks it
//     must not block and must not call back into Normalizer; the intended implementation posts
//     to the UI thread, because EditBuffer is single-threaded by design (edit_buffer.h) and must
//     not be touched from here.
//   * At most one request is in flight. A `request()` made while one is outstanding **replaces**
//     the pending one rather than queueing it: the newer buffer is a superset of the older, so
//     the older answer is already stale and computing it would waste the model's turn and the
//     lock it shares with the recogniser. The superseded id is reported so the caller can tell a
//     dropped answer from a lost one.
//   * `cancel_all()` invalidates every outstanding and pending request. The worker may still be
//     inside a blocking WinHTTP call when it is invoked, so the result is discarded on arrival
//     rather than the call being aborted; correctness comes from the id check, not from timing.
//
// ─── What this deliberately does not do ─────────────────────────────────────────────────────
//
//   * No retry. A normalizer that answers slowly twice is worse than one that does not answer:
//     the user is holding a pill open waiting for it.
//   * No streaming. The endpoint returns one rewrite of one buffer.
//   * No HTTPS-specific handling beyond what WinHTTP does by default. The endpoint is a machine
//     on the user's own LAN named in settings; if that name is ever pointed at the internet, the
//     text of everything dictated goes with it, and that is a settings decision, made once,
//     rather than something this module can second-guess per request. plan.md §1.9's "nothing is
//     sent" is *why this is off by default* and why enabling it is an explicit act.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "util/result.h"

namespace kd {

// ─── Configuration ──────────────────────────────────────────────────────────────────────────

// The endpoint's control values. The server refuses an untrained value with 400 rather than
// sending it to the model (which answers an unknown control with invented text), so these are
// validated here too — a settings file should be rejected at load with the field named, not at
// the end of the first session the user dictates.
inline constexpr const char* kNormalizerStylings[] = {"casual", "semi-formal", "formal"};
inline constexpr const char* kNormalizerStructures[] = {"prose", "lists"};
inline constexpr const char* kNormalizerContexts[] = {"general", "email"};

struct NormalizerConfig {
    // Off by default, and that is a plan.md §1.9 decision rather than a cautious one: enabling
    // this sends every dictated word to another machine. Nothing turns it on but the user.
    bool enabled = false;

    // Full URL of the normalize route, e.g. "http://strixhalo.lan:10301/v1/text/normalize".
    std::string url;

    // The whole request, including connect and read. The model's own numbers are ~0.2 s for a
    // command and ~1.4 s for a paragraph; this is the point at which waiting has itself become
    // the problem. It is bounded well under main.cpp's kFinalizeGraceMs so that a normalizer
    // that has stopped answering can never be the reason a session fails to end.
    uint32_t timeout_ms = 3000;

    // Endpoint control values. Passed through verbatim; validated against the tables above.
    std::string styling = "semi-formal";
    std::string structure = "prose";
    std::string context = "general";

    // Longest buffer worth sending. The model's card says "up to ~1,000 tokens; chunk longer
    // transcripts", and this module does not chunk — a buffer past this is left raw and the log
    // says so, which is honest, where sending it and getting back a confidently truncated
    // rewrite would not be. Bytes, not tokens, deliberately: a byte count is something this side
    // can compute exactly and a token count is not.
    size_t max_bytes = 4000;

    // Terms the rewrite may not lose, beyond the built-in table. Seeded from Settings::
    // context_terms so that the vocabulary the user already maintains for the recogniser also
    // protects them from the rewriter, with one list to edit instead of two.
    std::vector<std::string> protect_terms;

    // Reject a configuration that would violate the above rather than silently correcting it.
    // `cadence_is_end_of_session` is passed in rather than read, so this header stays free of
    // commit_engine.h and the rule is stated where it is enforced.
    [[nodiscard]] Status validate(bool cadence_is_end_of_session) const;
};

// ─── The guard (pure) ───────────────────────────────────────────────────────────────────────

// The built-in protected terms: plan.md §4's list, kept deliberately in step with
// bench/metrics.py's TERMS table so that what the bench measures and what the app enforces
// cannot drift. Canonical spellings only — matching is case-insensitive and the bench's
// `wrong_forms` are not needed here, because the guard asks "did this term survive", not "what
// did it turn into".
[[nodiscard]] const std::vector<std::string>& builtin_protected_terms();

enum class GuardVerdict : uint8_t {
    Accept,          // the rewrite may replace the raw text
    RejectEmpty,     // the model returned nothing, or nothing but whitespace
    RejectTermLost,  // a protected term was in the raw text and is not in the rewrite
    RejectTooShort,  // the rewrite lost more of the text than any normalisation can explain
};

const char* to_string(GuardVerdict v) noexcept;

struct GuardResult {
    GuardVerdict verdict = GuardVerdict::Accept;
    // On RejectTermLost, the term that vanished — so the log names it and a bench run can count
    // which terms this model actually damages rather than reporting a bare rejection rate.
    std::string lost_term;

    [[nodiscard]] bool accepted() const noexcept { return verdict == GuardVerdict::Accept; }
};

// Decide whether `normalized` may replace `raw`.
//
// `extra_terms` is appended to builtin_protected_terms(). Matching is case-insensitive and
// substring-based on purpose: `sm_120` inside `--arch=sm_120` counts as surviving, and asking
// for a token match would require this module to have an opinion about tokenisation that the
// model does not share.
//
// The length floor exists because a term list can only protect the terms on it. A model that
// answers a paragraph with one sentence has failed in a way no vocabulary check would catch, and
// dropping the user's words is the failure that costs them most. `min_ratio` is the fraction of
// the raw text's length the rewrite must retain; normalisation legitimately shortens text (every
// "um" removed is three bytes) so it is generous, and it is a floor rather than a range because
// there is no upper bound on legitimate expansion — "twenty five dollars" → "$25" shortens,
// "at" → "@" shortens, and an ITN expansion of "twelve gigabytes" lengthens.
[[nodiscard]] GuardResult guard_verdict(std::string_view raw, std::string_view normalized,
                                        const std::vector<std::string>& extra_terms,
                                        double min_ratio = 0.5);

// ─── Request/response plumbing (pure, so the codec is testable without a network) ───────────

// Build the POST body for `text` under `cfg`. JSON, because the endpoint accepts it and because
// util/json.h is already here and already escapes correctly — the multipart and urlencoded forms
// the endpoint also accepts would each need an escaper this project does not have.
[[nodiscard]] std::string build_request_body(const NormalizerConfig& cfg, std::string_view text);

// Pull `text` out of a `{"text": "...", ...}` response. Returns a Malformed status naming what
// was wrong, so a normalizer that starts answering with an error page produces a log line that
// says so rather than a silent no-op.
[[nodiscard]] Result<std::string> parse_response_body(std::string_view body);

// ─── The client ─────────────────────────────────────────────────────────────────────────────

class Normalizer {
public:
    struct Result {
        // The id `request()` returned. The caller compares it against what it is still waiting
        // for; anything else is stale and must be dropped (see the threading note above).
        uint64_t id = 0;
        // The text as sent, so the caller can apply the guard against exactly what the model saw
        // rather than against whatever the buffer has become since.
        std::string raw;
        // Empty unless `status` is ok.
        std::string normalized;
        // Ok, or why not. Never fatal to the session.
        Status status;
        // Wall time from request() to the answer, for the latency the pill's pause is made of.
        uint64_t elapsed_ms = 0;
    };

    struct Callbacks {
        // Invoked on the worker thread. Must not block; must not call back into Normalizer.
        std::function<void(Result)> on_result;
    };

    Normalizer();
    ~Normalizer();

    Normalizer(const Normalizer&) = delete;
    Normalizer& operator=(const Normalizer&) = delete;

    // Opens the WinHTTP session and starts the worker. Fails only for reasons that are permanent
    // (a URL that will not parse, WinHTTP unavailable); a server that is merely down is not an
    // error here, because it may be up by the time the user dictates.
    [[nodiscard]] Status start(const NormalizerConfig& cfg, Callbacks cb);
    void stop();

    // Replaces the config for subsequent requests. Called when the settings dialog closes.
    void set_config(const NormalizerConfig& cfg);

    // Queue `text`. Returns the id to expect back, or 0 if the request was not made at all
    // (disabled, empty text, or past `max_bytes` — each logged with the reason).
    uint64_t request(std::string_view text);

    // Abandon everything outstanding. Called when the session ends and when the user starts
    // editing, so a late answer cannot overwrite text the user has since touched.
    void cancel_all();

    // Diagnostics, for the settings dialog's "test" button and for progress.md.
    [[nodiscard]] uint64_t requests_sent() const noexcept;
    [[nodiscard]] uint64_t requests_superseded() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace kd
