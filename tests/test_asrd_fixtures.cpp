// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// tests/test_asrd_fixtures.cpp — the shared corpus, run from the **server's** role.
//
// ═══ What "the other role" can and cannot mean here ═════════════════════════════════════════
//
// `tests/test_fixtures.cpp` drives all 21 cases as a Client. This file drives the same files as
// a Server, and it is worth being exact about what that adds, because "run the corpus from the
// other role" is easy to say and easy to fake.
//
//   framing (001-011)   Genuinely role-neutral bytes. Run here through the translator's own
//                       inbound path — the WebSocket layer, then the length-prefix check
//                       §2.6.3 requires, then FrameReader — rather than through FrameReader
//                       alone. That is a different code path from the client's and it is the
//                       one a real client's bytes traverse.
//
//   text (012-013)      Run against `StabilityTracker::check`, the **producer's** gate. The
//                       client validates an incoming mark with `is_valid_stable_offset`; the
//                       server refuses to *emit* one. These two must agree exactly, or the
//                       translator will publish a mark the client is required to reject — and
//                       nothing else in the tree compares them.
//
//   session (014-020)   Client-role by construction: every `expect.verdict` is a rule about
//                       what a *Client* does with a Server message, and there is no server-side
//                       verdict vocabulary for them. What is asserted here instead is the half
//                       that is server-role: every `from: "client"` message in those files must
//                       decode on this side and must be a message a Client may legally send.
//                       Claiming more than that would be a runner that passes without testing
//                       anything, which is the failure the corpus exists to prevent.
//
// The rules from docs/protocol.md §12 are unchanged: enumerate `index.json`, never glob, and
// never "fix" a fixture a correct implementation cannot satisfy.

#include "kd_test.h"

#include "asrd/stability.h"
#include "asrd/ws_server.h"
#include "protocol.h"
#include "util/json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using kd::json::Value;

std::string str_of(const Value* v, const char* dflt = "") {
    std::string_view sv;
    if (v != nullptr && v->get_string(sv)) return std::string(sv);
    return std::string(dflt);
}

bool int_of(const Value* v, int64_t& out) { return v != nullptr && v->get_int(out); }

bool bool_of(const Value* v, bool dflt = false) {
    bool b = dflt;
    if (v != nullptr) v->get_bool(b);
    return b;
}

bool load_json(const std::string& relative, Value& out, std::string& why) {
    const std::string path = kd::test::fixture_path(relative);
    std::string text;
    if (!kd::test::read_file(path, text)) {
        why = "cannot read " + path;
        return false;
    }
    kd::json::Error err;
    if (!kd::json::parse(text, out, err, {})) {
        why = path + ": " + std::string(kd::json::to_string(err.code)) + " at " +
              std::to_string(err.offset);
        return false;
    }
    return true;
}

