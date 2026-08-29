// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// tests/test_asrd_backend.cpp — the window driver, the wire encodings, and the URL guard.
//
// The network half of `backend_openai.cpp` is exercised in `test_asrd_session.cpp`, against a
// stub HTTP service on loopback. What is here is everything that has no socket in it, which is
// most of where the bugs live:
//
//   * the overlapping-window driver, whose freeze-on-slide stitching openai-translator.md §8
//     names as a standing risk;
//   * the WAV and multipart encoders, which are hand-rolled on both sides of the wire;
//   * `validate_service_url`, which is the line between "user-supplied content on an argv this
//     program builds" and "a user-supplied argv", and is what makes the spawn in §4.6 a
//     different thing from the one release-plan.md §1.2 deleted.

#include "kd_test.h"

#include "asrd/backend_openai.h"
#include "asrd/vad.h"
#include "asrd/http.h"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace kd::asrd;

std::vector<int16_t> pcm_ms(uint32_t ms, int16_t value = 1000) {
    return std::vector<int16_t>(static_cast<size_t>(ms) * 16, value);
}

uint32_t le32_at(const std::string& s, size_t off) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[off])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[off + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[off + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[off + 3])) << 24);
}

uint16_t le16_at(const std::string& s, size_t off) {
    return static_cast<uint16_t>(static_cast<uint8_t>(s[off]) |
                                 (static_cast<uint8_t>(s[off + 1]) << 8));
}

}  // namespace

// ─── The service URL guard ──────────────────────────────────────────────────────────────────

KD_TEST(asrd_backend, accepts_the_urls_a_user_would_actually_type) {
    KD_CHECK(validate_service_url("http://strixhalo.lan:10301").empty());
    KD_CHECK(validate_service_url("http://192.168.1.5:8000").empty());
    KD_CHECK(validate_service_url("https://asr.example.com").empty());
    KD_CHECK(validate_service_url("http://localhost:10301/").empty());
}

KD_TEST(asrd_backend, refuses_a_websocket_url_by_name) {
    // The mistake this whole document exists to end: `ServerSettings.url` is a protocol
    // WebSocket and `parse_server_url` treats http:// and https:// as WebSocket *synonyms*, so
    // an address put in the wrong field produces a WinHTTP upgrade against an HTTP API. The
    // message says which of the two things this field is.
    const std::string why = validate_service_url("ws://127.0.0.1:10402/");
    KD_CHECK(!why.empty());
    KD_CHECK(why.find("docs/protocol.md server is a different thing") != std::string::npos);
}

KD_TEST(asrd_backend, refuses_anything_that_would_need_quoting) {
    // The client builds asrd's command line, so `--service` carries user-supplied *content*
    // rather than a user-supplied argv vector. Validating before quoting is what keeps that
    // distinction real rather than rhetorical.
    KD_CHECK(!validate_service_url("http://host /x").empty());
    KD_CHECK(!validate_service_url("http://host\tx").empty());
    KD_CHECK(!validate_service_url("http://host\"x").empty());
    KD_CHECK(!validate_service_url("http://host'x").empty());
    KD_CHECK(!validate_service_url("http://host\nx").empty());
    KD_CHECK(!validate_service_url("http://host\x01x").empty());
    KD_CHECK(!validate_service_url("").empty());
    KD_CHECK(!validate_service_url("strixhalo.lan:10301").empty());  // no scheme
    KD_CHECK(!validate_service_url("file:///etc/passwd").empty());
}

KD_TEST(asrd_backend, parses_a_url_into_the_parts_winhttp_wants) {
    HttpUrl url;
    KD_REQUIRE(parse_http_url("http://strixhalo.lan:10301/v1/audio/transcriptions", url));
    KD_CHECK_EQ(url.host, std::string("strixhalo.lan"));
    KD_CHECK_EQ(url.port, uint16_t{10301});
    KD_CHECK_EQ(url.path, std::string("/v1/audio/transcriptions"));
    KD_CHECK(!url.secure);

    // The default ports, which WinHttpCrackUrl supplies and a hand-rolled parser forgets.
    HttpUrl plain;
    KD_REQUIRE(parse_http_url("http://example.com", plain));
    KD_CHECK_EQ(plain.port, uint16_t{80});
    KD_CHECK_EQ(plain.path, std::string("/"));

    HttpUrl secure;
    KD_REQUIRE(parse_http_url("https://example.com/v1", secure));
    KD_CHECK_EQ(secure.port, uint16_t{443});
    KD_CHECK(secure.secure);

    // Query strings belong on the request path, not thrown away.
    HttpUrl query;
    KD_REQUIRE(parse_http_url("http://h:1/p?a=b&c=d", query));
    KD_CHECK_EQ(query.path, std::string("/p?a=b&c=d"));
}

