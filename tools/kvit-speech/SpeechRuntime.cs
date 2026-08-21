// Making the Windows AI Speech API reachable from a process with no package identity.
//
// ─── The problem ────────────────────────────────────────────────────────────────────────────
//
// Microsoft's documentation says a Windows AI API requires the app to be "packaged as an MSIX
// package with the systemAIModels capability declared in Package.appxmanifest". Taken at face
// value that would force kvit-dictate's whole process tree to acquire package identity, because
// this helper is spawned by the Python sidecar, which is spawned by kvit-dictate.exe.
//
// Measured on 2026-08-21, that is not what the requirement actually is:
//
//   * SpeechRecognitionModel.GetReadyState() answers correctly from a plain unpackaged console
//     process - it returned NotReady, then Ready after EnsureReadyAsync(), with no manifest and
//     no capability anywhere.
//   * SpeechRecognitionModel.TryCreateAsync() is the call that fails, with COMException
//     0x8007007E - ERROR_MOD_NOT_FOUND. Not an access denial, not a policy refusal: a DLL that
//     could not be found.
//
// The DLL is real. The speech implementation ships inside the on-demand OS component package
// (Microsoft.Windows.AI.Speech.Preview.CPU, or its NPU sibling on a Copilot+ device) in a
// `winml\` subfolder: asrapi.dll, asrmodelapi.dll, vadapi.dll, cacheapi.dll. A packaged process
// gets those resolved through its package graph. An unpackaged one has no package graph, so the
// loader never looks there.
//
// ─── The fix ────────────────────────────────────────────────────────────────────────────────
//
// Find the component package, put its winml\ folder on the native search path, and TryCreateAsync
// succeeds. Confirmed: LoadLibraryEx on each of the four DLLs by full path succeeds from an
// unpackaged process, so this is a search-path problem and nothing more - no ACL stands in the
// way and no identity is required.
//
// This is a documented-behaviour gap, not a documented feature, and it is the single most likely
// thing here to break on a future Windows update. If it does, the symptom is exactly 0x8007007E
// from TryCreateAsync and the fallback is to give this helper - and only this helper - a sparse
// package identity. Nothing else in kvit-dictate would need to change, which is most of the
// reason the Speech API lives in a separate process at all.

using System.Runtime.InteropServices;
using Windows.Management.Deployment;

namespace KvitSpeech;

internal static class SpeechRuntime
{
    [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr AddDllDirectory(string path);

    [DllImport("kernel32", SetLastError = true)]
    private static extern bool SetDefaultDllDirectories(uint directoryFlags);

    private const uint LOAD_LIBRARY_SEARCH_DEFAULT_DIRS = 0x00001000;

    // Matched by prefix rather than by exact family name. The suffix names the execution provider
    // (".CPU" here, ".NPU" on a Copilot+ device) and "Preview" will presumably be dropped when the
    // API graduates; matching the stem means neither of those turns into "the API is unavailable".
    private const string ComponentNamePrefix = "Microsoft.Windows.AI.Speech";

    /// <summary>
    /// Puts the speech component's native folder on this process's DLL search path.
    /// Returns the folder that was added, or null if no component package was found -
    /// in which case the model is simply not installed and GetReadyState() will say so.
    /// </summary>
    public static string? Prepare()
    {
        // Opting in to the safe search order is required before AddDllDirectory does anything.
        SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);

        foreach (string winml in CandidateNativeFolders())
        {
            if (Directory.Exists(winml) && AddDllDirectory(winml) != IntPtr.Zero) return winml;
        }
        return null;
    }

    private static IEnumerable<string> CandidateNativeFolders()
    {
        foreach (string root in InstalledComponentRoots())
        {
            yield return Path.Combine(root, "winml");
        }
    }

    private static IEnumerable<string> InstalledComponentRoots()
    {
        // Asking the package manager is the supported way and needs no capability for a
        // current-user query. It is tried first and its failure is not fatal, because the
        // fallback below finds the same directory by the path the package manager would report.
        List<string> roots = new();
        try
        {
            foreach (var pkg in new PackageManager().FindPackagesForUser(string.Empty))
            {
                if (!pkg.Id.Name.StartsWith(ComponentNamePrefix, StringComparison.OrdinalIgnoreCase))
                    continue;
                try { roots.Add(pkg.InstalledPath); }
                catch (Exception) { /* a package mid-uninstall has no path; skip it */ }
            }
        }
        catch (Exception)
        {
            // No rights, or the API is unavailable. Fall through to the directory scan.
        }

        foreach (string r in roots) yield return r;

        string apps = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "WindowsApps");
        string[] dirs;
        try { dirs = Directory.GetDirectories(apps, ComponentNamePrefix + "*"); }
        catch (Exception) { yield break; }   // WindowsApps is ACL'd; not being able to list it is normal

        // A model update leaves the superseded directory in place, and the version is in the
        // directory name in a form that does not sort usefully. Most recently written wins.
        foreach (string dir in dirs.OrderByDescending(Directory.GetLastWriteTimeUtc)) yield return dir;
    }
}
