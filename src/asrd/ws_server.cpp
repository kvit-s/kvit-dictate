// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// RFC 6455 server — see ws_server.h for the design and for why it is written by hand.
//
// ─── Windows facts this file depends on, written down because each is easy to get wrong ─────
//
//   * <winsock2.h> MUST precede <windows.h>. WIN32_LEAN_AND_MEAN (set project-wide in
//     CMakeLists.txt) keeps <windows.h> from pulling in winsock1 and producing several hundred
//     redefinition errors, but the include order still has to be right in every TU that uses
//     sockets, so this file includes winsock2 first and does not rely on the define alone.
//   * WSAStartup is refcounted per process. The listener does it once and the count is released
//     in its destructor; a translator that listened twice would still work.
//   * `shutdown(s, SD_BOTH)` is what unblocks a thread parked in recv() or accept(). Closing the
//     handle from another thread is a race — the descriptor can be reused between the close and
//     the parked call noticing — so stop() shuts down first and closes on the owning thread.
//   * `recv` returning 0 is an orderly close; -1 with WSAEINTR/WSAENOTSOCK after a shutdown is
//     the same event seen from the other side of the race, and both are "the peer is gone".
//   * BCryptCreateHash wants a hash-object buffer whose size comes from BCryptGetProperty; the
//     "reuse the algorithm handle" flag (BCRYPT_HASH_REUSABLE_FLAG) is not used because this is
//     one hash per connection.

#define KD_LOG_MODULE "asrd.ws"

#include "asrd/ws_server.h"

// Order matters. See the note above.
#include <winsock2.h>
#include <ws2tcpip.h>
// Must follow winsock2.h.
#include <windows.h>
// Must follow windows.h.
#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>

#include "protocol.h"
#include "util/log.h"

#pragma comment(lib, "ws2_32.lib")

namespace kd::asrd {
namespace {

// A ceiling on one reassembled message, applied before the framing layer sees it. §3.3 bounds
// the protocol frame; this bounds what a hostile or broken peer can make us allocate while we
// are still working out that the frame is illegal.
constexpr size_t kMaxMessageBytes = proto::kMaxFrameOnWire;

// The opening handshake is not allowed to be large. A request head past this is either an
// attack or a bug, and in both cases the answer is a 400 rather than an unbounded read.
constexpr size_t kMaxHeadBytes = 65536;

constexpr int kRecvChunk = 65536;

std::string ascii_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        out.push_back(u < 0x80 ? static_cast<char>(std::tolower(u)) : c);
    }
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

bool sha1(const void* data, size_t size, uint8_t out[20]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0))) {
        return false;
    }
    DWORD object_len = 0;
    DWORD written = 0;
    bool ok = BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
                                               reinterpret_cast<PUCHAR>(&object_len),
                                               sizeof(object_len), &written, 0));
    std::vector<uint8_t> object(ok ? object_len : 0);
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (ok) {
        ok = BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, object.data(), object_len, nullptr, 0,
                                             0));
    }
    if (ok) {
        ok = BCRYPT_SUCCESS(BCryptHashData(
            hash, reinterpret_cast<PUCHAR>(const_cast<void*>(data)),
            static_cast<ULONG>(size), 0));
    }
    if (ok) ok = BCRYPT_SUCCESS(BCryptFinishHash(hash, out, 20, 0));
    if (hash != nullptr) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// True when a comma-separated header value contains `token`, case-insensitively.
bool has_token(std::string_view value, std::string_view token) {
    size_t at = 0;
    while (at <= value.size()) {
        const size_t comma = value.find(',', at);
        const std::string_view piece =
            trim(value.substr(at, comma == std::string_view::npos ? std::string_view::npos
                                                                 : comma - at));
        if (ascii_lower(piece) == ascii_lower(token)) return true;
        if (comma == std::string_view::npos) break;
        at = comma + 1;
    }
    return false;
}

}  // namespace

// ─── Handshake helpers ──────────────────────────────────────────────────────────────────────