// ─── WAV ────────────────────────────────────────────────────────────────────────────────────

KD_TEST(asrd_backend, writes_a_wav_header_the_far_end_can_read) {
    const std::vector<int16_t> pcm = {0, 1, -1, 32767, -32768};
    const std::string wav = wav_bytes(pcm.data(), pcm.size());

    KD_REQUIRE_EQ(wav.size(), size_t{44 + 10});
    KD_CHECK_EQ(wav.substr(0, 4), std::string("RIFF"));
    KD_CHECK_EQ(le32_at(wav, 4), uint32_t{36 + 10});
    KD_CHECK_EQ(wav.substr(8, 8), std::string("WAVEfmt "));
    KD_CHECK_EQ(le32_at(wav, 16), uint32_t{16});   // PCM fmt chunk
    KD_CHECK_EQ(le16_at(wav, 20), uint16_t{1});    // WAVE_FORMAT_PCM
    // docs/protocol.md §6.3 fixes the format, so none of these is negotiable or parameterised.
    KD_CHECK_EQ(le16_at(wav, 22), uint16_t{1});        // mono
    KD_CHECK_EQ(le32_at(wav, 24), uint32_t{16000});    // 16 kHz
    KD_CHECK_EQ(le32_at(wav, 28), uint32_t{32000});    // byte rate
    KD_CHECK_EQ(le16_at(wav, 32), uint16_t{2});        // block align
    KD_CHECK_EQ(le16_at(wav, 34), uint16_t{16});       // bits per sample
    KD_CHECK_EQ(wav.substr(36, 4), std::string("data"));
    KD_CHECK_EQ(le32_at(wav, 40), uint32_t{10});

    // Little-endian samples, written explicitly rather than memcpy'd.
    KD_CHECK_EQ(static_cast<uint8_t>(wav[44 + 6]), uint8_t{0xFF});  // 32767 low byte
    KD_CHECK_EQ(static_cast<uint8_t>(wav[44 + 7]), uint8_t{0x7F});
    KD_CHECK_EQ(static_cast<uint8_t>(wav[44 + 8]), uint8_t{0x00});  // -32768
    KD_CHECK_EQ(static_cast<uint8_t>(wav[44 + 9]), uint8_t{0x80});
}

KD_TEST(asrd_backend, an_empty_wav_is_still_a_valid_wav) {
    const std::string wav = wav_bytes(nullptr, 0);
    KD_CHECK_EQ(wav.size(), size_t{44});
    KD_CHECK_EQ(le32_at(wav, 40), uint32_t{0});
}

// ─── multipart ──────────────────────────────────────────────────────────────────────────────

KD_TEST(asrd_backend, encodes_multipart_the_way_the_python_backend_does) {
    std::string content_type;
    const std::string body = encode_multipart(
        {{"model", "Qwen/Qwen3-ASR-1.7B"}, {"language", "en"}, {"response_format", "json"}},
        "file", "utterance.wav", "audio/wav", std::string("RIFF...."), content_type);

    KD_CHECK(content_type.rfind("multipart/form-data; boundary=", 0) == 0);
    const std::string boundary = content_type.substr(content_type.find('=') + 1);
    // The same fixed token openai_http.py uses, so a capture taken against either backend reads
    // the same. Randomness would need a source and buys nothing: a WAV cannot contain the token,
    // because its body is length-prefixed rather than delimited.
    KD_CHECK_EQ(boundary, std::string("----kvit-dictate-boundary-7Nq2Xr"));

    KD_CHECK(body.rfind("--" + boundary + "\r\n", 0) == 0);
    KD_CHECK(body.find("Content-Disposition: form-data; name=\"model\"\r\n\r\n"
                       "Qwen/Qwen3-ASR-1.7B\r\n") != std::string::npos);
    KD_CHECK(body.find("name=\"file\"; filename=\"utterance.wav\"\r\n"
                       "Content-Type: audio/wav\r\n\r\nRIFF....\r\n") != std::string::npos);
    // The terminator is `--boundary--`, and a missing one makes the far end wait for more body.
    const std::string tail = "\r\n--" + boundary + "--\r\n";
    KD_CHECK_EQ(body.substr(body.size() - tail.size()), tail);
}

