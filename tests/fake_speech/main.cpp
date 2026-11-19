// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// kd_fake_speech — a stand-in for tools/kvit-speech, for the tests.
//
// Speaks the helper's framed protocol (tools/kvit-speech/Program.cs is the normative copy) on
// stdin/stdout with no model behind it, so `SpeechHelper` and `WindowsSpeechBackend` can be
// tested on any machine — including one without the optional Windows AI component, a .NET
// runtime or the Windows App SDK. Each flag makes it fail in one of the ways the real helper can.
//
//   (no flags)               'R', then answer every 'T' with "heard <ms> ms"
//   --answer TEXT            answer every 'T' with TEXT instead
//   --fail-start MSG         'E' MSG as the first frame, then exit 1 (the model is not installed)
//   --exit-before-ready      a line on stderr, then exit 3 with no frame at all
//   --hang-start             never send the first frame
//   --junk-stdout            write text to stdout where a frame should be
//   --delay-ms N             sleep N ms before answering each 'T'
//   --error-on-transcribe M  answer 'T' with 'E' M; the link stays usable
//   --die-on-transcribe      exit 4 on the first 'T', mid-decode
//   --count-file PATH        append one line per 'T' to PATH, so a test can count decodes

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

HANDLE g_out = nullptr;
HANDLE g_in = nullptr;

bool write_all(const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    while (size > 0) {
        DWORD n = 0;
        if (!WriteFile(g_out, p, static_cast<DWORD>(size), &n, nullptr) || n == 0) return false;
        p += n;
        size -= n;
    }
    return true;
}

bool read_exact(void* data, size_t size) {
    auto* p = static_cast<uint8_t*>(data);
    while (size > 0) {
        DWORD n = 0;
        if (!ReadFile(g_in, p, static_cast<DWORD>(size), &n, nullptr) || n == 0) return false;
        p += n;
        size -= n;
    }
    return true;
}

void frame(char tag, const std::string& payload) {
    const uint32_t len = static_cast<uint32_t>(payload.size() + 1);
    const uint8_t header[5] = {static_cast<uint8_t>(len), static_cast<uint8_t>(len >> 8),
                               static_cast<uint8_t>(len >> 16), static_cast<uint8_t>(len >> 24),
                               static_cast<uint8_t>(tag)};
    write_all(header, sizeof header);
    write_all(payload.data(), payload.size());
}

}  // namespace

int main(int argc, char** argv) {
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_in = GetStdHandle(STD_INPUT_HANDLE);

    std::string answer;
    std::string fail_start;
    std::string error_on_transcribe;
    std::string count_file;
    bool exit_before_ready = false;
    bool hang_start = false;
    bool junk_stdout = false;
    bool die_on_transcribe = false;
    DWORD delay_ms = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--answer") answer = value();
        else if (a == "--fail-start") fail_start = value();
        else if (a == "--exit-before-ready") exit_before_ready = true;
        else if (a == "--hang-start") hang_start = true;
        else if (a == "--junk-stdout") junk_stdout = true;
        else if (a == "--delay-ms") delay_ms = static_cast<DWORD>(std::strtoul(value().c_str(), nullptr, 10));
        else if (a == "--error-on-transcribe") error_on_transcribe = value();
        else if (a == "--die-on-transcribe") die_on_transcribe = true;
        else if (a == "--count-file") count_file = value();
    }

    // The real helper's model load prints this; the client must keep it out of the Info log.
    std::fputs("[loader] INT8 zp size mismatch: got 0 expected 768\n", stderr);
    std::fputs("[kvit-speech] fake helper starting\n", stderr);
    std::fflush(stderr);

    if (exit_before_ready) {
        std::fputs("[kvit-speech] startup failed: no speech component package found\n", stderr);
        std::fflush(stderr);
        return 3;
    }
    if (hang_start) {
        Sleep(INFINITE);
        return 0;
    }
    if (junk_stdout) {
        const char junk[] = "this is not a frame stream, it is a log line\n";
        write_all(junk, sizeof junk - 1);
        Sleep(5000);
        return 0;
    }
    if (!fail_start.empty()) {
        frame('E', fail_start);
        return 1;
    }
    frame('R', R"({"model":"fake-speech","proto":1})");

    for (;;) {
        uint8_t header[4];
        if (!read_exact(header, sizeof header)) return 0;  // EOF: the parent is gone
        const uint32_t len = static_cast<uint32_t>(header[0]) | (static_cast<uint32_t>(header[1]) << 8) |
                             (static_cast<uint32_t>(header[2]) << 16) |
                             (static_cast<uint32_t>(header[3]) << 24);
        if (len < 1 || len > 32u * 1024u * 1024u) return 2;
        std::vector<uint8_t> body(len);
        if (!read_exact(body.data(), body.size())) return 2;
        const char tag = static_cast<char>(body[0]);
        if (tag == 'Q') return 0;
        if (tag == 'P') {
            frame('O', "");
            continue;
        }
        if (tag != 'T') {
            frame('E', "unknown request tag");
            continue;
        }
        if (!count_file.empty()) {
            if (FILE* f = std::fopen(count_file.c_str(), "ab")) {
                std::fprintf(f, "%u\n", static_cast<unsigned>((len - 1) / 32u));
                std::fclose(f);
            }
        }
        if (die_on_transcribe) return 4;
        if (delay_ms > 0) Sleep(delay_ms);
        if (!error_on_transcribe.empty()) {
            frame('E', error_on_transcribe);
            continue;
        }
        const unsigned ms = static_cast<unsigned>((len - 1) / 32u);
        frame('O', answer.empty() ? " heard " + std::to_string(ms) + " ms " : answer);
    }
}
