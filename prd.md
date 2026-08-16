Yes — and the landscape has changed quite a bit. **I would no longer automatically choose Whisper if your goal is to build a private, local replacement for `Win+H`.**

There isn't a trustworthy public benchmark comparing **Windows `Win+H` directly** against the open models because Microsoft doesn't publish the model/WER behind Windows voice typing. So an exact "Windows = 4.2% WER, Qwen = 3.8%" comparison isn't possible. But based on current model benchmarks and architecture, I'd roughly rank them like this for your use case:

| Approach                               | Dictation accuracy | Difficult audio | Real-time feel | Local |
| -------------------------------------- | -----------------: | --------------: | -------------: | ----: |
| **Windows Win+H**                      |              ★★★★½ |            ★★★★ |          ★★★★★ |     ❌ |
| **Qwen3-ASR 1.7B**                     |              ★★★★★ |           ★★★★★ |           ★★★★ |     ✅ |
| **NVIDIA Nemotron ASR Streaming 0.6B** |        ★★★★½–★★★★★ |           ★★★★½ |      **★★★★★** |     ✅ |
| **Whisper large-v3**                   |              ★★★★½ |           ★★★★½ |            ★★★ |     ✅ |
| **Whisper small/medium**               |           ★★★–★★★★ |             ★★★ |           ★★★★ |     ✅ |

### Qwen3-ASR is probably the accuracy winner

This is the model I think you were remembering when we talked about recognition under extremely difficult conditions.

Qwen released **Qwen3-ASR-0.6B and 1.7B in January 2026**. Their technical report says the 1.7B model achieves state-of-the-art results among open-source ASR systems and is competitive with leading proprietary APIs. Importantly, they specifically evaluated real-world difficult acoustic conditions rather than relying only on standard clean-speech benchmarks. ([arXiv][1])

It supports 52 languages/dialects and was designed for things like accents, noisy audio and challenging speech patterns. ([arXiv][2])

So for something like:

> "Change the `llama.cpp` configuration to use Qwen3.6 with speculative MTP decoding on the RTX 5090."

I'd expect **Qwen3-ASR-1.7B to have a better chance of correctly understanding things like `llama.cpp`, Qwen, MTP, model numbers, etc.** than generic Windows dictation, particularly if you can provide vocabulary/context to your dictation layer.

### But there's an even more interesting option for replacing Win+H: Nemotron

NVIDIA released **Nemotron ASR Streaming 0.6B** specifically for very-low-latency live speech recognition. It isn't an offline-file-transcription model awkwardly adapted to streaming; its architecture maintains context while processing incoming chunks continuously. NVIDIA supports chunk sizes down to **80 ms**. ([Hugging Face][3])

That distinction matters enormously for `Win+H`.

With Whisper, you typically get something conceptually like:

> speak → buffer audio → transcribe chunk → revise text → wait → more text

Nemotron is actually designed for:

> speak → **words appear continuously**

NVIDIA explicitly targets voice assistants, live captioning and conversational agents with it, with built-in capitalization and punctuation. ([Hugging Face][3])

And there's unusually strong supporting evidence here. A 2026 research comparison evaluated **50+ ASR configurations**, including Whisper, Qwen3-ASR, Parakeet and Nemotron, specifically for local streaming speech recognition. It identified **Nemotron Speech Streaming as the strongest candidate for real-time English streaming**. ([arXiv][4])

That's almost exactly your use case.

### Where Whisper stands in 2026

Whisper is still excellent. Its original advantage was robustness: OpenAI trained it on 680,000 hours of varied multilingual audio specifically to improve handling of accents, background noise and technical speech. ([OpenAI][5])

But Whisper dates from an earlier generation of ASR architecture.

For **transcribing an existing recording**, Whisper large-v3 remains very good.

For:

> press hotkey → talk → text appears instantly wherever the cursor is

I wouldn't build a new system around Whisper today unless compatibility/ecosystem simplicity were the overriding concern.

Even OpenAI's newer cloud `gpt-4o-transcribe` significantly improves WER over Whisper. ([OpenAI][6]) But that's cloud-based, so it doesn't solve your privacy requirement.

### What I'd use on your machine

Given your RTX 5090, I think there's a really interesting architecture:

**Hotkey → microphone → VAD → Nemotron ASR Streaming → optional text cleanup/context layer → inject keystrokes into focused Windows application**

Nemotron does the immediate recognition so the text appears nearly as quickly as `Win+H`.

Then you could optionally have **Qwen3-ASR-1.7B** do a second-pass correction after a sentence/utterance ends if you want maximum accuracy.

For example, while you're talking:

> can you change the llama dot cpp configuration to use qwen three point six...

Nemotron immediately types it.

At the end of the utterance, Qwen could potentially correct:

> Can you change the `llama.cpp` configuration to use Qwen3.6...

That combination could conceivably be **better than Win+H while remaining completely local**, especially for your technical vocabulary.

And your 5090 is enormous overkill for these models — Nemotron is only **600M parameters** and Qwen3-ASR's larger model is **1.7B**. ([Hugging Face][3])

**So my recommendation for you today would be:**

**1. Nemotron ASR Streaming 0.6B** — try this first as the actual Win+H replacement.
**2. Qwen3-ASR 1.7B** — try this if absolute recognition accuracy/noise handling matters more than minimal latency.
**3. Whisper large-v3** — excellent baseline, but I wouldn't choose it first for a new live-dictation system.

There's an additional piece that makes this considerably more interesting: we can make the whole thing behave essentially **exactly like Win+H** — global shortcut, small floating indicator, auto-stop on silence, and type/paste into *whatever Windows field currently has focus*. On your hardware this could be a very polished local replacement rather than a transcription app.

[1]: https://arxiv.org/abs/2601.21337?utm_source=chatgpt.com "Qwen3-ASR Technical Report"
[2]: https://arxiv.org/html/2601.21337v1?utm_source=chatgpt.com "Qwen3-ASR Technical Report"
[3]: https://huggingface.co/nvidia/nemotron-speech-streaming-en-0.6b?utm_source=chatgpt.com "nvidia/nemotron-speech-streaming-en-0.6b · Hugging Face"
[4]: https://arxiv.org/abs/2604.14493?utm_source=chatgpt.com "Pushing the Limits of On-Device Streaming ASR: A Compact, High-Accuracy English Model for Low-Latency Inference"
[5]: https://openai.com/index/whisper/?utm_source=chatgpt.com "Introducing Whisper"
[6]: https://openai.com/index/introducing-our-next-generation-audio-models/?utm_source=chatgpt.com "Introducing next-generation audio models in the API"
