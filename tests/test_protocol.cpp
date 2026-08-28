// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Suite `protocol` — the message codec and the pure validators (src/protocol.cpp).
//
// docs/protocol.md §4 (encoding), §6 (catalogue), §7 (identity and ordering), §8 (stable_bytes)
// and §11 (error codes). The fixture runner exercises the same code from the shared files; this
// file covers the parts the fixture schema has no way to express — round-tripping, the layer
// ordering of failures, the required/optional/range rules of every message in the catalogue, and
// the exact wire spellings that the Python suite compares as strings.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "kd_test.h"
#include "protocol.h"

namespace {

using kd::proto::ErrorCode;
using kd::proto::Message;
using kd::proto::MessageType;
using kd::proto::ProtocolError;

// Decode a control payload, requiring success. Returns a default Message on failure so the test
// can carry on to its next check after a KD_CHECK rather than aborting the whole file.
Message must_decode(std::string_view payload) {
    Message m;
    ProtocolError err;
    if (!kd::proto::decode_control(payload, m, err)) {
        KD_FAIL("decode_control failed: " + err.to_string() + " on " +
                kd::test::quote(payload));
    }
    return m;
}

// Decode expecting a specific failure, and report the code that actually came back.
void expect_error(std::string_view payload, ErrorCode want) {
    Message m;
    ProtocolError err;
    const bool ok = kd::proto::decode_control(payload, m, err);
    if (ok) {
        KD_FAIL("expected " + std::string(kd::proto::to_string(want)) + " but decode succeeded: " +
                kd::test::quote(payload));
        return;
    }
    KD_CHECK_EQ(kd::proto::to_string(err.code), kd::proto::to_string(want));
}

Message round_trip(const Message& m) { return must_decode(kd::proto::encode_control(m)); }

}  // namespace

// ─── Round-tripping the whole catalogue ─────────────────────────────────────────────────────

KD_TEST(protocol, hello_round_trips) {
    kd::proto::HelloMsg h;
    h.proto = 1;
    h.client = "kvit-dictate/0.1";
    const Message rt = round_trip(Message::of(h));
    KD_REQUIRE(rt.type() == MessageType::Hello);
    const auto* got = rt.get_if<kd::proto::HelloMsg>();
    KD_REQUIRE(got != nullptr);
    KD_CHECK_EQ(got->proto, 1);
    KD_REQUIRE(got->client.has_value());
    KD_CHECK_EQ(*got->client, std::string("kvit-dictate/0.1"));

    // The optional field is omitted, not emitted as null — the protocol defines no null and this
    // decoder rejects one, so emitting one would make us unable to read our own output.
    kd::proto::HelloMsg bare;
    const std::string json = kd::proto::encode_control(Message::of(bare));
    KD_CHECK_EQ(json.find("null"), std::string::npos);
    KD_CHECK_EQ(json.find("client"), std::string::npos);
    KD_CHECK(!round_trip(Message::of(bare)).get_if<kd::proto::HelloMsg>()->client.has_value());
}

KD_TEST(protocol, start_round_trips_and_distinguishes_absent_from_empty_context) {
    kd::proto::StartMsg s;
    s.session_id = "s7";
    s.context = {"llama.cpp", "Qwen3"};
    s.context_present = true;
    const Message rt = round_trip(Message::of(s));
    const auto* got = rt.get_if<kd::proto::StartMsg>();
    KD_REQUIRE(got != nullptr);
    KD_CHECK_EQ(got->session_id, std::string("s7"));
    KD_CHECK_EQ(got->sample_rate, 16000);
    KD_CHECK_EQ(got->channels, 1);
    KD_CHECK_EQ(got->format, std::string("s16le"));
    KD_REQUIRE_EQ(got->context.size(), size_t{2});
    KD_CHECK_EQ(got->context[0], std::string("llama.cpp"));
    KD_CHECK(got->context_present);

    // plan.md §4 compares context-enabled and context-free runs separately, so "sent an empty
    // array" and "did not send the field" must survive a round trip as different things.
    kd::proto::StartMsg empty_sent;
    empty_sent.session_id = "s7";
    empty_sent.context_present = true;
    const Message rt_empty = round_trip(Message::of(empty_sent));
    const auto* e = rt_empty.get_if<kd::proto::StartMsg>();
    KD_REQUIRE(e != nullptr);
    KD_CHECK(e->context_present);
    KD_CHECK_EQ(e->context.size(), size_t{0});

    kd::proto::StartMsg absent;
    absent.session_id = "s7";
    const std::string json = kd::proto::encode_control(Message::of(absent));
    KD_CHECK_EQ(json.find("context"), std::string::npos);
    const Message rt_absent = round_trip(Message::of(absent));
    const auto* a = rt_absent.get_if<kd::proto::StartMsg>();
    KD_REQUIRE(a != nullptr);
    KD_CHECK(!a->context_present);
}

KD_TEST(protocol, stop_cancel_and_discontinuity_round_trip) {
    // Every round_trip() result is bound to a named local before anything points into it: the
    // structs hold std::string members, and a pointer taken from a temporary Message would
    // dangle at the end of its own statement.
    kd::proto::StopMsg stop;
    stop.session_id = "s7";
    const Message rt_stop = round_trip(Message::of(stop));
    KD_CHECK_EQ(rt_stop.get_if<kd::proto::StopMsg>()->session_id, std::string("s7"));

    kd::proto::CancelMsg cancel;
    cancel.session_id = "s7";
    const Message rt_cancel = round_trip(Message::of(cancel));
    KD_CHECK_EQ(rt_cancel.get_if<kd::proto::CancelMsg>()->session_id, std::string("s7"));

    kd::proto::DiscontinuityMsg d;
    d.session_id = "s7";
    d.gap_ms = 0;  // "a discontinuity occurred, duration unknown" — legal, not a missing value
    d.reason = "AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY";
    const Message rt_d = round_trip(Message::of(d));
    const auto* got = rt_d.get_if<kd::proto::DiscontinuityMsg>();
    KD_REQUIRE(got != nullptr);
    KD_CHECK_EQ(got->gap_ms, int64_t{0});
    KD_REQUIRE(got->reason.has_value());
    KD_CHECK_EQ(*got->reason, std::string("AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY"));
}

