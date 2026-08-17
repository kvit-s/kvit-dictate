// Suite `json` — src/util/json.cpp.
//
// The parser reads bytes that arrived from another process, so these tests are weighted towards
// rejection rather than round-trips: every leniency this parser could have would be a place
// where it and the independently written Python parser disagree, and docs/protocol.md §4.2 lists
// the exact restrictions that keep them in step. Each rejection class named there has a test
// here, with the specific malformed input spelled out rather than described.
//
// ─── A source-level trap, avoided deliberately ──────────────────────────────────────────────
//
// A JSON escape and a C++ universal-character-name are spelled identically. Writing the four
// characters of a JSON escape directly in a C++ literal is therefore ambiguous to every reader
// and to some tools, and getting it wrong silently converts the *test input* from "two
// characters the parser must decode" into "one character it must pass through" — which is a test
// that passes while proving nothing. So every JSON escape in this file is assembled by esc()
// below, and the sequence never appears literally in the source.

#include "kd_test.h"

#include <cstdint>
#include <string>

#include "util/json.h"

using kd::json::Error;
using kd::json::ErrorCode;
using kd::json::Options;
using kd::json::Value;

namespace {

// One JSON escape, e.g. esc("00e9") -> the four characters that a JSON parser decodes to U+00E9.
std::string esc(std::string_view hex4) {
    return std::string("\\") + "u" + std::string(hex4);
}

// Parse and return only the error, for the many cases where the failure *code* is the assertion.
ErrorCode error_of(std::string_view text, const Options& opts = {}) {
    Value v;
    Error err;
    if (kd::json::parse(text, v, err, opts)) return ErrorCode::Ok;
    return err.code;
}

bool parses(std::string_view text, Value& out, const Options& opts = {}) {
    Error err;
    return kd::json::parse(text, out, err, opts);
}

std::string nest(size_t depth) { return std::string(depth, '[') + std::string(depth, ']'); }

}  // namespace

// ─── Happy path and round-trips ─────────────────────────────────────────────────────────────

KD_TEST(json, parses_a_partial_message) {
    // The `partial` example from docs/protocol.md §1.2, verbatim.
    const std::string wire =
        R"({"t":"partial","session_id":"s7","utterance_id":3,"seq":12,)"
        R"("text":"can you change the llama dot cpp config","stable_bytes":31})";

    Value v;
    Error err;
    KD_REQUIRE(kd::json::parse_object(wire, v, err, {}));
    KD_REQUIRE(v.is_object());

    const Value* t = v.find("t");
    KD_REQUIRE(t != nullptr);
    KD_CHECK_EQ(t->as_string(), std::string_view("partial"));

    KD_REQUIRE(v.find("seq") != nullptr);
    int64_t seq = 0;
    KD_CHECK(v.find("seq")->get_int(seq));
    KD_CHECK_EQ(seq, int64_t{12});

    int64_t stable = 0;
    KD_CHECK(v.find("stable_bytes")->get_int(stable));
    KD_CHECK_EQ(stable, int64_t{31});

    KD_CHECK(v.find("nonexistent") == nullptr);
}

KD_TEST(json, serialize_is_compact_and_in_insertion_order) {
    Value o = Value::object();
    o.set("z", Value::number(int64_t{1}));
    o.set("a", Value::string("two"));
    o.set("m", Value::boolean(true));

    // Insertion order, not sorted: byte-deterministic output is what makes golden fixtures
    // possible (util/json.h, "Object key order").
    KD_CHECK_EQ(kd::json::serialize(o), std::string(R"({"z":1,"a":"two","m":true})"));
    KD_CHECK_EQ(o.key_at(0), std::string("z"));
    KD_CHECK_EQ(o.key_at(2), std::string("m"));
}