KD_TEST(asrd_backend, a_file_only_multipart_has_no_stray_field_parts) {
    // The VAD route takes no fields unless a threshold is configured, and an empty field list
    // must not produce an empty part — which a naive loop over an empty map does not, but a
    // loop that always writes a leading boundary does.
    std::string content_type;
    const std::string body =
        encode_multipart({}, "file", "clip.wav", "audio/wav", "xx", content_type);
    const std::string boundary = content_type.substr(content_type.find('=') + 1);
    size_t parts = 0;
    for (size_t at = body.find("--" + boundary); at != std::string::npos;
         at = body.find("--" + boundary, at + 1)) {
        ++parts;
    }
    KD_CHECK_EQ(parts, size_t{2});  // the file part and the terminator
}

// ─── The window driver ──────────────────────────────────────────────────────────────────────

KD_TEST(asrd_backend, a_hop_is_due_once_hop_ms_of_audio_has_arrived) {
    PseudoStreamingDriver driver(8000, 1000);
    KD_CHECK(!driver.feed(pcm_ms(400).data(), pcm_ms(400).size()));
    const std::vector<int16_t> more = pcm_ms(500);
    KD_CHECK(!driver.feed(more.data(), more.size()));
    const std::vector<int16_t> last = pcm_ms(200);
    KD_CHECK(driver.feed(last.data(), last.size()));
    KD_CHECK_EQ(driver.audio_ms(), uint64_t{1100});
}

KD_TEST(asrd_backend, finalize_only_never_slides_the_window) {
    // The v1 default, and the reason the stitching risk below is not a shipping risk: with
    // hop >= window the window never slides, because the utterance ends first.
    PseudoStreamingDriver driver(8000, 8000);
    for (int i = 0; i < 5; ++i) {
        const std::vector<int16_t> chunk = pcm_ms(1000);
        KD_CHECK(!driver.feed(chunk.data(), chunk.size()));
    }
    std::vector<int16_t> window;
    bool slid = true;
    KD_REQUIRE(driver.take_window(/*force=*/true, window, slid));
    KD_CHECK(!slid);
    KD_CHECK_EQ(window.size(), size_t{5 * 16000});
    KD_CHECK(driver.frozen_prefix().empty());
}

KD_TEST(asrd_backend, take_window_refuses_when_nothing_has_arrived) {
    PseudoStreamingDriver driver(8000, 1000);
    std::vector<int16_t> window;
    bool slid = false;
    KD_CHECK(!driver.take_window(/*force=*/true, window, slid));
    KD_CHECK(!driver.any_audio());
}

KD_TEST(asrd_backend, a_growing_window_is_trivially_cumulative) {
    // While the utterance is shorter than window_ms the window starts at the utterance start,
    // so the hypothesis is simply "the transcript of everything said so far" and there is no
    // stitching to get wrong. Most real utterances never leave this regime.
    PseudoStreamingDriver driver(8000, 1000);
    const std::vector<int16_t> chunk = pcm_ms(1000);

    driver.feed(chunk.data(), chunk.size());
    std::vector<int16_t> window;
    bool slid = false;
    KD_REQUIRE(driver.take_window(false, window, slid));
    KD_CHECK(!slid);
    KD_CHECK_EQ(driver.commit("hello", slid), std::string("hello"));

    driver.feed(chunk.data(), chunk.size());
    KD_REQUIRE(driver.take_window(false, window, slid));
    KD_CHECK(!slid);
    KD_CHECK_EQ(driver.commit("hello there", slid), std::string("hello there"));
    KD_CHECK(driver.frozen_prefix().empty());
}

