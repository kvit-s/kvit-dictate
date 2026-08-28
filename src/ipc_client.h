// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// The named-pipe transport: pipe creation with an explicit DACL, child spawn under a job
// object, peer validation, overlapped framed I/O, bounded queues.
//
// Implements: docs/protocol.md §2 (transport) and §9 (backpressure), plan.md §1.2 (security).
// The framing and message codec live in protocol.h and are pure; this file is the Windows half
// and is deliberately the only place in the project that opens a pipe or spawns a process.
//
// ─── Role naming ────────────────────────────────────────────────────────────────────────────
//
// This class is the **pipe server** (it creates the instance) and the **protocol client** (it
// drives sessions). The inversion is deliberate: only the parent can generate a per-launch
// name, apply a DACL before anyone can connect, and know the child's PID for peer validation.
// The class is named for its protocol role, which is the one the rest of the program cares
// about. See docs/protocol.md §1.
//
// ─── The security requirements, none of which are optional ──────────────────────────────────
//
// Revision 1 of the plan claimed a named pipe's default ACL restricts it to the current user.
// **That is false** — Microsoft documents that the default descriptor grants full control to
// LocalSystem, administrators and the creator owner, and read access to Everyone and to the
// anonymous account. docs/protocol.md §2.3 has the correction; the required construction is:
//
//   1. An explicit DACL built on the **logon SID** (`GetTokenInformation(TokenGroups)`, the
//      group whose attributes include SE_GROUP_LOGON_ID) — not the user SID, which does not
//      separate concurrent terminal-services sessions of the same user.
//   2. Exactly one allow-ACE granting only FILE_READ_DATA | FILE_WRITE_DATA |
//      FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | SYNCHRONIZE. Not FILE_GENERIC_WRITE,
//      which implies FILE_CREATE_PIPE_INSTANCE and would let a client create rival instances.
//   3. `SetSecurityDescriptorDacl(sd, TRUE, dacl, FALSE)`. **An explicit empty DACL denies
//      everyone; a NULL DACL grants everyone.** The difference is one argument and the failure
//      is silent, which is why SecurityDescriptorBuilder below exposes its result for a unit
//      test to assert rather than hiding it inside start().
//   4. A per-launch unpredictable name: `\\.\pipe\kvit-dictate-<32 lowercase hex>` from
//      BCryptGenRandom(BCRYPT_USE_SYSTEM_PREFERRED_RNG). The name is not a secret — command
//      lines are readable by other processes as the same user — it defeats *squatting*, and
//      FILE_FLAG_FIRST_PIPE_INSTANCE turns a squatting attempt into ERROR_ACCESS_DENIED.
//   5. Peer validation with GetNamedPipeClientProcessId against the PID CreateProcess returned.
//      PID recycling is not a hole: the child's process handle is held open for its whole
//      lifetime, and Windows does not recycle a PID while a handle to that process is open.
//
// What this does **not** defend against, stated because claiming it would be dishonest: another
// process running as the same user in the same logon session. Such a process can already read
// our command line, open our process, and inject code. Integrity level partially helps for
// writes (NO_WRITE_UP) and not at all for reads.
//
// ─── Threading ──────────────────────────────────────────────────────────────────────────────
//
// One dedicated I/O thread owns the pipe handle and both overlapped operations.
//
//   * **Callbacks** (`Callbacks::on_message`, `on_error`, `on_disconnected`) are invoked **on
//     the I/O thread**. They must not block and must not call back into IpcClient except for
//     `send_*`. The intended implementation posts to the UI thread — SessionMachine and
//     CommitEngine are single-threaded by design (see session.h) and must not be touched from
//     here.
//   * **`send_control()` and `send_audio()` are thread-safe.** `send_audio()` is called from
//     the WASAPI capture thread and must never block it: it enqueues and returns. This is why
//     the pipe is opened FILE_FLAG_OVERLAPPED — a wedged peer must not be able to stall the
//     audio thread past WRITE_DEADLINE_MS.
//   * `start()` and `stop()` are called from the UI thread only. `stop()` joins the I/O thread.
//
// ─── Backpressure: fail loudly, never drop ──────────────────────────────────────────────────
//
// docs/protocol.md §9 and plan.md §1.2: silently dropping or delaying audio produces
// mysteriously bad transcription and is not acceptable. Both send queues are bounded; on
// overflow the connection reports `overload_audio` / `overload_control` (fatal) and tears down.
// **No path in this module may drop an audio frame to relieve pressure** — not the newest, not
// the oldest, not every other one.

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "protocol.h"
#include "util/result.h"

