// kvit-speech - a long-lived transcription helper for the Windows AI Speech API.
//
// This program exists for one reason: Microsoft.Windows.AI.Speech is a WinRT API in the Windows
// App SDK, and the kvit-dictate sidecar is stdlib-only CPython. Rather than teach Python to
// activate WinAppSDK types, the sidecar spawns this helper and talks to it over a tiny framed
// protocol. Everything else - VAD, utterance boundaries, stability, cumulative text, the wire
// protocol in docs/protocol.md - stays where it already is and is not duplicated here.
//
// ─── Why it must be long-lived ──────────────────────────────────────────────────────────────
//
// Measured on this machine (i7-14700K, CPU execution provider, no NPU):
//
//     SpeechRecognitionModel.TryCreateAsync()   2851 ms cold, 919 ms warm
//     BatchRecognition.Recognize(pcm)           ~1350 ms fixed + ~0.30x real time
//
// A helper spawned per utterance would pay the load cost every time and be unusable. So the model
// is created once at startup and reused for the life of the process.
//
// ─── The protocol ───────────────────────────────────────────────────────────────────────────
//
// Deliberately NOT the protocol in docs/protocol.md. That one is frozen, is between the C++
// client and the Python sidecar, and carries sessions, sequence numbers and stability. This link
// carries one question - "what do these bytes say?" - and must not grow a second implementation
// of any of that.
//
// Frames are little-endian, length-prefixed, and identical in shape in both directions:
//
//     [u32 length][u8 tag][payload...]        length counts the tag byte and the payload
//
// Requests (sidecar -> helper):
//     'T'  transcribe        payload = raw PCM, 16 kHz mono signed 16-bit LE (docs/protocol.md 6.3)
//     'P'  ping              payload = empty
//     'Q'  quit              payload = empty
//
// Responses (helper -> sidecar):
//     'O'  ok                payload = UTF-8 text (possibly empty - silence is a valid answer)
//     'E'  error             payload = UTF-8 message; the link stays usable
//     'R'  ready             payload = UTF-8 JSON, sent unsolicited exactly once at startup
//
// A response is sent for every request, in order. There is no pipelining: the sidecar's Backend
// contract is synchronous, so making this link asynchronous would buy nothing.
//
// ─── Why stdout is hijacked ─────────────────────────────────────────────────────────────────
//
// The speech implementation DLLs write to stdout. Loading the quantized model prints a run of
// "[loader] INT8 zp size mismatch: got 0 expected 768" lines - harmless, but on the same stream
// the protocol would be using. That is precisely the hazard docs/protocol.md 2.1 cites when it
// refuses stdio for the main protocol ("one stray print would corrupt a binary frame stream"),
// and it is not hypothetical here: it is observed on every single startup.
//
// So before any speech DLL is loaded, the real stdout handle is captured and STD_OUTPUT_HANDLE is
// re-pointed at NUL. Native code that resolves stdout afterwards - which is all of it, since the
// DLLs are loaded later - writes into the void, while the captured handle carries the frames.
// Diagnostics go to stderr, which is left alone for the sidecar to capture and log.

using System.Buffers.Binary;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;
using Microsoft.Windows.AI;
using Microsoft.Windows.AI.Speech;

namespace KvitSpeech;

internal static class Program
{
    // 32 MiB is ~17 minutes of 16 kHz mono audio. No utterance is that long; the cap is here so a
    // desynchronised stream fails immediately with a legible message instead of trying to allocate
    // whatever four arbitrary bytes happened to say.
    private const int MaxFrameBytes = 32 * 1024 * 1024;

    private const int BytesPerMs = 32;   // 16000 Hz * 2 bytes / 1000

    private static async Task<int> Main(string[] args)
    {
        bool ensureModel = args.Contains("--ensure-model");
        // A test switch: take the model's non-VNNI routine even on a processor that does not
        // need it, to show that routine transcribes. See VnniWorkaround.cs.
        bool vnniOff = args.Contains("--vnni-off");

        // Must happen before the first speech DLL is loaded. See the header.
        Stream protocolOut = StdIo.CaptureStdoutAndSilenceIt();
        Stream protocolIn = Console.OpenStandardInput();

        string? nativeFolder = SpeechRuntime.Prepare();
        Log($"native search path: {nativeFolder ?? "(no speech component package found)"}");

        SpeechRecognitionModel model;
        try
        {
            model = await CreateModelAsync(ensureModel);
        }
        catch (Exception ex)
        {
            // The sidecar turns this into a BackendLoadError naming the reason, which is the
            // difference between "this backend needs the optional model" and a bare traceback.
            WriteFrame(protocolOut, (byte)'E', Encoding.UTF8.GetBytes(ex.Message));
            Log("startup failed: " + ex);
            return 1;
        }

        // After the model exists and before the first decode, which is where the fault was.
        // In the ready answer as well as on stderr, because the ready answer is what the sidecar
        // always logs.
        string vnni = VnniWorkaround.Apply(nativeFolder, vnniOff);
        Log("vnni: " + vnni);

        using (model)
        {
            WriteFrame(protocolOut, (byte)'R', Encoding.UTF8.GetBytes(ReadyJson(nativeFolder, vnni)));
            return await ServeAsync(protocolIn, protocolOut, model);
        }
    }

