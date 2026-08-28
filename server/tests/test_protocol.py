# Copyright 2026 Kvit
# Licensed under the Apache License, Version 2.0. See LICENSE-APACHE, or
# https://www.apache.org/licenses/LICENSE-2.0
"""Properties of `kvit_asr.protocol` that the shared fixtures do not, and cannot, cover.

`test_fixtures.py` drives `tests/fixtures/protocol/` — the cases both endpoints must agree on. This
file covers the rest: the properties that are either too broad to enumerate as fixtures (feed a
stream at every possible fragmentation), specific to *this* language's traps (`json.loads` accepts
`NaN`; Python strings are code points and the wire is UTF-8 bytes), or statements about resource
use that a fixture file has no way to express (an oversize length must be refused before anything
proportional to it is allocated).

Nothing here duplicates a fixture. Where a property has a fixture, this file tests the
*generalisation* of it: fixture 001 pins one fragmentation, this file pins all of them.

─── The seven properties, and why each earns a section ──────────────────────────────────────────

1. **Byte-stream decoding.** The pipe is created `PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE`, and
   that is not a licence to write a message-oriented parser (docs/protocol.md §2.2). A reader whose
   buffer is smaller than the message gets `ERROR_MORE_DATA`; a peer that sets `PIPE_READMODE_BYTE`
   on its own handle changes delivery silently; and this side reaches the pipe through
   ctypes/kernel32, where §14 item 3 records that whether message mode even takes effect is
   *unverified*. So: one byte at a time must produce exactly what one big read produces.

2. **Size limits from the prefix alone.** §3.3 requires the length to be validated from the 4-byte
   prefix before reading, buffering or allocating any payload. A `length` of `0xFFFFFFFF` that
   causes a 4 GiB allocation before the range check is a remote out-of-memory, and the fixture
   format cannot assert "did not allocate".

3. **JSON strictness.** `json.loads` accepts `NaN`, `Infinity` and `-Infinity` by default, and
   turns `1e400` into `inf` without going anywhere near `parse_constant`. §4.2 is plain RFC 8259.

4. **UTF-8 in control frames.** Must fail as a protocol error with a code, not as a stray
   `UnicodeDecodeError` escaping from a codec into a caller that has no idea it is holding wire
   data — and never as a U+FFFD substitution (§4.1), which is silent corruption of text on its way
   to a keystroke injector aimed at the user's document.

5. **String index vs UTF-8 byte offset.** The single most likely bug on this side. `len(text)` is
   code points, `stable_bytes` is bytes, and `text[:stable_bytes]` is a plausible-looking slice of
   the wrong region. Astral-plane text makes the three diverge maximally: one Python character,
   four UTF-8 bytes, two UTF-16 code units.

6. **Grapheme-boundary agreement with the C++ side.** `src/util/utf8.h` declares an explicit
   approximation and `docs/protocol.md` §8.4 makes the same list the normative floor. This side is
   the *sender* of `stable_bytes`, so its boundary set must be a subset of the receiver's: an
   offset this side emits and the C++ side rejects is a `stability_violation` and an aborted
   session in a shipped build.

7. **`stable_bytes` monotonicity and prefix stability.** Both halves of §8.2 requirement 3. The
   second half is the one with teeth: a non-decreasing `stable_bytes` whose underlying bytes
   changed has still retracted text the Client may already have typed into the user's document.
"""

from __future__ import annotations

import json
import tracemalloc
import unittest

from kvit_asr import protocol as proto


def frame_records(frames):
    """A comparable, fully-determined view of a decoded frame list.

    Includes the stream offsets, not just the payloads: `payload_offset` is what rebases a
    payload-relative `at_offset` onto the stream, so a decoder that got the frames right and the
    offsets wrong would still report `bad_utf8` at the wrong byte of the wrong frame.
    """
    return [(f.type, f.payload, f.offset, f.payload_offset) for f in frames]


def feed(stream: bytes, chunk_size: int):
    """Push `stream` in `chunk_size` pieces, draining fully after each push.

    Returns `(frames, reader)`; the reader is returned so a caller can inspect what is still
    buffered. Draining fully after every push is not an optimisation: fixture 002 delivers three
    frames in one read, and a reader that handles one frame per push silently stalls two of them.
    """
    reader = proto.FrameReader()
    out = []
    for i in range(0, len(stream), chunk_size):
        reader.push(stream[i:i + chunk_size])
        out.extend(reader)
    return out, reader


# ─── 1. The framing layer is a byte-stream decoder ────────────────────────────────────────────