namespace kd {

// ─── Pure, testable helpers ─────────────────────────────────────────────────────────────────
//
// These are pure functions on purpose: the name format and the ACL shape are the two places a
// silent mistake would cost security, and both are asserted by unit tests.

// `\\.\pipe\kvit-dictate-<32 lowercase hex digits>` from 16 random bytes.
std::wstring format_pipe_name(const uint8_t random16[16]);

// Draws 16 bytes from BCryptGenRandom with BCRYPT_USE_SYSTEM_PREFERRED_RNG and formats them.
// Fails rather than falling back to rand()/GetTickCount — a predictable name is a squattable
// name, and a degraded fallback would make that failure invisible.
Result<std::wstring> generate_pipe_name();

// ─── The access mask, spelled once ──────────────────────────────────────────────────────────
//
// docs/protocol.md §2.3 item 2: the single allow-ACE grants exactly
//
//     FILE_READ_DATA (0x0001) | FILE_WRITE_DATA (0x0002) | FILE_READ_ATTRIBUTES (0x0080) |
//     FILE_WRITE_ATTRIBUTES (0x0100) | SYNCHRONIZE (0x00100000)
//
// and nothing else. The numeric value is written here rather than as an expression over the SDK
// macros because **no header under src/ includes <windows.h>** — that is what keeps the pure
// modules testable. ipc_client.cpp static_asserts this value against the real macros, so the two
// spellings cannot drift.
//
// FILE_GENERIC_WRITE is deliberately NOT used: it is 0x00120116 and includes
// FILE_CREATE_PIPE_INSTANCE (0x0004), which would let a client create rival instances of our
// pipe name.
inline constexpr uint32_t kPipeAccessRights = 0x00100183u;
inline constexpr uint32_t kFileCreatePipeInstance = 0x00000004u;

// **Consequence for the sidecar, measured on this machine and easy to get wrong.** A DACL this
// narrow denies `CreateFileW(name, GENERIC_READ | GENERIC_WRITE, …)` with ERROR_ACCESS_DENIED,
// because FILE_GENERIC_READ (0x00120089) additionally asks for FILE_READ_EA and READ_CONTROL,
// which we do not grant. The pipe client must request exactly `kPipeAccessRights`. This is a
// property of the security decision, not a bug in it: granting the extra bits to make the naive
// open work would mean granting rights nobody needs.

// ─── The security descriptor, exposed so a test can assert it ───────────────────────────────
//
// docs/protocol.md §2.3 asks for this assertion by name: "An explicit empty DACL denies
// everyone; a NULL DACL grants everyone. The difference is one argument and the failure is
// silent, so this is asserted in a unit test." A builder that hid its result inside start()
// could only be checked by observing that connections work — which an all-permitting NULL DACL
// also achieves.
//
// The descriptor points at the ACL, and the SECURITY_ATTRIBUTES points at the descriptor, so the
// object is self-referential and therefore neither copyable nor movable. It must outlive the
// CreateNamedPipeW call that consumes it (only that call; the kernel object keeps its own copy).
class SecurityDescriptorBuilder {
public:
    SecurityDescriptorBuilder();
    ~SecurityDescriptorBuilder();
    SecurityDescriptorBuilder(const SecurityDescriptorBuilder&) = delete;
    SecurityDescriptorBuilder& operator=(const SecurityDescriptorBuilder&) = delete;
    SecurityDescriptorBuilder(SecurityDescriptorBuilder&&) = delete;
    SecurityDescriptorBuilder& operator=(SecurityDescriptorBuilder&&) = delete;