    // ─── Model ──────────────────────────────────────────────────────────────────────────────

    private static async Task<SpeechRecognitionModel> CreateModelAsync(bool ensureModel)
    {
        AIFeatureReadyState state = SpeechRecognitionModel.GetReadyState();
        Log($"GetReadyState() = {state}");

        if (state == AIFeatureReadyState.NotSupportedOnCurrentSystem)
        {
            throw new InvalidOperationException(
                "the Windows AI speech model is not supported on this system; it needs Windows 11 " +
                "24H2 (build 26100) or later, and either a Copilot+ NPU or a CPU the Windows AI " +
                "APIs support");
        }

        if (state != AIFeatureReadyState.Ready)
        {
            // plan.md 1.9 forbids downloading a model at runtime, and this one is a several
            // hundred megabyte background download through Windows Update. So it is opt-in, and
            // the refusal says exactly how to opt in.
            if (!ensureModel)
            {
                throw new InvalidOperationException(
                    $"the Windows AI speech model is not installed (state: {state}). It is an " +
                    "optional component and downloading it is not something a dictation session " +
                    "should do on its own. Run kvit-speech.exe --ensure-model once to install it, " +
                    "or install it from Windows Settings; it can be removed again at Settings > " +
                    "System > AI Components.");
            }

            Log("EnsureReadyAsync(): downloading the speech model through Windows Update...");
            var op = SpeechRecognitionModel.EnsureReadyAsync();
            // Progress.Progress is already a percentage; it is NOT a 0..1 fraction.
            op.Progress = (_, p) => Log($"  {p.Status} {p.Progress:F0}%");
            var result = await op;
            if (result.Status != AIFeatureReadyResultState.Success)
            {
                throw new InvalidOperationException(
                    $"EnsureReadyAsync failed: {result.Status} ({result.ExtendedError?.Message})");
            }
        }

        var created = await SpeechRecognitionModel.TryCreateAsync();
        if (created.SpeechModel is null)
        {
            string extra = created.ExtendedError?.HResult == unchecked((int)0x8007007E)
                ? " (0x8007007E ERROR_MOD_NOT_FOUND - the speech component's winml folder is not " +
                  "on the DLL search path; see SpeechRuntime.cs)"
                : "";
            throw new InvalidOperationException(
                $"SpeechRecognitionModel.TryCreateAsync failed: {created.ExtendedError?.Message}{extra}");
        }
        return created.SpeechModel;
    }

    private static string ReadyJson(string? nativeFolder, string vnni)
    {
        // Hand-built rather than serialized: four fields, and a JSON dependency for four fields
        // is not worth the reference. Both strings go through JsonString.
        return "{\"model\":\"windows-ai-speech\",\"proto\":1,\"native_folder\":" +
               JsonString(nativeFolder ?? "") + ",\"vnni\":" + JsonString(vnni) + "}";
    }

    private static string JsonString(string s)
    {
        var sb = new StringBuilder("\"");
        foreach (char c in s)
        {
            if (c == '"' || c == '\\') sb.Append('\\').Append(c);
            else if (c < ' ') sb.Append("\\u").Append(((int)c).ToString("x4"));
            else sb.Append(c);
        }
        return sb.Append('"').ToString();
    }

    // ─── Serving ────────────────────────────────────────────────────────────────────────────

    private static async Task<int> ServeAsync(Stream input, Stream output, SpeechRecognitionModel model)
    {
        while (true)
        {
            (byte tag, byte[] payload)? frame;
            try
            {
                frame = ReadFrame(input);
            }
            catch (Exception ex)
            {
                Log("read failed: " + ex.Message);
                return 1;
            }

            if (frame is null)
            {
                // Clean EOF. The sidecar closed the pipe, or died and the OS closed it for it;
                // either way there is nothing left to answer to.
                Log("stdin closed; exiting");
                return 0;
            }

            (byte tag, byte[] payload) = frame.Value;
            switch (tag)
            {
                case (byte)'Q':
                    Log("quit requested");
                    return 0;

                case (byte)'P':
                    WriteFrame(output, (byte)'O', Array.Empty<byte>());
                    break;

                case (byte)'T':
                    await TranscribeAsync(output, model, payload);
                    break;

                default:
                    WriteFrame(output, (byte)'E',
                        Encoding.UTF8.GetBytes($"unknown request tag 0x{tag:X2}"));
                    break;
            }
        }
    }