KD_TEST(protocol, server_messages_round_trip) {
    kd::proto::HelloAckMsg ack;
    ack.proto = 1;
    ack.server = "kvit-asr/0.1";
    const Message rt_ack = round_trip(Message::of(ack));
    const auto* a = rt_ack.get_if<kd::proto::HelloAckMsg>();
    KD_REQUIRE(a != nullptr);
    KD_CHECK_EQ(a->proto, 1);

    kd::proto::LoadingMsg loading;
    loading.model = "example/streaming-en";
    loading.stage = "weights";
    loading.progress = 42;
    const Message rt_loading = round_trip(Message::of(loading));
    const auto* l = rt_loading.get_if<kd::proto::LoadingMsg>();
    KD_REQUIRE(l != nullptr);
    KD_CHECK_EQ(l->model, std::string("example/streaming-en"));
    KD_REQUIRE(l->progress.has_value());
    KD_CHECK_EQ(*l->progress, 42);

    kd::proto::ReadyMsg ready;
    ready.model = "example/streaming-en";
    ready.warm_ms = 2140;
    ready.mode = kd::proto::StreamingMode::StatefulChunked;
    ready.chunk_ms = 640;
    ready.context_supported = true;
    const Message rt_ready = round_trip(Message::of(ready));
    const auto* r = rt_ready.get_if<kd::proto::ReadyMsg>();
    KD_REQUIRE(r != nullptr);
    KD_CHECK_EQ(r->warm_ms, int64_t{2140});
    KD_CHECK(r->mode == kd::proto::StreamingMode::StatefulChunked);
    KD_REQUIRE(r->chunk_ms.has_value());
    KD_CHECK_EQ(*r->chunk_ms, 640);
    KD_CHECK(r->context_supported);

    kd::proto::StoppedMsg stopped;
    stopped.session_id = "s7";
    stopped.reason = "cancel";
    const Message rt_stopped = round_trip(Message::of(stopped));
    const auto* s = rt_stopped.get_if<kd::proto::StoppedMsg>();
    KD_REQUIRE(s != nullptr);
    KD_REQUIRE(s->reason.has_value());
    KD_CHECK(kd::proto::stop_reason_from_string(*s->reason) == kd::proto::StopReason::Cancel);
}

KD_TEST(protocol, partial_and_final_round_trip_including_non_ascii_text) {
    kd::proto::PartialMsg p;
    p.session_id = "s7";
    p.utterance_id = 3;
    p.seq = 12;
    p.text = "caf\xC3\xA9 \xE2\x98\x95";  // "café ☕", 9 bytes
    p.stable_bytes = 5;
    p.audio_pos_ms = 4820;
    const Message rt_p = round_trip(Message::of(p));
    const auto* got = rt_p.get_if<kd::proto::PartialMsg>();
    KD_REQUIRE(got != nullptr);
    KD_CHECK_EQ(got->text, p.text);
    KD_CHECK_EQ(got->text.size(), size_t{9});
    KD_CHECK_EQ(got->stable_bytes, uint64_t{5});
    KD_REQUIRE(got->audio_pos_ms.has_value());
    KD_CHECK_EQ(*got->audio_pos_ms, int64_t{4820});

    // §4.2: senders SHOULD emit raw UTF-8 rather than \u escapes, precisely so that offsets are
    // checkable against the wire bytes. If this ever starts escaping, `stable_bytes` stops
    // lining up with what a packet dump shows.
    const std::string wire = kd::proto::encode_control(Message::of(p));
    KD_CHECK_NE(wire.find("caf\xC3\xA9"), std::string::npos);
    KD_CHECK_EQ(wire.find("\\u"), std::string::npos);

    kd::proto::FinalMsg f;
    f.session_id = "s7";
    f.utterance_id = 3;
    f.seq = 13;
    f.text = "Can you change the llama.cpp config";
    f.audio_pos_ms = 4820;
    f.stable_bytes = f.text.size();  // redundant but permitted (§6.2)
    const Message rt_f = round_trip(Message::of(f));
    const auto* gf = rt_f.get_if<kd::proto::FinalMsg>();
    KD_REQUIRE(gf != nullptr);
    KD_CHECK_EQ(gf->text, f.text);
    KD_REQUIRE(gf->stable_bytes.has_value());
    KD_CHECK_EQ(*gf->stable_bytes, uint64_t{f.text.size()});
}

KD_TEST(protocol, error_round_trips_and_keeps_the_raw_code) {
    kd::proto::ErrorMsg e;
    e.code = ErrorCode::proto_unsupported;
    e.code_raw = "proto_unsupported";
    e.fatal = true;
    e.msg = "this build speaks proto 1 only";
    e.supported = {1};
    const Message rt_e = round_trip(Message::of(e));
    const auto* got = rt_e.get_if<kd::proto::ErrorMsg>();
    KD_REQUIRE(got != nullptr);
    KD_CHECK(got->code == ErrorCode::proto_unsupported);
    KD_CHECK(got->fatal);
    KD_REQUIRE_EQ(got->supported.size(), size_t{1});
    KD_CHECK_EQ(got->supported[0], 1);

    kd::proto::ErrorMsg nf;
    nf.code = ErrorCode::stability_violation;
    nf.code_raw = "stability_violation";
    nf.fatal = false;
    nf.msg = "stable prefix mutated at byte 4";
    nf.session_id = "s7";
    const Message rt_nf = round_trip(Message::of(nf));
    const auto* gnf = rt_nf.get_if<kd::proto::ErrorMsg>();
    KD_REQUIRE(gnf != nullptr);
    KD_CHECK(!gnf->fatal);
    KD_REQUIRE(gnf->session_id.has_value());
    KD_CHECK_EQ(*gnf->session_id, std::string("s7"));
}

