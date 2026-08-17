# kvit-dictate wire protocol — proto 1

**Status: v1 draft — frozen in Phase 2** (plan.md §3). Until the Phase 2 freeze, changes to
this file are edits to a draft and do not require a version bump. After the freeze, every
change to the normative content requires a new `proto` number and a row in §13.

This document is the authority both endpoints are implemented against. It is written to be
sufficient on its own: an implementer with this file, and no access to the other endpoint's
source or author, should be able to build a conforming peer. Where it is not sufficient, that
is a defect in this file — record it in §14 rather than deciding locally.

Normative keywords MUST / MUST NOT / SHOULD / MAY are used in the RFC 2119 sense.

---

## 1. Scope, roles, and layering

Two processes, one duplex pipe (plan.md §1.1):

| Term used here | Process | Role on the pipe | Role in the protocol |
|---|---|---|---|
| **Client** | `kvit-dictate.exe` (C++) | pipe **server** — creates the instance | protocol **client** — drives sessions |
| **Server** | `asr-server` (Python) | pipe **client** — connects by name | protocol **server** — answers with transcripts |

**The inversion is deliberate and is the single most confusing thing in this document.** The
process that creates the pipe is the one that spawns the other, because only the parent can
generate a per-launch name, apply a DACL before anyone can connect, and know the child's PID
for peer validation. Everywhere below, "Client" and "Server" mean the protocol roles in the
table; the pipe roles are named explicitly as "pipe server" / "pipe client" when they matter.

Four layers, each of which can fail independently and is tested independently:

| Layer | Unit | Failure surface |
|---|---|---|
| L0 transport | octets | broken pipe, ACL, peer identity |
| L1 framing | frames (§3) | `bad_frame`, `frame_too_large`, `truncated_frame`, `unknown_frame_type` |
| L2 encoding | UTF-8 + JSON (§4) | `bad_utf8`, `bad_json`, `bad_message` |
| L3 session | messages in order (§6–§10) | `state_violation`, `stability_violation`, overload |

A receiver MUST detect a failure at the lowest layer that can see it, and MUST NOT let a
higher layer paper over a lower-layer failure (for example: no U+FFFD substitution to rescue a
malformed control payload — see §4.1).

### 1.1 Constants

| Name | Value | Where used |
|---|---|---|
| `PROTO_VERSION` | `1` | §5 |
| `MAX_PAYLOAD` | `65536` bytes (64 KiB) | §3 |
| `MIN_LENGTH_FIELD` | `1` | §3 |
| `MAX_LENGTH_FIELD` | `65537` (= `MAX_PAYLOAD` + 1 type byte) | §3 |
| `MAX_FRAME_ON_WIRE` | `65541` (= 4 + 1 + `MAX_PAYLOAD`) | §2.2, §3 |
| `SAMPLE_RATE` | `16000` Hz | §6.3 |
| `AUDIO_FRAME_MS_RECOMMENDED` | 20–100 ms | §6.3 |
| `AUDIO_QUEUE_MAX_MS` | `3000` | §9 |
| `CONTROL_QUEUE_MAX` | `256` messages | §9 |
| `WRITE_DEADLINE_MS` | `5000` | §9 |
| `CONNECT_TIMEOUT_MS` | `15000` | §2.4 |
| `HELLO_TIMEOUT_MS` | `5000` | §5 |
| `LOADING_HEARTBEAT_MS` | `5000` | §6.6 |
| `READY_TIMEOUT_MS` | `180000`, reset by each `loading` | §6.6 |

---

## 2. Transport

### 2.1 Why a named pipe

Not a TCP socket: no Windows Firewall prompt on first run (which reads badly for a privacy
tool), no port collision, no discovery dance. Not stdin/stdout: the sidecar's Python runtime,
its logging, and any C extension it loads all write to stdout/stderr unpredictably, and one
stray `print` would corrupt a binary frame stream.

### 2.2 Pipe creation (Client, the pipe server)

Name: `\\.\pipe\kvit-dictate-<32 lowercase hex digits>`, where the 32 hex digits are 16 bytes
from `BCryptGenRandom` with `BCRYPT_USE_SYSTEM_PREFERRED_RNG`. A new name is generated for
every child launch and is never reused, including across restarts of the same child.

`CreateNamedPipeW` parameters, all load-bearing:

| Parameter | Value | Why |
|---|---|---|
| `dwOpenMode` | `PIPE_ACCESS_DUPLEX \| FILE_FLAG_OVERLAPPED \| FILE_FLAG_FIRST_PIPE_INSTANCE` | Overlapped so a wedged peer can never block a thread past `WRITE_DEADLINE_MS`. `FILE_FLAG_FIRST_PIPE_INSTANCE` makes creation fail with `ERROR_ACCESS_DENIED` if the name already exists — i.e. it turns a squatting attempt into a loud error instead of a silent second instance. |
| `dwPipeMode` | `PIPE_TYPE_MESSAGE \| PIPE_READMODE_MESSAGE \| PIPE_WAIT \| PIPE_REJECT_REMOTE_CLIENTS` | Message mode keeps frame boundaries intact for conforming peers; `PIPE_REJECT_REMOTE_CLIENTS` refuses clients arriving over the network redirector. |
| `nMaxInstances` | `1` | There is exactly one sidecar per pipe name. A second instance of the same name cannot exist, so a late or rival connector gets `ERROR_PIPE_BUSY` rather than a private conversation. |
| `nOutBufferSize` / `nInBufferSize` | `262144` (4 × `MAX_FRAME_ON_WIRE`, rounded) | A quota, not a limit. Sized so ordinary bursts do not force the writer to block. |
| `nDefaultTimeOut` | `0` (system default) | Unused; all waits are explicit. |
| `lpSecurityAttributes` | see §2.3, `bInheritHandle = FALSE` | The child opens the pipe by name; it must not inherit the pipe-server handle. |

**Message mode does not excuse a message-oriented parser.** Both endpoints MUST implement the
framing layer as a *byte stream* decoder that tolerates arbitrary fragmentation and coalescing
(fixtures 001 and 002). Reasons, all real: a reader that supplies a buffer smaller than the
message gets `ERROR_MORE_DATA` and must reassemble anyway; a peer that sets
`PIPE_READMODE_BYTE` on its own handle silently changes delivery; and the Python side reaches
the pipe through `ctypes`/`kernel32` where the read mode is easy to get wrong. Relying on
message boundaries would make the protocol correct only by accident.

Conversely, a *writer* MUST issue exactly one write per frame and MUST NOT split a frame
across writes or pack two frames into one write. This keeps message boundaries aligned with
frame boundaries for conforming peers, which makes packet-level debugging legible.

If a reader receives `ERROR_MORE_DATA`, it MUST continue reading the remainder into the same
frame buffer. Discarding the remainder, or treating the partial data as a frame, is a defect.

### 2.3 Security descriptor

Revision 1 of the plan claimed a named pipe's default ACL restricts it to the current user.
That is false and is corrected in plan.md §1.2: Microsoft documents that the default descriptor
grants full control to LocalSystem, administrators and the creator owner, and **read access to
Everyone and to the anonymous account**. A default descriptor is therefore not acceptable.

The Client MUST build an explicit DACL:

