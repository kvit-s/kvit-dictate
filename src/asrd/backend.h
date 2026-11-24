// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// The seam between the session machine and whatever turns PCM into text.
//
// `SessionServer` used to hold an `OpenAiBackend&`, because there was exactly one backend and it
// was compiled in. There are two:
//
//   backend_openai.cpp    an OpenAI-compatible `/v1/audio/transcriptions` service, over HTTP
//   backend_nemotron.cpp  NVIDIA Nemotron on this machine's CPU, streaming
//
// The HTTP one is a batch model behind a streaming interface, driven by `PseudoStreamingDriver`
// (backend_openai.h), and the contract below is the one `OpenAiBackend` already had, named.
// Nemotron streams for real and keeps the same contract: it decodes on its own thread as audio
// is fed, and `transcribe` reads what it has. The contract is deliberately not wider: the session
// machine must not learn which backend it is talking to, because a branch on that inside
// `session_server.cpp` is a branch whose other arm is tested by nobody.
//
// Threading: `feed` runs on the connection thread, `transcribe` on the decode thread, `cancel`
// from either. Each implementation owes that.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "util/result.h"

namespace kd::asrd {

// A cut the model proposed for itself. `committed_text` is the transcript of exactly the first
// `cut_samples` samples — not an estimate of it, not a prefix matched against it — so a caller
// may emit that text as final and keep the rest of the audio with no overlap between the two.
struct ProposedCut {
    size_t cut_samples = 0;
    std::string committed_text;
};

struct Transcription {
    // The cumulative hypothesis for everything currently buffered.
    std::string text;
    bool have_text = false;
    // Set only when the backend can say *where in the audio* a settled prefix ends, and never on
    // a finalize. A backend with no timestamps never sets it, and the utterance is then bounded
    // by the endpointer alone.
    bool have_cut = false;
    ProposedCut cut;
};

class Backend {
public:
    virtual ~Backend() = default;

    // Earn `ready`. Fails with a sentence the user can act on, which the connection sends as a
    // fatal `error` instead of `ready` — so a green Test button means this succeeded.
    virtual Status load() = 0;

    // Start a new utterance: the buffer and the audio clock go back to zero.
    virtual void reset() = 0;

    // The session's vocabulary hints (docs/protocol.md §6.1). A backend that cannot use them
    // keeps them and ignores them, and says so in `ready.context_supported`.
    virtual void set_context(const std::vector<std::string>& terms) = 0;
    [[nodiscard]] virtual const std::string& context_prompt() const noexcept = 0;

    // Append gated PCM. Cheap. Returns true when a preview hop is due.
    virtual bool feed(const int16_t* samples, size_t count) = 0;

    // Transcribe the buffer. `force` finalises. Blocking; decode thread only.
    virtual Status transcribe(bool force, Transcription& out) = 0;

    // Abort whatever is in flight, from any thread, so a `cancel` does not sit out a decode.
    virtual void cancel() = 0;
    virtual void reset_cancel() = 0;

    // Drop the committed head after a `ProposedCut` has been acted on.
    virtual void retain_tail(size_t cut_samples) = 0;

    // Keep a copy of the audio each `transcribe` decoded, for the revision capture.
    virtual void set_capture_window(bool on) = 0;
    [[nodiscard]] virtual const std::vector<int16_t>& last_window() const noexcept = 0;
};

}  // namespace kd::asrd