KD_TEST(json, round_trips_through_parse_and_serialize) {
    const std::string wire =
        R"({"t":"start","session_id":"s7","sample_rate":16000,"context":["llama.cpp","Qwen3"]})";
    Value v;
    KD_REQUIRE(parses(wire, v));
    const std::string again = kd::json::serialize(v);
    KD_CHECK_EQ(again, wire);

    Value v2;
    KD_REQUIRE(parses(again, v2));
    KD_CHECK_EQ(kd::json::serialize(v2), wire);
}

KD_TEST(json, nested_containers_and_positional_access) {
    Value v;
    KD_REQUIRE(parses(R"({"a":[1,[2,{"b":null}]],"c":{}})", v));
    KD_CHECK_EQ(v.size(), size_t{2});

    const Value* a = v.find("a");
    KD_REQUIRE(a != nullptr);
    KD_REQUIRE(a->is_array());
    KD_CHECK_EQ(a->size(), size_t{2});
    KD_CHECK(a->at(2) == nullptr);  // out of range is nullptr, not a default-constructed value

    const Value* inner = a->at(1);
    KD_REQUIRE(inner != nullptr);
    const Value* obj = inner->at(1);
    KD_REQUIRE(obj != nullptr);
    KD_REQUIRE(obj->is_object());

    // Absence and explicit null are different (util/json.h): "b" is present *and* null.
    const Value* b = obj->find("b");
    KD_REQUIRE(b != nullptr);
    KD_CHECK(b->is_null());
    KD_CHECK(obj->find("absent") == nullptr);

    KD_CHECK_EQ(kd::json::serialize(v), std::string(R"({"a":[1,[2,{"b":null}]],"c":{}})"));
}

// ─── Rejection classes from docs/protocol.md §4.2 ───────────────────────────────────────────

KD_TEST(json, rejects_trailing_commas) {
    KD_CHECK_EQ(error_of(R"({"a":1,})"), ErrorCode::UnexpectedChar);
    KD_CHECK_EQ(error_of(R"([1,2,])"), ErrorCode::UnexpectedChar);
    KD_CHECK_EQ(error_of(R"([,])"), ErrorCode::UnexpectedChar);
}

KD_TEST(json, rejects_comments) {
    KD_CHECK_EQ(error_of(R"({"a":/*x*/1})"), ErrorCode::UnexpectedChar);
    KD_CHECK_EQ(error_of("// hi\n{}"), ErrorCode::UnexpectedChar);
}

KD_TEST(json, rejects_single_quotes_and_unquoted_keys) {
    KD_CHECK_EQ(error_of("{'a':1}"), ErrorCode::UnexpectedChar);
    KD_CHECK_EQ(error_of(R"({"a":'x'})"), ErrorCode::UnexpectedChar);
    KD_CHECK_EQ(error_of("{a:1}"), ErrorCode::UnexpectedChar);
}

KD_TEST(json, rejects_nan_and_infinity) {
    KD_CHECK_EQ(error_of(R"({"a":NaN})"), ErrorCode::BadNumber);
    KD_CHECK_EQ(error_of(R"({"a":Infinity})"), ErrorCode::BadNumber);
    KD_CHECK_EQ(error_of(R"({"a":-Infinity})"), ErrorCode::BadNumber);
}

KD_TEST(json, rejects_malformed_numbers) {
    KD_CHECK_EQ(error_of(R"({"a":+1})"), ErrorCode::BadNumber);   // leading '+'
    KD_CHECK_EQ(error_of(R"({"a":01})"), ErrorCode::BadNumber);   // leading zero
    KD_CHECK_EQ(error_of(R"({"a":-01})"), ErrorCode::BadNumber);
    KD_CHECK_EQ(error_of(R"({"a":1.})"), ErrorCode::BadNumber);   // fraction with no digits
    KD_CHECK_EQ(error_of(R"({"a":.5})"), ErrorCode::UnexpectedChar);
    KD_CHECK_EQ(error_of(R"({"a":1e})"), ErrorCode::BadNumber);   // exponent with no digits
    KD_CHECK_EQ(error_of(R"({"a":1e+})"), ErrorCode::BadNumber);
    KD_CHECK_EQ(error_of(R"({"a":0x10})"), ErrorCode::UnexpectedChar);
    // A '-' with no digits after it is BadNumber for the same reason "1e" and "1e+" above are:
    // the input has not ended (a '}' follows), it is the numeric literal that is malformed.
    KD_CHECK_EQ(error_of(R"({"a":-})"), ErrorCode::BadNumber);

    // ...and accepts the legal forms, so the strictness above is not just "rejects everything".
    Value v;
    KD_REQUIRE(parses(R"({"a":0,"b":-0,"c":1.5,"d":1e2,"e":1E+2,"f":-2.5e-3})", v));
    KD_CHECK_EQ(v.size(), size_t{6});
}

