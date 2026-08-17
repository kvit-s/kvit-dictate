"""The shared cross-language conformance fixtures, driven against `kvit_asr.protocol`.

`tests/fixtures/protocol/` is read by **both** test suites — this one and the C++ one under
`tests/`. That is the entire point of the directory: a disagreement between the two decoders shows
up here as a failing shared case rather than as garbled text in someone's document three weeks
later. Neither suite may carry a private copy of a case (docs/protocol.md §12).

Two rules from §12.1 that this file obeys deliberately, and that are easy to "helpfully" break:

  * **The index is enumerated, never globbed.** `index.json`'s `cases` array is the manifest. A
    fixture file that exists but is not registered is a visible omission, and globbing would hide
    it. There is no `Path.glob` anywhere in this module.
  * **Byte sources are decoded, not inlined.** A byte source is a lowercase hex string, or
    `{"repeat": "<hex unit>", "count": n}`, or an array of either, concatenated. The `repeat` form
    exists so that cases 004 and 005 — a 65536-byte payload and its one-byte-over sibling — do not
    put 128 KiB of hex in the repository (§12.2).

Each case becomes its own `test_<id>` method, so a failure names the case rather than the loop.

─── What is asserted, and what is deliberately not ──────────────────────────────────────────────

`code` and `fatal` are asserted strictly: they are the machine-readable contract, and §13 requires
receivers to act on `fatal` even for a code they do not recognise.

`at_offset` is asserted **when the case supplies a non-null one**. §12.3 calls it informational and
says a runner MAY compare it. We compare it, because it is currently exact on this side and an
off-by-one in either implementation's offset arithmetic is precisely the class of bug that makes a
`bad_utf8` report point at the wrong byte of the wrong frame.

`detail` is **never** asserted. It is prose, it differs between the two implementations by design
(case 008 says "truncated 3-byte sequence: lead 0xE9 followed by 0x22" where CPython's decoder says
"invalid continuation byte"), and a test that pinned it would be pinning wording, not behaviour.

`effects` entries are prose too. §12.5 is explicit that a runner MUST assert only the ones it can
express and MUST NOT fail a case for an effect it does not model. The two machine-checkable ones —
`log_gap` and `reset_anchor_confidence`, which `classify_inbound` emits verbatim — are asserted;
the rest ("surface sidecar-crashed in tray") are not modelled by a pure codec and are skipped.
"""

from __future__ import annotations

import json
import unittest
from pathlib import Path
from typing import Any

from kvit_asr import protocol as proto

# server/tests/test_fixtures.py -> server/tests -> server -> <repo root>
REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURE_DIR = REPO_ROOT / "tests" / "fixtures" / "protocol"
INDEX_PATH = FIXTURE_DIR / "index.json"


def _load_json(path: Path) -> dict[str, Any]:
    # The fixtures are UTF-8 with non-ASCII text in cases 012 and 013; on Windows the default
    # encoding is cp1252, so the encoding is explicit here. Note this uses plain `json.loads`, not
    # `strict_json_loads`: these are repository files, not wire data.
    return json.loads(path.read_text(encoding="utf-8"))


INDEX = _load_json(INDEX_PATH)


# ─── §12.2 Byte sources ───────────────────────────────────────────────────────────────────────


def decode_byte_source(src: Any) -> bytes:
    """Decode a byte source: hex string, `{"repeat", "count"}`, or an array of either."""
    if isinstance(src, str):
        if len(src) % 2:
            raise ValueError(f"byte source hex string has odd length: {src!r}")
        if src != src.lower():
            raise ValueError(f"byte source hex must be lowercase: {src!r}")
        return bytes.fromhex(src)
    if isinstance(src, dict):
        if set(src) != {"repeat", "count"}:
            raise ValueError(f"byte source object must be exactly repeat+count: {sorted(src)}")
        return decode_byte_source(src["repeat"]) * int(src["count"])
    if isinstance(src, list):
        return b"".join(decode_byte_source(x) for x in src)
    raise TypeError(f"not a byte source: {type(src).__name__}")


# ─── Framing runner ───────────────────────────────────────────────────────────────────────────


class DeliveredFrame:
    """One frame the pipeline delivered, with its control payload already decoded.

    The fixtures' `expect.frames` list is the set of frames a *receiver* delivers upward, which is
    why a control frame whose payload fails §4 validation is not in it (case 008 expects zero
    frames even though the framing layer parsed the header perfectly). So the runner's unit of
    delivery is frame + decoded message, not frame alone.
    """

    __slots__ = ("type", "payload", "json")

    def __init__(self, type_: int, payload: bytes, json_: dict[str, Any] | None) -> None:
        self.type = int(type_)
        self.payload = payload
        self.json = json_