    private static async Task TranscribeAsync(Stream output, SpeechRecognitionModel model, byte[] pcm)
    {
        if (pcm.Length % 2 != 0)
        {
            WriteFrame(output, (byte)'E',
                Encoding.UTF8.GetBytes($"odd-length PCM payload ({pcm.Length} bytes): a sample was split"));
            return;
        }
        if (pcm.Length == 0)
        {
            WriteFrame(output, (byte)'O', Array.Empty<byte>());
            return;
        }

        try
        {
            // A fresh BatchRecognition per call. Measured: reusing one saves nothing (2207 ms vs
            // 2269 ms for the same 3 s window), and a fresh one cannot carry state between
            // utterances, which is the property that matters.
            using var recognition = new BatchRecognition(model);
            long t0 = Environment.TickCount64;
            // Raw PCM is accepted directly - no WAV container needed. Verified against the same
            // audio wrapped and unwrapped: byte-identical transcripts. The API assumes the format
            // docs/protocol.md 6.3 already fixes, so there is nothing to negotiate.
            string text = await recognition.Recognize(pcm);
            long ms = Environment.TickCount64 - t0;
            Log($"decoded {pcm.Length / BytesPerMs} ms of audio in {ms} ms -> {text.Length} chars");

            // The model returns a leading space on almost every result. Trimming here rather than
            // in the sidecar keeps the byte offsets the sidecar computes over its own text, which
            // is the text that reaches the wire.
            WriteFrame(output, (byte)'O', Encoding.UTF8.GetBytes(text.Trim()));
        }
        catch (Exception ex)
        {
            // One failed window must not end the session: the sidecar turns 'E' into a decode
            // error for this utterance and carries on.
            Log("decode failed: " + ex);
            WriteFrame(output, (byte)'E',
                Encoding.UTF8.GetBytes($"{ex.GetType().Name} 0x{ex.HResult:X8}: {ex.Message}"));
        }
    }

    // ─── Framing ────────────────────────────────────────────────────────────────────────────

    private static (byte tag, byte[] payload)? ReadFrame(Stream input)
    {
        byte[] header = new byte[4];
        if (!ReadExactly(input, header, allowEof: true)) return null;

        uint length = BinaryPrimitives.ReadUInt32LittleEndian(header);
        if (length < 1 || length > MaxFrameBytes)
        {
            throw new InvalidDataException(
                $"frame length {length} is outside 1..{MaxFrameBytes}; the stream is desynchronised");
        }

        byte[] body = new byte[length];
        if (!ReadExactly(input, body, allowEof: false))
            throw new EndOfStreamException($"stream ended {length} bytes into a frame body");

        return (body[0], body[1..]);
    }

    private static bool ReadExactly(Stream input, byte[] buffer, bool allowEof)
    {
        int got = 0;
        while (got < buffer.Length)
        {
            int n = input.Read(buffer, got, buffer.Length - got);
            if (n == 0)
            {
                if (got == 0 && allowEof) return false;
                throw new EndOfStreamException($"stream ended after {got} of {buffer.Length} bytes");
            }
            got += n;
        }
        return true;
    }

    private static void WriteFrame(Stream output, byte tag, byte[] payload)
    {
        byte[] header = new byte[4];
        BinaryPrimitives.WriteUInt32LittleEndian(header, (uint)(payload.Length + 1));
        output.Write(header);
        output.WriteByte(tag);
        output.Write(payload);
        // Flushed on every frame. The sidecar is blocked on the answer, so a buffered reply is a
        // deadlock rather than an optimisation.
        output.Flush();
    }

    private static void Log(string message) => Console.Error.WriteLine("[kvit-speech] " + message);
}

/// <summary>Captures the real stdout and points the process's STD_OUTPUT_HANDLE at NUL.</summary>
internal static class StdIo
{
    private const int STD_OUTPUT_HANDLE = -11;

    [DllImport("kernel32", SetLastError = true)]
    private static extern IntPtr GetStdHandle(int nStdHandle);

    [DllImport("kernel32", SetLastError = true)]
    private static extern bool SetStdHandle(int nStdHandle, IntPtr hHandle);

    [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(
        string lpFileName, uint dwDesiredAccess, uint dwShareMode, IntPtr lpSecurityAttributes,
        uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    public static Stream CaptureStdoutAndSilenceIt()
    {
        IntPtr real = GetStdHandle(STD_OUTPUT_HANDLE);
        var stream = new FileStream(new SafeFileHandle(real, ownsHandle: false), FileAccess.Write);

        // GENERIC_WRITE, FILE_SHARE_READ|WRITE, OPEN_EXISTING.
        var nul = CreateFileW("NUL", 0x40000000, 0x00000003, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (!nul.IsInvalid) SetStdHandle(STD_OUTPUT_HANDLE, nul.DangerousGetHandle());

        // And stop this program's own Console.Write from reaching the frame stream, which would
        // corrupt it exactly the way the native chatter would have.
        Console.SetOut(TextWriter.Null);
        return stream;
    }
}
