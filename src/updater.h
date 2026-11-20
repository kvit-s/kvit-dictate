// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// Updates: ask GitHub for a newer release, download its installer, check it, and run it.
//
// ─── What it does, start to finish ──────────────────────────────────────────────────────────
//
//   1. GET https://api.github.com/repos/kvit-s/kvit-dictate/releases — the list, not
//      /releases/latest, because /latest never returns a pre-release and a pre-release build has
//      to be able to see the next one (`include_prereleases` below).
//   2. Pick the highest version above the running one that carries an installer and a
//      SHA256SUMS.txt (`pick_update`, pure and unit-tested).
//   3. Download SHA256SUMS.txt, then the installer into %TEMP%\kvit-dictate-update\, and refuse
//      it unless its SHA-256 is the one the release lists.
//   4. Run it as `setup.exe /SILENT ... /WAITPID=<this process> /RELAUNCH=1` and quit. The
//      installer waits for this process to be gone before it touches a file — the Inno Setup
//      Restart Manager cannot close this app, which has no top-level window — and starts the new
//      version when it is done (packaging/windows/kvit-dictate.iss).
//
// ─── What it does not do ────────────────────────────────────────────────────────────────────
//
// It never installs without being asked. The automatic daily check (Settings → "Check GitHub
// for a new version once a day") only *announces* a release, with a tray notification; the
// download starts after the user presses Install in the dialog that follows.
//
// It does not update a copy that the installer did not put there — the portable ZIP, or a
// development build. Running the installer from one would install a *second* copy under
// %LOCALAPPDATA%\Programs and leave the one the user runs untouched; `installed_here` detects
// it and the dialog offers the download page instead.
//
// The checksum guards against a damaged or truncated download, not against a compromised
// GitHub account: SHA256SUMS.txt comes from the same release as the installer. That is the same
// trust a user extends by downloading the installer from the release page in a browser, and no
// more. Nothing is signed (release-plan.md §4).
//
// Threading: everything here blocks, and is meant for a worker thread. Nothing here may run on
// main.cpp's UI thread, which owns the low-level keyboard hook (Windows removes a hook whose
// thread stops answering).

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "util/result.h"

namespace kd::update {

// ─── Versions ───────────────────────────────────────────────────────────────────────────────

// MAJOR.MINOR.PATCH with an optional pre-release suffix, `1.0.0-rc2`. A leading `v` (the tag
// spelling) is accepted.
struct Version {
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;
    std::string pre;  // without the '-': "rc2", "beta.1"; empty for a release

    [[nodiscard]] bool is_prerelease() const noexcept { return !pre.empty(); }
};

bool parse_version(std::string_view text, Version& out);
std::string to_string(const Version& v);

// Semantic-versioning order: a pre-release is older than its release (1.0.0-rc2 < 1.0.0), and
// pre-release identifiers compare naturally, so rc10 is newer than rc9 — SemVer proper would
// compare "rc10" and "rc9" as strings and get that backwards. Returns <0, 0 or >0.
int compare(const Version& a, const Version& b);

// The version this build was stamped with: CMakeLists.txt's project() version plus the
// pre-release suffix the release build passes from its tag (KD_VERSION_SUFFIX). A local build
// has no suffix.
std::string_view running_version() noexcept;

// ─── The release list ───────────────────────────────────────────────────────────────────────

inline constexpr const char* kReleasesPage = "https://github.com/kvit-s/kvit-dictate/releases";

struct Release {
    std::string tag;            // "v1.0.1"
    Version version;
    bool prerelease = false;
    std::string page_url;       // the release's page, for "what's new" and the portable case
    std::string installer_name; // "kvit-dictate-1.0.1-setup.exe"
    std::string installer_url;
    uint64_t installer_size = 0;
    std::string sums_url;       // SHA256SUMS.txt
};

// Pure. `releases_json` is the body of GET /repos/{owner}/{repo}/releases. Drafts are skipped,
// pre-releases unless `include_prereleases`, and so is any release without both an installer and
// SHA256SUMS.txt — it cannot be installed safely, so offering it would only end in an error.
// Returns no value when nothing is newer than `current`.
Result<std::optional<Release>> pick_update(std::string_view releases_json, const Version& current,
                                           bool include_prereleases);

// Pure. Finds `file_name`'s line in a `sha256sum` listing ("<64 hex>  <name>", with an optional
// '*' before the name) and returns the hash in lower case.
bool find_checksum(std::string_view sums_text, std::string_view file_name, std::string& hex_out);

// ─── Network ────────────────────────────────────────────────────────────────────────────────

// Steps 1 and 2. Pre-releases are included exactly when the running build is one, so a user who
// installed a release candidate hears about the next candidate and about the release, and a user
// on a release only ever hears about releases.
Result<std::optional<Release>> check_github(const Version& current);

// Step 3. `progress(done, total)` is called from this thread as bytes arrive; `cancel` is
// polled between reads. On success `out_file` is the verified installer. An installer already
// in the folder with the right hash is used without downloading it again.
Status download_installer(const Release& r, const std::function<void(uint64_t, uint64_t)>& progress,
                          const std::atomic<bool>& cancel, std::filesystem::path& out_file);

// Deletes what an earlier update left in %TEMP%\kvit-dictate-update: installers for this
// version or older (the one that just installed this copy among them), broken downloads, and the
// install log. Not the installer of a newer version, which may still be about to run.
void remove_stale_downloads(const Version& running);

// ─── Installing ─────────────────────────────────────────────────────────────────────────────

enum class InstallKind : uint8_t {
    NotInstalled,  // portable ZIP, or a build: the installer would not replace this copy
    PerUser,       // the default: %LOCALAPPDATA%\Programs, no UAC prompt
    AllUsers,      // installed for all users (an elevated install); updating asks for UAC
};

// How the copy in `exe_dir` was installed, from the uninstall entry Inno Setup writes under the
// application's fixed AppId (kvit-dictate.iss). Only a matching InstallLocation counts.
InstallKind installed_here(const std::filesystem::path& exe_dir);

// Pure. Whether two directory paths name the same directory, ignoring case and a trailing
// separator — the comparison `installed_here` makes against InstallLocation.
bool same_directory(std::wstring_view a, std::wstring_view b);

// Pure. The installer's command line (argv[0] included).
std::wstring installer_command_line(const std::filesystem::path& setup, InstallKind kind,
                                    uint32_t wait_pid);

// Step 4, minus the quitting, which is the caller's. The installer is started as a separate
// process, outside this one's job objects, so it outlives this process.
Status launch_installer(const std::filesystem::path& setup, InstallKind kind, uint32_t wait_pid);

// ─── The dialogs ────────────────────────────────────────────────────────────────────────────

struct UiContext {
    std::filesystem::path exe_dir;
    // Called on the worker thread once the user agreed and the installer is downloaded and
    // verified. Not called otherwise. The receiver hands it to its UI thread, which decides when
    // to run it — not in the middle of a dictation.
    std::function<void(const std::filesystem::path& setup, InstallKind kind)> on_ready;
};

// The whole interactive flow on the calling thread, which must be a worker: check (unless
// `known` is given — the release a background check already found), say what was found, ask,
// download with a progress bar and a Cancel button, verify, and call `on_ready`.
// Every outcome is reported in a dialog, including "you have the latest version".
void run_interactive(const UiContext& ctx, std::optional<Release> known);

// A plain error dialog, for the caller's own failure after `on_ready` (the installer would not
// start). Blocks; call it from a worker.
void show_error(std::string_view headline, std::string_view detail);

}  // namespace kd::update
