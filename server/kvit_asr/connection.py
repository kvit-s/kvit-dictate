# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
"""The connection-scoped session router, and the backend and VAD adapters it needs.

This module exists because four finished modules were each written against a different, plausible
interface, and nothing owned the space between them. The seams, named so that a future reader can
check whether they still exist:

1. ``kvit_asr.server.Server`` drives one **connection-scoped** object for the whole life of the
   pipe: ``handle_control(msg: dict)``, ``handle_audio(pcm)``, ``close()``, and optionally
   ``current_session_id``. ``kvit_asr.session.Session`` is **session-scoped** — one per ``start``,
   dead at ``stopped`` — and speaks ``on_audio`` / ``on_stop`` / ``on_cancel`` /
   ``on_discontinuity`` / ``tick``. :class:`ConnectionSession` is the object in between, and it is
   where every "which session is this for" rule in docs/protocol.md §6.1, §7.1 and §7.2 is
   applied.

2. ``kvit_asr.session.Session`` wants a backend with ``reset`` / ``accept_audio`` / ``finalise``
   returning plain text. ``kvit_asr.backends.base.Backend`` offers ``reset(ResetReason)`` /
   ``feed`` / ``finalize`` and returns :class:`~kvit_asr.backends.base.Hypothesis` objects.
   :class:`LoadedBackend` is both faces of that seam at once — it is also the object
   ``kvit_asr.server`` reads ``model_id`` / ``mode`` / ``chunk_ms`` / ``context_supported`` off to
   build ``ready``.

3. ``kvit_asr.session.Session`` wants a VAD reporting **byte offsets into the chunk just pushed**;
   ``kvit_asr.vad.VadEndpointer`` reports **absolute sample positions**, backdated to where the
   speech actually was, and distinguishes four event kinds where the session knows two.
   :class:`VadAdapter` translates, and §"What the VAD adapter loses" below says exactly what does
   not survive the translation.

Nothing here makes a protocol decision that is not quoted from docs/protocol.md. Zero third-party
imports: the backend package is imported lazily inside :func:`open_backend`, so a missing model
runtime disables one backend rather than this module.

─── What the VAD adapter loses ────────────────────────────────────────────────────────────────

Two things, both stated rather than hidden:

* **Pre-roll.** ``VadEndpointer`` hands back gated audio with ``preroll_ms`` of lead-in already
  attached, because a detector cannot confirm an onset until the onset is over. ``Session`` does
  not consume that audio: it splits the chunk *it* was given at the offsets the VAD reports, so
  audio the VAD would have replayed from before the onset is simply not fed. The practical cost is
  the leading fraction of the first word of an utterance. Fixing it means teaching ``Session`` to
  take the VAD's audio instead of its own, which is a change to a tested module and belongs in the
  same pass as measuring whether it matters.
* **The distinction between a pause and an endpoint.** ``VadEndpointer`` emits ``SPEECH_END`` at
  every hangover and ``UTTERANCE_END`` only when the endpoint timer expires; an utterance survives
  a mid-sentence pause, which is the whole point of plan.md §1.6. ``Session`` closes the utterance
  — and therefore emits ``final`` — on every ``SPEECH_END`` it is told about. So this adapter maps
  ``UTTERANCE_END`` onto the session's ``SPEECH_END`` and **drops** the hangover events entirely.
  The result is exactly ``VadEndpointer.gate_open``: audio flows to the decoder across a short
  pause and the utterance ends at the endpoint. Reporting the hangover events instead would
  finalise a sentence every time the speaker drew breath.
"""

from __future__ import annotations

from typing import Any, Callable, Sequence

from kvit_asr import session as session_mod
from kvit_asr import vad as vad_mod
from kvit_asr.backends.base import ResetReason, StreamingMode
from kvit_asr.config import BackendConfig, SessionConfig as CliSessionConfig
from kvit_asr.stability import StabilityParams

__all__ = ["ConnectionSession", "LoadedBackend", "VadAdapter", "open_backend"]

# The one streaming mode whose backends re-decode the whole utterance and may therefore revise
# text they have already published. Spelled from the enum so the wire value has a single source;
# `LoadedBackend.mode` is the plain string, which is what it is compared against.
_REVISING_MODE = StreamingMode.PSEUDO_STREAMING.value


# ─── Seam 2: the backend ────────────────────────────────────────────────────────────────────