KD_TEST(protocol, an_unrecognised_error_code_is_tolerated_and_preserved) {
    // §13's single point of forward tolerance: "Receivers MUST treat an unrecognised error.code
    // as a well-formed error and act on `fatal`." A fatal error about a fatal error would lose
    // the diagnostic being delivered.
    const Message m = must_decode(
        R"({"t":"error","code":"quantum_flux","fatal":true,"msg":"from a future proto"})");
    const auto* e = m.get_if<kd::proto::ErrorMsg>();
    KD_REQUIRE(e != nullptr);
    KD_CHECK(e->code == ErrorCode::unrecognised);
    KD_CHECK_EQ(e->code_raw, std::string("quantum_flux"));
    KD_CHECK(e->fatal);

    // Relaying it must not flatten the spelling to "unrecognised": the log line has to carry
    // what actually arrived.
    const std::string wire = kd::proto::encode_control(m);
    KD_CHECK_NE(wire.find("quantum_flux"), std::string::npos);
    KD_CHECK_EQ(wire.find("unrecognised"), std::string::npos);
}

// ─── §1 / §4: failures land at the lowest layer that can see them ───────────────────────────

KD_TEST(protocol, invalid_utf8_is_bad_utf8_and_never_bad_json) {
    // Fixture 008's payload. 0xE9 is 'é' in Latin-1 and an incomplete 3-byte lead in UTF-8.
    const std::string payload =
        "{\"t\":\"final\",\"session_id\":\"s7\",\"utterance_id\":1,\"seq\":2,\"text\":\"caf\xE9\"}";
    Message m;
    ProtocolError err;
    KD_REQUIRE(!kd::proto::decode_control(payload, m, err));
    KD_CHECK(err.code == ErrorCode::bad_utf8);
    KD_CHECK(err.fatal);
    // Pointing at the lead byte, not at the byte that failed to continue the sequence — that is
    // the offset a human needs when reading a hex dump.
    KD_CHECK_EQ(err.at_offset, uint64_t{payload.find('\xE9')});

    // Broken UTF-8 in a payload that is *also* broken JSON still reports bad_utf8: §1 forbids a
    // higher layer papering over a lower-layer failure.
    KD_CHECK(!kd::proto::decode_control("{\"t\":\xE9", m, err));
    KD_CHECK(err.code == ErrorCode::bad_utf8);
}

KD_TEST(protocol, malformed_json_is_bad_json_and_never_bad_message) {
    expect_error("{\"t\":\"stop\",", ErrorCode::bad_json);
    expect_error("", ErrorCode::bad_json);                        // legal frame, empty payload
    expect_error("[]", ErrorCode::bad_json);                      // top level must be an object
    expect_error("\"stop\"", ErrorCode::bad_json);                // ...not a scalar either
    expect_error("{\"t\":\"stop\",}", ErrorCode::bad_json);       // no trailing commas
    expect_error("{'t':'stop'}", ErrorCode::bad_json);            // no single quotes
    expect_error("{\"t\":\"stop\",\"session_id\":\"s7\",\"session_id\":\"s8\"}",
                 ErrorCode::bad_json);                            // §4.2: never last-wins
    expect_error("{\"t\":\"hello\",\"proto\":NaN}", ErrorCode::bad_json);
    expect_error("{\"t\":\"hello\",\"proto\":01}", ErrorCode::bad_json);   // no leading zeros
    expect_error("{\"t\":\"hello\",\"proto\":+1}", ErrorCode::bad_json);   // no leading plus
}

KD_TEST(protocol, unknown_t_is_fatal_but_an_unknown_field_is_ignored) {
    // §4.2's deliberate asymmetry. An extra field cannot change the meaning of a message a
    // receiver already understands; an unknown message type means the peer believes something is
    // happening that this peer knows nothing about.
    expect_error(R"({"t":"telepathy","session_id":"s7"})", ErrorCode::unknown_message);
    expect_error(R"({"t":"","session_id":"s7"})", ErrorCode::unknown_message);

    const Message m =
        must_decode(R"({"t":"stop","session_id":"s7","invented_in_proto_2":{"deep":[1,2]}})");
    KD_REQUIRE(m.type() == MessageType::Stop);
    KD_CHECK_EQ(m.get_if<kd::proto::StopMsg>()->session_id, std::string("s7"));
}

KD_TEST(protocol, a_message_with_no_t_is_bad_message_not_unknown_message) {
    // No `t` selects no schema at all — that is malformed, not unrecognised. Keeping them apart
    // matters because `unknown_message` tells an operator "upgrade one side" and `bad_message`
    // tells them "one side has a bug".
    expect_error(R"({"session_id":"s7"})", ErrorCode::bad_message);
    expect_error(R"({"t":7})", ErrorCode::bad_message);
    expect_error(R"({"t":null})", ErrorCode::bad_message);
}

// ─── §6 schema enforcement ──────────────────────────────────────────────────────────────────

KD_TEST(protocol, required_fields_are_required) {
    expect_error(R"({"t":"hello"})", ErrorCode::bad_message);
    expect_error(R"({"t":"start","sample_rate":16000})", ErrorCode::bad_message);
    expect_error(R"({"t":"start","session_id":"s7"})", ErrorCode::bad_message);
    expect_error(R"({"t":"stop"})", ErrorCode::bad_message);
    expect_error(R"({"t":"cancel"})", ErrorCode::bad_message);
    expect_error(R"({"t":"discontinuity","session_id":"s7"})", ErrorCode::bad_message);
    expect_error(R"({"t":"loading"})", ErrorCode::bad_message);
    expect_error(R"({"t":"ready","model":"m","warm_ms":1})", ErrorCode::bad_message);
    expect_error(R"({"t":"partial","session_id":"s7","utterance_id":1,"seq":1,"text":"x"})",
                 ErrorCode::bad_message);
    expect_error(R"({"t":"final","session_id":"s7","utterance_id":1,"seq":1,"text":"x"})",
                 ErrorCode::bad_message);
    expect_error(R"({"t":"stopped"})", ErrorCode::bad_message);
    expect_error(R"({"t":"error","code":"internal_error","fatal":true})", ErrorCode::bad_message);
}