class ByteStreamDecoding(unittest.TestCase):
    """§2.2 — message mode does not excuse a message-oriented parser."""

    def setUp(self) -> None:
        # A realistic multi-frame stream: handshake, session start, audio, two hypotheses with
        # non-ASCII and astral text, a legal zero-length audio frame, and teardown. Deliberately
        # mixed types and sizes, because a decoder that only ever sees same-sized frames can hide
        # an off-by-one that a varied stream exposes immediately.
        self.messages = [
            proto.Hello(proto=1, client="kvit-dictate/0.1"),
            proto.Start(session_id="s7", context=["llama.cpp", "Qwen3"], context_present=True),
            proto.Partial(session_id="s7", utterance_id=1, seq=1, text="can you", stable_bytes=4),
            proto.Partial(
                session_id="s7", utterance_id=1, seq=2,
                # 24 bytes is the offset after the é — 23 characters, and the trap that makes this
                # message worth carrying: a code-point count would put the mark inside the é.
                text="can you change the café ☕ config", stable_bytes=24,
            ),
            proto.Final(
                session_id="s7", utterance_id=1, seq=3,
                text="Can you change the café ☕ config?", audio_pos_ms=4820,
            ),
            proto.Stop(session_id="s7"),
        ]
        self.stream = b"".join(
            [
                self.messages[0].encode_frame(),
                self.messages[1].encode_frame(),
                proto.encode_audio_frame(bytes(640)),          # 20 ms of silence
                self.messages[2].encode_frame(),
                proto.encode_audio_frame(b""),                 # legal empty payload (§6.3)
                self.messages[3].encode_frame(),
                proto.encode_audio_frame(b"\x01\x00\xff\xff" * 160),
                self.messages[4].encode_frame(),
                self.messages[5].encode_frame(),
            ]
        )
        self.expected_frame_count = 9

    def test_one_byte_at_a_time_matches_one_big_read(self) -> None:
        """The property the whole section exists for."""
        whole, _ = feed(self.stream, len(self.stream))
        single, _ = feed(self.stream, 1)

        self.assertEqual(len(whole), self.expected_frame_count)
        self.assertEqual(frame_records(single), frame_records(whole))

    def test_every_chunk_size_produces_the_identical_frame_list(self) -> None:
        """Not just 1 and N: the sizes in between are where a buffer-compaction bug lives.

        The primes and the awkward sizes are chosen so that chunk boundaries land inside the
        length prefix, inside the type byte, inside payloads, and exactly on frame boundaries.
        """
        reference = frame_records(feed(self.stream, len(self.stream))[0])
        for size in (1, 2, 3, 4, 5, 6, 7, 11, 13, 17, 29, 64, 97, 256, 1021, len(self.stream) - 1,
                     len(self.stream), len(self.stream) + 1):
            with self.subTest(chunk_size=size):
                self.assertEqual(frame_records(feed(self.stream, size)[0]), reference)

    def test_decoded_messages_survive_the_round_trip_unchanged(self) -> None:
        """Byte-identical frames are necessary but not sufficient — decode them too.

        In particular `stable_bytes` must still index the same region after a round trip: the text
        here contains a 2-byte and a 3-byte sequence, so an encoder that emitted `\\uXXXX` escapes
        would put the payload's byte count and the offset's byte space into permanent disagreement
        (§4.2, and why `encode_control` sets `ensure_ascii=False`).
        """
        frames, _ = feed(self.stream, 1)
        control = [f for f in frames if f.type == proto.FrameType.CONTROL]
        self.assertEqual(len(control), len(self.messages))
        for frame, original in zip(control, self.messages):
            decoded = proto.decode_control(frame.payload)
            self.assertEqual(decoded.to_json(), original.to_json())

        partial = proto.decode_control(control[3].payload)
        assert isinstance(partial, proto.Partial)
        self.assertEqual(
            partial.text.encode("utf-8")[:partial.stable_bytes].decode("utf-8"),
            "can you change the café",
        )

    def test_a_frame_is_never_delivered_before_its_last_byte_arrives(self) -> None:
        """A decoder that emitted on a partial payload would corrupt text without any error."""
        for missing in (1, 2, 5):
            with self.subTest(bytes_withheld=missing):
                frames, reader = feed(self.stream[:-missing], 1)
                self.assertEqual(len(frames), self.expected_frame_count - 1)
                self.assertGreater(reader.buffered, 0)
                self.assertFalse(reader.at_end_of_stream)
                self.assertIsNone(reader.error)

    def test_end_of_stream_mid_frame_is_truncated_not_orderly(self) -> None:
        """The distinction fixtures 010 and 011 draw, generalised over the cut point."""
        for missing in (1, 3, 20):
            with self.subTest(bytes_withheld=missing):
                reader = proto.FrameReader()
                reader.push(self.stream[:-missing])
                list(reader)
                reader.notify_eof()
                with self.assertRaises(proto.ProtocolError) as caught:
                    list(reader)
                self.assertEqual(caught.exception.code_raw, "truncated_frame")
                self.assertTrue(caught.exception.fatal)
                self.assertFalse(reader.at_end_of_stream)

        reader = proto.FrameReader()
        reader.push(self.stream)
        self.assertEqual(len(list(reader)), self.expected_frame_count)
        reader.notify_eof()
        self.assertEqual(list(reader), [])
        self.assertTrue(reader.at_end_of_stream)

    def test_empty_pushes_and_an_empty_stream_are_not_errors(self) -> None:
        """A 0-byte read that is not EOF happens; it must not be mistaken for one."""
        reader = proto.FrameReader()
        for _ in range(3):
            reader.push(b"")
            self.assertEqual(list(reader), [])
            self.assertFalse(reader.at_end_of_stream)
        reader.notify_eof()
        self.assertEqual(list(reader), [])
        self.assertTrue(reader.at_end_of_stream)


# ─── 2. Size limits are decided from the 4-byte prefix ────────────────────────────────────────


class LengthPrefixValidation(unittest.TestCase):
    """§3.3 — validate `length` from the prefix alone, before reading or allocating a payload."""

    #: Length fields that must be refused, and the code §11.3 assigns each.
    BAD_LENGTHS = (
        (0, "bad_frame"),                                # §3.1: length covers the type byte
        (proto.MAX_LENGTH_FIELD + 1, "frame_too_large"),  # 65538, one over (fixture 005)
        (0x7FFFFFFF, "frame_too_large"),
        (0x80000000, "frame_too_large"),                 # sign bit set: a signed reader sees -2^31
        (0xFFFFFFFF, "frame_too_large"),                 # fixture 007
    )

    def test_rejected_with_only_the_four_byte_prefix_in_hand(self) -> None:
        """No type byte, no payload — four bytes, and the decision is already made.

        This is the strongest form of §3.3's requirement: not merely "rejected before the payload
        is used", but "rejected before any byte after the prefix has even been delivered". A
        decoder that waited for `4 + length` bytes before range-checking would hang here instead
        of failing, and on a real pipe it would sit there consuming memory.
        """
        for length, code in self.BAD_LENGTHS:
            with self.subTest(length=length, code=code):
                reader = proto.FrameReader()
                reader.push(length.to_bytes(4, "little"))
                with self.assertRaises(proto.ProtocolError) as caught:
                    reader.next_frame()
                self.assertEqual(caught.exception.code_raw, code)
                self.assertTrue(caught.exception.fatal)
                self.assertEqual(caught.exception.at_offset, 0)
                # The buffer is untouched: nothing was consumed, nothing was read past it.
                self.assertEqual(reader.buffered, 4)

    def test_no_allocation_proportional_to_a_bogus_length(self) -> None:
        """A 4 GiB length must not cost 4 GiB — or 4 MiB, or anything but a message string.

        Measured with `tracemalloc` rather than asserted in prose, because "did not allocate" is
        exactly the property a fixture file cannot express and exactly the one that turns a
        one-line decoder bug into a remote out-of-memory. The bound is one payload's worth
        (64 KiB); the claimed length is 65536 times that.
        """
        reader = proto.FrameReader()
        reader.push((0xFFFFFFFF).to_bytes(4, "little"))

        tracemalloc.start()
        try:
            tracemalloc.reset_peak()
            with self.assertRaises(proto.ProtocolError):
                reader.next_frame()
            _current, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()

        self.assertLess(
            peak, proto.MAX_PAYLOAD,
            f"rejecting a 0xFFFFFFFF length peaked at {peak} bytes; "
            f"the range check must precede any payload allocation",
        )

    def test_the_largest_legal_length_is_not_refused_on_the_prefix(self) -> None:
        """The other side of the boundary: 65537 is legal, so the prefix alone must not reject it.

        A decoder with an off-by-one here would refuse the largest legal frame — 2048 ms of audio,
        which nothing in the capture path should produce, but which the protocol declares legal.
        """
        reader = proto.FrameReader()
        reader.push(proto.MAX_LENGTH_FIELD.to_bytes(4, "little"))
        self.assertIsNone(reader.next_frame())      # incomplete, not invalid
        self.assertIsNone(reader.error)

        reader.push(bytes((proto.FrameType.AUDIO,)) + bytes(proto.MAX_PAYLOAD))
        frame = reader.next_frame()
        self.assertIsNotNone(frame)
        assert frame is not None
        self.assertEqual(frame.type, proto.FrameType.AUDIO)
        self.assertEqual(len(frame.payload), proto.MAX_PAYLOAD)
        self.assertIsNone(reader.next_frame())

    def test_the_error_is_sticky_and_the_stream_is_never_resynchronised(self) -> None:
        """§3.3 forbids resynchronisation, and the reason is not fussiness.

        There is no framing marker to resynchronise *to*: any byte pattern is a legal payload, so a
        scan for a plausible header finds false ones and can deliver attacker- or bug-chosen bytes
        as a valid message. A desynchronised stream is unrecoverable by construction.
        """
        reader = proto.FrameReader()
        reader.push((0).to_bytes(4, "little"))
        with self.assertRaises(proto.ProtocolError) as first:
            reader.next_frame()

        # Push a perfectly good frame after the fault. It must not be decoded.
        reader.push(proto.Hello().encode_frame())
        for _ in range(3):
            with self.assertRaises(proto.ProtocolError) as again:
                reader.next_frame()
            self.assertIs(again.exception, first.exception)
        self.assertFalse(reader.at_end_of_stream)

    def test_encoding_an_oversize_payload_fails_locally_not_on_the_wire(self) -> None:
        """An oversize payload is a bug in *this* endpoint, so it never reaches the pipe.

        `internal_error`, not `frame_too_large`: the latter is what a peer says about something it
        received, and reporting it here would misattribute our bug to them.
        """
        with self.assertRaises(proto.ProtocolError) as caught:
            proto.encode_frame(proto.FrameType.AUDIO, bytes(proto.MAX_PAYLOAD + 1))
        self.assertEqual(caught.exception.code_raw, "internal_error")

        # And the boundary case is accepted, so the check is not off by one in the other direction.
        self.assertEqual(
            len(proto.encode_frame(proto.FrameType.AUDIO, bytes(proto.MAX_PAYLOAD))),
            proto.MAX_FRAME_ON_WIRE,
        )


