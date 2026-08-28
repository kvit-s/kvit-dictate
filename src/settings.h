// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// Settings, and the `models.json` model→interpreter manifest.
//
// Implements: plan.md §1.1 (the manifest lives on the C++ side), and the settings surface named
// across §1.4 (stability params, cadence), §1.6 (the two silence timers), §1.7 (injection method
// and per-app policy), §1.8 (hotkey), §1.9 (privacy: transcript logging opt-in).
//
// This header is **pure**: no Windows headers, no registry, no COM. It parses and serialises
// with util/json.h and takes paths as std::filesystem::path, so manifest parsing and settings
// migration are unit-testable. The one Windows-flavoured thing it exposes is
// `default_settings_path()`, which is declared here and implemented in settings.cpp against
// SHGetKnownFolderPath.
//
// ═══════════════════════════════════════════════════════════════════════════════════════════
//  models.json — schema version 1
// ═══════════════════════════════════════════════════════════════════════════════════════════
//
// JSON carries no comments, so the schema is documented here, next to the parser that enforces
// it. `models.json` sits beside the executable (and, in a source tree, at the repository root).
//
// ─── Why the manifest is on the C++ side at all ─────────────────────────────────────────────
//
// plan.md §1.1: every candidate model has a different runtime — NeMo, HuggingFace transformers,
// faster-whisper/CTranslate2, ONNX — and plan.md §5 gives several of them **their own locked
// virtual environment**. A Python-side model registry cannot enumerate models that only exist
// under a *different* interpreter, which is exactly the situation that creates. So the
// model→interpreter mapping lives here, and selecting a model restarts the sidecar under that
// model's interpreter. The C++ side still never learns what the model *is* — only which
// executable to launch and with what arguments.
//
// ─── Shape ──────────────────────────────────────────────────────────────────────────────────
//
//     {
//       "schema": 1,
//       "default_model": "mock",
//       "models": [ <entry>, ... ]
//     }
//
// ─── Entry fields ───────────────────────────────────────────────────────────────────────────
//
//   "id"              (string, required)  Stable identifier. Referenced by `default_model`, by
//                                         settings, and echoed by the sidecar in `loading`/
//                                         `ready` (docs/protocol.md §6.2). `[A-Za-z0-9_.-]{1,64}`.
//   "display_name"    (string, required)  Shown in the tray menu. Free text.
//   "interpreter"     (string, required)  **Absolute path to a python.exe.** Not a name on
//                                         PATH, not a venv directory, not "python". Absolute,
//                                         because plan.md §5.1 makes the whole point that
//                                         different models run under different, non-default
//                                         interpreters, and resolving through PATH would
//                                         silently pick whichever one happened to be first.
//   "module"          (string, required)  Passed as `-m <module>`. Normally "kvit_asr".
//   "args"            (array of string)   Extra arguments, before the protocol-mandated
//                                         `--pipe <name> --proto 1` that ipc_client.h appends.
//   "streaming_mode"  (string, required)  "native-streaming" | "stateful-chunked" |
//                                         "pseudo-streaming". Exactly the taxonomy Phase 0A
//                                         records and `ready.mode` carries, so latency numbers
//                                         stay interpretable — a batch Whisper adapter wrapped
//                                         in a sliding window is not comparable to a natively
//                                         streaming model. Declared here **and** reported by the
//                                         sidecar; a mismatch is a configuration error worth
//                                         surfacing rather than silently trusting one side.
//   "requires"        (array of string)   Human-readable prerequisites ("nemo>=25.11",
//                                         "onnxruntime-gpu"). Never installed automatically —
//                                         plan.md §1.9 forbids runtime downloads and §5.1
//                                         forbids installing into a shared venv. This field is
//                                         documentation that surfaces in the UI when a launch
//                                         fails. NOTE for implementers: `requires` is a C++20
//                                         keyword, so the struct member below is `requires_`.
//   "env"             (object)            Extra environment variables for the child, e.g.
//                                         HF_HOME, UV_CACHE_DIR (plan.md §5.2).
//   "notes"           (string)            Free text, shown in the model picker's tooltip.
//   "enabled"         (bool, default true)
//
// ─── What the manifest deliberately does not contain ────────────────────────────────────────
//
// No download URLs, no checksums, no "fetch if missing". plan.md §1.9: no model auto-download at
// runtime; model fetching is an explicit, separate, user-initiated action. A manifest that could
// trigger a download would make that promise unenforceable.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "anchor.h"
#include "commit_engine.h"
#include "hotkey.h"
#include "normalizer.h"
#include "injector.h"
#include "overlay.h"
#include "protocol.h"
#include "session.h"
#include "util/log.h"
#include "util/result.h"