KD_TEST(protocol, an_explicit_null_is_rejected_even_for_an_optional_field) {
    // The protocol never defines a null. Treating `{"stage":null}` as "absent" is how two
    // hand-written parsers quietly diverge on input neither author considered.
    expect_error(R"({"t":"loading","model":"m","stage":null})", ErrorCode::bad_message);
    expect_error(R"({"t":"stop","session_id":null})", ErrorCode::bad_message);
}

KD_TEST(protocol, wrong_types_and_non_integers_are_bad_message) {
    expect_error(R"({"t":"hello","proto":"1"})", ErrorCode::bad_message);
    expect_error(R"({"t":"hello","proto":1.5})", ErrorCode::bad_message);
    // 12.0 is an integer *value* written as a non-integer literal. §4.2 permits rejecting it,
    // and rejecting it is what stops a float round-trip silently renumbering a sequence.
    expect_error(R"({"t":"partial","session_id":"s7","utterance_id":1,"seq":12.0,)"
                 R"("text":"x","stable_bytes":0})",
                 ErrorCode::bad_message);
    expect_error(R"({"t":"stop","session_id":7})", ErrorCode::bad_message);
    expect_error(R"({"t":"error","code":"bad_frame","fatal":"true","msg":"x"})",
                 ErrorCode::bad_message);
    expect_error(R"({"t":"start","session_id":"s7","sample_rate":16000,"context":"llama.cpp"})",
                 ErrorCode::bad_message);
}

KD_TEST(protocol, start_pins_the_audio_format_at_the_start_of_the_session) {
    // §6.1: sample_rate is present despite being fixed so that a mismatch is caught here rather
    // than inferred later from bad transcripts.
    expect_error(R"({"t":"start","session_id":"s7","sample_rate":48000})", ErrorCode::bad_message);
    expect_error(R"({"t":"start","session_id":"s7","sample_rate":16000,"channels":2})",
                 ErrorCode::bad_message);
    expect_error(R"({"t":"start","session_id":"s7","sample_rate":16000,"format":"f32le"})",
                 ErrorCode::bad_message);
    KD_CHECK(must_decode(R"({"t":"start","session_id":"s7","sample_rate":16000,)"
                         R"("channels":1,"format":"s16le"})")
                 .type() == MessageType::Start);
}

KD_TEST(protocol, context_limits_are_enforced) {
    std::string too_many = R"({"t":"start","session_id":"s7","sample_rate":16000,"context":[)";
    for (size_t i = 0; i < kd::proto::kMaxContextItems + 1; ++i) {
        if (i != 0) too_many += ",";
        too_many += "\"x\"";
    }
    too_many += "]}";
    expect_error(too_many, ErrorCode::bad_message);

    const std::string long_item(kd::proto::kMaxContextItemBytes + 1, 'x');
    expect_error(R"({"t":"start","session_id":"s7","sample_rate":16000,"context":[")" + long_item +
                     R"("]})",
                 ErrorCode::bad_message);

    // Exactly at the limits is legal — an off-by-one here would reject a conforming peer.
    const std::string at_limit(kd::proto::kMaxContextItemBytes, 'x');
    KD_CHECK(must_decode(R"({"t":"start","session_id":"s7","sample_rate":16000,"context":[")" +
                         at_limit + R"("]})")
                 .type() == MessageType::Start);
}

KD_TEST(protocol, ready_mode_is_the_phase_0a_taxonomy_and_nothing_else) {
    // §6.2: "Exactly the taxonomy Phase 0A records, so latency numbers stay interpretable."
    // A backend that invented its own mode string would make its latency figures incomparable.
    expect_error(R"({"t":"ready","model":"m","warm_ms":1,"mode":"streaming"})",
                 ErrorCode::bad_message);
    expect_error(R"({"t":"ready","model":"m","warm_ms":1,"mode":"native_streaming"})",
                 ErrorCode::bad_message);
    for (const char* mode : {"native-streaming", "stateful-chunked", "pseudo-streaming"}) {
        const std::string js = std::string(R"({"t":"ready","model":"m","warm_ms":0,"mode":")") +
                               mode + "\"}";
        KD_CHECK(must_decode(js).type() == MessageType::Ready);
    }
    expect_error(R"({"t":"ready","model":"m","warm_ms":-1,"mode":"native-streaming"})",
                 ErrorCode::bad_message);
}

KD_TEST(protocol, proto_and_progress_ranges_are_enforced) {
    expect_error(R"({"t":"hello","proto":0})", ErrorCode::bad_message);
    expect_error(R"({"t":"hello","proto":65536})", ErrorCode::bad_message);
    expect_error(R"({"t":"hello_ack","proto":-1})", ErrorCode::bad_message);
    expect_error(R"({"t":"loading","model":"m","progress":101})", ErrorCode::bad_message);
    expect_error(R"({"t":"loading","model":"m","progress":-1})", ErrorCode::bad_message);
    KD_CHECK(must_decode(R"({"t":"loading","model":"m","progress":0})").type() ==
             MessageType::Loading);
    KD_CHECK(must_decode(R"({"t":"loading","model":"m","progress":100})").type() ==
             MessageType::Loading);
}