# ─── 3. JSON strictness ───────────────────────────────────────────────────────────────────────


class JsonStrictness(unittest.TestCase):
    """§4.2 — plain RFC 8259, which `json.loads` is not by default."""

    def test_python_would_accept_the_non_rfc_constants(self) -> None:
        """The trap, asserted first, so the tests below are visibly not redundant.

        `json.loads` accepts `NaN`, `Infinity` and `-Infinity` out of the box. Every one of them
        is outside RFC 8259, and a receiver that silently accepted them would disagree with a
        conforming C++ parser about whether a frame is even well formed.
        """
        for literal in ("NaN", "Infinity", "-Infinity"):
            with self.subTest(literal=literal):
                value = json.loads('{"x": %s}' % literal)["x"]
                self.assertIsInstance(value, float)
                self.assertFalse(
                    -1e308 < value < 1e308,
                    f"json.loads({literal}) unexpectedly produced a finite {value!r}",
                )

    def test_non_finite_constants_are_bad_json(self) -> None:
        for literal in ("NaN", "Infinity", "-Infinity"):
            with self.subTest(literal=literal):
                payload = ('{"t":"hello","proto":1,"x":%s}' % literal).encode("utf-8")
                with self.assertRaises(proto.ProtocolError) as caught:
                    proto.strict_json_loads(payload)
                self.assertEqual(caught.exception.code_raw, "bad_json")
                self.assertTrue(caught.exception.fatal)

    def test_overflowing_literals_are_bad_json_too(self) -> None:
        """The second route to a non-finite number, which `parse_constant` never sees.

        `1e400` is syntactically legal JSON. Python turns it into `inf` in `parse_float`, so a
        receiver that only guarded `parse_constant` would end up holding an infinity anyway — and
        would then disagree with a C++ parser that overflowed to `HUGE_VAL` or refused outright.
        """
        for literal in ("1e400", "-1e400", "1E400", "1e999999"):
            with self.subTest(literal=literal):
                payload = ('{"t":"hello","proto":1,"x":%s}' % literal).encode("utf-8")
                with self.assertRaises(proto.ProtocolError) as caught:
                    proto.strict_json_loads(payload)
                self.assertEqual(caught.exception.code_raw, "bad_json")

    def test_duplicate_keys_are_bad_json(self) -> None:
        """Python's default keeps the last duplicate silently — a quiet divergence between two
        independent parsers is exactly what §4.2 forbids."""
        self.assertEqual(json.loads('{"a":1,"a":2}'), {"a": 2})  # the default behaviour
        with self.assertRaises(proto.ProtocolError) as caught:
            proto.strict_json_loads(b'{"t":"hello","proto":1,"proto":2}')
        self.assertEqual(caught.exception.code_raw, "bad_json")

    def test_top_level_must_be_an_object(self) -> None:
        for payload in (b"[1,2,3]", b'"hello"', b"1", b"true", b"null"):
            with self.subTest(payload=payload):
                with self.assertRaises(proto.ProtocolError) as caught:
                    proto.strict_json_loads(payload)
                self.assertEqual(caught.exception.code_raw, "bad_json")

    def test_lone_surrogate_escapes_are_bad_json(self) -> None:
        """`"\\ud800"` decodes to a character that cannot be re-encoded to UTF-8.

        Python's JSON decoder produces it happily; the failure then surfaces much later, in code
        that has no idea it is holding wire data. §4.2 requires surrogate pairs to be well formed,
        so it is caught here, once, where the error is still nameable.
        """
        for payload in (rb'{"t":"loading","model":"\ud800"}',
                        rb'{"t":"loading","model":"a\udfffb"}'):
            with self.subTest(payload=payload):
                with self.assertRaises(proto.ProtocolError) as caught:
                    proto.strict_json_loads(payload)
                self.assertEqual(caught.exception.code_raw, "bad_json")

        # A well-formed surrogate *pair* is legal and must decode to the astral character —
        # the JSON escape form of U+1D11E, which is how a peer that sets ensure_ascii would
        # spell it. Rejecting lone surrogates must not also reject the legal pairing.
        pair = r'{"t":"loading","model":"\ud834\udd1e"}'
        obj = proto.strict_json_loads(pair.encode("ascii"))
        self.assertEqual(obj["model"], "\U0001D11E")

    def test_malformed_and_truncated_json_is_bad_json(self) -> None:
        for payload in (b"{", b'{"t":}', b'{"t":"hello",}', b"{'t':'hello'}", b"",
                        b'{"t":"hello","proto":+1}', b'{"t":"hello","proto":01}'):
            with self.subTest(payload=payload):
                with self.assertRaises(proto.ProtocolError) as caught:
                    proto.strict_json_loads(payload)
                self.assertEqual(caught.exception.code_raw, "bad_json")

    def test_unknown_field_is_ignored_but_unknown_message_type_is_fatal(self) -> None:
        """§4.2's deliberate asymmetry.

        An extra field cannot change the meaning of a message the receiver already understands. An
        unknown `t` means the peer believes something is happening that this peer knows nothing
        about, and carrying on would look healthy while being wrong.
        """
        msg = proto.decode_control(b'{"t":"hello","proto":1,"invented_by_a_future_version":7}')
        self.assertIsInstance(msg, proto.Hello)

        with self.assertRaises(proto.ProtocolError) as caught:
            proto.decode_control(b'{"t":"reticulate","proto":1}')
        self.assertEqual(caught.exception.code_raw, "unknown_message")
        self.assertTrue(caught.exception.fatal)


