// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// WinHTTP POST — see http.h for the Windows facts this depends on and for why it is not
// `Normalizer`.

#define KD_LOG_MODULE "asrd.http"

#include "asrd/http.h"

#include <winsock2.h>
#include <ws2tcpip.h>
// Must follow winsock2.h.
#include <windows.h>
// Must follow windows.h.
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "util/log.h"
#include "util/utf8.h"

namespace kd::asrd {
namespace {

// Same token openai_http.py uses. Kept identical so a packet capture taken against the Python
// backend and one taken against this one are comparable line for line.
constexpr const char* kBoundary = "----kvit-dictate-boundary-7Nq2Xr";

constexpr size_t kMaxResponseBytes = 1u << 20;  // 1 MiB; a transcript is never close

std::string ascii_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        out.push_back(u < 0x80 ? static_cast<char>(std::tolower(u)) : c);
    }
    return out;
}

void put_le32(std::string& out, uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

void put_le16(std::string& out, uint16_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
}

}  // namespace

bool parse_http_url(const std::string& url, HttpUrl& out) {
    const std::string lower = ascii_lower(url);
    if (lower.rfind("http://", 0) != 0 && lower.rfind("https://", 0) != 0) return false;

    std::wstring wide;
    if (!utf8::to_utf16(url, wide)) return false;

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    // Null buffers with -1 lengths: WinHttpCrackUrl then reports offsets into `wide` rather
    // than copying, which is what this wants. See the note in http.h.
    uc.dwSchemeLength = static_cast<DWORD>(-1);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0, &uc)) return false;
    if (uc.lpszHostName == nullptr || uc.dwHostNameLength == 0) return false;
    if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS) return false;

    std::wstring whost(uc.lpszHostName, uc.dwHostNameLength);
    if (!utf8::from_utf16(whost, out.host)) return false;
    out.port = uc.nPort;
    out.secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

    std::wstring wpath;
    if (uc.lpszUrlPath != nullptr && uc.dwUrlPathLength != 0) {
        wpath.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    }
    if (uc.lpszExtraInfo != nullptr && uc.dwExtraInfoLength != 0) {
        wpath.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    }
    out.path.clear();
    if (!wpath.empty() && !utf8::from_utf16(wpath, out.path)) return false;
    if (out.path.empty()) out.path = "/";
    return true;
}

std::string validate_service_url(const std::string& url) {
    if (url.empty()) return "--service is required; give the speech service's base URL";
    const std::string lower = ascii_lower(url);
    if (lower.rfind("http://", 0) != 0 && lower.rfind("https://", 0) != 0) {
        return "--service must be an http:// or https:// URL (a docs/protocol.md server is a "
               "different thing and is not what this translator talks to)";
    }
    for (const char c : url) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) return "--service contains a control character";
        if (c == ' ' || c == '\t') return "--service contains whitespace";
        if (c == '"' || c == '\'' || c == '`') return "--service contains a quote character";
    }
    HttpUrl parsed;
    if (!parse_http_url(url, parsed)) return "--service is not a well-formed URL: " + url;
    return {};
}

std::string api_key_problem(std::string_view key) {
    if (key.size() > 1024) return "the API key is longer than 1024 characters";
    for (const char c : key) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u >= 0x7F) {
            return "the API key contains a space, a line break or a non-ASCII character";
        }
    }
    return {};
}

// ─── multipart and WAV ──────────────────────────────────────────────────────────────────────

std::string encode_multipart(const std::vector<std::pair<std::string, std::string>>& fields,
                             std::string_view file_field, std::string_view filename,
                             std::string_view file_content_type, std::string_view payload,
                             std::string& content_type_out) {
    std::string out;
    out.reserve(payload.size() + 512);
    for (const auto& [key, value] : fields) {
        out += "--";
        out += kBoundary;
        out += "\r\nContent-Disposition: form-data; name=\"";
        out += key;
        out += "\"\r\n\r\n";
        out += value;
        out += "\r\n";
    }
    if (file_field.empty()) {
        out += "--";
        out += kBoundary;
        out += "--\r\n";
        content_type_out = std::string("multipart/form-data; boundary=") + kBoundary;
        return out;
    }
    out += "--";
    out += kBoundary;
    out += "\r\nContent-Disposition: form-data; name=\"";
    out.append(file_field);
    out += "\"; filename=\"";
    out.append(filename);
    out += "\"\r\nContent-Type: ";
    out.append(file_content_type);
    out += "\r\n\r\n";
    out.append(payload);
    out += "\r\n--";
    out += kBoundary;
    out += "--\r\n";

    content_type_out = std::string("multipart/form-data; boundary=") + kBoundary;
    return out;
}