def run_framing(chunks: list[bytes], eof: bool) -> tuple[list[DeliveredFrame],
                                                         proto.ProtocolError | None,
                                                         bool]:
    """Feed `chunks` to a `FrameReader` one delivery at a time and collect what comes out.

    Returns `(frames, error, end_of_stream)`. Chunks are pushed exactly as given and never
    concatenated first — §12.3 makes that the whole purpose of case 001.

    A payload-relative `at_offset` from `decode_control` is rebased onto the stream by adding
    `Frame.payload_offset`; a pure codec cannot know where the payload sat in the stream, and the
    fixtures assert stream-absolute offsets.
    """
    reader = proto.FrameReader()
    frames: list[DeliveredFrame] = []
    error: proto.ProtocolError | None = None

    for chunk in chunks:
        reader.push(chunk)
        try:
            for frame in reader:
                decoded: dict[str, Any] | None = None
                if frame.type == proto.FrameType.CONTROL:
                    try:
                        decoded = proto.decode_control(frame.payload).to_json()
                    except proto.ProtocolError as exc:
                        if exc.at_offset is not None:
                            exc.at_offset += frame.payload_offset
                        raise
                else:
                    proto.decode_audio_payload(frame.payload)  # §6.3: odd length is bad_message
                frames.append(DeliveredFrame(frame.type, frame.payload, decoded))
        except proto.ProtocolError as exc:
            error = exc
            break

    if error is None and eof:
        reader.notify_eof()
        try:
            for frame in reader:  # pragma: no cover - no fixture leaves a whole frame for EOF
                frames.append(DeliveredFrame(frame.type, frame.payload, None))
        except proto.ProtocolError as exc:
            error = exc

    return frames, error, error is None and reader.at_end_of_stream


# ─── The test case ────────────────────────────────────────────────────────────────────────────


