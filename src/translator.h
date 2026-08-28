// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// Launching and supervising `kvit-asrd.exe`, the other half of this application.
//
// ═══ Yes, this reinstates process spawning, and the distinction is the whole point ══════════
//
// `release-plan.md` §1.2 deleted the spawn machinery and said of it: "There is no child to
// spawn, kill or outlive." What it removed was spawning **an arbitrary interpreter named in a
// user-editable `models.json`, with a user-supplied argv and environment**. What this adds back
// is launching **one fixed executable, by absolute path, from the installation directory**.
//
// The first is a configuration file that can execute anything. The second is two halves of one
// application. Concretely:
//
//   * `KD_ENABLE_SIDECAR_SPAWN` stays OFF in shipped builds. The named-pipe / `models.json` /
//     interpreter path is not re-enabled and the `#ifdef` around `ChildSpec` and
//     `IpcClient::start` stays exactly where it is.
//   * This path is **always compiled in**, and is not behind that flag. A shipped
//     `kvit-dictate.exe` therefore does contain `CreateProcessW` and `AssignProcessToJobObject`.
//     That is a reversal of the "attack surface a binary does not contain" sentence in
//     `packaging/windows/build-release.ps1`, and it is accepted because the alternative is the
//     Python window this whole change exists to remove.
//   * The security property that remains, and which the release script now checks instead:
//     **the image path is `GetModuleFileName(nullptr)`'s directory plus `kvit-asrd.exe`, and
//     nothing else.** There is no setting, no environment variable and no manifest that can
//     point it somewhere else. If that file is missing, the tray says so and nothing is
//     launched.
//   * The user's speech-service URL is **data on a command line this program constructs** — it
//     is user-supplied *content*, not a user-supplied argv vector — and it is validated as
//     `http://` or `https://` with no spaces, quotes or control characters before it is quoted
//     onto `--service`.
//
// ═══ The port ═══════════════════════════════════════════════════════════════════════════════
//
// **The client chooses it.** Bind `127.0.0.1:0`, `getsockname`, close, and pass
// `--listen 127.0.0.1:N`. There is a small TOCTOU window between the close and the child's
// bind; on loopback that is acceptable, and if the child exits because the bind failed the
// supervisor retries a handful of times with a fresh port.
//
// A fixed `10402` is deliberately not used for the supervised case: two clients, or a leftover
// Python server, would collide — which is not hypothetical, because a Python server on that
// port is exactly what this replaces.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "util/result.h"

namespace kd {

// What the client tells the translator. Everything else is the translator's compiled default;
// the client deliberately does not expose N/M/T, `--hop-ms` or `--window-ms`, because the server
// owns the published mark and a second place to set those numbers is one too many
// (openai-translator.md §4.3).
struct TranslatorSpec {
    // The user's speech service. Validated with `validate_speech_service_url()` before it gets
    // anywhere near a command line.
    std::string service_url;

    // Optional. Empty means the translator's compiled default, which is also what `hello.model`
    // being absent means on the wire.
    std::string model;

    // Live preview: `partial`s with a computed `stable_bytes`, against a model that
    // structurally violates monotonicity. Off by default and labelled in the dialog
    // (openai-translator.md §4.3's batch-model caveat).
    bool live_preview = false;

    uint32_t endpoint_silence_ms = 1500;
};

// One supervised `kvit-asrd.exe`. Not copyable; owns a job object, a process and two threads.
class Translator {
public:
    Translator();
    ~Translator();
    Translator(const Translator&) = delete;
    Translator& operator=(const Translator&) = delete;

    // Where `kvit-asrd.exe` must be: this executable's own directory. Fails with a message
    // naming the absolute path when it is not there, because that is a broken installation and
    // the user needs to see which file is missing.
    static Result<std::filesystem::path> image_path();

    // Choose a port, launch, and supervise. Returns the `ws://127.0.0.1:N/` URL to connect to.
    //
    // Returns as soon as the child is running and its port is known — it does **not** wait for
    // the child to answer, because `ready` can take as long as the speech host takes to accept
    // a TCP connection, and blocking the UI thread on that is the freeze this project has spent
    // some effort avoiding elsewhere.
    Result<std::string> start(const TranslatorSpec& spec);

    // Kill the child and wait for it. Idempotent, and safe on a teardown path where our own
    // state is uncertain — the same reason `stop`/`cancel` are idempotent on the wire (§6.1).
    void stop();

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] uint16_t port() const noexcept;
    [[nodiscard]] uint32_t pid() const noexcept;

    // The url start() returned, or empty. Shown on the Settings → Server page while the child is
    // up, because "which port is it on" is the first support question.
    [[nodiscard]] std::string url() const;

    // Called on a background thread when the child exits without being asked to. The tray uses
    // it to say so; `App::on_sidecar_lost` already knows how to keep committed text and lose
    // only the tail, and a dead translator is that same event.
    void set_on_exit(std::function<void(uint32_t exit_code)> cb);

private:
    // One attempt on one port. `start()` is this plus the retry over the TOCTOU window.
    Status launch(const std::filesystem::path& image, const TranslatorSpec& spec, uint16_t port);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Pick a free loopback TCP port by binding one and letting it go. Exposed because the retry
// logic in start() is worth testing without launching anything.
Result<uint16_t> pick_loopback_port();

// The exact argv the client builds for a spec and port, as one command line, quoted.
//
// Exposed so a test can assert what actually reaches `CreateProcessW`: this is the argument
// vector the security argument above depends on, and reviewing it in a test is better than
// reviewing it in a comment.
std::wstring build_translator_command_line(const std::filesystem::path& image,
                                           const TranslatorSpec& spec, uint16_t port,
                                           uint32_t parent_pid);

}  // namespace kd
