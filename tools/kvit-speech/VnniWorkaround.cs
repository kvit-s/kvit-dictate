// Keeping Microsoft's speech model off an instruction this processor does not have.
//
// ─── The fault ──────────────────────────────────────────────────────────────────────────────
//
// Reported 2026-11-20 from an AMD Ryzen Threadripper PRO 7945WX (Zen 4): every decode killed this
// helper with 0xC000001D, STATUS_ILLEGAL_INSTRUCTION. Windows' crash record named the module:
// asrmodelapi.dll 5.2.1.261053, in Microsoft.Windows.AI.Speech.Preview.CPU 1.2607.112.0, at
// offset 0x14D82F. The same file on an i7-14700K, where it works, has at that offset
//
//     C4 C2 7D 50 14 11    vpdpbusd ymm2, ymm0, [r9+rdx]      ; VEX-encoded: AVX-VNNI
//
// — the only VNNI instruction in the whole DLL. It runs when a one-byte flag is set, and the
// flag is set by a static initializer that reads
//
//     CPUID.(7,1):EAX[4]   AVX-VNNI        (the VEX form, which is what the code uses)
//     CPUID.(7,0):ECX[11]  AVX512_VNNI     (the EVEX form, which it never uses)
//
// and sets the flag if **either** is present. A processor with only the AVX-512 form — Zen 4,
// Intel's Ice Lake, Tiger Lake, Rocket Lake and Cascade Lake — passes the check and faults on
// the instruction. That is Microsoft's bug, and not one this program can fix where it lives.
//
// ─── The workaround ─────────────────────────────────────────────────────────────────────────
//
// Clear the flag in this process, so the model takes the routine it uses on a processor with
// no VNNI at all. Nothing on disk is touched and nothing outside this process is affected.
//
// The flag is found, not hard-coded: the initializer's own instructions are matched — both
// CPUID reads, both bit tests, and both stores, which must name the same byte — and the address
// is read out of them. A different build of the DLL either matches exactly that code, in which
// case the byte it stores to is by construction the flag in question, or it does not match and
// nothing is written. A wrong write here would be worse than the crash, because it would be
// silent; a crash at least says what happened.
//
// The DLL is loaded here, by the full path of the folder SpeechRuntime.Prepare put on the search
// path, so the module patched is the module the speech runtime uses, and the reference taken
// keeps it — and the cleared flag — loaded for the life of the process.

using System.Runtime.InteropServices;
using System.Runtime.Intrinsics.X86;

namespace KvitSpeech;

