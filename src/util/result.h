#pragma once
//
// kd::Status / kd::Result<T> — the error-return type used by every module that can fail
// for a reason the caller must be able to branch on.
//
// Why this exists at all, and why it is not std::expected:
//   std::expected is C++23. This project builds with /std:c++20 (plan.md §5.3, matching
//   kvit-notes), so <expected> is not available. Rather than raise the language level for one
//   type, or scatter `bool f(..., std::string* err)` signatures with subtly different
//   conventions across ten modules, the convention is fixed here once.
//
// Design constraints, all deliberate:
//   * No exceptions on the hot paths. Audio capture, the low-level keyboard hook, and the pipe
//     I/O thread all run under latency budgets where an exception unwind is unacceptable, and
//     the LL hook in particular is silently unhooked by Windows if it is slow (plan.md §1.8).
//     Status is a value; throwing is reserved for genuinely exceptional startup failures.
//   * The machine-readable half is a `ProtocolError` code where one applies (see protocol.h) and
//     a plain enum otherwise. `message` is prose for logs and the tray, and is never parsed —
//     the same rule docs/protocol.md §6.2 applies to `error.msg` on the wire.
//   * Zero allocation on the success path: an ok Status holds an empty std::string, which for
//     every MSVC release is SSO and does not touch the heap.
//
// Threading: Status and Result are plain values with no shared state. They are safe to move
// across threads; they are not synchronised and were never meant to be shared by reference.
//
// Implements: no plan.md section of its own — infrastructure for all of them.

#include <cstdint>
#include <cstdio>  // std::snprintf, for Status::to_string's hex os_error
#include <string>
#include <type_traits>
#include <utility>

namespace kd {

// Coarse failure classification. This is *not* the protocol error code set (protocol.h owns
// that, and it is normative on the wire); it is the local vocabulary for "why did this call
// fail", used by modules that have no wire representation at all — settings loading, device
// enumeration, overlay creation.
enum class StatusCode : uint8_t {
    Ok = 0,
    InvalidArgument,  // caller passed something malformed; a bug on the caller's side
    NotFound,         // file, device, model id, window
    PermissionDenied, // ACL, UIPI, integrity level, secure desktop
    Unavailable,      // device removed, sidecar not running, pipe not connected
    Timeout,          // a bounded wait expired; see the constants in protocol.h §1.1
    Overloaded,       // a bounded queue is full — never resolved by dropping data (§9)
    Malformed,        // data read from disk or the wire did not conform
    Unsupported,      // a declared, deliberate "we do not do this" (e.g. elevated targets)
    Internal,         // a bug in this program; the message names the invariant that broke
    OsError,          // a Win32 call failed; `os_error` carries the GetLastError() value
};

// Inline for the same reason Status::to_string() is: this header has no .cpp, so a declaration
// here with the definition "somewhere" resolves to nowhere. The spellings are log-facing only —
// StatusCode is deliberately NOT the wire vocabulary (protocol.h owns that), so nothing parses
// these and they may be reworded freely.
inline const char* to_string(StatusCode c) noexcept {
    switch (c) {
        case StatusCode::Ok:               return "Ok";
        case StatusCode::InvalidArgument:  return "InvalidArgument";
        case StatusCode::NotFound:         return "NotFound";
        case StatusCode::PermissionDenied: return "PermissionDenied";
        case StatusCode::Unavailable:      return "Unavailable";
        case StatusCode::Timeout:          return "Timeout";
        case StatusCode::Overloaded:       return "Overloaded";
        case StatusCode::Malformed:        return "Malformed";
        case StatusCode::Unsupported:      return "Unsupported";
        case StatusCode::Internal:         return "Internal";
        case StatusCode::OsError:          return "OsError";
    }
    return "Unknown";
}

// A failure, or success. Deliberately small and copyable.
class Status {
public:
    Status() noexcept = default;  // ok

    Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}

    // `os_error` is a raw GetLastError()/HRESULT value. Kept separate from the message so a
    // caller can retry on a specific code without regex-ing prose.
    Status(StatusCode code, std::string message, uint32_t os_error)
        : code_(code), os_error_(os_error), message_(std::move(message)) {}

    static Status ok() noexcept { return Status{}; }

    [[nodiscard]] bool is_ok() const noexcept { return code_ == StatusCode::Ok; }
    explicit operator bool() const noexcept { return is_ok(); }

    [[nodiscard]] StatusCode code() const noexcept { return code_; }
    [[nodiscard]] uint32_t os_error() const noexcept { return os_error_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    // "InvalidArgument: sample_rate must be 16000 (os error 0x00000057)" — for logs only.
    //
    // Defined inline because this header has no .cpp: it was declared and never defined, so the
    // first executable to actually link a caller (main.cpp, pulling settings.obj) failed with an
    // unresolved external rather than at the point the declaration was written. The os_error is
    // printed in hex because that is the form every Windows error is documented and searched in.
    [[nodiscard]] std::string to_string() const {
        std::string s = kd::to_string(code_);
        if (!message_.empty()) {
            s += ": ";
            s += message_;
        }
        if (os_error_ != 0) {
            char buf[32];
            std::snprintf(buf, sizeof buf, " (os error 0x%08X)", os_error_);
            s += buf;
        }
        return s;
    }

private:
    StatusCode code_ = StatusCode::Ok;
    uint32_t os_error_ = 0;
    std::string message_;
};

// A value or a Status. Never both; never neither.
//
// Usage:
//     Result<AudioFormat> r = probe_device(id);
//     if (!r) return r.status();
//     use(*r);
//
// Note the absence of monadic combinators (and_then/transform). They were left out because
// every call site in this project is a handful of lines that reads better as an early return,
// and because a partially-implemented monadic surface invites the assumption that the rest of
// it exists.
template <typename T>
class Result {
    static_assert(!std::is_reference_v<T>, "Result<T&> is not supported; return a pointer.");

public:
    Result(T value) : value_(std::move(value)) {}  // NOLINT(google-explicit-constructor)

    Result(Status status) : status_(std::move(status)) {  // NOLINT(google-explicit-constructor)
        // A Result constructed from an ok Status has no value, which would be a silent
        // default-construction. Callers must return a value or a real failure.
        if (status_.is_ok()) {
            status_ = Status{StatusCode::Internal, "Result constructed from an ok Status"};
        }
    }

    [[nodiscard]] bool is_ok() const noexcept { return status_.is_ok(); }
    explicit operator bool() const noexcept { return is_ok(); }

    [[nodiscard]] const Status& status() const noexcept { return status_; }

    // Precondition: is_ok(). Violating it is a programming error; in debug builds the
    // implementation asserts, in release it is undefined. There is no throwing accessor,
    // for the no-exceptions-on-hot-paths reason above.
    T& value() noexcept { return value_; }
    const T& value() const noexcept { return value_; }
    T& operator*() noexcept { return value_; }
    const T& operator*() const noexcept { return value_; }
    T* operator->() noexcept { return &value_; }
    const T* operator->() const noexcept { return &value_; }

    // For the common "use it or use a default" shape.
    T value_or(T fallback) const { return is_ok() ? value_ : std::move(fallback); }

private:
    Status status_;
    T value_{};
};

}  // namespace kd