1. Obtain the **logon SID** from the process token (`GetTokenInformation(TokenGroups)`, the
   group whose attributes include `SE_GROUP_LOGON_ID`). Microsoft's guidance: *"To prevent
   remote users or users on a different terminal services session from accessing a named pipe,
   use the logon SID on the DACL for the pipe."* The user SID alone is insufficient — it does
   not separate concurrent sessions of the same user.
2. Build a DACL with exactly one allow-ACE, for that logon SID, granting **only**:
   `FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES |
   SYNCHRONIZE`.
3. No ACE for `Everyone`, `ANONYMOUS LOGON`, `Authenticated Users`, or `NETWORK`.
4. Apply it with `InitializeSecurityDescriptor` + `SetSecurityDescriptorDacl(sd, TRUE, dacl,
   FALSE)`. An **explicit empty DACL denies everyone**; a **NULL DACL grants everyone**. The
   difference is one argument and the failure is silent, so this is asserted in a unit test.

**Why individual rights and not `FILE_GENERIC_WRITE`.** `FILE_GENERIC_WRITE` on a named pipe
includes `FILE_CREATE_PIPE_INSTANCE`. A client holding that right can create additional
instances of the same pipe name. With `nMaxInstances = 1` it cannot create a second instance
concurrently, but the right is still unnecessary for a peer that only reads and writes, and
granting it would leave the door open the moment `nMaxInstances` is ever changed.
`FILE_WRITE_ATTRIBUTES` is granted because the pipe client calls `SetNamedPipeHandleState` to
set its read mode.

**What this does and does not defend.** It defends against other logon sessions, other users,
and remote connections. It partially constrains lower-integrity processes: under the default
mandatory-integrity policy (`NO_WRITE_UP`) a low-integrity process cannot obtain write access
to our medium-integrity pipe. Integrity does not restrict *read* access, however, and a low-IL
process in this logon session holds the logon SID like any other, so the DACL does not exclude
it. More broadly, this does **not** defend against another process running as the same user in
the same session: such a process can already read our command line, open our process, and inject
code. That is not a boundary this design claims to hold, and claiming it would be dishonest.

### 2.4 Child launch, connection, and peer validation

1. Client generates the pipe name and creates the instance (§2.2). Creation happens **before**
   the child is spawned, so the child never races an uncreated pipe and never has to retry.
2. Client spawns the sidecar with, at minimum, `--pipe <full pipe name> --proto 1`. Other
   arguments (model id, interpreter, device) come from `models.json` and are outside this spec.
3. Client calls `ConnectNamedPipe` (overlapped). If no client connects within
   `CONNECT_TIMEOUT_MS`, the Client terminates the child, closes the pipe, and reports
   `spawn_timeout` locally.
4. On connection the Client calls `GetNamedPipeClientProcessId` and compares the result with
   the `dwProcessId` returned by `CreateProcess`. On mismatch it MUST disconnect immediately,
   kill the child, and report `peer_mismatch` locally. **PID recycling is not a hole here**:
   the Client holds the child's process handle open for the child's whole lifetime, and Windows
   does not recycle a PID while a handle to that process is open.
5. The child SHOULD perform the mirror check with `GetNamedPipeServerProcessId` against a
   `--parent-pid` argument, and exit non-zero on mismatch. See §14 — this is the one API in
   this section whose exact name and availability must be confirmed against the SDK at
   implementation time rather than trusted from this document.

**The random name is not a secret.** Command lines are readable by other processes running as
the same user. The random name defeats *squatting* — an attacker cannot create the pipe before
us if it cannot predict the name — and `FILE_FLAG_FIRST_PIPE_INSTANCE` catches the attempt if
it ever guesses. Confidentiality comes from the DACL and the peer check, not from the name.

The child is created suspended-and-assigned or immediately assigned to a **job object** with
`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, so a crashed Client can never orphan a sidecar holding
VRAM (plan.md §3, Phase 3). This is process supervision, not protocol, but it is the reason
the protocol never needs a keepalive: a dead peer becomes a broken pipe, promptly.

### 2.5 Teardown

Orderly: Client sends `stop` (or `cancel`), waits for `stopped`, closes its write side by
calling `FlushFileBuffers` then `DisconnectNamedPipe`, then closes the handle. The Server sees
end of stream on a frame boundary (fixture 010) and exits 0.

Disorderly: any read returning `ERROR_BROKEN_PIPE` / `ERROR_PIPE_NOT_CONNECTED`, or end of
stream mid-frame (fixture 011), is a dead connection. See §10.3 for the required behaviour.

---

## 3. Framing

Every frame, in both directions:

```
 0        1        2        3        4        5                    4+length
 +--------+--------+--------+--------+--------+--------------------+
 |      length  (u32, little-endian) |  type  |  payload           |
 +--------+--------+--------+--------+--------+--------------------+
                                     \___________________________/
                                       length counts these bytes
```

### 3.1 The length field covers the type byte

**Decision, stated once and relied on everywhere: `length = 1 + sizeof(payload)`.**

Both alternatives work; what matters is that exactly one is written down. This one is chosen
because it makes the reader's inner loop a single subtraction — after consuming the 4-byte
prefix the reader needs exactly `length` more bytes, with no special case for the type byte —
and because it makes `length == 0` structurally impossible, which converts a common encoder bug
(forgetting to add the type byte) into a loud `bad_frame` instead of a silent off-by-one that
would desynchronise the stream and only surface as garbage text much later.

Consequences, all of which the fixtures pin down:

- `MIN_LENGTH_FIELD` is `1` (a frame with an empty payload). `length == 0` is malformed →
  `bad_frame` (fixture 006).
- `MAX_LENGTH_FIELD` is `65537`, corresponding to the maximum 65536-byte payload
  (fixture 004). `65538` is `frame_too_large` (fixture 005).
- The total on-wire size of a frame is `4 + length`, at most `MAX_FRAME_ON_WIRE = 65541`.

The length field is **little-endian**, matching the only architecture this project targets. No
`htonl`. Readers MUST treat it as unsigned 32-bit; a reader that sign-extends fails fixture 007.

### 3.2 Type byte

| Type | Name | Payload |
|---|---|---|
| `0x01` | control | UTF-8 JSON object (§4) |
| `0x02` | audio | raw PCM, 16 kHz mono s16le (§6.3) |
| others | — | undefined in proto 1 |

An undefined type byte is a fatal `unknown_frame_type` (fixture 009). It is **not** treated as
a skippable extension: the handshake in §5 exists precisely so that both peers agree on the
message set, so an undefined type means one peer is broken, and skipping it would hide the bug
while the stream continues to look healthy.

### 3.3 Size limits are fatal, never truncating

A receiver MUST validate `length` against `[MIN_LENGTH_FIELD, MAX_LENGTH_FIELD]` **from the
4-byte prefix alone**, before reading, buffering, or allocating any payload. On violation it
MUST NOT read the payload, MUST NOT truncate the frame, MUST NOT attempt to resynchronise by
scanning for a plausible header, and MUST terminate the connection (§11).

Resynchronisation is forbidden because there is no framing marker to resynchronise *to*: any
byte pattern is a legal payload, so a scan would find false headers and could deliver
attacker- or bug-chosen bytes as a valid message. A desynchronised stream is unrecoverable by
construction; the only safe move is to stop.

### 3.4 Reference decoder

```
loop:
  if buffered < 4: read more; on EOF -> if buffered == 0 then END_OF_STREAM
                                        else error truncated_frame
  length = u32le(buffer[0..4])
  if length < 1 or length > 65537: error frame_too_large / bad_frame   # before any payload read
  if buffered < 4 + length: read more; on EOF -> error truncated_frame
  type    = buffer[4]
  payload = buffer[5 .. 4+length]
  consume(4 + length)
  deliver(type, payload)