    // Builds the descriptor: logon SID from the process token, one allow-ACE with
    // kPipeAccessRights, SetSecurityDescriptorDacl(sd, TRUE, dacl, FALSE).
    Status build();

    [[nodiscard]] bool is_built() const noexcept;

    // False means the logon SID was not found and the **weaker** user-SID fallback was used —
    // see the comment on the fallback in ipc_client.cpp. A caller that cares (the tray, a test)
    // can surface it; nothing silently depends on it.
    [[nodiscard]] bool used_logon_sid() const noexcept;

    // The SID the single ACE names, in SDDL form ("S-1-5-5-0-4779726"). Diagnostics and tests.
    [[nodiscard]] std::string sid_string() const;

    // A `SECURITY_ATTRIBUTES*` as an integer, because this header must not name Windows types.
    // Zero until build() succeeds. Valid while this object lives.
    [[nodiscard]] uintptr_t security_attributes() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

// Creates the pipe-server instance with exactly the parameter set in docs/protocol.md §2.2.
// Returns the `HANDLE` as an integer; **the caller owns it and must CloseHandle it.**
//
// Exposed rather than buried in start() for one reason: the DACL assertion in tests/test_ipc.cpp
// reads the descriptor back off a real kernel object with GetSecurityInfo. Asserting the
// builder's own buffers would only prove the builder agrees with itself, and would not catch a
// descriptor that was built correctly and then never passed to CreateNamedPipeW.
//
// A name that already exists fails with StatusCode::PermissionDenied and the message
// `pipe_squatted` — FILE_FLAG_FIRST_PIPE_INSTANCE turns a squatting attempt into a loud error
// instead of a silent second instance (docs §2.2, §11.3 final paragraph).
Result<uintptr_t> create_pipe_instance(const std::wstring& pipe_name,
                                       const SecurityDescriptorBuilder& sec);

// ─── Child process description ──────────────────────────────────────────────────────────────

struct ChildSpec {
    // Absolute path to the interpreter, straight from models.json (settings.h). The C++ side
    // never learns what the model *is* — only which executable to launch (plan.md §1.1).
    std::wstring interpreter;

    // Arguments before the protocol-mandated ones. `--pipe <name> --proto 1` are appended by
    // IpcClient itself (docs §2.4 step 2) so no caller can forget them or spell them differently.
    std::vector<std::wstring> args;

    std::wstring working_directory;  // empty = inherit

    // Extra environment variables, applied on top of the inherited block. Used for HF_HOME and
    // UV_CACHE_DIR (plan.md §5.2).
    std::vector<std::pair<std::wstring, std::wstring>> environment;

    // Model identifier, for `loading`/`ready` correlation and for log lines.
    std::string model_id;
};

// ─── Connection ─────────────────────────────────────────────────────────────────────────────

class IpcClient {
public:
    struct Callbacks {
        // A decoded control message. Called on the I/O thread. Post it; do not process it here.
        std::function<void(const proto::Message&)> on_message;

        // A framing/encoding/transport fault, or a fatal local condition. Called on the I/O
        // thread. After a fatal error the connection is DEAD and no further callback except
        // on_disconnected will arrive.
        std::function<void(const proto::ProtocolError&)> on_error;

        // The connection has ended, orderly or not. Always the last callback.
        std::function<void()> on_disconnected;

        // State changes, for the tray. Called on the I/O thread.
        std::function<void(proto::ConnState)> on_state;
    };

    IpcClient();
    ~IpcClient();
    IpcClient(const IpcClient&) = delete;
    IpcClient& operator=(const IpcClient&) = delete;