class LoadedBackend:
    """A loaded :class:`~kvit_asr.backends.base.Backend`, wearing both interfaces it is asked for.

    ``kvit_asr.server`` reads the four declaration fields to build ``ready`` (docs/protocol.md
    §6.2); ``kvit_asr.session`` calls ``reset`` / ``accept_audio`` / ``finalise``. One object
    rather than two because the session's backend and the server's backend are the same model, and
    a second wrapper would only be somewhere else for the two to disagree.
    """

    def __init__(self, backend: Any, *, log: Any = None, allow_hostile: bool = False) -> None:
        caps = backend.capabilities
        mode = getattr(caps, "mode", None)

        #: Model identifier **as spelled in models.json** (§6.2). Echoed verbatim, never prettified.
        self.model_id: str = str(getattr(caps, "model_id", "") or "")
        #: `ready.mode` — one of server.VALID_MODES. The server rejects anything else.
        self.mode: str = str(getattr(mode, "value", mode))
        #: `ready.chunk_ms`, or None when the backend has no natural chunk.
        self.chunk_ms: int | None = getattr(caps, "natural_chunk_ms", 0) or None
        #: `ready.context_supported` (§6.1). Declared by the backend, never inferred.
        self.context_supported: bool = bool(getattr(caps, "supports_context", False))

        self._backend = backend
        self._log = log
        self._context: tuple[str, ...] = ()
        self._context_warned = False
        # `reset()` carries no reason across the session.py seam, and base.py is explicit that
        # DISCONTINUITY is "the one with teeth" — a backend that treats it as an ordinary utterance
        # boundary is not wrong, but one that never hears about it cannot choose. The router arms
        # this immediately before Session.on_discontinuity, and the next reset consumes it.
        self._next_reset_is_discontinuity = False
        # `--allow-hostile-backend`. See `_take_override` for what it gates and why the gate is
        # here rather than in the backend.
        self._allow_hostile = bool(allow_hostile)
        self._pending_override: int | None = None
        self._refused_overrides = 0
        # One `window_slide` warning per utterance. See `_note_window_slide`.
        self._slide_warned = False

    # ── Router-facing ───────────────────────────────────────────────────────────────────────

    def begin_session(self, context: Sequence[str] = ()) -> None:
        self._apply_context(context)
        self._slide_warned = False
        self._backend.reset(ResetReason.SESSION_START)

    def end_session(self, cancelled: bool = False) -> None:
        self._backend.reset(ResetReason.CANCEL if cancelled else ResetReason.SESSION_END)

    def note_discontinuity(self) -> None:
        self._next_reset_is_discontinuity = True

    def close(self) -> None:
        self._backend.unload()

    # ── Session-facing (kvit_asr.session.Backend) ───────────────────────────────────────────

    def reset(self, context: Sequence[str] = ()) -> None:
        self._apply_context(context)
        reason = (
            ResetReason.DISCONTINUITY
            if self._next_reset_is_discontinuity
            else ResetReason.UTTERANCE_BOUNDARY
        )
        self._next_reset_is_discontinuity = False
        self._slide_warned = False   # a new utterance gets a fresh window and a fresh warning
        self._backend.reset(reason)

    def accept_audio(self, pcm: bytes) -> str | None:
        """The **newest** hypothesis, never a concatenation of them.

        ``feed`` may return several for one chunk, and each supersedes the previous one for the
        same utterance (docs/protocol.md §8.1). Joining them would turn cumulative text into a
        pile of deltas, which is the one mistake in this area that produces no error anywhere and
        corrupts every document the tool touches.
        """
        hypotheses = self._backend.feed(pcm)
        self._note_window_slide()
        if not hypotheses:
            self._pending_override = None
            return None
        newest = hypotheses[-1]
        self._pending_override = self._take_override(newest)
        return newest.text

    def finalise(self) -> str:
        final = self._backend.finalize()
        self._pending_override = None if final is None else self._take_override(final)
        return final.text if final is not None else ""

    def pending_stable_bytes_override(self) -> int | None:
        """The `stable_bytes` the newest hypothesis demanded, or None for the normal case.

        `kvit_asr.session.Session` calls this immediately after `accept_audio`/`finalise` — see
        that module's `Backend` protocol, where it is documented as optional. Optional rather than
        required because it is a test hook: a backend that never lies is the normal case, and a
        session driving a backend that does not offer this method must behave exactly as before.
        """
        return self._pending_override

    # ── internals ───────────────────────────────────────────────────────────────────────────

    def _note_window_slide(self) -> None:
        """Say so, once per utterance, when audio has scrolled out of a sliding window.

        `PseudoStreamingBackend` keeps only the last `window_ms` of speech. Past that it freezes
        the text whose audio has gone and stitches the rest on by word overlap — and its own
        docstring is blunt about the failure mode: *"a model that repunctuates or recases across
        the boundary produces no exact overlap at all, in which case the fallback freezes the
        entire pre-slide transcript"*, and *"the frozen prefix is not 'stable' in the §1.4 sense
        -- no stability computation blessed it -- it is merely unrecoverable."*

        Qwen is exactly such a model; that is the §3 finding. So this is a real quality cliff that
        the stability machinery cannot protect the document from, and until now it was crossed in
        complete silence -- no message, no log line, nothing distinguishing a mis-stitched
        sentence from a badly transcribed one. That is the failure mode this codebase keeps
        rediscovering, so: one line, at WARN, naming the window it belongs to.

        Once per utterance, not per hop: a long utterance slides on every hop thereafter, and a
        line per hop would bury the first one that mattered.
        """
        if self._log is None or self._slide_warned:
            return
        frozen = getattr(self._backend, "frozen_prefix", "")
        if not frozen:
            return
        self._slide_warned = True
        window_ms = getattr(self._backend, "window_ms", 0)
        # The text itself is not logged: recognised text never enters the diagnostic channel
        # (plan.md §1.9). Its size is what says how much is now unrevisable.
        self._log.warning(
            "window_slide",
            window_ms=window_ms,
            frozen_bytes=len(frozen.encode("utf-8")),
            detail=(
                "speech has outrun the re-transcription window; text before this point can no "
                "longer be revised and was stitched on by word overlap, which is unreliable for "
                "a model that recases or repunctuates across the boundary"
            ),
        )

    def _take_override(self, hypothesis: Any) -> int | None:
        """`Hypothesis.stable_bytes_override`, if this server was told it may honour one.

        The gate lives here, on the server's side of the seam, because that is what
        `backends.base.Hypothesis.stable_bytes_override` asks for: *"The server SHOULD refuse to
        honour this unless it was started with an explicit allow-hostile-backend flag, and MUST
        log loudly when it does."* A backend can therefore always *offer* one — the mock's hostile
        modes do it unconditionally — and it reaches the wire only when an operator asked for it.

        Both branches are logged, and the refusal branch matters more than it looks: a hostile
        mock configured without the flag would otherwise behave indistinguishably from a
        well-behaved one, and a fixture run that proved nothing would look exactly like a fixture
        run that passed.
        """
        override = getattr(hypothesis, "stable_bytes_override", None)
        if override is None:
            return None

        if not self._allow_hostile:
            self._refused_overrides += 1
            if self._refused_overrides == 1 and self._log is not None:
                self._log.error(
                    "hostile_override_refused",
                    model=self.model_id,
                    stable_bytes=int(override),
                    detail="the backend asked to publish a stable_bytes the protocol forbids; "
                    "start the sidecar with --allow-hostile-backend if that was the intent",
                )
            return None

        if self._log is not None:
            # Every one, not the first: each is a deliberately non-conforming frame on the wire,
            # and a run's log is the only record of which frames were manufactured.
            self._log.error(
                "hostile_override_honoured",
                model=self.model_id,
                stable_bytes=int(override),
                hostile=bool(getattr(hypothesis, "hostile", False)),
                detail="emitting a stable_bytes docs/protocol.md §8.2 forbids, because "
                "--allow-hostile-backend was given",
            )
        return int(override)

    def _apply_context(self, context: Sequence[str]) -> None:
        terms = tuple(context or ())
        if terms == self._context:
            return
        self._context = terms
        if not self.context_supported:
            # §6.1: a Server without context support MUST ignore `start.context` and MUST report
            # context_supported:false — which `ready` already did. Ignoring it here is therefore
            # the conforming behaviour; saying so once is what stops it being invisible.
            if terms and not self._context_warned and self._log is not None:
                self._context_warned = True
                self._log.info("context_ignored", terms=len(terms), model=self.model_id)
            return
        self._backend.set_context(list(terms))


