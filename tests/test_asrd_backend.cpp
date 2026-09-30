// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// tests/test_asrd_backend.cpp — the utterance buffer, the wire encodings, and the URL guard.
//
// The network half of `backend_openai.cpp` is exercised in `test_asrd_session.cpp`, against a
// stub HTTP service on loopback. What is here is everything that has no socket in it, which is
// most of where the bugs live:
//
//   * the utterance buffer, whose one invariant — fed audio is never dropped — is what replaced
//     the overlapping window and the freeze-on-slide stitching that openai-translator.md §8 used
//     to name as a standing risk, after it duplicated a user's dictation three times;
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

// Milliseconds of audio in `samples`, matching the driver's own arithmetic.
uint64_t samples_ms(size_t samples) { return static_cast<uint64_t>(samples) * 1000u / 16000u; }

// A buffer only just longer than a proposed cut at `cut_s`, so the tail falls under the minimum.
size_t cut_just_past(double cut_s) {
    return static_cast<size_t>(cut_s * 16000.0 + 0.5) + 16000u / 4u;
}

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

std::vector<Segment> two_segments() {
    // The shape measured against the real endpoint on 2026-08-30: a 21 s clip came back as two
    // segments with an interior boundary at 15.22 s, and cutting the audio there and decoding
    // the halves separately reproduced the whole transcript character for character.
    return {Segment{0.0, 15.22, "The first sentence. The second sentence. The third sentence."},
            Segment{15.22, 21.39, "The fourth sentence is still being revised."}};
}

