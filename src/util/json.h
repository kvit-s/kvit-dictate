#pragma once
//
// A minimal in-repo JSON DOM: parser, value type, serializer.
//
// Implements: docs/protocol.md §4.2 (the JSON subset the wire protocol permits), and the
// `models.json` manifest reader in settings.h. Those are the only two consumers; the parser is
// sized for them and for nothing else.
//
// ─── Why this file exists ───────────────────────────────────────────────────────────────────
//
// The repository takes no third-party C++ dependencies at all (plan.md §5.3 / house rules), so
// there is no nlohmann/json, no RapidJSON, no simdjson. That is not a hardship here: the
// protocol's JSON is a flat object with at most a short array of short strings, the total
// payload is capped at 64 KiB by docs/protocol.md §1.1, and the spec deliberately restricts the
// grammar precisely so that two hand-written parsers — this one and the Python side's — cannot
// disagree on exotic input. A large general-purpose parser would *increase* the risk of
// divergence, because the two implementations would differ in exactly the corners no test
// covers.
//
// ─── The subset, restated from docs/protocol.md §4.2 ────────────────────────────────────────
//
//   * Input must be well-formed UTF-8 (strict; see util/utf8.h). Invalid UTF-8 is reported as
//     a distinct error code so the caller can raise `bad_utf8` rather than `bad_json` — the
//     protocol distinguishes them and a receiver must fail at the lowest layer that can see the
//     fault (docs/protocol.md §1).
//   * Top level for a control frame must be an object. `parse()` will happily return a scalar
//     or array; it is `parse_object()` that enforces the protocol's rule, so `models.json`
//     (also an object) and any future array-valued file share one parser.
//   * No trailing commas. No comments. No single quotes. No unquoted keys.
//   * No `NaN`, `Infinity`, `-Infinity`; no leading `+`; no leading zeros. Plain RFC 8259.
//   * **Duplicate keys are an error**, not last-wins. Last-wins is how two implementations end
//     up disagreeing about which value was meant.
//   * `\uXXXX` escapes are accepted and decoded; surrogate pairs must be well formed and a lone
//     surrogate is an error. Escapes are decoded *before* any offset is applied — this is the
//     subtle one that docs/protocol.md §4.2 calls out: `"café"` is 8 bytes on the wire and 5
//     bytes decoded, and `stable_bytes` means the 5-byte space.
//   * Depth and size limits, checked while parsing, so a hostile or corrupt payload cannot
//     recurse the stack or allocate unboundedly before the length check catches it.
//
// ─── Numbers ────────────────────────────────────────────────────────────────────────────────
//
// Every number this protocol defines is an integer (docs/protocol.md §4.2). A JsonValue
// therefore records whether the literal it parsed was integral and preserves the exact int64
// alongside the double. `as_int()` succeeds only for a value that was written as an integer and
// fits in int64 — so `{"seq":12}` is accepted and `{"seq":12.0}` and `{"seq":1e400}` are
// rejected as `bad_message` by the caller, which is what the spec permits and what keeps a
// float round-trip from silently renumbering a sequence.
//
// ─── Object key order ───────────────────────────────────────────────────────────────────────
//
// Members are stored in insertion order in a vector, not a hash map. Three reasons: objects
// here have under a dozen keys so linear lookup is faster than hashing; serialization is then
// byte-deterministic, which makes fixtures and golden files possible; and duplicate detection
// is a natural part of insertion rather than an extra pass.
//
// ─── Threading ──────────────────────────────────────────────────────────────────────────────
//
// JsonValue is a plain value type with no shared state: safe to move between threads, not safe
// to mutate from two at once. parse() and serialize() are pure and reentrant. No Windows
// headers, no globals — the protocol codec that uses this must stay unit-testable off-Windows.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kd::json {

enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

// Object members are reached through key_at()/value_at() in insertion order; there is no
// exposed Member struct, so the storage layout stays an implementation detail of json.cpp.
class Value {
public:
    Value() noexcept;  // Null
    ~Value();
    Value(const Value&);
    Value& operator=(const Value&);
    Value(Value&&) noexcept;
    Value& operator=(Value&&) noexcept;

    // ── Construction ────────────────────────────────────────────────────────────────────────
    static Value null();
    static Value boolean(bool b);
    static Value number(int64_t n);
    static Value number(double d);  // records itself as non-integral
    static Value string(std::string s);
    static Value array();
    static Value object();

    // ── Interrogation ───────────────────────────────────────────────────────────────────────
    [[nodiscard]] Type type() const noexcept;
    [[nodiscard]] bool is_null() const noexcept { return type() == Type::Null; }
    [[nodiscard]] bool is_bool() const noexcept { return type() == Type::Bool; }
    [[nodiscard]] bool is_number() const noexcept { return type() == Type::Number; }
    [[nodiscard]] bool is_string() const noexcept { return type() == Type::String; }
    [[nodiscard]] bool is_array() const noexcept { return type() == Type::Array; }
    [[nodiscard]] bool is_object() const noexcept { return type() == Type::Object; }

    // True only if the literal was written without a fraction or exponent and fits in int64.
    [[nodiscard]] bool is_integer() const noexcept;

