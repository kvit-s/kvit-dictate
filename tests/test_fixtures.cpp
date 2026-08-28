// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// tests/test_fixtures.cpp — the cross-language conformance runner.
//
// These fixtures are the only thing in the repository that both endpoints execute. The C++ client
// and the Python sidecar are written against docs/protocol.md by people who never speak, and the
// failure mode that costs the most is not "one side crashes" — it is "both sides work and they
// disagree by one byte", which surfaces in production as a corrupted document.
//
// That is not hypothetical. The C++ JSON decoder reported `bad_utf8`'s `at_offset` one byte high
// (the byte that failed to continue a sequence, rather than the byte that began it) and every
// C++ unit test agreed with it, because they were written from the same misunderstanding. Only
// fixture 008 — which the Python side already passed — disagreed.
//
// Rules this runner follows, from docs/protocol.md §12:
//   * Enumerate index.json's `cases`. NEVER glob the directory: the index is the manifest, and a
//     fixture that exists but is not listed must show up as a missing case, not run silently.
//   * A fixture that a correct implementation cannot satisfy is reported as suspect. It is never
//     "fixed" by weakening this file — that would defeat the entire purpose of a shared corpus.

#include "kd_test.h"

#include "protocol.h"
#include "session.h"
#include "util/clock.h"
#include "util/json.h"
#include "util/utf8.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using kd::json::Value;

// ─── Accessors ──────────────────────────────────────────────────────────────────────────────
//
// kd::json reports absence and type mismatch distinctly from a wrong value (that is the whole
// point of its out-parameter accessors). A fixture runner wants the terse form, so these wrap it
// once and answer a documented default rather than repeating the three-line dance per field.

std::string str_of(const Value* v, const char* dflt = "") {
    std::string_view sv;
    if (v != nullptr && v->get_string(sv)) return std::string(sv);
    return std::string(dflt);
}

bool bool_of(const Value* v, bool dflt = false) {
    bool b = dflt;
    if (v != nullptr) v->get_bool(b);
    return b;
}

bool int_of(const Value* v, int64_t& out) {
    return v != nullptr && v->get_int(out);
}

const Value* at(const Value& v, size_t i) { return v.value_at(i); }

// ─── Loading ────────────────────────────────────────────────────────────────────────────────

bool load_json(const std::string& relative, Value& out, std::string& why) {
    const std::string path = kd::test::fixture_path(relative);
    std::string text;
    if (!kd::test::read_file(path, text)) {
        why = "cannot read " + path;
        return false;
    }
    kd::json::Error err;
    // The fixtures are ours, but parsing them with the production parser (rather than something
    // lenient) means a fixture that our own parser cannot read is a finding rather than a file
    // that quietly works here and fails on the wire.
    if (!kd::json::parse(text, out, err, {})) {
        why = path + ": " + std::string(kd::json::to_string(err.code)) + " at " +
              std::to_string(err.offset);
        return false;
    }
    return true;
}

// ─── Byte sources (index.json documents the shape) ──────────────────────────────────────────
//
// "A byte source is either a lowercase hex string, or an object {"repeat":"<hex unit>",
// "count":<n>} meaning that unit repeated n times. Arrays of byte sources concatenate."
//
// The repeat form exists so the two 64 KiB cases are not committed as 128 KiB of hex.

