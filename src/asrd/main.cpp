// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// kvit-asrd — the translator. `docs/protocol.md` on one side, an OpenAI-compatible HTTP speech
// service on the other. See openai-translator.md for why this exists at all.
//
// The short version: the protocol boundary was the right call and stays. Making the *other side
// of it* the user's problem was not. Before this binary, a user who had installed one executable
// and named their endpoint still had to keep a Python window open, because the endpoint on their
// network speaks `/v1/audio/transcriptions` and not this protocol. This program is the half that
// was missing.
//
// ─── Layout ─────────────────────────────────────────────────────────────────────────────────
//
//   ws_server.cpp        L0: TCP, RFC 6455.                                    NEW
//   protocol.cpp         L1/L2: framing and the message codec.                 REUSED, verbatim
//   session_server.cpp   L3: start/stop/cancel, utterances, emission.          PORTED
//   vad.cpp              endpointing.                                          PORTED
//   stability.cpp        hypotheses -> stable_bytes.                           PORTED
//   backend_openai.cpp   PCM -> text, over HTTP.                               PORTED
//   backend_nemotron.cpp PCM -> text on this machine's CPU, streaming.         NEW
//
// This file is the run loop and the handshake, and nothing else.

#define KD_LOG_MODULE "asrd"

#include <filesystem>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "asrd/backend_nemotron.h"
#include "asrd/backend_openai.h"
#include "asrd/connection.h"
#include "asrd/http.h"
#include "model_download.h"
#include "asrd/power.h"
#include "asrd/ws_server.h"
#include "protocol.h"
#include "util/log.h"

namespace kd::asrd {
namespace {

// ─── Command line (openai-translator.md §4.7) ───────────────────────────────────────────────

// The process's own settings. Everything one *connection* needs is in `translator`, which is
// `connection.h`'s type and is what serve_connection() is handed.
struct Options {
    std::string listen_host = "127.0.0.1";
    uint16_t listen_port = kDefaultPort;
    uint32_t parent_pid = 0;
    uint32_t max_connections = 1;
    kd::log::Level log_level = kd::log::Level::Info;
    bool api_key_stdin = false;
    // `--nemotron-download`: fetch and check the model, then exit. No listener.
    bool download_only = false;
    TranslatorConfig translator;
};

void print_usage() {
    std::fputs(
        "kvit-asrd - the kvit-dictate translator.\n"
        "\n"
        "Speaks docs/protocol.md on a loopback WebSocket, and turns audio into text with an\n"
        "OpenAI-compatible /v1/audio/transcriptions service over HTTP, or with NVIDIA Nemotron\n"
        "on this machine's CPU.\n"
        "\n"
        "  kvit-asrd --service http://host:port [--listen 127.0.0.1:10402] [options]\n"
        "  kvit-asrd --backend nemotron [--listen 127.0.0.1:10402] [options]\n"
        "  kvit-asrd --nemotron-download [--nemotron-model DIR]\n"
        "\n"
        "  --listen [host:]port        default 127.0.0.1:10402. Loopback unless a host is given.\n"
        "  --backend NAME              openai-http (default) or nemotron.\n"
        "  --service URL               REQUIRED for openai-http. http:// or https:// only.\n"
        "  --nemotron-runtime DIR     nemotron: the folder holding onnxruntime-genai.dll.\n"
        "                              Default nemotron\\ beside this program.\n"
        "  --nemotron-model DIR        nemotron: the model folder. Default\n"
        "                              %LOCALAPPDATA%\\kvit-dictate\\models\\nemotron-...-v3.\n"
        "  --nemotron-install-model    nemotron: download the model (728 MB, from Microsoft's\n"
        "                              model catalog) if it is not there.\n"
        "  --nemotron-download         download and check the model, then exit.\n"
        "  --model NAME                default Qwen/Qwen3-ASR-1.7B\n"
        "  --language LANG             default en. Empty means the server's default.\n"
        "  --parent-pid PID            exit when that process does.\n"
        "  --max-connections N         default 1. 0 = unlimited. Extras get server_busy.\n"
        "  --finalize-only on|off      default on. Off is live preview, and is labelled.\n"
        "  --vad-gate on|off           default on. Fails open.\n"
        "  --api-key-stdin             read an API key from the first line of stdin and send it\n"
        "                              as Authorization: Bearer. The key is never an argument:\n"
        "                              other processes can read a command line.\n"
        "  --endpoint-silence-ms MS    default 1500\n"
        "  --stability-n N             default 2\n"
        "  --stability-m M             default 3\n"
        "  --stability-hold-ms T       default 200\n"
        "  --hostile-stability         deliberately non-conforming. Test use only.\n"
        "  --log-level LEVEL           trace|debug|info|warn|error. Default info.\n"
        "  --version, --help\n",
        stderr);
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr,
                                      0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) {
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n,
                            nullptr, nullptr);
    }
    return s;
}