std::string base64_encode(const void* data, size_t size) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* p = static_cast<const uint8_t*>(data);
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 3 <= size; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(p[i]) << 16) |
                           (static_cast<uint32_t>(p[i + 1]) << 8) | p[i + 2];
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(kAlphabet[(v >> 6) & 0x3F]);
        out.push_back(kAlphabet[v & 0x3F]);
    }
    if (i + 1 == size) {
        const uint32_t v = static_cast<uint32_t>(p[i]) << 16;
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == size) {
        const uint32_t v =
            (static_cast<uint32_t>(p[i]) << 16) | (static_cast<uint32_t>(p[i + 1]) << 8);
        out.push_back(kAlphabet[(v >> 18) & 0x3F]);
        out.push_back(kAlphabet[(v >> 12) & 0x3F]);
        out.push_back(kAlphabet[(v >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

bool compute_accept(std::string_view key, std::string& out) {
    std::string material(key);
    material += kWsGuid;
    uint8_t digest[20] = {};
    if (!sha1(material.data(), material.size(), digest)) return false;
    out = base64_encode(digest, sizeof digest);
    return true;
}

std::string_view HttpHead::get(std::string_view name) const noexcept {
    for (const auto& [k, v] : headers) {
        if (k.size() == name.size() &&
            std::equal(k.begin(), k.end(), name.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
            })) {
            return v;
        }
    }
    return {};
}

bool parse_http_head(std::string_view raw, HttpHead& out) {
    out = HttpHead{};
    const size_t blank = raw.find("\r\n\r\n");
    std::string_view head = blank == std::string_view::npos ? raw : raw.substr(0, blank);

    size_t at = 0;
    bool first = true;
    while (at <= head.size()) {
        size_t eol = head.find("\r\n", at);
        std::string_view line =
            head.substr(at, eol == std::string_view::npos ? std::string_view::npos : eol - at);
        if (first) {
            // "GET /path HTTP/1.1"
            const size_t sp1 = line.find(' ');
            if (sp1 == std::string_view::npos) return false;
            out.method = std::string(line.substr(0, sp1));
            const size_t sp2 = line.find(' ', sp1 + 1);
            out.path = std::string(line.substr(sp1 + 1, sp2 == std::string_view::npos
                                                            ? std::string_view::npos
                                                            : sp2 - sp1 - 1));
            first = false;
        } else if (!line.empty()) {
            const size_t colon = line.find(':');
            if (colon != std::string_view::npos) {
                out.headers.emplace_back(ascii_lower(trim(line.substr(0, colon))),
                                         std::string(trim(line.substr(colon + 1))));
            }
        }
        if (eol == std::string_view::npos) break;
        at = eol + 2;
    }
    return !out.method.empty();
}

bool is_upgrade_request(const HttpHead& head) {
    return has_token(head.get("upgrade"), "websocket") && !head.get("sec-websocket-key").empty();
}

bool offers_subprotocol(const HttpHead& head) {
    return has_token(head.get("sec-websocket-protocol"), kSubprotocol);
}

std::string build_upgrade_response(std::string_view accept_key, bool echo_subprotocol) {
    std::string r;
    r += "HTTP/1.1 101 Switching Protocols\r\n";
    r += "Upgrade: websocket\r\n";
    r += "Connection: Upgrade\r\n";
    r += "Sec-WebSocket-Accept: ";
    r.append(accept_key);
    r += "\r\n";
    if (echo_subprotocol) {
        r += "Sec-WebSocket-Protocol: ";
        r += kSubprotocol;
        r += "\r\n";
    }
    r += "\r\n";
    return r;
}

std::string build_bad_request_response(uint16_t port) {
    std::string body =
        "kvit-asrd, the kvit-dictate translator.\n"
        "This is a WebSocket endpoint (docs/protocol.md section 2.6), not an HTTP API.\n"
        "Point kvit-dictate at ws://127.0.0.1:" +
        std::to_string(port) + "/ instead of opening it in a browser.\n";
    std::string r;
    r += "HTTP/1.1 400 Bad Request\r\n";
    r += "Content-Type: text/plain; charset=utf-8\r\n";
    r += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    r += "Connection: close\r\n\r\n";
    r += body;
    return r;
}

// ─── Frame encoding ─────────────────────────────────────────────────────────────────────────

std::string encode_ws_frame(WsOpcode opcode, std::string_view payload, const uint8_t* mask4) {
    std::string out;
    out.reserve(payload.size() + 14);
    out.push_back(static_cast<char>(0x80 | static_cast<uint8_t>(opcode)));

    const size_t n = payload.size();
    const uint8_t mask_bit = mask4 != nullptr ? 0x80 : 0x00;
    if (n < 126) {
        out.push_back(static_cast<char>(mask_bit | static_cast<uint8_t>(n)));
    } else if (n < 65536) {
        out.push_back(static_cast<char>(mask_bit | 126));
        out.push_back(static_cast<char>((n >> 8) & 0xFF));
        out.push_back(static_cast<char>(n & 0xFF));
    } else {
        out.push_back(static_cast<char>(mask_bit | 127));
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<char>((static_cast<uint64_t>(n) >> shift) & 0xFF));
        }
    }
    if (mask4 != nullptr) {
        out.append(reinterpret_cast<const char*>(mask4), 4);
        for (size_t i = 0; i < n; ++i) {
            out.push_back(static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask4[i & 3]));
        }
    } else {
        out.append(payload);
    }
    return out;
}

