"""Command line, backend configuration, model-directory resolution, and the honest defaults.

Stdlib only, and deliberately so: this module is imported before anything else in the sidecar,
including before we know whether the selected backend's runtime is installed at all. A missing
`onnxruntime` must disable one backend, not stop the process from being able to say so on the
wire (plan.md §1.1, and the repository rule that only backend modules may import third-party
packages).

It also imports nothing from `kvit_asr.protocol`. Argument parsing must work even if the
protocol module is broken, because the resulting `--help` and usage errors are how a human
debugs a sidecar that will not start. The one protocol-derived default (`--proto`) is therefore
left as `None` here and resolved by `kvit_asr.server` against `protocol.PROTO_VERSION`.

──────────────────────────────────────────────────────────────────────────────────────────────
NETWORK POLICY — normative, not aspirational (plan.md §1.9)

The sidecar performs **no network access at any point during operation**: no telemetry, no cloud
fallback, and above all **no model auto-download**. Model fetching is a separate, explicit,
user-initiated action performed by a human running documented commands (see server/README.md).
`apply_offline_policy()` below sets the offline environment variables that the common ML
libraries honour *before* any backend is imported. That is a guard rail, not a sandbox: it
stops the accidental download that a library performs on a cache miss, and it cannot stop a
library that opens a socket regardless. The real enforcement is that nothing in this package
opens a socket, and that a reviewer can check that claim by grepping for `socket`, `urllib`,
`http` and `requests` in `kvit_asr/` — there are no hits outside this comment.
"""

from __future__ import annotations

import argparse
import os
from dataclasses import dataclass, field
from pathlib import Path

# The build id sent as `hello_ack.server` and used in logs. Diagnostics only — docs/protocol.md
# §6.2 says it is never parsed for behaviour. Kept here rather than read from package metadata
# because the sidecar must run from a source checkout that was never `pip install`ed (the mock
# environment is created with no packages at all).
SERVER_BUILD_ID = "kvit-asr/0.1"

# ─── Defaults, each with the plan.md section that chose it ──────────────────────────────────
#
# These are *defaults*, not constants: the Phase 2 sweep in plan.md §4 varies the stability
# triple and the chunk size, so every one of them is reachable from the command line and is
# recorded in the `ready`/log stream so a bench result can name the configuration that produced
# it.

DEFAULT_ENDPOINT_SILENCE_MS = 800  # plan.md §1.6 — ends an *utterance*, never the session.
DEFAULT_STABILITY_N = 2  # plan.md §1.4 step 1 — hypotheses in the longest-common-prefix window
DEFAULT_STABILITY_M = 3  # plan.md §1.4 step 3 — clusters retracted from the decode head
DEFAULT_STABILITY_HOLD_MS = 200  # plan.md §1.4 step 5 — unchanged-for-T before it is stable

# Session silence (plan.md §1.6, 120 s default, `never` permitted) is deliberately absent: it is
# a Client-side timer. The sidecar has no opinion about how long a user may think, and giving it
# one would put the same policy in two places.

_LOG_LEVELS = ("debug", "info", "warning", "error")

# Model-id → directory-name spellings we will look for under a model directory. The HF hub cache
# uses `models--org--name`; a hand-unpacked download is usually `org/name` or just `name`.
# Checked in this order, first hit wins.
_MODEL_DIR_PATTERNS = ("{org}/{name}", "models--{org}--{name}", "{org}--{name}", "{name}")


@dataclass(frozen=True)
class BackendConfig:
    """Everything a backend module needs, and nothing about transport or protocol.

    `options` carries `--backend-opt KEY=VALUE` pairs verbatim as strings. Coercion is the
    backend's job: only the backend knows whether `providers` is a list or `int8` is a bool, and
    a central coercion table here would have to be edited every time a backend gains an option,
    which is the coupling the one-backend-per-process design exists to avoid.
    """

    id: str
    model: str | None = None
    model_dir: Path | None = None
    chunk_ms: int | None = None
    decoding: str | None = None
    device: str = "auto"
    options: dict[str, str] = field(default_factory=dict)


@dataclass(frozen=True)
class SessionConfig:
    """VAD/stability knobs. Consumed by `kvit_asr.session`; parsed here so that one module owns
    the command line."""

    endpoint_silence_ms: int = DEFAULT_ENDPOINT_SILENCE_MS
    stability_n: int = DEFAULT_STABILITY_N
    stability_m: int = DEFAULT_STABILITY_M
    stability_hold_ms: int = DEFAULT_STABILITY_HOLD_MS

    # Which endpointer, per `kvit_asr.vad.create_vad`: "auto" (Silero if it loads, else the stdlib
    # energy fallback with a warning), "silero" (or fail — for the bake-off, where a silent
    # downgrade would change what is being measured), "energy", or "none".
    #
    # "none" is a measurement arm, not an off switch: with no VAD every sample is speech, the
    # session is one long utterance and `final` arrives only at `stop`. plan.md §4 wants that
    # comparison available.
    vad: str = "auto"

    # Absolute path to silero_vad.onnx. Nothing downloads it (plan.md §1.9); an absent file is
    # reported with the path and, under "auto", falls back.
    silero_model_path: str | None = None