# ─── 4. UTF-8 in control frames ───────────────────────────────────────────────────────────────


class ControlFrameUtf8(unittest.TestCase):
    """§4.1 — validate before parsing; never substitute U+FFFD; report `bad_utf8`."""

    #: (label, the malformed bytes, prose). Each is well-formed *except* for the planted sequence,
    #: and each is a different class of ill-formedness the strict Unicode definition rejects.
    MALFORMED = (
        ("lone continuation", b"\x80", "a continuation byte with no lead"),
        ("truncated 3-byte", b"\xe9", "the 0xE9 of fixture 008: Latin-1 e-acute, UTF-8 lead"),
        ("overlong", b"\xc0\xaf", "overlong encoding of '/' — a classic filter bypass"),
        ("surrogate", b"\xed\xa0\x80", "CESU-8 style encoding of U+D800"),
        ("above U+10FFFF", b"\xf5\x80\x80\x80", "beyond the Unicode range"),
        ("bare 0xFF", b"\xff", "never legal in UTF-8 at any position"),
    )

    def payload_with(self, bad: bytes) -> tuple[bytes, int]:
        head = b'{"t":"final","session_id":"s7","utterance_id":1,"seq":2,"text":"caf'
        return head + bad + b'","audio_pos_ms":10}', len(head)

    def test_every_ill_formed_sequence_is_a_protocol_error_not_a_codec_exception(self) -> None:
        """`ProtocolError(bad_utf8)`, with an offset — not a `UnicodeDecodeError` from the codec.

        The distinction is not cosmetic. A `UnicodeDecodeError` escaping `strict_json_loads` would
        propagate into transport code that cannot name it, cannot map it to a wire code, and would
        most likely log it and continue on a stream that is now untrusted.
        """
        for label, bad, why in self.MALFORMED:
            with self.subTest(case=label, why=why):
                payload, offset = self.payload_with(bad)
                with self.assertRaises(proto.ProtocolError) as caught:
                    proto.strict_json_loads(payload)
                err = caught.exception
                self.assertEqual(err.code_raw, "bad_utf8")
                self.assertTrue(err.fatal)
                # The offset is payload-relative (the caller adds Frame.payload_offset) and must
                # point at the first byte that could not begin or continue a valid sequence.
                self.assertEqual(err.at_offset, offset)

    def test_truncated_sequence_at_the_very_end_of_the_payload(self) -> None:
        """"Ran out of bytes" is a different code path in most decoders from "wrong bytes"."""
        payload = b'{"t":"loading","model":"\xe2\x98'
        with self.assertRaises(proto.ProtocolError) as caught:
            proto.strict_json_loads(payload)
        self.assertEqual(caught.exception.code_raw, "bad_utf8")
        self.assertEqual(caught.exception.at_offset, payload.index(b"\xe2"))

    def test_replacement_characters_are_never_produced(self) -> None:
        """The forbidden alternative, shown to be available and shown not to be taken.

        `errors="replace"` would turn every case above into a payload that parses cleanly and
        carries U+FFFD into text bound for the user's document. §4.1 forbids it because the user
        discovers that corruption weeks later, by reading their own notes.
        """
        for label, bad, _why in self.MALFORMED:
            with self.subTest(case=label):
                payload, _ = self.payload_with(bad)
                self.assertIn("�", payload.decode("utf-8", errors="replace"))
                with self.assertRaises(proto.ProtocolError):
                    proto.decode_control(payload)

    def test_the_failure_happens_before_json_parsing(self) -> None:
        """Layer order (§1): the lowest layer that can see the fault reports it.

        This payload is ill-formed UTF-8 *and* ill-formed JSON. The answer must be `bad_utf8`; a
        receiver that reported `bad_json` would be letting a higher layer paper over a lower one,
        and would send the peer a diagnosis that points at the wrong thing.
        """
        with self.assertRaises(proto.ProtocolError) as caught:
            proto.strict_json_loads(b'{"t":"hel\xff')
        self.assertEqual(caught.exception.code_raw, "bad_utf8")

    def test_well_formed_multi_byte_and_astral_text_is_accepted(self) -> None:
        """The negative control: strictness must not reject legal text."""
        text = "café ☕ 𝄞 👨‍👩‍👧"
        msg = proto.Partial(session_id="s7", utterance_id=1, seq=1, text=text,
                            stable_bytes=proto.utf8_len("café ☕ "))
        decoded = proto.decode_control(proto.encode_control(msg))
        assert isinstance(decoded, proto.Partial)
        self.assertEqual(decoded.text, text)
        self.assertEqual(decoded.stable_bytes, msg.stable_bytes)


# ─── 5. String index vs UTF-8 byte offset ─────────────────────────────────────────────────────