bool parse_bool(const std::string& v, bool& out) {
    if (v == "on" || v == "1" || v == "true" || v == "yes") { out = true; return true; }
    if (v == "off" || v == "0" || v == "false" || v == "no") { out = false; return true; }
    return false;
}

bool parse_u32(const std::string& v, uint32_t& out) {
    if (v.empty()) return false;
    char* end = nullptr;
    const unsigned long n = std::strtoul(v.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || n > 0xFFFFFFFFul) return false;
    out = static_cast<uint32_t>(n);
    return true;
}

// `[host:]port`. A bare number is a port on loopback; anything else must be an address literal,
// because a name would have to be resolved and a translator that binds "whatever that name
// resolves to" is a translator that occasionally binds the wrong interface.
bool parse_listen(const std::string& v, std::string& host, uint16_t& port) {
    const size_t colon = v.rfind(':');
    std::string port_text = v;
    if (colon != std::string::npos) {
        host = v.substr(0, colon);
        port_text = v.substr(colon + 1);
        // An IPv6 literal is written [::1]:10402; strip the brackets for getaddrinfo.
        if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
            host = host.substr(1, host.size() - 2);
        }
    }
    uint32_t n = 0;
    if (!parse_u32(port_text, n) || n > 65535) return false;
    port = static_cast<uint16_t>(n);
    return true;
}

