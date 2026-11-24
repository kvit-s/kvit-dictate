# Times NVIDIA Nemotron Speech Streaming on recorded sessions, on the CPU, with onnxruntime-genai
# alone: no Foundry Local runtime, no server, no telemetry (disable_telemetry_events below).
#
# The model folder is the one Copilot's dictation downloads: Microsoft's Foundry Local catalog
# entry nemotron-speech-streaming-en-0.6b-generic-cpu:3 (ONNX, int4, 696 MB). How it was fetched,
# and what was measured with this script, is in docs/copilot-speech-model.md.
#
#   python -m venv venv && venv\Scripts\pip install onnxruntime-genai==0.20.0
#   venv\Scripts\python -I time_nemotron.py [--throttled] MODEL_DIR H:\kvit-corpus\<session>\audio.wav ...
#
# Each WAV must be 16 kHz mono s16 (the corpus's audio.wav is). The audio is fed in 560 ms chunks,
# the chunk size the int4 export was built for (genai_config.json chunk_samples = 8960), as fast
# as the model takes them. A chunk's compute time is what decides whether live dictation keeps up:
# it has to stay under 560 ms, and the last one is most of the wait after the speaker stops.
# Transcripts are printed, not saved: they are corpus text (plan.md §1.9).
import sys
import time
import wave

import numpy as np
import onnxruntime_genai as og

CHUNK = 8960  # samples, 560 ms at 16 kHz


def opt_out_of_power_throttling():
    # Windows throttles a window-less background process (EcoQoS): on a hybrid Intel CPU its
    # threads move to the efficiency cores, and this model's chunks went from ~75 ms to ~280 ms
    # after a couple of seconds of work. PROCESS_POWER_THROTTLING_EXECUTION_SPEED, controlled
    # and off, keeps it at full speed. Pass --throttled to measure what Windows does by default.
    import ctypes
    from ctypes import wintypes

    class State(ctypes.Structure):
        _fields_ = [("Version", wintypes.ULONG), ("ControlMask", wintypes.ULONG),
                    ("StateMask", wintypes.ULONG)]

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.GetCurrentProcess.restype = wintypes.HANDLE
    kernel32.SetProcessInformation.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                               wintypes.DWORD]
    state = State(1, 0x1, 0x0)
    ok = kernel32.SetProcessInformation(kernel32.GetCurrentProcess(), 4,  # ProcessPowerThrottling
                                        ctypes.byref(state), ctypes.sizeof(state))
    print("power throttling:", "off" if ok else f"unchanged (error {ctypes.get_last_error()})")


args = sys.argv[1:]
if "--throttled" in args:
    args.remove("--throttled")
    print("power throttling: left as Windows sets it")
else:
    opt_out_of_power_throttling()
og.disable_telemetry_events()
model_dir, wavs = args[0], args[1:]
t = time.perf_counter()
model = og.Model(model_dir)
tokenizer = og.Tokenizer(model)
print(f"model load {1000 * (time.perf_counter() - t):.0f} ms, type {model.type}")

for path in wavs:
    with wave.open(path) as w:
        if (w.getframerate(), w.getnchannels(), w.getsampwidth()) != (16000, 1, 2):
            sys.exit(f"{path}: need 16 kHz mono s16")
        pcm = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
    audio = pcm.astype(np.float32) / 32768.0

    features = model.create_streaming_processor()  # PCM -> mel features, keeps its own cache
    decoder = og.Generator(model, og.GeneratorParams(model))  # RNN-T decode, keeps encoder state
    stream = tokenizer.create_stream()
    text, per_chunk = [], []
    t0 = time.perf_counter()
    for i in range(0, len(audio), CHUNK):
        chunk = audio[i:i + CHUNK]
        if len(chunk) < CHUNK:
            chunk = np.pad(chunk, (0, CHUNK - len(chunk)))
        tc = time.perf_counter()
        inputs = features.process(chunk)
        if inputs is not None:
            decoder.set_inputs(inputs)
            while not decoder.is_done():
                decoder.generate_next_token()
                for token in decoder.get_next_tokens():
                    text.append(stream.decode(int(token)))
        per_chunk.append(1000 * (time.perf_counter() - tc))

    total = 1000 * (time.perf_counter() - t0)
    audio_ms = 1000 * len(audio) / 16000
    per_chunk.sort()
    print(f"== {path}\n   audio {audio_ms / 1000:.1f} s, compute {total:.0f} ms "
          f"({total / audio_ms:.2f} x real time), per 560 ms chunk: "
          f"median {per_chunk[len(per_chunk) // 2]:.0f} ms, max {per_chunk[-1]:.0f} ms")
    print("  ", "".join(text).strip())