class Utf8ByteOffsets(unittest.TestCase):
    """§8.2 — offsets are UTF-8 bytes of the decoded string; Python strings are code points."""

    #: One character from each UTF-8 width, plus a combining sequence and an astral character, so
    #: that code-point index, UTF-8 byte offset and UTF-16 code-unit index all disagree.
    SAMPLES = (
        ("ascii", "abc"),
        ("latin-1 range", "café"),
        ("3-byte", "a☕b"),
        ("astral", "𝄞"),
        ("astral run", "𝄞𝄢𝄪"),
        ("combining", "éfg"),
        ("mixed", "a é ☕ 𝄞 👍"),
        ("empty", ""),
    )

    def test_len_is_code_points_and_utf8_len_is_bytes(self) -> None:
        """The bug this section exists to prevent, stated as an inequality.

        `len("𝄞")` is 1, its UTF-8 length is 4, and its UTF-16 length — the one Windows APIs
        count — is 2. Any of the three read as either of the others is a corrupted offset.
        """
        self.assertEqual(len("𝄞"), 1)
        self.assertEqual(proto.utf8_len("𝄞"), 4)
        self.assertEqual(len("𝄞".encode("utf-16-le")) // 2, 2)

        for label, text in self.SAMPLES:
            with self.subTest(case=label):
                self.assertEqual(proto.utf8_len(text), len(text.encode("utf-8")))

    def test_byte_offset_of_index_round_trips_for_every_index(self) -> None:
        for label, text in self.SAMPLES:
            for i in range(len(text) + 1):
                with self.subTest(case=label, index=i):
                    offset = proto.byte_offset_of_index(text, i)
                    self.assertEqual(offset, len(text[:i].encode("utf-8")))
                    self.assertEqual(proto.index_of_byte_offset(text, offset), i)
                    self.assertTrue(proto.is_code_point_boundary(text, offset))

    def test_offsets_inside_a_sequence_raise_rather_than_rounding(self) -> None:
        """Rounding would turn a protocol violation into a plausible-looking slice.

        That is precisely how a `stable_bytes` bug becomes corrupted text in someone's document
        instead of an aborted session and a log line.
        """
        for label, text in self.SAMPLES:
            encoded = text.encode("utf-8")
            valid = {proto.byte_offset_of_index(text, i) for i in range(len(text) + 1)}
            for offset in range(len(encoded) + 1):
                if offset in valid:
                    continue
                with self.subTest(case=label, offset=offset):
                    self.assertFalse(proto.is_code_point_boundary(text, offset))
                    with self.assertRaises(ValueError):
                        proto.index_of_byte_offset(text, offset)

    def test_out_of_range_offsets_and_indices(self) -> None:
        text = "a☕"
        with self.assertRaises(ValueError):
            proto.index_of_byte_offset(text, proto.utf8_len(text) + 1)
        with self.assertRaises(ValueError):
            proto.index_of_byte_offset(text, -1)
        with self.assertRaises(ValueError):
            proto.byte_offset_of_index(text, len(text) + 1)
        with self.assertRaises(ValueError):
            proto.byte_offset_of_index(text, -1)

    def test_slicing_a_python_string_by_a_byte_offset_is_wrong(self) -> None:
        """Shown, not merely asserted, because the wrong version looks right in a code review.

        `text[:stable_bytes]` is the mistake. It compiles, it runs, and on ASCII it is even
        correct — which is why it survives review and then mangles the first accented word a user
        dictates.
        """
        text = "café ☕ config"
        stable = proto.utf8_len("café ☕ ")           # 10 bytes, 7 code points

        correct = text.encode("utf-8")[:stable].decode("utf-8")
        naive = text[:stable]

        self.assertEqual(correct, "café ☕ ")
        self.assertNotEqual(naive, correct)
        self.assertEqual(proto.index_of_byte_offset(text, stable), 7)
        self.assertEqual(text[:proto.index_of_byte_offset(text, stable)], correct)

    def test_astral_text_offsets_land_where_the_wire_says(self) -> None:
        """A whole hypothesis of astral text, checked against the encoded bytes directly."""
        text = "score: 𝄞 then 👨‍👩‍👧 done"
        encoded = text.encode("utf-8")
        for i in range(len(text) + 1):
            offset = proto.byte_offset_of_index(text, i)
            self.assertEqual(encoded[:offset], text[:i].encode("utf-8"))
        self.assertEqual(proto.utf8_len(text), len(encoded))
        self.assertGreater(proto.utf8_len(text), len(text))


# ─── 6. Grapheme-boundary agreement with the C++ side ─────────────────────────────────────────


class GraphemeBoundaryAgreement(unittest.TestCase):
    """§8.4 and `src/util/utf8.h` — the sequences neither endpoint may split.

    This side is the **sender** of `stable_bytes`, so its boundary set must be a subset of the
    receiver's. A sequence this side is willing to split and the C++ side is not becomes a
    `stability_violation` and an aborted session in a shipped build; the reverse merely delays a
    commit. Every case below is one entry in the list `src/util/utf8.h` declares and
    `docs/protocol.md` §8.4 makes normative.
    """

    def assert_single_cluster(self, text: str, label: str) -> None:
        """`text` is exactly one grapheme cluster: no interior offset may be published."""
        total = proto.utf8_len(text)
        self.assertEqual(
            proto.grapheme_boundaries(text), (0, total),
            f"{label}: expected one cluster, got boundaries "
            f"{proto.grapheme_boundaries(text)} over {total} bytes",
        )
        self.assertEqual(proto.count_graphemes(text), 1, label)
        # Every interior *code-point* boundary must be refused. These are the offsets that pass a
        # UTF-8-only check and fail a cluster check — the entire point of fixture 013.
        for i in range(1, len(text)):
            offset = proto.byte_offset_of_index(text, i)
            self.assertTrue(proto.is_code_point_boundary(text, offset), label)
            self.assertFalse(
                proto.is_valid_stable_offset(text, offset),
                f"{label}: byte offset {offset} splits the cluster",
            )

    def test_combining_mark_runs(self) -> None:
        """Grapheme_Extend: a base plus any number of combining marks is one cluster."""
        self.assert_single_cluster("é", "e + combining acute")
        self.assert_single_cluster("ȩ́̈", "e + three stacked marks")
        self.assert_single_cluster("a⃗", "a + combining right arrow above")

    def test_zwj_emoji_sequences(self) -> None:
        """GB11 — what holds a family emoji together."""
        self.assert_single_cluster("\U0001F468‍\U0001F469‍\U0001F467", "family ZWJ")
        self.assert_single_cluster("\U0001F469‍\U0001F4BB", "woman technologist")
        self.assert_single_cluster(
            "\U0001F468\U0001F3FB‍\U0001F52C", "ZWJ across a skin-tone modifier"
        )

    def test_emoji_modifier_sequences(self) -> None:
        """The skin-tone modifiers U+1F3FB–U+1F3FF carry Grapheme_Extend=Yes."""
        for modifier in "\U0001F3FB\U0001F3FC\U0001F3FD\U0001F3FE\U0001F3FF":
            self.assert_single_cluster("\U0001F44D" + modifier, f"thumbs-up + U+{ord(modifier):X}")

    def test_regional_indicator_pairs(self) -> None:
        """GB12/GB13 — pairs, and the parity rule that makes a third RI start a new cluster."""
        self.assert_single_cluster("\U0001F1E9\U0001F1EA", "flag DE")

        two_flags = "\U0001F1E9\U0001F1EA\U0001F1EB\U0001F1F7"
        self.assertEqual(proto.grapheme_boundaries(two_flags), (0, 8, 16), "DE FR")
        self.assertFalse(proto.is_valid_stable_offset(two_flags, 4), "split inside the first flag")
        self.assertFalse(proto.is_valid_stable_offset(two_flags, 12), "split inside the second")

        odd = "\U0001F1E9\U0001F1EA\U0001F1EB"
        self.assertEqual(proto.grapheme_boundaries(odd), (0, 8, 12), "an odd trailing RI")

    def test_variation_selectors_and_the_keycap(self) -> None:
        """§8.4 names these explicitly, and they are the least obvious of the set."""
        self.assert_single_cluster("#️⃣", "keycap # (VS16 + U+20E3)")
        self.assert_single_cluster("☕️", "hot beverage + VS16")
        self.assert_single_cluster("☕︎", "hot beverage + VS15 (text presentation)")
        self.assert_single_cluster("葛\U000E0100", "CJK ideograph + variation selector 17")

    def test_crlf_is_never_split(self) -> None:
        """GB3 — the one rule about ordinary text rather than emoji."""
        self.assert_single_cluster("\r\n", "CR LF")

        text = "a\r\nb"
        self.assertEqual(proto.grapheme_boundaries(text), (0, 1, 3, 4))
        self.assertFalse(proto.is_valid_stable_offset(text, 2), "between CR and LF")

    def test_spacing_marks(self) -> None:
        """GB9a — Devanagari vowel signs are spacing (category Mc), not combining (Mn)."""
        self.assert_single_cluster("का", "ka + aa matra")
        self.assert_single_cluster("কা", "Bengali ka + aa")

    def test_the_boundary_set_is_consistent_with_the_predicate_everywhere(self) -> None:
        """`grapheme_boundaries`, `is_grapheme_boundary` and `floor_grapheme` are one answer.

        Three functions, one fact. The commit engine calls `floor_grapheme` and the wire check
        calls `is_grapheme_boundary`; if they disagreed anywhere, a commit could be placed at an
        offset the receiver rejects, which is the failure mode this whole section exists to stop.
        """
        texts = [
            "é \U0001F468‍\U0001F469‍\U0001F467 \U0001F1E9\U0001F1EA #️⃣",
            "plain ascii text",
            "a\r\nb\r\n",
            "काखी",
            "",
        ]
        for text in texts:
            with self.subTest(text=text):
                boundaries = set(proto.grapheme_boundaries(text))
                for offset in range(proto.utf8_len(text) + 1):
                    expected = offset in boundaries
                    self.assertEqual(proto.is_grapheme_boundary(text, offset), expected, offset)
                    self.assertEqual(proto.is_valid_stable_offset(text, offset), expected, offset)
                    snapped = proto.floor_grapheme(text, offset)
                    self.assertIn(snapped, boundaries)
                    self.assertLessEqual(snapped, offset)
                    if expected:
                        self.assertEqual(snapped, offset)
                # Out of range is not a boundary, in either direction.
                self.assertFalse(proto.is_grapheme_boundary(text, -1))
                self.assertFalse(proto.is_grapheme_boundary(text, proto.utf8_len(text) + 1))

    def test_hangul_jamo_is_a_known_divergence_from_the_cpp_side(self) -> None:
        """Not asserted either way — reported, because the two sides currently disagree.

        `src/util/utf8.cpp` implements GB6–GB8 (Hangul L/V/T composition) and `src/util/utf8.h`
        says so explicitly: "An earlier draft of this header listed it as a known gap; it is not
        one." `protocol.py`'s `grapheme_boundaries` docstring says the opposite — that GB6–GB8 are
        "deliberately not implemented, to match the C++ side's declared approximation".

        The consequence runs the *unsafe* way. Omitting GB6–GB8 gives this side **more** boundaries
        than the C++ side, and this side is the sender: it would publish a `stable_bytes` between
        two jamo of one decomposed syllable, and the C++ receiver would reject that offset as
        off-boundary — `stability_violation`, session aborted, per §8.3. The docstring's claim that
        the omission "fails in the conservative direction" holds against a full UAX #29
        implementation, not against a peer that implements more rules than we do.

        No fixture covers Hangul, so nothing else catches this. Pinning either behaviour here would
        be wrong: fixing it belongs in `protocol.py` or `utf8.cpp`, whichever the integration pass
        decides is authoritative, and a test that pinned today's answer would have to be rewritten
        by that fix. So this test records the divergence and skips.
        """
        syllable = "각"          # decomposed 각: L + V + T
        boundaries = proto.grapheme_boundaries(syllable)
        self.skipTest(
            f"KNOWN DIVERGENCE: Python reports {len(boundaries) - 1} clusters for decomposed "
            f"Hangul (boundaries {boundaries}); src/util/utf8.cpp implements GB6-GB8 and reports "
            f"1. Python is the stable_bytes sender, so its extra boundaries are the unsafe "
            f"direction. Resolve in protocol.py or utf8.cpp; see docs/protocol.md 8.4."
        )


# ─── 7. stable_bytes monotonicity and prefix stability ────────────────────────────────────────


class StableBytesInvariant(unittest.TestCase):
    """§8.2 requirement 3, both halves, and §8.3's refusal to clamp."""

    def test_a_growing_hypothesis_with_a_frozen_prefix_is_accepted(self) -> None:
        """The normal case: `stable_bytes` rises, the bytes below it never move."""
        history = [
            ("can", 0),
            ("can you", 4),
            ("can you change", 8),
            ("can you change the", 15),
            ("can you change the llama.cpp", 19),
        ]
        prev_text, prev_stable = "", 0
        for text, stable in history:
            with self.subTest(text=text, stable_bytes=stable):
                check, diverge = proto.check_stability(prev_text, prev_stable, text, stable)
                self.assertIs(check, proto.StabilityCheck.OK)
                self.assertIsNone(diverge)
                prev_text, prev_stable = text, stable

    def test_stable_bytes_may_not_decrease(self) -> None:
        check, _ = proto.check_stability("can you change", 8, "can you change the llama", 4)
        self.assertIs(check, proto.StabilityCheck.DECREASED)

        # Equal is not a decrease: a hypothesis that revises only above the mark is normal.
        check, _ = proto.check_stability("can you change", 8, "can you CHANGE the", 8)
        self.assertIs(check, proto.StabilityCheck.OK)

    def test_the_prefix_below_the_mark_must_be_byte_identical(self) -> None:
        """The half with teeth: `stable_bytes` rose, and text already injected was retracted."""
        check, diverge = proto.check_stability("can you change", 8, "can we change the", 12)
        self.assertIs(check, proto.StabilityCheck.PREFIX_MUTATED)
        self.assertEqual(diverge, 4)

    def test_prefix_mutation_inside_a_multi_byte_character(self) -> None:
        """A one-byte diff that a code-point comparison could round past.

        "café " and "cafe " differ only in the 2-byte U+00E9 vs the 1-byte 'e', so the mutation
        also shifts every following byte. Comparing decoded strings sliced by a *byte* offset would
        compare two different regions entirely, which is why `check_stability` compares encoded
        bytes.
        """
        check, diverge = proto.check_stability("café latte", 6, "cafe latte extra", 6)
        self.assertIs(check, proto.StabilityCheck.PREFIX_MUTATED)
        self.assertEqual(diverge, 3)                     # the first byte of the é

        # And the negative control: the same accented prefix, unchanged, with the tail revised.
        check, diverge = proto.check_stability("café latte", 6, "café mocha", 6)
        self.assertIs(check, proto.StabilityCheck.OK)
        self.assertIsNone(diverge)

    def test_range_and_boundary_faults_are_distinguished(self) -> None:
        """Three different faults, three different verdicts — §8.3 gives them the same handling
        but the log line must say which one happened."""
        check, _ = proto.check_stability("", 0, "café", 99)
        self.assertIs(check, proto.StabilityCheck.OUT_OF_RANGE)

        check, _ = proto.check_stability("", 0, "café", -1)
        self.assertIs(check, proto.StabilityCheck.OUT_OF_RANGE)

        check, _ = proto.check_stability("", 0, "café", 4)   # inside the 2-byte é
        self.assertIs(check, proto.StabilityCheck.NOT_GRAPHEME_BOUNDARY)

        check, _ = proto.check_stability("", 0, "éx", 1)  # between e and its acute
        self.assertIs(check, proto.StabilityCheck.NOT_GRAPHEME_BOUNDARY)

    def test_monotonicity_holds_across_astral_text(self) -> None:
        """The invariant on text where a single cluster is four bytes and one Python character."""
        first = "score 𝄞"
        second = "score 𝄞 and 👨‍👩‍👧"
        mark = proto.utf8_len("score ")
        check, _ = proto.check_stability(first, mark, second, proto.utf8_len(first))
        self.assertIs(check, proto.StabilityCheck.OK)

        # Publishing a mark inside the astral character is a boundary fault, not a range fault.
        check, _ = proto.check_stability("", 0, first, proto.utf8_len(first) - 2)
        self.assertIs(check, proto.StabilityCheck.NOT_GRAPHEME_BOUNDARY)


class StableBytesInvariantOnTheWire(unittest.TestCase):
    """The same invariant driven through `classify_inbound`, which is what a receiver runs.

    `check_stability` is the predicate; this is the state machine around it. The distinction
    matters because §8.3's requirements are about *state*: do not clamp, do not absorb the bad
    message, abort the session rather than the connection.
    """

    def receiver(self) -> proto.ReceiverState:
        return proto.ReceiverState(
            current_session_id="s7", state=proto.ConnState.RUNNING, last_seq=0, utterance_id=0
        )

    def partial(self, seq: int, text: str, stable: int, utterance: int = 1) -> proto.Partial:
        return proto.Partial(
            session_id="s7", utterance_id=utterance, seq=seq, text=text, stable_bytes=stable
        )

    def test_a_conforming_stream_is_accepted_and_moves_the_high_water_mark(self) -> None:
        state = self.receiver()
        for seq, (text, stable) in enumerate(
            [("can", 0), ("can you", 4), ("can you change", 8), ("can you change the", 15)],
            start=1,
        ):
            outcome = proto.classify_inbound(state, self.partial(seq, text, stable))
            self.assertIs(outcome.verdict, proto.Verdict.ACCEPT, (seq, text))
            self.assertEqual(state.last_stable_bytes, stable)
            self.assertEqual(state.last_stable_text, text)

    def test_a_violation_aborts_the_session_and_absorbs_nothing(self) -> None:
        """§8.3: preserve the document, abort the session, keep the connection, never clamp.

        The state assertions after the violation are the important half. A receiver that recorded
        the offending message's `seq` or `stable_bytes` — "clamp and continue" — would let the next
        hypothesis look conforming relative to a mark the sender never legitimately published.
        """
        state = self.receiver()
        self.assertIs(
            proto.classify_inbound(state, self.partial(1, "can you change", 8)).verdict,
            proto.Verdict.ACCEPT,
        )

        outcome = proto.classify_inbound(state, self.partial(2, "can we change the", 12))
        self.assertIs(outcome.verdict, proto.Verdict.PROTOCOL_ERROR)
        assert outcome.error is not None
        self.assertEqual(outcome.error.code_raw, "stability_violation")
        self.assertFalse(outcome.error.fatal, "the session dies, the connection does not")
        self.assertEqual(outcome.error.at_offset, 4)
        self.assertIn("preserve document", outcome.effects)

        self.assertEqual(state.last_seq, 1, "the bad message must not advance seq")
        self.assertEqual(state.last_stable_bytes, 8, "the mark must not be clamped or moved")
        self.assertEqual(state.last_stable_text, "can you change")
        self.assertIs(state.state, proto.ConnState.FINALIZING)

    def test_the_mark_resets_at_an_utterance_boundary_and_not_at_a_seq_gap(self) -> None:
        """§8.2's last paragraph, which is the rule a buggy backend would otherwise escape through.

        Resetting on a gap would let a sender that skipped `seq` numbers publish anything it liked;
        a gap means hypotheses were *missed*, not retracted.
        """
        state = self.receiver()
        proto.classify_inbound(state, self.partial(1, "first utterance", 5, utterance=1))

        # A gap: accepted, logged, anchor confidence reset — but still held to the mark.
        outcome = proto.classify_inbound(state, self.partial(4, "worst utterance", 5, utterance=1))
        self.assertIs(outcome.verdict, proto.Verdict.PROTOCOL_ERROR)
        assert outcome.error is not None
        self.assertEqual(outcome.error.code_raw, "stability_violation")

        state = self.receiver()
        proto.classify_inbound(state, self.partial(1, "first utterance", 5, utterance=1))
        outcome = proto.classify_inbound(state, self.partial(4, "first utterance too", 5,
                                                            utterance=1))
        self.assertIs(outcome.verdict, proto.Verdict.ACCEPT_AFTER_GAP)
        self.assertEqual(outcome.gap, {"expected_seq": 2, "received_seq": 4, "missing": 2})
        self.assertIn("reset_anchor_confidence", outcome.effects)

        # A new utterance starts from zero: unrelated text with a lower mark is legitimate.
        outcome = proto.classify_inbound(state, self.partial(5, "second", 0, utterance=2))
        self.assertIs(outcome.verdict, proto.Verdict.ACCEPT)
        self.assertEqual(state.utterance_id, 2)

    def test_a_final_must_extend_the_published_prefix(self) -> None:
        """§8.2: "The same applies to `final`" — a `final` is not exempt from the invariant.

        This is the one that reaches the user's document. Everything below `stable_bytes` has
        already been typed by the time the `final` arrives, so a `final` that contradicts it is
        asking for a repair that §1.4 forbids.
        """
        state = self.receiver()
        proto.classify_inbound(state, self.partial(1, "can you change", 8))

        good = proto.Final(session_id="s7", utterance_id=1, seq=2,
                           text="Can you change the config.", audio_pos_ms=4820)
        bad = proto.Final(session_id="s7", utterance_id=1, seq=2,
                          text="can we change the config.", audio_pos_ms=4820)

        outcome = proto.classify_inbound(state, bad)
        self.assertIs(outcome.verdict, proto.Verdict.PROTOCOL_ERROR)
        assert outcome.error is not None
        self.assertEqual(outcome.error.code_raw, "stability_violation")

        # "Can you change" differs from the published "can you " in byte 0 only by case, so it is
        # a violation too — capitalisation of committed text is still a retraction.
        state = self.receiver()
        proto.classify_inbound(state, self.partial(1, "can you change", 8))
        outcome = proto.classify_inbound(state, good)
        self.assertIs(outcome.verdict, proto.Verdict.PROTOCOL_ERROR,
                      "recasing a committed byte is a retraction, not a courtesy")

        # A final that does extend the published prefix is accepted and resets the mark.
        state = self.receiver()
        proto.classify_inbound(state, self.partial(1, "can you change", 8))
        outcome = proto.classify_inbound(
            state,
            proto.Final(session_id="s7", utterance_id=1, seq=2,
                        text="can you change the config.", audio_pos_ms=4820),
        )
        self.assertIs(outcome.verdict, proto.Verdict.ACCEPT)
        self.assertEqual(state.last_stable_bytes, 0, "the mark resets at the utterance boundary")

    def test_a_dropped_message_never_triggers_a_stability_violation(self) -> None:
        """Rule order (§7.4 before §8.2): a duplicate is dropped before it can be judged.

        Otherwise a retransmitted hypothesis — which carries no new information at all — could
        abort a healthy session.
        """
        state = self.receiver()
        proto.classify_inbound(state, self.partial(1, "can you change", 8))
        proto.classify_inbound(state, self.partial(2, "can you change the", 15))

        stale = self.partial(1, "can we change", 4)      # would be a violation if it were judged
        outcome = proto.classify_inbound(state, stale)
        self.assertIs(outcome.verdict, proto.Verdict.DROP_OUT_OF_ORDER)
        self.assertIsNone(outcome.error)
        self.assertEqual(state.last_stable_bytes, 15)
        self.assertEqual(state.last_seq, 2)

    def test_stable_bytes_must_land_on_a_cluster_boundary_of_the_new_text(self) -> None:
        """Range-checked at decode time, boundary-checked as session state — both must fire."""
        state = self.receiver()
        outcome = proto.classify_inbound(state, self.partial(1, "café", 4))
        self.assertIs(outcome.verdict, proto.Verdict.PROTOCOL_ERROR)
        assert outcome.error is not None
        self.assertEqual(outcome.error.code_raw, "stability_violation")

        # Out of range is caught one layer lower, as a schema fault, because a `stable_bytes`
        # larger than the text it indexes is a malformed message rather than a bad hypothesis.
        with self.assertRaises(proto.ProtocolError) as caught:
            proto.decode_control(
                proto.encode_control(self.partial(1, "café", 99))
            )
        self.assertEqual(caught.exception.code_raw, "bad_message")


if __name__ == "__main__":  # pragma: no cover
    unittest.main()


class TestPipeAccessMaskMatchesTheCxxDacl(unittest.TestCase):
    """The client's CreateFileW mask and the parent's DACL are one contract in two files.

    They live in different languages and neither compiler can see the other, so the only thing
    keeping them in step is this test. Getting it wrong does not degrade anything gracefully:
    the sidecar simply never connects, with ERROR_ACCESS_DENIED, on every launch.
    """

    def test_mask_matches_kd_kPipeAccessRights(self):
        import re
        from pathlib import Path

        from kvit_asr.transport import PIPE_ACCESS_RIGHTS

        header = Path(__file__).resolve().parents[2] / "src" / "ipc_client.h"
        text = header.read_text(encoding="utf-8")
        m = re.search(r"kPipeAccessRights\s*=\s*(0x[0-9A-Fa-f]+)u?", text)
        self.assertIsNotNone(m, "kPipeAccessRights not found in src/ipc_client.h")
        self.assertEqual(
            PIPE_ACCESS_RIGHTS,
            int(m.group(1), 16),
            "transport.py's PIPE_ACCESS_RIGHTS and ipc_client.h's kPipeAccessRights disagree; "
            "the sidecar will fail to open the pipe",
        )

    def test_mask_excludes_create_pipe_instance(self):
        from kvit_asr.transport import PIPE_ACCESS_RIGHTS

        FILE_CREATE_PIPE_INSTANCE = 0x00000004
        self.assertEqual(
            PIPE_ACCESS_RIGHTS & FILE_CREATE_PIPE_INSTANCE,
            0,
            "requesting FILE_CREATE_PIPE_INSTANCE asks for the right to create rival instances "
            "of our own pipe name (plan.md 1.2)",
        )