@dataclass(frozen=True)
class ServerConfig:
    pipe_name: str
    proto: int | None = None  # None = "whatever this build supports"; resolved in server.py
    parent_pid: int | None = None
    log_level: str = "info"
    log_transcripts: bool = False
    backend: BackendConfig = field(default_factory=lambda: BackendConfig(id="mock"))
    session: SessionConfig = field(default_factory=SessionConfig)

    allow_hostile_backend: bool = False
    """Permit a backend to force a `stable_bytes` the protocol forbids onto the wire.

    `backends.base.Hypothesis.stable_bytes_override` is the hook, and it exists because
    docs/protocol.md §8.3 — what a Client does when a Server retracts already-published stable
    text — is otherwise **untestable end to end**: stability is the server's own computation
    (plan.md §1.4), and no correct pipeline can produce the input §8.3 handles.

    A server-level policy and deliberately **not** a `--backend-opt`. Those are supplied by
    whoever names the backend and are read by the backend itself, so routing this through them
    would let a backend authorise its own protocol violations. This flag is the operator saying
    "I know, I asked for it", which is exactly what base.py's field docstring requires ("The
    server SHOULD refuse to honour this unless it was started with an explicit
    allow-hostile-backend flag, and MUST log loudly when it does")."""


class _RefuseNetworkAction(argparse.Action):
    """`--port`/`--host` exist only to fail loudly.

    plan.md §1.2 chose a named pipe specifically so there is no firewall prompt on first run
    (which reads badly for a privacy tool) and no port collision. If a future launcher grows a
    `--port`, the failure should name the decision rather than produce an "unrecognised
    arguments" message that invites someone to add the flag.
    """

    def __init__(self, option_strings, dest, **kwargs):
        super().__init__(option_strings, dest, nargs="?", help=argparse.SUPPRESS, **kwargs)

    def __call__(self, parser, namespace, values, option_string=None):
        parser.error(
            f"{option_string}: the sidecar never listens on a network port. Transport is a "
            "named pipe whose per-launch random name is passed with --pipe (plan.md §1.2, "
            "docs/protocol.md §2.1)."
        )


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="python -m kvit_asr",
        description=(
            "kvit-dictate ASR sidecar. Launched by kvit-dictate.exe, which creates the named "
            "pipe, generates its per-launch random name, and passes that name here. Running it "
            "by hand is a debugging aid only; see server/README.md."
        ),
        epilog=(
            "Privacy: recognised text is NEVER written to the log unless --log-transcripts is "
            "given, which is off by default and should stay off outside a debugging session "
            "(plan.md §1.9). The sidecar makes no network access and downloads no models."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    t = p.add_argument_group("transport")
    t.add_argument(
        "--pipe",
        required=True,
        metavar=r"\\.\pipe\kvit-dictate-<32 hex>",
        help="Full name of the named pipe created by the parent. Per-launch and unpredictable "
        "(docs/protocol.md §2.2); never hard-code it.",
    )
    t.add_argument(
        "--proto",
        type=int,
        default=None,
        help="Protocol version the parent intends to speak. Advisory: the authoritative check "
        "is against the `proto` field of the `hello` message (docs/protocol.md §5). Default: "
        "this build's version.",
    )
    t.add_argument(
        "--parent-pid",
        type=int,
        default=None,
        help="PID of the launching process. If given, the sidecar checks it against "
        "GetNamedPipeServerProcessId and exits non-zero on mismatch (docs/protocol.md §2.4 "
        "step 5). Best effort — see the note in server.py about that API being an open "
        "question in docs/protocol.md §14.",
    )
    t.add_argument("--port", action=_RefuseNetworkAction)
    t.add_argument("--host", action=_RefuseNetworkAction)

    b = p.add_argument_group("backend (exactly one per process — see server.py)")
    b.add_argument(
        "--backend",
        required=True,
        help="Backend id, e.g. 'mock', 'sherpa-onnx', 'transformers-whisper'. One backend per "
        "process: changing model means the parent restarts this process under the interpreter "
        "named in models.json.",
    )
    b.add_argument("--model", default=None, help="Model id or path, as spelled in models.json.")
    b.add_argument(
        "--model-dir",
        default=None,
        type=Path,
        help="Directory holding unpacked model files. Default: $KVIT_MODEL_DIR, else the "
        "repository's models/ directory if it exists. Nothing is ever downloaded.",
    )
    b.add_argument("--chunk-ms", type=int, default=None, help="Backend chunk size, if it has one.")
    b.add_argument(
        "--decoding",
        default=None,
        help="Decoding method, e.g. greedy_search / modified_beam_search. Backend-specific.",
    )
    b.add_argument(
        "--device",
        default="auto",
        choices=("auto", "cpu", "cuda", "dml"),
        help="Compute device hint. 'auto' lets the backend choose and REPORT what it chose; "
        "bench/CANDIDATES.md §6.1 records that onnxruntime-gpu on this Blackwell card is "
        "unverified, so a silent CPU fallback must be visible in the log. (default: auto)",
    )
    b.add_argument(
        "--backend-opt",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="Extra backend option, repeatable. Passed through as strings.",
    )
    b.add_argument(
        "--allow-hostile-backend",
        action="store_true",
        help="Permit the backend to force an illegal `stable_bytes` onto the wire, so that the "
        "Client's docs/protocol.md §8.3 violation handling can be exercised end to end. Every "
        "honoured override is logged at ERROR. TESTING ONLY: with this on, the sidecar knowingly "
        "emits non-conforming messages. Separate from --backend-opt on purpose — a backend must "
        "not be able to authorise its own protocol violations.",
    )

    s = p.add_argument_group("session tuning (plan.md §1.4, §1.6 — swept in Phase 2)")
    s.add_argument("--endpoint-silence-ms", type=int, default=DEFAULT_ENDPOINT_SILENCE_MS)
    s.add_argument(
        "--vad",
        default="auto",
        choices=("auto", "silero", "energy", "none"),
        help="Endpointer backend. 'none' disables gating entirely, which makes the whole session "
        "one utterance — a measurement arm (plan.md §4), not an off switch. (default: auto)",
    )
    s.add_argument(
        "--silero-model",
        default=None,
        metavar="PATH",
        help="Absolute path to silero_vad.onnx. Never downloaded (plan.md §1.9).",
    )
    s.add_argument("--stability-n", type=int, default=DEFAULT_STABILITY_N)
    s.add_argument("--stability-m", type=int, default=DEFAULT_STABILITY_M)
    s.add_argument("--stability-hold-ms", type=int, default=DEFAULT_STABILITY_HOLD_MS)

    d = p.add_argument_group("diagnostics")
    d.add_argument("--log-level", default="info", choices=_LOG_LEVELS)
    d.add_argument(
        "--log-transcripts",
        action="store_true",
        help="OFF BY DEFAULT. Include recognised text in the stderr log. Only for debugging a "
        "specific transcription fault: it writes what the user said into whatever captures "
        "stderr, which plan.md §1.9 forbids by default. Without it, text fields are logged as "
        "a byte count only.",
    )
    return p


def parse_backend_opts(pairs: list[str]) -> dict[str, str]:
    out: dict[str, str] = {}
    for item in pairs:
        key, sep, value = item.partition("=")
        if not sep or not key:
            raise ValueError(f"--backend-opt expects KEY=VALUE, got {item!r}")
        if key in out:
            # A repeated key is ambiguous (override? append?), and guessing which the caller
            # meant is exactly the kind of silent behaviour this project logs rather than infers.
            raise ValueError(f"--backend-opt {key!r} given more than once")
        out[key] = value
    return out


def parse_args(argv: list[str] | None = None) -> ServerConfig:
    """Parse the command line into a ServerConfig. Exits (SystemExit 2) on a usage error."""
    parser = build_parser()
    ns = parser.parse_args(argv)

    try:
        options = parse_backend_opts(ns.backend_opt)
    except ValueError as exc:
        parser.error(str(exc))

    for name in ("chunk_ms", "endpoint_silence_ms", "stability_hold_ms"):
        value = getattr(ns, name)
        if value is not None and value < 0:
            parser.error(f"--{name.replace('_', '-')} must be >= 0")
    for name in ("stability_n", "stability_m"):
        if getattr(ns, name) < 1:
            parser.error(f"--{name.replace('_', '-')} must be >= 1")
    if ns.proto is not None and not (1 <= ns.proto <= 65535):
        parser.error("--proto must be in 1..65535 (docs/protocol.md §6.1)")

    return ServerConfig(
        pipe_name=ns.pipe,
        proto=ns.proto,
        parent_pid=ns.parent_pid,
        log_level=ns.log_level,
        log_transcripts=ns.log_transcripts,
        backend=BackendConfig(
            id=ns.backend,
            model=ns.model,
            model_dir=resolve_model_dir(ns.model_dir),
            chunk_ms=ns.chunk_ms,
            decoding=ns.decoding,
            device=ns.device,
            options=options,
        ),
        session=SessionConfig(
            endpoint_silence_ms=ns.endpoint_silence_ms,
            stability_n=ns.stability_n,
            stability_m=ns.stability_m,
            stability_hold_ms=ns.stability_hold_ms,
            vad=ns.vad,
            silero_model_path=ns.silero_model,
        ),
        allow_hostile_backend=ns.allow_hostile_backend,
    )


# ─── Model location ─────────────────────────────────────────────────────────────────────────
#
# Resolution never creates, downloads, or writes anything. It answers one question — "is there
# a directory on this disk that looks like this model?" — and returns None when the answer is
# no. The backend turns that None into a `model_load_failed` error naming the paths it tried,
# which is a message a human can act on; a download would be a policy violation and a several-
# gigabyte surprise in the middle of a dictation session.


def repo_root() -> Path:
    """The kvit-dictate checkout, derived from this file's location: server/kvit_asr/config.py."""
    return Path(__file__).resolve().parents[2]


def resolve_model_dir(explicit: Path | None = None) -> Path | None:
    """--model-dir, else $KVIT_MODEL_DIR, else <repo>/models if it exists, else None."""
    if explicit is not None:
        return Path(explicit).expanduser()
    env = os.environ.get("KVIT_MODEL_DIR")
    if env:
        return Path(env).expanduser()
    default = repo_root() / "models"
    return default if default.is_dir() else None


def hf_home() -> Path | None:
    """The shared HuggingFace cache, if the machine defines one.

    plan.md §5.2 sets `HF_HOME=c:\\apps\\llama-swap\\gguf\\hf` globally so weights are shared
    regardless of how many environments exist and nothing is downloaded twice. We *read* it; we
    never populate it.
    """
    env = os.environ.get("HF_HOME")
    return Path(env).expanduser() if env else None


def candidate_model_paths(cfg: BackendConfig) -> list[Path]:
    """Every path we would accept for `cfg.model`, in priority order.

    Returned even when they do not exist, because the useful error message is "I looked here,
    here and here" rather than "not found".
    """
    if not cfg.model:
        return []
    model = cfg.model
    out: list[Path] = []

    direct = Path(model).expanduser()
    # An absolute path, or a relative one that exists from the current directory, is taken
    # literally. models.json may legitimately name a directory rather than a hub id.
    if direct.is_absolute() or os.sep in model or (os.altsep and os.altsep in model):
        out.append(direct)

    org, _, name = model.rpartition("/")
    roots = [d for d in (cfg.model_dir, hf_home() / "hub" if hf_home() else None) if d]
    for root in roots:
        for pattern in _MODEL_DIR_PATTERNS:
            if "{org}" in pattern and not org:
                continue
            out.append(root / pattern.format(org=org, name=name))

    # Deduplicate, preserving order.
    seen: set[Path] = set()
    unique: list[Path] = []
    for path in out:
        if path not in seen:
            seen.add(path)
            unique.append(path)
    return unique


def resolve_model_path(cfg: BackendConfig) -> Path | None:
    """First existing entry of `candidate_model_paths`, or None. Never downloads."""
    for path in candidate_model_paths(cfg):
        if path.exists():
            return path
    return None


# ─── Offline enforcement ────────────────────────────────────────────────────────────────────

#: Environment variables set before any backend import. Values are the documented
#: "do not touch the network" settings for the libraries our backends may load. They are set
#: unconditionally rather than only when unset: a caller that wants an online download must do
#: it in a separate, deliberate step outside this process.
OFFLINE_ENV = {
    "HF_HUB_OFFLINE": "1",
    "TRANSFORMERS_OFFLINE": "1",
    "HF_HUB_DISABLE_TELEMETRY": "1",
    "HF_HUB_DISABLE_IMPLICIT_TOKEN": "1",
    "NO_PROXY": "*",
}


def apply_offline_policy(env: dict[str, str] | None = None) -> dict[str, str]:
    """Apply OFFLINE_ENV to `env` (default `os.environ`) and return what changed.

    Honest about its limits: these variables are honoured by huggingface_hub and transformers
    because those libraries document them. Whether every library a future backend pulls in
    honours something equivalent is *not* verified, and this function cannot make it so. It
    reduces the accidental-download surface; it is not a network sandbox.
    """
    target = os.environ if env is None else env
    changed: dict[str, str] = {}
    for key, value in OFFLINE_ENV.items():
        if target.get(key) != value:
            changed[key] = value
        target[key] = value
    return changed