namespace kd {

// ─── models.json ────────────────────────────────────────────────────────────────────────────

inline constexpr int kModelManifestSchema = 1;

struct ModelEntry {
    std::string id;
    std::string display_name;
    std::filesystem::path interpreter;  // absolute path to a python.exe
    std::string module = "kvit_asr";
    std::vector<std::string> args;
    proto::StreamingMode streaming_mode = proto::StreamingMode::NativeStreaming;
    std::vector<std::string> requires_;  // JSON key is "requires"; `requires` is a C++20 keyword
    std::vector<std::pair<std::string, std::string>> env;
    std::string notes;
    bool enabled = true;

    // True if `interpreter` exists on disk right now. The manifest is allowed to list models
    // whose environment has not been created yet — plan.md §5.2 creates environments lazily and
    // only when a backend needs one — so a missing interpreter is a *state*, surfaced in the
    // picker, not a parse error.
    [[nodiscard]] bool interpreter_available() const;
};

struct ModelManifest {
    int schema = kModelManifestSchema;
    std::string default_model;
    std::vector<ModelEntry> models;

    [[nodiscard]] const ModelEntry* find(std::string_view id) const;
};

// Parse manifest text. Strict: an unknown schema version, a missing required field, a relative
// interpreter path, or an unrecognised streaming_mode is an error, because every one of those
// produces a confusing runtime failure later. `err` carries a message naming the entry and the
// field.
Result<ModelManifest> parse_model_manifest(std::string_view json_text);

Result<ModelManifest> load_model_manifest(const std::filesystem::path& file);

// Everything that must be true before a sidecar can be launched for `model_id`: the id resolves,
// the entry is enabled, and the interpreter file is actually on disk.
//
// This exists so the failure is reported *here*, naming the model, the absolute path that is
// missing and the entry's `requires` list, instead of surfacing as CreateProcess failing with
// ERROR_FILE_NOT_FOUND against a path the user never sees. plan.md §5.2 creates environments
// lazily, so "listed but not installed" is the expected first-run state for every entry except
// the mock, and it deserves a sentence the user can act on.
Status check_model_launchable(const ModelManifest& m, std::string_view model_id);

// Serialise, for a future manifest editor. Round-trips through parse_model_manifest().
std::string serialize_model_manifest(const ModelManifest& m);

// ─── Settings ───────────────────────────────────────────────────────────────────────────────

inline constexpr int kSettingsSchema = 1;

struct Settings {
    int schema = kSettingsSchema;

    // Which sidecar to run (an id from models.json).
    std::string model_id;

    HotkeySpec hotkey;

    // plan.md §1.6 — two unrelated timers a naive implementation merges.
    SessionConfig session;

    // plan.md §1.4 — eligibility and cadence, separated.
    StabilityParams stability;

    // plan.md §1.7 — global default plus per-app overrides.
    InjectConfig inject;
    std::vector<AppPolicy> app_policies;

    // plan.md §1.7: "terminals want Ctrl+Shift+V". The paste chord is a per-target property, so
    // the authoritative override is AppPolicy::use_ctrl_shift_v (injector.h) and consoles are
    // additionally detected through TargetIdentity::is_console. This is only the default applied
    // to a target with no policy of its own.
    //
    // GAP, recorded rather than hidden: InjectConfig has no corresponding field, so the injector
    // cannot currently read this. It is persisted, shown in the dialog, and used to seed new
    // per-app policies. Wiring it fully needs one bool on InjectConfig.
    bool paste_ctrl_shift_v_default = false;

