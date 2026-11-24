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
//
// ─── AND: the manifest is no longer user-facing ─────────────────────────────────────────────
//
// release-plan.md §2.2. A shipped build has no child process to launch, so a map from a model id
// to an interpreter and an argv has nothing to map. `ServerSettings` below replaced it: the user
// types a URL, and which model runs behind that URL is the server operator's business.
//
// **The file, the schema and this parser stay** — the bench still spawns interpreters locally and
// still needs the map — but the installer does not lay `models.json` down beside the executable
// and the settings dialog does not read it. A source tree keeps both.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "anchor.h"
#include "corpus.h"
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

// ─── Where the ASR server is ────────────────────────────────────────────────────────────────
//
// release-plan.md §2.1: there was no way for a user to type a server address. The endpoint was a
// Python module constant, the model was a `models.json` id, and every field in `Settings`
// assumed the person running the app had also written it.
//
// UTF-8 rather than wide, like every other string in this header, so that the file stays pure
// and testable; ipc_client.h takes the wide form and `endpoint()` converts.
// How to read `ServerSettings::url` (openai-translator.md §4.6).
//
// This exists because the two things a user might type into one box are indistinguishable by
// inspection and catastrophically different in effect. `parse_server_url` treats `http://` and
// `https://` as **WebSocket synonyms** (docs §2.6.1, because people type them), so storing
// `http://your-gpu-box.lan:10301` in a Local-mode field and connecting to it as a protocol server
// would make the client attempt a WinHTTP upgrade against an OpenAI HTTP API — which is
// precisely the failure openai-translator.md exists to end.
enum class ServerMode : uint8_t {
    // `url` is an OpenAI-compatible speech service. The client launches `kvit-asrd.exe` beside
    // itself and points it at that address. **The default**, and what most people configure.
    Local,
    // `url` is a docs/protocol.md server the user runs themselves. Unchanged behaviour: the
    // client speaks the protocol to it directly and spawns nothing.
    Protocol,
    // **No server at all.** The client launches `kvit-asrd.exe --backend nemotron`, which runs
    // NVIDIA's Nemotron speech model on this machine's CPU (src/asrd/backend_nemotron.h). The
    // model is downloaded once, from the Settings page; after that nothing crosses the network.
    // `url`, `model` and `api_key` are kept and unused, so switching back to a service restores
    // what was typed there. Until 2026-11-23 this meant Microsoft's Windows speech model; the
    // stored value `this-pc` is unchanged, so a settings file from then simply moves to Nemotron.
    ThisPc,
};

const char* to_string(ServerMode m) noexcept;
bool server_mode_from_string(std::string_view s, ServerMode& out) noexcept;

struct ServerSettings {
    // What `url` means. Written as `server.mode` and **absent in every settings file written by
    // an older build**, which is why `migrate_server_mode()` below exists rather than a schema
    // bump: bumping `kSettingsSchema` would make those files unreadable, and every one of them
    // can be interpreted correctly from the URL it already holds.
    ServerMode mode = ServerMode::Local;

    // In Protocol mode: docs/protocol.md §2.6.1 — `ws://`, `wss://`, and `http://`/`https://`
    // as synonyms because people type them. In Local mode: `http://` or `https://` only, and
    // validated by a separate helper, never by `parse_server_url`.
    //
    // **Empty is the normal first-run state, not an error** — docs §10.6 and release-plan.md
    // §2.3 make "no server configured" a first-class state.
    std::string url;

    // Optional model request (docs §5.1). Empty means "whatever the server serves by default".
    // Free text rather than a picker, because a server is not obliged to advertise a catalogue;
    // when it does, the Test button shows what came back.
    std::string model;

    // Optional, Local mode only: sent as `Authorization: Bearer` to the speech service. A LAN box
    // wants none; a hosted one such as OpenRouter does.
    //
    // Plain text here and **only here**. On disk it is `server.api_key_dpapi`, the key encrypted
    // with CryptProtectData under this Windows user and base64'd, so settings.json can be backed
    // up, diffed or pasted into a bug report without handing the key over. A file copied to
    // another user or machine therefore loses its key rather than failing to load: the key is
    // re-entered, and everything else survives. On the way to the translator it goes through a
    // pipe, never a command line (translator.h).
    std::string api_key;