def _coerce_option(value: str) -> Any:
    """`--backend-opt KEY=VALUE` values arrive as strings; constructor keywords are typed."""
    low = value.strip().lower()
    if low in ("true", "false"):
        return low == "true"
    if low == "none":
        return None
    for cast in (int, float):
        try:
            return cast(value)
        except ValueError:
            pass
    return value


def open_backend(
    backend_cfg: BackendConfig,
    on_progress: Callable[..., None] | None = None,
    log: Any = None,
    allow_hostile: bool = False,
) -> LoadedBackend:
    """`kvit_asr.server.Server`'s default ``backend_loader``: resolve, construct, load, wrap.

    Imports ``kvit_asr.backends`` **here** rather than at module scope, which is the rule that
    makes a missing runtime disable one backend instead of the server: an ImportError raised from
    inside this call becomes a `model_load_failed` error frame the parent can display, while the
    same import at module scope would take the process down before the handshake.

    A backend class MAY expose ``from_backend_config(cfg)``; that is the hook for a backend with
    its own config dataclass (sherpa-onnx, zipformer, both Whispers). Otherwise it is constructed
    from ``--backend-opt`` keywords, which is what ``MockBackend(**overrides)`` accepts.

    ``log`` is threaded through rather than defaulted away because two of :class:`LoadedBackend`'s
    obligations are *to log*: a `start.context` this backend cannot use is ignored (§6.1) and an
    ignored hint that says nothing is invisible, and an honoured ``stable_bytes_override`` MUST be
    logged loudly. Both were unreachable while nothing passed a logger.

    ``allow_hostile`` is ``ServerConfig.allow_hostile_backend``, a server policy rather than a
    backend option — see that field.
    """
    from kvit_asr.backends import load_backend  # noqa: PLC0415 — deliberately lazy, see above

    cls = load_backend(backend_cfg.id)
    factory = getattr(cls, "from_backend_config", None)
    if callable(factory):
        backend = factory(backend_cfg)
    else:
        backend = cls(**{k: _coerce_option(v) for k, v in (backend_cfg.options or {}).items()})

    backend.load(progress=on_progress)
    return LoadedBackend(backend, log=log, allow_hostile=allow_hostile)


