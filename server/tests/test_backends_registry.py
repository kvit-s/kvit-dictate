"""The registry, and the command line's route into a backend.

Two defects motivated this file, and both were invisible to every other suite:

1. **Four backend modules existed and none was registered.** `backends/__init__.py` listed only
   the mock, so `--backend transformers-whisper` — the spelling models.json actually passes —
   failed with "unknown backend" no matter what was installed. :func:`test_models_json_backends`
   is the cross-check that would have caught it: every `--backend X` in models.json must name a
   registry row.

2. **`open_backend` dropped the typed command line.** It documented a `from_backend_config` hook
   and no backend provided one, so construction fell back to `cls(**--backend-opt)` and `--model`,
   `--chunk-ms`, `--device` and `--model-dir` were silently discarded. A run started with
   `--model openai/whisper-large-v3-turbo` would have loaded `DEFAULT_MODEL` instead and reported
   that model's numbers under the requested model's name.

Nothing here imports a model runtime, and that is itself asserted: the whole point of the lazy
registry is that a machine with no torch and no sherpa-onnx can still enumerate, resolve and
*construct* every backend, failing only when `load()` is called.
"""

from __future__ import annotations

import json
import unittest
from pathlib import Path

from kvit_asr.backends import (
    Backend,
    BackendCapabilities,
    BackendUnavailable,
    REGISTRY,
    StreamingMode,
    backend_names,
    load_backend,
    probe,
)
from kvit_asr.config import BackendConfig

REPO_ROOT = Path(__file__).resolve().parents[2]


class RegistryTests(unittest.TestCase):
    def test_every_entry_names_module_and_attribute(self) -> None:
        for name, target in REGISTRY.items():
            with self.subTest(backend=name):
                self.assertIn(":", target, f"{name} target must be 'module:attribute'")

    def test_every_entry_imports_without_its_runtime(self) -> None:
        """The lazy-import rule, asserted rather than trusted.

        `backends/base.py` and `backends/__init__.py` both promise that a heavy import lives inside
        the concrete backend's `_load()`. If one migrates to module scope, this fails on any
        machine without that runtime — which is the machine the promise exists for.
        """
        for name in backend_names():
            with self.subTest(backend=name):
                self.assertIsNone(probe(name), f"{name} could not be imported")

    def test_every_entry_resolves_to_a_backend_subclass(self) -> None:
        for name in backend_names():
            with self.subTest(backend=name):
                cls = load_backend(name)
                self.assertTrue(issubclass(cls, Backend), f"{name} is not a Backend subclass")

    def test_unknown_backend_lists_what_is_registered(self) -> None:
        with self.assertRaises(BackendUnavailable) as caught:
            load_backend("no-such-backend")
        message = str(caught.exception)
        self.assertIn("no-such-backend", message)
        for name in backend_names():
            self.assertIn(name, message)

    def test_models_json_backends(self) -> None:
        """Every `--backend X` in models.json names a registry row.

        models.json is the C++ side's list of what a human can select; the registry is the Python
        side's list of what can be loaded. Nothing else compares the two, and when they disagree
        the failure surfaces only when someone picks that model — as an "unknown backend" from a
        sidecar that has already been spawned.
        """
        manifest = json.loads((REPO_ROOT / "models.json").read_text(encoding="utf-8"))
        seen = 0
        for entry in manifest["models"]:
            args = list(entry.get("args") or [])
            if "--backend" not in args:
                continue
            name = args[args.index("--backend") + 1]
            seen += 1
            with self.subTest(model=entry["id"], backend=name):
                self.assertIn(
                    name,
                    REGISTRY,
                    f"models.json entry {entry['id']!r} selects --backend {name!r}, which is not "
                    f"in backends.REGISTRY ({', '.join(backend_names())})",
                )
        self.assertGreater(seen, 0, "models.json declared no --backend at all")

    def test_models_json_args_parse(self) -> None:
        """Every entry's `args` survive the sidecar's own argument parser.

        models.json is written by hand and consumed by a process that has already been spawned, so
        a typo here becomes "the sidecar exited 2 before the handshake" — which is what defect §5.5
        of handover.md actually was. Parsing the arguments in-process turns that into a test.
        """
        from kvit_asr.config import build_parser

        manifest = json.loads((REPO_ROOT / "models.json").read_text(encoding="utf-8"))
        parser = build_parser()
        for entry in manifest["models"]:
            with self.subTest(model=entry["id"]):
                args = ["--pipe", r"\\.\pipe\kvit-test", *(entry.get("args") or [])]
                parser.parse_args(args)