    // Live preview in Local mode: `partial`s with a computed `stable_bytes` against a batch
    // model that structurally violates monotonicity. Meaningless in Protocol mode, where the
    // server decides for itself what it publishes.
    //
    // **On by default**, which is a reversal. openai-translator.md §4.3 argued for off, and the
    // argument was about committing: a batch model revises byte 0, so a preview against one
    // cannot honestly publish a mark. That part is true, and it is now handled where it belongs:
    // against this backend a `partial` carries `stable_bytes = 0` and promises nothing, so no
    // revision can contradict one and everything still commits at the `final`. (It was not
    // always: the two were one switch, and a mark this backend could not keep cost a real
    // dictation its session mid-sentence. See openai-translator.md §4.3.) What the argument got
    // wrong is that the choice was never about committing:
    // partials are also what the overlay greys out as "still moving", and with them off there
    // is nothing to see until the speaker pauses. The Python bridge this replaced defaulted the
    // other way (`openai_http.py`'s `finalize_only=False`) and that is the behaviour people
    // recognise as the thing working. Costing nothing in the document and everything in the
    // feedback, it belongs on.
    bool live_preview = true;

    uint32_t connect_timeout_ms = 15000;

    // Reconnect policy after a lost connection (docs §10.3 step 5). Exponential from `initial`
    // to `max`. A server that is switched off must not be hammered, and the tray must say
    // "reconnecting" rather than pretending to be idle.
    uint32_t reconnect_initial_ms = 1000;
    uint32_t reconnect_max_ms = 30000;

    // This PC needs no address: the model is on the machine, so choosing it *is* configuring it.
    [[nodiscard]] bool configured() const { return mode == ServerMode::ThisPc || !url.empty(); }
};

// Validate `url` as a **speech-service** address: `http://` or `https://` only, well formed, and
// free of the characters that would need quoting on the command line the client builds for
// `kvit-asrd --service`.
//
// Deliberately not `parse_server_url`. That function's job is the opposite — it *accepts*
// `http://` as a WebSocket synonym — and reusing it here is exactly how a speech-service address
// ends up being upgraded. Returns an empty string when the URL is usable, and a sentence the
// user can act on otherwise.
std::string validate_speech_service_url(std::string_view url);

// Validate an API key: printable ASCII, no spaces, at most 1024 characters. The same rule as
// kvit-asrd's `api_key_problem()`, so a key this accepts is a key the translator accepts, and
// none of them can end the header line it is sent on. Empty is valid and means "no key".
// Returns an empty string when the key is usable, and a sentence the user can act on otherwise.
std::string validate_api_key(std::string_view key);

// Choose a `mode` for a settings file that has none (openai-translator.md §4.6):
//
//   * empty url                -> Local, and the first-run Server page. Unchanged behaviour.
//   * `ws://` or `wss://`      -> Protocol. A user who already pointed the app at a Python
//                                 server keeps that, with no dialog and no surprise.
//   * anything else            -> Local, and the URL is re-validated as a speech-service
//                                 address on the next save. It is NOT silently fed to
//                                 `IpcClient::connect`, because an `http://` URL there is the
//                                 ambiguous case this whole enum exists to disambiguate.
ServerMode migrate_server_mode(std::string_view url) noexcept;

// ─── The dictation marker ───────────────────────────────────────────────────────────────────
//
// Text put around what was dictated, on its way into the document: prefix + words + suffix.
//
// It exists for one use, and the use explains the shape. Dictating into a coding agent, nobody
// speaks a proper noun the recogniser has never heard and gets it back right — `context_terms`
// helps, the rewriter helps a little, and neither of them fixes the tail. But the reader is an
// LLM, which is very good at working out what was meant *if it knows it is reading speech*. So
// rather than making the transcript exact, the marker tells the reader it is approximate:
//
//     prefix "[dictated: "  suffix "]"   ->   [dictated: open teh sesson file and fix the typo]
//
// Both halves are free text and either may be empty. A prefix alone is a leading tag; both
// together are a wrapper, which is what a bracket needs and why this is not one field.
//
// ─── Where it is applied, and where it is deliberately not ──────────────────────────────────
//
// **At injection time, never in the buffer.** Under `Cadence::EndOfSession` the dictated text
// lives in `EditBuffer`, which is what the compose pill shows and what the rewriter rewrites; a
// marker stored there would be editable by Backspace, would be sent to the rewriter as if it
// were speech, and would come back rewritten into prose. So the buffer holds the user's words
// and only their words, and `App::flush_deferred_commits` wraps on the way out — including on
// the clipboard fallback, so a failed injection offers exactly the text the document would have
// received. Under the streaming cadences the prefix rides along with the first committed op and
// the suffix is injected once, by `App::finish_dictation`, on the one path every session ends
// through.
//
// A session that committed nothing is wrapped in nothing: an empty dictation must not deliver a
// bare "[dictated: ]".
struct TextAffix {
    std::string prefix;
    std::string suffix;