// ─── WsReader ───────────────────────────────────────────────────────────────────────────────

struct WsReader::Impl {
    std::string buf;
    size_t at = 0;  // consumed prefix of `buf`, compacted lazily

    bool eof = false;
    bool errored = false;
    bool closed = false;
    uint16_t close_status = 1005;  // RFC 6455 §7.1.5: "no status code was present"
    std::string error;

    // Reassembly of a fragmented data message.
    bool in_message = false;
    WsOpcode first_opcode = WsOpcode::Binary;
    std::string body;

    std::string outbound;

    void compact() {
        if (at == 0) return;
        buf.erase(0, at);
        at = 0;
    }

    [[nodiscard]] size_t avail() const noexcept { return buf.size() - at; }

    WsStatus fail(std::string why) {
        errored = true;
        error = std::move(why);
        return WsStatus::Error;
    }
};

WsReader::WsReader() : impl_(std::make_unique<Impl>()) {}
WsReader::~WsReader() = default;

void WsReader::push(const void* data, size_t size) {
    if (size == 0) return;
    impl_->compact();
    impl_->buf.append(static_cast<const char*>(data), size);
}

void WsReader::notify_eof() noexcept { impl_->eof = true; }

const std::string& WsReader::error() const noexcept { return impl_->error; }

uint16_t WsReader::close_status() const noexcept { return impl_->close_status; }

bool WsReader::take_outbound(std::string& out) {
    if (impl_->outbound.empty()) return false;
    out.swap(impl_->outbound);
    impl_->outbound.clear();
    return true;
}