KD_TEST(asrd_backend, past_the_window_the_scrolled_off_transcript_is_frozen) {
    PseudoStreamingDriver driver(2000, 1000);
    const std::vector<int16_t> chunk = pcm_ms(1000);
    std::vector<int16_t> window;
    bool slid = false;

    driver.feed(chunk.data(), chunk.size());
    KD_REQUIRE(driver.take_window(false, window, slid));
    KD_CHECK_EQ(driver.commit("one two three", slid), std::string("one two three"));

    driver.feed(chunk.data(), chunk.size());
    KD_REQUIRE(driver.take_window(false, window, slid));
    KD_CHECK(!slid);  // exactly window_ms is not past it

    driver.feed(chunk.data(), chunk.size());
    KD_REQUIRE(driver.take_window(false, window, slid));
    KD_CHECK(slid);
    KD_CHECK_EQ(window.size(), size_t{2 * 16000});
    // "one" scrolled off; the overlap "two three" identifies where.
    KD_CHECK_EQ(driver.commit("two three four", slid), std::string("one two three four"));
    KD_CHECK_EQ(driver.frozen_prefix(), std::string("one"));
}

KD_TEST(asrd_backend, stitching_finds_the_largest_overlap) {
    KD_CHECK_EQ(scrolled_off_words("one two three", "two three four"), std::string("one"));
    KD_CHECK_EQ(scrolled_off_words("a b c d e", "d e f"), std::string("a b c"));
    KD_CHECK_EQ(scrolled_off_words("", "anything"), std::string(""));
}

KD_TEST(asrd_backend, stitching_tolerates_recasing_and_repunctuation) {
    // A model that repunctuates across the boundary would otherwise find no overlap at all and
    // fall to the conservative branch. The comparison key is deliberately crude — an
    // over-clever matcher would silently freeze the wrong words.
    KD_CHECK_EQ(scrolled_off_words("so i was thinking we", "Thinking, we should"),
                std::string("so i was"));
}

KD_TEST(asrd_backend, no_overlap_freezes_everything_rather_than_dropping_it) {
    // The conservative branch, copied across with its own justification: this can commit text
    // the next window would have revised, but the alternative deletes words the user actually
    // said, and losing text is worse than freezing it early.
    KD_CHECK_EQ(scrolled_off_words("alpha beta", "totally different words"),
                std::string("alpha beta"));
}

KD_TEST(asrd_backend, repeated_words_are_where_stitching_can_be_wrong) {
    // Recorded rather than fixed. base.py documents this failure and the fix is not a cleverer
    // matcher — it is finalize-only, which is why that is the v1 default.
    //
    // "the the cat" -> "the cat sat": the largest k with a matching overlap is 2 ("the cat"),
    // so one "the" is frozen. Had the window really dropped both, a word is duplicated. The
    // assertion pins the behaviour so a future change to the heuristic is a visible decision.
    KD_CHECK_EQ(scrolled_off_words("the the cat", "the cat sat"), std::string("the"));
}

KD_TEST(asrd_backend, joining_never_doubles_or_swallows_a_space) {
    KD_CHECK_EQ(join_words("one", "two"), std::string("one two"));
    KD_CHECK_EQ(join_words("one ", " two"), std::string("one two"));
    KD_CHECK_EQ(join_words("", "two"), std::string("two"));
    KD_CHECK_EQ(join_words("one", ""), std::string("one"));
    KD_CHECK_EQ(join_words("", ""), std::string(""));
}

KD_TEST(asrd_backend, reset_forgets_the_frozen_prefix_and_the_clock) {
    // Per utterance, not per session: a frozen prefix carried across an utterance boundary would
    // prepend the previous utterance's tail to the next one's first partial.
    PseudoStreamingDriver driver(2000, 1000);
    const std::vector<int16_t> chunk = pcm_ms(3000);
    driver.feed(chunk.data(), chunk.size());
    std::vector<int16_t> window;
    bool slid = false;
    KD_REQUIRE(driver.take_window(true, window, slid));
    driver.commit("one two three", slid);

    driver.reset();
    KD_CHECK(driver.frozen_prefix().empty());
    KD_CHECK_EQ(driver.audio_ms(), uint64_t{0});
    KD_CHECK(!driver.any_audio());
    KD_CHECK(!driver.take_window(true, window, slid));
}

// ─── The slide must never happen ─────────────────────────────────────────────────────────────

// How `connection.cpp` sizes the window. Kept here rather than repeated as a literal, so a
// change to either side of that sum is caught by the tests below.
static uint32_t shipped_window_ms() {
    return static_cast<uint32_t>(VadConfig{}.max_utterance_audio_ms() + 0.5);
}