bool hex_to_bytes(std::string_view hex, std::string& out, std::string& why) {
    if ((hex.size() % 2) != 0) {
        why = "hex string of odd length";
        return false;
    }
    auto nibble = [](char c, int& v) {
        if (c >= '0' && c <= '9') { v = c - '0'; return true; }
        if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; return true; }
        if (c >= 'A' && c <= 'F') { v = c - 'A' + 10; return true; }
        return false;
    };
    out.reserve(out.size() + hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = 0;
        int lo = 0;
        if (!nibble(hex[i], hi) || !nibble(hex[i + 1], lo)) {
            why = "non-hex character in byte source";
            return false;
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}

bool byte_source(const Value& v, std::string& out, std::string& why) {
    if (v.type() == kd::json::Type::String) {
        return hex_to_bytes(str_of(&v), out, why);
    }
    if (v.type() == kd::json::Type::Array) {
        for (size_t i = 0; i < v.size(); ++i) {
            const Value* e = v.value_at(i);
            if (e == nullptr || !byte_source(*e, out, why)) return false;
        }
        return true;
    }
    if (v.type() == kd::json::Type::Object) {
        const Value* unit = v.find("repeat");
        const Value* count = v.find("count");
        if (unit == nullptr || count == nullptr) {
            why = "byte-source object without repeat/count";
            return false;
        }
        std::string one;
        if (!hex_to_bytes(str_of(unit), one, why)) return false;
        int64_t n = 0;
        if (!int_of(count, n) || n < 0) {
            why = "repeat count is not a non-negative integer";
            return false;
        }
        out.reserve(out.size() + one.size() * static_cast<size_t>(n));
        for (int64_t i = 0; i < n; ++i) out += one;
        return true;
    }
    why = "byte source is neither string, array nor object";
    return false;
}

// ─── kind: framing ──────────────────────────────────────────────────────────────────────────

void run_framing(const std::string& id, const Value& doc) {
    const Value* input = doc.find("input");
    KD_REQUIRE(input != nullptr);
    const Value* chunks = input->find("chunks");
    KD_REQUIRE(chunks != nullptr);

    std::vector<std::string> deliveries;
    for (size_t i = 0; i < chunks->size(); ++i) {
        std::string bytes;
        std::string why;
        const Value* ch = chunks->value_at(i);
        if (ch == nullptr || !byte_source(*ch, bytes, why)) {
            KD_FAIL(id + ": " + why);
            return;
        }
        deliveries.push_back(std::move(bytes));
    }

    const Value* expect = doc.find("expect");
    KD_REQUIRE(expect != nullptr);
    const Value* want_frames = expect->find("frames");
    const Value* want_error = expect->find("error");

    // §12.3 is a SHOULD: a conforming decoder must produce the same answer however the bytes are
    // delivered. Running all three feed modes is what turns "works on my read pattern" into a
    // property. A decoder that passes as-delivered and fails one-byte-at-a-time is broken in the
    // exact way a real pipe will find in production and a test rarely does.
    enum class Feed { AsDelivered, Concatenated, ByteAtATime };
    const Feed feeds[] = {Feed::AsDelivered, Feed::Concatenated, Feed::ByteAtATime};
    const char* feed_names[] = {"as-delivered", "concatenated", "byte-at-a-time"};

    for (size_t fi = 0; fi < 3; ++fi) {
        std::vector<std::string> pushes;
        switch (feeds[fi]) {
            case Feed::AsDelivered:
                pushes = deliveries;
                break;
            case Feed::Concatenated: {
                std::string all;
                for (const std::string& d : deliveries) all += d;
                pushes.push_back(all);
                break;
            }
            case Feed::ByteAtATime:
                for (const std::string& d : deliveries) {
                    for (char c : d) pushes.push_back(std::string(1, c));
                }
                break;
        }

        kd::proto::FrameReader reader;
        std::vector<kd::proto::Frame> got;
        std::vector<std::string> got_payloads;  // Frame::payload is a view into reader's buffer
        std::optional<kd::proto::ProtocolError> got_error;

        // Drain to NeedMore after EVERY push. A caller that takes one frame per read fails
        // fixture 002, where three frames arrive in a single delivery.
        auto drain = [&]() {
            for (;;) {
                kd::proto::Frame f;
                const kd::proto::FrameStatus st = reader.next(f);
                if (st == kd::proto::FrameStatus::Frame) {
                    // A control frame is not decoded until its payload has been validated and
                    // parsed. Composing the two steps here mirrors ipc_client; checking only the
                    // framing layer would let fixture 008 (a well-formed frame carrying
                    // ill-formed UTF-8) pass as a clean decode.
                    if (f.type == kd::proto::FrameType::Control) {
                        kd::proto::Message decoded_msg;
                        kd::proto::ProtocolError derr;
                        if (!kd::proto::decode_control(f.payload, decoded_msg, derr)) {
                            // Re-base the payload-relative offset onto the stream, which is the
                            // coordinate system at_offset is defined in.
                            const uint64_t frame_end = reader.stream_offset();
                            const uint64_t payload_start =
                                frame_end - static_cast<uint64_t>(f.payload.size());
                            derr.at_offset += payload_start;
                            if (!got_error.has_value()) got_error = derr;
                            return;
                        }
                    }
                    got.push_back(f);
                    got_payloads.emplace_back(f.payload);
                    continue;
                }
                if (st == kd::proto::FrameStatus::Error) {
                    if (!got_error.has_value()) got_error = reader.error();
                    return;
                }
                return;  // NeedMore or EndOfStream
            }
        };

        for (const std::string& p : pushes) {
            if (got_error.has_value()) break;
            reader.push(p);
            drain();
        }
        if (!got_error.has_value()) {
            reader.notify_eof();
            drain();
        }

        const std::string where = id + " [" + feed_names[fi] + "]";

        if (want_error != nullptr && want_error->type() != kd::json::Type::Null) {
            if (!got_error.has_value()) {
                KD_FAIL(where + ": fixture expects error \"" + str_of(want_error->find("code")) +
                        "\" but the stream decoded cleanly with " + std::to_string(got.size()) +
                        " frame(s)");
                continue;
            }
            KD_CHECK(true);

            const Value* code = want_error->find("code");
            if (code != nullptr) {
                const std::string want_code = str_of(code);
                KD_CHECK_EQ(std::string(kd::proto::to_string(got_error->code)), want_code);
            }
            const Value* fatal = want_error->find("fatal");
            if (fatal != nullptr) KD_CHECK_EQ(got_error->fatal, bool_of(fatal, true));

            const Value* at = want_error->find("at_offset");
            if (at != nullptr) {
                int64_t want_at = 0;
                if (int_of(at, want_at)) {
                    // The offset that caught the JSON decoder being one byte off.
                    KD_CHECK_EQ(got_error->at_offset, static_cast<uint64_t>(want_at));
                }
            }
            // `detail` is deliberately NOT asserted: it is prose, and two conforming decoders
            // describe the same fault in different words ("truncated 3-byte sequence" vs
            // "invalid continuation byte"). Asserting it would make wording a wire contract.
        } else {
            KD_CHECK(!got_error.has_value());
            if (got_error.has_value()) {
                KD_FAIL(where + ": unexpected error " + got_error->to_string());
                continue;
            }
        }

        if (want_frames != nullptr) {
            KD_CHECK_EQ(got.size(), want_frames->size());
            const size_t n = (got.size() < want_frames->size()) ? got.size() : want_frames->size();
            for (size_t i = 0; i < n; ++i) {
                const Value* wfp = want_frames->value_at(i);
                if (wfp == nullptr) continue;
                const Value& wf = *wfp;
                const Value* wtype = wf.find("type");
                if (wtype != nullptr) {
                    int64_t t = 0;
                    if (int_of(wtype, t)) {
                        KD_CHECK_EQ(static_cast<int>(got[i].type), static_cast<int>(t));
                    } else {
                        const std::string s = str_of(wtype);
                        const int want = (s == "control") ? 1 : 2;
                        KD_CHECK_EQ(static_cast<int>(got[i].type), want);
                    }
                }
                const Value* wpayload = wf.find("payload_hex");
                if (wpayload != nullptr) {
                    std::string want_bytes;
                    std::string why;
                    if (byte_source(*wpayload, want_bytes, why)) {
                        KD_CHECK_EQ(got_payloads[i], want_bytes);
                    }
                }
                const Value* wlen = wf.find("payload_bytes");
                if (wlen != nullptr) {
                    int64_t len = 0;
                    if (int_of(wlen, len)) {
                        KD_CHECK_EQ(got_payloads[i].size(), static_cast<size_t>(len));
                    }
                }
                const Value* wjson = wf.find("json");
                if (wjson != nullptr) {
                    // Compare decoded control messages structurally rather than byte-wise: key
                    // order and number formatting are not wire-significant, and asserting the
                    // serialised bytes would forbid a conforming decoder from re-emitting them.
                    Value parsed;
                    kd::json::Error perr;
                    KD_CHECK(kd::json::parse(got_payloads[i], parsed, perr, {}));
                    KD_CHECK_EQ(kd::json::serialize(parsed), kd::json::serialize(*wjson));
                }
            }
        }
    }
}

// ─── kind: text ─────────────────────────────────────────────────────────────────────────────

void run_text(const std::string& id, const Value& doc) {
    const Value* hexv = doc.find("text_hex");
    KD_REQUIRE(hexv != nullptr);
    std::string text;
    std::string why;
    if (!hex_to_bytes(str_of(hexv), text, why)) {
        KD_FAIL(id + ": " + why);
        return;
    }

    // The fixture states the text twice — as UTF-8 and as hex — so a mangled file is caught here
    // rather than producing a confusing offset failure below.
    const Value* textv = doc.find("text");
    if (textv != nullptr && textv->type() == kd::json::Type::String) {
        KD_CHECK_EQ(text, str_of(textv));
    }
    const Value* nbytes = doc.find("text_bytes");
    if (nbytes != nullptr) {
        int64_t n = 0;
        if (int_of(nbytes, n)) KD_CHECK_EQ(text.size(), static_cast<size_t>(n));
    }

    const Value* checks = doc.find("checks");
    KD_REQUIRE(checks != nullptr);
    for (size_t i = 0; i < checks->size(); ++i) {
        const Value* cp = checks->value_at(i);
        KD_REQUIRE(cp != nullptr);
        int64_t off = 0;
        KD_REQUIRE(int_of(cp->find("stable_bytes"), off));
        const bool want_valid = bool_of(cp->find("valid"));

        const bool got_valid =
            kd::proto::is_valid_stable_offset(text, static_cast<uint64_t>(off));
        if (got_valid != want_valid) {
            const Value* reason = cp->find("reason");
            KD_FAIL(id + ": stable_bytes=" + std::to_string(off) + " expected " +
                    (want_valid ? "valid" : "invalid") + " but got " +
                    (got_valid ? "valid" : "invalid") +
                    (reason != nullptr ? " - " + str_of(reason) : ""));
        } else {
            KD_CHECK(true);
        }

        // The protocol layer's answer and the text layer's answer must agree, because the commit
        // engine consults the second and the wire validates with the first. If these two ever
        // diverge the client accepts a stable_bytes it would never itself have produced.
        if (static_cast<uint64_t>(off) <= text.size()) {
            KD_CHECK_EQ(got_valid,
                        kd::utf8::is_grapheme_boundary(text, static_cast<size_t>(off)));
        }
    }
}

// ─── kind: session ──────────────────────────────────────────────────────────────────────────

void run_session(const std::string& id, const Value& doc) {
    // A manual clock: nothing in a session fixture is time-driven, and a real clock would let a
    // timeout fire differently on a loaded machine than on an idle one.
    kd::ManualClock clock(0);
    kd::SessionConfig cfg;
    kd::SessionMachine machine(clock, cfg);

    const Value* initial = doc.find("initial");
    if (initial != nullptr) {
        kd::SessionSnapshot snap;
        if (const Value* v = initial->find("current_session_id")) {
            snap.current_session_id = str_of(v);
        }
        if (const Value* v = initial->find("last_seq")) {
            int64_t n = 0;
            if (int_of(v, n)) snap.last_seq = static_cast<uint64_t>(n);
        }
        if (const Value* v = initial->find("utterance_id")) {
            int64_t n = 0;
            if (int_of(v, n)) snap.utterance_id = static_cast<uint64_t>(n);
        }
        if (const Value* v = initial->find("cancelled")) snap.cancelled = bool_of(v);
        if (const Value* v = initial->find("state")) {
            const std::string s = str_of(v);
            bool known = false;
            for (int i = 0; i <= static_cast<int>(kd::proto::ConnState::Dead); ++i) {
                const auto st = static_cast<kd::proto::ConnState>(i);
                if (s == kd::proto::to_string(st)) {
                    snap.state = st;
                    known = true;
                    break;
                }
            }
            if (!known) {
                KD_FAIL(id + ": fixture names an unknown ConnState \"" + s +
                        "\" — the spellings are a wire-adjacent contract (§12.5)");
                return;
            }
        }
        machine.restore(snap);
    }

    const Value* messages = doc.find("messages");
    KD_REQUIRE(messages != nullptr);

    for (size_t i = 0; i < messages->size(); ++i) {
        const Value* ep = messages->value_at(i);
        KD_REQUIRE(ep != nullptr);
        const Value& entry = *ep;
        const std::string origin = str_of(entry.find("from"), "server");
        const Value* expect = entry.find("expect");
        const std::string want_verdict =
            (expect != nullptr) ? str_of(expect->find("verdict")) : std::string();

        // `client` and `transport` entries are not inbound wire messages, but they are not
        // decoration either: they are what puts the machine into the state the following
        // server messages are judged against. Skipping them made fixtures 019 and 020 fail for
        // the runner's reasons rather than the endpoint's — 019 expects `drop_cancelled_session`
        // only because the client already wrote `cancel`, and 020 expects `accept` for session s8
        // only because the client already started it.
        if (origin == "transport") {
            const std::string evname = str_of(entry.find("event"));
            if (evname == "broken_pipe" || evname == "disconnected") {
                const kd::SessionMachine::Outcome o =
                    machine.on_transport(kd::TransportEvent::BrokenPipe);
                if (!want_verdict.empty()) {
                    KD_CHECK_EQ(std::string(kd::to_string(o.verdict)), want_verdict);
                }
            } else if (evname == "child_respawned" || evname == "reconnected") {
                // No Outcome to check: respawning is a local decision, not an observation. §7.1
                // requires the id space to survive it, which fixture 020 then tests by sending a
                // partial for the OLD session and expecting it dropped.
                machine.restart_connection();
            } else {
                KD_FAIL(id + " msg[" + std::to_string(i) + "]: unhandled transport event \"" +
                        evname + "\"");
            }
            continue;
        }

        if (origin == "client") {
            const Value* jv = entry.find("json");
            const std::string t = (jv != nullptr) ? str_of(jv->find("t")) : std::string();
            if (t == "cancel") {
                machine.on_local(kd::LocalEvent::EscPressed);
            } else if (t == "stop" || t == "start") {
                machine.on_local(kd::LocalEvent::FinalisePressed);
            }
            if (t == "hello") {
                // The machine writes `hello` itself, but only once the pipe has connected and the
                // peer has been validated (§2.4). Fixture 020 lists the client `hello` without
                // those two steps because they are transport mechanics, not protocol messages —
                // so the runner supplies them, otherwise the server`s hello_ack arrives in
                // Spawning and is judged a protocol error for the runner`s reasons.
                machine.on_transport(kd::TransportEvent::Connected);
                machine.on_transport(kd::TransportEvent::PeerValidated);
            }
            //
            // Where the fixture names the session_id the client used, adopt it. The machine
            // generates its own ids and has no reason to pick the fixture's spelling; without
            // this the following server messages would be judged stale for a bookkeeping reason
            // that has nothing to do with the rule under test.
            if (jv != nullptr) {
                const std::string sid = str_of(jv->find("session_id"));
                if (!sid.empty()) {
                    kd::SessionSnapshot snap = machine.snapshot();
                    snap.current_session_id = sid;
                    machine.restore(snap);
                }
            }
            continue;
        }

        if (origin != "server") {
            KD_FAIL(id + " msg[" + std::to_string(i) + "]: unknown origin \"" + origin + "\"");
            continue;
        }

        const Value* jsonv = entry.find("json");
        KD_REQUIRE(jsonv != nullptr);
        const std::string wire = kd::json::serialize(*jsonv);

        kd::proto::Message msg;
        kd::proto::ProtocolError perr;
        const bool decoded = kd::proto::decode_control(wire, msg, perr);

        if (!decoded) {
            // A fixture whose message does not decode is only correct if it expected exactly
            // that. Otherwise it is a finding, and a loud one.
            if (want_verdict == "protocol_error") {
                KD_CHECK(true);
            } else {
                KD_FAIL(id + " msg[" + std::to_string(i) + "]: decode failed (" +
                        perr.to_string() + ") but fixture expected verdict \"" + want_verdict +
                        "\"");
            }
            continue;
        }

        const kd::SessionMachine::Outcome o = machine.on_message(msg);
        if (!want_verdict.empty()) {
            const std::string got = std::string(kd::to_string(o.verdict));
            if (got != want_verdict) {
                KD_FAIL(id + " msg[" + std::to_string(i) + "]: verdict \"" + got +
                        "\", fixture says \"" + want_verdict + "\"");
            } else {
                KD_CHECK(true);
            }
        }

        // §12.5 says only the machine-checkable effects are asserted. `log_gap` and
        // `reset_anchor_confidence` are observable; "the overlay flags the utterance" is not.
        if (expect != nullptr) {
            if (const Value* eff = expect->find("effects")) {
                for (size_t k = 0; k < eff->size(); ++k) {
                    const std::string name = str_of(eff->value_at(k));
                    if (name != "log_gap" && name != "reset_anchor_confidence") continue;
                    bool found = false;
                    for (const kd::Effect& got_eff : o.effects) {
                        if (std::string(kd::to_string(got_eff)) == name) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        KD_FAIL(id + " msg[" + std::to_string(i) + "]: missing effect " + name);
                    } else {
                        KD_CHECK(true);
                    }
                }
            }
        }
    }
}

}  // namespace