    [[nodiscard]] bool active() const { return !prefix.empty() || !suffix.empty(); }

    // `dictated` with the marker around it, for the paths that have the whole text at once.
    // Returns `dictated` untouched when it is empty, or when there is no marker to add.
    [[nodiscard]] std::string wrap(std::string_view dictated) const;
};

// Per field. Generous — a marker is a phrase, and someone will want a sentence of instructions
// in it — but bounded, because this text is injected into a document on every session and a
// settings file that carried a megabyte of it would type a megabyte.
inline constexpr size_t kMaxAffixBytes = 512;

struct Settings {
    int schema = kSettingsSchema;

    // release-plan.md §2.2. Where the recogniser is.
    ServerSettings server;

    // Which sidecar to run (an id from models.json). **Bench and source trees only**: a shipped
    // build spawns nothing, and `server.url` is what it uses instead. Kept because the bench
    // reads the same settings file.
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
    // Copied into InjectConfig::paste_ctrl_shift_v at session start, where the injector ORs it
    // with TargetIdentity::is_console. It was persisted, shown in the dialog and read by nothing
    // at all for as long as it existed — a checkbox that changed no behaviour.
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

    // Keep the pill out of screen capture (overlay.h, `set_capture_excluded`). On by default,
    // because the anchor estimator's commit differencing (source C) captures the screen around
    // the caret and must not find the pill's own glyphs there, and because it keeps dictated
    // text out of screenshots and screen sharing.
    //
    // **Off is for a screen the user sees *through* a capture**: Citrix, a remote-viewing app,
    // a DisplayLink dock. Those copy the desktop the way a screenshot does, so a hidden pill
    // never reaches the person dictating — reported 2026-11-23 from a work PC used through
    // Citrix, where the log placed the pill beside the caret on every dictation and nobody
    // could see it. With this off, App skips source C, since the pill would be in its captures;
    // the caret and the last click still place the pill.
    bool overlay_hide_from_capture = true;

    // Audio device. Empty means "follow the default capture device".
    std::string audio_device_id;

    // plan.md §1.9 — privacy. Both default to the private choice, and the transcript switch is
    // additionally compiled out of release builds (util/log.h), so a mis-set setting cannot
    // enable it in a shipped binary.
    log::Level log_level = log::Level::Info;
    bool diagnostics_file_enabled = false;
    log::TranscriptPolicy transcript;

    // Keep the undelivered buffer on disk while a deferred session is running, so that a session
    // interrupted by anything other than the user survives it.
    //
    // **This is the one privacy default that points the other way, and it is deliberate.** §1.9's
    // rule is about *transcripts* — a durable record of everything ever dictated. This is not
    // that: it holds exactly the text that has not yet reached the document, it is replaced in
    // place rather than appended to, and `App::clear_recovery` deletes it the instant that text
    // is delivered. A file exists only while a session is in flight, or after one ended without
    // delivering. It never accumulates and never outlives the words it is holding.
    //
    // What it buys, measured on 2026-08-31: 1742 bytes — three minutes of speech — destroyed when
    // the process was killed mid-session. `Cadence::EndOfSession` exists to encourage exactly
    // that kind of long dictation, and until this the buffer lived only in `EditBuffer` with the
    // clipboard fallback sitting on the graceful path, so a crash, Task Manager or a power cut
    // lost all of it. Worse, the overlay carries `WDA_EXCLUDEFROMCAPTURE` (overlay.h) unless
    // `overlay_hide_from_capture` is off — so the pending text is invisible to screen recorders
    // too. Nothing outside the process can see it. That is what
    // makes an on-disk copy the only recovery there can be, and why this defaults to on.
    bool crash_recovery = true;