std::string wav_bytes(const int16_t* samples, size_t count) {
    const uint32_t data_bytes = static_cast<uint32_t>(count * 2);
    std::string out;
    out.reserve(44 + data_bytes);
    out += "RIFF";
    put_le32(out, 36 + data_bytes);
    out += "WAVEfmt ";
    put_le32(out, 16);                  // PCM fmt chunk size
    put_le16(out, 1);                   // WAVE_FORMAT_PCM
    put_le16(out, 1);                   // mono (docs/protocol.md §6.3)
    put_le32(out, 16000);               // 16 kHz
    put_le32(out, 16000 * 2);           // byte rate
    put_le16(out, 2);                   // block align
    put_le16(out, 16);                  // bits per sample
    out += "data";
    put_le32(out, data_bytes);
    // Written byte by byte rather than memcpy'd: on x64 the result is identical, but the
    // little-endian conversion is explicit so the code does not become wrong by accident if it
    // is ever read on another architecture.
    for (size_t i = 0; i < count; ++i) {
        const auto v = static_cast<uint16_t>(samples[i]);
        out.push_back(static_cast<char>(v & 0xFF));
        out.push_back(static_cast<char>((v >> 8) & 0xFF));
    }
    return out;
}

// ─── HttpClient ─────────────────────────────────────────────────────────────────────────────

struct HttpClient::Impl {
    std::mutex mutex;
    HINTERNET in_flight = nullptr;  // the request handle, guarded by `mutex`
    bool cancelled = false;
    std::string bearer;  // set before the first post(); never logged
};

HttpClient::HttpClient() : impl_(new Impl()) {}

HttpClient::~HttpClient() {
    cancel();
    delete impl_;
}

void HttpClient::cancel() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->cancelled = true;
    if (impl_->in_flight != nullptr) {
        // Closing the handle is what makes the blocked WinHTTP call return. The worker still
        // owns the handle and will close it again; WinHttpCloseHandle is not idempotent, so the
        // worker clears this pointer under the same lock and only one close happens.
        WinHttpCloseHandle(impl_->in_flight);
        impl_->in_flight = nullptr;
    }
}

void HttpClient::reset_cancel() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->cancelled = false;
}

void HttpClient::set_bearer_token(std::string token) { impl_->bearer = std::move(token); }

bool HttpClient::cancelled() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->cancelled;
}

Status HttpClient::probe_tcp(const std::string& host, uint16_t port, uint32_t timeout_ms) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return Status{StatusCode::Unavailable, "WSAStartup failed"};
    }
    struct Cleanup {
        ~Cleanup() { WSACleanup(); }
    } cleanup;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0 || result == nullptr) {
        return Status{StatusCode::NotFound, "cannot resolve " + host};
    }

    Status last{StatusCode::Unavailable, "no address answered"};
    for (const addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        const SOCKET fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == INVALID_SOCKET) continue;

        // Non-blocking connect plus select, because a blocking connect to a host that is up but
        // not listening can sit on the OS's own SYN retry schedule for 20 seconds, which is far
        // past any budget the load path has.
        u_long nonblocking = 1;
        ::ioctlsocket(fd, FIONBIO, &nonblocking);
        const int rc = ::connect(fd, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
        bool ok = rc == 0;
        if (!ok && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set write_set;
            FD_ZERO(&write_set);
            FD_SET(fd, &write_set);
            timeval tv{};
            tv.tv_sec = static_cast<long>(timeout_ms / 1000);
            tv.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
            if (::select(0, nullptr, &write_set, nullptr, &tv) > 0) {
                int err = 0;
                int len = sizeof err;
                ok = ::getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err),
                                  &len) == 0 &&
                     err == 0;
            }
        }
        ::closesocket(fd);
        if (ok) {
            ::freeaddrinfo(result);
            return Status::success();
        }
        last = Status{StatusCode::Unavailable,
                      "cannot reach " + host + ":" + std::to_string(port),
                      static_cast<uint32_t>(WSAGetLastError())};
    }
    ::freeaddrinfo(result);
    return last;
}