KD_TEST(json, rejects_duplicate_keys) {
    // Never last-wins: last-wins is how two implementations disagree about which value was meant.
    Value v;
    Error err;
    KD_REQUIRE(!kd::json::parse(R"({"seq":1,"seq":2})", v, err, {}));
    KD_CHECK_EQ(err.code, ErrorCode::DuplicateKey);

    // Nested objects get the same treatment, and a repeated key in a *different* object does not.
    KD_CHECK_EQ(error_of(R"({"a":{"x":1,"x":2}})"), ErrorCode::DuplicateKey);
    KD_CHECK_EQ(error_of(R"({"a":{"x":1},"b":{"x":2}})"), ErrorCode::Ok);
}

KD_TEST(json, duplicate_keys_may_be_allowed_only_by_explicit_option) {
    Options opts;
    opts.reject_duplicate_keys = false;  // never set by the protocol codec; see util/json.h
    Value v;
    KD_REQUIRE(parses(R"({"seq":1,"seq":2})", v, opts));
    KD_CHECK_EQ(v.size(), size_t{1});
    KD_CHECK_EQ(v.find("seq")->as_int(), int64_t{2});
}

KD_TEST(json, rejects_trailing_data_and_empty_input) {
    KD_CHECK_EQ(error_of(R"({"a":1} {"b":2})"), ErrorCode::TrailingData);
    KD_CHECK_EQ(error_of(R"({} )"), ErrorCode::Ok);  // trailing whitespace is fine
    KD_CHECK_EQ(error_of(""), ErrorCode::UnexpectedEnd);
    KD_CHECK_EQ(error_of("   \t\r\n "), ErrorCode::UnexpectedEnd);
}