WsStatus WsReader::next(WsOpcode& opcode, std::string& payload) noexcept {
    Impl& s = *impl_;
    if (s.errored) return WsStatus::Error;
    if (s.closed) return WsStatus::Closed;

    for (;;) {
        // A frame header is 2 bytes, plus up to 8 of extended length, plus 4 of mask.
        if (s.avail() < 2) {
            if (s.eof) {
                if (s.avail() == 0 && !s.in_message) {
                    s.closed = true;
                    return WsStatus::Closed;
                }
                return s.fail("end of stream inside a WebSocket frame header");
            }
            return WsStatus::NeedMore;
        }
        const auto* p = reinterpret_cast<const uint8_t*>(s.buf.data()) + s.at;
        const bool fin = (p[0] & 0x80) != 0;
        const uint8_t rsv = p[0] & 0x70;
        const auto op = static_cast<WsOpcode>(p[0] & 0x0F);
        const bool masked = (p[1] & 0x80) != 0;
        uint64_t length = p[1] & 0x7F;
        size_t header = 2;

        if (rsv != 0) {
            // No extensions were negotiated, so a reserved bit means the peer is speaking
            // something this build does not implement. RFC 6455 §5.2.
            return s.fail("reserved bits set with no extension negotiated");
        }

        if (length == 126) {
            if (s.avail() < 4) {
                if (s.eof) return s.fail("end of stream inside a 16-bit length");
                return WsStatus::NeedMore;
            }
            length = (static_cast<uint64_t>(p[2]) << 8) | p[3];
            header = 4;
        } else if (length == 127) {
            if (s.avail() < 10) {
                if (s.eof) return s.fail("end of stream inside a 64-bit length");
                return WsStatus::NeedMore;
            }
            length = 0;
            for (int i = 0; i < 8; ++i) length = (length << 8) | p[2 + i];
            header = 10;
        }

        if (!masked) {
            // RFC 6455 §5.1. Refused rather than tolerated: a client that does not mask has not
            // read the RFC, and the next thing it gets wrong will be less visible.
            return s.fail("a client frame arrived unmasked (RFC 6455 §5.1)");
        }
        const size_t mask_at = header;
        header += 4;

        const bool control = (static_cast<uint8_t>(op) & 0x08) != 0;
        if (control && (length > 125 || !fin)) {
            // RFC 6455 §5.5: control frames are never fragmented and never exceed 125 bytes.
            return s.fail("a control frame was fragmented or oversized");
        }
        if (length > kMaxMessageBytes || s.body.size() + length > kMaxMessageBytes) {
            return s.fail("message exceeds MAX_FRAME_ON_WIRE (" +
                          std::to_string(kMaxMessageBytes) + " bytes)");
        }
        if (s.avail() < header + length) {
            if (s.eof) return s.fail("end of stream inside a WebSocket payload");
            return WsStatus::NeedMore;
        }

        // Unmask in place into a local copy. `length` is bounded above, so this allocation is
        // bounded too.
        std::string data;
        data.resize(static_cast<size_t>(length));
        const uint8_t* mask = p + mask_at;
        for (size_t i = 0; i < data.size(); ++i) {
            data[i] = static_cast<char>(p[header + i] ^ mask[i & 3]);
        }
        s.at += header + static_cast<size_t>(length);

        switch (op) {
            case WsOpcode::Close: {
                if (data.size() >= 2) {
                    s.close_status = static_cast<uint16_t>(
                        (static_cast<uint8_t>(data[0]) << 8) | static_cast<uint8_t>(data[1]));
                }
                // §2.6.4: answer a Close with a Close, then stop.
                s.outbound += encode_ws_frame(WsOpcode::Close, data.substr(0, 2), nullptr);
                s.closed = true;
                return WsStatus::Closed;
            }
            case WsOpcode::Ping:
                s.outbound += encode_ws_frame(WsOpcode::Pong, data, nullptr);
                continue;
            case WsOpcode::Pong:
                continue;
            case WsOpcode::Continuation:
                if (!s.in_message) return s.fail("continuation frame with nothing to continue");
                s.body += data;
                break;
            case WsOpcode::Text:
            case WsOpcode::Binary:
                if (s.in_message) return s.fail("a new data frame inside a fragmented message");
                s.in_message = true;
                s.first_opcode = op;
                s.body = std::move(data);
                break;
            default:
                return s.fail("unknown WebSocket opcode " +
                              std::to_string(static_cast<int>(op)));
        }

        if (fin) {
            opcode = s.first_opcode;
            payload.swap(s.body);
            s.body.clear();
            s.in_message = false;
            return WsStatus::Message;
        }
    }
}

// ─── Sockets ────────────────────────────────────────────────────────────────────────────────

namespace {

// WSAStartup/WSACleanup are refcounted per process. One instance of this guard is held by the
// listener and one by each connection, so a connection outliving its listener still has a live
// Winsock.
class WinsockGuard {
public:
    WinsockGuard() {
        WSADATA d{};
        ok_ = WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }
    ~WinsockGuard() {
        if (ok_) WSACleanup();
    }
    WinsockGuard(const WinsockGuard&) = delete;
    WinsockGuard& operator=(const WinsockGuard&) = delete;
    [[nodiscard]] bool ok() const noexcept { return ok_; }

private:
    bool ok_ = false;
};

Status wsa_failure(const char* what) {
    const int e = WSAGetLastError();
    return Status{StatusCode::Unavailable, std::string(what) + " failed",
                  static_cast<uint32_t>(e)};
}

}  // namespace

struct WsConnection::Impl {
    WinsockGuard winsock;
    SOCKET sock = INVALID_SOCKET;
    WsReader reader;
    std::mutex send_mutex;
    bool closed = false;