KD_TEST(protocol, seq_and_utterance_id_start_at_one) {
    expect_error(R"({"t":"partial","session_id":"s7","utterance_id":0,"seq":1,)"
                 R"("text":"x","stable_bytes":0})",
                 ErrorCode::bad_message);
    expect_error(R"({"t":"partial","session_id":"s7","utterance_id":1,"seq":0,)"
                 R"("text":"x","stable_bytes":0})",
                 ErrorCode::bad_message);
    expect_error(R"({"t":"partial","session_id":"s7","utterance_id":1,"seq":1,)"
                 R"("text":"x","stable_bytes":-1})",
                 ErrorCode::bad_message);
}

KD_TEST(protocol, decode_does_not_police_stable_bytes_range_or_boundaries) {
    // Deliberate, and the single most surprising decision in this codec. docs/protocol.md §8.3
    // says an out-of-range or off-cluster-boundary stable_bytes gets the *stability_violation*
    // handling: fatal:false, abort the session, keep the connection. bad_message is fatal:true
    // and tears the connection down, so rejecting it here would kill a connection the spec says
    // must survive. check_stability() is where those two conditions are detected.
    const Message m = must_decode(R"({"t":"partial","session_id":"s7","utterance_id":1,)"
                                  R"("seq":1,"text":"ab","stable_bytes":99})");
    const auto* p = m.get_if<kd::proto::PartialMsg>();
    KD_REQUIRE(p != nullptr);
    KD_CHECK_EQ(p->stable_bytes, uint64_t{99});
    KD_CHECK(kd::proto::check_stability("", 0, p->text, p->stable_bytes) ==
             kd::proto::StabilityCheck::OutOfRange);
}

KD_TEST(protocol, final_stable_bytes_when_present_must_equal_the_text_length) {
    // §6.2 states this as a constraint on a redundant field, so a `final` that contradicts its
    // own text is malformed rather than an inadmissible revision.
    expect_error(R"({"t":"final","session_id":"s7","utterance_id":1,"seq":1,)"
                 R"("text":"hello","audio_pos_ms":10,"stable_bytes":3})",
                 ErrorCode::bad_message);
    KD_CHECK(must_decode(R"({"t":"final","session_id":"s7","utterance_id":1,"seq":1,)"
                         R"("text":"hello","audio_pos_ms":10,"stable_bytes":5})")
                 .type() == MessageType::Final);
}

KD_TEST(protocol, error_requires_session_id_when_it_is_not_fatal) {
    // §11.1: fatal:false means "the named session is aborted". With no name there is nothing to
    // abort, and a receiver that guessed would abort the wrong one.
    expect_error(R"({"t":"error","code":"decode_failed","fatal":false,"msg":"boom"})",
                 ErrorCode::bad_message);
    KD_CHECK(must_decode(
                 R"({"t":"error","code":"decode_failed","fatal":false,"msg":"boom","session_id":"s7"})")
                 .type() == MessageType::Error);
    // A fatal error may precede any session, so session_id is optional there.
    KD_CHECK(must_decode(R"({"t":"error","code":"internal_error","fatal":true,"msg":"boom"})")
                 .type() == MessageType::Error);
}

KD_TEST(protocol, session_ids_are_validated_wherever_they_appear) {
    expect_error(R"({"t":"stop","session_id":""})", ErrorCode::bad_message);
    expect_error(R"({"t":"stop","session_id":"s 7"})", ErrorCode::bad_message);
    expect_error(R"({"t":"stop","session_id":"../etc"})", ErrorCode::bad_message);
    const std::string too_long(kd::proto::kMaxSessionIdBytes + 1, 'a');
    expect_error(R"({"t":"stop","session_id":")" + too_long + R"("})", ErrorCode::bad_message);
}

// ─── §7.1 identity helpers ──────────────────────────────────────────────────────────────────

KD_TEST(protocol, is_valid_session_id_matches_the_spec_grammar) {
    KD_CHECK(kd::proto::is_valid_session_id("s7"));
    KD_CHECK(kd::proto::is_valid_session_id("A-b_0"));
    KD_CHECK(kd::proto::is_valid_session_id(std::string(64, 'x')));
    KD_CHECK(!kd::proto::is_valid_session_id(""));
    KD_CHECK(!kd::proto::is_valid_session_id(std::string(65, 'x')));
    // Restricted so it is safe in log lines and file names — which is why '.', '/', ':' and
    // whitespace are all out, not merely discouraged.
    KD_CHECK(!kd::proto::is_valid_session_id("s.7"));
    KD_CHECK(!kd::proto::is_valid_session_id("s/7"));
    KD_CHECK(!kd::proto::is_valid_session_id("s\t7"));
    KD_CHECK(!kd::proto::is_valid_session_id("caf\xC3\xA9"));
    KD_CHECK(!kd::proto::is_valid_session_id(std::string("s\0 7", 4)));
}

KD_TEST(protocol, message_accessors_implement_the_staleness_rule_once) {
    kd::proto::PartialMsg p;
    p.session_id = "s7";
    const Message pm = Message::of(p);
    KD_REQUIRE(pm.session_id() != nullptr);
    KD_CHECK_EQ(*pm.session_id(), std::string("s7"));
    KD_CHECK(pm.carries_text());
    KD_CHECK(pm.consumes_seq());

    kd::proto::FinalMsg f;
    f.session_id = "s7";
    KD_CHECK(Message::of(f).carries_text());
    KD_CHECK(Message::of(f).consumes_seq());

    // §7.1's exception: stopped/error are bookkeeping, not text. §7.4: they consume no seq.
    kd::proto::StoppedMsg st;
    st.session_id = "s7";
    KD_CHECK(!Message::of(st).carries_text());
    KD_CHECK(!Message::of(st).consumes_seq());
    KD_REQUIRE(Message::of(st).session_id() != nullptr);

    // hello / hello_ack / loading / ready carry no session at all.
    KD_CHECK(Message::of(kd::proto::HelloMsg{}).session_id() == nullptr);
    KD_CHECK(Message::of(kd::proto::HelloAckMsg{}).session_id() == nullptr);
    KD_CHECK(Message::of(kd::proto::LoadingMsg{}).session_id() == nullptr);
    KD_CHECK(Message::of(kd::proto::ReadyMsg{}).session_id() == nullptr);
    KD_CHECK(!Message::of(kd::proto::ReadyMsg{}).consumes_seq());

    // A fatal error may precede any session; a non-fatal one always names one.
    kd::proto::ErrorMsg e;
    const Message fatal_err = Message::of(e);
    KD_CHECK(fatal_err.session_id() == nullptr);
    e.session_id = "s7";
    const Message named_err = Message::of(e);
    KD_REQUIRE(named_err.session_id() != nullptr);
    KD_CHECK_EQ(*named_err.session_id(), std::string("s7"));
}