class ProtocolFixtures(unittest.TestCase):
    """One `test_<id>` method per registered case. Methods are attached below the class body."""

    maxDiff = None

    # -- shared assertions ---------------------------------------------------------------------

    def assert_error_matches(self, got: proto.ProtocolError | None, want: dict[str, Any] | None,
                             where: str) -> None:
        if want is None:
            self.assertIsNone(
                got, f"{where}: expected no error, got {got!r}"
            )
            return
        self.assertIsNotNone(got, f"{where}: expected error {want['code']}, none was raised")
        assert got is not None
        self.assertEqual(got.code_raw, want["code"], f"{where}: wrong error code")
        self.assertEqual(got.fatal, want["fatal"], f"{where}: wrong fatal flag")
        if want.get("at_offset") is not None:
            self.assertEqual(got.at_offset, want["at_offset"], f"{where}: wrong at_offset")

    # -- kind: framing -------------------------------------------------------------------------

    def run_framing_case(self, case: dict[str, Any]) -> None:
        chunks = [decode_byte_source(c) for c in case["input"]["chunks"]]
        eof = bool(case["input"].get("eof", True))
        expect = case["expect"]
        whole = b"".join(chunks)

        # §12.3: "Runners SHOULD additionally re-run every framing case with the chunks
        # concatenated, and with the stream re-split one byte at a time; both must produce the
        # same frames." All three are run here, because the three of them together are what
        # distinguishes a byte-stream decoder from one that got lucky on the delivery pattern.
        modes = (
            ("as-delivered", chunks),
            ("concatenated", [whole] if whole else []),
            ("one-byte-at-a-time", [bytes((b,)) for b in whole]),
        )

        for mode, feed in modes:
            with self.subTest(feed=mode):
                frames, error, end_of_stream = run_framing(feed, eof)

                self.assertEqual(
                    [f.type for f in frames],
                    [f["type"] for f in expect["frames"]],
                    f"{case['id']}/{mode}: wrong frame types delivered",
                )
                for i, (got, want) in enumerate(zip(frames, expect["frames"])):
                    where = f"{case['id']}/{mode}/frame[{i}]"
                    self.assertEqual(len(got.payload), want["payload_len"], f"{where}: length")
                    if "payload_repeat" in want:
                        want_payload = decode_byte_source(want["payload_repeat"])
                    else:
                        want_payload = decode_byte_source(want.get("payload_hex", ""))
                    self.assertEqual(got.payload, want_payload, f"{where}: payload bytes")
                    if "json" in want:
                        self.assertEqual(got.json, want["json"], f"{where}: decoded control")
                    if "samples" in want:
                        # s16le: two bytes per sample. A fixture that disagreed would mean one of
                        # the two implementations had drifted on the audio format itself.
                        self.assertEqual(
                            len(got.payload) // proto.BYTES_PER_SAMPLE, want["samples"],
                            f"{where}: sample count",
                        )

                self.assert_error_matches(error, expect.get("error"), f"{case['id']}/{mode}")

                if "end_of_stream" in expect:
                    self.assertEqual(
                        end_of_stream, expect["end_of_stream"],
                        f"{case['id']}/{mode}: end_of_stream",
                    )
                elif expect.get("error") is None:
                    # No fixture asserts the negative, but a decoder that reported orderly
                    # shutdown while a frame was still pending would be wrong in a way nothing
                    # else here would catch.
                    self.assertFalse(
                        end_of_stream and not eof,
                        f"{case['id']}/{mode}: end_of_stream without EOF",
                    )

    # -- kind: text ----------------------------------------------------------------------------

    def run_text_case(self, case: dict[str, Any]) -> None:
        text = case["text"]

        # `text_hex` is authoritative and `text` is for reading (§12.4). If they disagree the
        # fixture is broken, and every offset below it would be measured against the wrong bytes.
        self.assertEqual(
            text.encode("utf-8").hex(), case["text_hex"],
            f"{case['id']}: text and text_hex disagree",
        )
        self.assertEqual(
            proto.utf8_len(text), case["text_bytes"],
            f"{case['id']}: text_bytes disagrees with utf8_len",
        )

        boundaries = proto.grapheme_boundaries(text)

        for check in case["checks"]:
            offset = check["stable_bytes"]
            with self.subTest(stable_bytes=offset, reason=check["reason"]):
                self.assertEqual(
                    proto.is_valid_stable_offset(text, offset), check["valid"],
                    f"{case['id']}: is_valid_stable_offset({text!r}, {offset}) "
                    f"-- {check['reason']}",
                )
                # The same fact, reached through the two functions the commit engine actually
                # calls. `grapheme_boundaries` is the enumeration and `floor_grapheme` is the
                # snap; if either disagreed with the predicate, a commit could be placed at an
                # offset the predicate would reject.
                self.assertEqual(
                    offset in boundaries and offset <= proto.utf8_len(text), check["valid"],
                    f"{case['id']}: grapheme_boundaries disagrees at {offset}",
                )
                if check["valid"]:
                    self.assertEqual(
                        proto.floor_grapheme(text, offset), offset,
                        f"{case['id']}: floor_grapheme moved a valid offset",
                    )
                elif 0 <= offset <= proto.utf8_len(text):
                    snapped = proto.floor_grapheme(text, offset)
                    self.assertLess(snapped, offset)
                    self.assertTrue(
                        proto.is_valid_stable_offset(text, snapped),
                        f"{case['id']}: floor_grapheme({offset}) -> {snapped}, not a boundary",
                    )

    # -- kind: session -------------------------------------------------------------------------

    def run_session_case(self, case: dict[str, Any]) -> None:
        state = proto.ReceiverState.from_fixture(case.get("initial", {}))

        for i, entry in enumerate(case["messages"]):
            expect = entry["expect"]
            where = f"{case['id']}/messages[{i}] from={entry['from']}"
            with self.subTest(index=i, source=entry["from"]):
                if entry["from"] == "transport":
                    outcome = self.apply_transport_event(state, entry["event"], where)
                else:
                    # Round-trip through the wire encoding rather than handing the dict to the
                    # decoder: the fixture's `json` block is what appears in a control payload, so
                    # decoding it is part of what is under test. `ensure_ascii=False` matches
                    # `encode_control` (§4.2) — with escapes, the payload's byte count and the
                    # byte count `stable_bytes` indexes into would disagree.
                    payload = json.dumps(entry["json"], ensure_ascii=False).encode("utf-8")
                    message = proto.decode_control(payload)
                    if entry["from"] == "client":
                        outcome = proto.apply_outbound(state, message)
                    else:
                        outcome = proto.classify_inbound(state, message)

                self.assertEqual(
                    outcome.verdict.value, expect["verdict"], f"{where}: wrong verdict"
                )

                if "gap" in expect:
                    self.assertEqual(outcome.gap, expect["gap"], f"{where}: wrong gap block")

                self.assert_error_matches(outcome.error, expect.get("error"), where)

                # §12.5: assert only the effects this model expresses. `log_gap` and
                # `reset_anchor_confidence` are emitted verbatim by classify_inbound; every other
                # effect in these fixtures is prose about tray state, injection, or queues, which
                # a pure codec has no way to observe.
                for effect in expect.get("effects", []):
                    if effect in ("log_gap", "reset_anchor_confidence"):
                        self.assertIn(effect, outcome.effects, f"{where}: missing effect")

    def apply_transport_event(self, state: proto.ReceiverState, event: str,
                              where: str) -> proto.InboundOutcome:
        """Model the two `from: "transport"` events in case 020.

        These are connection events, not messages, so `protocol.py` — which is a pure codec with
        no transport — has no handler for them. The state changes below are docs/protocol.md
        §10.3's list, reduced to the part a codec can hold: the session is abandoned without
        injecting anything, and the per-session identity space is discarded so that nothing from
        the dead session can be matched by accident on the new connection.
        """
        if event == "broken_pipe":
            state.state = proto.ConnState.DEAD
            state.current_session_id = None
            state.last_seq = 0
            state.utterance_id = 0
            state.last_stable_bytes = 0
            state.last_stable_text = ""
            state.cancelled_sessions.clear()
            return proto.InboundOutcome(
                proto.Verdict.ABORT_SESSION,
                effects=["state -> DEAD", "no injection", "commit engine reset"],
            )
        if event == "child_respawned":
            state.state = proto.ConnState.SPAWNING
            return proto.InboundOutcome(
                proto.Verdict.RECONNECTED, effects=["new pipe name", "new client handle"]
            )
        raise AssertionError(f"{where}: unmodelled transport event {event!r}")

    # -- meta ----------------------------------------------------------------------------------

    def test_index_is_well_formed(self) -> None:
        """The manifest itself, before any case runs.

        A malformed index would otherwise surface as a confusing failure inside whichever case
        happened to be enumerated first.
        """
        self.assertEqual(INDEX["schema"], 1)
        self.assertEqual(INDEX["proto"], proto.PROTO_VERSION)
        self.assertIn("byte_source", INDEX, "the index must restate §12.2 to be self-describing")

        ids = [c["id"] for c in INDEX["cases"]]
        self.assertEqual(len(ids), len(set(ids)), "duplicate case ids in the index")
        self.assertTrue(ids, "the index registers no cases at all")

        declared_kinds = set(INDEX["kinds"])
        for case in INDEX["cases"]:
            self.assertIn(case["kind"], declared_kinds, f"{case['id']}: undeclared kind")
            self.assertEqual(
                case["file"], f"{case['id']}.json",
                f"{case['id']}: file name and id must agree (§12.1: id == filename stem)",
            )

    def test_every_registered_case_file_agrees_with_the_index(self) -> None:
        """Each file exists and its own header matches the row that registered it."""
        for case in INDEX["cases"]:
            with self.subTest(case=case["id"]):
                path = FIXTURE_DIR / case["file"]
                self.assertTrue(path.is_file(), f"{case['id']}: {path} is missing")
                body = _load_json(path)
                self.assertEqual(body["schema"], 1)
                self.assertEqual(body["proto"], proto.PROTO_VERSION)
                self.assertEqual(body["id"], case["id"])
                self.assertEqual(body["kind"], case["kind"])
                self.assertIn("description", body, f"{case['id']}: cases must say why they exist")