// ─── The suite ──────────────────────────────────────────────────────────────────────────────

KD_TEST(fixtures, index_is_the_manifest_and_every_listed_case_exists) {
    Value index;
    std::string why;
    KD_REQUIRE(load_json("protocol/index.json", index, why));

    const Value* cases = index.find("cases");
    KD_REQUIRE(cases != nullptr);
    KD_CHECK_GT(cases->size(), size_t{0});

    for (size_t i = 0; i < cases->size(); ++i) {
        const Value* cp = cases->value_at(i);
        KD_REQUIRE(cp != nullptr);
        const Value* file = cp->find("file");
        const Value* idv = cp->find("id");
        const Value* kind = cp->find("kind");
        KD_REQUIRE(file != nullptr && idv != nullptr && kind != nullptr);

        Value doc;
        std::string load_why;
        if (!load_json("protocol/" + str_of(file), doc, load_why)) {
            KD_FAIL(load_why);
            continue;
        }
        // The index and the case file each state the id and kind. They must agree, or a case can
        // be silently run as the wrong kind.
        if (const Value* did = doc.find("id")) KD_CHECK_EQ(str_of(did), str_of(idv));
        if (const Value* dkind = doc.find("kind")) KD_CHECK_EQ(str_of(dkind), str_of(kind));
    }
}