KD_TEST(json, rejects_unterminated_structures) {
    KD_CHECK_EQ(error_of(R"({"a":1)"), ErrorCode::UnexpectedEnd);
    KD_CHECK_EQ(error_of(R"([1,2)"), ErrorCode::UnexpectedEnd);
    KD_CHECK_EQ(error_of(R"({"a")"), ErrorCode::UnexpectedEnd);
    KD_CHECK_EQ(error_of(R"("abc)"), ErrorCode::BadString);
    KD_CHECK_EQ(error_of("tru"), ErrorCode::UnexpectedChar);
}

KD_TEST(json, parse_object_rejects_a_non_object_top_level) {
    // docs/protocol.md §4.2: "Top level is an object. Not an array, not a scalar."
    Value v;
    Error err;
    KD_CHECK(!kd::json::parse_object("[1,2]", v, err, {}));
    KD_CHECK_EQ(err.code, ErrorCode::NotAnObject);

    KD_CHECK(!kd::json::parse_object("42", v, err, {}));
    KD_CHECK_EQ(err.code, ErrorCode::NotAnObject);

    KD_CHECK(!kd::json::parse_object(R"("hello")", v, err, {}));
    KD_CHECK_EQ(err.code, ErrorCode::NotAnObject);

    // ...while plain parse() is happy with all three, because models.json and future files share
    // this parser (util/json.h).
    KD_CHECK_EQ(error_of("[1,2]"), ErrorCode::Ok);
}

// ─── Strings, escapes, and Unicode ──────────────────────────────────────────────────────────

KD_TEST(json, escapes_are_decoded_before_any_offset_applies) {
    // docs/protocol.md §4.2's worked example: "café" is 8 bytes escaped on the wire and 5 bytes
    // decoded, and stable_bytes means the 5-byte space.
    const std::string wire = "{\"text\":\"caf" + esc("00e9") + "\"}";
    KD_CHECK_EQ(wire.size(), size_t{20});  // 9 of which are the escaped value "caf" + one escape

    Value v;
    KD_REQUIRE(parses(wire, v));
    const std::string_view text = v.find("text")->as_string();
    KD_CHECK_EQ(text.size(), size_t{5});
    KD_CHECK_EQ(std::string(text), std::string("caf\xC3\xA9"));

    // The raw-UTF-8 spelling of the same string decodes identically, so a sender's choice of
    // escaping can never move an offset.
    Value raw;
    KD_REQUIRE(parses("{\"text\":\"caf\xC3\xA9\"}", raw));
    KD_CHECK_EQ(std::string(raw.find("text")->as_string()), std::string(text));
}

KD_TEST(json, decodes_bmp_escapes_and_a_surrogate_pair) {
    Value v;
    const std::string wire =
        "{\"a\":\"A" + esc("00e9") + esc("20ac") + esc("d83d") + esc("de00") + "\"}";
    KD_REQUIRE(parses(wire, v));
    const std::string_view s = v.find("a")->as_string();

    // A(1) + U+00E9(2) + U+20AC(3) + U+1F600 from the surrogate pair(4) = 10 bytes
    KD_CHECK_EQ(s.size(), size_t{10});
    KD_CHECK_EQ(std::string(s), std::string("A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"));

    // Upper-case hex digits are equally legal.
    Value u;
    KD_REQUIRE(parses("{\"a\":\"" + esc("D83D") + esc("DE00") + "\"}", u));
    KD_CHECK_EQ(u.find("a")->as_string().size(), size_t{4});
}

KD_TEST(json, rejects_lone_surrogates) {
    // A lone surrogate has no UTF-8 encoding at all; encoding one "as best we can" is exactly the
    // silent corruption docs/protocol.md §4.1 forbids.
    const auto in_string = [](const std::string& body) { return "{\"a\":\"" + body + "\"}"; };

    KD_CHECK_EQ(error_of(in_string(esc("d83d"))), ErrorCode::LoneSurrogate);  // high, alone
    KD_CHECK_EQ(error_of(in_string(esc("de00"))), ErrorCode::LoneSurrogate);  // low, alone
    KD_CHECK_EQ(error_of(in_string(esc("d83d") + esc("0041"))),
                ErrorCode::LoneSurrogate);  // high + a BMP escape
    KD_CHECK_EQ(error_of(in_string(esc("d83d") + "x")),
                ErrorCode::LoneSurrogate);  // high + a raw character
    KD_CHECK_EQ(error_of(in_string(esc("d83d") + esc("d83d"))),
                ErrorCode::LoneSurrogate);  // high + high
    KD_CHECK_EQ(error_of(in_string(esc("de00") + esc("d83d"))),
                ErrorCode::LoneSurrogate);  // the pair, backwards

    // The well-formed pair is of course accepted; these tests would pass vacuously otherwise.
    KD_CHECK_EQ(error_of(in_string(esc("d83d") + esc("de00"))), ErrorCode::Ok);
}

KD_TEST(json, rejects_bad_escapes_and_raw_control_characters) {
    KD_CHECK_EQ(error_of(R"({"a":"\q"})"), ErrorCode::BadString);
    KD_CHECK_EQ(error_of(R"({"a":"\x41"})"), ErrorCode::BadString);
    KD_CHECK_EQ(error_of("{\"a\":\"" + esc("12") + "\"}"), ErrorCode::BadString);   // short
    KD_CHECK_EQ(error_of("{\"a\":\"" + esc("12g4") + "\"}"), ErrorCode::BadString);  // not hex

    // RFC 8259 §7: U+0000–U+001F must be escaped inside a string, raw is a fault.
    KD_CHECK_EQ(error_of(std::string("{\"a\":\"x\ny\"}")), ErrorCode::BadString);
    KD_CHECK_EQ(error_of(std::string("{\"a\":\"x\ty\"}")), ErrorCode::BadString);
    KD_CHECK_EQ(error_of(std::string("{\"a\":\"x\x01y\"}")), ErrorCode::BadString);

    // ...but the escaped forms are accepted and decode to the real bytes.
    Value v;
    KD_REQUIRE(parses("{\"a\":\"x" + std::string("\\n") + "y" + std::string("\\t") + "z" +
                          esc("0001") + "\"}",
                      v));
    KD_CHECK_EQ(std::string(v.find("a")->as_string()), std::string("x\ny\tz\x01"));
}

KD_TEST(json, rejects_invalid_utf8_distinctly_from_a_syntax_error) {
    // docs/protocol.md §1: fail at the lowest layer that can see the fault. `bad_utf8` and
    // `bad_json` are different wire codes, so these must not collapse into ErrorCode::BadString.
    KD_CHECK_EQ(error_of(std::string("{\"a\":\"\xE9\"}")), ErrorCode::InvalidUtf8);  // lone 0xE9
    KD_CHECK_EQ(error_of(std::string("{\"a\":\"\xC0\xAF\"}")),
                ErrorCode::InvalidUtf8);  // overlong '/'
    KD_CHECK_EQ(error_of(std::string("{\"a\":\"\xED\xA0\x80\"}")),
                ErrorCode::InvalidUtf8);  // an encoded surrogate, U+D800
    KD_CHECK_EQ(error_of(std::string("{\"a\":\"\xF5\x80\x80\x80\"}")),
                ErrorCode::InvalidUtf8);  // above U+10FFFF
    KD_CHECK_EQ(error_of(std::string("{\"a\":\"\xE2\x82\"}")),
                ErrorCode::InvalidUtf8);  // truncated 3-byte sequence

    // The offset points at the START of the malformed sequence — the 0xE9 lead at 6, not the '"'
    // at 7 that failed to continue it. This is the convention docs/protocol.md fixes for
    // `bad_utf8`'s at_offset (fixture 008 pins the same case at stream offset 72, the lead), and
    // it is deliberately NOT utf8::validate's first_bad_offset, which is the precise byte.
    Value v;
    Error err;
    KD_REQUIRE(!kd::json::parse(std::string("{\"a\":\"\xE9\"}"), v, err, {}));
    KD_CHECK_EQ(err.offset, size_t{6});

    // Invalid UTF-8 anywhere in the payload is caught, including where a syntax error comes
    // first, because validation runs over the whole input before parsing starts.
    KD_CHECK_EQ(error_of(std::string("{ oops \xE9 }")), ErrorCode::InvalidUtf8);
}

// ─── Numbers: the int64 contract ────────────────────────────────────────────────────────────

KD_TEST(json, int64_is_exact_beyond_the_double_mantissa) {
    // 2^53 + 1 is the smallest positive integer a double cannot represent. A parser that stored
    // only a double would silently renumber this to 2^53, which for `seq` or `utterance_id` is a
    // correctness bug rather than a rounding nuisance (util/json.h, "Numbers").
    Value v;
    KD_REQUIRE(parses(R"({"seq":9007199254740993})", v));
    const Value* seq = v.find("seq");
    KD_REQUIRE(seq != nullptr);
    KD_CHECK(seq->is_integer());
    KD_CHECK_EQ(seq->as_int(), int64_t{9007199254740993});

    // The lossy path is right there for contrast: through the double it is 2^53.
    KD_CHECK_EQ(static_cast<int64_t>(seq->as_double()), int64_t{9007199254740992});

    // ...and it survives serialization, so a value read from the wire and echoed back is intact.
    KD_CHECK_EQ(kd::json::serialize(v), std::string(R"({"seq":9007199254740993})"));

    // int64 extremes. INT64_MIN is written as a negation because -9223372036854775808 is parsed
    // by C++ as a negated positive literal that does not fit; the JSON side has no such problem.
    Value ext;
    KD_REQUIRE(parses(R"({"lo":-9223372036854775808,"hi":9223372036854775807})", ext));
    KD_CHECK_EQ(ext.find("lo")->as_int(), int64_t{-9223372036854775807LL - 1});
    KD_CHECK_EQ(ext.find("hi")->as_int(), int64_t{9223372036854775807LL});
}

KD_TEST(json, get_int_fails_rather_than_truncating) {
    Value v;
    KD_REQUIRE(parses(R"({"a":12.0,"b":1e2,"c":1e400,"d":99999999999999999999,"e":"12"})", v));

    int64_t out = -1;
    // 12.0 is written with a fraction: an integer field must reject it as bad_message rather than
    // quietly accept 12.
    KD_CHECK(!v.find("a")->get_int(out));
    KD_CHECK(!v.find("a")->is_integer());
    KD_CHECK_EQ(out, int64_t{-1});  // `out` is left untouched on failure

    KD_CHECK(!v.find("b")->get_int(out));  // 1e2 has an exponent
    KD_CHECK(!v.find("c")->get_int(out));  // 1e400 is out of range for a double, let alone int64
    KD_CHECK(!v.find("d")->get_int(out));  // integral spelling, outside int64
    KD_CHECK(!v.find("e")->get_int(out));  // a string is not a number

    // The double side still works where it makes sense.
    double d = 0.0;
    KD_CHECK(v.find("a")->get_double(d));
    KD_CHECK_NEAR(d, 12.0, 1e-12);
    KD_CHECK(v.find("d")->get_double(d));
    KD_CHECK(d > 1e19);
}

KD_TEST(json, accessors_separate_absence_from_type_mismatch) {
    Value v;
    KD_REQUIRE(parses(R"({"n":7,"s":"x","b":true,"z":null})", v));

    // Absence: find() returns nullptr, which a decoder reports as "missing required field".
    KD_CHECK(v.find("missing") == nullptr);

    // Type mismatch: present, but the typed getter fails and leaves `out` alone.
    std::string_view sv = "untouched";
    KD_CHECK(!v.find("n")->get_string(sv));
    KD_CHECK_EQ(sv, std::string_view("untouched"));

    bool b = false;
    KD_CHECK(!v.find("s")->get_bool(b));
    KD_CHECK(v.find("b")->get_bool(b));
    KD_CHECK(b);

    // An explicit null is present, and is not a value this protocol ever defines.
    KD_REQUIRE(v.find("z") != nullptr);
    KD_CHECK(v.find("z")->is_null());
    int64_t n = 0;
    KD_CHECK(!v.find("z")->get_int(n));

    // Unchecked forms fall back to the supplied default rather than throwing.
    KD_CHECK_EQ(v.find("s")->as_int(-5), int64_t{-5});
    KD_CHECK_EQ(v.find("n")->as_string("fallback"), std::string_view("fallback"));
}

// ─── Hard limits ────────────────────────────────────────────────────────────────────────────

KD_TEST(json, depth_limit_is_enforced_during_the_parse) {
    Options defaults;
    KD_CHECK_EQ(defaults.max_depth, size_t{32});  // the documented default

    KD_CHECK_EQ(error_of(nest(32)), ErrorCode::Ok);
    KD_CHECK_EQ(error_of(nest(33)), ErrorCode::DepthExceeded);

    // Objects count the same way, and the two mix. Depth counts containers, not scalars, exactly
    // as util/json.h describes it ("object -> array -> string, i.e. depth 2").
    Options shallow;
    shallow.max_depth = 2;
    KD_CHECK_EQ(error_of(R"({"a":[1]})", shallow), ErrorCode::Ok);
    KD_CHECK_EQ(error_of(R"({"a":[{"b":1}]})", shallow), ErrorCode::DepthExceeded);
}

KD_TEST(json, hostile_nesting_cannot_exhaust_the_stack) {
    // A caller may not raise its own stack-overflow risk: json.cpp caps recursion regardless of
    // Options::max_depth. The assertion that matters most is that this test *returns* at all.
    Options opts;
    opts.max_depth = 1000000;
    opts.max_bytes = 1u << 20;
    KD_CHECK_EQ(error_of(nest(5000), opts), ErrorCode::DepthExceeded);
    KD_CHECK_EQ(error_of(std::string(200000, '['), opts), ErrorCode::DepthExceeded);
}

KD_TEST(json, size_limit_is_checked_before_anything_is_parsed) {
    Options tiny;
    tiny.max_bytes = 8;
    Value v;
    Error err;
    KD_CHECK(!kd::json::parse(R"({"a":1234})", v, err, tiny));
    KD_CHECK_EQ(err.code, ErrorCode::SizeExceeded);
    KD_CHECK_EQ(err.offset, size_t{8});

    // Not valid JSON at all, and 200 KiB of it: SizeExceeded rather than UnexpectedChar is what
    // proves the limit ran before the parser looked at byte 0.
    KD_CHECK_EQ(error_of(std::string(200000, 'x')), ErrorCode::SizeExceeded);

    // The default is MAX_PAYLOAD from docs/protocol.md §1.1.
    Options def;
    KD_CHECK_EQ(def.max_bytes, size_t{65536});
    const std::string filler(65536 - 8, 'a');  // 8 = the surrounding {"a":"…"}
    const std::string at_limit = "{\"a\":\"" + filler + "\"}";
    KD_REQUIRE_EQ(at_limit.size(), size_t{65536});
    KD_CHECK_EQ(error_of(at_limit), ErrorCode::Ok);
    KD_CHECK_EQ(error_of("{\"a\":\"" + filler + "b\"}"), ErrorCode::SizeExceeded);
}

// ─── Serialization ──────────────────────────────────────────────────────────────────────────

KD_TEST(json, serialize_escapes_exactly_what_rfc8259_requires) {
    Value o = Value::object();
    o.set("a", Value::string("q:\" b:\\ s:/ n:\n t:\t r:\r b:\b f:\f"));
    o.set("c", Value::string(std::string("ctl:\x01\x1F")));

    const std::string out = kd::json::serialize(o);
    const std::string expected =
        std::string(R"({"a":"q:\" b:\\ s:/ n:\n t:\t r:\r b:\b f:\f",)") + "\"c\":\"ctl:" +
        esc("0001") + esc("001f") + "\"}";
    KD_CHECK_EQ(out, expected);

    // '/' is deliberately NOT escaped: \/ is a legacy HTML-embedding habit that costs the peer an
    // un-escape for nothing.
    KD_CHECK(out.find("\\/") == std::string::npos);

    // ...and it round-trips back to the same bytes.
    Value back;
    KD_REQUIRE(parses(out, back));
    KD_CHECK_EQ(std::string(back.find("c")->as_string()), std::string("ctl:\x01\x1F"));
    KD_CHECK_EQ(std::string(back.find("a")->as_string()),
                std::string("q:\" b:\\ s:/ n:\n t:\t r:\r b:\b f:\f"));
}

KD_TEST(json, serialize_emits_raw_utf8_not_escapes) {
    // docs/protocol.md §4.2 asks for this specifically: raw UTF-8 makes stable_bytes offsets
    // directly checkable against the wire bytes.
    const std::string text = "caf\xC3\xA9 \xE2\x98\x95 \xF0\x9F\x98\x80";  // "café ☕ 😀"
    Value o = Value::object();
    o.set("text", Value::string(text));
    const std::string out = kd::json::serialize(o);

    KD_CHECK(out.find("\\u") == std::string::npos);
    KD_CHECK_EQ(out, "{\"text\":\"" + text + "\"}");

    Value back;
    KD_REQUIRE(parses(out, back));
    KD_CHECK_EQ(std::string(back.find("text")->as_string()), text);
}

KD_TEST(json, round_trips_an_embedded_nul) {
    // An escaped U+0000 is legal JSON and produces a NUL *inside* the decoded string. A
    // serializer that treated the value as a C string would truncate here, and the far end would
    // see a shorter text than stable_bytes describes.
    Value v;
    KD_REQUIRE(parses("{\"k\":\"a" + esc("0000") + "b\"}", v));
    const std::string_view s = v.find("k")->as_string();
    KD_REQUIRE_EQ(s.size(), size_t{3});
    KD_CHECK_EQ(s[1], '\0');
    KD_CHECK_EQ(kd::json::serialize(v), "{\"k\":\"a" + esc("0000") + "b\"}");
}

KD_TEST(json, serialize_pretty_is_indented_and_reparses) {
    Value v;
    KD_REQUIRE(parses(R"({"a":[1,2],"b":{},"c":[]})", v));
    const std::string pretty = kd::json::serialize_pretty(v, 2);

    KD_CHECK(pretty.find('\n') != std::string::npos);
    KD_CHECK(pretty.find("[]") != std::string::npos);  // empty containers stay on one line
    KD_CHECK(pretty.find("{}") != std::string::npos);

    Value back;
    KD_REQUIRE(parses(pretty, back));
    KD_CHECK_EQ(kd::json::serialize(back), kd::json::serialize(v));
}

KD_TEST(json, values_copy_deeply_and_null_costs_nothing) {
    Value v;
    KD_REQUIRE(parses(R"({"a":[1,{"b":"x"}]})", v));
    Value copy = v;  // deep copy; mutating one must not touch the other
    copy.set("a", Value::number(int64_t{9}));

    KD_CHECK_EQ(kd::json::serialize(v), std::string(R"({"a":[1,{"b":"x"}]})"));
    KD_CHECK_EQ(kd::json::serialize(copy), std::string(R"({"a":9})"));

    Value empty;
    KD_CHECK(empty.is_null());
    KD_CHECK_EQ(empty.size(), size_t{0});
    KD_CHECK(empty.at(0) == nullptr);
    KD_CHECK(empty.find("x") == nullptr);
    KD_CHECK(!empty.set("x", Value::null()));  // not an object: refused, not silently promoted
    KD_CHECK_EQ(kd::json::serialize(empty), std::string("null"));

    // push_back on a non-array is likewise a no-op rather than a promotion.
    Value not_an_array = Value::string("s");
    not_an_array.push_back(Value::number(int64_t{1}));
    KD_CHECK_EQ(not_an_array.size(), size_t{0});
    KD_CHECK(not_an_array.is_string());
}

KD_TEST(json, error_codes_have_stable_spellings) {
    // These strings appear in log lines that a human reads next to docs/protocol.md.
    KD_CHECK_EQ(kd::json::to_string(ErrorCode::Ok), "ok");
    KD_CHECK_EQ(kd::json::to_string(ErrorCode::InvalidUtf8), "invalid_utf8");
    KD_CHECK_EQ(kd::json::to_string(ErrorCode::DuplicateKey), "duplicate_key");
    KD_CHECK_EQ(kd::json::to_string(ErrorCode::LoneSurrogate), "lone_surrogate");
    KD_CHECK_EQ(kd::json::to_string(ErrorCode::DepthExceeded), "depth_exceeded");
    KD_CHECK_EQ(kd::json::to_string(ErrorCode::SizeExceeded), "size_exceeded");
    KD_CHECK_EQ(kd::json::to_string(ErrorCode::NotAnObject), "not_an_object");
}
