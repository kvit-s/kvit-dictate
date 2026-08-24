# normalizer — rewriting the pill, never the document

Status: **implemented, off by default, unproven in real dictation.** The module, its guard and
its settings block are in the tree and tested; the first measurement against the real endpoint is
in `bench/results/normalizer/2026-08-24-s1-mini.md` and it is a damage test, not a quality test.
No one has yet dictated a real sentence through it.

Implemented by `src/normalizer.{h,cpp}`, wired in `src/main.cpp`, measured by
`bench/normalize.py`. Depends on `docs/editing.md` §2 (compose-then-commit) and is bounded by
`plan.md` §1.4 (the committed-prefix invariant).

---

## 1. What it is

`superwhisper/s1-mini` is a 596M text model — a fine-tune of `Qwen3-0.6B` — that takes a raw ASR
transcript and rewrites it as written text: fillers dropped, false starts resolved to whatever
the speaker landed on, punctuation and capitalisation applied, spoken numbers and dates and
currency and email addresses in written form. English only, Apache-2.0 with a naming clause.

**It is not a recogniser and it is not a backend.** It runs after ASR, over text, and knows
nothing about audio. `server/kvit_asr/backends/` is for models that turn sound into words; this
turns words into different words, which is a different and more dangerous act.

The endpoint is an HTTP route on another machine:

```
POST /v1/text/normalize
{"text": "...", "styling": "semi-formal", "structure": "prose", "context": "general",
 "response_format": "json"}
→ {"text": "...", "styling": "...", "structure": "...", "context": "..."}
```

`styling` ∈ {casual, semi-formal, formal}, `structure` ∈ {prose, lists}, `context` ∈
{general, email}. An untrained value is refused with 400 rather than sent, because the model
answers an unknown control with invented text. `NormalizerConfig::validate()` checks the same
tables so a bad value is caught when the settings file loads, naming the field, instead of at the
end of the first session.

---

## 2. Why it is on the client and not in the sidecar

The sidecar is the obvious home: it is where models live, where the model→interpreter map lives,
and where nothing in C++ has to learn what a model is (`plan.md` §5.1). It is the wrong home, for
a reason that is not a matter of taste.

**The sidecar does not know what is in the pill.** It knows the `final`s it sent. `docs/editing.md`
§3.2 closes the microphone at the first edit key and lets the hotkey reopen it, so by the time a
session ends the buffer may contain the user's own corrections, deletions and pastes — none of
which crossed the pipe. A normalizer fed the sidecar's transcript would rewrite text the user had
already fixed, and silently discard the fix.

So the request is made from the side that owns the buffer. The consequences are all good:
`docs/protocol.md` is untouched — no new message type (§4.3 makes an unknown `t` a fatal
`unknown_message`, so a new one costs a proto version), no new field, no fixtures, no second
implementation to keep in agreement. It works with every ASR backend, including the ones that do
not exist yet. And the sidecar never learns it exists.

---

## 3. Why it may only run under `Cadence::EndOfSession`

A rewriter's output is **not prefix-stable**, and the whole program is built on text only ever
growing. Watch a correction cross an utterance boundary:

```
raw  "...send the report by friday"                    →  "...send the report by Friday."
raw  "...send the report by friday no wait thursday"   →  "...send the report by Thursday."
```

`Friday.` was in the first answer and is gone from the second. Under `Cadence::Immediate` those
bytes are already in the user's document, and taking them back means sending backspaces into an
arbitrary application — the single thing `plan.md` §1.4 exists to refuse.

Under `Cadence::EndOfSession` nothing has been committed. The retraction happens in the pill,
where the user can see it, and §1.4 is untouched. `Settings::normalise()` forces `enabled` off
under any other cadence, exactly as it already does for `editing.enabled`, and
`NormalizerConfig::validate()` rejects the combination by name for a `Settings` built
programmatically.

---

## 4. When it fires, and why not at commit

The rewrite runs **at each utterance boundary**, from the `EndOfSession` branch of
`App::perform_commit`.

The alternative — running it when the user presses Enter — forces a bad trade. Either a second
Enter to review the rewrite, or a separate key the user must remember, or the rewrite goes
unread into the document. Firing at the utterance boundary avoids all three:

* The endpointer has just fired on silence, so the recogniser is idle. On a deployment where the
  two models share a lock, they are not competing.
* The user has stopped talking, so the 0.2–1.4 s lands in a pause that already existed.
* The cleaned text is in the pill by the time Enter is pressed. `docs/editing.md` §2's "text the
  user has read and approved" holds with **no extra gesture**.

The visible cost is that the pill churns: words already read can change as later speech arrives.
That is the model changing its mind in the pill instead of in the document, which is what
compose-then-commit is for — but it is a real UX cost and `bench/metrics.py`'s `flicker()` is how
to measure it rather than argue about it.

### Always renormalise from raw

`App::normalize_raw_` grows alongside `edit_` and is what every request sends. A rewriter is not
idempotent: feed it its own output and it keeps editing, toward shorter and more confident, which
is the direction that loses words. Sending the whole raw buffer every time also resolves a
correction the VAD happened to split in two, because the model always sees the entire session.

### `stopped` is held for an in-flight rewrite