# ─── Seam 3: the VAD ────────────────────────────────────────────────────────────────────────


class VadAdapter:
    """`kvit_asr.vad.VadEndpointer` seen through the interface ``kvit_asr.session.Session`` asks for.

    See the module docstring for what this translation loses. What it keeps is the only thing the
    session needs: where, inside the chunk it just handed over, an utterance began and ended.
    """

    def __init__(self, endpointer: Any) -> None:
        self._vad = endpointer
        # Stream position of the start of the next chunk, in the endpointer's own sample clock.
        # It has to be *its* clock, because that is what `VadEvent.sample` is measured in.
        self._pos_samples = int(endpointer.position_samples)

    def reset(self) -> None:
        """Between sessions. `VadEndpointer.reset` keeps its position clock and drops the partial
        frame it was holding, so the adapter re-reads the clock rather than assuming it restarted —
        assuming zero here would put every subsequent offset a session's worth of audio out."""
        self._vad.reset()
        self._pos_samples = int(self._vad.position_samples)

    def push(self, pcm: bytes) -> list[session_mod.VadEvent]:
        chunk_start = self._pos_samples
        self._pos_samples += len(pcm) // 2
        result = self._vad.process(pcm)

        events: list[session_mod.VadEvent] = []
        floor = 0  # offsets must not go backwards: Session walks them with a single cursor
        for ev in result.events:
            if ev.kind is vad_mod.VadEventKind.SPEECH_START:
                kind = session_mod.SPEECH_START
            elif ev.kind is vad_mod.VadEventKind.UTTERANCE_END:
                kind = session_mod.SPEECH_END
            else:
                # SPEECH_END is the hangover, not the endpoint (see the module docstring), and
                # DISCONTINUITY reaches the session through its own `discontinuity` message.
                continue
            # `ev.sample` is where the speech actually was, which for an onset is deliberately
            # *before* the frame that detected it and can therefore precede this chunk entirely.
            # Clamping to the chunk is the coarser answer session.py's VadEvent documents as
            # acceptable: "a VAD that can only answer per-chunk reports offset = 0 for a start".
            offset = (int(ev.sample) - chunk_start) * 2
            offset = max(floor, min(offset, len(pcm)))
            floor = offset
            events.append(session_mod.VadEvent(kind, offset))
        return events


