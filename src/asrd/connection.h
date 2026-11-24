// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// One connection, end to end: the §5 handshake, the load that earns `ready`, and the §6 message
// loop. Everything `kvit-asrd.exe` does after `accept()` returns.
//
// **This is a separate translation unit from `main.cpp` so the tests can drive it.**
// openai-translator.md §5 asks for an in-process loopback test — `IpcClient::connect` against a
// translator listening on an ephemeral port, in the same test binary — and a serve loop that
// lived inside `main.cpp` could only be reached through `CreateProcess`. A translator whose
// handshake, error paths and session teardown are testable only by launching a process is one
// whose error paths are tested by observation, which is how `stopped` stops being emitted on
// some path and nobody notices for a release.
//
// `main.cpp` keeps the command line, the listener and the process lifetime, and nothing else.

#include <filesystem>
#include <cstdint>
#include <string>

#include "asrd/backend_nemotron.h"
#include "asrd/backend_openai.h"
#include "asrd/stability.h"
#include "asrd/ws_server.h"

namespace kd::asrd {

// Exit codes, matching `server/kvit_asr`'s so a supervisor learns one set.
inline constexpr int kExitOk = 0;
inline constexpr int kExitProtocolError = 3;
inline constexpr int kExitTransport = 4;
inline constexpr int kExitInternal = 5;

// Everything a connection needs to know. Filled from the command line in `main.cpp`; there is
// no settings file and no environment on this side (openai-translator.md §4.6: configuration
// arrives on the command line from the client that launched it).
// Which side of the translator turns audio into text. `--backend` on the command line.
enum class BackendKind : uint8_t {
    // `--service`, an OpenAI-compatible `/v1/audio/transcriptions` endpoint. The default.
    OpenAiHttp,
    // NVIDIA Nemotron Speech Streaming on this machine's CPU, through onnxruntime-genai. No
    // `--service`; the network is used once, for the model download Test asks for:
    // backend_nemotron.h.
    Nemotron,
};

struct TranslatorConfig {
    BackendKind backend = BackendKind::OpenAiHttp;
    std::string service;                      // validated by validate_service_url(); OpenAiHttp only
    std::string model = kDefaultModel;
    std::string language = kDefaultLanguage;  // empty means "omit the field, server default"
    bool finalize_only = true;
    bool vad_gate = true;
    uint32_t endpoint_silence_ms = 1500;
    StabilityParams stability;
    bool hostile_stability = false;
    uint32_t http_timeout_ms = 30000;

    // `OpenAiBackendConfig::api_key`. The one field here that does not come from a command-line
    // argument: main.cpp reads it from stdin under `--api-key-stdin`, because a command line is
    // readable by every other process running as the same user.
    std::string api_key;

    // `--backend nemotron` only. `finalize_only` above wins over the copy in here.
    NemotronConfig nemotron;

    // session_server.h `SessionServerConfig::revision_dir` — where to write the audio behind a
    // hypothesis that retracted words it had already shown the user. Empty disables it, and
    // empty is the default: the Client passes a path only when the user has enabled the corpus.
    std::filesystem::path revision_dir;
};

// Serve one connection until it ends. Returns a process exit code, which matters only for the
// single-connection case; with several, the last one wins and nobody reads it.
int serve_connection(WsConnection& conn, const TranslatorConfig& cfg);

// One WebSocket message must hold exactly one protocol frame (§2.6.3), and the redundant 4-byte
// length prefix is checked against the message size.
//
// §2.6.3 keeps that field precisely so one decoder serves both bindings, and a decoder that
// ignores a field it is given will be wrong the first time the field matters: two frames packed
// into one message would otherwise decode as a legal stream and surface later as garbled text.
bool length_agrees(const std::string& message);

}  // namespace kd::asrd
