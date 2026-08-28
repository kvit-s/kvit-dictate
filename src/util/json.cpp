// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// A strict, minimal JSON DOM for the control protocol.
//
// Specification: util/json.h, and docs/protocol.md §4.2 for the subset that is normative on the
// wire. This file adds nothing to that subset. Every leniency a general-purpose parser would
// offer — trailing commas, comments, single quotes, unquoted keys, NaN/Infinity, last-wins
// duplicate keys — is a place where this parser and the independently written Python one could
// disagree, and a disagreement between them surfaces as garbled text in the user's document
// rather than as a failing test.
//
// ─── Hostile input ──────────────────────────────────────────────────────────────────────────
//
// The bytes reaching parse() came off a pipe from another process. Three specific attacks are
// designed against here:
//
//   * **Stack exhaustion.** `[[[[…` recurses. parse_value() is recursive because the recursive
//     form is a third of the size of an explicit-stack one and is far easier to check by eye —
//     so the depth is capped *hard*, at kHardMaxDepth, regardless of what Options::max_depth
//     says. A caller cannot raise its own stack-overflow risk by passing a large max_depth.
//   * **Unbounded allocation.** The whole input is length-checked before a single byte is
//     examined, so a 4 GiB payload costs one comparison, not one allocation.
//   * **Silent corruption.** Invalid UTF-8 is reported as its own error code, distinct from a
//     syntax error, because docs/protocol.md §1 requires a receiver to fail at the lowest layer
//     that can see the fault and `bad_utf8` and `bad_json` are different wire codes. Nothing
//     here ever substitutes U+FFFD (docs/protocol.md §4.1).

#include "util/json.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include "util/utf8.h"

namespace kd::json {

// ═══ Value ══════════════════════════════════════════════════════════════════════════════════

// A Null Value holds no Impl at all. That is not a micro-optimisation: Value's default
// constructor is declared `noexcept` in the header, and an allocating constructor cannot honour
// that (a throwing bad_alloc from a noexcept function calls std::terminate). Null is therefore
// represented by a null impl_, and every accessor tolerates it.
struct Value::Impl {
    Type type = Type::Null;
    bool boolean = false;

    // Numbers carry both forms. `integral` is true only when the *literal* had no fraction and
    // no exponent AND the value fits in int64 — see the note on as_int() in the header: a
    // sequence number that round-tripped through a double is a correctness bug, not a rounding
    // nuisance.
    double dbl = 0.0;
    int64_t i64 = 0;
    bool integral = false;