def make_vad(cli: CliSessionConfig, log: Any = None) -> VadAdapter | None:
    """The VAD for one connection, or None when the configuration asks for no gating.

    One per connection, not one per session: `create_vad` probes for Silero and logs a downgrade
    when it is not there, and a probe per hotkey press would turn one honest warning into a stream
    of them. `VadAdapter.reset` is what separates one session from the next.

    ``--vad none`` is a real choice, not a failure mode: without a VAD every sample is speech, the
    session is one long utterance and `final` arrives only at `stop`. That is what the bake-off
    needs when it measures what the VAD is worth (plan.md §4), and it is the honest fallback when
    the configured detector cannot be built.
    """
    mode = str(getattr(cli, "vad", "auto") or "auto").lower()
    if mode == "none":
        return None
    cfg = vad_mod.VadConfig(
        endpoint_silence_ms=float(getattr(cli, "endpoint_silence_ms", 800)),
        backend=mode,
        silero_model_path=getattr(cli, "silero_model_path", None),
    )
    try:
        return VadAdapter(vad_mod.create_vad(cfg))
    except Exception as exc:
        # plan.md §1.9 forbids downloading the Silero weights, so "silero was asked for and is not
        # here" is an ordinary state on a fresh machine. Degrade to no gating and say so — a
        # session with no endpointing still transcribes, and a server that refuses to start does
        # not.
        if log is not None:
            log.error("vad_unavailable", mode=mode, reason=repr(exc))
        return None


# ─── Seam 1: the connection-scoped session router ───────────────────────────────────────────


