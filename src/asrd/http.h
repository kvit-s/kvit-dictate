// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// A small synchronous WinHTTP POST, cancellable from another thread.
//
// ─── Why this is not `Normalizer` ───────────────────────────────────────────────────────────
//
// `src/normalizer.cpp` is the *shape* to copy, not a library to call: it talks to a different
// URL with a different body, owns its own worker thread and its own supersede/coalesce policy,
// and lives in kd_core, which kvit-asrd deliberately does not link (openai-translator.md §4.8).
// What transfers is the WinHTTP knowledge, and it transfers as facts rather than as code:
//
//   * WinHttpCrackUrl needs every URL_COMPONENTS pointer either null with a zero length, or a
//     real buffer with a real length. Null pointers with dwXxxLength = -1 make it report
//     *offsets into the original string*, which is what this file wants.
//   * The timeouts WinHttpSetTimeouts sets are **per operation, not per request**. A request
//     that stalls after each of four operations takes four times the configured value, so the
//     total is bounded again by hand between operations.
//   * WinHttpReceiveResponse must precede WinHttpQueryHeaders and WinHttpReadData, and
//     WinHttpQueryDataAvailable returning 0 is the only reliable end-of-body signal. A reader
//     that stops at a short read truncates any body arriving in more than one TCP segment.
//
// ─── Cancellation ───────────────────────────────────────────────────────────────────────────
//
// `HttpClient::cancel()` closes the in-flight request handle from another thread, which makes
// the blocked WinHTTP call fail promptly. That is what `cancel` and process teardown need:
// docs/protocol.md §6.1 requires `stopped` to follow a `cancel` without waiting, and a `stop`
// that sat on a 30-second HTTP timeout before emitting `stopped` would hang the client's
// teardown for half a minute.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "util/result.h"

namespace kd::asrd {

// A parsed `http://` or `https://` URL.
struct HttpUrl {
    std::string host;    // UTF-8; converted to wide at the call
    std::string path;    // path + query, "/" when empty
    uint16_t port = 0;
    bool secure = false;
};

// Parse an absolute http/https URL. Any other scheme is rejected: openai-translator.md §1.4
// keeps this backend to plain HTTP(S), and a `ws://` here would be the configuration mistake
// that whole document exists to end.
bool parse_http_url(const std::string& url, HttpUrl& out);

// Validate a `--service` URL as *data on a command line this program constructs*.
//
// The client passes the user's speech-service address to `--service`, so it is user-supplied
// content in an argv the client builds — not a user-supplied argv vector, which is the thing
// release-plan.md §1.2 removed. Rejecting spaces, quotes and control characters before quoting
// is what keeps that distinction real. Returns an empty string when the URL is acceptable and
// the reason otherwise.
std::string validate_service_url(const std::string& url);

// Why `key` cannot be sent as a bearer token, or empty when it can. Printable ASCII with no
// spaces, and not absurdly long: every provider's keys fit that, and anything else is either a
// paste accident — a trailing newline, a smart quote — or a header injection. The client applies
// the same rule to the settings field (`Settings::validate`), so a key it saved is a key this
// accepts.
std::string api_key_problem(std::string_view key);

struct HttpResponse {
    uint32_t status = 0;
    std::string body;
};

// One connection's worth of WinHTTP state. Not thread-safe except for cancel(), which is the
// whole point of it existing.
class HttpClient {
public:
    HttpClient();
    ~HttpClient();
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // A TCP connect to host:port, nothing more. This is `OpenAiHttpBackend._load()`'s check:
    // enough to separate "the host is down or the name does not resolve" from "the endpoint
    // rejected the request", and it costs no inference on a shared box. Done with plain Winsock
    // rather than WinHTTP because WinHTTP has no connect-only operation — WinHttpConnect does
    // not touch the network.
    static Status probe_tcp(const std::string& host, uint16_t port, uint32_t timeout_ms);

    // POST `body` with `content_type`. Returns the status line and the whole body; a non-2xx is
    // a successful call with a status the caller inspects, not a Status failure.
    Result<HttpResponse> post(const HttpUrl& url, std::string_view content_type,
                              std::string_view body, uint32_t timeout_ms);

    // Sent as `Authorization: Bearer <token>` on every post() from now on; empty sends no
    // `Authorization` header at all, which is what a LAN service that asks for none wants.
    //
    // Set once, before the first post(), by whoever owns the client. A token that could end a
    // header line is refused here as well as at the command line — see `api_key_problem()` —
    // because this is the call that would put it on the wire.
    void set_bearer_token(std::string token);

    // Abort whatever is in flight, from any thread. Idempotent, and safe when nothing is in
    // flight. Latches: every subsequent post() fails immediately until reset_cancel().
    void cancel();
    void reset_cancel();
    [[nodiscard]] bool cancelled() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

// ─── multipart/form-data ────────────────────────────────────────────────────────────────────

// Encode text fields plus one file part, as `openai_http.py::_multipart` does. An empty
// `file_field` leaves the file part out, which is how the API-key check asks a question that no
// model has to answer (backend_openai.h, `OpenAiBackend::load`).
//
// Hand-rolled on both sides. The boundary is a fixed string rather than a random one:
// randomness would need a source and buys nothing here, because a WAV cannot contain the token
// — its body is length-prefixed rather than delimited.
std::string encode_multipart(const std::vector<std::pair<std::string, std::string>>& fields,
                             std::string_view file_field, std::string_view filename,
                             std::string_view file_content_type, std::string_view payload,
                             std::string& content_type_out);

// Wrap raw 16 kHz mono s16le PCM in a WAV container.
//
// WAV rather than a compressed format because it is lossless and because re-encoding would put
// a codec between the measured audio and the model for no benefit. The protocol fixes the
// format (docs/protocol.md §6.3), so the header is not negotiable and is not parameterised.
std::string wav_bytes(const int16_t* samples, size_t count);

}  // namespace kd::asrd