    bool write_all(const std::string& bytes) {
        size_t sent = 0;
        while (sent < bytes.size()) {
            const int n = ::send(sock, bytes.data() + sent,
                                 static_cast<int>(bytes.size() - sent), 0);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }
};

WsConnection::WsConnection(uintptr_t sock, std::string peer)
    : impl_(std::make_unique<Impl>()), peer_(std::move(peer)) {
    impl_->sock = static_cast<SOCKET>(sock);
}

WsConnection::~WsConnection() {
    close();
    if (impl_->sock != INVALID_SOCKET) {
        ::closesocket(impl_->sock);
        impl_->sock = INVALID_SOCKET;
    }
}

bool WsConnection::recv_message(WsOpcode& opcode, std::string& payload) {
    Impl& s = *impl_;
    for (;;) {
        // Drain first: fixture 002's lesson applies one layer up too — one read can carry
        // several WebSocket frames, and a caller that handles one per read stalls.
        const WsStatus st = s.reader.next(opcode, payload);

        std::string owed;
        if (s.reader.take_outbound(owed)) {
            std::lock_guard<std::mutex> lock(s.send_mutex);
            if (!s.closed) s.write_all(owed);
        }

        switch (st) {
            case WsStatus::Message:
                return true;
            case WsStatus::Closed:
                error_ = "peer closed the WebSocket";
                return false;
            case WsStatus::Error:
                error_ = s.reader.error();
                return false;
            case WsStatus::NeedMore:
                break;
        }

        char buf[kRecvChunk];
        const int n = ::recv(s.sock, buf, static_cast<int>(sizeof buf), 0);
        if (n > 0) {
            s.reader.push(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            s.reader.notify_eof();
            continue;
        }
        // A shutdown from another thread lands here. It is the peer being gone, not a fault.
        error_ = "socket read failed (WSA " + std::to_string(WSAGetLastError()) + ")";
        s.reader.notify_eof();
        return false;
    }
}

bool WsConnection::send_binary(std::string_view payload) {
    Impl& s = *impl_;
    const std::string frame = encode_ws_frame(WsOpcode::Binary, payload, nullptr);
    std::lock_guard<std::mutex> lock(s.send_mutex);
    if (s.closed) return false;
    return s.write_all(frame);
}

void WsConnection::close(uint16_t status) {
    Impl& s = *impl_;
    {
        std::lock_guard<std::mutex> lock(s.send_mutex);
        if (s.closed) return;
        s.closed = true;
        if (s.sock != INVALID_SOCKET) {
            char payload[2] = {static_cast<char>((status >> 8) & 0xFF),
                               static_cast<char>(status & 0xFF)};
            s.write_all(encode_ws_frame(WsOpcode::Close, std::string_view(payload, 2), nullptr));
        }
    }
    if (s.sock != INVALID_SOCKET) ::shutdown(s.sock, SD_BOTH);
}

void WsConnection::shutdown_now() {
    if (impl_->sock != INVALID_SOCKET) ::shutdown(impl_->sock, SD_BOTH);
}

// ─── WsListener ─────────────────────────────────────────────────────────────────────────────

struct WsListener::Impl {
    WinsockGuard winsock;
    SOCKET sock = INVALID_SOCKET;
    uint16_t port = 0;
    bool stopping = false;
};

WsListener::WsListener() : impl_(std::make_unique<Impl>()) {}

WsListener::~WsListener() {
    if (impl_->sock != INVALID_SOCKET) {
        ::closesocket(impl_->sock);
        impl_->sock = INVALID_SOCKET;
    }
}

Status WsListener::listen(const std::string& host, uint16_t port) {
    Impl& s = *impl_;
    if (!s.winsock.ok()) {
        return Status{StatusCode::Unavailable, "WSAStartup failed"};
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;

    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &result) != 0 || result == nullptr) {
        return Status{StatusCode::InvalidArgument,
                      "--listen host must be an address literal, not a name: " + host};
    }

    SOCKET fd = ::socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd == INVALID_SOCKET) {
        ::freeaddrinfo(result);
        return wsa_failure("socket");
    }

    // Deliberately NOT SO_REUSEADDR. On Windows that flag lets a second process steal a port
    // another is actively listening on, which for a loopback control channel is exactly the
    // squatting hazard the random pipe name avoids on the other binding. A bind failure here
    // should be a bind failure; the client retries with a fresh port (openai-translator.md §4.6).
    if (::bind(fd, result->ai_addr, static_cast<int>(result->ai_addrlen)) == SOCKET_ERROR) {
        const Status st = wsa_failure("bind");
        ::freeaddrinfo(result);
        ::closesocket(fd);
        return st;
    }
    ::freeaddrinfo(result);

    if (::listen(fd, SOMAXCONN) == SOCKET_ERROR) {
        const Status st = wsa_failure("listen");
        ::closesocket(fd);
        return st;
    }