```

`END_OF_STREAM` with an empty buffer is orderly shutdown, not an error (fixture 010). Any
partial frame at end of stream is `truncated_frame` (fixture 011).

---

## 4. Encoding rules for control frames

### 4.1 UTF-8

A control payload MUST be well-formed UTF-8. A receiver MUST validate before parsing, and MUST
reject invalid input with `bad_utf8` (fixture 008). Substituting U+FFFD is **forbidden**: this
text is destined for a keystroke injector aimed at the user's document, and a replacement
character is a silent corruption that the user discovers only by reading their own document
later.

Well-formed means the Unicode definition, not the permissive one: no overlong encodings, no
encodings of surrogate code points U+D800–U+DFFF, nothing above U+10FFFF, no truncated
sequences.

### 4.2 JSON subset

Payload MUST be a single JSON object (RFC 8259) with these additional restrictions:

- Top level is an object. Not an array, not a scalar.
- No duplicate keys in any object → `bad_json`.
- No `NaN`, `Infinity`, `-Infinity`, no leading `+`, no leading zeros — plain RFC 8259.
- Every number this protocol defines is an **integer**. A receiver MAY reject a
  non-integer-valued number where an integer is specified, as `bad_message`.
- `\uXXXX` escapes are permitted; surrogate pairs MUST be well formed. A lone surrogate is
  `bad_json`. Senders SHOULD emit raw UTF-8 rather than escapes (Python: `ensure_ascii=False`),
  because it makes `stable_bytes` offsets directly checkable against the wire bytes.
- **Offsets index the decoded string, not the wire bytes.** A receiver MUST decode escapes
  before applying `stable_bytes` (§8). This is the subtle one: `"café"` is 8 bytes on the
  wire and 5 bytes decoded, and `stable_bytes` means 5-byte space.
- Whitespace between tokens is permitted. Senders SHOULD emit compact JSON.
- An **unknown field** in a known message is ignored, with a log line. An **unknown `t` value**
  is fatal `unknown_message`. The asymmetry is intentional: an extra field cannot change the
  meaning of a message a receiver already understands, while an unknown message type means the
  peer believes something is happening that this peer knows nothing about.

Both endpoints implement their own JSON parser (no third-party dependency on either side, per
the repository rules). The restrictions above exist to keep both parsers small and to keep the
two implementations from disagreeing on exotic inputs.

---

## 5. Handshake and version negotiation

```
Client                                   Server
  |  0x01 {"t":"hello","proto":1}  ---->  |
  |  <----  0x01 {"t":"hello_ack","proto":1,"server":"kvit-asr/0.1"}