// Returns false when the program should exit; `exit_code` says with what.
bool parse_args(int argc, char** argv, Options& opt, int& exit_code) {
    exit_code = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "kvit-asrd: %s needs a value\n", what);
                exit_code = 2;
                return {};
            }
            return argv[++i];
        };
        if (a == "--help" || a == "-h") {
            print_usage();
            return false;
        }
        if (a == "--version") {
#ifdef KVIT_VERSION_RAW
#define KD_STR2(x) #x
#define KD_STR(x) KD_STR2(x)
            std::fprintf(stderr, "kvit-asrd %s (proto %d)\n", KD_STR(KVIT_VERSION_RAW),
                         proto::kProtoVersion);
#undef KD_STR
#undef KD_STR2
#else
            std::fprintf(stderr, "kvit-asrd (proto %d)\n", proto::kProtoVersion);
#endif
            return false;
        }
        if (a == "--listen") {
            const std::string v = next("--listen");
            if (exit_code != 0) return false;
            if (!parse_listen(v, opt.listen_host, opt.listen_port)) {
                std::fprintf(stderr, "kvit-asrd: --listen wants [host:]port, got %s\n", v.c_str());
                exit_code = 2;
                return false;
            }
            continue;
        }
        if (a == "--backend") {
            const std::string v = next("--backend");
            if (exit_code != 0) return false;
            if (v == "openai-http") {
                opt.translator.backend = BackendKind::OpenAiHttp;
            } else if (v == "nemotron") {
                opt.translator.backend = BackendKind::Nemotron;
            } else {
                // `windows-speech` was removed on 2026-11-23, with the helper it drove.
                std::fprintf(stderr, "kvit-asrd: --backend wants openai-http or nemotron, got %s\n",
                             v.c_str());
                exit_code = 2;
                return false;
            }
            continue;
        }
        if (a == "--nemotron-runtime" || a == "--nemotron-model") {
            const std::string v = next(a.c_str());
            if (exit_code != 0) return false;
            (a == "--nemotron-runtime" ? opt.translator.nemotron.runtime_dir
                                       : opt.translator.nemotron.model_dir) =
                std::filesystem::path(widen(v));
            continue;
        }
        if (a == "--nemotron-install-model") {
            opt.translator.nemotron.install_model = true;
            continue;
        }
        if (a == "--nemotron-download") {
            opt.download_only = true;
            continue;
        }
        if (a == "--service") {
            opt.translator.service = next("--service");
            if (exit_code != 0) return false;
            continue;
        }
        if (a == "--model") {
            opt.translator.model = next("--model");
            if (exit_code != 0) return false;
            continue;
        }
        if (a == "--language") {
            opt.translator.language = next("--language");
            if (exit_code != 0) return false;
            continue;
        }
        if (a == "--parent-pid") {
            const std::string v = next("--parent-pid");
            if (exit_code != 0) return false;
            if (!parse_u32(v, opt.parent_pid)) {
                std::fprintf(stderr, "kvit-asrd: --parent-pid wants a number\n");
                exit_code = 2;
                return false;
            }
            continue;
        }
        if (a == "--max-connections") {
            const std::string v = next("--max-connections");
            if (exit_code != 0) return false;
            if (!parse_u32(v, opt.max_connections)) {
                std::fprintf(stderr, "kvit-asrd: --max-connections wants a number\n");
                exit_code = 2;
                return false;
            }
            continue;
        }
        if (a == "--finalize-only" || a == "--vad-gate") {
            // Both take an explicit on/off rather than being bare switches, because both default
            // to ON and a bare `--finalize-only` would then read as if it changed something.
            const std::string v = next(a.c_str());
            if (exit_code != 0) return false;
            bool value = true;
            if (!parse_bool(v, value)) {
                std::fprintf(stderr, "kvit-asrd: %s wants on or off, got %s\n", a.c_str(),
                             v.c_str());
                exit_code = 2;
                return false;
            }
            (a == "--finalize-only" ? opt.translator.finalize_only : opt.translator.vad_gate) = value;
            continue;
        }
        if (a == "--revision-dir") {
            // Where to write the audio behind a hypothesis that took back words the user had
            // already read. See SessionServerConfig::revision_dir for why the Client's own
            // corpus cannot capture this.
            const std::string v = next(a.c_str());
            if (exit_code != 0) return false;
            opt.translator.revision_dir = std::filesystem::path(v);
            continue;
        }
        if (a == "--endpoint-silence-ms" || a == "--stability-n" || a == "--stability-m" ||
            a == "--stability-hold-ms") {
            const std::string v = next(a.c_str());
            if (exit_code != 0) return false;
            uint32_t value = 0;
            if (!parse_u32(v, value)) {
                std::fprintf(stderr, "kvit-asrd: %s wants a number, got %s\n", a.c_str(),
                             v.c_str());
                exit_code = 2;
                return false;
            }
            if (a == "--endpoint-silence-ms") opt.translator.endpoint_silence_ms = value;
            else if (a == "--stability-n") opt.translator.stability.hypotheses_n = value;
            else if (a == "--stability-m") opt.translator.stability.retract_clusters_m = value;
            else opt.translator.stability.hold_ms_t = value;
            continue;
        }
        if (a == "--api-key-stdin") {
            opt.api_key_stdin = true;
            continue;
        }
        if (a == "--hostile-stability") {
            opt.translator.hostile_stability = true;
            continue;
        }
        if (a == "--log-level") {
            const std::string v = next("--log-level");
            if (exit_code != 0) return false;
            if (!kd::log::parse_level(v, opt.log_level)) {
                std::fprintf(stderr, "kvit-asrd: --log-level wants trace|debug|info|warn|error\n");
                exit_code = 2;
                return false;
            }
            continue;
        }
        std::fprintf(stderr, "kvit-asrd: unknown option %s\n", a.c_str());
        print_usage();
        exit_code = 2;
        return false;
    }

    // There is deliberately no --api-key that takes a value: the key arrives on stdin under
    // --api-key-stdin, because every process running as this user can read a command line. And
    // still no --silero-model; openai-translator.md §1.4 records why it is a separate, argued
    // addition. `--backend` was one too, and backend_nemotron.h is the argument.
    if (opt.download_only) return true;
    if (opt.translator.backend == BackendKind::Nemotron) {
        if (!opt.translator.service.empty()) {
            std::fprintf(stderr,
                         "kvit-asrd: --service has no meaning with --backend nemotron; the model "
                         "runs on this machine\n");
            exit_code = 2;
            return false;
        }
        // One model, and its name is what `hello.model` must match. A `--model` that said
        // anything else would be a promise this backend cannot keep.
        opt.translator.model = kNemotronModel;
        return true;
    }
    const std::string why = validate_service_url(opt.translator.service);
    if (!why.empty()) {
        std::fprintf(stderr, "kvit-asrd: %s\n", why.c_str());
        print_usage();
        exit_code = 2;
        return false;
    }
    return true;
}