class FromBackendConfigTests(unittest.TestCase):
    """The `--model` / `--chunk-ms` / `--device` route, for the backends that have the hook.

    Constructed only — never loaded — so these run on a machine with neither torch nor CTranslate2.
    """

    def _construct(self, name: str, **fields):
        cls = load_backend(name)
        factory = getattr(cls, "from_backend_config", None)
        self.assertTrue(callable(factory), f"{name} has no from_backend_config hook")
        return factory(BackendConfig(id=name, **fields))

    # -- transformers-whisper -------------------------------------------------------------------

    def test_transformers_whisper_takes_model_and_hop_and_device(self) -> None:
        b = self._construct(
            "transformers-whisper",
            model="openai/whisper-large-v3-turbo",
            chunk_ms=320,
            device="cuda",
        )
        self.assertEqual(b.config.model, "openai/whisper-large-v3-turbo")
        self.assertEqual(b.config.hop_ms, 320)
        self.assertEqual(b.config.device, "cuda")
        # `--chunk-ms` must reach `ready.chunk_ms`, not merely the config object.
        self.assertEqual(b.capabilities.natural_chunk_ms, 320)
        self.assertEqual(b.capabilities.model_id, "openai/whisper-large-v3-turbo")

    def test_transformers_whisper_leaves_auto_device_to_the_backend(self) -> None:
        b = self._construct("transformers-whisper", model="openai/whisper-tiny")
        self.assertEqual(b.config.device, "auto")

    def test_transformers_whisper_backend_opt_wins(self) -> None:
        b = self._construct(
            "transformers-whisper",
            model="openai/whisper-tiny",
            chunk_ms=320,
            options={"hop_ms": "640", "num_beams": "4"},
        )
        self.assertEqual(b.config.hop_ms, 640)
        self.assertEqual(b.config.num_beams, 4)

    def test_transformers_whisper_rejects_model_dir(self) -> None:
        with self.assertRaises(ValueError) as caught:
            self._construct("transformers-whisper", model="x", model_dir=Path("D:/models/x"))
        self.assertIn("HF_HOME", str(caught.exception))

    def test_transformers_whisper_rejects_decoding(self) -> None:
        with self.assertRaises(ValueError) as caught:
            self._construct("transformers-whisper", model="x", decoding="modified_beam_search")
        self.assertIn("num_beams", str(caught.exception))

    def test_transformers_whisper_rejects_dml(self) -> None:
        with self.assertRaises(ValueError):
            self._construct("transformers-whisper", model="x", device="dml")

    def test_transformers_whisper_rejects_unknown_backend_opt(self) -> None:
        with self.assertRaises(ValueError) as caught:
            self._construct("transformers-whisper", model="x", options={"hopms": "640"})
        self.assertIn("hopms", str(caught.exception))

    # -- faster-whisper -------------------------------------------------------------------------

    def test_faster_whisper_takes_model_dir(self) -> None:
        b = self._construct(
            "faster-whisper",
            model="Systran/faster-whisper-large-v3",
            model_dir=Path("D:/models/fw-large-v3"),
            chunk_ms=320,
            device="cpu",
        )
        self.assertEqual(b.config.model, "Systran/faster-whisper-large-v3")
        self.assertEqual(b.config.model_dir, str(Path("D:/models/fw-large-v3")))
        self.assertEqual(b.config.hop_ms, 320)
        self.assertEqual(b.config.device, "cpu")

    def test_faster_whisper_rejects_decoding(self) -> None:
        with self.assertRaises(ValueError) as caught:
            self._construct("faster-whisper", model="x", decoding="greedy_search")
        self.assertIn("beam_size", str(caught.exception))

    def test_faster_whisper_rejects_dml(self) -> None:
        with self.assertRaises(ValueError):
            self._construct("faster-whisper", model="x", device="dml")


class RecordingBackend(Backend):
    """A `Backend` that records how `open_backend` built it. Registered only inside a test."""

    calls: list = []

    @classmethod
    def from_backend_config(cls, cfg):
        instance = cls()
        cls.calls.append(("from_backend_config", cfg))
        return instance

    @property
    def capabilities(self) -> BackendCapabilities:
        return BackendCapabilities(model_id="recording", mode=StreamingMode.NATIVE_STREAMING)

    def _load(self, progress):
        type(self).calls.append(("load", progress))

    def feed(self, pcm):
        return []

    def finalize(self):
        return None

    def _reset(self, reason):
        pass


class OpenBackendHookTests(unittest.TestCase):
    """`open_backend` must prefer `from_backend_config` and must still load afterwards."""

    def test_prefers_from_backend_config(self) -> None:
        from kvit_asr.connection import open_backend

        RecordingBackend.calls = []
        original = dict(REGISTRY)
        REGISTRY["_recording"] = f"{__name__}:RecordingBackend"
        try:
            cfg = BackendConfig(id="_recording", model="m", chunk_ms=160)
            loaded = open_backend(cfg)
        finally:
            REGISTRY.clear()
            REGISTRY.update(original)

        kinds = [c[0] for c in RecordingBackend.calls]
        self.assertEqual(kinds, ["from_backend_config", "load"])
        self.assertIs(RecordingBackend.calls[0][1], cfg)
        self.assertEqual(loaded.model_id, "recording")

    def test_falls_back_to_backend_opt_keywords(self) -> None:
        """The mock has no hook, so it must still be constructible from `--backend-opt`.

        models.json's `mock-ungated` entry depends on exactly this path.
        """
        from kvit_asr.connection import open_backend

        loaded = open_backend(
            BackendConfig(id="mock", options={"silence_peak_threshold": "0"})
        )
        self.assertEqual(loaded.model_id, "mock")


if __name__ == "__main__":  # pragma: no cover
    unittest.main()