```

- The Client MUST send `hello` as the **first frame** on the connection, before any other
  frame, and MUST NOT send audio before `ready`.
- The Server MUST answer with `hello_ack` or with a fatal `error` carrying code
  `proto_unsupported`, and MUST send nothing else before it answers.
- If the Server does not support the offered version, it MUST reply
  `{"t":"error","fatal":true,"code":"proto_unsupported","msg":"...","supported":[1]}` and then
  close. It MUST NOT downgrade silently and MUST NOT proceed on a best-effort basis.
- If the Client receives `hello_ack` with `proto` != the version it offered, that is fatal
  `proto_unsupported` on the Client side: version selection is the Client's, and a Server that
  answers with a different number has changed the contract unilaterally.
- If no `hello_ack` (or `error`) arrives within `HELLO_TIMEOUT_MS`, the Client kills the child
  and reports `handshake_timeout` locally.

**`hello_ack` is not in plan.md §1.2, and is added deliberately.** The plan's sketch has the
Client send `hello` and the Server reply `loading`/`ready`, but those describe the *model*, not
the *protocol*: a Client that receives `loading` learns only that some peer is alive, not that
its protocol version was accepted, and a Client that receives nothing cannot tell "rejected"
from "still starting Python". A handshake needs a positive acknowledgement or it is not a
handshake.

proto 1 supports exactly one version. `supported` in the rejection is an array of integers so
that a future Server can tell an old Client what to fall back to.

---

## 6. Message catalogue

All control messages are type `0x01` frames. `t` is required in every message and selects the
schema. Field tables below use: **R** required, **O** optional.

### 6.1 Client → Server

#### `hello`

| Field | Type | R/O | Value |
|---|---|---|---|
| `t` | string | R | `"hello"` |
| `proto` | integer | R | `1` in this version. Range 1..65535. |
| `client` | string | O | Client build id, e.g. `"kvit-dictate/0.1"`. Diagnostics only; never parsed for behaviour. |

#### `start`

| Field | Type | R/O | Value |
|---|---|---|---|
| `t` | string | R | `"start"` |
| `session_id` | string | R | Session identity, §7.1. |
| `sample_rate` | integer | R | MUST be `16000` in proto 1. Any other value → `bad_message`. Present, despite being fixed, so that a mismatch is caught at the start of a session rather than inferred from bad transcripts. |
| `channels` | integer | O | Default `1`; MUST be `1` in proto 1. |
| `format` | string | O | Default `"s16le"`; MUST be `"s16le"` in proto 1. |
| `context` | array of string | O | Vocabulary hints, e.g. `["llama.cpp","Qwen3"]`. At most 256 items, each at most 128 bytes. A Server without context support MUST ignore it and MUST report `context_supported:false` in `ready` (§4 of plan.md requires context-enabled and context-free runs to be compared separately — silently ignoring the field would corrupt that comparison). |

A `start` while a session is already open is `state_violation` (fatal). Sessions do not nest
and are not concurrent: exactly one may be open per connection (§7.2).

#### `stop`

| Field | Type | R/O | Value |
|---|---|---|---|
| `t` | string | R | `"stop"` |
| `session_id` | string | R | MUST be the currently open session. |

Semantics: finalise what exists, then end. The Server flushes its decoder, emits any remaining
`final` for the open utterance, then `stopped`.

#### `cancel`

| Field | Type | R/O | Value |
|---|---|---|---|
| `t` | string | R | `"cancel"` |
| `session_id` | string | R | MUST be the currently open session. |

Semantics: discard, emit nothing further. The Server MUST NOT emit any `partial` or `final` for
this session after it *reads* the `cancel` — but messages already written before it read the
cancel will still arrive, which is why §10.4 puts the discard obligation on the Client too. The
Server MUST still emit `stopped`, so the Client's teardown is acknowledged rather than
timed out.

`stop` and `cancel` are **idempotent** with respect to unknown or already-ended sessions: a
`stop`/`cancel` naming a session the Server does not have open is answered with `stopped` for
that `session_id` and is **not** an error. This makes the Client's teardown path safe to run
twice, which matters because teardown also runs on crash paths.

#### `discontinuity`

| Field | Type | R/O | Value |
|---|---|---|---|
| `t` | string | R | `"discontinuity"` |
| `session_id` | string | R | Current session. |
| `gap_ms` | integer | R | Estimated milliseconds of audio lost, ≥ 0. `0` means "a discontinuity occurred, duration unknown". |
| `reason` | string | O | Free text for logs, e.g. `"AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY"`. |

**Also not in plan.md §1.2, also deliberate.** WASAPI reports capture glitches
(`AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY`). The two silent options — pad with zeroes, or splice
the audio together — both hand the decoder a lie, and produce mysteriously bad transcription
that is indistinguishable from a bad model. This message is the honest third option: the Server
learns the stream is not contiguous and MAY reset decoder state or force an utterance boundary.
Sending it is required whenever the Client detects a gap; the Server MUST accept it and MUST
NOT treat it as fatal.

#### audio (frame type `0x02`)

Not a JSON message. See §6.3.

### 6.2 Server → Client

#### `loading`

| Field | Type | R/O | Value |
|---|---|---|---|
| `t` | string | R | `"loading"` |
| `model` | string | R | Model identifier as spelled in `models.json`. |
| `stage` | string | O | Free text, e.g. `"import torch"`, `"weights"`, `"warmup"`. |
| `progress` | integer | O | 0..100 if known. Absent means unknown; a Client MUST NOT invent a progress bar from its absence. |

Emitted zero or more times between `hello_ack` and `ready`. The Server SHOULD emit one at least
every `LOADING_HEARTBEAT_MS` while loading, since cold model load can take minutes and the
Client's `READY_TIMEOUT_MS` restarts on each `loading`.

#### `hello_ack`

| Field | Type | R/O | Value |
|---|---|---|---|
| `t` | string | R | `"hello_ack"` |
| `proto` | integer | R | The accepted version; MUST equal the offered `proto`. |
| `server` | string | O | Server build id. Diagnostics only. |

#### `ready`

| Field | Type | R/O | Value |
|---|---|---|---|
| `t` | string | R | `"ready"` |
| `model` | string | R | Model identifier. |
| `warm_ms` | integer | R | Milliseconds from process start to ready, ≥ 0. Feeds the cold/warm start metric in plan.md §4. |
| `mode` | string | R | One of `"native-streaming"`, `"stateful-chunked"`, `"pseudo-streaming"`. Exactly the taxonomy Phase 0A records, so latency numbers stay interpretable. Any other value → `bad_message`. |
| `chunk_ms` | integer | O | The chunk size the backend is configured for, if it has one. |
| `context_supported` | boolean | O | Default `false`. Whether `start.context` is honoured. |

`ready` MUST be sent exactly once per connection, after `hello_ack`. There is no
`ready`-again: a model change means a new child process (plan.md §1.1).

#### `partial`

| Field | Type | R/O | Value |
|---|---|---|---|
| `t` | string | R | `"partial"` |
| `session_id` | string | R | §7.1 |
| `utterance_id` | integer | R | ≥ 1, §7.3 |
| `seq` | integer | R | ≥ 1, strictly increasing per session, §7.4 |
| `text` | string | R | **Cumulative** hypothesis for the current utterance, not a delta. §8 |
| `stable_bytes` | integer | R | Prefix length of `text` in UTF-8 bytes, `0 ≤ stable_bytes ≤ len(utf8(text))`, on a grapheme-cluster boundary, non-decreasing within the utterance. §8 |
| `audio_pos_ms` | integer | O | Decoder position in the session's audio timeline, ≥ 0. |

#### `final`

| Field | Type | R/O | Value |
|---|---|---|---|
| `t` | string | R | `"final"` |
| `session_id` | string | R | §7.1 |
| `utterance_id` | integer | R | The utterance being closed. |
| `seq` | integer | R | Same sequence space as `partial`. |
| `text` | string | R | The complete text of the utterance. Entirely stable by definition. |
| `audio_pos_ms` | integer | R | End of the utterance in the session audio timeline, ms, ≥ 0. |
| `stable_bytes` | integer | O | If present MUST equal `len(utf8(text))`. Redundant; permitted so a Server can use one code path for both message types. |

Exactly one `final` per `utterance_id`. After a `final` for utterance *n*, the next `partial`
MUST carry `utterance_id` *n+1* (or the session ends).

#### `stopped`

| Field | Type | R/O | Value |
|---|---|---|---|
| `t` | string | R | `"stopped"` |
| `session_id` | string | R | The session that has ended. |
| `reason` | string | O | `"stop"`, `"cancel"`, `"error"`, or `"unknown_session"` (the idempotent case in §6.1). Diagnostics. |

Terminal for the session. The Server MUST send nothing further carrying that `session_id`.

#### `error`

| Field | Type | R/O | Value |
|---|---|---|---|
| `t` | string | R | `"error"` |
| `code` | string | R | One of §11. Stable machine-readable identifier. |
| `fatal` | boolean | R | `true` = the **connection** is finished (§11.1). |
| `msg` | string | R | Human-readable, for logs and the tray. Never parsed. |
| `session_id` | string | conditional | **Required when `fatal` is `false`**, absent or present when `fatal` is `true`. |
| `supported` | array of integer | O | Only with `proto_unsupported`. |

`error` is bidirectional; the Client sends it too (for example `stability_violation`).

plan.md §1.2 sketches `error` with only `fatal` and `msg`. `code` is added and made required
because `msg` is prose: it will be reworded, translated, and interpolated with details, and a
receiver that branches on it is broken by the first wording change. `code` is the stable
identifier and `msg` is for humans.

### 6.3 Audio frames (type `0x02`)

Payload is raw PCM: **16000 Hz, mono, signed 16-bit little-endian**, samples contiguous, no
header, no padding, no interleaving. Payload length MUST be even; an odd length is
`bad_message` (it means the sender split a sample, which means its buffer arithmetic is wrong).

- Recommended payload: 20–100 ms, i.e. 640–3200 bytes. The hard cap of 65536 bytes is 2048 ms,
  which no sane capture path should reach.
- A zero-length audio payload is legal and means "no samples" (fixture 003). It carries no
  meaning; a receiver processes it as a no-op. It exists as a legal state so that a sender's
  empty-buffer edge case does not have to be special-cased into a protocol error.

**Session attribution of audio frames — the one real design compromise, stated openly.** Audio
frames carry no `session_id`. They belong to whichever session is open on the connection at the
point in the stream where they appear. This is sound only because a named pipe delivers bytes
reliably and in order, so "between `start` and `stop`" is unambiguous, and because of two
obligations:

- **Client obligation:** before writing `stop` or `cancel` for a session, the Client MUST have
  written, or dropped, every audio frame belonging to it. No audio for session *n* may follow
  the terminating control frame for session *n*.
- **Server obligation:** audio received while no session is open MUST be discarded and counted.
  It MUST NOT be buffered for the next session, and MUST NOT be an error — losing the race
  between a `stop` and an in-flight audio frame is a Client bug this rule contains rather than
  escalates.

The alternative — an 8-byte binary header per audio frame carrying a session tag and sequence
number — was considered and rejected. On a reliable ordered transport it would detect nothing
that the framing layer does not already detect: a pipe does not drop a frame in transit, and a
*locally* dropped frame is something the sender already knows about and must report as
`overload_audio` (§9) rather than silently skip. The header would add per-chunk parsing cost
and a second identity space to keep consistent, for no detection the protocol lacks.

---

## 7. Identity and ordering

This section exists to prevent one specific failure: a late `final` from a finished session
being injected into whatever window the user has since focused (plan.md §6, risk table).

### 7.1 `session_id`

- Opaque string, 1–64 bytes, generated by the **Client**, one per hotkey activation.
- Characters restricted to ASCII `[A-Za-z0-9_-]` so it is safe in log lines and file names.
- MUST be unique for the lifetime of the Client process, **including across child restarts**.
  Recommended form: `s<N>` with a counter that never resets (`s7`, `s8`, …). Reusing an id
  after a restart would defeat the staleness check exactly when it matters most (fixture 020).
- The Server echoes it verbatim in every message it produces for that session. It MUST NOT
  normalise, trim, or re-case it.

**The rule, absolute:** the Client MUST drop any `partial` or `final` whose `session_id` is not
the currently open session — not clamp it, not queue it, not inject it (fixture 014).

`stopped` and `error` for a non-current session are the exception, and only in one direction:
the Client MUST NOT apply any text or state from them, but MUST record them for bookkeeping
(releasing resources, logging why the old session died). The fixtures call this verdict
`accept_bookkeeping_only`. Without it, the Client would have no way to observe the orderly end
of a session it has already moved on from.

### 7.2 One session at a time

Exactly one session may be open per connection. `start` while open is `state_violation`
(fatal). The Client serialises hotkey activations; the pipe is not a multiplexer.

### 7.3 `utterance_id`

- Unsigned integer, assigned by the **Server**, starting at `1` for the first VAD-delimited
  utterance of a session and incrementing by exactly 1.
- Scoped to the session. It restarts at 1 in the next session.
- The stability high-water mark (§8) resets at every utterance boundary.

### 7.4 `seq`

- Unsigned integer, assigned by the **Server**, scoped to the session (not the utterance),
  starting at `1`, strictly increasing across all `partial` and `final` messages.
- `loading`, `ready`, `hello_ack`, `stopped` and `error` do **not** consume `seq` numbers; they
  are not part of the text stream and are never dropped for ordering reasons.

Receiver rules (fixtures 015, 016):

| Condition | Behaviour |
|---|---|
| `seq == last_seq` | Drop as duplicate. Log. No state change. |
| `seq < last_seq` | Drop as out of order. Log. No state change. |
| `seq == last_seq + 1` | Normal. Accept. |
| `seq > last_seq + 1` | **Accept**, log the gap, and **reset anchor confidence** to zero. |

A gap is not fatal. On a reliable transport a gap cannot arise in transit, so it means the
sender skipped numbers or dropped its own output — but the message in hand is still the newest
hypothesis, and discarding it would be worse than accepting it. The anchor-confidence reset is
the real consequence: commit differencing (plan.md §1.5) infers the caret position from the
change produced by text the Client believes it injected, and a missed hypothesis means that
belief is no longer sound.

---

## 8. Text semantics and the committed-prefix invariant

### 8.1 `text` is cumulative

`partial.text` is the **complete current hypothesis for the current utterance**, not a delta
and not a suffix. Each `partial` supersedes the previous one for that `utterance_id`. Deltas
were rejected because a receiver that loses one delta silently produces wrong text, whereas a
receiver that loses one cumulative hypothesis produces stale text and recovers on the next one.

`text` may shrink, reword, or repunctuate freely **above** `stable_bytes`. That is what
streaming ASR does, and the overlay exists to absorb it.

### 8.2 `stable_bytes`

`stable_bytes` is a **prefix length of `text`, measured in UTF-8 bytes of the decoded string**
(§4.2). Not characters, not code points, not UTF-16 code units. Revision 1 of the plan called
this `stable_chars`, which was ambiguous between all four; the rename is part of the fix.

Three requirements on the sender, each independently checkable:

1. **Range.** `0 ≤ stable_bytes ≤ len(utf8(text))`.
2. **Grapheme-cluster boundary.** `stable_bytes` MUST fall on a boundary between extended
   grapheme clusters (UAX #29) of `text`. It therefore also falls on a UTF-8 sequence boundary,
   but that is weaker and not sufficient — fixture 013 is entirely made of offsets that are
   valid UTF-8 boundaries and invalid cluster boundaries.
3. **Monotonic within an utterance.** For two messages of the same `utterance_id` with
   `seq₁ < seq₂`:
   - `text₂.stable_bytes ≥ text₁.stable_bytes`, **and**
   - `utf8(text₂)[0 : stable_bytes₁] == utf8(text₁)[0 : stable_bytes₁]`.

   The second half is the one that is easy to forget and is the one with teeth: a
   non-decreasing `stable_bytes` whose underlying bytes changed has still retracted text the
   Client may already have typed into the user's document (fixture 018).

   The same applies to `final`: `utf8(final.text)` MUST begin with the highest stable prefix
   published for that utterance.

The high-water mark resets to 0 at each `utterance_id` change. It is **not** reset by a `seq`
gap — a gap means hypotheses were missed, not retracted, and weakening the invariant on a gap
would let a buggy Server escape it by skipping numbers.

Trailing whitespace SHOULD be excluded from `stable_bytes` (plan.md §1.4): holding the space
back lets a following punctuation revision land without rewriting across committed text.

### 8.3 What a receiver does when the invariant is violated

Not "clamp and continue". Not "log and take the new value". A violation of §8.2 is a
**protocol error**, and the required behaviour is (fixtures 017, 018):

1. **Preserve the document.** Whatever has already been injected stays. No backspaces, no
   selection-and-replace, no repair of any kind. This is the plan's §1.4 rule and it is
   absolute: repair means sending backspaces into an arbitrary application, where backspace can
   navigate a browser, dismiss an IDE autocomplete, or eat a terminal prompt.
2. **Abort the session, not the connection.** Send
   `{"t":"error","session_id":"<current>","code":"stability_violation","fatal":false,"msg":...}`
   and stop applying anything for that session. The child is not at fault as a *process*; its
   model produced an inadmissible revision, and the next session may well be fine.
3. **Log both texts and the diverging byte offset**, plus `session_id`, `utterance_id` and both
   `seq` values.
4. **Count it as an irrevocable-error event** attributed to that backend and configuration —
   this feeds ICER (plan.md §1.4), which is the metric that gates model selection. Silently
   clamping would hide exactly the defect the metric exists to measure, which is why clamping is
   forbidden rather than merely discouraged.
5. Surface it: the overlay flags the utterance, the tray shows a warning state.

The same handling applies when `stable_bytes` is out of range or off a cluster boundary.

### 8.4 Grapheme clusters, honestly

Neither endpoint may depend on ICU (C++ has no third-party dependencies at all here), so both
ship their own UAX #29 table. proto 1 requires a receiver's boundary check to reject, at
minimum, an offset that:

- splits a UTF-8 sequence;
- falls immediately before a code point with `Grapheme_Extend = Yes` (combining marks) or
  `Grapheme_Cluster_Break = SpacingMark`;
- splits a `ZWJ` emoji sequence (UAX #29 rule GB11);
- splits a regional-indicator pair (GB12/GB13);
- falls before a variation selector or `U+20E3` combining enclosing keycap.

Fixture 013 pins exactly these. Strengthening a receiver's check toward fuller UAX #29
conformance later does **not** require a proto bump, because a conforming sender never emits an
offset that a stricter check would newly reject. The Unicode version whose tables both
endpoints pin is an open question (§14) — it must be the same version on both sides, and it
must be recorded in-repo rather than inherited from whatever the toolchain happens to ship.

---

## 9. Backpressure

Bounded queues in both directions. The governing rule from plan.md §1.2: **silently dropping or
delaying audio produces mysteriously bad transcription and is not acceptable.** An explicit
overload error that ends the session is a worse user experience for ten seconds and a better
one forever, because it is diagnosable.

| Queue | Owner | Bound | On overflow |
|---|---|---|---|
| Outbound audio | Client | `AUDIO_QUEUE_MAX_MS` = 3000 ms of PCM | `error` `overload_audio`, `fatal:true`, tear down |
| Inbound audio | Server | `AUDIO_QUEUE_MAX_MS` = 3000 ms of PCM | `error` `overload_audio`, `fatal:true`, tear down |
| Outbound control | Server | `CONTROL_QUEUE_MAX` = 256 messages | `error` `overload_control`, `fatal:true`, tear down |
| Inbound control | Client | `CONTROL_QUEUE_MAX` = 256 messages | `error` `overload_control`, `fatal:true`, tear down |

Additional rules:

- A write that has not completed within `WRITE_DEADLINE_MS` is treated as overload of the
  corresponding queue. This is why both endpoints use overlapped/async I/O: a peer that stops
  reading must not be able to block the Client's audio thread indefinitely.
- Neither endpoint may drop an audio frame to relieve pressure. Not the newest, not the oldest,
  not "every other one". The only legal responses are to keep up or to fail loudly.
- A Server that cannot decode in real time is **not** an overload condition by itself as long
  as it consumes audio at real-time rate; falling behind on *decoding* shows up as latency in
  the metrics, which is a measurement, not a fault. Overload is specifically about the queue.
- Backpressure applies per connection. There is no per-session reset of these limits.

The exit gate in plan.md §7 — "bounded audio and IPC queues under overload; no silent audio
drops" — is tested against these numbers.

---

## 10. Lifecycle

### 10.1 States

Connection-scoped states, both endpoints (the Client also has `SPAWNING`/`VALIDATING`, which
exist before any frame is exchanged):

| State | Meaning |
|---|---|
| `SPAWNING` | Client only. Pipe created, child launched, awaiting `ConnectNamedPipe`. |
| `VALIDATING` | Client only. Connected; checking `GetNamedPipeClientProcessId`. |
| `HANDSHAKE` | `hello` sent / awaited; nothing else may cross. |
| `LOADING` | `hello_ack` exchanged; model loading; `loading` messages permitted. |
| `IDLE` | `ready` exchanged; no session open. |
| `RUNNING` | Session open; audio flowing; `partial`/`final` flowing. |
| `FINALIZING` | `stop` sent/received; awaiting `stopped`. |
| `CANCELLING` | `cancel` sent/received; awaiting `stopped`; all text discarded. |
| `DEAD` | Connection finished. Terminal. Recovery is a new child and a new pipe. |

### 10.2 Client transitions

| State | Event | Next | Action |
|---|---|---|---|
| `SPAWNING` | client connects | `VALIDATING` | — |
| `SPAWNING` | `CONNECT_TIMEOUT_MS` elapsed | `DEAD` | kill child, report `spawn_timeout` |
| `VALIDATING` | PID matches | `HANDSHAKE` | send `hello` |
| `VALIDATING` | PID mismatch | `DEAD` | disconnect, kill child, report `peer_mismatch` |
| `HANDSHAKE` | `hello_ack` with matching `proto` | `LOADING` | — |
| `HANDSHAKE` | `hello_ack` with other `proto`, or `error proto_unsupported` | `DEAD` | surface "sidecar protocol mismatch" |
| `HANDSHAKE` | `HELLO_TIMEOUT_MS` elapsed | `DEAD` | kill child, report `handshake_timeout` |
| `HANDSHAKE` | any other message | `DEAD` | `state_violation` |
| `LOADING` | `loading` | `LOADING` | restart `READY_TIMEOUT_MS`, update tray |
| `LOADING` | `ready` | `IDLE` | tray → idle-ready |
| `LOADING` | hotkey pressed | `LOADING` | **do not send `start`.** Show "model loading". Buffer locally or refuse — a Client choice, but the wire rule is that `start` may only be sent from `IDLE`. |
| `LOADING` | `READY_TIMEOUT_MS` elapsed | `DEAD` | kill child, report `model_load_timeout` |
| `IDLE` | hotkey pressed | `RUNNING` | new `session_id`, send `start` |
| `IDLE` | `partial`/`final` for any session | `IDLE` | drop (stale, §7.1) |
| `RUNNING` | mic buffer ready | `RUNNING` | send `0x02` audio frame |
| `RUNNING` | capture glitch | `RUNNING` | send `discontinuity` |
| `RUNNING` | `partial`/`final` for the current session | `RUNNING` | apply §7.4, §8 |
| `RUNNING` | `partial`/`final` for another session | `RUNNING` | drop |
| `RUNNING` | §8.2 violated | `FINALIZING` | send `error stability_violation` (`fatal:false`), abort the session, preserve the document |
| `RUNNING` | hotkey pressed again / silence timeout / stop-on-input | `FINALIZING` | flush pending audio, then send `stop` |
| `RUNNING` | `Esc` | `CANCELLING` | drop queued audio, then send `cancel` |
| `RUNNING` | audio queue full | `DEAD` | send `error overload_audio` (`fatal:true`), tear down |
| `FINALIZING` | `partial`/`final` for the current session | `FINALIZING` | **apply** — this is the point of `stop` |
| `FINALIZING` | `Esc` | `CANCELLING` | send `cancel`; from here on discard all text for this session |
| `FINALIZING` | `stopped` | `IDLE` | commit engine reset |
| `CANCELLING` | `partial`/`final` for the cancelled session | `CANCELLING` | **drop** (fixture 019) |
| `CANCELLING` | `stopped` | `IDLE` | commit engine reset; no injection performed after `cancel` |
| any | `error` `fatal:true` | `DEAD` | tear down, surface, restart child |
| any | broken pipe / EOF | `DEAD` | §10.3 |
| any | L1/L2 error (§3, §4) | `DEAD` | §11.2 |

### 10.3 Child crash

`ERROR_BROKEN_PIPE`, `ERROR_PIPE_NOT_CONNECTED`, EOF mid-frame, or child process exit. The
Client MUST:

1. Move to `DEAD`. Never attempt to reuse the handle or the pipe name.
2. Abort any open session **without injecting anything further**. Text already committed stays
   in the document (that is the invariant); the unstable tail is lost, which is the honest
   outcome — the hypothesis that would have finalised it no longer exists.
3. Reset the commit engine and anchor state.
4. Surface "sidecar stopped" in the tray. Do not silently restart mid-session and pretend
   nothing happened.
5. Reap the child, then respawn with a **new random pipe name** and a **new `session_id`
   space** (fixture 020). Restart backoff is a Client policy matter, not protocol.

A message bearing a pre-crash `session_id` arriving on the new connection is dropped by §7.1.
With a fresh random pipe name this should be impossible; the rule is defence in depth, and the
fixture exists because the cost of getting it wrong is text injected into an unrelated window.

### 10.4 Cancel during final

The race the design cannot remove: the user presses `Esc` at the same instant the Server
commits an utterance. The `final` was written before the `cancel` was read, so ordering on the
pipe does not help. The resolution splits the obligation:

- **Server:** after *reading* `cancel`, emit no further `partial`/`final` for that session;
  always emit `stopped`.
- **Client:** after *writing* `cancel`, discard every subsequent `partial`/`final` for that
  session regardless of content, until `stopped` arrives. `error` and `stopped` are still
  processed.

Fixture 019 is exactly this trace.

### 10.5 Stop before ready

`start` is legal only from `IDLE`. A hotkey press during `LOADING` does not produce a `start`
frame; the Client shows the loading state and either buffers locally or refuses (a UX choice,
not a protocol one). Consequently `stop`/`cancel` before `ready` name no open session, and are
handled by the idempotent rule in §6.1: the Server answers `stopped` with
`reason:"unknown_session"`. A Server MUST NOT treat this as an error, because the Client's
teardown path runs on crash and shutdown paths where its own state may be uncertain.

---

## 11. Error codes

### 11.1 What `fatal` means

Exactly two levels, no third:

- `fatal: true` — **the connection is finished.** The sender will write nothing more. The
  receiver tears down; the Client additionally kills and respawns the child. `session_id` is
  optional (some fatal errors precede any session).
- `fatal: false` — **the named session is aborted; the connection remains usable.** A new
  session may be started afterwards. `session_id` is **required**.

There is deliberately no advisory level. Information that does not abort something belongs in
a log, not on the wire, where it would compete for the control queue with messages that matter.

### 11.2 Reporting a framing-layer error to the peer

A receiver that hits an L1/L2 error has a desynchronised or untrusted stream, so it cannot rely
on being understood. It MAY make one best-effort attempt to write an `error` frame with a
200 ms deadline before closing, and MUST close regardless of whether that write succeeded. It
MUST NOT wait for a reply.

### 11.3 The codes

| `code` | `fatal` | Sent by | Meaning | Required receiver behaviour |
|---|---|---|---|---|
| `proto_unsupported` | true | Server | Offered `proto` not supported | Client: tear down, surface "sidecar protocol mismatch", do not auto-restart in a loop |
| `bad_frame` | true | either | `length` below `MIN_LENGTH_FIELD`, or otherwise unparseable header | Tear down. Never resynchronise. |
| `frame_too_large` | true | either | `length` above `MAX_LENGTH_FIELD` | Tear down without reading the payload |
| `truncated_frame` | true | either | End of stream inside a frame | Tear down; usually detected locally, rarely sendable |
| `unknown_frame_type` | true | either | Type byte not `0x01`/`0x02` | Tear down |
| `bad_utf8` | true | either | Control payload is not well-formed UTF-8 | Tear down. Never substitute U+FFFD. |
| `bad_json` | true | either | Payload is not a conforming JSON object (§4.2) | Tear down |
| `bad_message` | true | either | Valid JSON, invalid schema: missing required field, wrong type, out of range, odd-length audio payload | Tear down |
| `unknown_message` | true | either | `t` not defined in the negotiated proto | Tear down |
| `state_violation` | true | either | Message illegal in the current state (`start` while `RUNNING`, audio before `ready`, anything before `hello_ack`) | Tear down |
| `overload_audio` | true | either | Audio queue exceeded `AUDIO_QUEUE_MAX_MS` or a write exceeded `WRITE_DEADLINE_MS` | Tear down; surface as an overload, never as a transcription problem |
| `overload_control` | true | either | Control queue exceeded `CONTROL_QUEUE_MAX` | Tear down |
| `model_load_failed` | true | Server | Model could not be loaded (missing weights, OOM, import failure) | Client: tear down, surface the `msg` verbatim, do not retry automatically more than once |
| `internal_error` | true | either | Unhandled fault in the peer | Tear down; log the `msg` |
| `stability_violation` | false | Client | Server violated §8.2 | Server: abort the session, emit `stopped`, keep the connection. Count against this backend's ICER. |
| `decode_failed` | false | Server | Backend raised while decoding this session | Client: abort the session, preserve the document, surface a warning, allow a new session |
| `audio_underrun` | false | Server | Not enough audio arrived to decode the session meaningfully | Client: abort the session, surface, allow a new session |

Locally-detected conditions that are **never** wire messages, because there is no peer to tell:
`spawn_timeout`, `handshake_timeout`, `model_load_timeout`, `peer_mismatch`, `pipe_squatted`
(`FILE_FLAG_FIRST_PIPE_INSTANCE` failure). They use the same string identifiers in logs so that
a log reader does not have to learn two vocabularies.

---

## 12. Conformance fixtures

Both test suites — C++ (`tests/`) and Python (`server/`) — read the same files from
`tests/fixtures/protocol/`. Neither suite may carry its own private copy: the entire point is
that a disagreement between the two implementations shows up as a failing shared fixture rather
than as garbled text in someone's document.

### 12.1 Layout

```
tests/fixtures/protocol/
├─ index.json                 # manifest: schema, byte-source encoding, case list
├─ 001-fragmented-reads.json
├─ 002-coalesced-frames.json
└─ …                          # one file per case, id == filename stem
```

`index.json` fields: `schema` (1), `proto` (1), `description`, `byte_source` (a prose
restatement of §12.2, so the file is self-describing), `kinds`, and `cases` — an array of
`{id, file, kind, name, tags}`. A runner enumerates `cases`; it MUST NOT glob the directory,
so that adding a file without registering it is a visible omission.

Every case file carries `schema`, `proto`, `id`, `kind`, `name`, `description`, then
kind-specific content. Unknown fields in a case file are ignored by runners, so a case can
carry extra documentation.

### 12.2 Byte sources

A **byte source** is either:

- a string of lowercase hex digits, even length (`"18000000017b22"`), or
- an object `{"repeat": "<hex unit>", "count": <n>}`, meaning that unit repeated `n` times.

An array of byte sources concatenates them. The `repeat` form exists only so that the 64 KiB
frames in cases 004 and 005 do not put 128 KiB of hex in the repository; decoding it is about
ten lines in each suite.

### 12.3 Kind `framing`

```jsonc
{
  "kind": "framing",
  "input":  { "chunks": [ <byte source>, … ], "eof": true },   // eof optional, default true
  "expect": {
    "frames": [ { "type": 1, "payload_len": 23,
                  "payload_hex": "…",            // or "payload_repeat": {…}
                  "json": { … } } ],             // present only for type 1
    "error":  null | { "code": "…", "fatal": true, "at_offset": 28, "detail": "…" },
    "end_of_stream": true                        // optional
  }
}
```

`chunks` are the successive deliveries a reader would see. A runner MUST feed them one at a
time and MUST NOT concatenate them first — the whole purpose of case 001 is to catch a decoder
that assumes one read yields one frame. Runners SHOULD additionally re-run every framing case
with the chunks concatenated, and with the stream re-split one byte at a time; both must produce
the same frames. `at_offset` is a byte offset from the start of the whole stream, and is
informational: a runner MAY compare it, but the `code` is the assertion that matters.

Frames listed in `expect.frames` MUST be delivered *before* the error, in order. A decoder that
throws away already-decoded frames when it later hits a bad header fails case 006.

### 12.4 Kind `text`

```jsonc
{
  "kind": "text",
  "text": "café ☕", "text_hex": "…", "text_bytes": 9,
  "checks": [ { "stable_bytes": 4, "valid": false, "reason": "inside the 2-byte sequence c3 a9" } ]
}
```

`text_hex` is authoritative; `text` is for reading. A runner asserts that its
`is_valid_stable_offset(text, n)` returns `valid` for every check. `reason` is documentation.

### 12.5 Kind `session`

```jsonc
{
  "kind": "session",
  "initial": { "current_session_id": "s7", "state": "RUNNING", "last_seq": 11,
               "utterance_id": 3, "last_stable_bytes": 0 },
  "messages": [
    { "from": "server", "json": { … }, "expect": { "verdict": "accept" } },
    { "from": "client", "json": { … }, "expect": { "verdict": "sent", "effects": [ … ] } },
    { "from": "transport", "event": "broken_pipe", "expect": { "verdict": "abort_session" } }
  ]
}
```

`from` is `server` (inbound to the Client), `client` (outbound, asserting the Client produces
it), or `transport` (a connection event rather than a message).

| Verdict | Meaning |
|---|---|
| `accept` | Applied normally |
| `accept_after_gap` | Applied; `expect.gap` gives `expected_seq`/`received_seq`/`missing`, `expect.effects` lists `log_gap` and `reset_anchor_confidence` |
| `accept_bookkeeping_only` | State recorded, no text applied, no injection (§7.1) |
| `drop_stale_session` | Dropped: `session_id` is not current |
| `drop_cancelled_session` | Dropped: session was cancelled and `stopped` has not arrived |
| `drop_duplicate_seq` | Dropped: `seq` already seen |
| `drop_out_of_order` | Dropped: `seq` below the high-water mark |
| `protocol_error` | `expect.error` gives the `code`/`fatal`/`detail` the receiver must raise |
| `sent` | For `from: "client"`: the Client must emit exactly this message here |
| `abort_session` / `reconnected` | For `from: "transport"` |

`effects` entries are prose assertions about observable state (`"state -> IDLE"`,
`"no injection performed for s7 after cancel"`). Runners SHOULD assert the ones they can
express and MUST NOT fail a case for an effect they do not model — the field documents intent
across two languages whose internals differ.

### 12.6 The cases

| id | Kind | What it pins |
|---|---|---|
| `001-fragmented-reads` | framing | Chunk boundaries inside the length prefix, inside a payload, across frames |
| `002-coalesced-frames` | framing | Three frames in one read; decoder drains its buffer |
| `003-zero-length-payload` | framing | `length == 1`; legal empty audio frame; resync on the next frame |
| `004-max-size-frame` | framing | `length == 65537`, 65536-byte payload, accepted |
| `005-oversize-frame` | framing | `length == 65538`, rejected on the header, payload never read |
| `006-malformed-length-zero` | framing | `length == 0` after a good frame; earlier frames still reported |
| `007-malformed-length-huge` | framing | `0xFFFFFFFF`; unsigned handling, no allocation before range check |
| `008-invalid-utf8-control` | framing | Lone `0xE9`; `bad_utf8`, no U+FFFD substitution |
| `009-unknown-frame-type` | framing | Type `0x03` is fatal, not skippable |
| `010-clean-eof-at-frame-boundary` | framing | Orderly shutdown is not an error |
| `011-truncated-frame-at-eof` | framing | Partial frame at EOF is `truncated_frame`; never delivered partially |
| `012-offset-splits-utf8-sequence` | text | Offsets inside 2- and 3-byte sequences; end-of-text legal; past-end illegal |
| `013-offset-splits-combining-sequence` | text | Offsets that are valid UTF-8 boundaries and invalid cluster boundaries: combining acute, ZWJ family, regional-indicator pair, keycap + VS16 |
| `014-stale-session-id` | session | `partial`/`final` dropped; `stopped`/`error` bookkeeping-only |
| `015-duplicate-seq` | session | Duplicate and lower `seq` dropped without state change |
| `016-seq-gap` | session | Gap accepted, logged, anchor confidence reset |
| `017-stable-bytes-decreases` | session | Monotonicity violation → `stability_violation`, session aborted, no clamping |
| `018-stable-prefix-mutates` | session | Non-decreasing `stable_bytes` but mutated bytes below the mark → same error |
| `019-cancel-during-final` | session | `final` in flight past a `cancel` is dropped; `stopped` still expected |
| `020-reconnect-after-child-crash` | session | Broken pipe aborts without injection; new session id; ghost of the old session dropped |

---

## 13. Version history

| proto | Date | Status | Changes |
|---|---|---|---|
| 1 | 2026-08-16 | v1 draft — frozen in Phase 2 | Initial specification. Length covers the type byte. Frame types `0x01` control / `0x02` audio, `MAX_PAYLOAD` 64 KiB. `hello`/`hello_ack` handshake. `session_id`/`utterance_id`/`seq` identity. Cumulative `partial.text` with `stable_bytes` in UTF-8 bytes on grapheme boundaries. Bounded queues with explicit overload errors. `discontinuity` added to the Client→Server set; `hello_ack` added to the Server→Client set. |

A new `proto` number is required to change any of: the framing header layout or the meaning of
`length`; the set or meaning of frame type bytes; the size limits; the audio format or its
attribution rule; any required field, its type, or its range; the meaning of `stable_bytes`;
the identity or ordering rules; the `fatal` semantics; the set of message types.

A new `proto` number is **not** required to: add an optional field that a conforming receiver
ignores; add an error `code` (receivers already handle unknown codes by their `fatal` flag —
see below); tighten a receiver's grapheme boundary check toward fuller UAX #29 conformance;
change any constant in §1.1 marked as a recommendation rather than a limit.

Receivers MUST treat an unrecognised `error.code` as a well-formed error and act on `fatal`.
This is the one place where forward tolerance is allowed, because the alternative — a fatal
error about a fatal error — loses the diagnostic that was being delivered.

---

## 14. Open questions

Recorded rather than guessed. Each must be resolved before the Phase 2 freeze.

1. **`GetNamedPipeServerProcessId`.** §2.4 step 5 has the child validate the parent. The
   mirror-image API is believed to exist and to be available from `kernel32` on Windows Vista
   and later, but this document does not assert its exact signature or availability. Confirm
   against the Windows SDK 10.0.26100 headers before implementing; if it is unavailable, the
   child-side check is dropped and this document is amended to say so, rather than the check
   being silently skipped.
2. **Unicode version for UAX #29.** Both endpoints must pin the *same* version of the grapheme
   break property tables, and the tables must live in-repo (Python's `unicodedata` tracks the
   CPython build's Unicode version, which is not under our control and will drift from a
   hand-maintained C++ table). Decide the version, generate both tables from the same source
   file, and record the version here.
3. **`PIPE_READMODE_MESSAGE` from Python.** The sidecar reaches the pipe through `ctypes` and
   `kernel32`; whether `SetNamedPipeHandleState` reliably puts a `CreateFileW` handle into
   message read mode in that environment is untested here. The stream-oriented parser rule in
   §2.2 makes the answer non-load-bearing for correctness, but it affects whether
   `ERROR_MORE_DATA` handling is exercised in practice. Test in Phase 2.
4. **Whether `audio_pos_ms` should be required on `partial`.** It is optional today. Latency
   decomposition (plan.md §3, Phase 2 item 4) may need it on every hypothesis, in which case it
   becomes required — which is a proto bump. Decide during the Phase 2 latency work, before the
   freeze, not after.
5. **Overload thresholds.** `AUDIO_QUEUE_MAX_MS = 3000` and `CONTROL_QUEUE_MAX = 256` are
   reasoned guesses, not measurements. Phase 2 runs streaming backends at real-time cadence and
   should confirm that a healthy system stays an order of magnitude below both.