# ─── Case registration ────────────────────────────────────────────────────────────────────────
#
# One method per case, named for the case, so `unittest -v` reads as a conformance report and a
# failure names the fixture instead of a loop index. The index drives this; the directory is never
# globbed (§12.1).

_RUNNERS = {
    "framing": ProtocolFixtures.run_framing_case,
    "text": ProtocolFixtures.run_text_case,
    "session": ProtocolFixtures.run_session_case,
}


def _make_case_method(case_row: dict[str, Any]):
    def method(self: ProtocolFixtures) -> None:
        body = _load_json(FIXTURE_DIR / case_row["file"])
        runner = _RUNNERS.get(body["kind"])
        if runner is None:
            # Not a skip: an unrunnable kind means the fixtures grew a feature this suite does
            # not implement, and silently passing would make the C++ side the only one checking
            # it — exactly the divergence the shared directory exists to prevent.
            self.fail(
                f"{body['id']}: kind {body['kind']!r} has no runner in this suite. "
                f"docs/protocol.md §12 declares kinds {sorted(_RUNNERS)}."
            )
        runner(self, body)

    method.__name__ = "test_" + case_row["id"].replace("-", "_")
    method.__doc__ = f"{case_row['id']} ({case_row['kind']}): {case_row['name']}"
    return method


for _row in INDEX["cases"]:
    _m = _make_case_method(_row)
    setattr(ProtocolFixtures, _m.__name__, _m)
del _row, _m


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