The last utterance's `final` arrives moments before `stopped`, so a rewrite fired from it is
almost always still in flight when the session ends. Without a wait, the last utterance of every
session would be the one that never got normalized. The target is already held for exactly this
kind of wait and `finalize_deadline_ms_` still bounds it — `NormalizerConfig::validate()` caps
the timeout well inside `kFinalizeGraceMs` — so this can defer a commit but never prevent one.

---

## 5. The guard

`plan.md` §4 is a list of the strings `Win+H` gets wrong and this program exists to get right.
A 0.6B model whose card claims 94.8% *token* accuracy is precisely what damages them.

So a rewrite is not accepted on trust. `guard_verdict()` compares raw against normalized and
**rejects the whole rewrite** if any protected term present in the raw text is absent from the
rewrite. Whole, not partial: a rewrite is one act of judgement by the model, and splicing half of
it back would produce text neither the model nor the user composed. On rejection the raw text
stands, the pill does not change, and the log names the term that died.

Three verdicts reject:

| | |
|---|---|
| `RejectTermLost` | a protected term the user actually said is gone |
| `RejectEmpty` | the answer is empty or whitespace — "the pill went blank" |
| `RejectTooShort` | the rewrite kept under half the raw text's length |

The length floor exists because a term list can only protect the terms on it. A model that
answers a paragraph with one sentence has failed in a way no vocabulary check catches, because
the terms it kept may all be in the sentence it did return.

Protected terms are `builtin_protected_terms()` — kept in step with `bench/metrics.py`'s `TERMS`
table, so what the bench measures and what the app enforces cannot drift — plus the user's own
`context.terms`, derived in `Settings::normalise()`. One list to edit, two things protected: the
recogniser's vocabulary and the rewriter's floor.

Matching is case-insensitive and substring-based. `sm_120` inside `--arch=sm_120` counts as
surviving; a token match would need this module to hold an opinion about word boundaries in
`--n-gpu-layers` and `C:\Users\sk\AppData` that the model does not share, and every disagreement
would be a false rejection of a good rewrite.

---

## 6. What the first measurement found

`bench/normalize.py`, 83 prompts, 2026-08-24. Full note in `bench/results/normalizer/`.

Two genuine damages, both caught by the guard:

```
sm_120     →  SM120        bench/metrics.py:675 predicted "sm 120" for this term
                           before the model existed. Same failure, a spelling the
                           table had not thought of.
llama.cpp  →  LLMCpp       not a mis-rendering but a rename of a real identifier
                           into one that does not exist.
```

`LLMCpp` is the one to keep in mind. A recogniser cannot fail that way — it mishears sounds. A
rewriter can, because it is a language model with an opinion about what you probably meant.

Both came out of **bf16 weights**, the unquantised model. The card's 94.8% token accuracy was
measured on `Q4_K_M`, which is lossier, so quantisation is not available as an explanation and a
better build is not available as a fix: this is the better build.

The guard's rejection rate was 6.0%, and **not all of it is damage.** `hard-003` lost `ONNX`
because the speaker abandoned the clause containing it, which is the model resolving a false
start exactly as designed. The guard cannot tell that from a loss, so it throws away a correct
rewrite. Every protected term appearing only inside an abandoned clause will do this.

---

## 7. Failure is never fatal

Every failure path — DNS, connect, timeout, non-200, malformed JSON, a superseded request, a
guard rejection, the user starting to edit — leaves the raw text exactly as it was and logs why.
The user's own machine hears them regardless of whether some other machine is up. This mirrors
the endpoint's own `/health` reporting `"normalizer": false` when the weights are absent: a
missing normalizer cannot stop the house hearing.

---

## 8. Privacy

`plan.md` §1.9 promises nothing is sent. Enabling this sends **every dictated word** to another
machine, so:

* It is off by default, and a settings file with no `normalizer` block leaves it off — nothing
  written before this feature existed can start sending dictation anywhere.
* Turning it on is an explicit act by the user, in the settings file or the dialog.
* The URL is whatever the user names. If that name is ever pointed at the internet, the text of
  everything dictated goes with it. That is a settings decision, made once, and deliberately not
  something the module second-guesses per request.
* This is the only outbound network call in the program. `winhttp` appears in `CMakeLists.txt`
  for this and nothing else.

---

## 9. Not yet answered

* **Whether it is any good.** §6 is a damage test. Measuring quality needs paired data this
  repository does not have — the raw ASR transcript on one side and the intended written text on
  the other. `bench/prompts/hard.jsonl`'s references are verbatim-as-spoken by construction
  ("Reference is verbatim as spoken, including the correction"), so a *correct* normalisation
  scores as pure error against them. Recording that corpus is Phase 1 work.
* **Whether the endpoint's `context` control fixes the damage.** If passing the user's terms
  prevents the `llama.cpp` and `sm_120` rewrites, the term list stops being only a veto and
  becomes a fix. Nothing currently sends it.
* **Whether 0.5 is the right length floor.** Three of five rejections in §6 were `RejectTooShort`,
  and `hard-021` ("sixty five thousand five hundred and thirty six" → "65,536") is correct ITN
  rejected purely for shortening the text.
* **Whether the pill's churn is tolerable.** Unobserved: nobody has dictated through this yet.