KD_TEST(asrd_backend, the_window_is_sized_in_audio_not_in_the_timer_that_ends_the_utterance) {
    // The 2026-08-28 fix asserted `window_ms >= max_utterance_ms`, and the test below walked
    // exactly 30 000 ms through it. Both passed, and the duplication was reported again the
    // next day — because the endpointer does not hand over `max_utterance_ms` of audio. It
    // hands over the pre-roll as well.
    //
    // The onset frame replays `preroll_ms` from before the detected onset so the decoder hears
    // the beginning of the first word, and the safety valve's clock starts at the onset. So a
    // cut utterance arrives as 30 000 ms + 300 ms + a frame of overshoot, the buffer crosses a
    // 30 000 ms window during that last 300 ms, and the *finalize* — which takes the window
    // whether or not a hop is due — slides every single time. One guaranteed stitch per
    // over-long dictation.
    const VadConfig vad;
    KD_CHECK(vad.max_utterance_audio_ms() > vad.max_utterance_ms);
    KD_CHECK(shipped_window_ms() >= static_cast<uint32_t>(vad.max_utterance_audio_ms()));

    // 0 means "unbounded", and no window can hold that. It has to come back as 0 rather than as
    // a plausible number, so `connection.cpp` notices instead of building a zero-length window
    // that slides on its first hop.
    VadConfig unbounded;
    unbounded.max_utterance_ms = 0.0;
    KD_CHECK_EQ(unbounded.max_utterance_audio_ms(), 0.0);
}

KD_TEST(asrd_backend, the_shipped_window_holds_a_whole_utterance_so_nothing_is_ever_stitched) {
    // Reported 2026-08-28 as "a dictation that somehow duplicated": the same sentence two and
    // three times in the document, each copy slightly reworded, joined at a visible seam
    // ("and then give gives"). The second half of this test reproduces exactly that.
    //
    // The shipped window must be at least as long as the longest utterance the endpointer will
    // ever hand over, because a window shorter than the utterance slides, and every slide is
    // stitched by a heuristic that a revising model defeats. This is the assertion that keeps
    // the two numbers together; `connection.cpp` reads one from the other for the same reason.
    const VadConfig vad;
    PseudoStreamingDriver driver(shipped_window_ms(), OpenAiBackendConfig{}.hop_ms);

    // Every millisecond the endpointer can deliver — to the millisecond, since the whole defect
    // was a 300 ms shortfall — hopping the whole way, and then the finalize on top. The
    // finalize is the take that mattered: it forces a window whether or not a hop is due, so it
    // is the one that slid on every over-long utterance. Nothing slides, nothing is frozen, and
    // every hypothesis reaching the client is one decode of one recording.
    const double deliverable_ms = vad.max_utterance_audio_ms();
    const int whole_seconds = static_cast<int>(deliverable_ms / 1000.0);
    const std::vector<int16_t> second = pcm_ms(1000);
    std::vector<int16_t> window;
    bool slid = true;
    for (int i = 0; i < whole_seconds; ++i) {
        driver.feed(second.data(), second.size());
        KD_REQUIRE(driver.take_window(false, window, slid));
        KD_CHECK(!slid);
        // Whatever the model says is what the client is told. No prefix, no seam.
        KD_CHECK_EQ(driver.commit("hypothesis " + std::to_string(i), slid),
                    std::string("hypothesis " + std::to_string(i)));
        KD_CHECK(driver.frozen_prefix().empty());
    }
    const auto remainder_ms =
        static_cast<uint32_t>(deliverable_ms - 1000.0 * static_cast<double>(whole_seconds));
    const std::vector<int16_t> remainder = pcm_ms(remainder_ms);
    driver.feed(remainder.data(), remainder.size());
    KD_REQUIRE(driver.take_window(true, window, slid));
    KD_CHECK(!slid);
    KD_CHECK_EQ(driver.commit("the final", slid), std::string("the final"));
    KD_CHECK(driver.frozen_prefix().empty());
}

