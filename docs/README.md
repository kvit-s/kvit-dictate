# docs/

Specifications and published matrices for kvit-dictate. The authoritative design document is
`../plan.md`; everything here is subordinate to it and cites the section it implements.

| File | What it is | Status |
|---|---|---|
| `protocol.md` | Versioned wire spec for the C++ ↔ Python named-pipe transport: framing, message types, identity fields (`session_id` / `utterance_id` / `seq`), UTF-8 byte-offset semantics, backpressure and error handling. Implements `plan.md` §1.2. | **Not yet written.** `plan.md` §1.2 requires it to exist *before either endpoint is implemented*; it freezes in Phase 2 along with the cross-language fixtures. |
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