// ─── §8.2 stable_bytes ──────────────────────────────────────────────────────────────────────

KD_TEST(protocol, is_valid_stable_offset_rejects_offsets_inside_a_utf8_sequence) {
    // Fixture 012's text: 63 61 66 | c3 a9 | 20 | e2 98 95.
    const std::string text = "caf\xC3\xA9 \xE2\x98\x95";
    KD_REQUIRE_EQ(text.size(), size_t{9});
    KD_CHECK(kd::proto::is_valid_stable_offset(text, 0));
    KD_CHECK(kd::proto::is_valid_stable_offset(text, 3));
    KD_CHECK(!kd::proto::is_valid_stable_offset(text, 4));
    KD_CHECK(kd::proto::is_valid_stable_offset(text, 5));
    KD_CHECK(kd::proto::is_valid_stable_offset(text, 6));
    KD_CHECK(!kd::proto::is_valid_stable_offset(text, 7));
    KD_CHECK(!kd::proto::is_valid_stable_offset(text, 8));
    KD_CHECK(kd::proto::is_valid_stable_offset(text, 9));   // end of text is legal
    KD_CHECK(!kd::proto::is_valid_stable_offset(text, 10));  // past the end is not
    KD_CHECK(!kd::proto::is_valid_stable_offset(text, uint64_t{1} << 40));
    KD_CHECK(kd::proto::is_valid_stable_offset("", 0));
}

KD_TEST(protocol, is_valid_stable_offset_rejects_offsets_inside_a_grapheme_cluster) {
    // Fixture 013's motivating case: every rejected offset here is a valid *code point*
    // boundary, which is exactly why the UTF-8 check alone is insufficient.
    const std::string e_acute = "e\xCC\x81";  // 'e' + U+0301
    KD_CHECK(kd::proto::is_valid_stable_offset(e_acute, 0));
    KD_CHECK(!kd::proto::is_valid_stable_offset(e_acute, 1));
    KD_CHECK(kd::proto::is_valid_stable_offset(e_acute, 3));

    const std::string flag = "\xF0\x9F\x87\xA9\xF0\x9F\x87\xAA";  // 🇩🇪, a regional-indicator pair
    KD_CHECK(kd::proto::is_valid_stable_offset(flag, 0));
    KD_CHECK(!kd::proto::is_valid_stable_offset(flag, 4));
    KD_CHECK(kd::proto::is_valid_stable_offset(flag, 8));
}

KD_TEST(protocol, check_stability_accepts_a_growing_prefix) {
    size_t diverge = 999;
    KD_CHECK(kd::proto::check_stability("", 0, "can you change", 8, &diverge) ==
             kd::proto::StabilityCheck::Ok);
    KD_CHECK(kd::proto::check_stability("can you change", 8, "can you change the llama", 15,
                                        &diverge) == kd::proto::StabilityCheck::Ok);
    // Equal is not "decreased": a hypothesis that revises only above the mark is the normal case.
    KD_CHECK(kd::proto::check_stability("can you change", 8, "can you CHANGE", 8) ==
             kd::proto::StabilityCheck::Ok);
}

KD_TEST(protocol, check_stability_rejects_a_decrease) {
    // Fixture 017, verbatim. Not "clamp and continue": bytes already injected cannot be
    // withdrawn, so clamping would paper over a backend bug while the document diverged.
    KD_CHECK(kd::proto::check_stability("can you change the", 8, "can you change the llama", 4) ==
             kd::proto::StabilityCheck::Decreased);
}

KD_TEST(protocol, check_stability_rejects_a_mutated_prefix_and_reports_where) {
    // Fixture 018, verbatim. stable_bytes rises 8 -> 12 while byte 4 changes 'y' -> 'w': the
    // mark did not go backwards, but text the client may already have typed was retracted.
    size_t diverge = 0;
    KD_CHECK(kd::proto::check_stability("can you change", 8, "can we change the", 12, &diverge) ==
             kd::proto::StabilityCheck::PrefixMutated);
    KD_CHECK_EQ(diverge, size_t{4});  // §8.3 step 3 requires this in the log line
    // The optional out-parameter must be optional.
    KD_CHECK(kd::proto::check_stability("can you change", 8, "can we change the", 12) ==
             kd::proto::StabilityCheck::PrefixMutated);
}

KD_TEST(protocol, check_stability_checks_range_and_boundary_before_monotonicity) {
    // Order matters: an out-of-range offset cannot be meaningfully compared against a previous
    // one, and reporting "decreased" for it would send an operator hunting the wrong bug.
    KD_CHECK(kd::proto::check_stability("abcdefgh", 8, "ab", 99) ==
             kd::proto::StabilityCheck::OutOfRange);
    KD_CHECK(kd::proto::check_stability("", 0, "caf\xC3\xA9", 4) ==
             kd::proto::StabilityCheck::NotGraphemeBoundary);
    KD_CHECK(kd::proto::check_stability("e\xCC\x81 x", 0, "e\xCC\x81 x", 1) ==
             kd::proto::StabilityCheck::NotGraphemeBoundary);
}