// The first line of stdin, which is where the client puts the API key (translator.cpp writes it
// into a pipe and closes it). A console works too, for somebody running this by hand — the key
// echoes there, which is their terminal and their call.
bool read_api_key(std::string& key, std::string& why) {
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (in == nullptr || in == INVALID_HANDLE_VALUE) {
        why = "--api-key-stdin was given but this process has no stdin";
        return false;
    }
    std::string buf;
    for (;;) {
        char chunk[256];
        DWORD n = 0;
        if (!ReadFile(in, chunk, sizeof chunk, &n, nullptr) || n == 0) break;
        buf.append(chunk, n);
        SecureZeroMemory(chunk, sizeof chunk);
        if (buf.find('\n') != std::string::npos || buf.size() > 4096) break;
    }
    const size_t eol = buf.find_first_of("\r\n");
    key.assign(buf, 0, eol == std::string::npos ? buf.size() : eol);
    SecureZeroMemory(buf.data(), buf.size());
    if (key.empty()) {
        why = "--api-key-stdin was given but stdin held no key";
        return false;
    }
    if (const std::string bad = api_key_problem(key); !bad.empty()) {
        SecureZeroMemory(key.data(), key.size());
        key.clear();
        why = "--api-key-stdin: " + bad;
        return false;
    }
    return true;
}

// ─── Parent liveness ────────────────────────────────────────────────────────────────────────

std::atomic<bool>* g_shutdown = nullptr;
WsListener* g_listener = nullptr;

BOOL WINAPI console_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        if (g_shutdown != nullptr) g_shutdown->store(true);
        if (g_listener != nullptr) g_listener->stop();
        return TRUE;
    }
    return FALSE;
}

// Watch the parent and exit when it dies.
//
// The job object the client puts this process in is the real guarantee — it is what makes an
// orphan impossible even if this process is wedged — and this watch is the backup for the case
// where the child somehow ends up outside that job. `kd_probe` and standalone use omit
// `--parent-pid` and this thread is never started.
void watch_parent(uint32_t pid, std::atomic<bool>& shutdown, WsListener& listener) {
    const HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (h == nullptr) {
        KD_LOG_WARN("cannot open parent pid %u to watch it (error %lu); relying on the job object",
                    pid, GetLastError());
        return;
    }
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    KD_LOG_INFO("parent pid %u exited; shutting down", pid);
    shutdown.store(true);
    listener.stop();
}