Result<HttpResponse> HttpClient::post(const HttpUrl& url, std::string_view content_type,
                                      std::string_view body, uint32_t timeout_ms) {
    if (cancelled()) {
        return Status{StatusCode::Unavailable, "cancelled before the request was sent"};
    }

    std::string headers = std::string("Content-Type: ") + std::string(content_type) + "\r\n";
    if (!impl_->bearer.empty()) {
        if (!api_key_problem(impl_->bearer).empty()) {
            return Status{StatusCode::InvalidArgument, "the API key cannot be sent as a header"};
        }
        headers += "Authorization: Bearer " + impl_->bearer + "\r\n";
    }

    std::wstring whost;
    std::wstring wpath;
    std::wstring wheaders;
    if (!utf8::to_utf16(url.host, whost) || !utf8::to_utf16(url.path, wpath) ||
        !utf8::to_utf16(headers, wheaders)) {
        return Status{StatusCode::InvalidArgument, "URL is not representable as UTF-16"};
    }

    // ─── Time ───────────────────────────────────────────────────────────────────────────────
    //
    // **WinHTTP's own timeouts do not bound a synchronous request, and this learned that late.**
    // Measured 2026-09-30 against servers that take 3-4 s to answer, with every timeout at
    // 1-1.5 s: WinHttpSendRequest itself blocks until the answer arrives. On loopback the request
    // then simply succeeds; against httpbin.org, WinHttpReceiveResponse reports
    // ERROR_WINHTTP_TIMEOUT only *after* the answer is already there. So the timeouts decide
    // whether a slow answer is *kept*, and never when the caller gets control back.
    //
    // Two things follow, one for each half of that:
    //
    //   * **The receive timeout is whatever is left of the budget**, not a quarter of it like
    //     resolve, connect and send. A transcription is silence on the socket until the model is
    //     done, so the server's processing time is all receive. A quarter of 30 s never mattered
    //     against a LAN box answering in half a second. OpenRouter took 9.8 s for ten seconds of
    //     audio on 2026-09-30 (and 0.5 s a minute later), and the quarter-budget receive threw
    //     that answer away with 22 s of the budget unspent — the first dictation sent through it.
    //
    //   * **The deadline is enforced by a watchdog**, which closes the request handle when it
    //     passes — exactly what cancel() does, so the blocked call returns promptly and the
    //     handle is still closed exactly once. Without it a provider that hangs holds the
    //     session for as long as the provider likes.
    const auto share = static_cast<int>(timeout_ms / 4 + 1);
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    const auto expired = [&]() { return GetTickCount64() >= deadline; };
    const auto arm_receive = [&](HINTERNET h) {
        const ULONGLONG now = GetTickCount64();
        DWORD left = now >= deadline ? 1u : static_cast<DWORD>(deadline - now);
        WinHttpSetOption(h, WINHTTP_OPTION_RECEIVE_TIMEOUT, &left, sizeof left);
        WinHttpSetOption(h, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &left, sizeof left);
    };

    HINTERNET session = WinHttpOpen(L"kvit-asrd/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        return Status{StatusCode::Unavailable, "WinHttpOpen failed", GetLastError()};
    }
    WinHttpSetTimeouts(session, share, share, share, share);

    HINTERNET connect = WinHttpConnect(session, whost.c_str(), url.port, 0);
    if (connect == nullptr) {
        const DWORD e = GetLastError();
        WinHttpCloseHandle(session);
        return Status{StatusCode::Unavailable, "WinHttpConnect failed", e};
    }

    HINTERNET request =
        WinHttpOpenRequest(connect, L"POST", wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, url.secure ? WINHTTP_FLAG_SECURE : 0u);
    if (request == nullptr) {
        const DWORD e = GetLastError();
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return Status{StatusCode::Unavailable, "WinHttpOpenRequest failed", e};
    }

    // Publish the handle so cancel() — and the watchdog — can close it. Everything below must go
    // through `take_request()` for the close, so the handle is closed exactly once.
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->cancelled) {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return Status{StatusCode::Unavailable, "cancelled before the request was sent"};
        }
        impl_->in_flight = request;
    }
    const auto take_request = [this]() -> HINTERNET {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        HINTERNET h = impl_->in_flight;
        impl_->in_flight = nullptr;
        return h;
    };

    // The watchdog. It owns nothing: it closes the handle only if it is still the one in flight,
    // under the same lock cancel() and take_request() use, and says so in `timed_out` so the
    // failure below is reported as the timeout it is rather than as whatever WinHTTP calls a
    // handle closed under it.
    std::atomic<bool> timed_out{false};
    struct Watchdog {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        std::thread t;
        ~Watchdog() {
            {
                std::lock_guard<std::mutex> lock(m);
                done = true;
            }
            cv.notify_all();
            if (t.joinable()) t.join();
        }
    } watchdog;
    const ULONGLONG armed_at = GetTickCount64();
    const auto left = std::chrono::milliseconds(deadline > armed_at ? deadline - armed_at : 0);
    watchdog.t = std::thread([&, request, left] {
        std::unique_lock<std::mutex> lock(watchdog.m);
        if (watchdog.cv.wait_for(lock, left, [&] { return watchdog.done; })) {
            return;
        }
        std::lock_guard<std::mutex> inner(impl_->mutex);
        if (impl_->in_flight == request) {
            timed_out.store(true);
            WinHttpCloseHandle(request);
            impl_->in_flight = nullptr;
        }
    });

    const auto finish = [&](Status st) -> Result<HttpResponse> {
        if (HINTERNET h = take_request(); h != nullptr) WinHttpCloseHandle(h);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return st;
    };
    const std::string too_slow =
        "the speech service did not answer within " + std::to_string(timeout_ms) + " ms";
    // Every WinHTTP failure goes through here, so the three reasons a call can fail under us —
    // the deadline, a cancel, and WinHTTP's own timeout — are told apart the same way each time.
    const auto failed = [&](const char* what, DWORD e) -> Result<HttpResponse> {
        if (timed_out.load()) return finish(Status{StatusCode::Timeout, too_slow});
        if (cancelled()) return finish(Status{StatusCode::Unavailable, "request cancelled"});
        if (e == ERROR_WINHTTP_TIMEOUT) return finish(Status{StatusCode::Timeout, too_slow, e});
        return finish(Status{StatusCode::Unavailable, std::string(what) + " failed", e});
    };

    arm_receive(request);
    if (!WinHttpSendRequest(request, wheaders.c_str(), static_cast<DWORD>(-1),
                            const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                            static_cast<DWORD>(body.size()), 0)) {
        return failed("WinHttpSendRequest", GetLastError());
    }
    if (!WinHttpReceiveResponse(request, nullptr)) {
        return failed("WinHttpReceiveResponse", GetLastError());
    }
    if (expired()) return finish(Status{StatusCode::Timeout, too_slow});

    HttpResponse response;
    DWORD status_code = 0;
    DWORD len = sizeof status_code;
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &len,
                             WINHTTP_NO_HEADER_INDEX)) {
        return failed("WinHttpQueryHeaders", GetLastError());
    }
    response.status = status_code;

    for (;;) {
        if (expired()) return finish(Status{StatusCode::Timeout, too_slow});
        DWORD avail = 0;
        arm_receive(request);
        if (!WinHttpQueryDataAvailable(request, &avail)) {
            return failed("WinHttpQueryDataAvailable", GetLastError());
        }
        // Zero available is the only reliable end-of-body signal; a short read is not one.
        if (avail == 0) break;
        if (response.body.size() + avail > kMaxResponseBytes) {
            return finish(Status{StatusCode::Malformed, "response exceeds 1 MiB"});
        }
        const size_t at = response.body.size();
        response.body.resize(at + avail);
        DWORD read = 0;
        if (!WinHttpReadData(request, response.body.data() + at, avail, &read)) {
            return failed("WinHttpReadData", GetLastError());
        }
        response.body.resize(at + read);
        if (read == 0) break;
    }

    if (HINTERNET h = take_request(); h != nullptr) WinHttpCloseHandle(h);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return response;
}

}  // namespace kd::asrd