    // Report the bound port, which is the whole point when port 0 was requested.
    sockaddr_storage bound{};
    int bound_len = sizeof bound;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
        if (bound.ss_family == AF_INET) {
            s.port = ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
        } else if (bound.ss_family == AF_INET6) {
            s.port = ntohs(reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port);
        }
    }
    if (s.port == 0) s.port = port;
    s.sock = fd;
    port_ = s.port;
    return Status::success();
}

std::unique_ptr<WsConnection> WsListener::accept(bool& retry) {
    Impl& s = *impl_;
    retry = false;
    if (s.sock == INVALID_SOCKET || s.stopping) return nullptr;

    sockaddr_storage addr{};
    int addr_len = sizeof addr;
    const SOCKET fd = ::accept(s.sock, reinterpret_cast<sockaddr*>(&addr), &addr_len);
    if (fd == INVALID_SOCKET) {
        // stop() shuts the listener down, which lands here. Not a fault.
        if (!s.stopping) {
            KD_LOG_ERROR("accept failed (WSA %d)", WSAGetLastError());
        }
        return nullptr;
    }

    char host[NI_MAXHOST] = {};
    char serv[NI_MAXSERV] = {};
    std::string peer = "?";
    if (::getnameinfo(reinterpret_cast<sockaddr*>(&addr), addr_len, host, sizeof host, serv,
                      sizeof serv, NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
        peer = std::string(host) + ":" + serv;
    }

    BOOL nodelay = TRUE;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay),
                 sizeof nodelay);

    // Read the request head. Bounded, and a peer that dribbles it is bounded by the socket's
    // own timeouts rather than by a spin here.
    std::string raw;
    for (;;) {
        if (raw.find("\r\n\r\n") != std::string::npos) break;
        if (raw.size() > kMaxHeadBytes) {
            KD_LOG_WARN("upgrade rejected from %s: request head too large", peer.c_str());
            ::closesocket(fd);
            retry = true;
            return nullptr;
        }
        char buf[4096];
        const int n = ::recv(fd, buf, static_cast<int>(sizeof buf), 0);
        if (n <= 0) {
            ::closesocket(fd);
            retry = true;
            return nullptr;
        }
        raw.append(buf, static_cast<size_t>(n));
    }

    HttpHead head;
    if (!parse_http_head(raw, head) || !is_upgrade_request(head)) {
        const std::string body = build_bad_request_response(s.port);
        ::send(fd, body.data(), static_cast<int>(body.size()), 0);
        ::shutdown(fd, SD_BOTH);
        ::closesocket(fd);
        KD_LOG_INFO("answered a plain HTTP request from %s with a 400", peer.c_str());
        retry = true;
        return nullptr;
    }

    std::string accept_key;
    if (!compute_accept(head.get("sec-websocket-key"), accept_key)) {
        KD_LOG_ERROR("BCrypt SHA-1 failed; cannot complete the WebSocket handshake");
        ::closesocket(fd);
        retry = true;
        return nullptr;
    }

    const std::string response = build_upgrade_response(accept_key, offers_subprotocol(head));
    size_t sent = 0;
    while (sent < response.size()) {
        const int n = ::send(fd, response.data() + sent,
                             static_cast<int>(response.size() - sent), 0);
        if (n <= 0) {
            ::closesocket(fd);
            retry = true;
            return nullptr;
        }
        sent += static_cast<size_t>(n);
    }

    KD_LOG_INFO("websocket upgraded from %s (path %s, subprotocol %s)", peer.c_str(),
                head.path.c_str(), offers_subprotocol(head) ? kSubprotocol : "none");

    // The head read above may have pulled the first data frames along with the request; hand
    // them to the reader rather than losing them.
    auto conn = std::unique_ptr<WsConnection>(new WsConnection(static_cast<uintptr_t>(fd),
                                                               std::move(peer)));
    const size_t body_at = raw.find("\r\n\r\n") + 4;
    if (body_at < raw.size()) {
        conn->impl_->reader.push(raw.data() + body_at, raw.size() - body_at);
    }
    return conn;
}

void WsListener::stop() {
    Impl& s = *impl_;
    s.stopping = true;
    if (s.sock != INVALID_SOCKET) {
        // Closing is what unblocks accept(); shutdown() on a listening socket is not defined to.
        ::closesocket(s.sock);
        s.sock = INVALID_SOCKET;
    }
}

}  // namespace kd::asrd
