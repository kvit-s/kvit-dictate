// Suite `framing` — FrameReader and the frame encoders (src/protocol.cpp, docs/protocol.md §3).
//
// The fixture runner in test_fixtures.cpp covers the same ground from the shared cross-language
// files. This file drives the decoder directly instead, because a fixture failure tells you
// "case 006 disagreed" while these tell you which invariant broke — and because several of the
// invariants below (sticky errors, buffer growth, payload-view lifetime, the alignment rule in
// decode_audio_payload) have no representation in the fixture schema at all.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "kd_test.h"
#include "protocol.h"

namespace {

using kd::proto::Frame;
using kd::proto::FrameReader;
using kd::proto::FrameStatus;
using kd::proto::FrameType;

// Two hex digits per byte, lowercase or uppercase. Returns "" on malformed input, which every
// caller here treats as a test bug rather than a condition under test.
std::string unhex(std::string_view h) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    if ((h.size() % 2) != 0) return out;
    out.reserve(h.size() / 2);
    for (size_t i = 0; i < h.size(); i += 2) {
        const int hi = nib(h[i]);
        const int lo = nib(h[i + 1]);
        if (hi < 0 || lo < 0) return std::string();
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

// A frame as the test wants to look at it: type plus an owned copy of the payload. Owned,
// because Frame::payload is a view into the reader's buffer and dies at the next call.
struct Decoded {
    FrameType type = FrameType::Control;
    std::string payload;
};

struct DrainResult {
    std::vector<Decoded> frames;
    bool errored = false;
    bool end_of_stream = false;
    kd::proto::ProtocolError error;
};

// Feed `chunks` in order, draining to NeedMore after every push (the loop shape protocol.h
// documents; a one-frame-per-read caller fails fixture 002), then signal EOF if asked.
DrainResult drive(const std::vector<std::string>& chunks, bool eof) {
    DrainResult r;
    FrameReader reader;
    auto drain = [&]() {
        for (;;) {
            Frame f;
            const FrameStatus st = reader.next(f);
            if (st == FrameStatus::Frame) {
                r.frames.push_back(Decoded{f.type, std::string(f.payload)});
                continue;
            }
            if (st == FrameStatus::Error) {
                r.errored = true;
                r.error = reader.error();
            } else if (st == FrameStatus::EndOfStream) {
                r.end_of_stream = true;
            }
            return;
        }
    };
    for (const std::string& c : chunks) {
        reader.push(c);
        drain();
        if (r.errored) return r;
    }
    if (eof) {
        reader.notify_eof();
        drain();
    }
    return r;
}

std::vector<std::string> split_one_byte_at_a_time(const std::string& all) {
    std::vector<std::string> out;
    out.reserve(all.size());
    for (char c : all) out.emplace_back(1, c);
    return out;
}

std::string concat(const std::vector<std::string>& parts) {
    std::string s;
    for (const std::string& p : parts) s += p;
    return s;
}

const char* kHelloFrame = "18000000017b2274223a2268656c6c6f222c2270726f746f223a317d";
const char* kHelloPayload = "7b2274223a2268656c6c6f222c2270726f746f223a317d";

}  // namespace

// ─── Encoding ───────────────────────────────────────────────────────────────────────────────

KD_TEST(framing, length_field_covers_the_type_byte) {
    // §3.1's single most load-bearing sentence: length = 1 + sizeof(payload). A two-byte payload
    // therefore produces a length field of 3, and the whole frame is 7 bytes on the wire.
    const std::vector<uint8_t> f = kd::proto::encode_frame(FrameType::Control, "{}");
    KD_REQUIRE_EQ(f.size(), size_t{7});
    KD_CHECK_EQ(int(f[0]), 3);
    KD_CHECK_EQ(int(f[1]), 0);
    KD_CHECK_EQ(int(f[2]), 0);
    KD_CHECK_EQ(int(f[3]), 0);
    KD_CHECK_EQ(int(f[4]), 0x01);
    KD_CHECK_EQ(int(f[5]), '{');
    KD_CHECK_EQ(int(f[6]), '}');
}

KD_TEST(framing, empty_payload_encodes_length_one) {
    // length == 0 is structurally impossible, which is the whole point of covering the type
    // byte: "forgot to add the type byte" becomes a loud bad_frame instead of a silent desync.
    const std::vector<uint8_t> f = kd::proto::encode_frame(FrameType::Audio, "");
    KD_REQUIRE_EQ(f.size(), size_t{5});
    KD_CHECK_EQ(int(f[0]), 1);
    KD_CHECK_EQ(int(f[4]), 0x02);
}

KD_TEST(framing, encoder_refuses_an_oversize_payload) {
    const std::string too_big(kd::proto::kMaxPayload + 1, 'x');
    std::vector<uint8_t> out;
    KD_CHECK(!kd::proto::encode_frame(FrameType::Control, too_big, out));
    KD_CHECK_EQ(out.size(), size_t{0});  // "Returns false — writing nothing"

    const std::string exactly(kd::proto::kMaxPayload, 'x');
    KD_CHECK(kd::proto::encode_frame(FrameType::Control, exactly, out));
    KD_CHECK_EQ(out.size(), size_t{4} + 1 + kd::proto::kMaxPayload);
    KD_CHECK_EQ(out.size(), size_t{kd::proto::kMaxFrameOnWire});
}

KD_TEST(framing, encode_frame_appends_rather_than_replaces) {
    // The transport packs one frame per write, but the encoder is an appender so that a caller
    // building a scratch buffer does not have to clear it. Two frames appended must decode as
    // two frames.
    std::vector<uint8_t> out;
    KD_REQUIRE(kd::proto::encode_frame(FrameType::Control, "{}", out));
    // Explicit length, because audio payloads contain NUL bytes and a bare "\x01\x00" literal
    // would decay to a const char* whose strlen is 1 — silently encoding a 1-byte frame and
    // making this test assert the wrong total. s16le samples hit this constantly.
    KD_REQUIRE(kd::proto::encode_frame(FrameType::Audio, std::string_view("\x01\x00", 2), out));
    KD_CHECK_EQ(out.size(), size_t{7 + 7});

    const DrainResult r =
        drive({std::string(reinterpret_cast<const char*>(out.data()), out.size())}, true);
    KD_REQUIRE_EQ(r.frames.size(), size_t{2});
    KD_CHECK(r.frames[0].type == FrameType::Control);
    KD_CHECK(r.frames[1].type == FrameType::Audio);
    KD_CHECK(r.end_of_stream);
}

KD_TEST(framing, encode_audio_frame_is_little_endian_and_bounded) {
    const int16_t samples[] = {0, 1, -1, 256, -32768, 32767};
    std::vector<uint8_t> out;
    KD_REQUIRE(kd::proto::encode_audio_frame(samples, 6, out));
    KD_REQUIRE_EQ(out.size(), size_t{4 + 1 + 12});
    KD_CHECK_EQ(int(out[0]), 13);      // 1 + 12
    KD_CHECK_EQ(int(out[4]), 0x02);
    KD_CHECK_EQ(int(out[5]), 0x00);    // 0
    KD_CHECK_EQ(int(out[6]), 0x00);
    KD_CHECK_EQ(int(out[7]), 0x01);    // 1 -> 01 00
    KD_CHECK_EQ(int(out[8]), 0x00);
    KD_CHECK_EQ(int(out[9]), 0xFF);    // -1 -> ff ff
    KD_CHECK_EQ(int(out[10]), 0xFF);
    KD_CHECK_EQ(int(out[11]), 0x00);   // 256 -> 00 01
    KD_CHECK_EQ(int(out[12]), 0x01);
    KD_CHECK_EQ(int(out[13]), 0x00);   // -32768 -> 00 80
    KD_CHECK_EQ(int(out[14]), 0x80);
    KD_CHECK_EQ(int(out[15]), 0xFF);   // 32767 -> ff 7f
    KD_CHECK_EQ(int(out[16]), 0x7F);

    // 32768 samples is exactly MAX_PAYLOAD; one more overflows the frame, not the buffer.
    std::vector<int16_t> big(kd::proto::kMaxPayload / 2 + 1, 0);
    std::vector<uint8_t> refused;
    KD_CHECK(!kd::proto::encode_audio_frame(big.data(), big.size(), refused));
    KD_CHECK_EQ(refused.size(), size_t{0});
    KD_CHECK(kd::proto::encode_audio_frame(big.data(), big.size() - 1, refused));
    KD_CHECK_EQ(refused.size(), size_t{kd::proto::kMaxFrameOnWire});
}

// ─── Fragmentation and coalescing ───────────────────────────────────────────────────────────

KD_TEST(framing, fragmented_and_whole_deliveries_agree) {
    // Fixture 001's shape, driven directly: the chunk boundaries fall inside the length prefix,
    // inside a payload, and across a frame boundary.
    const std::vector<std::string> chunks = {unhex("1800"), unhex("0000017b227422"),
                                             unhex("3a2268656c6c6f222c2270726f746f223a317d050000"),
                                             unhex("00020100ffff")};
    const DrainResult as_delivered = drive(chunks, true);
    KD_REQUIRE(!as_delivered.errored);
    KD_REQUIRE_EQ(as_delivered.frames.size(), size_t{2});
    KD_CHECK(as_delivered.frames[0].type == FrameType::Control);
    KD_CHECK_EQ(as_delivered.frames[0].payload, unhex(kHelloPayload));
    KD_CHECK(as_delivered.frames[1].type == FrameType::Audio);
    KD_CHECK_EQ(as_delivered.frames[1].payload, unhex("0100ffff"));

    const std::string all = concat(chunks);
    const DrainResult whole = drive({all}, true);
    const DrainResult dribbled = drive(split_one_byte_at_a_time(all), true);

    KD_REQUIRE_EQ(whole.frames.size(), as_delivered.frames.size());
    KD_REQUIRE_EQ(dribbled.frames.size(), as_delivered.frames.size());
    for (size_t i = 0; i < as_delivered.frames.size(); ++i) {
        KD_CHECK(whole.frames[i].type == as_delivered.frames[i].type);
        KD_CHECK(dribbled.frames[i].type == as_delivered.frames[i].type);
        KD_CHECK_EQ(whole.frames[i].payload, as_delivered.frames[i].payload);
        KD_CHECK_EQ(dribbled.frames[i].payload, as_delivered.frames[i].payload);
    }
    KD_CHECK(whole.end_of_stream && dribbled.end_of_stream);
}

KD_TEST(framing, three_frames_in_one_read_are_all_drained) {
    // The caller-side half of fixture 002: a reader that returns one frame per push loses two of
    // these three. push() then a single next() must therefore NOT be the shape anyone copies.
    std::string stream = unhex(kHelloFrame);
    stream += unhex("05000000020100ffff");
    stream += unhex("05000000020200fefe");

    FrameReader r;
    r.push(stream);
    int count = 0;
    for (;;) {
        Frame f;
        const FrameStatus st = r.next(f);
        if (st != FrameStatus::Frame) {
            KD_CHECK(st == FrameStatus::NeedMore);
            break;
        }
        ++count;
    }
    KD_CHECK_EQ(count, 3);
    KD_CHECK_EQ(r.buffered(), size_t{0});
    KD_CHECK_EQ(r.stream_offset(), uint64_t{28 + 9 + 9});
}

KD_TEST(framing, a_split_length_prefix_is_need_more_not_an_error) {
    // Three bytes of 0xFFFFFFFF have arrived. A decoder that range-checked what it had so far
    // would report frame_too_large on a length it has not finished reading.
    FrameReader r;
    r.push(unhex("ffffff"));
    Frame f;
    KD_CHECK(r.next(f) == FrameStatus::NeedMore);
    KD_CHECK_EQ(r.buffered(), size_t{3});
    r.push(unhex("ff01"));
    KD_CHECK(r.next(f) == FrameStatus::Error);
    KD_CHECK(r.error().code == kd::proto::ErrorCode::frame_too_large);
}

KD_TEST(framing, zero_length_payload_is_legal_and_the_stream_continues) {
    // Fixture 003. A zero-sample audio frame is a legal state so that a sender's empty-buffer
    // edge case does not have to be special-cased into a protocol error.
    const DrainResult r = drive({unhex("01000000") + unhex("02") + unhex(kHelloFrame)}, true);
    KD_REQUIRE(!r.errored);
    KD_REQUIRE_EQ(r.frames.size(), size_t{2});
    KD_CHECK(r.frames[0].type == FrameType::Audio);
    KD_CHECK_EQ(r.frames[0].payload.size(), size_t{0});
    KD_CHECK(r.frames[1].type == FrameType::Control);
    KD_CHECK_EQ(r.frames[1].payload, unhex(kHelloPayload));
}

// ─── Size limits: fatal, never truncating, never resynchronising ────────────────────────────

KD_TEST(framing, max_length_field_is_accepted) {
    // 65537 = 0x00010001. The largest legal frame carries a 65536-byte payload.
    std::string stream = unhex("0100010002");
    stream.append(kd::proto::kMaxPayload, '\x7f');
    const DrainResult r = drive({stream}, true);
    KD_REQUIRE(!r.errored);
    KD_REQUIRE_EQ(r.frames.size(), size_t{1});
    KD_CHECK_EQ(r.frames[0].payload.size(), size_t{kd::proto::kMaxPayload});
    KD_CHECK(r.end_of_stream);
}

KD_TEST(framing, one_byte_over_the_limit_is_rejected_on_the_header_alone) {
    // 65538 = 0x00010002. Only the 5 header-and-type bytes are ever supplied; the decoder must
    // fail without waiting for, reading, or reserving room for a payload it will never see.
    FrameReader r;
    r.push(unhex("0200010002"));
    Frame f;
    KD_REQUIRE(r.next(f) == FrameStatus::Error);
    KD_CHECK(r.error().code == kd::proto::ErrorCode::frame_too_large);
    KD_CHECK(r.error().fatal);
    KD_CHECK_EQ(r.error().at_offset, uint64_t{0});
    // Nothing was consumed and nothing was staged: the reader is still holding exactly the five
    // bytes it was given. A decoder that pre-sized a payload buffer would be holding 65 KiB.
    KD_CHECK_EQ(r.buffered(), size_t{5});
    KD_CHECK_EQ(r.stream_offset(), uint64_t{0});
}

KD_TEST(framing, length_is_unsigned_and_0xffffffff_does_not_sign_extend) {
    // Fixture 007. A reader that sign-extends sees -1, and a reader that then computes
    // `4 + length` as a signed size gets 3 — i.e. it happily "accepts" a 3-byte frame.
    FrameReader r;
    r.push(unhex("ffffffff01"));
    Frame f;
    KD_REQUIRE(r.next(f) == FrameStatus::Error);
    KD_CHECK(r.error().code == kd::proto::ErrorCode::frame_too_large);
    KD_CHECK_EQ(r.error().at_offset, uint64_t{0});
    KD_CHECK_EQ(r.buffered(), size_t{5});
    KD_CHECK_NE(r.error().msg.find("4294967295"), std::string::npos);
}

KD_TEST(framing, length_zero_is_bad_frame_and_earlier_frames_still_arrive) {
    // Fixture 006, and the reason errors surface through next() rather than through push(): a
    // decoder that threw away decoded frames on a later bad header would lose the hello.
    const DrainResult r = drive({unhex(kHelloFrame) + unhex("0000000001")}, true);
    KD_REQUIRE(r.errored);
    KD_REQUIRE_EQ(r.frames.size(), size_t{1});
    KD_CHECK_EQ(r.frames[0].payload, unhex(kHelloPayload));
    KD_CHECK(r.error.code == kd::proto::ErrorCode::bad_frame);
    KD_CHECK(r.error.fatal);
    KD_CHECK_EQ(r.error.at_offset, uint64_t{28});  // start of the malformed header
}

KD_TEST(framing, unknown_type_byte_is_fatal_not_skippable) {
    // Fixture 009. Both peers agreed on proto 1 in the handshake, so an undefined type means one
    // of them is broken. Skipping the frame would leave the stream looking healthy.
    FrameReader r;
    r.push(unhex("03000000030102"));
    Frame f;
    KD_REQUIRE(r.next(f) == FrameStatus::Error);
    KD_CHECK(r.error().code == kd::proto::ErrorCode::unknown_frame_type);
    KD_CHECK_EQ(r.error().at_offset, uint64_t{4});  // the type byte itself
    KD_CHECK(!kd::proto::is_known_frame_type(0x00));
    KD_CHECK(kd::proto::is_known_frame_type(0x01));
    KD_CHECK(kd::proto::is_known_frame_type(0x02));
    KD_CHECK(!kd::proto::is_known_frame_type(0x03));
    KD_CHECK(!kd::proto::is_known_frame_type(0xFF));
}

KD_TEST(framing, an_error_is_sticky_and_no_resynchronisation_is_attempted) {
    // §3.3: there is no framing marker to resynchronise *to*, so a scan would find false headers
    // and could deliver attacker- or bug-chosen bytes as a valid message. Pushing a perfectly
    // good frame after the fault must not revive the reader.
    FrameReader r;
    r.push(unhex("0000000001"));
    Frame f;
    KD_REQUIRE(r.next(f) == FrameStatus::Error);
    const kd::proto::ErrorCode first = r.error().code;
    r.push(unhex(kHelloFrame));
    KD_CHECK(r.next(f) == FrameStatus::Error);
    KD_CHECK(r.next(f) == FrameStatus::Error);
    KD_CHECK(r.error().code == first);
    r.notify_eof();
    KD_CHECK(r.next(f) == FrameStatus::Error);  // not EndOfStream: the fault outranks the EOF
}

KD_TEST(framing, reset_returns_the_reader_to_its_initial_state) {
    FrameReader r;
    r.push(unhex("0000000001"));
    Frame f;
    KD_REQUIRE(r.next(f) == FrameStatus::Error);
    r.reset();
    KD_CHECK_EQ(r.buffered(), size_t{0});
    KD_CHECK_EQ(r.stream_offset(), uint64_t{0});
    r.push(unhex(kHelloFrame));
    KD_REQUIRE(r.next(f) == FrameStatus::Frame);
    KD_CHECK_EQ(std::string(f.payload), unhex(kHelloPayload));
    KD_CHECK(r.next(f) == FrameStatus::NeedMore);
}

// ─── End of stream ──────────────────────────────────────────────────────────────────────────

KD_TEST(framing, eof_on_a_frame_boundary_is_orderly) {
    // Fixture 010. Orderly shutdown is not an error, and a reader that reported one would make
    // every clean teardown look like a crash in the logs.
    const DrainResult r = drive({unhex(kHelloFrame) + unhex(kHelloFrame)}, true);
    KD_CHECK(!r.errored);
    KD_CHECK_EQ(r.frames.size(), size_t{2});
    KD_CHECK(r.end_of_stream);
}

KD_TEST(framing, eof_mid_frame_is_truncated_frame_and_the_partial_is_never_delivered) {
    // Fixture 011. The already-decoded frames stand; the partial one is discarded rather than
    // handed up in truncated form, which is how a half-received `final` would otherwise reach
    // the injector.
    std::string stream = unhex(kHelloFrame);
    stream += unhex("18000000017b2274223a2268656c6c6f22");  // 17 of 28 bytes
    const DrainResult r = drive({stream}, true);
    KD_REQUIRE(r.errored);
    KD_REQUIRE_EQ(r.frames.size(), size_t{1});
    KD_CHECK(r.error.code == kd::proto::ErrorCode::truncated_frame);
    KD_CHECK(r.error.fatal);
    KD_CHECK_EQ(r.error.at_offset, uint64_t{28});
    KD_CHECK(!r.end_of_stream);
}

KD_TEST(framing, eof_with_a_partial_length_prefix_is_also_truncated_frame) {
    // Fewer than four bytes buffered is still a partial frame — the reader must not mistake it
    // for a clean close just because it never got far enough to learn the length.
    const DrainResult r = drive({unhex("1800")}, true);
    KD_CHECK(r.errored);
    KD_CHECK(!r.end_of_stream);
    KD_CHECK(r.error.code == kd::proto::ErrorCode::truncated_frame);
    KD_CHECK_EQ(r.error.at_offset, uint64_t{0});
}

KD_TEST(framing, eof_on_an_empty_stream_is_orderly) {
    const DrainResult r = drive({}, true);
    KD_CHECK(!r.errored);
    KD_CHECK_EQ(r.frames.size(), size_t{0});
    KD_CHECK(r.end_of_stream);
}

// ─── Payload view lifetime ──────────────────────────────────────────────────────────────────

KD_TEST(framing, payload_is_a_view_that_must_be_copied_before_the_next_call) {
    // Not a test that the view *dangles* — that would be undefined behaviour to observe.
    // It asserts the usable half of the contract: the view is valid up to the next call, so a
    // caller that copies inside the loop keeps correct bytes for every frame.
    std::string stream = unhex(kHelloFrame);
    stream += unhex("05000000020100ffff");

    FrameReader r;
    r.push(stream);

    Frame f;
    KD_REQUIRE(r.next(f) == FrameStatus::Frame);
    // Compared as integers, not as `const char*`: the framework's equal() overload for two
    // `const char*` is a strcmp, and a payload view is not NUL-terminated.
    const uintptr_t first_data = reinterpret_cast<uintptr_t>(f.payload.data());
    const std::string first_copy(f.payload);
    KD_CHECK_EQ(first_copy, unhex(kHelloPayload));

    KD_REQUIRE(r.next(f) == FrameStatus::Frame);
    KD_CHECK_EQ(std::string(f.payload), unhex("0100ffff"));
    // The second frame's payload comes from the same buffer, further along — which is exactly
    // why the first frame's view could not be kept.
    KD_CHECK_NE(reinterpret_cast<uintptr_t>(f.payload.data()), first_data);
    KD_CHECK_EQ(first_copy, unhex(kHelloPayload));  // the copy is untouched
}

// ─── §6.3 Audio payload decoding ────────────────────────────────────────────────────────────

KD_TEST(framing, decode_audio_payload_rejects_an_odd_length) {
    // An odd length means the sender split a sample, i.e. its buffer arithmetic is wrong.
    std::vector<int16_t> out{1, 2, 3};
    kd::proto::ProtocolError err;
    KD_CHECK(!kd::proto::decode_audio_payload(std::string_view("\x01\x02\x03", 3), out, err));
    KD_CHECK(err.code == kd::proto::ErrorCode::bad_message);
    KD_CHECK(err.fatal);
    KD_CHECK_EQ(out.size(), size_t{0});  // cleared, not left holding stale samples
}

KD_TEST(framing, decode_audio_payload_accepts_an_empty_payload) {
    // Fixture 003: legal, and a no-op rather than an error.
    std::vector<int16_t> out{7};
    kd::proto::ProtocolError err;
    KD_CHECK(kd::proto::decode_audio_payload(std::string_view(), out, err));
    KD_CHECK_EQ(out.size(), size_t{0});
}

KD_TEST(framing, decode_audio_payload_copies_from_an_unaligned_view) {
    // The gotcha this function exists for: reinterpret_cast<const int16_t*>(payload.data()) is
    // UB on a view with no 2-byte alignment guarantee. Here the payload deliberately starts at
    // an odd address inside a larger buffer.
    const std::string pcm = unhex("0100ffff00800a00");  // 1, -1, -32768, 10
    std::vector<char> raw(pcm.size() + 2, '\0');
    // The base address of a heap block is even in practice but is not promised to be, so the
    // padding is chosen from the address actually handed out rather than assumed.
    const size_t pad = (reinterpret_cast<uintptr_t>(raw.data()) % 2 == 0) ? 1u : 0u;
    std::memcpy(raw.data() + pad, pcm.data(), pcm.size());
    const std::string_view payload(raw.data() + pad, pcm.size());
    KD_REQUIRE_EQ(reinterpret_cast<uintptr_t>(payload.data()) % 2, uintptr_t{1});

    std::vector<int16_t> out;
    kd::proto::ProtocolError err;
    KD_REQUIRE(kd::proto::decode_audio_payload(payload, out, err));
    KD_REQUIRE_EQ(out.size(), size_t{4});
    KD_CHECK_EQ(out[0], int16_t{1});
    KD_CHECK_EQ(out[1], int16_t{-1});
    KD_CHECK_EQ(out[2], int16_t{-32768});
    KD_CHECK_EQ(out[3], int16_t{10});
}

KD_TEST(framing, audio_round_trips_through_a_real_frame) {
    // And the payload a FrameReader hands back is itself at buffer offset 5, i.e. odd whenever
    // the buffer is even-aligned — so the alignment hazard above is the normal case, not a
    // contrived one.
    const int16_t samples[] = {-32768, -1, 0, 1, 32767};
    std::vector<uint8_t> wire;
    KD_REQUIRE(kd::proto::encode_audio_frame(samples, 5, wire));

    FrameReader r;
    r.push(wire.data(), wire.size());
    Frame f;
    KD_REQUIRE(r.next(f) == FrameStatus::Frame);
    KD_REQUIRE(f.type == FrameType::Audio);

    std::vector<int16_t> out;
    kd::proto::ProtocolError err;
    KD_REQUIRE(kd::proto::decode_audio_payload(f.payload, out, err));
    KD_REQUIRE_EQ(out.size(), size_t{5});
    for (size_t i = 0; i < 5; ++i) KD_CHECK_EQ(out[i], samples[i]);
}

KD_TEST(framing, constants_match_the_specification) {
    // §1.1, restated once here so that a well-meaning edit to protocol.h shows up as a test
    // failure rather than as a cross-language disagreement discovered by the Python suite.
    KD_CHECK_EQ(kd::proto::kProtoVersion, 1);
    KD_CHECK_EQ(kd::proto::kMaxPayload, uint32_t{65536});
    KD_CHECK_EQ(kd::proto::kMinLengthField, uint32_t{1});
    KD_CHECK_EQ(kd::proto::kMaxLengthField, uint32_t{65537});
    KD_CHECK_EQ(kd::proto::kMaxFrameOnWire, uint32_t{65541});
    KD_CHECK_EQ(kd::proto::kMaxLengthField, kd::proto::kMaxPayload + 1);
    KD_CHECK_EQ(kd::proto::kMaxFrameOnWire, kd::proto::kMaxLengthField + 4);
}