    // Keep the text of the most recent *delivered* submission, so there is something to hand
    // back when injection reported success and the document got nothing.
    //
    // **This is an amendment to §1.9, and it is written here rather than assumed.** §1.9 says no
    // transcripts persisted by default, and `crash_recovery` above is allowed past it because it
    // holds only undelivered text and deletes itself on delivery. This one outlives delivery, so
    // it is a persisted transcript — of exactly one utterance, overwritten by the next, never
    // appended to. What buys it the default is that the failure it covers is *silent*: the
    // clipboard path returns Ok when the paste chord was accepted, which an application that
    // binds Ctrl+V elsewhere produces identically with nothing inserted (injector.cpp says so at
    // the return). Before this, `clear_recovery` deleted the only copy at exactly that moment.
    //
    // A user who wants §1.9 read strictly turns this off and loses only the recovery, not the
    // dictation. The corpus below is the setting for keeping *everything*, and it stays off.
    bool keep_last_submission = true;

    // corpus.h — the dictation corpus: audio plus the hypothesis timeline, kept so a different
    // model can be asked the same question later. **Off unless the user turned it on**, which is
    // the opposite of `crash_recovery` above and for a reason: this one is durable, accumulating
    // and includes the recording, which is precisely what plan.md §1.9 is about.
    CorpusConfig corpus;

    bool start_with_windows = false;

    // updater.h: ask GitHub once a day whether a newer release exists, and say so in a tray
    // notification. Only *asks*: nothing is downloaded or installed until the user presses
    // Install. On by default, because a user who never hears about a fix keeps the bug; the one
    // request it makes goes to api.github.com and carries nothing but the version.
    bool check_for_updates = true;

    // Put around the dictated text on its way into the document. Empty by default: the marker
    // is useful to a reader that knows what to do with it and is noise to every other one, so
    // it is opt-in rather than a default anybody has to notice and turn off.
    TextAffix affix;

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

// True when the settings file did not exist and these are pure defaults — i.e. a first run.
// release-plan.md §2.2 requires a shipped build to open the Server page rather than start
// against `models.json`'s `default_model`, which is `"mock"`: a scripted fixture that types
// invented sentences. A build that started there would look, to a new user, exactly like a
// recogniser that hallucinates.
bool settings_file_exists(const std::filesystem::path& file);

// A missing file is not an error: it yields defaults, which is a first run. A malformed file
// *is* an error, and the caller must surface it rather than silently reverting to defaults,
// because silently discarding a user's configuration is indistinguishable from losing it.
Result<Settings> load_settings(const std::filesystem::path& file);
Status save_settings(const Settings& s, const std::filesystem::path& file);

Result<Settings> parse_settings(std::string_view json_text);
std::string serialize_settings(const Settings& s);

// The file name "Save settings…" offers, without the extension: the speech service's host and
// the last part of the model, so several saved setups can be told apart in a folder listing —
// `openrouter.ai qwen3-asr-1.7b`, `your-gpu-box.lan`. `settings` when there is no server.
// Characters Windows refuses in a file name become `_`.
std::string suggested_settings_file_name(const Settings& s);

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

// The open dialog's HWND, as an opaque value, or 0 when there is none.
//
// For the caller's "Settings…" handler, which can be reached again while the dialog is already
// up — from the tray menu, or from a second launch of the executable. The answer to all of them
// is to raise the one dialog that exists, and this is how the caller finds it.
//
// GetLastActivePopup on the owner is the usual way and does not work here: the owner is a
// *message-only* window (main.cpp), which is not on the desktop and tracks no popups, so it
// returns itself and the caller concludes there is nothing to raise. That was the bug — the
// tray's Settings item looked dead whenever the dialog was behind another window.
//
// Valid only on the thread that opened the dialog, which is the only thread that may touch it.
uintptr_t open_settings_dialog_window() noexcept;

// The WM_COMMAND id the dialog's "Check for updates now" button posts to its owner, which is
// main.cpp's window and also the id of its tray-menu item. The dialog stays open; the update
// flow (updater.h) runs on its own thread with its own dialogs.
inline constexpr unsigned kCommandCheckForUpdates = 108;

}  // namespace kd
