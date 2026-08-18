# Handover — kvit-dictate

Written 2026-08-17, end of the second session. Commit `feeb92f` on `main`.
Amended at the end of the third session (`c270faf`, then `d2bac8b`); see §9 for what that
changed and §10 for what a human then observed. **§4 checks 1 and 2 are now confirmed working**;
3, 4 and 5 remain unobserved, and check 3 was described in a way that cannot work — see it.

Read this with `plan.md` (the specification), `README.md` (current status) and
`docs/protocol.md` (the wire contract). This file covers only what those three do not: where the
work actually stands, what is untrue about it, and what to do next.

---

## 1. State in one paragraph

**Dictation works.** Press `Win+Y` in Notepad3, the mock backend's sentence is typed in word by
word as the hypotheses stabilise, press `Win+Y` again and the `final`'s remaining tail is appended
once — verified by a human watching it happen. That path had never run before this session and was
broken in nineteen places, none of which 691 passing tests could see. 728 tests now pass across
four suites at zero warnings, with warnings as errors. What is still untrue of the product: **no
real speech has ever been transcribed** (every word so far came from the mock's script), no model
has been downloaded, and no Phase 0B cell has been measured. ~~the inline overlay has never been
seen doing its job~~ — **it has now**: it paints, it sits at the caret, and it follows the caret
as text is committed (third session, §10). Windows 11's own Notepad garbles our injected
keystrokes, which is a target finding and the first entry in the supported-targets matrix that
came from running the product.

---

## 2. How to build and test

```
cmake --preset windows-msvc-debug
cmake --build build-windows-msvc-debug --config Debug
build-windows-msvc-debug\bin\Debug\kd_tests.exe            # 299 tests / 35820 checks
cd build-windows-msvc-debug && ctest -C Debug              # 14 suites, none disabled

python -m unittest discover -s server/tests -t server      # 372 tests  (run from repo root)
python -m unittest discover -s bench/tests  -t .           # 129 tests  (run from repo root)
```

Use `C:\Python312\python.exe`. The `python` on PATH in Git Bash is a conda 3.9 and will fail.

Working notes that cost time to rediscover:

* **Kill any running `kvit-dictate.exe` before rebuilding.** It holds the link output *and* the
  single-instance mutex, so a stale one produces `LNK1168` *and* a new instance that exits
  immediately — two confusing symptoms of one cause. `taskkill /F /IM kvit-dictate.exe`.
* `cmake` is at `C:\Program Files\CMake\bin\cmake.exe`; Ninja is NOT on PATH, so the VS 2022
  generator is the only one configured.
* The two Python suites must be run **from the repository root**; `-t` sets the import root and
  running from inside `server/` or `bench/` fails with "Start directory is not importable".
* `kd_tests.exe --list` prints every test; `--suite <name>` filters.
* `KD_WERROR` is **ON**. Debug and Release both build the whole tree at zero warnings. MSVC emits
  a spurious C4129 for escape-looking sequences inside *raw* string literals; tests/test_json.cpp
  writes those two cases with ordinary escapes instead.

### 2.1 Running it, and reading what happened

`%LOCALAPPDATA%\kvit-dictate\settings.json` is currently:

```json
{
  "schema": 1,
  "model_id": "mock",
  "privacy": { "log_level": "debug", "diagnostics_file_enabled": true },
  "app_policies": [ { "process_name": "notepad.exe", "method": "Clipboard", ... } ]
}
```

The diagnostics file only exists because `diagnostics_file_enabled` is on — it is opt-in for the
reason plan.md §1.9 gives, and without it the log goes to `OutputDebugString` and nowhere else.
It lands in `%LOCALAPPDATA%\kvit-dictate\logs\kvit-dictate.log`.

Three model entries in `models.json` matter:

* `mock` — realistic: the VAD is in the path, so it needs real sound to produce anything.
* `mock-ungated` — `--vad none` plus `silence_peak_threshold=0`, so the script advances on
  whatever the microphone captures, including a silent room. **This is the one to use when
  testing the C++ side**, because a failure is then attributable to the client and not to a
  detector or to the room.
* `mock-hostile` — `mock-ungated` plus a sidecar that knowingly emits messages the protocol
  forbids: after two clean emissions the reported `stable_bytes` goes backwards, retracting text
  the client has already typed. Added in the third session and **never run against the C++ side**.
  See §9.

**A diagnostic worth knowing:** the mock advances exactly one script line per *utterance*, in a
fixed order ("can you change the llama.cpp config…", "open src/protocol.h…", "the café bill…"). So
the number of distinct sentences in the document is the number of utterances that happened, and
seeing sentence 3 means two utterance boundaries fired. That is how the missing space between
utterances was spotted.

**Nothing can test the hotkey chord except a human.** The input filter ignores every event flagged
`LLKHF_INJECTED`, by design (plan.md §1.8: our own SendInput must never stop the session producing
it), so a synthesised `Win+Y` activates nothing, ever. The message underneath it *can* be driven:
post `WM_APP+0x11` with wParam 0 (activate) or 1 (finalize) to the message-only window, found with
`FindWindowExW(HWND_MESSAGE, nullptr, L"kvit-dictate-message-window", nullptr)`. That is how the
injection path was exercised without a human. **Check `GetForegroundWindow()` first and refuse to
proceed unless it is your own target window** — the text goes wherever focus is, and on a machine
someone is using, focus moves.

---

## 3. What is done, and what that means

| Area | State |
|---|---|
| `docs/protocol.md` + 20 fixtures | Written, and **both endpoints execute the same corpus**. |
| C++ modules | All 13 implemented. 299 tests. |
| `src/main.cpp` | **Runs, end to end**, against a real target. Still no test touches it, and every defect in §5 lived here or in a seam it owns. |
| Python sidecar | Implemented and integrated: `kvit_asr/connection.py` is the connection-scoped layer that was missing entirely. 372 tests. All five backends reachable by name; only `mock` has run. |
| Bench harness | Recorder, metrics (incl. ICER), matrix runner with the pre-registered selection rule. 129 tests. Never run against real data. |
| `tools/testtarget` | Used as an injection target. Its own log pane — the thing that reports which code points arrived — has never been read. |
| `tools/probe` | Builds, walks a resumable 162-cell matrix. **0 cells measured.** |

### Explicitly NOT done

* **No model downloaded.** plan.md §1.9 makes fetching a deliberate, user-initiated act, so this
  is correct rather than an oversight — but every backend except `mock` is untested against
  reality. ~~only `mock` is registered~~ — *fixed in the third session:* all five are in
  `backends/__init__.py`'s REGISTRY, `--model`/`--chunk-ms`/`--device` now reach them through
  `from_backend_config`, and `backends/sherpa_adapter.py` is the `Backend`-ABC translation the
  two sherpa modules never had. Selecting one now fails at `load()` with the environment to
  create, which is the correct failure. Nothing has decoded a sample.
* **No environments created.** `envs/` does not exist. The mock entries need none — they run on
  `C:\Python312\python.exe`, because the sidecar core is stdlib-only.
* **No corpus recorded.** `bench/prompts/{tech,hard,ambient}.jsonl` hold 91 real prompts with the
  dev/heldout split baked in; no audio exists.
* **Phase 0B not run.** `docs/supported-targets.md` is all `—`, except §5.10, which is two
  observations explicitly marked as not measurements.
* **The clipboard injection path has never executed.** It is now selected by policy for
  `notepad.exe` and nobody has watched it run — including whether the clipboard is restored.
* ~~**`Hypothesis.stable_bytes_override` is not honoured.**~~ *Done in the third session.* The
  seam carries it, gated on a new server-level `--allow-hostile-backend` (not a `--backend-opt`:
  a backend must not authorise its own violations), and `models.json` has a `mock-hostile` entry
  whose args a test drives verbatim. A decreasing `stable_bytes` and a mutated stable prefix are
  both observed on the wire by `tests/test_hostile_stability.py`. **The C++ half has still never
  seen one** — the sidecar can now produce the input §8.3 handles, and pointing a real dictation
  session at `mock-hostile` is the check nobody has run.

---

## 4. Do this first: four things built but never observed

All four were committed **after** the last human test, so none has been seen working. All four are
one dictation session each.

Run with `model_id: "mock-ungated"` unless the check needs speech. **Checks 1, 2 and 4 only** —
check 3 needs an utterance boundary, which `mock-ungated` cannot produce; see it for why.

1. **The overlay actually paints.** `set_text` only marks the window dirty; `render()` draws.
   main.cpp called `render()` only when a new *anchor* arrived, so the preview was invisible for
   whole sessions. Now the 50 ms tick always calls it (it no-ops unless something changed). Look
   for a **light box with dark grey text** showing the not-yet-committed words. If you see a blue
   box with white text, that is the target's own autocomplete — Notepad3 does this — not us.
2. **Where the overlay sits.** At the caret (inline, what plan.md §1.5 wants) or off in a corner
   (the floating-pill fallback, meaning the anchor is below `min_confidence` 0.5). The second fix
   in that commit is why this might now differ: anchor.h asks for `refresh_caret` "at session
   start **and after each commit**" and main.cpp only did the first, so even a perfect caret
   anchor went stale after `stale_after_ms` (3 s) and degraded for the rest of the session.
3. **The space between utterances.** ~~Dictate long enough for two sentences.~~ **Not testable
   with `mock-ungated`, and the instruction above was wrong.** That entry passes `--vad none`, so
   the session is one long utterance (connection.py's `make_vad` says exactly this), and the mock
   resets `_utterance_index` to 0 on every SESSION_START/SESSION_END (mock.py:494). One session
   therefore replays script line 1 and stops there, however long you wait — as a 42-second run in
   the third session confirmed. An utterance boundary needs the VAD, so this check merges into
   check 5 below: run it with `mock`, speak, pause, speak. Expect
   `…the default model open src/protocol.h…`, not `…modelopen…`.
4. **The clipboard path, in Notepad.** `notepad.exe` now has an app policy selecting it, because
   Win11's Notepad garbles paced SendInput (§5.10 of the supported-targets doc). Two questions:
   is the pasted text clean, and is the clipboard restored afterwards? plan.md §1.7 says restore
   only if we still own what we set — losing a user's clipboard is worse than leaving ours in it.

Then, needing real speech and `model_id: "mock"`:

5. **The endpointer.** Speak, pause for more than a second mid-sentence, speak again, and see
   whether a `final` closes the utterance without ending the session (plan.md §1.6). Leave a beat
   of silence *after* the hotkey: the stdlib energy fallback calibrates its noise floor from the
   quietest frame of the opening 200 ms, so a stream that opens on speech calibrates the floor to
   the speech and hears nothing after it.

---

## 5. Defects, and where they came from

The first four are the previous session's, found by tests. The rest are this session's; **not one
was visible to a passing test**, and the last five were found by a human pressing the hotkey and
describing what happened.

Found by tests:

1. **`common_grapheme_prefix` returned 0 on mid-code-point divergence**, so nothing would commit
   for the rest of any utterance containing `café`/`cafè`, `и`/`й`, or two CJK characters sharing
   lead bytes.
2. **`at_offset` was one byte high** in the C++ JSON decoder. Every C++ unit test agreed with the
   bug because they shared its misunderstanding; only fixture 008 disagreed.
3. **The sidecar could never have connected** — it opened the pipe with `GENERIC_READ |
   GENERIC_WRITE`, which asks for rights the DACL deliberately does not grant.
4. **The hotkey's binding key never left the down-at-activation set.**

Found by running the two processes:

5. **The sidecar was launched without `-m <module>`** and without the package on its PYTHONPATH,
   so it died on an unknown option before the pipe mattered. main.cpp also never applied a model
   entry's `env`, never fell back to `default_model` (so a first run with no settings file started
   no sidecar at all), and never gave `log::Config` a file even when the settings asked for one.
6. **`decode_control` returned a `Message`; `server.py` speaks dicts.** The handshake died on
   `'Hello' object has no attribute 'items'`. It could only ever have worked against the test
   suite's shim.
7. **Every `error.code` the sidecar raised as a `ProtocolError` reached the wire spelled
   `"ErrorCode.state_violation"`** — outside §11.3's catalogue, which §13 tells a receiver to treat
   as an error about an error. Already pinned as an `expectedFailure`; the fix is `code_raw`.
8. **Nothing connected `server.py` to `session.py`, `backends/` or `vad.py`.** Four modules, four
   plausible interfaces, no owner for the space between: connection-scoped versus session-scoped,
   `feed`/`finalize`/`Hypothesis` versus `accept_audio`/`finalise`/`str`, absolute backdated sample
   positions versus byte offsets into the current chunk. `kvit_asr/connection.py` is those three
   translations and nothing else.
9. **Two authors for `hello`.** `IpcClient` wrote the handshake's first frame and so did
   `SessionMachine`, which is the half the frozen fixture corpus tests. Nothing forwarded the
   transport's walk to the machine, so it sat in SPAWNING and killed every connection on the
   `hello_ack`. The machine keeps the frame; main.cpp maps connection states onto
   `Connected`/`PeerValidated`/`ChildRespawned`.
10. **`FindWindowW` cannot find a message-only window**, so the single-instance path returned null
    every time and a second launch exited in silence — the exact "double-click does nothing"
    outcome it was written to avoid.
11. **The settings dialog was unreachable.** ~200 lines of programmatic Win32 that nothing called.

Found by a human at the keyboard:

12. **The hotkey's message never reached its listener.** hotkey.h's contract is that the hook posts
    `WM_APP+0x11` and the owner hands it back through `dispatch_hotkey_message()`. main.cpp had no
    case for it, so the message fell into DefWindowProc: the hook installed, saw the chord, posted,
    and nothing happened. Two neighbouring contracts were also unwired — `set_injection_mark`
    (without which our own injected keystrokes read as the user typing and stop the session
    producing them) and `note_session_started`/`note_session_ended`.
13. **The modifier of the finalising chord was read as user input.** `Win+Y` arrives as LWIN down,
    then Y down; the LWIN stopped the session, and the Y then found none running and started a new
    one — so a second press produced a second block of text. The existing test pressed both keys
    and passed, because it ignored the modifier's outcome and nothing in a test ends a session;
    only the owner does.
14. **The target was released before `final` could arrive.** `end_dictation` reset it immediately
    after sending `stop`, so the tail of every utterance — the part `stop` exists to request — found
    no target and was dropped. Held until `stopped` now, with a finite grace.
15. **Source C was handed an empty capture rectangle**, so every before-frame capture failed and
    commit differencing had never run once. `capture_region_for` wants a box around the insertion
    point and a `MonitorGeometry`, which nothing in the tree could produce.
16. **The overlay was never repainted.** See §4.1.
17. **The caret anchor was refreshed only at session start**, so it went stale after 3 s. See §4.2.
18. **`CommitOp::leading_space` was ignored**, so every utterance ran into the one before it:
    `…the default modelopen src/protocol.h…`. The engine holds back trailing whitespace precisely
    so the boundary side adds that space exactly once, and the boundary side never did.
19. **`find_app_policy` had never been called**, so the whole per-application tier mechanism that
    `docs/supported-targets.md` is built around did nothing.

**The pattern, stated because it will repeat:** every one of 12–19 is a contract written down in a
header, implemented on one side, and never called from the other. The modules are not the problem;
`main.cpp` is where they were supposed to meet, and it is the one file nothing tests. When
something does not work, grep the relevant header for the function nobody calls before suspecting
the module.

---

## 6. Open questions recorded rather than guessed

* **Windows 11 Notepad garbles paced `SendInput`** — long runs of one character, most others
  dropped, while Notepad3 takes the identical bytes perfectly. Whether slower pacing fixes it, or
  whether the clipboard path is the answer, is §4.4.
* **`WDA_EXCLUDEFROMCAPTURE` vs a desktop-DC `BitBlt`.** The overlay excludes itself from capture
  so it cannot contaminate the anchor diff (plan.md §1.5). Whether the exclusion actually applies
  to a desktop-DC `BitBlt` is unverified, and if it does not, the overlay pollutes every source-C
  measurement and Phase 0B's numbers are worthless. **Now testable**, because source C runs.
* **Windows.Graphics.Capture desktop path**, and whether it shows a user-visible capture
  indicator. `make_wgc_capture` is deliberately unimplemented; anchor.cpp detects the all-black
  `BitBlt` that requires it and reports it.
* **WSLg + `KEYEVENTF_UNICODE`.** Whether Unicode survives translation to X11 keysyms across the
  RDP pipe. Phase 0B scenario A04 decides it.
* **The energy VAD's noise-floor warmup.** It is deaf to speech starting within the first 200 ms
  of a session. Silero would not have this wart. `tests/test_connection.py` pins the behaviour so
  it is at least not a surprise.
* **Pre-roll is dropped across the VAD seam.** `VadEndpointer` replays `preroll_ms` of audio from
  before a detected onset; `session.Session` gates by splitting the chunk it was given, so that
  audio is never fed. The cost is the leading fraction of the first word. Fixing it means teaching
  `Session` to consume the VAD's audio instead of its own — a change to a tested module, and it
  belongs in the same pass as measuring whether it matters.
* **Unicode version drift.** `utf8::kUnicodeVersion` is literally
  `"unspecified-pending-protocol-md-14.2"`. The real fix is generating both tables from one source.
* **`onnxruntime-gpu` on sm_120 (Blackwell)** — unconfirmed; gates nothing, because the chosen
  model runs faster than real time on CPU.

---

## 7. Suggested order

1. **The five checks in §4.** Each is one session, each decides something, and four of them are
   code written after anyone last looked.
2. **Phase 0B**, with `tools/probe --status` to see the matrix, then cell by cell. This is the gate
   that decides whether the inline overlay survives at all — plan.md is explicit that missing it
   changes both the UX and the commit policy, and that *not knowing* is the only failure. Add
   Notepad to the application list while you are there; it is the first thing anyone tries and it
   is already known to break the default method.
3. **Fetch the ONNX model** (`bench/CANDIDATES.md` pick 1, a few hundred MB, no PyTorch) and make
   the sidecar real. ~~register the backend in REGISTRY~~ — done; what remains is `envs/onnx`, the
   weights, and the first real `load()`. Note `UV_CACHE_DIR` must be on `D:` — uv hardlinks from
   cache into venvs and hardlinks cannot cross volumes, so a `C:` cache with `D:` venvs silently
   full-copies everything. Expect `sherpa_onnx_streaming.py`'s eight recorded UNVERIFIED items to
   be what the first run is actually about; the adapter around it is tested, the assumptions
   inside it are not.
4. **Record the corpus** with `bench/record.py` — but read its report first: it is written against
   the C++ capture path and may need a recording entry point `audio_capture` does not yet expose.

---

## 8. Conventions to preserve

* **Commit after each working step, directly to `main`.** Verify before committing — a commit
  should mark a state where the build and the tests actually pass.
* No third-party C++ dependencies. No Qt. Everything in-repo, including the JSON parser.
* No header under `src/` includes `<windows.h>`; handles cross module boundaries as `uintptr_t`.
  This is what keeps the pure modules unit-testable, so do not "fix" it.
* The sidecar core is stdlib-only. Only individual backends may import numpy/onnxruntime/torch,
  and each such import lives inside the backend module, so a missing runtime disables one backend
  rather than the server.
* `..\wan-dancer\.venv` is **read-only**. Run its Whisper baseline; install nothing into it.
* Comments explain *why*. Where something is unverified, the code says so rather than asserting.
* Measurements and observations are different things, and `docs/supported-targets.md` keeps them in
  different sections. Nothing that was not taken with the probe may fill in a cell.
* Empty test suites are registered `DISABLED` in `CMakeLists.txt` rather than omitted, so the gap
  between "planned" and "covered" shows up in every ctest run. `KD_TEST_SUITES_PENDING` is
  currently empty; a new module starts there.

---

## 9. What the third session did

No C++ was written, no hotkey was pressed, and no model was fetched. Everything in §4 is still
unobserved, and §1's paragraph is still an accurate description of the product.

Two commits, both Python, both closing items §3 listed as not done. Both were the same defect as
§5's list — *a contract written down on one side and never called from the other* — which is now
the third session in a row where that pattern produced the session's work.

**`9ed2dcc` — the four real backends were unreachable.** `backends/__init__.py` listed only the
mock, so every `--backend` spelling `models.json` passes resolved to "unknown backend" whatever
was installed: ~3800 lines of finished adapter with no route in. Underneath that:

* `open_backend` documented a `from_backend_config` hook and no backend provided one, so
  construction fell back to `cls(**--backend-opt)` and the *typed* command line — `--model`,
  `--chunk-ms`, `--device`, `--model-dir` — was discarded in silence. A run started with
  `--model openai/whisper-large-v3-turbo` would have loaded `DEFAULT_MODEL` and reported its
  numbers under the requested model's name.
* `SherpaOnnxStreamingBackend` is not a `Backend` at all — it was written standalone against
  `start_session`/`accept_pcm`/`finish_utterance`. `backends/sherpa_adapter.py` is the owner for
  that gap, built like `connection.py`: four named translations with what each costs written
  down. The fourth has teeth — the inner raises a plain `RuntimeError`, which untranslated
  reaches the wire as a fatal `internal_error` and takes the connection down rather than the
  session.
* `models.json`'s zipformer entry said `--backend sherpa-onnx --decoding modified_beam_search`,
  which could only ever have failed: the base class refuses that method outright. Biasing is a
  different class, not a flag; it is `--backend zipformer` now.

**`c270faf` — `stable_bytes_override` reaches the wire.** Four links were broken, and the third
is the one worth remembering: the session's own stability tracker raises `StabilityViolation`
exactly when a hypothesis contradicts what was published, which is what a hostile backend is
*for*, so it aborted the utterance with `decode_failed` before the manufactured frame could be
sent. A test would have looked like it ran. Forcing now stands the computation down for that
utterance and only that one. An override is deliberately **not** honoured on a `final`: §6.2 makes
`final.stable_bytes` the whole text and the decoder enforces it, so a forced partial mark there is
rejected as `bad_message` — the wrong reason, and exactly the trap `mock.py` warns about.

### What this changes about what to do next

`models.json` has a new **`mock-hostile`** entry. It is a one-session check that did not exist
before, and it belongs with the five in §4: point a real dictation session at it and watch what
the C++ side does when the sidecar retracts stable text it has already typed. §8.3's handling is
covered by fixtures at the codec level; it has never run against a live sidecar in a real
document. Expect the commit engine to refuse the retraction and log it — and if instead the
document is corrupted, that is the single most valuable defect this project can find before a real
model is ever loaded.

Otherwise the §7 order is unchanged. §7.3 is smaller now: only `envs/onnx`, the weights and the
first real `load()` remain, and the thing that first run is really about is the eight recorded
UNVERIFIED assumptions inside `sherpa_onnx_streaming.py` — the adapter around them is tested, the
assumptions themselves cannot be until sherpa-onnx is installed.

Nothing was done about the pre-roll dropped across the VAD seam (§6). §6 says it belongs in the
same pass as measuring whether it matters, and no measurement happened, so it was left alone.

Test counts: **800** across four suites — 299 C++, 372 server, 129 bench — at zero warnings.

---

## 10. What a human observed at the end of the third session

One dictation session into Notepad3 with `mock-ungated`, plus three shorter ones. The model was
switched through the tray's Settings dialog, which is itself a first: that dialog had never been
usable past the tab it opened on (see the `d2bac8b` fix), so every prior model change was a hand
edit of settings.json.

**§4.1 — the overlay paints. CONFIRMED.** The light box with dark grey text appeared, and the
observer specifically confirmed it was not the blue-on-white box Notepad3's own autocomplete draws.
`render()` on the 50 ms tick was the right fix.

**§4.2 — the overlay is inline, and it stays there. CONFIRMED, and this is the bigger one.** It sat
directly under the caret and *followed it* as text was committed. That is plan.md §1.5's inline
anchor working as specified — not the floating-pill fallback — which means the caret anchor is
scoring above `min_confidence` 0.5 against a real editor, and it means the `refresh_caret`
after-each-commit fix holds. Phase 0B still has to measure this properly across the application
matrix; one editor is one cell, not a result. But the mechanism is no longer hypothetical.

**§4.3 — the space between utterances. STILL UNKNOWN, and not the observer's fault.** The
instruction was impossible: see §4 check 3. It needs `mock` and real speech.

**§4.4, §4.5 — not attempted.**

Two things in the log worth carrying forward:

* The finalising chord is being read correctly: the 42-second session ended `hotkey_pressed`, not
  `stop_on_input`. §5.13's fix is holding against a real second press. (The three short sessions
  after it ended `stop_on_input`, which is the click-or-keystroke rule working as designed.)
* `anchor.cpp:1151 empty diff after a visible commit; keeping the previous anchor` fires after
  commits. It is explicitly benign — the log line says so — but source C's commit differencing
  finding nothing is exactly the thing §6's `WDA_EXCLUDEFROMCAPTURE`-vs-`BitBlt` question is about,
  and it is now reproducible on demand. Worth pulling on before Phase 0B rather than during it.