    // ── Accessors. Each returns false and leaves `out` untouched on a type mismatch, so a
    //    message decoder is a straight-line sequence of `if (!get(...)) return bad_message;`.
    bool get_bool(bool& out) const noexcept;
    bool get_int(int64_t& out) const noexcept;
    bool get_double(double& out) const noexcept;
    bool get_string(std::string_view& out) const noexcept;

    // Unchecked forms for call sites that have already tested type(). Return a default value on
    // mismatch rather than throwing (see util/result.h on why nothing here throws).
    [[nodiscard]] bool as_bool(bool def = false) const noexcept;
    [[nodiscard]] int64_t as_int(int64_t def = 0) const noexcept;
    [[nodiscard]] double as_double(double def = 0.0) const noexcept;
    [[nodiscard]] std::string_view as_string(std::string_view def = {}) const noexcept;

    // ── Arrays ──────────────────────────────────────────────────────────────────────────────
    [[nodiscard]] size_t size() const noexcept;  // array element count, or object member count
    [[nodiscard]] const Value* at(size_t index) const noexcept;  // nullptr if out of range
    void push_back(Value v);                                     // no-op unless is_array()

    // ── Objects ─────────────────────────────────────────────────────────────────────────────
    // nullptr when the key is absent. Absence and null are different: a missing optional field
    // is "not sent", an explicit `null` is a value the protocol never defines and which a
    // decoder should reject as bad_message.
    [[nodiscard]] const Value* find(std::string_view key) const noexcept;
    [[nodiscard]] bool has(std::string_view key) const noexcept { return find(key) != nullptr; }

    // Insert or replace. Returns false if this is not an object.
    bool set(std::string_view key, Value v);

    // Member access in insertion order, for serialization and for iterating a manifest.
    [[nodiscard]] const std::string& key_at(size_t index) const noexcept;
    [[nodiscard]] const Value* value_at(size_t index) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ─── Errors ─────────────────────────────────────────────────────────────────────────────────

enum class ErrorCode : uint8_t {
    Ok = 0,
    InvalidUtf8,      // -> protocol `bad_utf8`; distinct from Syntax on purpose
    UnexpectedEnd,    // input ended mid-value
    UnexpectedChar,   // a character that cannot appear here
    BadNumber,        // leading zero, leading +, NaN/Infinity, malformed exponent
    BadString,        // unterminated, raw control character, unknown escape
    LoneSurrogate,    // \uD800 not followed by a low surrogate, or a stray low surrogate
    DuplicateKey,     // docs/protocol.md §4.2: never last-wins
    DepthExceeded,    // nesting beyond Options::max_depth
    SizeExceeded,     // input longer than Options::max_bytes
    TrailingData,     // a second value after the first
    NotAnObject,      // parse_object() only: the top-level value was not an object
};

const char* to_string(ErrorCode c) noexcept;

struct Error {
    ErrorCode code = ErrorCode::Ok;
    size_t offset = 0;   // byte offset into the input where the fault was detected
    std::string detail;  // prose for logs; never parsed (same rule as `error.msg` on the wire)

    [[nodiscard]] bool is_ok() const noexcept { return code == ErrorCode::Ok; }
};

struct Options {
    // Nesting limit. The protocol's deepest legal structure is object -> array -> string, i.e.
    // depth 2; `models.json` reaches depth 3. 32 is generous and still far below any plausible
    // stack limit, and it is checked *during* parsing rather than after.
    size_t max_depth = 32;

    // Input size limit. Defaults to MAX_PAYLOAD from docs/protocol.md §1.1 so that a control
    // frame that somehow got past the framing check cannot allocate without bound here either.
    // settings.h raises it for `models.json`, which is a local trusted file.
    size_t max_bytes = 65536;

    // Reject duplicate keys (docs/protocol.md §4.2). Settable only so a future non-protocol
    // consumer can opt out with the decision visible at the call site; the protocol codec must
    // leave it true.
    bool reject_duplicate_keys = true;
};

// ─── Entry points ───────────────────────────────────────────────────────────────────────────

// Parse any JSON value. Returns false on failure with `err` populated.
bool parse(std::string_view text, Value& out, Error& err, const Options& opts = {});

// Parse and require the top level to be an object — docs/protocol.md §4.2's rule for control
// frames ("Top level is an object. Not an array, not a scalar.").
bool parse_object(std::string_view text, Value& out, Error& err, const Options& opts = {});

// Compact serialization: no spaces, no newlines, raw UTF-8 output rather than \u escapes
// (the Python side's equivalent is `ensure_ascii=False`). docs/protocol.md §4.2 asks for this
// specifically because it makes `stable_bytes` offsets directly checkable against the wire
// bytes during debugging.
//
// Escapes exactly what RFC 8259 requires: `"` `\` and U+0000–U+001F. Does not escape `/`.
// Members are emitted in insertion order, so the output is byte-deterministic.
std::string serialize(const Value& v);

// Indented serialization, for `models.json` round-trips and for log lines a human will read.
// Never used on the wire.
std::string serialize_pretty(const Value& v, int indent = 2);

}  // namespace kd::json