KD_TEST(fixtures, every_case_runs_against_the_cxx_endpoint) {
    Value index;
    std::string why;
    KD_REQUIRE(load_json("protocol/index.json", index, why));
    const Value* cases = index.find("cases");
    KD_REQUIRE(cases != nullptr);

    size_t framing = 0;
    size_t text = 0;
    size_t session = 0;

    for (size_t i = 0; i < cases->size(); ++i) {
        const Value* cp = cases->value_at(i);
        KD_REQUIRE(cp != nullptr);
        const std::string id = str_of(cp->find("id"));
        const std::string kind = str_of(cp->find("kind"));

        Value doc;
        std::string load_why;
        if (!load_json("protocol/" + str_of(cp->find("file")), doc, load_why)) {
            KD_FAIL(load_why);
            continue;
        }

        if (kind == "framing") {
            run_framing(id, doc);
            ++framing;
        } else if (kind == "text") {
            run_text(id, doc);
            ++text;
        } else if (kind == "session") {
            run_session(id, doc);
            ++session;
        } else {
            KD_FAIL(id + ": unknown fixture kind \"" + kind + "\"");
        }
    }

    KD_INFO("ran framing=" + std::to_string(framing) + " text=" + std::to_string(text) +
            " session=" + std::to_string(session));
    // If the corpus ever shrinks silently — a case dropped from the index during a merge — this
    // is what notices. The Python runner asserts the same totals against the same file.
    KD_CHECK_EQ(framing + text + session, cases->size());
}