std::string whole_text(const std::vector<Segment>& segs) {
    std::string out;
    for (const Segment& s : segs) {
        if (!out.empty()) out += ' ';
        out += s.text;
    }
    return out;
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

KD_TEST(asrd_backend, a_fields_only_multipart_has_no_file_part) {
    // The API-key check at load: a form a service must authenticate before it can complain about
    // the missing file, so a good key costs a 400 and no model is run.
    std::string content_type;
    const std::string body =
        encode_multipart({{"model", "qwen/qwen3-asr-1.7b"}}, {}, {}, {}, {}, content_type);
    const std::string boundary = content_type.substr(content_type.find('=') + 1);
    KD_CHECK(body.find("name=\"model\"\r\n\r\nqwen/qwen3-asr-1.7b\r\n") != std::string::npos);
    KD_CHECK(body.find("filename=") == std::string::npos);
    const std::string tail = "\r\n--" + boundary + "--\r\n";
    KD_CHECK_EQ(body.substr(body.size() - tail.size()), tail);
}

// ─── The API key ────────────────────────────────────────────────────────────────────────────

KD_TEST(asrd_backend, an_api_key_that_could_end_a_header_line_is_refused) {
    // It goes into `Authorization: Bearer <key>\r\n`. A CR or LF in it would end that line and
    // start another header of the key's choosing.
    KD_CHECK(api_key_problem("sk-or-v1-0123456789abcdef").empty());
    KD_CHECK(api_key_problem("").empty());  // no key is not a bad key
    KD_CHECK(!api_key_problem("sk-1\r\nX-Evil: 1").empty());
    KD_CHECK(!api_key_problem("sk-1\n").empty());
    KD_CHECK(!api_key_problem("sk 1").empty());
    KD_CHECK(!api_key_problem("sk-\xE2\x80\x9C" "1").empty());  // a smart quote from a paste
    KD_CHECK(!api_key_problem(std::string(1025, 'a')).empty());
}

// ─── The utterance buffer ──────────────────────────────────────────────────────────────

KD_TEST(asrd_backend, a_hop_is_due_once_hop_ms_of_audio_has_arrived) {
    PseudoStreamingDriver driver(1000);
    KD_CHECK(!driver.feed(pcm_ms(400).data(), pcm_ms(400).size()));
    const std::vector<int16_t> more = pcm_ms(500);
    KD_CHECK(!driver.feed(more.data(), more.size()));
    const std::vector<int16_t> last = pcm_ms(200);
    KD_CHECK(driver.feed(last.data(), last.size()));
    KD_CHECK_EQ(driver.audio_ms(), uint64_t{1100});
}

KD_TEST(asrd_backend, finalize_only_never_hops) {
    // The v1 default. Hops are off outright rather than pushed past a window length, so no
    // amount of audio makes one due and the finalize is the only transcription.
    PseudoStreamingDriver driver(0);
    KD_CHECK(!driver.hops_enabled());
    for (int i = 0; i < 5; ++i) {
        const std::vector<int16_t> chunk = pcm_ms(1000);
        KD_CHECK(!driver.feed(chunk.data(), chunk.size()));
    }
    std::vector<int16_t> window;
    KD_CHECK(!driver.take_window(/*force=*/false, window));
    KD_REQUIRE(driver.take_window(/*force=*/true, window));
    KD_CHECK_EQ(window.size(), size_t{5 * 16000});
}

KD_TEST(asrd_backend, take_window_refuses_when_nothing_has_arrived) {
    PseudoStreamingDriver driver(1000);
    std::vector<int16_t> window;
    KD_CHECK(!driver.take_window(/*force=*/true, window));
    KD_CHECK(!driver.any_audio());
}

KD_TEST(asrd_backend, reset_forgets_the_utterance_and_the_clock) {
    // Per utterance, not per session: audio carried across an utterance boundary would prepend
    // the previous utterance's speech to the next one's first partial.
    PseudoStreamingDriver driver(1000);
    const std::vector<int16_t> chunk = pcm_ms(3000);
    driver.feed(chunk.data(), chunk.size());
    std::vector<int16_t> window;
    KD_REQUIRE(driver.take_window(true, window));

    driver.reset();
    KD_CHECK_EQ(driver.audio_ms(), uint64_t{0});
    KD_CHECK(!driver.any_audio());
    KD_CHECK(!driver.take_window(true, window));
}

// ─── Committing the settled head on the model's own boundary ─────────────────────────

KD_TEST(asrd_backend, a_segment_table_gives_both_the_text_to_commit_and_where_to_cut) {
    // The whole point of `verbose_json`: the response relates text to time itself, so committing
    // a prefix needs no matching, no overlap and no guess. `committed_text` is not an estimate
    // of what the head decodes to — it is what the model said the head decodes to.
    const std::vector<Segment> segs = two_segments();
    ProposedCut cut;
    KD_REQUIRE(propose_segment_cut(OpenAiBackendConfig{}, 22 * 16000, whole_text(segs), segs, cut));
    KD_CHECK_EQ(cut.cut_samples, static_cast<size_t>(15.22 * 16000.0 + 0.5));
    KD_CHECK_EQ(cut.committed_text, segs[0].text);
    // The last segment is the one still being revised, and it is exactly what is *not* committed.
    KD_CHECK(cut.committed_text.find("fourth") == std::string::npos);
}

KD_TEST(asrd_backend, a_cut_partitions_the_recording_so_no_audio_is_described_twice) {
    // The invariant that replaced the stitcher, now stated across a commit rather than within
    // one decode: the committed text describes [0, cut) and nothing else, and the audio left in
    // the buffer is exactly [cut, end). Add them up and every sample is accounted for once.
    const std::vector<Segment> segs = two_segments();
    const size_t buffered = 22 * 16000;
    ProposedCut cut;
    KD_REQUIRE(propose_segment_cut(OpenAiBackendConfig{}, buffered, whole_text(segs), segs, cut));

    PseudoStreamingDriver driver(1000);
    const std::vector<int16_t> pcm(buffered, 1000);
    driver.feed(pcm.data(), pcm.size());
    driver.retain_tail(cut.cut_samples);

    std::vector<int16_t> window;
    KD_REQUIRE(driver.take_window(true, window));
    KD_CHECK_EQ(window.size(), buffered - cut.cut_samples);
    KD_CHECK_EQ(driver.audio_ms(), samples_ms(buffered - cut.cut_samples));
}

KD_TEST(asrd_backend, a_cut_is_refused_on_anything_that_cannot_be_fully_accounted_for) {
    const OpenAiBackendConfig cfg;
    const size_t buffered = 22 * 16000;
    ProposedCut cut;

    // One segment: the model drew no line inside this recording, so there is nothing to commit.
    {
        const std::vector<Segment> one{Segment{0.0, 21.39, "all of it in one piece"}};
        KD_CHECK(!propose_segment_cut(cfg, buffered, whole_text(one), one, cut));
    }
    // No segments at all — a service that ignored `verbose_json`. Not an error, just no cut.
    KD_CHECK(!propose_segment_cut(cfg, buffered, "some text", {}, cut));

    // The table does not reconcile with `text`. This is the check that makes the rest safe: if
    // the segments say something the transcript does not, committing part of them would commit
    // words the model never put in its answer.
    {
        std::vector<Segment> segs = two_segments();
        segs[0].text = "something else entirely";
        KD_CHECK(!propose_segment_cut(cfg, buffered, whole_text(two_segments()), segs, cut));
    }
    // Times that go backwards. `end` is about to be used as a sample offset; a table that is not
    // a partition in order is a table whose offsets mean nothing.
    {
        std::vector<Segment> segs = two_segments();
        segs[1].start_s = 9.0;
        KD_CHECK(!propose_segment_cut(cfg, buffered, whole_text(segs), segs, cut));
    }
    // A boundary past the audio we actually hold. Trusting it would drop the whole buffer.
    {
        const std::vector<Segment> segs = two_segments();
        KD_CHECK(!propose_segment_cut(cfg, 10 * 16000, whole_text(segs), segs, cut));
    }
    // Too little either side to be worth an utterance boundary.
    {
        const std::vector<Segment> segs{Segment{0.0, 0.4, "hi"}, Segment{0.4, 21.0, "the rest"}};
        KD_CHECK(!propose_segment_cut(cfg, buffered, whole_text(segs), segs, cut));
    }
    {
        const std::vector<Segment> segs = two_segments();
        KD_CHECK(!propose_segment_cut(cfg, cut_just_past(15.22), whole_text(segs), segs, cut));
    }
    // Switched off: back to committing the utterance whole.
    {
        OpenAiBackendConfig off = cfg;
        off.min_cut_head_ms = 60000;
        const std::vector<Segment> segs = two_segments();
        KD_CHECK(!propose_segment_cut(off, buffered, whole_text(segs), segs, cut));
    }
}

// ─── The invariant: audio is never dropped, so text is never duplicated ─────────────────────

KD_TEST(asrd_backend, every_take_hands_over_the_whole_utterance_from_its_first_sample) {
    // The one property this driver has, stated as a property rather than as a configuration.
    //
    // The duplication reported three times from real use — the same speech twice, joined at a
    // seam, each copy slightly reworded — came from a sliding window whose stitcher had to guess
    // where the dropped audio's transcript ended, and duplicated the overlap whenever the guess
    // failed. Twice that was answered by sizing the window so it could not slide, and twice a
    // number moved and it came back. There is no window now: the buffer only grows, so a take
    // can never contain less than everything, and there is no second text to join to.
    //
    // Two hours of hop sizes and utterance lengths, none of which can produce a short window.
    for (const uint32_t hop_ms : {200u, 1000u, 4000u}) {
        PseudoStreamingDriver driver(hop_ms);
        const std::vector<int16_t> second = pcm_ms(1000);
        std::vector<int16_t> window;
        size_t fed_seconds = 0;
        for (int i = 0; i < 120; ++i) {
            driver.feed(second.data(), second.size());
            ++fed_seconds;
            if (!driver.take_window(false, window)) continue;
            // Not "most of it", not "the last few seconds of it" — all of it, every time.
            KD_CHECK_EQ(window.size(), fed_seconds * 16000);
        }
        KD_REQUIRE(driver.take_window(/*force=*/true, window));
        KD_CHECK_EQ(window.size(), size_t{120 * 16000});
        KD_CHECK_EQ(driver.audio_ms(), uint64_t{120000});
    }
}

KD_TEST(asrd_backend, a_long_utterance_reaches_the_model_whole_in_finalize_only_too) {
    // The regression this test was written for, reported from real use as "a chunk of dictation
    // was never captured": in finalize-only nothing had been transcribed when the finalize
    // forced a take, so the slide froze nothing and every second before the last window
    // vanished. Both that hole and the duplication were the same mechanism seen from its two
    // sides, and both are gone with it.
    PseudoStreamingDriver driver(0);
    const std::vector<int16_t> chunk = pcm_ms(1000);
    for (int i = 0; i < 20; ++i) driver.feed(chunk.data(), chunk.size());

    std::vector<int16_t> window;
    KD_REQUIRE(driver.take_window(/*force=*/true, window));
    KD_CHECK_EQ(window.size(), size_t{20 * 16000});
}

KD_TEST(asrd_backend, the_hypothesis_is_the_window_transcript_with_nothing_added_to_it) {
    // There is no commit(), no frozen prefix and no join, which is the point: whatever the model
    // says about the recording is what the client is told, so the preview and the `final` differ
    // only in when they were asked. The driver hands over audio and nothing else; the transcript
    // path through `OpenAiBackend::transcribe` has no text-combining step left to get wrong.
    PseudoStreamingDriver driver(1000);
    const std::vector<int16_t> chunk = pcm_ms(1000);
    std::vector<int16_t> window;

    driver.feed(chunk.data(), chunk.size());
    KD_REQUIRE(driver.take_window(false, window));
    KD_CHECK_EQ(window.size(), size_t{16000});

    driver.feed(chunk.data(), chunk.size());
    KD_REQUIRE(driver.take_window(false, window));
    KD_CHECK_EQ(window.size(), size_t{2 * 16000});  // the first second is still in there
}

KD_TEST(asrd_backend, the_endpointer_is_what_bounds_the_buffer) {
    // The driver has no bound of its own — deliberately, since every bound it could enforce
    // would have to drop or stitch audio to enforce it. So the bound belongs to the endpointer,
    // and `connection.cpp` refuses to serve a connection without one. This pins the default,
    // which is the thing that would have to change for the refusal to start firing.
    KD_CHECK_GT(VadConfig{}.max_utterance_ms, 0.0);
}


// ─── Vocabulary hints on the wire ───────────────────────────────────────────────────────────

KD_TEST(asrd_backend, context_terms_are_joined_for_the_prompt_field) {
    const std::string out = join_context_prompt({"kvit-ui", "piperd", "heic"}, kMaxPromptBytes);
    KD_CHECK_EQ(out, std::string("kvit-ui, piperd, heic"));
}

KD_TEST(asrd_backend, an_empty_vocabulary_sends_no_prompt_at_all) {
    // Distinct from a blank one: transcribe_batch omits the field when this is empty, because a
    // blank prompt is still a field the server has to interpret.
    KD_CHECK(join_context_prompt({}, kMaxPromptBytes).empty());
    KD_CHECK(join_context_prompt({"", "   ", ""}, kMaxPromptBytes).empty());
}

KD_TEST(asrd_backend, terms_are_stripped_and_blanks_do_not_leave_stray_separators) {
    KD_CHECK_EQ(join_context_prompt({"  kvit-ui ", "", " heic"}, kMaxPromptBytes),
                std::string("kvit-ui, heic"));
}

KD_TEST(asrd_backend, the_prompt_never_exceeds_the_endpoints_cap) {
    // The endpoint answers 4097 bytes with HTTP 400, which would cost the whole utterance. The
    // client's own limit is 256 terms of 128 bytes, so a legal start.context can be 32 KB.
    std::vector<std::string> many(256, std::string(128, 'x'));
    const std::string out = join_context_prompt(many, kMaxPromptBytes);
    KD_CHECK_LE(out.size(), kMaxPromptBytes);
    KD_CHECK(!out.empty());
}

KD_TEST(asrd_backend, truncation_is_on_a_term_boundary_and_is_counted) {
    // Cut mid-term and the model is biased toward half a word, which is worse than no bias.
    size_t dropped = 0;
    const std::string a(10, 'a'), b(10, 'b'), c(10, 'c');
    // Room for "aaa…, bbb…" (22 bytes) but not for a third.
    const std::string out = join_context_prompt({a, b, c}, 22, &dropped);
    KD_CHECK_EQ(out, a + ", " + b);
    KD_CHECK_EQ(dropped, size_t{1});
}

KD_TEST(asrd_backend, a_short_term_after_an_oversized_one_still_gets_through) {
    // Order is not priority: skipping the term that does not fit and carrying on sends more of
    // the user's vocabulary than stopping at the first refusal would.
    size_t dropped = 0;
    const std::string out = join_context_prompt({"ab", std::string(100, 'x'), "cd"}, 10, &dropped);
    KD_CHECK_EQ(out, std::string("ab, cd"));
    KD_CHECK_EQ(dropped, size_t{1});
}

KD_TEST(asrd_backend, a_single_term_larger_than_the_cap_is_dropped_not_truncated) {
    size_t dropped = 0;
    KD_CHECK(join_context_prompt({std::string(50, 'x')}, 10, &dropped).empty());
    KD_CHECK_EQ(dropped, size_t{1});
}