KD_TEST(asrd_backend, a_window_sized_at_the_valve_stitches_the_finalize_of_a_cut_utterance) {
    // The 2026-08-29 report, reduced to arithmetic: the window sized at `max_utterance_ms`, and
    // the audio carrying `preroll_ms` on top of it. Every hop before the cut is clean — which
    // is what made this hard to see — and then the finalize slides, freezes the whole previous
    // window, and appends the new one. The user gets the tail of their dictation twice.
    const VadConfig vad;
    PseudoStreamingDriver driver(static_cast<uint32_t>(vad.max_utterance_ms),
                                 OpenAiBackendConfig{}.hop_ms);

    // The two hypotheses are the ones from the 2026-08-29 report, trimmed to one sentence. The
    // model dropped a word on the re-decode — "so I need" became "so need" — which is all it
    // takes: `scrolled_off_words` needs an exact run of words, and one substitution anywhere in
    // the overlap makes every candidate k fail.
    const std::string before =
        "there are lots of scripts and stuff picking up for those serpent names, "
        "so I need to review all of that";
    const std::string after =
        "there are lots of scripts and stuff picking up for those serpent names, "
        "so need to review all of that";

    // The pre-roll first, exactly as the onset frame delivers it, then the utterance.
    const std::vector<int16_t> preroll = pcm_ms(static_cast<uint32_t>(vad.preroll_ms));
    const std::vector<int16_t> second = pcm_ms(1000);
    const int seconds = static_cast<int>(vad.max_utterance_ms / 1000.0);
    std::vector<int16_t> window;
    bool slid = true;

    driver.feed(preroll.data(), preroll.size());
    for (int i = 0; i + 1 < seconds; ++i) {
        driver.feed(second.data(), second.size());
        KD_REQUIRE(driver.take_window(false, window, slid));
        KD_CHECK(!slid);
        driver.commit(before, slid);
    }
    KD_CHECK(driver.frozen_prefix().empty());  // 29 clean seconds; it looks fixed all the way

    // The thirtieth second. The buffer is now `preroll_ms` past a window sized at the valve, so
    // the very next take — this hop, or the finalize the valve is about to force — slides, and
    // the fallback freezes the entire previous window in front of a new one that still contains
    // every second of it. This is what the user read back: the sentence twice, the second copy
    // slightly reworded.
    driver.feed(second.data(), second.size());
    KD_REQUIRE(driver.take_window(false, window, slid));
    KD_CHECK(slid);
    KD_CHECK_EQ(driver.commit(after, slid), before + " " + after);
    KD_CHECK_EQ(driver.frozen_prefix(), before);

    // The same walk against a window sized in audio: no slide, nothing frozen, and the client is
    // told exactly what the model said.
    PseudoStreamingDriver fixed(shipped_window_ms(), OpenAiBackendConfig{}.hop_ms);
    fixed.feed(preroll.data(), preroll.size());
    for (int i = 0; i < seconds; ++i) {
        fixed.feed(second.data(), second.size());
        KD_REQUIRE(fixed.take_window(false, window, slid));
        KD_CHECK(!slid);
        fixed.commit(before, slid);
    }
    KD_REQUIRE(fixed.take_window(true, window, slid));
    KD_CHECK(!slid);
    KD_CHECK_EQ(fixed.commit(after, slid), after);
    KD_CHECK(fixed.frozen_prefix().empty());
}

KD_TEST(asrd_backend, a_window_shorter_than_the_utterance_duplicates_what_the_model_revises) {
    // Why the test above is worth its assertions, demonstrated rather than asserted about.
    //
    // Eight seconds of window, one second of hop, and a model that re-punctuates what it has
    // already said — which is what every batch model does as it hears the end of a sentence.
    // The stitch matches the tail of the previous transcript against the head of the new one;
    // the revision breaks that match, and the fallback freezes the *entire* previous window
    // while the new window still holds seven of those eight seconds.
    PseudoStreamingDriver driver(8000, 1000);
    const std::vector<int16_t> second = pcm_ms(1000);
    for (int i = 0; i < 9; ++i) driver.feed(second.data(), second.size());

    std::vector<int16_t> window;
    bool slid = true;
    KD_REQUIRE(driver.take_window(false, window, slid));
    KD_CHECK(!slid);  // nothing transcribed yet, so nothing may be dropped
    driver.commit("confused application does something weird and then give", slid);

    driver.feed(second.data(), second.size());
    KD_REQUIRE(driver.take_window(false, window, slid));
    KD_CHECK(slid);
    // One second of audio left the window. The model, hearing more, has repunctuated the middle
    // and revised one word: "give" became "gives". That single substitution is enough — the
    // matcher needs an exact run, so no k matches at all and the fallback freezes all of it.
    const std::string stitched =
        driver.commit("Application does something weird, and then gives inconsistent instruction",
                      slid);

    // Every word is present twice: once frozen from the old window, once from the new one. This
    // is the reported defect, and it is the arithmetic of the fallback rather than a bug in it —
    // it preserves the audio it drops, and it preserves seven seconds it did not drop with it.
    KD_CHECK_EQ(driver.frozen_prefix(),
                std::string("confused application does something weird and then give"));
    KD_CHECK_EQ(stitched,
                std::string("confused application does something weird and then give "
                            "Application does something weird, and then gives inconsistent "
                            "instruction"));
}

