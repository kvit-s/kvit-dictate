# docs/

Specifications and published matrices for kvit-dictate. The authoritative design document is
`../plan.md`; everything here is subordinate to it and cites the section it implements —
**except `protocol.md`**, which is now a public API specification, is licensed Apache-2.0 rather
than MPL-2.0, and is written to be sufficient without any other file in this repository.

| File | What it is | Status |
|---|---|---|
| `protocol.md` | **The public wire specification.** Two transport bindings — WebSocket (shipped) and a named pipe (retained for the bench) — over one framing layer: message types, identity fields (`session_id` / `utterance_id` / `seq`), UTF-8 byte-offset semantics, backpressure and error handling. Implements `plan.md` §1.2 and `release-plan.md` §1. | **Written and implemented on both sides.** Licensed Apache-2.0 (`../LICENSE-APACHE`). §15 is a worked example of a conforming server. |
| `examples/minimal_server.py` | A complete conforming server in the standard library and nothing else: WebSocket upgrade, framing, handshake, one session, an audio counter where a recogniser would go. `protocol.md` §15 walks through it. | **Executed, not asserted.** `../server/tests/test_minimal_server.py` drives it through a full session and every fault path on every test run. |
| `setup.md` | How to stand up an ASR endpoint for the app to talk to: which existing servers can sit behind the protocol, a worked example, and the honest statement that this is the part the project does not do for you yet. | Written for the v1 release. |
| `cla.md` | The contributor agreement `../CONTRIBUTING.md` refers to, and §4's plain statement of why it asks for more than the outbound licence does. | Draft; `release-plan.md` §5 wants a lawyer's eye on it before the first binary ships. |
| `editing.md` | The pill as an editable compose buffer: nothing reaches the document until the user presses Enter. Two disjoint session states — dictating with the microphone open, editing with it closed — plus keyboard capture, the settled/unstable split the caret may occupy, mouse hit-testing against a non-focusable window, and the fail-open rule that keeps a wedged UI thread from eating the keyboard. Amends `plan.md` §1.8 and §1.6; strengthens §1.4. | **Proposal, not implemented.** Nothing in `src/` does any of it. §15 holds the questions that must be measured rather than guessed — the resume latency is the one that could still change the design. |
| `supported-targets.md` | Per-application support matrix: which applications kvit-dictate types into, which get the degraded floating-pill experience, and which are refused with a reason. Also the Phase 0B methodology that produces it. Implements `plan.md` §0B/§1.7/§4. | **v1 template, no measurements.** Cells read `—` until the Phase 0B probe fills them in. Do not cite it as evidence of support yet. |

## Documents that live elsewhere, and why

- **`../bench/CANDIDATES.md`** — the ASR model candidate list from Phase 0A: which of the PRD's
  cited models actually resolve, their licence, parameter count, runtime dependency, streaming
  mode and whether a published ONNX export exists. It lives under `bench/` rather than `docs/`
  because it is **data for the experiment harness**, not a specification — `bench/run_matrix.py`
  reads from the same candidate set, and `plan.md` §0 is explicit that the candidate list is data,
  not code.
- **`../plan.md`** — the implementation plan, revision 2. Authoritative.
- **`../prd.md`** — the original product recommendation. Background only; `plan.md` §0 records why
  its specific model citations are treated as unverified until Phase 0A resolves them.
- **`../plan-review.md`** — the review that produced revision 2 (see `plan.md` §9).

## Conventions for anything added here

- Every document states which `plan.md` section it implements, and says so in its first screen.
- Uncertainty about a Windows API is written down as an open question in the file, not asserted.
  `supported-targets.md` §9 is the model for this.
- Measured documents carry a status banner distinguishing *template* from *results*, and a change
  log. A reader must never have to guess whether a number was measured.