int run(int argc, char** argv) {
    Options opt;
    int exit_code = 0;
    if (!parse_args(argc, argv, opt, exit_code)) return exit_code;
    if (opt.api_key_stdin) {
        std::string why;
        if (!read_api_key(opt.translator.api_key, why)) {
            std::fprintf(stderr, "kvit-asrd: %s\n", why.c_str());
            return 2;
        }
    }

    kd::log::Config log_cfg;
    log_cfg.level = opt.log_level;
    // stderr and nothing else. Supervised, the parent redirects this process's stderr into the
    // client's log; standalone, it is the console. No file: a separate asrd.log would split the
    // first support question across two files.
    log_cfg.to_stderr = true;
    log_cfg.to_debugger = false;
    kd::log::init(log_cfg);

    // power.h: a window-less decoder is exactly what Windows throttles, and the user is waiting.
    if (!opt_out_of_power_throttling()) {
        KD_LOG_WARN("Windows refused to exempt this process from power throttling (error %lu); "
                    "on-device decoding may run on efficiency cores",
                    GetLastError());
    }

    if (opt.download_only) {
        const std::filesystem::path dir = opt.translator.nemotron.model_dir.empty()
                                              ? default_nemotron_model_dir()
                                              : opt.translator.nemotron.model_dir;
        const CatalogModel& model = nemotron_en_catalog_model();
        std::atomic<bool> never{false};
        int last_percent = -1;
        Status st = download_catalog_model(
            model, dir,
            [&last_percent](uint64_t done, uint64_t total) {
                const int percent = total == 0 ? 100 : static_cast<int>(done * 100 / total);
                if (percent / 5 != last_percent / 5) {
                    last_percent = percent;
                    KD_LOG_INFO("downloaded %llu of %llu MB (%d%%)",
                                static_cast<unsigned long long>(done / 1000000),
                                static_cast<unsigned long long>(total / 1000000), percent);
                }
            },
            never);
        // A folder that was already there was only checked by size; this is the full check.
        if (st.ok()) st = verify_model_dir(model, dir, true);
        if (!st.ok()) {
            KD_LOG_ERROR("%s", st.to_string().c_str());
            kd::log::shutdown();
            return kExitInternal;
        }
        KD_LOG_INFO("the Nemotron model is in %s and every file matches its SHA-256",
                    narrow(dir.wstring()).c_str());
        kd::log::shutdown();
        return kExitOk;
    }

    if (opt.translator.backend == BackendKind::Nemotron) {
        const NemotronConfig& n = opt.translator.nemotron;
        KD_LOG_INFO("kvit-asrd starting: backend=nemotron runtime=%s model=%s %s install_model=%s",
                    narrow((n.runtime_dir.empty() ? default_nemotron_runtime_dir() : n.runtime_dir)
                               .wstring())
                        .c_str(),
                    narrow((n.model_dir.empty() ? default_nemotron_model_dir() : n.model_dir)
                               .wstring())
                        .c_str(),
                    opt.translator.finalize_only ? "finalize-only" : "live-preview",
                    n.install_model ? "yes" : "no");
    } else {
        KD_LOG_INFO("kvit-asrd starting: service=%s model=%s %s vad_gate=%s api_key=%s",
                    opt.translator.service.c_str(), opt.translator.model.c_str(),
                    opt.translator.finalize_only ? "finalize-only" : "live-preview",
                    opt.translator.vad_gate ? "on" : "off",
                    opt.translator.api_key.empty() ? "none" : "set");
    }
    if (opt.translator.hostile_stability) {
        KD_LOG_ERROR(
            "--hostile-stability is on. This build deliberately emits non-conforming "
            "stable_bytes and exists only to exercise docs/protocol.md §8.3. Do not ship it.");
    }

    WsListener listener;
    if (const Status st = listener.listen(opt.listen_host, opt.listen_port); !st.ok()) {
        KD_LOG_ERROR("cannot listen on %s:%u — %s", opt.listen_host.c_str(), opt.listen_port,
                     st.to_string().c_str());
        std::fprintf(stderr, "kvit-asrd: cannot listen on %s:%u — %s\n", opt.listen_host.c_str(),
                     opt.listen_port, st.to_string().c_str());
        kd::log::shutdown();
        return kExitTransport;
    }
    // Logged rather than merely known: the client chooses the port for the supervised case, and
    // this line is what a support question is answered from.
    KD_LOG_INFO("listening on ws://%s:%u/", opt.listen_host.c_str(), listener.port());

    std::atomic<bool> shutdown{false};
    g_shutdown = &shutdown;
    g_listener = &listener;
    SetConsoleCtrlHandler(console_handler, TRUE);

    std::thread parent_watch;
    if (opt.parent_pid != 0) {
        parent_watch = std::thread(
            [&] { watch_parent(opt.parent_pid, shutdown, listener); });
        parent_watch.detach();
    }

    int last = kExitOk;
    std::atomic<uint32_t> live{0};
    std::vector<std::thread> connections;
    while (!shutdown.load()) {
        bool retry = false;
        std::unique_ptr<WsConnection> conn = listener.accept(retry);
        if (conn == nullptr) {
            if (retry) continue;
            break;
        }

        const bool admitted = opt.max_connections == 0 || live.load() < opt.max_connections;
        if (!admitted) {
            // §2.7: refused **in words**, not in silence. A connection that never becomes ready
            // and a socket that hangs up both present to the user as "it hangs".
            KD_LOG_WARN("refusing a connection from %s: --max-connections %u is reached",
                        conn->peer().c_str(), opt.max_connections);
            proto::ErrorMsg e;
            e.code = proto::ErrorCode::server_busy;
            e.code_raw = proto::to_string(e.code);
            e.fatal = true;
            e.msg = "this translator serves one connection at a time";
            e.retry_after_ms = 1000;
            const std::vector<uint8_t> frame =
                proto::encode_control_frame(proto::Message::of(std::move(e)));
            conn->send_binary(std::string_view(reinterpret_cast<const char*>(frame.data()),
                                               frame.size()));
            conn->close();
            continue;
        }

        if (opt.max_connections == 1) {
            // The common case, and worth keeping on this thread: with one connection there is
            // nothing to overlap, and a thread per connection would only make the exit code and
            // the shutdown path harder to follow.
            ++live;
            last = serve_connection(*conn, opt.translator);
            --live;
            conn->close();
            continue;
        }

        ++live;
        connections.emplace_back([&, c = std::move(conn)]() mutable {
            serve_connection(*c, opt.translator);
            c->close();
            --live;
        });
    }

    listener.stop();
    for (std::thread& t : connections) {
        if (t.joinable()) t.join();
    }
    // After the last connection: the runtime's globals must not be left to static destruction.
    release_nemotron_engines();
    KD_LOG_INFO("kvit-asrd exiting (%d)", last);
    kd::log::shutdown();
    return last;
}

}  // namespace
}  // namespace kd::asrd

int main(int argc, char** argv) { return kd::asrd::run(argc, argv); }