// ─── The slide must never lose audio ────────────────────────────────────────────────────────

KD_TEST(asrd_backend, finalize_only_never_drops_the_start_of_a_long_utterance) {
    // The regression this test exists for, reported from real use as "a chunk of dictation was
    // never captured".
    //
    // In finalize-only no intermediate hop ever runs, so `prev_window_text_` is empty when the
    // finalize forces a take. The window used to slide anyway, `scrolled_off_words("", text)`
    // froze nothing, and every second of audio before the last `window_ms` vanished with no
    // error, no log line and no gap in the transcript to notice.
    //
    // openai-translator.md §4.5 says "finalize-only makes hop_ms >= window_ms, so the window
    // never slides" — that claim is about *hops*, and the finalize is not a hop.
    PseudoStreamingDriver driver(8000, 8000);  // the shipped finalize-only shape

    // Twenty seconds of speech in one utterance: a long sentence, or a paragraph read aloud.
    const std::vector<int16_t> chunk = pcm_ms(1000);
    for (int i = 0; i < 20; ++i) driver.feed(chunk.data(), chunk.size());

    std::vector<int16_t> window;
    bool slid = true;
    KD_REQUIRE(driver.take_window(/*force=*/true, window, slid));

    KD_CHECK(!slid);
    // The whole utterance reaches the model, not the last eight seconds of it.
    KD_CHECK_EQ(window.size(), size_t{20 * 16000});
    KD_CHECK_EQ(driver.commit("the whole thing", slid), std::string("the whole thing"));
    KD_CHECK(driver.frozen_prefix().empty());
}

KD_TEST(asrd_backend, a_slide_with_nothing_to_stitch_against_is_refused) {
    // The general rule, stated without reference to finalize-only: dropping audio is survivable
    // only because commit() freezes its transcript first, and it can only do that by stitching
    // the pre-slide transcript against the post-slide one. With no pre-slide transcript there is
    // nothing to freeze, so the slide must not happen.
    PseudoStreamingDriver driver(2000, 1000);
    const std::vector<int16_t> chunk = pcm_ms(5000);
    driver.feed(chunk.data(), chunk.size());

    std::vector<int16_t> window;
    bool slid = true;
    KD_REQUIRE(driver.take_window(false, window, slid));
    KD_CHECK(!slid);                            // first take: nothing has ever been transcribed
    KD_CHECK_EQ(window.size(), size_t{5 * 16000});

    // Once a transcript exists, the window is allowed to slide and the words are frozen.
    driver.commit("one two three", slid);
    driver.feed(chunk.data(), chunk.size());
    KD_REQUIRE(driver.take_window(false, window, slid));
    KD_CHECK(slid);
    KD_CHECK_EQ(window.size(), size_t{2 * 16000});
    KD_CHECK_EQ(driver.commit("three four", slid), std::string("one two three four"));
    KD_CHECK_EQ(driver.frozen_prefix(), std::string("one two"));
}

KD_TEST(asrd_backend, every_second_fed_reaches_the_model_or_the_frozen_prefix) {
    // The invariant behind both tests above, driven over a long live-preview session where the
    // window really does slide repeatedly. Nothing asserts the *text* — the stitch is a
    // heuristic and base.py documents where it is wrong — but no second of audio may leave the
    // buffer without a transcription having seen it.
    PseudoStreamingDriver driver(4000, 1000);
    const std::vector<int16_t> second = pcm_ms(1000);

    size_t transcribed_seconds = 0;
    for (int i = 0; i < 30; ++i) {
        driver.feed(second.data(), second.size());
        std::vector<int16_t> window;
        bool slid = false;
        if (!driver.take_window(false, window, slid)) continue;
        transcribed_seconds += window.size() / 16000;
        driver.commit("hypothesis " + std::to_string(i), slid);
    }
    // Every hop re-transcribes the whole window, so the total far exceeds the 30 s fed; what
    // matters is that the buffer never got ahead of what was transcribed.
    KD_CHECK_GT(transcribed_seconds, size_t{30});
    KD_CHECK_EQ(driver.audio_ms(), uint64_t{30000});
}