KD_TEST(protocol, stability_check_spellings_are_stable) {
    KD_CHECK_EQ(kd::proto::to_string(kd::proto::StabilityCheck::Ok), "ok");
    KD_CHECK_EQ(kd::proto::to_string(kd::proto::StabilityCheck::OutOfRange), "out_of_range");
    KD_CHECK_EQ(kd::proto::to_string(kd::proto::StabilityCheck::NotGraphemeBoundary),
                "not_grapheme_boundary");
    KD_CHECK_EQ(kd::proto::to_string(kd::proto::StabilityCheck::Decreased), "decreased");
    KD_CHECK_EQ(kd::proto::to_string(kd::proto::StabilityCheck::PrefixMutated), "prefix_mutated");
}

// ─── §7.4 sequence rules ────────────────────────────────────────────────────────────────────

KD_TEST(protocol, classify_seq_implements_the_table_exactly) {
    using kd::proto::SeqVerdict;
    KD_CHECK(kd::proto::classify_seq(11, 12) == SeqVerdict::Accept);
    KD_CHECK(kd::proto::classify_seq(12, 12) == SeqVerdict::DropDuplicate);
    KD_CHECK(kd::proto::classify_seq(12, 11) == SeqVerdict::DropOutOfOrder);
    KD_CHECK(kd::proto::classify_seq(12, 1) == SeqVerdict::DropOutOfOrder);
    // A gap is ACCEPTED. Discarding the newest hypothesis because two older ones went missing
    // would be worse than accepting it; the consequence is the anchor-confidence reset.
    KD_CHECK(kd::proto::classify_seq(12, 15) == SeqVerdict::AcceptAfterGap);
    KD_CHECK(kd::proto::classify_seq(0, 1) == SeqVerdict::Accept);
    KD_CHECK(kd::proto::classify_seq(0, 2) == SeqVerdict::AcceptAfterGap);
    KD_CHECK(kd::proto::classify_seq(0, 0) == SeqVerdict::DropDuplicate);
}

// ─── Wire-adjacent spellings the Python suite compares as strings ────────────────────────────

KD_TEST(protocol, message_type_spellings_round_trip) {
    const std::pair<MessageType, const char*> table[] = {
        {MessageType::Hello, "hello"},         {MessageType::Start, "start"},
        {MessageType::Stop, "stop"},           {MessageType::Cancel, "cancel"},
        {MessageType::Discontinuity, "discontinuity"},
        {MessageType::HelloAck, "hello_ack"},  {MessageType::Loading, "loading"},
        {MessageType::Ready, "ready"},         {MessageType::Partial, "partial"},
        {MessageType::Final, "final"},         {MessageType::Stopped, "stopped"},
        {MessageType::Error, "error"},
    };
    for (const auto& [t, text] : table) {
        KD_CHECK_EQ(kd::proto::to_string(t), text);
        MessageType back{};
        KD_REQUIRE(kd::proto::message_type_from_string(text, back));
        KD_CHECK(back == t);
    }
    MessageType unused{};
    KD_CHECK(!kd::proto::message_type_from_string("Hello", unused));  // case-sensitive
    KD_CHECK(!kd::proto::message_type_from_string("", unused));
}

KD_TEST(protocol, message_direction_table_matches_the_catalogue) {
    KD_CHECK(kd::proto::is_client_message(MessageType::Hello));
    KD_CHECK(kd::proto::is_client_message(MessageType::Start));
    KD_CHECK(kd::proto::is_client_message(MessageType::Discontinuity));
    KD_CHECK(!kd::proto::is_server_message(MessageType::Start));
    KD_CHECK(kd::proto::is_server_message(MessageType::Partial));
    KD_CHECK(kd::proto::is_server_message(MessageType::HelloAck));
    KD_CHECK(!kd::proto::is_client_message(MessageType::Ready));
    // §6.2: "error is bidirectional; the Client sends it too."
    KD_CHECK(kd::proto::is_client_message(MessageType::Error));
    KD_CHECK(kd::proto::is_server_message(MessageType::Error));
}

KD_TEST(protocol, error_code_spellings_match_section_11_3) {
    const std::pair<ErrorCode, const char*> table[] = {
        {ErrorCode::proto_unsupported, "proto_unsupported"},
        {ErrorCode::bad_frame, "bad_frame"},
        {ErrorCode::frame_too_large, "frame_too_large"},
        {ErrorCode::truncated_frame, "truncated_frame"},
        {ErrorCode::unknown_frame_type, "unknown_frame_type"},
        {ErrorCode::bad_utf8, "bad_utf8"},
        {ErrorCode::bad_json, "bad_json"},
        {ErrorCode::bad_message, "bad_message"},
        {ErrorCode::unknown_message, "unknown_message"},
        {ErrorCode::state_violation, "state_violation"},
        {ErrorCode::overload_audio, "overload_audio"},
        {ErrorCode::overload_control, "overload_control"},
        {ErrorCode::model_load_failed, "model_load_failed"},
        {ErrorCode::internal_error, "internal_error"},
        {ErrorCode::stability_violation, "stability_violation"},
        {ErrorCode::decode_failed, "decode_failed"},
        {ErrorCode::audio_underrun, "audio_underrun"},
        {ErrorCode::spawn_timeout, "spawn_timeout"},
        {ErrorCode::handshake_timeout, "handshake_timeout"},
        {ErrorCode::model_load_timeout, "model_load_timeout"},
        {ErrorCode::peer_mismatch, "peer_mismatch"},
        {ErrorCode::pipe_squatted, "pipe_squatted"},
    };
    for (const auto& [c, text] : table) {
        KD_CHECK_EQ(kd::proto::to_string(c), text);
        KD_CHECK(kd::proto::error_code_from_string(text) == c);
    }
    KD_CHECK(kd::proto::error_code_from_string("not_a_code") == ErrorCode::unrecognised);
    KD_CHECK(kd::proto::error_code_from_string("") == ErrorCode::unrecognised);
    // "unrecognised" is a receiver-side placeholder, not a code a peer can send. Its own
    // spelling must not resolve back to it, or a genuine future code named that would be
    // silently absorbed.
    KD_CHECK(kd::proto::error_code_from_string("unrecognised") == ErrorCode::unrecognised);
}