class ConnectionSession:
    """One per connection. Owns *which* session is open and hands the rest to `session.Session`.

    Every rule below is quoted from docs/protocol.md rather than chosen here:

    * ``start`` while a session is open → fatal ``state_violation`` (§7.2: sessions do not nest
      and are not concurrent; the pipe is not a multiplexer).
    * ``stop`` / ``cancel`` naming a session that is not open → ``stopped`` with
      ``reason:"unknown_session"``, and **not** an error (§6.1's idempotence rule, §10.5). The
      Client's teardown path runs on crash paths where its own state is uncertain.
    * audio with no session open → discarded and counted, never buffered for the next session
      (§6.3's Server obligation).
    * a non-fatal peer ``error`` naming the open session → that session is abandoned, the
      connection is not (§11.1).

    **Threading.** Everything here runs on the server's reader thread, which is what makes it the
    single decode thread ``kvit_asr.session`` requires — with the one exception that module
    documents, ``on_cancel``, which is safe from any thread and is reached from this same one
    anyway.
    """

    def __init__(self, *, backend: Any, send: Callable[[dict], None], config: Any,
                 log: Any = None, clock: Callable[[], int] | None = None) -> None:
        self._backend = backend
        self._send = send
        self._cfg = config
        self._log = log
        # The stability dwell (plan.md §1.4 step 5) is a wall clock, so the default is the real
        # one. It is injectable because a test that feeds a session's worth of audio in a
        # microsecond would otherwise measure nothing but its own speed: every candidate would
        # still be inside its hold and `stable_bytes` would be 0 forever, which looks like a
        # passing pipeline that commits nothing.
        self._clock = clock
        self._session: Any = None
        self._vad = make_vad(getattr(config, "session", None) or CliSessionConfig(), log)
        self.discarded_audio_bytes = 0

    # ── What the server reads off us ────────────────────────────────────────────────────────

    @property
    def current_session_id(self) -> str | None:
        s = self._session
        return s.session_id if s is not None and not s.ended else None

    @property
    def connection_fatal(self) -> bool:
        """True once a session has reported a §9/§11.1 fault that ends the **connection**.

        The session has already written the `error` frame (`fatal:true`, no `stopped` — there is
        no session left to acknowledge). All that remains is for the server to stop reading, which
        it does by checking this.
        """
        s = self._session
        return bool(s is not None and getattr(s, "connection_fatal", False))

    # ── Inbound ─────────────────────────────────────────────────────────────────────────────

    def handle_control(self, msg: dict) -> None:
        t = msg.get("t")
        if t == "start":
            self._start(msg)
        elif t == "stop":
            self._stop(msg)
        elif t == "cancel":
            self._cancel(msg)
        elif t == "discontinuity":
            self._discontinuity(msg)
        elif t == "error":
            self.abort(str(msg.get("session_id") or ""))
        else:  # pragma: no cover - the decoder rejects unknown types, and server.py the rest
            from kvit_asr.server import ProtocolError_new

            raise ProtocolError_new("unknown_message", True, f"unknown control message {t!r}")

    def handle_audio(self, pcm: bytes) -> None:
        session = self._session
        if session is None or session.ended:
            # §6.3: losing the race between a `stop` and an in-flight audio frame is a Client bug
            # this rule contains rather than escalates.
            self.discarded_audio_bytes += len(pcm)
            return
        session.on_audio(pcm)
        # The tick is load-bearing, not an optimisation: plan.md §1.4 step 5 requires a candidate
        # to be *unchanged* for T ms, and Session only re-evaluates when a new hypothesis arrives.
        # Without a tick, a speaker who keeps talking keeps restarting the dwell and nothing is
        # ever eligible to commit. Driving it from the audio path rather than from a timer thread
        # is what keeps the decode path single-threaded (session.py's threading note): the Client
        # streams the microphone continuously for the whole session, silence included, so audio
        # arrival is a finer-grained clock than the T/2 timer would be.
        session.tick()

    def abort(self, session_id: str) -> None:
        session = self._session
        if session is not None and session.session_id == session_id and not session.ended:
            session.on_cancel()
            self._backend.end_session(cancelled=True)

    def close(self) -> None:
        session = self._session
        if session is not None and not session.ended:
            session.on_cancel()
            self._backend.end_session(cancelled=True)

    # ── internals ───────────────────────────────────────────────────────────────────────────

    def _start(self, msg: dict) -> None:
        open_id = self.current_session_id
        if open_id is not None:
            from kvit_asr.server import ProtocolError_new

            raise ProtocolError_new(
                "state_violation",
                True,
                f"start for {msg.get('session_id')!r} while {open_id!r} is open "
                "(docs/protocol.md §7.2)",
            )

        cli = getattr(self._cfg, "session", None) or CliSessionConfig()
        context = tuple(msg.get("context") or ())
        self._backend.begin_session(context)
        if self._vad is not None:
            self._vad.reset()
        self._session = session_mod.Session(
            str(msg["session_id"]),
            emit=self._send,
            backend=self._backend,
            vad=self._vad,
            config=session_mod.SessionConfig(
                sample_rate=int(msg.get("sample_rate", 16000)),
                stability=StabilityParams(
                    hypotheses_n=cli.stability_n,
                    retract_clusters_m=cli.stability_m,
                    hold_ms_t=cli.stability_hold_ms,
                ),
                context=context,
                # Read off the backend's own declaration, not a setting. `pseudo-streaming` is
                # defined in backends/base.py as "a batch model driven by a sliding or growing
                # window **that we built**" — which is exactly the class of backend that replaces
                # its previous hypothesis instead of extending it. See
                # `SessionConfig.publish_stable_partials` for why no stability tuning substitutes
                # for this. A backend that mislabels its mode gets the wrong answer here, which is
                # one more reason base.py calls the field "not decoration".
                publish_stable_partials=(self._backend.mode != _REVISING_MODE),
            ),
            **({} if self._clock is None else {"clock": self._clock}),
        )
        if self._log is not None:
            self._log.info(
                "session_start",
                session_id=str(msg["session_id"]),
                context=len(context) or None,
            )

    def _for(self, msg: dict) -> Any:
        session = self._session
        sid = str(msg.get("session_id"))
        if session is None or session.session_id != sid or session.ended:
            # §6.1 / §10.5: idempotent with respect to unknown or already-ended sessions. Not an
            # error — `stop` before `ready` names no session at all and is legal.
            self._send({"t": "stopped", "session_id": sid, "reason": "unknown_session"})
            return None
        return session

    def _stop(self, msg: dict) -> None:
        session = self._for(msg)
        if session is not None:
            session.on_stop()
            self._backend.end_session()

    def _cancel(self, msg: dict) -> None:
        session = self._for(msg)
        if session is not None:
            session.on_cancel()
            self._backend.end_session(cancelled=True)

    def _discontinuity(self, msg: dict) -> None:
        session = self._session
        if session is None or session.ended:
            return
        # §6.1: the Server MUST accept this and MUST NOT treat it as fatal.
        self._backend.note_discontinuity()
        session.on_discontinuity(int(msg.get("gap_ms", 0)), msg.get("reason"))