    std::string str;
    std::vector<Value> elements;
    std::vector<std::pair<std::string, Value>> members;  // insertion order; see the header
};

Value::Value() noexcept = default;
Value::~Value() = default;
Value::Value(Value&&) noexcept = default;
Value& Value::operator=(Value&&) noexcept = default;

Value::Value(const Value& other)
    : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

Value& Value::operator=(const Value& other) {
    if (this != &other) {
        impl_ = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
    }
    return *this;
}

// ── Construction ────────────────────────────────────────────────────────────────────────────

Value Value::null() { return Value{}; }

Value Value::boolean(bool b) {
    Value v;
    v.impl_ = std::make_unique<Impl>();
    v.impl_->type = Type::Bool;
    v.impl_->boolean = b;
    return v;
}

Value Value::number(int64_t n) {
    Value v;
    v.impl_ = std::make_unique<Impl>();
    v.impl_->type = Type::Number;
    v.impl_->i64 = n;
    v.impl_->dbl = static_cast<double>(n);
    v.impl_->integral = true;
    return v;
}

Value Value::number(double d) {
    Value v;
    v.impl_ = std::make_unique<Impl>();
    v.impl_->type = Type::Number;
    v.impl_->dbl = d;
    v.impl_->integral = false;  // documented in the header: a double records itself as non-integral
    return v;
}

Value Value::string(std::string s) {
    Value v;
    v.impl_ = std::make_unique<Impl>();
    v.impl_->type = Type::String;
    v.impl_->str = std::move(s);
    return v;
}

Value Value::array() {
    Value v;
    v.impl_ = std::make_unique<Impl>();
    v.impl_->type = Type::Array;
    return v;
}

Value Value::object() {
    Value v;
    v.impl_ = std::make_unique<Impl>();
    v.impl_->type = Type::Object;
    return v;
}

// ── Interrogation ───────────────────────────────────────────────────────────────────────────

Type Value::type() const noexcept { return impl_ ? impl_->type : Type::Null; }

bool Value::is_integer() const noexcept {
    return impl_ && impl_->type == Type::Number && impl_->integral;
}

bool Value::get_bool(bool& out) const noexcept {
    if (!is_bool()) return false;
    out = impl_->boolean;
    return true;
}

bool Value::get_int(int64_t& out) const noexcept {
    if (!is_integer()) return false;
    out = impl_->i64;
    return true;
}

bool Value::get_double(double& out) const noexcept {
    if (!is_number()) return false;
    out = impl_->dbl;
    return true;
}

bool Value::get_string(std::string_view& out) const noexcept {
    if (!is_string()) return false;
    out = impl_->str;
    return true;
}

bool Value::as_bool(bool def) const noexcept {
    bool v = def;
    get_bool(v);
    return v;
}

int64_t Value::as_int(int64_t def) const noexcept {
    int64_t v = def;
    get_int(v);
    return v;
}

double Value::as_double(double def) const noexcept {
    double v = def;
    get_double(v);
    return v;
}

std::string_view Value::as_string(std::string_view def) const noexcept {
    std::string_view v = def;
    get_string(v);
    return v;
}

// ── Arrays and objects ──────────────────────────────────────────────────────────────────────

size_t Value::size() const noexcept {
    if (!impl_) return 0;
    if (impl_->type == Type::Array) return impl_->elements.size();
    if (impl_->type == Type::Object) return impl_->members.size();
    return 0;
}

const Value* Value::at(size_t index) const noexcept {
    if (!impl_) return nullptr;
    if (impl_->type == Type::Array) {
        return index < impl_->elements.size() ? &impl_->elements[index] : nullptr;
    }
    // Indexing an object positionally yields the member *value*, which is what makes
    // at()/key_at() a usable pair for iterating a manifest.
    if (impl_->type == Type::Object) {
        return index < impl_->members.size() ? &impl_->members[index].second : nullptr;
    }
    return nullptr;
}

void Value::push_back(Value v) {
    if (impl_ && impl_->type == Type::Array) impl_->elements.push_back(std::move(v));
}

const Value* Value::find(std::string_view key) const noexcept {
    if (!impl_ || impl_->type != Type::Object) return nullptr;
    for (const auto& m : impl_->members) {
        if (m.first == key) return &m.second;
    }
    return nullptr;
}

bool Value::set(std::string_view key, Value v) {
    if (!impl_ || impl_->type != Type::Object) return false;
    for (auto& m : impl_->members) {
        if (m.first == key) {
            m.second = std::move(v);
            return true;
        }
    }
    impl_->members.emplace_back(std::string(key), std::move(v));
    return true;
}

const std::string& Value::key_at(size_t index) const noexcept {
    // A reference has to refer to something even when the index is out of range, and this
    // function is noexcept so it cannot report the mistake. The empty string is the least
    // surprising answer and never collides with a real key, because "" is a legal JSON key but
    // one this protocol never uses.
    static const std::string kEmpty;
    if (!impl_ || impl_->type != Type::Object || index >= impl_->members.size()) return kEmpty;
    return impl_->members[index].first;
}

const Value* Value::value_at(size_t index) const noexcept { return at(index); }

// ═══ Errors ═════════════════════════════════════════════════════════════════════════════════

const char* to_string(ErrorCode c) noexcept {
    switch (c) {
        case ErrorCode::Ok: return "ok";
        case ErrorCode::InvalidUtf8: return "invalid_utf8";
        case ErrorCode::UnexpectedEnd: return "unexpected_end";
        case ErrorCode::UnexpectedChar: return "unexpected_char";
        case ErrorCode::BadNumber: return "bad_number";
        case ErrorCode::BadString: return "bad_string";
        case ErrorCode::LoneSurrogate: return "lone_surrogate";
        case ErrorCode::DuplicateKey: return "duplicate_key";
        case ErrorCode::DepthExceeded: return "depth_exceeded";
        case ErrorCode::SizeExceeded: return "size_exceeded";
        case ErrorCode::TrailingData: return "trailing_data";
        case ErrorCode::NotAnObject: return "not_an_object";
    }
    return "unknown";
}

// ═══ Parser ═════════════════════════════════════════════════════════════════════════════════

namespace {

// The recursion ceiling, independent of Options::max_depth.
//
// Options::max_depth is a *policy* knob (settings.h raises it for models.json). This is a
// *safety* one: whatever policy asks for, parse_value() will not nest deeper than this. Each
// frame is well under 200 bytes on MSVC x64, so 256 frames is ~50 KiB against a 1 MiB default
// stack — two orders of magnitude of headroom, and the Value tree it builds is destroyed by the
// same recursion depth on the way out.
constexpr size_t kHardMaxDepth = 256;

// Longest prose fragment echoed from the input into an Error::detail. Detail strings go to logs;
// quoting an unbounded slice of a hostile 64 KiB payload into a log line is its own small denial
// of service.
constexpr size_t kDetailEchoMax = 48;

bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

bool hex_value(char c, uint32_t& out) noexcept {
    if (c >= '0' && c <= '9') { out = static_cast<uint32_t>(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { out = static_cast<uint32_t>(c - 'a') + 10u; return true; }
    if (c >= 'A' && c <= 'F') { out = static_cast<uint32_t>(c - 'A') + 10u; return true; }
    return false;
}

void append_utf8(std::string& out, char32_t cp) {
    const uint32_t u = static_cast<uint32_t>(cp);
    if (u < 0x80u) {
        out.push_back(static_cast<char>(u));
    } else if (u < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (u >> 6)));
        out.push_back(static_cast<char>(0x80u | (u & 0x3Fu)));
    } else if (u < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (u >> 12)));
        out.push_back(static_cast<char>(0x80u | ((u >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (u & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (u >> 18)));
        out.push_back(static_cast<char>(0x80u | ((u >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((u >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (u & 0x3Fu)));
    }
}

std::string echo(std::string_view s) {
    if (s.size() <= kDetailEchoMax) return std::string(s);
    return std::string(s.substr(0, kDetailEchoMax)) + "…";
}

class Parser {
public:
    Parser(std::string_view text, const Options& opts, Error& err) noexcept
        : s_(text),
          max_depth_(std::min(opts.max_depth, kHardMaxDepth)),
          reject_duplicate_keys_(opts.reject_duplicate_keys),
          err_(err) {}

    bool run(Value& out) {
        if (!parse_value(out, 0)) return false;
        skip_ws();
        if (pos_ != s_.size()) {
            return fail(ErrorCode::TrailingData, pos_,
                        "a second value follows the first; a control frame carries exactly one");
        }
        return true;
    }

private:
    std::string_view s_;
    size_t pos_ = 0;
    size_t max_depth_ = kHardMaxDepth;
    bool reject_duplicate_keys_ = true;
    Error& err_;

    bool fail(ErrorCode code, size_t offset, std::string detail) {
        err_.code = code;
        err_.offset = offset;
        err_.detail = std::move(detail);
        return false;
    }

    bool eof() const noexcept { return pos_ >= s_.size(); }
    char peek() const noexcept { return s_[pos_]; }

    // RFC 8259 §2: the only insignificant characters are space, tab, LF and CR. Not vertical
    // tab, not form feed, not U+00A0 — a parser that skipped those would accept documents the
    // Python side rejects.
    void skip_ws() noexcept {
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool literal(std::string_view lit, Value v, Value& out) {
        if (s_.compare(pos_, lit.size(), lit) != 0) {
            return fail(ErrorCode::UnexpectedChar, pos_,
                        "expected the literal " + std::string(lit));
        }
        pos_ += lit.size();
        out = std::move(v);
        return true;
    }

    // `depth` is the number of *enclosing containers*, matching the counting in json.h ("object
    // -> array -> string, i.e. depth 2"). Scalars do not consume depth.
    bool parse_value(Value& out, size_t depth) {
        skip_ws();
        if (eof()) return fail(ErrorCode::UnexpectedEnd, pos_, "expected a value, found end of input");

        const char c = peek();
        switch (c) {
            case '{': return parse_object_body(out, depth + 1);
            case '[': return parse_array_body(out, depth + 1);
            case '"': {
                std::string s;
                if (!parse_string(s)) return false;
                out = Value::string(std::move(s));
                return true;
            }
            case 't': return literal("true", Value::boolean(true), out);
            case 'f': return literal("false", Value::boolean(false), out);
            case 'n': return literal("null", Value::null(), out);

            // Number-adjacent rejections, each given its own message because each is a distinct
            // and common encoder bug rather than a random byte.
            case '+':
                return fail(ErrorCode::BadNumber, pos_,
                            "RFC 8259 has no leading '+' on a number");
            case 'N':
                return fail(ErrorCode::BadNumber, pos_,
                            "NaN is not JSON (docs/protocol.md §4.2)");
            case 'I':
                return fail(ErrorCode::BadNumber, pos_,
                            "Infinity is not JSON (docs/protocol.md §4.2)");
            case '\'':
                return fail(ErrorCode::UnexpectedChar, pos_,
                            "single-quoted strings are not JSON; keys and strings use '\"'");
            case '/':
                return fail(ErrorCode::UnexpectedChar, pos_, "JSON has no comments");
            default:
                if (c == '-' || is_digit(c)) {
                    if (c == '-' && pos_ + 1 < s_.size() && s_[pos_ + 1] == 'I') {
                        return fail(ErrorCode::BadNumber, pos_,
                                    "-Infinity is not JSON (docs/protocol.md §4.2)");
                    }
                    return parse_number(out);
                }
                return fail(ErrorCode::UnexpectedChar, pos_,
                            "unexpected character where a value must begin");
        }
    }

    bool parse_object_body(Value& out, size_t depth) {
        if (depth > max_depth_) {
            return fail(ErrorCode::DepthExceeded, pos_, "object nesting exceeds the depth limit");
        }
        ++pos_;  // '{'
        Value obj = Value::object();

        skip_ws();
        if (eof()) return fail(ErrorCode::UnexpectedEnd, pos_, "unterminated object");
        if (peek() == '}') {
            ++pos_;
            out = std::move(obj);
            return true;
        }

        for (;;) {
            skip_ws();
            if (eof()) return fail(ErrorCode::UnexpectedEnd, pos_, "unterminated object");
            if (peek() != '"') {
                if (peek() == '}') {
                    return fail(ErrorCode::UnexpectedChar, pos_, "trailing comma before '}'");
                }
                return fail(ErrorCode::UnexpectedChar, pos_,
                            "object keys must be double-quoted strings");
            }

            const size_t key_offset = pos_;
            std::string key;
            if (!parse_string(key)) return false;

            skip_ws();
            if (eof()) return fail(ErrorCode::UnexpectedEnd, pos_, "expected ':' after an object key");
            if (peek() != ':') {
                return fail(ErrorCode::UnexpectedChar, pos_, "expected ':' after an object key");
            }
            ++pos_;

            Value val;
            if (!parse_value(val, depth)) return false;

            // docs/protocol.md §4.2: duplicate keys are an error, never last-wins. Last-wins is
            // exactly how two implementations end up disagreeing about which value was meant.
            if (reject_duplicate_keys_ && obj.find(key) != nullptr) {
                return fail(ErrorCode::DuplicateKey, key_offset,
                            "duplicate key \"" + echo(key) + "\"");
            }
            obj.set(key, std::move(val));

            skip_ws();
            if (eof()) return fail(ErrorCode::UnexpectedEnd, pos_, "unterminated object");
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                break;
            }
            return fail(ErrorCode::UnexpectedChar, pos_, "expected ',' or '}' in an object");
        }

        out = std::move(obj);
        return true;
    }

    bool parse_array_body(Value& out, size_t depth) {
        if (depth > max_depth_) {
            return fail(ErrorCode::DepthExceeded, pos_, "array nesting exceeds the depth limit");
        }
        ++pos_;  // '['
        Value arr = Value::array();

        skip_ws();
        if (eof()) return fail(ErrorCode::UnexpectedEnd, pos_, "unterminated array");
        if (peek() == ']') {
            ++pos_;
            out = std::move(arr);
            return true;
        }

        for (;;) {
            skip_ws();
            if (!eof() && peek() == ']') {
                return fail(ErrorCode::UnexpectedChar, pos_, "trailing comma before ']'");
            }

            Value el;
            if (!parse_value(el, depth)) return false;
            arr.push_back(std::move(el));

            skip_ws();
            if (eof()) return fail(ErrorCode::UnexpectedEnd, pos_, "unterminated array");
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                break;
            }
            return fail(ErrorCode::UnexpectedChar, pos_, "expected ',' or ']' in an array");
        }

        out = std::move(arr);
        return true;
    }

    // On entry s_[pos_] == '"'. Produces the *decoded* string: escapes are resolved here, which
    // is what makes docs/protocol.md §4.2's "offsets index the decoded string, not the wire
    // bytes" true by construction for every consumer of this parser.
    bool parse_string(std::string& out) {
        ++pos_;  // opening quote
        out.clear();
        for (;;) {
            if (eof()) return fail(ErrorCode::BadString, pos_, "unterminated string");
            const unsigned char c = static_cast<unsigned char>(s_[pos_]);
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c == '\\') {
                ++pos_;
                if (!parse_escape(out)) return false;
                continue;
            }
            if (c < 0x20) {
                return fail(ErrorCode::BadString, pos_,
                            "a raw control character U+0000–U+001F must be escaped in a string");
            }
            // Bytes ≥ 0x80 are copied verbatim. The whole input was validated as well-formed
            // UTF-8 before parsing began, so this cannot manufacture a malformed sequence.
            out.push_back(static_cast<char>(c));
            ++pos_;
        }
    }

    bool parse_escape(std::string& out) {
        if (eof()) return fail(ErrorCode::UnexpectedEnd, pos_, "string ends inside an escape");
        const char e = s_[pos_++];
        switch (e) {
            case '"': out.push_back('"'); return true;
            case '\\': out.push_back('\\'); return true;
            case '/': out.push_back('/'); return true;   // legal to escape, never required
            case 'b': out.push_back('\b'); return true;
            case 'f': out.push_back('\f'); return true;
            case 'n': out.push_back('\n'); return true;
            case 'r': out.push_back('\r'); return true;
            case 't': out.push_back('\t'); return true;
            case 'u': return parse_unicode_escape(out);
            default:
                return fail(ErrorCode::BadString, pos_ - 1,
                            std::string("unknown escape \\") + e);
        }
    }

    bool read_hex4(uint32_t& out) {
        if (pos_ + 4 > s_.size()) {
            return fail(ErrorCode::UnexpectedEnd, pos_, "\\u needs four hex digits");
        }
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            uint32_t nibble = 0;
            if (!hex_value(s_[pos_ + static_cast<size_t>(i)], nibble)) {
                return fail(ErrorCode::BadString, pos_ + static_cast<size_t>(i),
                            "\\u needs four hex digits");
            }
            v = (v << 4) | nibble;
        }
        pos_ += 4;
        out = v;
        return true;
    }

    // Surrogate handling is the one place a JSON parser can silently emit invalid UTF-8. A lone
    // surrogate has no UTF-8 encoding at all (docs/protocol.md §4.1 spells this out among the
    // ill-formed cases), so it is an error rather than something to encode "as best we can".
    bool parse_unicode_escape(std::string& out) {
        const size_t escape_offset = pos_ - 2;  // points at the backslash
        uint32_t hi = 0;
        if (!read_hex4(hi)) return false;

        if (hi >= 0xD800u && hi <= 0xDBFFu) {
            if (pos_ + 1 >= s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u') {
                return fail(ErrorCode::LoneSurrogate, escape_offset,
                            "high surrogate not followed by a \\u low surrogate");
            }
            pos_ += 2;
            uint32_t lo = 0;
            if (!read_hex4(lo)) return false;
            if (lo < 0xDC00u || lo > 0xDFFFu) {
                return fail(ErrorCode::LoneSurrogate, escape_offset,
                            "high surrogate followed by a non-low-surrogate escape");
            }
            const uint32_t cp = 0x10000u + ((hi - 0xD800u) << 10) + (lo - 0xDC00u);
            append_utf8(out, static_cast<char32_t>(cp));
            return true;
        }
        if (hi >= 0xDC00u && hi <= 0xDFFFu) {
            return fail(ErrorCode::LoneSurrogate, escape_offset,
                        "low surrogate without a preceding high surrogate");
        }
        append_utf8(out, static_cast<char32_t>(hi));
        return true;
    }

    // RFC 8259 §6, scanned by hand before any conversion runs, so that "01", "+1", ".5", "1.",
    // "1e", "0x10" and "Infinity" are all rejected by the grammar rather than by whatever a
    // conversion routine happens to accept.
    bool parse_number(Value& out) {
        const size_t start = pos_;

        if (peek() == '-') ++pos_;
        if (eof()) return fail(ErrorCode::UnexpectedEnd, pos_, "number ends after '-'");

        if (peek() == '0') {
            ++pos_;
            if (!eof() && is_digit(peek())) {
                return fail(ErrorCode::BadNumber, pos_ - 1,
                            "leading zeros are not allowed (RFC 8259 §6)");
            }
        } else if (is_digit(peek())) {
            while (!eof() && is_digit(peek())) ++pos_;
        } else {
            return fail(ErrorCode::BadNumber, pos_, "expected a digit");
        }

        bool integral = true;
        bool negative_exponent = false;

        if (!eof() && peek() == '.') {
            integral = false;
            ++pos_;
            if (eof() || !is_digit(peek())) {
                return fail(ErrorCode::BadNumber, pos_, "a fraction needs at least one digit");
            }
            while (!eof() && is_digit(peek())) ++pos_;
        }

        if (!eof() && (peek() == 'e' || peek() == 'E')) {
            integral = false;
            ++pos_;
            if (!eof() && (peek() == '+' || peek() == '-')) {
                negative_exponent = (peek() == '-');
                ++pos_;
            }
            if (eof() || !is_digit(peek())) {
                return fail(ErrorCode::BadNumber, pos_, "an exponent needs at least one digit");
            }
            while (!eof() && is_digit(peek())) ++pos_;
        }

        const std::string_view token = s_.substr(start, pos_ - start);

        // std::from_chars, not strtod/atof: from_chars is locale-independent. strtod reads the
        // decimal separator from the C locale, so on a machine whose locale uses ',' a perfectly
        // valid "1.5" would parse as 1. Nothing in this program calls setlocale today, but a
        // dependency-free parser should not depend on that staying true.
        double dbl = 0.0;
        const auto dr = std::from_chars(token.data(), token.data() + token.size(), dbl);
        if (dr.ec == std::errc::result_out_of_range) {
            // 1e400 / 1e-400. json.h is explicit that such a literal parses and is rejected by
            // the *caller* through get_int(); it is representable here only as ±inf or 0, and
            // is_integer() is false either way so no integer field can be built from it.
            dbl = negative_exponent ? 0.0 : std::numeric_limits<double>::infinity();
            if (!token.empty() && token.front() == '-') dbl = -dbl;
            integral = false;
        } else if (dr.ec != std::errc{} || dr.ptr != token.data() + token.size()) {
            return fail(ErrorCode::BadNumber, start, "number literal could not be converted");
        }

        int64_t i64 = 0;
        if (integral) {
            const auto ir = std::from_chars(token.data(), token.data() + token.size(), i64);
            if (ir.ec != std::errc{} || ir.ptr != token.data() + token.size()) {
                // Written as an integer but outside int64. The value survives as a double; what
                // is refused is the *claim* that it is an exact integer, which is the claim
                // get_int() makes and the one that matters for `seq` and `utterance_id`.
                integral = false;
                i64 = 0;
            }
        }

        out = integral ? Value::number(i64) : Value::number(dbl);
        return true;
    }
};

}  // namespace

// ═══ Entry points ═══════════════════════════════════════════════════════════════════════════

bool parse(std::string_view text, Value& out, Error& err, const Options& opts) {
    err = Error{};

    // Size first, before UTF-8 validation and before a single Value is built: the cheapest check
    // must be the one that bounds the cost of every check after it.
    if (text.size() > opts.max_bytes) {
        err.code = ErrorCode::SizeExceeded;
        err.offset = opts.max_bytes;
        err.detail = "input exceeds the configured size limit";
        return false;
    }

    // docs/protocol.md §1: fail at the lowest layer that can see the fault. Invalid UTF-8 is an
    // encoding-layer fault and maps to the wire code `bad_utf8`, not `bad_json` — and it is
    // checked over the whole input up front rather than per-string, so that a malformed sequence
    // in a part of the document the parser never reaches is still caught.
    // `err.offset` becomes the wire's `at_offset`, which docs/protocol.md anchors to the START of
    // the malformed sequence, not to the byte that failed to continue it. The two differ whenever
    // the lead byte is itself legal, so reporting the precise byte here put the C++ endpoint one
    // byte off from the Python one — see the comment on utf8::validate.
    size_t bad = 0;
    size_t seq_start = 0;
    if (!utf8::validate(text, &bad, &seq_start)) {
        err.code = ErrorCode::InvalidUtf8;
        err.offset = seq_start;
        err.detail = "input is not well-formed UTF-8 (sequence at " + std::to_string(seq_start) +
                     " fails at byte " + std::to_string(bad) + ")";
        return false;
    }

    Parser p(text, opts, err);
    Value parsed;
    if (!p.run(parsed)) return false;
    out = std::move(parsed);
    return true;
}

bool parse_object(std::string_view text, Value& out, Error& err, const Options& opts) {
    Value v;
    if (!parse(text, v, err, opts)) return false;
    if (!v.is_object()) {
        err.code = ErrorCode::NotAnObject;
        err.offset = 0;
        err.detail = "a control frame's top level must be an object (docs/protocol.md §4.2)";
        return false;
    }
    out = std::move(v);
    return true;
}

// ═══ Serialization ══════════════════════════════════════════════════════════════════════════

namespace {

// RFC 8259 §7 requires escaping exactly `"`, `\` and U+0000–U+001F. Everything else — including
// every non-ASCII code point — is emitted as raw UTF-8. That is deliberate and is asked for by
// docs/protocol.md §4.2 ("Senders SHOULD emit raw UTF-8 rather than escapes … because it makes
// stable_bytes offsets directly checkable against the wire bytes"). '/' is *not* escaped; the
// \/ escape is a legacy HTML-embedding habit and produces bytes the other side must un-escape
// for no gain.
void escape_string(std::string_view s, std::string& out) {
    out.push_back('"');
    for (const char raw : s) {
        const unsigned char c = static_cast<unsigned char>(raw);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static const char kHex[] = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(kHex[(c >> 4) & 0x0F]);
                    out.push_back(kHex[c & 0x0F]);
                } else {
                    out.push_back(raw);
                }
        }
    }
    out.push_back('"');
}

void write_number(const Value& v, std::string& out) {
    char buf[64];
    if (v.is_integer()) {
        const auto r = std::to_chars(buf, buf + sizeof(buf), v.as_int());
        out.append(buf, static_cast<size_t>(r.ptr - buf));
        return;
    }
    const double d = v.as_double();
    if (!std::isfinite(d)) {
        // JSON has no representation for infinity or NaN, and emitting the JavaScript spellings
        // would produce a frame the peer must reject as bad_json. `null` is wrong in a different
        // way but is at least parseable, and the protocol defines no non-integer number, so this
        // path is unreachable from the wire codec. Stated rather than left to be discovered.
        out += "null";
        return;
    }
    // Shortest round-trip form, locale-independent (same reason as from_chars above).
    const auto r = std::to_chars(buf, buf + sizeof(buf), d);
    out.append(buf, static_cast<size_t>(r.ptr - buf));
}

void serialize_into(const Value& v, std::string& out, int indent, int level) {
    const bool pretty = indent >= 0;
    const auto newline_indent = [&](int lvl) {
        if (!pretty) return;
        out.push_back('\n');
        out.append(static_cast<size_t>(indent) * static_cast<size_t>(lvl), ' ');
    };

    switch (v.type()) {
        case Type::Null:
            out += "null";
            return;
        case Type::Bool:
            out += v.as_bool() ? "true" : "false";
            return;
        case Type::Number:
            write_number(v, out);
            return;
        case Type::String:
            escape_string(v.as_string(), out);
            return;
        case Type::Array: {
            if (v.size() == 0) {
                out += "[]";
                return;
            }
            out.push_back('[');
            for (size_t i = 0; i < v.size(); ++i) {
                if (i != 0) out.push_back(',');
                newline_indent(level + 1);
                serialize_into(*v.at(i), out, indent, level + 1);
            }
            newline_indent(level);
            out.push_back(']');
            return;
        }
        case Type::Object: {
            if (v.size() == 0) {
                out += "{}";
                return;
            }
            out.push_back('{');
            for (size_t i = 0; i < v.size(); ++i) {
                if (i != 0) out.push_back(',');
                newline_indent(level + 1);
                escape_string(v.key_at(i), out);
                out.push_back(':');
                if (pretty) out.push_back(' ');
                serialize_into(*v.value_at(i), out, indent, level + 1);
            }
            newline_indent(level);
            out.push_back('}');
            return;
        }
    }
}

}  // namespace

std::string serialize(const Value& v) {
    std::string out;
    serialize_into(v, out, -1, 0);
    return out;
}

std::string serialize_pretty(const Value& v, int indent) {
    std::string out;
    serialize_into(v, out, indent < 0 ? 0 : indent, 0);
    return out;
}

}  // namespace kd::json