KD_TEST(protocol, fatal_defaults_and_the_local_only_set) {
    // §11.1: exactly two levels, no third. Exactly three codes are non-fatal.
    KD_CHECK(kd::proto::default_fatal(ErrorCode::bad_frame));
    KD_CHECK(kd::proto::default_fatal(ErrorCode::overload_audio));
    KD_CHECK(!kd::proto::default_fatal(ErrorCode::stability_violation));
    KD_CHECK(!kd::proto::default_fatal(ErrorCode::decode_failed));
    KD_CHECK(!kd::proto::default_fatal(ErrorCode::audio_underrun));

    // §11.3's final paragraph: these are never wire messages, because there is no peer to tell.
    KD_CHECK(!kd::proto::is_wire_error_code(ErrorCode::spawn_timeout));
    KD_CHECK(!kd::proto::is_wire_error_code(ErrorCode::handshake_timeout));
    KD_CHECK(!kd::proto::is_wire_error_code(ErrorCode::model_load_timeout));
    KD_CHECK(!kd::proto::is_wire_error_code(ErrorCode::peer_mismatch));
    KD_CHECK(!kd::proto::is_wire_error_code(ErrorCode::pipe_squatted));
    KD_CHECK(kd::proto::is_wire_error_code(ErrorCode::proto_unsupported));
    KD_CHECK(kd::proto::is_wire_error_code(ErrorCode::stability_violation));
}

KD_TEST(protocol, conn_state_spellings_are_uppercase_and_round_trip) {
    // §12.5's `initial.state` uses exactly these; the Python runner compares them as strings.
    const std::pair<kd::proto::ConnState, const char*> table[] = {
        {kd::proto::ConnState::Spawning, "SPAWNING"},
        {kd::proto::ConnState::Validating, "VALIDATING"},
        {kd::proto::ConnState::Handshake, "HANDSHAKE"},
        {kd::proto::ConnState::Loading, "LOADING"},
        {kd::proto::ConnState::Idle, "IDLE"},
        {kd::proto::ConnState::Running, "RUNNING"},
        {kd::proto::ConnState::Finalizing, "FINALIZING"},
        {kd::proto::ConnState::Cancelling, "CANCELLING"},
        {kd::proto::ConnState::Dead, "DEAD"},
    };
    for (const auto& [s, text] : table) {
        KD_CHECK_EQ(kd::proto::to_string(s), text);
        kd::proto::ConnState back{};
        KD_REQUIRE(kd::proto::conn_state_from_string(text, back));
        KD_CHECK(back == s);
    }
    kd::proto::ConnState unused{};
    KD_CHECK(!kd::proto::conn_state_from_string("Running", unused));
    KD_CHECK(!kd::proto::conn_state_from_string("running", unused));
}

KD_TEST(protocol, streaming_mode_and_stop_reason_spellings) {
    KD_CHECK_EQ(kd::proto::to_string(kd::proto::StreamingMode::NativeStreaming),
                "native-streaming");
    KD_CHECK_EQ(kd::proto::to_string(kd::proto::StreamingMode::StatefulChunked),
                "stateful-chunked");
    KD_CHECK_EQ(kd::proto::to_string(kd::proto::StreamingMode::PseudoStreaming),
                "pseudo-streaming");
    kd::proto::StreamingMode m{};
    KD_CHECK(!kd::proto::streaming_mode_from_string("native streaming", m));

    KD_CHECK(kd::proto::stop_reason_from_string("stop") == kd::proto::StopReason::Stop);
    KD_CHECK(kd::proto::stop_reason_from_string("cancel") == kd::proto::StopReason::Cancel);
    KD_CHECK(kd::proto::stop_reason_from_string("error") == kd::proto::StopReason::Error);
    KD_CHECK(kd::proto::stop_reason_from_string("unknown_session") ==
             kd::proto::StopReason::UnknownSession);
    // §6.2 calls `reason` diagnostics, so an unfamiliar value is absorbed rather than fatal.
    KD_CHECK(kd::proto::stop_reason_from_string("vanished") == kd::proto::StopReason::Other);
}

KD_TEST(protocol, encode_control_frame_produces_one_complete_frame) {
    kd::proto::StopMsg s;
    s.session_id = "s7";
    const std::string payload = kd::proto::encode_control(Message::of(s));
    const std::vector<uint8_t> frame = kd::proto::encode_control_frame(Message::of(s));
    KD_REQUIRE_EQ(frame.size(), size_t{4} + 1 + payload.size());
    const uint32_t length = static_cast<uint32_t>(frame[0]) |
                            (static_cast<uint32_t>(frame[1]) << 8) |
                            (static_cast<uint32_t>(frame[2]) << 16) |
                            (static_cast<uint32_t>(frame[3]) << 24);
    KD_CHECK_EQ(length, static_cast<uint32_t>(payload.size() + 1));
    KD_CHECK_EQ(int(frame[4]), 0x01);
}

KD_TEST(protocol, protocol_error_to_string_carries_what_section_8_3_asks_for) {
    ProtocolError e;
    e.code = ErrorCode::stability_violation;
    e.fatal = false;
    e.session_id = "s7";
    e.at_offset = 4;
    e.msg = "stable prefix mutated";
    const std::string s = e.to_string();
    KD_CHECK_NE(s.find("stability_violation"), std::string::npos);
    KD_CHECK_NE(s.find("s7"), std::string::npos);
    KD_CHECK_NE(s.find("4"), std::string::npos);
    KD_CHECK_NE(s.find("stable prefix mutated"), std::string::npos);
    KD_CHECK_EQ(s.find("(fatal)"), std::string::npos);
}