    // Creates the pipe, spawns the child into a job object with
    // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE (so a crashed Client can never orphan a sidecar
    // holding VRAM), waits for the connection, validates the peer, and enters HANDSHAKE.
    //
    // **It does not send `hello`.** SessionMachine does, on TransportEvent::PeerValidated — see
    // fixture 020 — and the caller is what connects the two: it observes `on_state(Handshake)`
    // and feeds that event to the machine, whose Outcome carries the frame. Entering HANDSHAKE
    // arms HELLO_TIMEOUT_MS here, so a caller that never sends it fails loudly.
    //
    // Returns as soon as the child is spawned and the connect is pending — the handshake
    // completes asynchronously and is reported through on_state/on_error. Blocking here would
    // freeze the UI thread for the model's whole cold load, which can be minutes.
    Status start(const ChildSpec& child, Callbacks cb);

    // Runs the framed I/O loop over an **already connected, already peer-validated** pipe-server
    // handle, taking ownership of it. It enters HANDSHAKE without writing `hello`, exactly as
    // start() does; see the note there for why that frame belongs to SessionMachine.
    //
    // start() is implemented in terms of this, which is the point: the transport half (framing,
    // bounded queues, write deadlines, handshake state) is then reachable without spawning a
    // Python interpreter. tests/test_ipc.cpp uses it to play the sidecar's part in-process — a
    // loopback that exercises the real I/O thread rather than a parallel test-only one.
    //
    // There is no peer validation here because there is no child: the caller asserts that the
    // handle is trustworthy. Never call it with a handle whose peer has not been checked.
    Status attach_connected(uintptr_t connected_pipe_handle, Callbacks cb);

    // Orderly teardown (docs §2.5): flush, DisconnectNamedPipe, close, wait for the child, close
    // the job. Safe to call twice — the Client's teardown path also runs on crash paths, which
    // is the same reason `stop`/`cancel` are idempotent on the wire (§6.1).
    void stop();

    [[nodiscard]] proto::ConnState state() const noexcept;
    [[nodiscard]] bool is_connected() const noexcept;

    // ── Sending ─────────────────────────────────────────────────────────────────────────────

    // Enqueue one control message. Thread-safe. Returns Overloaded when the control queue is
    // full (CONTROL_QUEUE_MAX = 256), at which point the caller must treat the connection as
    // finished — §9 makes control backlog fatal rather than absorbed.
    Status send_control(const proto::Message& m);

    // Enqueue one audio frame. Thread-safe, non-blocking, called from the capture thread.
    // Returns Overloaded when the queue exceeds AUDIO_QUEUE_MAX_MS (3000 ms of PCM). **The
    // caller must not retry or drop; it must end the session with `overload_audio`.**
    Status send_audio(const int16_t* samples, size_t count);

    // docs §6.3, Client obligation: before writing `stop` or `cancel` for a session, every audio
    // frame belonging to it must already have been written or dropped. Audio frames carry no
    // session_id — attribution is positional — so this ordering *is* the attribution.
    // Blocks until the audio queue has drained or `timeout_ms` elapses.
    Status flush_audio(uint32_t timeout_ms);

    // Discard queued audio without writing it. For `cancel`, where the audio is about to become
    // meaningless. Distinct from flush_audio() so the two paths cannot be confused.
    void discard_queued_audio();

    // ── Diagnostics ─────────────────────────────────────────────────────────────────────────

    struct Stats {
        uint64_t control_sent = 0;
        uint64_t control_received = 0;
        uint64_t audio_frames_sent = 0;
        uint64_t audio_bytes_sent = 0;
        uint32_t audio_queue_ms = 0;      // current depth, against AUDIO_QUEUE_MAX_MS
        uint32_t control_queue_depth = 0; // current depth, against CONTROL_QUEUE_MAX
        uint64_t max_write_latency_ms = 0;
    };
    [[nodiscard]] Stats stats() const noexcept;

    [[nodiscard]] std::wstring pipe_name() const;
    [[nodiscard]] uint32_t child_pid() const noexcept;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace kd