internal static class VnniWorkaround
{
    [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr LoadLibraryExW(string fileName, IntPtr file, uint flags);

    [DllImport("kernel32", SetLastError = true)]
    private static extern bool VirtualProtect(IntPtr address, UIntPtr size, uint newProtect,
                                              out uint oldProtect);

    private const uint LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR = 0x00000100;
    private const uint LOAD_LIBRARY_SEARCH_DEFAULT_DIRS = 0x00001000;
    private const uint PAGE_READWRITE = 0x04;
    private const uint IMAGE_SCN_MEM_EXECUTE = 0x20000000;
    private const uint IMAGE_SCN_MEM_WRITE = 0x80000000;

    private const string ModuleName = "asrmodelapi.dll";

    // mov eax,7 · mov ecx,1 · cpuid · test al,10h · je <fallback>
    private static readonly byte[] CheckHead =
        { 0xB8, 0x07, 0x00, 0x00, 0x00, 0xB9, 0x01, 0x00, 0x00, 0x00, 0x0F, 0xA2, 0xA8, 0x10, 0x74 };

    // The fallback: xor ecx,ecx · mov eax,7 · cpuid · shr ecx,0Bh · and cl,1 · mov [rip+x],cl
    private static readonly byte[] Fallback =
    {
        0x33, 0xC9, 0xB8, 0x07, 0x00, 0x00, 0x00, 0x0F, 0xA2, 0xC1, 0xE9, 0x0B, 0x80, 0xE1, 0x01,
        0x88, 0x0D,
    };

    /// <summary>What the processor says about the two forms of VNNI.</summary>
    public readonly record struct Vnni(bool Vex, bool Evex)
    {
        public override string ToString() =>
            $"AVX-VNNI {(Vex ? "yes" : "no")}, AVX512-VNNI {(Evex ? "yes" : "no")}";
    }

    public static Vnni Detect()
    {
        if (!X86Base.IsSupported) return new Vnni(false, false);
        if (X86Base.CpuId(0, 0).Eax < 7) return new Vnni(false, false);
        (int eax70, _, int ecx70, _) = X86Base.CpuId(7, 0);
        // EAX of leaf 7 subleaf 0 is the highest subleaf; asking past it is undefined.
        int eax71 = eax70 >= 1 ? X86Base.CpuId(7, 1).Eax : 0;
        return new Vnni(Vex: (eax71 & (1 << 4)) != 0, Evex: (ecx70 & (1 << 11)) != 0);
    }

    /// <summary>
    /// Clears the DLL's VNNI flag when this processor would fault on it, or always when
    /// <paramref name="force"/> is set (a test switch: it proves the other routine transcribes
    /// on a processor that does not need it). Returns one line for the log saying what it did.
    /// </summary>
    public static string Apply(string? nativeFolder, bool force)
    {
        Vnni cpu = Detect();
        bool needed = cpu.Evex && !cpu.Vex;
        if (!needed && !force) return $"{cpu}: Microsoft's VNNI check is right on this processor";

        string why = needed ? $"{cpu}: the speech model would fault on AVX-VNNI"
                            : $"{cpu}: forced by --vnni-off";
        if (nativeFolder is null) return $"{why}; no speech component folder, nothing to patch";

        string path = Path.Combine(nativeFolder, ModuleName);
        IntPtr module = LoadLibraryExW(path, IntPtr.Zero,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (module == IntPtr.Zero)
        {
            return $"{why}; could not load {path} (error {Marshal.GetLastWin32Error()}), not patched";
        }

        try
        {
            IntPtr flag = FindFlag(module, out string detail);
            if (flag == IntPtr.Zero) return $"{why}; {detail}; not patched";

            byte before = Marshal.ReadByte(flag);
            if (before == 0) return $"{why}; the flag is already clear, nothing to do";
            if (before != 1)
            {
                return $"{why}; the flag holds {before}, not a flag this code understands; not patched";
            }

            if (!VirtualProtect(flag, 1, PAGE_READWRITE, out uint old))
            {
                return $"{why}; VirtualProtect failed (error {Marshal.GetLastWin32Error()}), not patched";
            }
            Marshal.WriteByte(flag, 0);
            VirtualProtect(flag, 1, old, out _);
            return $"{why}; cleared its VNNI flag at {ModuleName}+0x{(long)flag - (long)module:X}, " +
                   "so it uses its non-VNNI routine";
        }
        catch (Exception ex)
        {
            return $"{why}; {ex.GetType().Name}: {ex.Message}; not patched";
        }
        // The module is deliberately not freed: this reference is what keeps the cleared flag
        // alive if the speech runtime ever lets go of its own.
    }

    // The address of the flag byte, or zero with `detail` saying why not.
    private static IntPtr FindFlag(IntPtr module, out string detail)
    {
        int ntOffset = Marshal.ReadInt32(module, 0x3C);
        IntPtr nt = module + ntOffset;
        if (Marshal.ReadInt32(nt) != 0x00004550)
        {
            detail = "the module has no PE header where one should be";
            return IntPtr.Zero;
        }
        int sections = Marshal.ReadInt16(nt, 4 + 2);
        int optionalSize = Marshal.ReadInt16(nt, 4 + 16);
        IntPtr table = nt + 4 + 20 + optionalSize;

        var code = new List<(uint va, uint size)>();
        var data = new List<(uint va, uint size)>();
        for (int i = 0; i < sections; i++)
        {
            IntPtr s = table + i * 40;
            uint size = (uint)Marshal.ReadInt32(s, 8);
            uint va = (uint)Marshal.ReadInt32(s, 12);
            uint flags = (uint)Marshal.ReadInt32(s, 36);
            if ((flags & IMAGE_SCN_MEM_EXECUTE) != 0) code.Add((va, size));
            if ((flags & IMAGE_SCN_MEM_WRITE) != 0) data.Add((va, size));
        }

        long found = 0;
        int matches = 0;
        foreach ((uint va, uint size) in code)
        {
            byte[] text = new byte[size];
            Marshal.Copy(module + (int)va, text, 0, (int)size);
            int from = 0;
            while (true)
            {
                int at = text.AsSpan(from).IndexOf(CheckHead);
                if (at < 0) break;
                at += from;
                from = at + 1;

                // je rel8, then mov byte ptr [rip+rel32],1
                int je = at + CheckHead.Length;          // the rel8
                int store = je + 1;
                if (store + 7 > text.Length) continue;
                if (text[store] != 0xC6 || text[store + 1] != 0x05 || text[store + 6] != 0x01) continue;
                long target1 = va + store + 7 + BitConverter.ToInt32(text, store + 2);

                int fallback = je + 1 + (sbyte)text[je];
                if (fallback < 0 || fallback + Fallback.Length + 4 > text.Length) continue;
                if (!text.AsSpan(fallback, Fallback.Length).SequenceEqual(Fallback)) continue;
                int rel2 = fallback + Fallback.Length;
                long target2 = va + rel2 + 4 + BitConverter.ToInt32(text, rel2);

                if (target1 != target2) continue;
                ++matches;
                found = target1;
            }
        }

        if (matches != 1)
        {
            detail = matches == 0
                ? "this build of the DLL does not contain the VNNI check this workaround knows"
                : $"the VNNI check appears {matches} times, so which flag to clear is ambiguous";
            return IntPtr.Zero;
        }
        if (!data.Any(d => found >= d.va && found < d.va + d.size))
        {
            detail = $"the flag the check names (+0x{found:X}) is not in a writable section";
            return IntPtr.Zero;
        }
        detail = "";
        return module + (int)found;
    }
}