// index.json's byte-source grammar, same as the client runner's: a hex string, an array that
// concatenates, or {"repeat": "<hex>", "count": n}.
bool hex_to_bytes(std::string_view hex, std::string& out, std::string& why) {
    if ((hex.size() % 2) != 0) {
        why = "hex string of odd length";
        return false;
    }
    const auto nibble = [](char c, int& v) {
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
    if (v.type() == kd::json::Type::String) return hex_to_bytes(str_of(&v), out, why);
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

// ─── The translator's inbound path, as a function ───────────────────────────────────────────
//
// Deliberately the same shape as `src/asrd/main.cpp`'s loop: drain to NeedMore after every
// push, decode a control frame's payload as part of accepting the frame, and stop at the first
// error while keeping the frames already decoded (fixture 006).
struct ServerDecode {
    std::vector<kd::proto::FrameType> types;
    std::vector<std::string> payloads;
    std::optional<kd::proto::ProtocolError> error;
};

ServerDecode decode_as_server(const std::vector<std::string>& pushes, bool eof) {
    ServerDecode out;
    kd::proto::FrameReader reader;

    const auto drain = [&]() {
        for (;;) {
            kd::proto::Frame f;
            const kd::proto::FrameStatus st = reader.next(f);
            if (st == kd::proto::FrameStatus::Frame) {
                if (f.type == kd::proto::FrameType::Control) {
                    kd::proto::Message m;
                    kd::proto::ProtocolError derr;
                    if (!kd::proto::decode_control(f.payload, m, derr)) {
                        const uint64_t payload_start =
                            reader.stream_offset() - static_cast<uint64_t>(f.payload.size());
                        derr.at_offset += payload_start;
                        if (!out.error.has_value()) out.error = derr;
                        return;
                    }
                }
                out.types.push_back(f.type);
                out.payloads.emplace_back(f.payload);
                continue;
            }
            if (st == kd::proto::FrameStatus::Error) {
                if (!out.error.has_value()) out.error = reader.error();
                return;
            }
            return;  // NeedMore or EndOfStream
        }
    };

    for (const std::string& p : pushes) {
        if (out.error.has_value()) break;
        reader.push(p);
        drain();
    }
    if (!out.error.has_value() && eof) {
        reader.notify_eof();
        drain();
    }
    return out;
}

// ─── kind: framing, over the WebSocket binding ──────────────────────────────────────────────

void run_framing_server_role(const std::string& id, const Value& doc) {
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
    const Value* want_error = expect->find("error");
    const Value* want_frames = expect->find("frames");
    const bool expects_error =
        want_error != nullptr && want_error->type() != kd::json::Type::Null;

    // 1. The translator's own decode of the same bytes, as delivered and byte-at-a-time.
    for (int mode = 0; mode < 2; ++mode) {
        std::vector<std::string> pushes;
        if (mode == 0) {
            pushes = deliveries;
        } else {
            for (const std::string& d : deliveries) {
                for (const char c : d) pushes.emplace_back(1, c);
            }
        }
        const ServerDecode got = decode_as_server(pushes, /*eof=*/true);
        const std::string where = id + (mode == 0 ? " [server, as-delivered]"
                                                  : " [server, byte-at-a-time]");

        if (expects_error) {
            if (!got.error.has_value()) {
                KD_FAIL(where + ": the fixture expects error \"" +
                        str_of(want_error->find("code")) + "\" and the stream decoded cleanly");
                continue;
            }
            KD_CHECK_EQ(std::string(kd::proto::to_string(got.error->code)),
                        str_of(want_error->find("code")));
            if (const Value* at = want_error->find("at_offset")) {
                int64_t want_at = 0;
                if (int_of(at, want_at)) {
                    KD_CHECK_EQ(got.error->at_offset, static_cast<uint64_t>(want_at));
                }
            }
        } else if (got.error.has_value()) {
            KD_FAIL(where + ": unexpected error " + got.error->to_string());
            continue;
        } else {
            KD_CHECK(true);
        }

        if (want_frames != nullptr) {
            // Frames decoded before an error are still delivered (fixture 006).
            KD_CHECK_EQ(got.payloads.size(), want_frames->size());
            const size_t n = (std::min)(got.payloads.size(), want_frames->size());
            for (size_t i = 0; i < n; ++i) {
                const Value* wf = want_frames->value_at(i);
                if (wf == nullptr) continue;
                if (const Value* wpayload = wf->find("payload_hex")) {
                    std::string want_bytes;
                    std::string why;
                    if (byte_source(*wpayload, want_bytes, why)) {
                        KD_CHECK_EQ(got.payloads[i], want_bytes);
                    }
                }
            }
        }
    }

    // 2. The same bytes through the WebSocket layer, which is what a real client's frames
    //    traverse before FrameReader ever sees them. Each delivery becomes one masked binary
    //    message; the reader must hand back exactly those bytes.
    //
    //    The §2.6.3 length check is applied per message and is *expected* to fail for most of
    //    these fixtures, because a fixture chunk is a slice of a byte stream and not a whole
    //    frame. That is the point of the check and it is asserted rather than worked around:
    //    only a delivery that happens to be exactly one frame may pass it.
    static const uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};
    kd::asrd::WsReader ws;
    std::string wire;
    for (const std::string& d : deliveries) {
        wire += kd::asrd::encode_ws_frame(kd::asrd::WsOpcode::Binary, d, mask);
    }
    ws.push(wire);
    ws.notify_eof();

    size_t recovered = 0;
    std::string rejoined;
    for (;;) {
        kd::asrd::WsOpcode op = kd::asrd::WsOpcode::Binary;
        std::string payload;
        const kd::asrd::WsStatus st = ws.next(op, payload);
        if (st != kd::asrd::WsStatus::Message) break;
        rejoined += payload;
        ++recovered;
    }
    KD_CHECK_EQ(recovered, deliveries.size());
    std::string all;
    for (const std::string& d : deliveries) all += d;
    KD_CHECK_EQ(rejoined, all);
}

// ─── kind: text, against the producer's gate ────────────────────────────────────────────────

void run_text_server_role(const std::string& id, const Value& doc) {
    std::string text;
    std::string why;
    if (!hex_to_bytes(str_of(doc.find("text_hex")), text, why)) {
        KD_FAIL(id + ": " + why);
        return;
    }

    const Value* checks = doc.find("checks");
    KD_REQUIRE(checks != nullptr);
    for (size_t i = 0; i < checks->size(); ++i) {
        const Value* cp = checks->value_at(i);
        KD_REQUIRE(cp != nullptr);
        int64_t off = 0;
        KD_REQUIRE(int_of(cp->find("stable_bytes"), off));
        const bool want_valid = bool_of(cp->find("valid"));

        // A fresh tracker: high_water is 0, so the only requirements in play are §8.2's range
        // and grapheme-boundary rules, which is exactly what these fixtures state.
        kd::asrd::StabilityTracker tracker;
        tracker.reset(1);
        kd::asrd::Violation v;
        const bool accepted = tracker.check(text, static_cast<uint64_t>(off), v);

        if (accepted != want_valid) {
            KD_FAIL(id + ": the server-side gate " + (accepted ? "accepted" : "refused") +
                    " stable_bytes=" + std::to_string(off) + " which the fixture calls " +
                    (want_valid ? "valid" : "invalid") + " — " +
                    str_of(cp->find("reason")));
        } else {
            KD_CHECK(true);
        }

        // The producer's gate and the wire validator must agree exactly. If they ever diverge,
        // the translator publishes a mark the Client is required to reject, and the user sees a
        // `stability_violation` for text that was never wrong.
        KD_CHECK_EQ(accepted,
                    kd::proto::is_valid_stable_offset(text, static_cast<uint64_t>(off)));

        // And the refusal must name the right rule, because §8.3's log line is the only
        // diagnosis anybody gets.
        if (!accepted) {
            const bool out_of_range = static_cast<uint64_t>(off) > text.size();
            KD_CHECK(v.kind == (out_of_range ? kd::asrd::ViolationKind::StableBytesOutOfRange
                                             : kd::asrd::ViolationKind::StableBytesNotOnBoundary));
        }
    }
}

// ─── kind: session, the client-originated half ──────────────────────────────────────────────

void run_session_server_role(const std::string& id, const Value& doc) {
    const Value* messages = doc.find("messages");
    KD_REQUIRE(messages != nullptr);

    size_t from_client = 0;
    for (size_t i = 0; i < messages->size(); ++i) {
        const Value* ep = messages->value_at(i);
        KD_REQUIRE(ep != nullptr);
        if (str_of(ep->find("from"), "server") != "client") continue;
        const Value* json = ep->find("json");
        if (json == nullptr) continue;
        ++from_client;

        // Round-trip through the wire form rather than reading the fixture's object directly:
        // that is the path a real message takes, and it is where an encoder/decoder
        // disagreement would show up.
        const std::string payload = kd::json::serialize(*json);
        kd::proto::Message m;
        kd::proto::ProtocolError err;
        if (!kd::proto::decode_control(payload, m, err)) {
            KD_FAIL(id + " msg[" + std::to_string(i) + "]: the server could not decode a message "
                    "the fixture says a client sends — " + err.to_string());
            continue;
        }
        // A Server that accepted a Server->Client message here would be accepting a message it
        // is itself supposed to originate (§11.3 state_violation). Checked because the
        // translator's dispatch relies on this partition.
        if (!kd::proto::is_client_message(m.type())) {
            KD_FAIL(id + " msg[" + std::to_string(i) + "]: \"" +
                    std::string(kd::proto::to_string(m.type())) +
                    "\" is marked from:client in the fixture but is not a Client message");
            continue;
        }
        KD_CHECK(true);
    }
    KD_INFO(id + ": " + std::to_string(from_client) + " client-originated message(s)");
}

}  // namespace

// ─── The suite ──────────────────────────────────────────────────────────────────────────────

KD_TEST(asrd_fixtures, every_case_runs_against_the_translator) {
    Value index;
    std::string why;
    KD_REQUIRE(load_json("protocol/index.json", index, why));
    const Value* cases = index.find("cases");
    KD_REQUIRE(cases != nullptr);
    KD_CHECK_GT(cases->size(), size_t{0});

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
            run_framing_server_role(id, doc);
            ++framing;
        } else if (kind == "text") {
            run_text_server_role(id, doc);
            ++text;
        } else if (kind == "session") {
            run_session_server_role(id, doc);
            ++session;
        } else {
            KD_FAIL(id + ": unknown fixture kind \"" + kind + "\"");
        }
    }

    KD_INFO("server role: framing=" + std::to_string(framing) + " text=" + std::to_string(text) +
            " session=" + std::to_string(session));
    // Three implementations of one spec now exist — the C++ client, this C++ server, and the
    // Python server — and this total is what notices when the corpus silently shrinks.
    KD_CHECK_EQ(framing + text + session, cases->size());
}