    // docs/editing.md §12 — the pill as a compose buffer. `editing.enabled` is forced off by
    // normalise() unless the cadence is `end-of-session`, because under Cadence::Immediate the
    // text is already in the document as it is spoken and there is nothing in the pill to edit
    // (docs/editing.md §6.1). That is a live trap rather than a hypothetical one: Immediate is
    // still the default in commit_engine.h, so a user who has changed nothing would otherwise
    // get a feature that parses, validates, round-trips and does nothing.
    EditingConfig editing;

    // The remote text rewriter (normalizer.h). Forced off by normalise() under any cadence but
    // `end-of-session`, for the same reason `editing.enabled` is and with more at stake: a
    // rewrite is not prefix-stable, so under Cadence::Immediate it would revise text that is
    // already in the user's document. Seeded from `context_terms` there too, so the vocabulary
    // the user maintains for the recogniser also protects them from the rewriter.
    NormalizerConfig normalizer;

    // plan.md §1.5 / §1.3.
    anchor::AnchorConfig anchor;
    OverlayStyle overlay_style;
    OverlayMode overlay_mode = OverlayMode::Inline;

    // Audio device. Empty means "follow the default capture device".
    std::string audio_device_id;

    // plan.md §1.9 — privacy. Both default to the private choice, and the transcript switch is
    // additionally compiled out of release builds (util/log.h), so a mis-set setting cannot
    // enable it in a shipped binary.
    log::Level log_level = log::Level::Info;
    bool diagnostics_file_enabled = false;
    log::TranscriptPolicy transcript;

    bool start_with_windows = false;

    // Vocabulary hints sent with `start` (docs/protocol.md §6.1). Kept separate from
    // SessionConfig::context so that the settings file is the single source and the session
    // config is derived — one place to edit, one place to read.
    std::vector<std::string> context_terms;
    bool send_context = true;

    // Apply the derived fields (context into SessionConfig, etc.) after loading or editing.
    void normalise();

    // Reject a settings file that would violate a plan invariant, rather than silently
    // correcting it: notably `stability.speculative_unsafe`, which plan.md §1.4 excludes from
    // defaults and never ships. Returns an error naming the field.
    [[nodiscard]] Status validate() const;
};

// %LOCALAPPDATA%\kvit-dictate\settings.json
//
// Local, not roaming. Everything in this file is machine-specific — absolute interpreter paths
// from models.json, a WASAPI endpoint id, per-app injection policies keyed on executables that
// exist on *this* machine — so roaming it to another PC would carry settings that are wrong
// there and cannot be right anywhere else.
std::filesystem::path default_settings_path();

// Directory the executable lives in — where models.json is looked up first.
std::filesystem::path executable_directory();

// The directory diagnostics and (when opted in) transcripts are written to, and the one the tray
// menu's "Open log folder" opens: %LOCALAPPDATA%\kvit-dictate\logs.
std::filesystem::path default_log_directory();

// A missing file is not an error: it yields defaults, which is a first run. A malformed file
// *is* an error, and the caller must surface it rather than silently reverting to defaults,
// because silently discarding a user's configuration is indistinguishable from losing it.
Result<Settings> load_settings(const std::filesystem::path& file);
Status save_settings(const Settings& s, const std::filesystem::path& file);

Result<Settings> parse_settings(std::string_view json_text);
std::string serialize_settings(const Settings& s);

// ─── The settings dialog ────────────────────────────────────────────────────────────────────
//
// Plain Win32, built programmatically (no .rc, no dialog template — CMakeLists links no resource
// script and adding one is a build change, not a settings change). plan.md §1.3 permits a Qt
// Widgets fallback for this one dialog; it was not taken. See the note at the top of the
// implementation in settings.cpp for the honest assessment of that trade.
//
// `instance` is an HINSTANCE and `owner` an HWND, both as opaque values, so this header stays
// Windows-free and the rest of the file remains unit-testable without a UI.
//
// Modal with respect to `owner`: the owner is disabled and a nested message loop runs, so the
// posted IPC/hotkey messages the owner relies on are still dispatched while the dialog is up.
// Returns true when the user accepted, in which case `s` has been replaced with a normalised and
// validated copy; false on cancel, in which case `s` is untouched. Saving is the caller's job —
// this function never writes a file.
bool show_settings_dialog(uintptr_t instance, uintptr_t owner, Settings& s,
                          const ModelManifest& manifest);

}  // namespace kd
