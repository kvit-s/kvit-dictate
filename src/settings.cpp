// Settings persistence, the models.json manifest, and the settings dialog.
//
// Specification: settings.h, which also carries the models.json schema (JSON cannot carry
// comments, so the schema lives next to the parser that enforces it). plan.md §1.1 (the manifest
// is on the C++ side), §1.4 (stability and cadence), §1.6 (the two silence timers), §1.7
// (injection method, per-app policy), §1.8 (hotkey), §1.9 (privacy defaults).
//
// ─── Three rules this file is built around ──────────────────────────────────────────────────
//
// 1. **The privacy defaults are policy, not preference.** They are stated once, in
//    `Settings{}`'s member initialisers and in the block below, and nothing here may quietly
//    relax them: no transcripts or audio persisted, no diagnostics file, no network access of
//    any kind (this file opens no socket and neither does anything it calls), and no model
//    auto-download — the manifest deliberately has no URL field to download *from*.
//
// 2. **A missing settings file is a first run; a malformed one is an error.** Silently reverting
//    to defaults is indistinguishable from losing the user's configuration, so a parse failure
//    is returned to the caller with the offending field named, and main.cpp refuses to overwrite
//    a file it could not read.
//
// 3. **The write is atomic.** Temp file in the same directory, FlushFileBuffers, then MoveFileEx
//    with MOVEFILE_REPLACE_EXISTING. Without the flush the rename can publish a file whose
//    contents are still in the cache, which is exactly the crash-mid-save case the atomic write
//    exists to survive.
//
// ─── On the dialog, and plan.md §1.3's Qt escape hatch ──────────────────────────────────────
//
// plan.md §1.3 allows Qt Widgets for the settings dialog alone "if the settings dialog and tray
// plumbing become a slog". They did not become a slog, but they were not free either: the
// programmatic-control approach below is roughly 400 lines to lay out and read back ~30 knobs,
// and it has no data binding, no validation framework and no scrolling. That is a fair price for
// keeping the dependency count at zero and the binary at a few hundred KB. The honest limit is
// recorded here rather than discovered: if this dialog grows a third page's worth of knobs again,
// or needs a live device/model list that updates while it is open, revisit the decision — the
// hot path (hooks, capture, injection, overlay) must stay native either way.

#define KD_LOG_MODULE "settings"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <commctrl.h>
#include <shlobj.h>
#include <uxtheme.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "settings.h"

#include "audio_capture.h"
#include "theme.h"
#include "util/json.h"
#include "util/log.h"
#include "util/utf8.h"

namespace kd {
namespace {

// ─── Sizes ──────────────────────────────────────────────────────────────────────────────────

// json.h's default max_bytes is the protocol's 64 KiB frame cap, which is the right bound for a
// control frame and the wrong one for a local file the user edits by hand: models.json already
// carries multi-paragraph `notes` per entry. Raised deliberately and still bounded — an
// unbounded parse of a file that a bad tool truncated into gigabytes is still a hang.
constexpr size_t kManifestMaxBytes = 4u * 1024u * 1024u;
constexpr size_t kSettingsMaxBytes = 1u * 1024u * 1024u;

// ─── Path <-> UTF-8 ─────────────────────────────────────────────────────────────────────────
//
// std::filesystem::path is wchar_t-native on Windows, and everything above the Win32 boundary in
// this program is UTF-8 (plan.md §1.2). path::string() would convert through the *active code
// page* and silently mangle any non-ASCII path, so the conversion goes through utf8.h — which is
// the only place in the project allowed to cross this boundary.

std::string path_to_utf8(const std::filesystem::path& p) {
    return utf8::from_utf16_or_empty(p.native());
}

std::filesystem::path path_from_utf8(std::string_view s) {
    std::wstring w;
    if (!utf8::to_utf16(s, w)) return {};  // malformed UTF-8: caller reports it as a field fault
    return std::filesystem::path(w);
}

bool file_exists(const std::filesystem::path& p) {
    if (p.empty()) return false;
    std::error_code ec;  // the throwing overload would turn "the drive is offline" into a crash
    return std::filesystem::is_regular_file(p, ec);
}

// ─── A small strict object reader ───────────────────────────────────────────────────────────
//
// Absent optional field  -> leave the struct's default in place. This is what makes an older
//                           settings file readable by a newer build.
// Present but wrong type -> a fault naming the field. Silently ignoring a field the user clearly
//                           meant to set is how a settings dialog becomes untrustworthy.
//
// The FIRST fault is kept, not the last: it is the one closest to what the user typed.
class Obj {
public:
    Obj(const json::Value& v, std::string ctx) : v_(&v), ctx_(std::move(ctx)) {}

    [[nodiscard]] bool failed() const noexcept { return !err_.empty(); }
    [[nodiscard]] const std::string& error() const noexcept { return err_; }

    void fail(std::string_view key, std::string_view what) {
        if (!err_.empty()) return;
        err_ = ctx_ + "." + std::string(key) + ": " + std::string(what);
    }

    [[nodiscard]] const json::Value* member(const char* key) const { return v_->find(key); }

    bool str_req(const char* key, std::string& out) {
        const json::Value* v = member(key);
        if (!v) {
            fail(key, "required field is missing");
            return false;
        }
        std::string_view sv;
        if (!v->get_string(sv)) {
            fail(key, "expected a string");
            return false;
        }
        out.assign(sv);
        return true;
    }

    void str_opt(const char* key, std::string& out) {
        const json::Value* v = member(key);
        if (!v) return;
        std::string_view sv;
        if (!v->get_string(sv)) {
            fail(key, "expected a string");
            return;
        }
        out.assign(sv);
    }

    void bool_opt(const char* key, bool& out) {
        const json::Value* v = member(key);
        if (!v) return;
        bool b = false;
        if (!v->get_bool(b)) {
            fail(key, "expected true or false");
            return;
        }
        out = b;
    }

    void u64_opt(const char* key, uint64_t& out, uint64_t lo, uint64_t hi) {
        const json::Value* v = member(key);
        if (!v) return;
        int64_t n = 0;
        if (!v->get_int(n)) {
            fail(key, "expected an integer");
            return;
        }
        if (n < 0 || static_cast<uint64_t>(n) < lo || static_cast<uint64_t>(n) > hi) {
            fail(key, "must be between " + std::to_string(lo) + " and " + std::to_string(hi));
            return;
        }
        out = static_cast<uint64_t>(n);
    }

    void u32_opt(const char* key, uint32_t& out, uint32_t lo, uint32_t hi) {
        uint64_t value64 = out;
        u64_opt(key, value64, lo, hi);
        out = static_cast<uint32_t>(value64);
    }

    void f32_opt(const char* key, float& out, float lo, float hi) {
        const json::Value* v = member(key);
        if (!v) return;
        double d = 0.0;
        int64_t n = 0;
        if (v->get_double(d)) {
            // fall through
        } else if (v->get_int(n)) {
            d = static_cast<double>(n);  // "14" is a legal spelling of 14.0 in a hand-edited file
        } else {
            fail(key, "expected a number");
            return;
        }
        if (d < static_cast<double>(lo) || d > static_cast<double>(hi)) {
            fail(key, "out of range");
            return;
        }
        out = static_cast<float>(d);
    }

    // Colours are 0xAARRGGBB written as "#AARRGGBB". A JSON number would work and would be
    // unreadable: nobody recognises 3858759678 as a colour, and 0xE6FFFFFF exceeds int32 so a
    // careless reader on the other side of a round trip could sign-extend it.
    void color_opt(const char* key, uint32_t& out) {
        const json::Value* v = member(key);
        if (!v) return;
        std::string_view sv;
        if (!v->get_string(sv)) {
            fail(key, "expected a colour string like \"#AARRGGBB\"");
            return;
        }
        if (!sv.empty() && sv.front() == '#') sv.remove_prefix(1);
        if (sv.size() != 8) {
            fail(key, "expected 8 hex digits: \"#AARRGGBB\"");
            return;
        }
        uint32_t value = 0;
        for (char c : sv) {
            uint32_t digit;
            if (c >= '0' && c <= '9') digit = static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<uint32_t>(c - 'a') + 10u;
            else if (c >= 'A' && c <= 'F') digit = static_cast<uint32_t>(c - 'A') + 10u;
            else {
                fail(key, "contains a non-hex digit");
                return;
            }
            value = (value << 4) | digit;
        }
        out = value;
    }

    void strings_opt(const char* key, std::vector<std::string>& out, size_t max_items,
                     size_t max_item_bytes) {
        const json::Value* v = member(key);
        if (!v) return;
        if (!v->is_array()) {
            fail(key, "expected an array of strings");
            return;
        }
        if (v->size() > max_items) {
            fail(key, "at most " + std::to_string(max_items) + " entries");
            return;
        }
        std::vector<std::string> parsed;
        parsed.reserve(v->size());
        for (size_t i = 0; i < v->size(); ++i) {
            const json::Value* e = v->at(i);
            std::string_view sv;
            if (!e || !e->get_string(sv)) {
                fail(key, "element " + std::to_string(i) + " is not a string");
                return;
            }
            if (max_item_bytes && sv.size() > max_item_bytes) {
                fail(key, "element " + std::to_string(i) + " exceeds " +
                              std::to_string(max_item_bytes) + " bytes");
                return;
            }
            parsed.emplace_back(sv);
        }
        out = std::move(parsed);
    }

private:
    const json::Value* v_;
    std::string ctx_;
    std::string err_;
};

// ─── Serialisation helpers ──────────────────────────────────────────────────────────────────
//
// Every numeric setter casts explicitly: json::Value::number is overloaded on int64_t and double
// and an unsuffixed integer literal is ambiguous between them.

void put_str(json::Value& o, const char* k, std::string v) {
    o.set(k, json::Value::string(std::move(v)));
}
void put_bool(json::Value& o, const char* k, bool v) { o.set(k, json::Value::boolean(v)); }
void put_u64(json::Value& o, const char* k, uint64_t v) {
    o.set(k, json::Value::number(static_cast<int64_t>(v)));
}
void put_f32(json::Value& o, const char* k, float v) {
    o.set(k, json::Value::number(static_cast<double>(v)));
}
void put_color(json::Value& o, const char* k, uint32_t argb) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%08X", argb);
    o.set(k, json::Value::string(std::string(buf)));
}
void put_strings(json::Value& o, const char* k, const std::vector<std::string>& v) {
    json::Value arr = json::Value::array();
    for (const std::string& s : v) arr.push_back(json::Value::string(s));
    o.set(k, std::move(arr));
}

// ─── Enum spellings owned by this file ──────────────────────────────────────────────────────
//
// OverlayMode, Cadence and anchor::Fallback each declare a to_string() in their own header but no
// inverse. Using half of a pair means the settings file's spelling would be defined by one module
// and its parser by another, and a rename on that side would silently reset a user's setting to
// the default on the next load. So both directions live here, in the file that owns the format.
//
// The three enums that DO declare an inverse in the same header as their to_string — InjectMethod
// (inject_method_from_string), log::Level (parse_level), proto::StreamingMode
// (streaming_mode_from_string) — use that pair, because there the two halves cannot drift apart.
//
// See the return note: `overlay_mode_from_string` / `cadence_from_string` /
// `fallback_from_string` would be a welcome addition to those headers, at which point these
// tables should be deleted rather than kept in parallel.

struct EnumName {
    const char* text;
    int value;
};

constexpr EnumName kOverlayModes[] = {
    {"hidden", static_cast<int>(OverlayMode::Hidden)},
    {"inline", static_cast<int>(OverlayMode::Inline)},
    {"floating-pill", static_cast<int>(OverlayMode::FloatingPill)},
    {"status-only", static_cast<int>(OverlayMode::StatusOnly)},
};

constexpr EnumName kThemeModes[] = {
    {"system", static_cast<int>(ThemeMode::System)},
    {"light", static_cast<int>(ThemeMode::Light)},
    {"dark", static_cast<int>(ThemeMode::Dark)},
    {"custom", static_cast<int>(ThemeMode::Custom)},
};

constexpr EnumName kCadences[] = {
    {"immediate", static_cast<int>(Cadence::Immediate)},
    {"periodic", static_cast<int>(Cadence::Periodic)},
    {"end-of-session", static_cast<int>(Cadence::EndOfSession)},
};

// The cadence combo's item order, in one place. The dialog populates, reads back and enables the
// interval field off this, so a reordered combo cannot silently start meaning something else --
// which is what a bare `CB_GETCURSEL == 1` was one edit away from.
constexpr Cadence kCadenceOrder[] = {Cadence::Immediate, Cadence::Periodic, Cadence::EndOfSession};

inline int cadence_index(Cadence c) noexcept {
    for (int i = 0; i < static_cast<int>(std::size(kCadenceOrder)); ++i) {
        if (kCadenceOrder[i] == c) return i;
    }
    return 0;
}

inline Cadence cadence_at(LRESULT sel) noexcept {
    if (sel < 0 || sel >= static_cast<LRESULT>(std::size(kCadenceOrder))) return Cadence::Immediate;
    return kCadenceOrder[sel];
}

// The colour-scheme combo's item order, on the same principle as the cadence one above: the
// dialog populates it, reads it back and enables the three colour fields off this one array.
constexpr ThemeMode kThemeOrder[] = {ThemeMode::System, ThemeMode::Light, ThemeMode::Dark,
                                     ThemeMode::Custom};

inline int theme_index(ThemeMode m) noexcept {
    for (int i = 0; i < static_cast<int>(std::size(kThemeOrder)); ++i) {
        if (kThemeOrder[i] == m) return i;
    }
    return 0;
}

inline ThemeMode theme_at(LRESULT sel) noexcept {
    if (sel < 0 || sel >= static_cast<LRESULT>(std::size(kThemeOrder))) return ThemeMode::System;
    return kThemeOrder[sel];
}

constexpr EnumName kFallbacks[] = {
    {"floating-pill", static_cast<int>(anchor::Fallback::FloatingPill)},
    {"no-preview", static_cast<int>(anchor::Fallback::NoPreview)},
};

// The settings-file spelling of a bare (unmodified) virtual key. Routed through HotkeySpec so
// that "enter" means exactly what it means in the `hotkey` string — the two are parsed by the
// same table and cannot drift apart — and lowercased because that is the spelling docs/editing.md
// §12 writes and JSON keys elsewhere in this file are lower case too.
std::string vk_setting_name(uint32_t v) {
    HotkeySpec spec;
    spec.vk = v;
    spec.mods = kModNone;
    std::string s = spec.to_display_string();
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

template <size_t N>
const char* enum_to_text(const EnumName (&table)[N], int value, const char* fallback) {
    for (const EnumName& e : table) {
        if (e.value == value) return e.text;
    }
    return fallback;
}

template <size_t N>
bool enum_from_text(const EnumName (&table)[N], std::string_view s, int& out) {
    for (const EnumName& e : table) {
        if (s == e.text) {
            out = e.value;
            return true;
        }
    }
    return false;
}

// ─── models.json entry parsing ──────────────────────────────────────────────────────────────

bool is_valid_model_id(std::string_view id) noexcept {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '.' || c == '-';
        if (!ok) return false;
    }
    return true;
}

Status parse_model_entry(const json::Value& v, size_t index, ModelEntry& out) {
    const std::string ctx = "models[" + std::to_string(index) + "]";
    if (!v.is_object()) return Status{StatusCode::Malformed, ctx + " is not an object"};

    Obj o(v, ctx);

    o.str_req("id", out.id);
    if (!o.failed() && !is_valid_model_id(out.id)) {
        o.fail("id", "must be 1-64 characters from [A-Za-z0-9_.-]");
    }

    o.str_req("display_name", out.display_name);

    std::string interpreter;
    if (o.str_req("interpreter", interpreter)) {
        std::filesystem::path p = path_from_utf8(interpreter);
        if (p.empty()) {
            o.fail("interpreter", "is empty or is not well-formed UTF-8");
        } else if (!p.is_absolute()) {
            // plan.md §5.1's whole point is that different models run under different, non-default
            // interpreters. Resolving through PATH would silently pick whichever python.exe
            // happened to be first, which is the one failure mode this manifest exists to prevent.
            o.fail("interpreter",
                   "must be an ABSOLUTE path to a python.exe (got \"" + interpreter +
                       "\"); a bare name would be resolved through PATH, which defeats the "
                       "point of a per-model interpreter");
        } else {
            out.interpreter = std::move(p);
        }
    }

    o.str_opt("module", out.module);
    o.strings_opt("args", out.args, 256, 0);

    std::string mode;
    if (o.str_req("streaming_mode", mode)) {
        proto::StreamingMode parsed{};
        if (!proto::streaming_mode_from_string(mode, parsed)) {
            o.fail("streaming_mode",
                   "must be \"native-streaming\", \"stateful-chunked\" or \"pseudo-streaming\" "
                   "(got \"" + mode + "\")");
        } else {
            out.streaming_mode = parsed;
        }
    }

    // The JSON key is "requires"; the member is `requires_` because `requires` is a C++20 keyword.
    // This is the single most likely place in the file to introduce a silent mismatch.
    o.strings_opt("requires", out.requires_, 64, 0);

    o.str_opt("notes", out.notes);
    o.bool_opt("enabled", out.enabled);

    if (const json::Value* env = o.member("env")) {
        if (!env->is_object()) {
            o.fail("env", "expected an object of NAME -> value strings");
        } else {
            for (size_t i = 0; i < env->size(); ++i) {
                const json::Value* value = env->value_at(i);
                std::string_view sv;
                if (!value || !value->get_string(sv)) {
                    o.fail("env", "value of \"" + env->key_at(i) + "\" is not a string");
                    break;
                }
                out.env.emplace_back(env->key_at(i), std::string(sv));
            }
        }
    }

    if (o.failed()) return Status{StatusCode::Malformed, o.error()};
    return Status::success();
}

// ─── Atomic file write ──────────────────────────────────────────────────────────────────────

// RAII, because every path out of write_file_atomic() — including the failure paths — must close
// the handle, and a CloseHandle at the bottom of a function with five early returns closes it on
// exactly one of them.
class FileHandle {
public:
    explicit FileHandle(HANDLE h) noexcept : h_(h) {}
    ~FileHandle() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    [[nodiscard]] bool valid() const noexcept { return h_ != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HANDLE get() const noexcept { return h_; }
    void close() noexcept {
        if (h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE h_;
};

Status write_file_atomic(const std::filesystem::path& file, const std::string& bytes) {
    std::error_code ec;
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path(), ec);
        if (ec) {
            return Status{StatusCode::OsError,
                          "could not create " + path_to_utf8(file.parent_path()) + ": " +
                              ec.message()};
        }
    }

    // The temp file must live in the SAME directory as the target: MoveFileEx can only be atomic
    // within one volume, and %TEMP% is frequently on a different one.
    std::filesystem::path tmp = file;
    tmp += L".tmp-" + std::to_wstring(GetCurrentProcessId());

    {
        FileHandle h(CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!h.valid()) {
            return Status{StatusCode::OsError, "could not create " + path_to_utf8(tmp),
                          GetLastError()};
        }
        const char* p = bytes.data();
        DWORD remaining = static_cast<DWORD>(bytes.size());
        while (remaining > 0) {
            DWORD written = 0;
            if (!WriteFile(h.get(), p, remaining, &written, nullptr) || written == 0) {
                const DWORD err = GetLastError();
                h.close();
                DeleteFileW(tmp.c_str());
                return Status{StatusCode::OsError, "write to " + path_to_utf8(tmp) + " failed",
                              err};
            }
            p += written;
            remaining -= written;
        }
        // Without this the rename below can publish a file whose contents are still only in the
        // cache. A crash between the two would then leave a settings.json of the right length and
        // the wrong (or zero) contents — precisely the outcome the atomic write exists to avoid.
        if (!FlushFileBuffers(h.get())) {
            const DWORD err = GetLastError();
            h.close();
            DeleteFileW(tmp.c_str());
            return Status{StatusCode::OsError, "flush of " + path_to_utf8(tmp) + " failed", err};
        }
    }

    if (!MoveFileExW(tmp.c_str(), file.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD err = GetLastError();
        DeleteFileW(tmp.c_str());
        return Status{StatusCode::OsError, "could not replace " + path_to_utf8(file), err};
    }
    return Status::success();
}

Result<std::string> read_file(const std::filesystem::path& file, size_t max_bytes) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        return Status{StatusCode::NotFound, path_to_utf8(file) + " does not exist"};
    }
    const uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec) {
        return Status{StatusCode::OsError, "could not stat " + path_to_utf8(file)};
    }
    if (size > max_bytes) {
        return Status{StatusCode::Malformed,
                      path_to_utf8(file) + " is " + std::to_string(size) +
                          " bytes, above the " + std::to_string(max_bytes) + " byte limit"};
    }

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return Status{StatusCode::OsError, "could not open " + path_to_utf8(file)};
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        return Status{StatusCode::OsError, "read of " + path_to_utf8(file) + " failed"};
    }

    // A BOM is not part of the JSON grammar and json.h rejects it. Notepad writes one; the user
    // editing models.json by hand should not have to know that.
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }
    return bytes;
}

std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    // KF_FLAG_CREATE: on a fresh profile the folder may not exist yet, and a settings path under a
    // directory that does not exist is a save failure the user cannot act on.
    const HRESULT hr = SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw);
    std::filesystem::path result;
    if (SUCCEEDED(hr) && raw) result = raw;
    if (raw) CoTaskMemFree(raw);
    return result;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  models.json
// ═══════════════════════════════════════════════════════════════════════════════════════════

bool ModelEntry::interpreter_available() const { return file_exists(interpreter); }

const ModelEntry* ModelManifest::find(std::string_view id) const {
    for (const ModelEntry& e : models) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

Result<ModelManifest> parse_model_manifest(std::string_view json_text) {
    json::Value root;
    json::Error jerr;
    json::Options opts;
    opts.max_bytes = kManifestMaxBytes;

    if (!json::parse_object(json_text, root, jerr, opts)) {
        std::string msg = "models.json is not a valid JSON object: ";
        msg += json::to_string(jerr.code);
        msg += " at byte " + std::to_string(jerr.offset);
        if (!jerr.detail.empty()) msg += " (" + jerr.detail + ")";
        return Status{StatusCode::Malformed, std::move(msg)};
    }

    ModelManifest m;

    const json::Value* schema = root.find("schema");
    int64_t schema_value = 0;
    if (!schema || !schema->get_int(schema_value)) {
        return Status{StatusCode::Malformed,
                      "models.json: \"schema\" is required and must be an integer"};
    }
    if (schema_value != kModelManifestSchema) {
        // Refused rather than best-effort parsed. A future schema may give an existing key a new
        // meaning, and launching a child under an interpreter chosen by a misread manifest is a
        // worse outcome than refusing to launch at all.
        return Status{StatusCode::Malformed,
                      "models.json: schema " + std::to_string(schema_value) +
                          " is not supported; this build understands schema " +
                          std::to_string(kModelManifestSchema)};
    }
    m.schema = static_cast<int>(schema_value);

    {
        Obj top(root, "models.json");
        top.str_opt("default_model", m.default_model);
        if (top.failed()) return Status{StatusCode::Malformed, top.error()};
    }

    const json::Value* models = root.find("models");
    if (!models || !models->is_array()) {
        return Status{StatusCode::Malformed, "models.json: \"models\" must be an array"};
    }

    for (size_t i = 0; i < models->size(); ++i) {
        const json::Value* entry = models->at(i);
        if (!entry) continue;
        ModelEntry parsed;
        Status st = parse_model_entry(*entry, i, parsed);
        if (!st.is_ok()) return st;
        if (m.find(parsed.id) != nullptr) {
            return Status{StatusCode::Malformed,
                          "models.json: duplicate model id \"" + parsed.id + "\""};
        }
        m.models.push_back(std::move(parsed));
    }

    if (m.models.empty()) {
        return Status{StatusCode::Malformed, "models.json: \"models\" is empty"};
    }

    if (!m.default_model.empty() && m.find(m.default_model) == nullptr) {
        return Status{StatusCode::Malformed, "models.json: default_model \"" + m.default_model +
                                                 "\" names no entry in \"models\""};
    }

    return m;
}

Result<ModelManifest> load_model_manifest(const std::filesystem::path& file) {
    Result<std::string> bytes = read_file(file, kManifestMaxBytes);
    if (!bytes) return bytes.status();

    Result<ModelManifest> parsed = parse_model_manifest(*bytes);
    if (!parsed) {
        // Prefix the path: "models.json: ..." is ambiguous the moment there is one beside the
        // executable and one in the source tree.
        return Status{parsed.status().code(),
                      path_to_utf8(file) + " — " + parsed.status().message()};
    }
    return parsed;
}

std::string serialize_model_manifest(const ModelManifest& m) {
    json::Value root = json::Value::object();
    put_u64(root, "schema", static_cast<uint64_t>(m.schema));
    put_str(root, "default_model", m.default_model);

    json::Value models = json::Value::array();
    for (const ModelEntry& e : m.models) {
        json::Value o = json::Value::object();
        put_str(o, "id", e.id);
        put_str(o, "display_name", e.display_name);
        put_str(o, "interpreter", path_to_utf8(e.interpreter));
        put_str(o, "module", e.module);
        put_strings(o, "args", e.args);
        put_str(o, "streaming_mode", proto::to_string(e.streaming_mode));
        put_strings(o, "requires", e.requires_);  // JSON key "requires", member `requires_`

        json::Value env = json::Value::object();
        for (const auto& kv : e.env) put_str(env, kv.first.c_str(), kv.second);
        o.set("env", std::move(env));

        put_str(o, "notes", e.notes);
        put_bool(o, "enabled", e.enabled);
        models.push_back(std::move(o));
    }
    root.set("models", std::move(models));

    return json::serialize_pretty(root, 2);
}

Status check_model_launchable(const ModelManifest& m, std::string_view model_id) {
    if (model_id.empty()) {
        return Status{StatusCode::InvalidArgument, "no model is selected"};
    }
    const ModelEntry* e = m.find(model_id);
    if (!e) {
        return Status{StatusCode::NotFound, "models.json lists no model with id \"" +
                                                std::string(model_id) + "\""};
    }
    if (!e->enabled) {
        return Status{StatusCode::Unavailable,
                      "model \"" + e->display_name + "\" is disabled in models.json"};
    }
    if (!e->interpreter_available()) {
        // The clear error the task asks for, assembled once here so every caller reports the same
        // thing: which model, which absolute path, and what would have to be installed. plan.md
        // §1.9 forbids doing anything about it automatically — no download, no pip install — so
        // the only useful response is an accurate sentence.
        std::string msg = "model \"" + e->display_name + "\" cannot start: its interpreter " +
                          path_to_utf8(e->interpreter) + " does not exist.";
        if (!e->requires_.empty()) {
            msg += " That environment needs:";
            for (size_t i = 0; i < e->requires_.size(); ++i) {
                msg += (i == 0 ? " " : ", ");
                msg += e->requires_[i];
            }
            msg += ".";
        }
        msg +=
            " Create it yourself (plan.md §5.2 creates environments lazily and per backend); "
            "kvit-dictate never installs or downloads anything at runtime.";
        return Status{StatusCode::NotFound, std::move(msg)};
    }
    return Status::success();
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  Settings
// ═══════════════════════════════════════════════════════════════════════════════════════════
//
// ─── settings.json — schema version 1 ───────────────────────────────────────────────────────
//
//   {
//     "schema": 1,
//     "model_id": "mock",
//     "hotkey": "Win+Y",                                  // HotkeySpec::to_display_string()
//     "session":   { "endpoint_silence_ms": 800,          // plan.md §1.6 — ends an UTTERANCE
//                    "session_silence_ms": 120000,        //            — ends the SESSION
//                    "session_silence_never": false,
//                    "activation_grace_ms": 250 },
//     "stability": { "hypotheses_n": 2, "retract_clusters_m": 3, "hold_ms_t": 200,
//                    "honour_server_stable_bytes": true },
//     "cadence":   { "mode": "immediate", "ms": 0 },      // flush timing only, never eligibility
//     "inject":    { "method": "SendInput", "chars_per_batch": 32, "inter_batch_delay_us": 1000,
//                    "paste_settle_ms": 60, "refuse_password_fields": true,
//                    "refuse_elevated_targets": true, "paste_ctrl_shift_v_default": false },
//     "app_policies": [ { "process_name": "windowsterminal.exe", "method": "Clipboard",
//                         "use_ctrl_shift_v": true, "supported": true, "note": "" } ],
//     "anchor":    { "min_confidence": 0.5, "below_threshold": "floating-pill", ... },
//     "editing":   { "enabled": true, "mouse": true,      // docs/editing.md §12
//                    "on_foreign_key": "ignore", "commit_key": "enter" },
//     "overlay":   { "mode": "inline", "theme": "system",   // system | light | dark | custom
//                    "font_family": "Segoe UI", "font_size_dip": 14.0,
//                    "color_pending": "#FF3C3C3C", ... },   // read only under theme "custom"
//     "audio_device_id": "",
//     "privacy":   { "log_level": "info", "diagnostics_file_enabled": false,
//                    "transcript": { "enabled": false, "file": "", "max_bytes": 1048576,
//                                    "max_age_days": 7 } },
//     "start_with_windows": false,
//     "context":   { "send": true, "terms": [ "llama.cpp", "Qwen3" ] }
//   }
//
// **Cadence is a sibling of stability, not a member of it**, even though StabilityParams carries
// both. The nesting is deliberate: plan.md §1.4 separates eligibility from cadence, and a file in
// which `cadence` sits inside `stability` invites exactly the merge the plan spent a revision
// undoing.

void Settings::normalise() {
    // The settings file is the single source; SessionConfig::context is derived. One place to
    // edit, one place to read.
    session.context = context_terms;
    session.send_context = send_context && !context_terms.empty();

    // Defensive clamps against the protocol's own limits (docs/protocol.md §6.1). validate()
    // reports these as errors so the user sees them; this clamp exists so that a Settings built
    // programmatically — by the bench harness, say — can never produce an illegal `start`.
    if (session.context.size() > proto::kMaxContextItems) {
        session.context.resize(proto::kMaxContextItems);
    }
    session.context.erase(
        std::remove_if(session.context.begin(), session.context.end(),
                       [](const std::string& s) {
                           return s.empty() || s.size() > proto::kMaxContextItemBytes;
                       }),
        session.context.end());

    // Cadence::Immediate ignores cadence_ms; zeroing it keeps the serialised file from carrying a
    // number that has no effect and would read as if it did.
    if (stability.cadence == Cadence::Immediate) stability.cadence_ms = 0;
    // EndOfSession has no interval either: "when the session ends" is not a number of ms.
    if (stability.cadence == Cadence::EndOfSession) stability.cadence_ms = 0;
    if (stability.cadence == Cadence::Periodic && stability.cadence_ms == 0) {
        stability.cadence_ms = 500;
    }

    // plan.md §1.9 is a promise about *persistence*, so the transcript file only gets a path when
    // the user has actually opted in. An unconditional default path would put a filename in the
    // settings file that reads like a file that exists.
    if (transcript.enabled && transcript.file.empty()) {
        transcript.file = default_log_directory() / L"transcript.log";
    }

    if (overlay_style.font_family.empty()) overlay_style.font_family = "Segoe UI";

    // docs/editing.md §6.1 and §16's second bullet. The compose buffer only exists under
    // `end-of-session`: with any other cadence the committed text has already been typed into
    // the document, so there is nothing left in the pill for Enter to commit or for Backspace to
    // remove. Forced here rather than checked at each use, so that every reader of
    // `editing.enabled` — the input filter, the overlay, the settings dialog — sees one answer.
    if (stability.cadence != Cadence::EndOfSession) editing.enabled = false;

    // The commit key is offered as "enter" and nothing else. A file that names some other key
    // still round-trips, but a file that names *no* key would silently disable the one gesture
    // that ends a composing session.
    if (editing.commit_vk == 0) editing.commit_vk = vk::kReturn;
}

Status Settings::validate() const {
    if (schema != kSettingsSchema) {
        return Status{StatusCode::Malformed,
                      "settings schema " + std::to_string(schema) + " is not supported; this "
                      "build understands schema " + std::to_string(kSettingsSchema)};
    }

    // plan.md §1.4: the speculative mode "is named unsafe, excluded from defaults, and never
    // shipped". It knowingly commits ahead of stability, i.e. it knowingly corrupts documents, so
    // the settings loader refuses it outright rather than honouring it. The bench harness sets
    // StabilityParams directly and never comes through here.
    if (stability.speculative_unsafe) {
        return Status{StatusCode::Unsupported,
                      "stability.speculative_unsafe is a bench-harness-only mode that commits "
                      "text ahead of stability (plan.md §1.4). It is never enabled from a "
                      "settings file."};
    }

    if (hotkey.vk == 0) {
        return Status{StatusCode::InvalidArgument, "hotkey: no key is bound"};
    }

    // The endpoint timer ends an utterance; the session timer ends the session. They are unrelated
    // (plan.md §1.6) and the only relationship worth enforcing is that an endpoint longer than the
    // session timeout can never fire, which is a configuration that silently does nothing.
    if (session.endpoint_silence_ms < 100 || session.endpoint_silence_ms > 10000) {
        return Status{StatusCode::InvalidArgument,
                      "session.endpoint_silence_ms must be between 100 and 10000 (default 800)"};
    }
    if (!session.session_silence_never) {
        if (session.session_silence_ms < 1000 || session.session_silence_ms > 24u * 3600u * 1000u) {
            return Status{StatusCode::InvalidArgument,
                          "session.session_silence_ms must be between 1000 and 86400000, or use "
                          "session_silence_never (plan.md §1.6 permits `never`)"};
        }
        if (session.session_silence_ms < session.endpoint_silence_ms) {
            return Status{StatusCode::InvalidArgument,
                          "session.session_silence_ms is below endpoint_silence_ms: the session "
                          "would end before an utterance could ever finish"};
        }
    }

    if (stability.hypotheses_n < 1 || stability.hypotheses_n > 8) {
        return Status{StatusCode::InvalidArgument, "stability.hypotheses_n must be 1..8 (N)"};
    }
    if (stability.retract_clusters_m > 64) {
        return Status{StatusCode::InvalidArgument, "stability.retract_clusters_m must be 0..64 (M)"};
    }
    if (stability.hold_ms_t > 5000) {
        return Status{StatusCode::InvalidArgument, "stability.hold_ms_t must be 0..5000 ms (T)"};
    }
    if (stability.cadence == Cadence::Periodic &&
        (stability.cadence_ms < 50 || stability.cadence_ms > 5000)) {
        return Status{StatusCode::InvalidArgument,
                      "cadence.ms must be 50..5000 when cadence.mode is \"periodic\""};
    }

    if (inject.chars_per_batch == 0 || inject.chars_per_batch > 4096) {
        return Status{StatusCode::InvalidArgument, "inject.chars_per_batch must be 1..4096"};
    }
    if (inject.paste_settle_ms > 5000) {
        return Status{StatusCode::InvalidArgument, "inject.paste_settle_ms must be 0..5000"};
    }
    for (const AppPolicy& p : app_policies) {
        if (p.process_name.empty()) {
            return Status{StatusCode::InvalidArgument,
                          "app_policies: an entry has an empty process_name"};
        }
    }

    if (anchor.min_confidence < 0.0f || anchor.min_confidence > 1.0f) {
        return Status{StatusCode::InvalidArgument, "anchor.min_confidence must be 0.0..1.0"};
    }
    if (overlay_style.font_size_dip < 6.0f || overlay_style.font_size_dip > 96.0f) {
        return Status{StatusCode::InvalidArgument, "overlay.font_size_dip must be 6..96"};
    }

    if (context_terms.size() > proto::kMaxContextItems) {
        return Status{StatusCode::InvalidArgument,
                      "context.terms holds " + std::to_string(context_terms.size()) +
                          " entries; docs/protocol.md §6.1 allows at most " +
                          std::to_string(proto::kMaxContextItems)};
    }
    for (const std::string& t : context_terms) {
        if (t.size() > proto::kMaxContextItemBytes) {
            return Status{StatusCode::InvalidArgument,
                          "context.terms: \"" + t.substr(0, 32) + "…\" exceeds " +
                              std::to_string(proto::kMaxContextItemBytes) + " bytes"};
        }
    }

    if (transcript.enabled) {
        if (transcript.max_bytes == 0 || transcript.max_age_days == 0) {
            // plan.md §1.9 says transcript logging is "retention-bounded". An unbounded switch
            // dressed as a bound is worse than no switch.
            return Status{StatusCode::InvalidArgument,
                          "privacy.transcript: retention must be bounded — max_bytes and "
                          "max_age_days must both be greater than zero (plan.md §1.9)"};
        }
    }

    return Status::success();
}

// ─── Paths ──────────────────────────────────────────────────────────────────────────────────

std::filesystem::path default_settings_path() {
    std::filesystem::path base = known_folder(FOLDERID_LocalAppData);
    if (base.empty()) return {};
    return base / L"kvit-dictate" / L"settings.json";
}

std::filesystem::path default_log_directory() {
    std::filesystem::path base = known_folder(FOLDERID_LocalAppData);
    if (base.empty()) return {};
    return base / L"kvit-dictate" / L"logs";
}

std::filesystem::path executable_directory() {
    // GetModuleFileNameW's failure mode is a truncated path with ERROR_INSUFFICIENT_BUFFER and no
    // null terminator on some Windows versions, so the buffer grows until the call fits rather
    // than trusting MAX_PATH — long paths are ordinary on a machine with a deep source tree.
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size() - 1) {
            std::filesystem::path p(buf.data(), buf.data() + n);
            return p.parent_path();
        }
        if (buf.size() >= 32768) return {};  // the Win32 path ceiling; stop rather than spin
        buf.resize(buf.size() * 2);
    }
}

// ─── Settings serialisation ─────────────────────────────────────────────────────────────────

std::string serialize_settings(const Settings& s) {
    json::Value root = json::Value::object();
    put_u64(root, "schema", static_cast<uint64_t>(s.schema));
    put_str(root, "model_id", s.model_id);
    put_str(root, "hotkey", s.hotkey.to_display_string());

    {
        json::Value o = json::Value::object();
        put_u64(o, "endpoint_silence_ms", s.session.endpoint_silence_ms);
        put_u64(o, "session_silence_ms", s.session.session_silence_ms);
        put_bool(o, "session_silence_never", s.session.session_silence_never);
        put_u64(o, "activation_grace_ms", s.session.activation_grace_ms);
        root.set("session", std::move(o));
    }
    {
        json::Value o = json::Value::object();
        put_u64(o, "hypotheses_n", s.stability.hypotheses_n);
        put_u64(o, "retract_clusters_m", s.stability.retract_clusters_m);
        put_u64(o, "hold_ms_t", s.stability.hold_ms_t);
        put_bool(o, "honour_server_stable_bytes", s.stability.honour_server_stable_bytes);
        root.set("stability", std::move(o));
    }
    {
        json::Value o = json::Value::object();
        put_str(o, "mode",
                enum_to_text(kCadences, static_cast<int>(s.stability.cadence), "immediate"));
        put_u64(o, "ms", s.stability.cadence_ms);
        root.set("cadence", std::move(o));
    }
    {
        json::Value o = json::Value::object();
        put_str(o, "method", to_string(s.inject.method));
        put_u64(o, "chars_per_batch", s.inject.chars_per_batch);
        put_u64(o, "inter_batch_delay_us", s.inject.inter_batch_delay_us);
        put_u64(o, "paste_settle_ms", s.inject.paste_settle_ms);
        put_bool(o, "refuse_password_fields", s.inject.refuse_password_fields);
        put_bool(o, "refuse_elevated_targets", s.inject.refuse_elevated_targets);
        put_bool(o, "paste_ctrl_shift_v_default", s.paste_ctrl_shift_v_default);
        root.set("inject", std::move(o));
    }
    {
        json::Value arr = json::Value::array();
        for (const AppPolicy& p : s.app_policies) {
            json::Value o = json::Value::object();
            put_str(o, "process_name", p.process_name);
            put_str(o, "method", to_string(p.method));
            put_bool(o, "use_ctrl_shift_v", p.use_ctrl_shift_v);
            put_bool(o, "supported", p.supported);
            put_str(o, "note", p.note);
            arr.push_back(std::move(o));
        }
        root.set("app_policies", std::move(arr));
    }
    {
        json::Value o = json::Value::object();
        put_f32(o, "min_confidence", s.anchor.min_confidence);
        put_str(o, "below_threshold",
                enum_to_text(kFallbacks, static_cast<int>(s.anchor.below_threshold),
                             "floating-pill"));
        put_f32(o, "source_switch_margin", s.anchor.source_switch_margin);
        put_u64(o, "stale_after_ms", s.anchor.stale_after_ms);
        put_u64(o, "capture_width", static_cast<uint64_t>(s.anchor.capture_width));
        put_u64(o, "capture_height", static_cast<uint64_t>(s.anchor.capture_height));
        put_u64(o, "after_frame_poll_ms", s.anchor.after_frame_poll_ms);
        put_u64(o, "after_frame_poll_interval_ms", s.anchor.after_frame_poll_interval_ms);
        put_u64(o, "diff_tolerance", static_cast<uint64_t>(s.anchor.diff_tolerance));
        root.set("anchor", std::move(o));
    }
    {
        json::Value o = json::Value::object();
        put_bool(o, "enabled", s.editing.enabled);
        put_bool(o, "mouse", s.editing.mouse);
        put_str(o, "on_foreign_key", to_string(s.editing.on_foreign_key));
        put_str(o, "commit_key", vk_setting_name(s.editing.commit_vk));
        root.set("editing", std::move(o));
    }
    {
        json::Value o = json::Value::object();
        put_str(o, "mode", enum_to_text(kOverlayModes, static_cast<int>(s.overlay_mode), "inline"));
        put_str(o, "theme",
                enum_to_text(kThemeModes, static_cast<int>(s.overlay_style.theme), "system"));
        put_str(o, "font_family", s.overlay_style.font_family);
        put_f32(o, "font_size_dip", s.overlay_style.font_size_dip);
        put_color(o, "color_pending", s.overlay_style.color_pending);
        put_color(o, "color_unstable", s.overlay_style.color_unstable);
        put_color(o, "color_background", s.overlay_style.color_background);
        put_color(o, "color_warning", s.overlay_style.color_warning);
        put_color(o, "color_border", s.overlay_style.color_border);
        put_f32(o, "corner_radius_dip", s.overlay_style.corner_radius_dip);
        put_f32(o, "padding_dip", s.overlay_style.padding_dip);
        put_f32(o, "max_width_dip", s.overlay_style.max_width_dip);
        root.set("overlay", std::move(o));
    }

    put_str(root, "audio_device_id", s.audio_device_id);

    {
        json::Value o = json::Value::object();
        put_str(o, "log_level", log::to_string(s.log_level));
        put_bool(o, "diagnostics_file_enabled", s.diagnostics_file_enabled);
        json::Value t = json::Value::object();
        put_bool(t, "enabled", s.transcript.enabled);
        put_str(t, "file", path_to_utf8(s.transcript.file));
        put_u64(t, "max_bytes", s.transcript.max_bytes);
        put_u64(t, "max_age_days", s.transcript.max_age_days);
        o.set("transcript", std::move(t));
        root.set("privacy", std::move(o));
    }

    put_bool(root, "start_with_windows", s.start_with_windows);

    {
        json::Value o = json::Value::object();
        put_bool(o, "send", s.send_context);
        put_strings(o, "terms", s.context_terms);
        root.set("context", std::move(o));
    }

    return json::serialize_pretty(root, 2);
}

Result<Settings> parse_settings(std::string_view json_text) {
    json::Value root;
    json::Error jerr;
    json::Options opts;
    opts.max_bytes = kSettingsMaxBytes;

    if (!json::parse_object(json_text, root, jerr, opts)) {
        std::string msg = "settings.json is not a valid JSON object: ";
        msg += json::to_string(jerr.code);
        msg += " at byte " + std::to_string(jerr.offset);
        if (!jerr.detail.empty()) msg += " (" + jerr.detail + ")";
        return Status{StatusCode::Malformed, std::move(msg)};
    }

    Settings s;  // every field starts at its documented default, including the privacy defaults

    Obj top(root, "settings");
    {
        uint32_t schema = static_cast<uint32_t>(s.schema);
        top.u32_opt("schema", schema, 1, 1000);
        s.schema = static_cast<int>(schema);
        if (top.failed()) return Status{StatusCode::Malformed, top.error()};
        if (s.schema != kSettingsSchema) {
            return Status{StatusCode::Malformed,
                          "settings.json: schema " + std::to_string(s.schema) +
                              " is not supported; this build understands schema " +
                              std::to_string(kSettingsSchema)};
        }
    }

    top.str_opt("model_id", s.model_id);
    top.str_opt("audio_device_id", s.audio_device_id);
    top.bool_opt("start_with_windows", s.start_with_windows);
    if (top.failed()) return Status{StatusCode::Malformed, top.error()};

    {
        std::string spelled = s.hotkey.to_display_string();
        top.str_opt("hotkey", spelled);
        if (top.failed()) return Status{StatusCode::Malformed, top.error()};
        HotkeySpec parsed;
        if (!HotkeySpec::parse(spelled, parsed)) {
            return Status{StatusCode::Malformed,
                          "settings.hotkey: \"" + spelled +
                              "\" is not a binding this build understands"};
        }
        s.hotkey = parsed;
    }

    if (const json::Value* v = root.find("session")) {
        Obj o(*v, "settings.session");
        o.u32_opt("endpoint_silence_ms", s.session.endpoint_silence_ms, 0, 60000);
        o.u32_opt("session_silence_ms", s.session.session_silence_ms, 0, 24u * 3600u * 1000u);
        o.bool_opt("session_silence_never", s.session.session_silence_never);
        o.u32_opt("activation_grace_ms", s.session.activation_grace_ms, 0, 5000);
        if (o.failed()) return Status{StatusCode::Malformed, o.error()};
    }

    if (const json::Value* v = root.find("stability")) {
        Obj o(*v, "settings.stability");
        o.u32_opt("hypotheses_n", s.stability.hypotheses_n, 1, 8);
        o.u32_opt("retract_clusters_m", s.stability.retract_clusters_m, 0, 64);
        o.u32_opt("hold_ms_t", s.stability.hold_ms_t, 0, 5000);
        o.bool_opt("honour_server_stable_bytes", s.stability.honour_server_stable_bytes);
        if (o.failed()) return Status{StatusCode::Malformed, o.error()};
        // `speculative_unsafe` is deliberately NOT read. plan.md §1.4 excludes it from anything
        // that ships, and a key that is never read cannot be set by editing the file. validate()
        // still rejects it, for a Settings built in code.
    }

    if (const json::Value* v = root.find("cadence")) {
        Obj o(*v, "settings.cadence");
        std::string mode = enum_to_text(kCadences, static_cast<int>(s.stability.cadence),
                                        "immediate");
        o.str_opt("mode", mode);
        o.u32_opt("ms", s.stability.cadence_ms, 0, 5000);
        if (o.failed()) return Status{StatusCode::Malformed, o.error()};
        int value = 0;
        if (!enum_from_text(kCadences, mode, value)) {
            return Status{StatusCode::Malformed,
                          "settings.cadence.mode: expected \"immediate\" or \"periodic\", got \"" +
                              mode + "\""};
        }
        s.stability.cadence = static_cast<Cadence>(value);
    }

    if (const json::Value* v = root.find("inject")) {
        Obj o(*v, "settings.inject");
        std::string method = to_string(s.inject.method);
        o.str_opt("method", method);
        o.u32_opt("chars_per_batch", s.inject.chars_per_batch, 1, 4096);
        o.u32_opt("inter_batch_delay_us", s.inject.inter_batch_delay_us, 0, 100000);
        o.u32_opt("paste_settle_ms", s.inject.paste_settle_ms, 0, 5000);
        o.bool_opt("refuse_password_fields", s.inject.refuse_password_fields);
        o.bool_opt("refuse_elevated_targets", s.inject.refuse_elevated_targets);
        o.bool_opt("paste_ctrl_shift_v_default", s.paste_ctrl_shift_v_default);
        if (o.failed()) return Status{StatusCode::Malformed, o.error()};
        InjectMethod parsed{};
        if (!inject_method_from_string(method, parsed)) {
            return Status{StatusCode::Malformed,
                          "settings.inject.method: \"" + method + "\" is not a known method"};
        }
        s.inject.method = parsed;
    }

    if (const json::Value* v = root.find("app_policies")) {
        if (!v->is_array()) {
            return Status{StatusCode::Malformed, "settings.app_policies: expected an array"};
        }
        s.app_policies.clear();
        for (size_t i = 0; i < v->size(); ++i) {
            const json::Value* e = v->at(i);
            if (!e || !e->is_object()) {
                return Status{StatusCode::Malformed,
                              "settings.app_policies[" + std::to_string(i) + "] is not an object"};
            }
            AppPolicy p;
            Obj o(*e, "settings.app_policies[" + std::to_string(i) + "]");
            o.str_req("process_name", p.process_name);
            std::string method = to_string(p.method);
            o.str_opt("method", method);
            o.bool_opt("use_ctrl_shift_v", p.use_ctrl_shift_v);
            o.bool_opt("supported", p.supported);
            o.str_opt("note", p.note);
            if (o.failed()) return Status{StatusCode::Malformed, o.error()};
            if (!inject_method_from_string(method, p.method)) {
                return Status{StatusCode::Malformed,
                              "settings.app_policies[" + std::to_string(i) + "].method: \"" +
                                  method + "\" is not a known method"};
            }
            // Keyed on the executable name, lowercased (injector.h): not the window title, which
            // changes with the document, and not the window class, which Electron apps share.
            std::transform(p.process_name.begin(), p.process_name.end(), p.process_name.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            s.app_policies.push_back(std::move(p));
        }
    }

    if (const json::Value* v = root.find("anchor")) {
        Obj o(*v, "settings.anchor");
        o.f32_opt("min_confidence", s.anchor.min_confidence, 0.0f, 1.0f);
        std::string fb = enum_to_text(kFallbacks, static_cast<int>(s.anchor.below_threshold),
                                      "floating-pill");
        o.str_opt("below_threshold", fb);
        o.f32_opt("source_switch_margin", s.anchor.source_switch_margin, 0.0f, 1.0f);
        o.u32_opt("stale_after_ms", s.anchor.stale_after_ms, 100, 600000);
        uint32_t cw = static_cast<uint32_t>(s.anchor.capture_width);
        uint32_t ch = static_cast<uint32_t>(s.anchor.capture_height);
        o.u32_opt("capture_width", cw, 32, 8192);
        o.u32_opt("capture_height", ch, 16, 8192);
        o.u32_opt("after_frame_poll_ms", s.anchor.after_frame_poll_ms, 0, 2000);
        o.u32_opt("after_frame_poll_interval_ms", s.anchor.after_frame_poll_interval_ms, 1, 500);
        uint32_t tol = static_cast<uint32_t>(s.anchor.diff_tolerance);
        o.u32_opt("diff_tolerance", tol, 0, 255);
        if (o.failed()) return Status{StatusCode::Malformed, o.error()};
        s.anchor.capture_width = static_cast<int32_t>(cw);
        s.anchor.capture_height = static_cast<int32_t>(ch);
        s.anchor.diff_tolerance = static_cast<int>(tol);
        int value = 0;
        if (!enum_from_text(kFallbacks, fb, value)) {
            return Status{StatusCode::Malformed,
                          "settings.anchor.below_threshold: expected \"floating-pill\" or "
                          "\"no-preview\", got \"" + fb + "\""};
        }
        s.anchor.below_threshold = static_cast<anchor::Fallback>(value);
    }

    if (const json::Value* v = root.find("editing")) {
        Obj o(*v, "settings.editing");
        o.bool_opt("enabled", s.editing.enabled);
        o.bool_opt("mouse", s.editing.mouse);
        std::string action = to_string(s.editing.on_foreign_key);
        o.str_opt("on_foreign_key", action);
        std::string key = vk_setting_name(s.editing.commit_vk);
        o.str_opt("commit_key", key);
        if (o.failed()) return Status{StatusCode::Malformed, o.error()};
        if (!parse_foreign_key_action(action, s.editing.on_foreign_key)) {
            return Status{StatusCode::Malformed,
                          "settings.editing.on_foreign_key: expected \"ignore\" or "
                          "\"end-session\", got \"" + action + "\""};
        }
        // Parsed through the hotkey module's own key-name table, so "enter" means the same thing
        // here as it does in the `hotkey` string and the two cannot drift apart.
        HotkeySpec parsed_key;
        if (!HotkeySpec::parse(key, parsed_key) || parsed_key.mods != kModNone) {
            return Status{StatusCode::Malformed,
                          "settings.editing.commit_key: \"" + key +
                              "\" is not an unmodified key name (only \"enter\" is offered)"};
        }
        s.editing.commit_vk = parsed_key.vk;
    }

    if (const json::Value* v = root.find("overlay")) {
        Obj o(*v, "settings.overlay");
        std::string mode = enum_to_text(kOverlayModes, static_cast<int>(s.overlay_mode), "inline");
        o.str_opt("mode", mode);
        std::string theme =
            enum_to_text(kThemeModes, static_cast<int>(s.overlay_style.theme), "system");
        // Noted before the colours are read, because the migration below turns on exactly this:
        // a file written before `theme` existed.
        const bool file_names_a_theme = v->has("theme");
        o.str_opt("theme", theme);
        o.str_opt("font_family", s.overlay_style.font_family);
        o.f32_opt("font_size_dip", s.overlay_style.font_size_dip, 6.0f, 96.0f);
        o.color_opt("color_pending", s.overlay_style.color_pending);
        o.color_opt("color_unstable", s.overlay_style.color_unstable);
        o.color_opt("color_background", s.overlay_style.color_background);
        o.color_opt("color_warning", s.overlay_style.color_warning);
        o.color_opt("color_border", s.overlay_style.color_border);
        o.f32_opt("corner_radius_dip", s.overlay_style.corner_radius_dip, 0.0f, 64.0f);
        o.f32_opt("padding_dip", s.overlay_style.padding_dip, 0.0f, 64.0f);
        o.f32_opt("max_width_dip", s.overlay_style.max_width_dip, 80.0f, 4000.0f);
        if (o.failed()) return Status{StatusCode::Malformed, o.error()};
        int value = 0;
        if (!enum_from_text(kOverlayModes, mode, value)) {
            return Status{StatusCode::Malformed,
                          "settings.overlay.mode: \"" + mode + "\" is not a known overlay mode"};
        }
        s.overlay_mode = static_cast<OverlayMode>(value);
        if (!enum_from_text(kThemeModes, theme, value)) {
            return Status{StatusCode::Malformed, "settings.overlay.theme: \"" + theme +
                                                     "\" is not one of system, light, dark, "
                                                     "custom"};
        }
        s.overlay_style.theme = static_cast<ThemeMode>(value);

        // ── Migration: a file written before the pill had a colour scheme ───────────────────
        //
        // The default is System, which is right for the overwhelming majority — a file whose
        // colours are the shipped defaults means "I never chose", and following Windows is the
        // better answer to that than repainting light forever.
        //
        // But a file that names colours *different* from the defaults is a choice someone made,
        // and a new key silently overriding it is exactly the "silently discarding a user's
        // configuration is indistinguishable from losing it" failure this parser is written to
        // avoid. So that file, and only that file, is migrated to Custom: it keeps painting what
        // it painted, and the dialog's scheme box tells the user which mode they are in and what
        // the alternative is.
        if (!file_names_a_theme && s.overlay_style.theme == ThemeMode::System) {
            const OverlayStyle shipped{};
            const bool colored = s.overlay_style.color_pending != shipped.color_pending ||
                                 s.overlay_style.color_unstable != shipped.color_unstable ||
                                 s.overlay_style.color_background != shipped.color_background ||
                                 s.overlay_style.color_warning != shipped.color_warning ||
                                 s.overlay_style.color_border != shipped.color_border;
            if (colored) {
                s.overlay_style.theme = ThemeMode::Custom;
                KD_LOG_INFO(
                    "settings.overlay has hand-picked colours and no theme; keeping them as "
                    "theme=custom rather than replacing them with the system scheme");
            }
        }
    }

    if (const json::Value* v = root.find("privacy")) {
        Obj o(*v, "settings.privacy");
        std::string level = log::to_string(s.log_level);
        o.str_opt("log_level", level);
        o.bool_opt("diagnostics_file_enabled", s.diagnostics_file_enabled);
        if (o.failed()) return Status{StatusCode::Malformed, o.error()};
        if (!log::parse_level(level, s.log_level)) {
            return Status{StatusCode::Malformed,
                          "settings.privacy.log_level: \"" + level + "\" is not a known level"};
        }
        if (const json::Value* t = v->find("transcript")) {
            Obj to(*t, "settings.privacy.transcript");
            to.bool_opt("enabled", s.transcript.enabled);
            std::string file = path_to_utf8(s.transcript.file);
            to.str_opt("file", file);
            to.u64_opt("max_bytes", s.transcript.max_bytes, 1, 1024ull * 1024ull * 1024ull);
            uint32_t days = s.transcript.max_age_days;
            to.u32_opt("max_age_days", days, 1, 3650);
            if (to.failed()) return Status{StatusCode::Malformed, to.error()};
            s.transcript.max_age_days = days;
            s.transcript.file = file.empty() ? std::filesystem::path{} : path_from_utf8(file);
        }
    }

    if (const json::Value* v = root.find("context")) {
        Obj o(*v, "settings.context");
        o.bool_opt("send", s.send_context);
        o.strings_opt("terms", s.context_terms, proto::kMaxContextItems,
                      proto::kMaxContextItemBytes);
        if (o.failed()) return Status{StatusCode::Malformed, o.error()};
    }

    s.normalise();
    Status valid = s.validate();
    if (!valid.is_ok()) return valid;
    return s;
}

Result<Settings> load_settings(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        // A missing file is a first run, not a fault. Defaults — which are the privacy-preserving
        // choices in every case — and nothing written until the user changes something.
        Settings s;
        s.normalise();
        return s;
    }

    Result<std::string> bytes = read_file(file, kSettingsMaxBytes);
    if (!bytes) return bytes.status();

    Result<Settings> parsed = parse_settings(*bytes);
    if (!parsed) {
        return Status{parsed.status().code(),
                      path_to_utf8(file) + " — " + parsed.status().message()};
    }
    return parsed;
}

Status save_settings(const Settings& s, const std::filesystem::path& file) {
    Status valid = s.validate();
    if (!valid.is_ok()) return valid;  // never persist a file we would refuse to load
    if (file.empty()) {
        return Status{StatusCode::InvalidArgument, "no settings path (is %LOCALAPPDATA% set?)"};
    }
    return write_file_atomic(file, serialize_settings(s));
}

// ═══════════════════════════════════════════════════════════════════════════════════════════
//  The settings dialog
// ═══════════════════════════════════════════════════════════════════════════════════════════

namespace {

constexpr wchar_t kDialogClass[] = L"kvit-dictate.settings";

enum : int {
    kIdOk = IDOK,
    kIdCancel = IDCANCEL,
    kIdDefaults = 100,
    kIdTabs,
    kIdHotkey,
    kIdEndpointMs,
    kIdSessionMs,
    kIdSessionNever,
    kIdStabN,
    kIdStabM,
    kIdStabT,
    kIdCadence,
    kIdCadenceMs,
    kIdInjectMethod,
    kIdCharsBatch,
    kIdBatchDelay,
    kIdPasteSettle,
    kIdPasteChord,
    kIdRefusePassword,
    kIdRefuseElevated,
    kIdAppPolicies,
    kIdAudioDevice,
    kIdModel,
    kIdModelStatus,
    kIdOverlayMode,
    kIdFallback,
    kIdMinConfidence,
    kIdTheme,
    kIdFontFamily,
    kIdFontSize,
    kIdColorPending,
    kIdColorUnstable,
    kIdColorBackground,
    kIdLogLevel,
    kIdDiagFile,
    kIdTranscript,
    kIdTranscriptKb,
    kIdTranscriptDays,
    kIdStartup,
    kIdSendContext,
    kIdContextTerms,
    kIdEditingEnabled,
    kIdEditingMouse,
    kIdEditingForeign,
    kIdEditingStatus,
};

constexpr int kPageCount = 4;

struct DialogState {
    Settings settings;                       // the working copy; only copied out on OK
    const ModelManifest* manifest = nullptr;
    std::vector<audio::DeviceInfo> devices;  // parallel to the audio combo's items, minus item 0
    std::vector<const ModelEntry*> models;   // parallel to the model combo's items
    std::vector<HWND> page_controls[kPageCount];
    HFONT font = nullptr;
    int dpi = 96;

    //: One line of `font`, in *logical* px, measured rather than assumed.
    //:
    //: The explanatory notes under each section are plain STATIC controls given a fixed height of
    //: `lines` × this. The height used to be the literal 13, which is smaller than Segoe UI 9pt
    //: actually occupies at 96 DPI — so the last line of every multi-line note was cut in half,
    //: and the dialog read as broken text rather than as a layout that had run out of room. It is
    //: measured because the whole point of laying labels out above their controls (see `Cursor`)
    //: is to survive a font or DPI this file did not anticipate.
    int line_h = 15;
    bool accepted = false;
    bool running = true;

    // ── The colour scheme (theme.h) ─────────────────────────────────────────────────────────
    //
    // The dialog paints in whichever scheme the pill is set to, resolved once when it opens and
    // again whenever the user moves the combo or Windows moves underneath it. `dark` is the
    // answer; the two brushes exist only while it is true, because light mode takes the same
    // path it always took — the system's own COLOR_BTNFACE brush and the system's own control
    // colours — and a hand-written light palette here could only differ from the system's.
    bool dark = false;
    HBRUSH face_brush = nullptr;
    HBRUSH field_brush = nullptr;

    // Which push button the pointer is over, or null. Owner-drawn buttons are told whether they
    // are pressed, focused or disabled and are never told they are hot, so the hover state is
    // tracked here — one HWND, because the pointer is only ever over one of them.
    HWND hot_button = nullptr;
};

int px(const DialogState& st, int logical) { return MulDiv(logical, st.dpi, 96); }

std::wstring wide(std::string_view utf8_text) { return utf8::to_utf16_or_empty(utf8_text); }
std::string narrow(const std::wstring& w) { return utf8::from_utf16_or_empty(w); }

std::wstring get_text(HWND h) {
    const int n = GetWindowTextLengthW(h);
    if (n <= 0) return {};
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    const int copied = GetWindowTextW(h, s.data(), n + 1);
    s.resize(copied > 0 ? static_cast<size_t>(copied) : 0u);
    return s;
}

void set_int(HWND h, uint64_t v) { SetWindowTextW(h, std::to_wstring(v).c_str()); }

void set_float(HWND h, float v) {
    wchar_t buf[32];
    _snwprintf_s(buf, _TRUNCATE, L"%.2f", static_cast<double>(v));
    SetWindowTextW(h, buf);
}

void set_color(HWND h, uint32_t argb) {
    wchar_t buf[16];
    _snwprintf_s(buf, _TRUNCATE, L"#%08X", argb);
    SetWindowTextW(h, buf);
}

bool read_uint(HWND dlg, int id, const wchar_t* field, uint64_t lo, uint64_t hi, uint64_t& out) {
    const std::wstring text = get_text(GetDlgItem(dlg, id));
    wchar_t* end = nullptr;
    const unsigned long long v = wcstoull(text.c_str(), &end, 10);
    if (text.empty() || (end && *end != L'\0') || v < lo || v > hi) {
        std::wstring msg = std::wstring(field) + L" must be a whole number between " +
                           std::to_wstring(lo) + L" and " + std::to_wstring(hi) + L".";
        MessageBoxW(dlg, msg.c_str(), L"kvit-dictate settings", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(dlg, id));
        return false;
    }
    out = v;
    return true;
}

bool read_float(HWND dlg, int id, const wchar_t* field, float lo, float hi, float& out) {
    const std::wstring text = get_text(GetDlgItem(dlg, id));
    wchar_t* end = nullptr;
    const double v = wcstod(text.c_str(), &end);
    if (text.empty() || (end && *end != L'\0') || v < lo || v > hi) {
        std::wstring msg = std::wstring(field) + L" must be a number in range.";
        MessageBoxW(dlg, msg.c_str(), L"kvit-dictate settings", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(dlg, id));
        return false;
    }
    out = static_cast<float>(v);
    return true;
}

bool read_color(HWND dlg, int id, const wchar_t* field, uint32_t& out) {
    std::wstring text = get_text(GetDlgItem(dlg, id));
    if (!text.empty() && text[0] == L'#') text.erase(0, 1);
    if (text.size() != 8) {
        std::wstring msg = std::wstring(field) + L" must be 8 hex digits, as in #FF3C3C3C.";
        MessageBoxW(dlg, msg.c_str(), L"kvit-dictate settings", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(dlg, id));
        return false;
    }
    wchar_t* end = nullptr;
    const unsigned long long v = wcstoull(text.c_str(), &end, 16);
    if (end && *end != L'\0') {
        std::wstring msg = std::wstring(field) + L" contains a non-hex digit.";
        MessageBoxW(dlg, msg.c_str(), L"kvit-dictate settings", MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(dlg, id));
        return false;
    }
    out = static_cast<uint32_t>(v);
    return true;
}

// A cursor that walks down one column of one page, creating controls as it goes. Labels sit above
// their controls rather than beside them: it costs vertical space and it is the only layout that
// survives translation and a 200 % DPI scale without measuring every string.
struct Cursor {
    HWND dlg = nullptr;
    HINSTANCE inst = nullptr;
    DialogState* st = nullptr;
    int page = 0;
    int x = 0;
    int y = 0;
    int w = 0;

    HWND add(const wchar_t* cls, const wchar_t* text, DWORD style, int logical_height, int id,
             DWORD ex = 0) {
        const int h = px(*st, logical_height);
        // WS_CLIPSIBLINGS because the three pages are stacked in the same rectangle: without it,
        // painting or erasing one page's control is free to scribble over the identically-placed
        // control of another page, and a hidden page's erase can blank a visible one. Created
        // without WS_VISIBLE — `show_page` owns visibility, and a control that flashed up before
        // its page was selected would be visible on the wrong tab.
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_CLIPSIBLINGS | style, x, y, w, h,
                                 dlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst,
                                 nullptr);
        if (c) {
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
            st->page_controls[page].push_back(c);
        }
        y += h;
        return c;
    }

    void gap(int logical = 8) { y += px(*st, logical); }

    void section(const wchar_t* title) {
        gap(10);
        add(L"STATIC", title, SS_LEFTNOWORDWRAP, 16, -1);
        add(L"STATIC", L"", SS_ETCHEDHORZ, 2, -1);
        gap(4);
    }

    // Explanatory text. The two silence timers get one each, because the single most likely UI
    // defect here is a user reading "silence" and assuming there is one of them (plan.md §1.6).
    void note(const wchar_t* text, int lines = 2) {
        add(L"STATIC", text, 0, st->line_h * lines, -1);
        gap(2);
    }

    HWND label_edit(const wchar_t* label, int id, DWORD extra = 0) {
        add(L"STATIC", label, SS_LEFTNOWORDWRAP, 15, -1);
        HWND h = add(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | extra, 21, id,
                     WS_EX_CLIENTEDGE);
        gap(6);
        return h;
    }

    HWND label_combo(const wchar_t* label, int id) {
        add(L"STATIC", label, SS_LEFTNOWORDWRAP, 15, -1);
        // The height passed to a CBS_DROPDOWNLIST combo is the height of the window *including*
        // its dropped-down list, not of the closed control. A combo created 21 px tall drops open
        // to show nothing. So it is created tall and the layout cursor is then put back to the
        // closed height explicitly rather than by subtracting two separately-rounded MulDivs.
        const int top_of_combo = y;
        HWND h = add(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 220, id);
        y = top_of_combo + px(*st, 24);
        gap(6);
        return h;
    }

    HWND check(const wchar_t* label, int id) {
        HWND h = add(L"BUTTON", label, WS_TABSTOP | BS_AUTOCHECKBOX, 19, id);
        gap(3);
        return h;
    }

    HWND multiline(int id, int logical_height) {
        HWND h = add(L"EDIT", L"",
                     WS_TABSTOP | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_WANTRETURN |
                         ES_AUTOVSCROLL,
                     logical_height, id, WS_EX_CLIENTEDGE);
        gap(6);
        return h;
    }
};

void combo_add(HWND combo, const wchar_t* text) {
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
}

// ─── Per-app policies as text ───────────────────────────────────────────────────────────────
//
// A list view with add/edit/remove sub-dialogs would be three times the code for a list that
// realistically holds a handful of rows. One line per policy, editable in place:
//
//     windowsterminal.exe = clipboard, ctrl-shift-v
//     code.exe            = sendinput
//     someapp.exe         = sendinput, unsupported   # reason shown in the UI

std::wstring policies_to_text(const std::vector<AppPolicy>& policies) {
    std::wstring out;
    for (const AppPolicy& p : policies) {
        out += wide(p.process_name);
        out += L" = ";
        out += wide(to_string(p.method));
        if (p.use_ctrl_shift_v) out += L", ctrl-shift-v";
        if (!p.supported) out += L", unsupported";
        if (!p.note.empty()) {
            out += L"  # ";
            out += wide(p.note);
        }
        out += L"\r\n";
    }
    return out;
}

bool policies_from_text(const std::wstring& text, std::vector<AppPolicy>& out, std::wstring& err) {
    std::vector<AppPolicy> parsed;
    std::wistringstream in(text);
    std::wstring line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (!line.empty() && line.back() == L'\r') line.pop_back();

        std::wstring note;
        const size_t hash = line.find(L'#');
        if (hash != std::wstring::npos) {
            note = line.substr(hash + 1);
            line.erase(hash);
        }
        const size_t first = line.find_first_not_of(L" \t");
        if (first == std::wstring::npos) continue;  // blank or comment-only
        line = line.substr(first);
        while (!line.empty() && (line.back() == L' ' || line.back() == L'\t')) line.pop_back();
        if (line.empty()) continue;

        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) {
            err = L"Per-app policy line " + std::to_wstring(lineno) +
                  L": expected \"process.exe = sendinput\" or \"process.exe = clipboard\".";
            return false;
        }

        AppPolicy p;
        std::wstring name = line.substr(0, eq);
        while (!name.empty() && (name.back() == L' ' || name.back() == L'\t')) name.pop_back();
        if (name.empty()) {
            err = L"Per-app policy line " + std::to_wstring(lineno) + L": no process name.";
            return false;
        }
        std::transform(name.begin(), name.end(), name.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        p.process_name = narrow(name);

        std::wstring rest = line.substr(eq + 1);
        bool method_seen = false;
        size_t pos = 0;
        while (pos <= rest.size()) {
            const size_t comma = rest.find(L',', pos);
            std::wstring token = rest.substr(pos, comma == std::wstring::npos ? std::wstring::npos
                                                                              : comma - pos);
            const size_t a = token.find_first_not_of(L" \t");
            if (a != std::wstring::npos) {
                token = token.substr(a);
                while (!token.empty() && (token.back() == L' ' || token.back() == L'\t')) {
                    token.pop_back();
                }
                std::transform(token.begin(), token.end(), token.begin(),
                               [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
                if (token == L"ctrl-shift-v") {
                    p.use_ctrl_shift_v = true;
                } else if (token == L"unsupported") {
                    p.supported = false;
                } else if (!token.empty()) {
                    if (!inject_method_from_string(narrow(token), p.method)) {
                        err = L"Per-app policy line " + std::to_wstring(lineno) +
                              L": \"" + token + L"\" is not an injection method.";
                        return false;
                    }
                    method_seen = true;
                }
            }
            if (comma == std::wstring::npos) break;
            pos = comma + 1;
        }
        if (!method_seen) {
            err = L"Per-app policy line " + std::to_wstring(lineno) +
                  L": no injection method given.";
            return false;
        }
        const size_t nb = note.find_first_not_of(L" \t");
        if (nb != std::wstring::npos) p.note = narrow(note.substr(nb));
        parsed.push_back(std::move(p));
    }
    out = std::move(parsed);
    return true;
}

std::wstring terms_to_text(const std::vector<std::string>& terms) {
    std::wstring out;
    for (const std::string& t : terms) {
        out += wide(t);
        out += L"\r\n";
    }
    return out;
}

std::vector<std::string> terms_from_text(const std::wstring& text) {
    std::vector<std::string> out;
    std::wistringstream in(text);
    std::wstring line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        const size_t a = line.find_first_not_of(L" \t");
        if (a == std::wstring::npos) continue;
        line = line.substr(a);
        while (!line.empty() && (line.back() == L' ' || line.back() == L'\t')) line.pop_back();
        if (!line.empty()) out.push_back(narrow(line));
    }
    return out;
}

// ─── Populating the controls from a Settings ────────────────────────────────────────────────

void update_model_status(HWND dlg, DialogState& st) {
    const int sel = static_cast<int>(SendMessageW(GetDlgItem(dlg, kIdModel), CB_GETCURSEL, 0, 0));
    std::wstring text = L"No model selected.";
    if (sel >= 0 && static_cast<size_t>(sel) < st.models.size()) {
        const ModelEntry* e = st.models[static_cast<size_t>(sel)];
        Status ok = check_model_launchable(*st.manifest, e->id);
        text = ok.is_ok() ? (L"Ready: " + wide(path_to_utf8(e->interpreter)))
                          : wide(ok.message());
    }
    SetWindowTextW(GetDlgItem(dlg, kIdModelStatus), text.c_str());
}

// docs/editing.md §6.1 and §12: `editing.enabled` is forced off unless the cadence is
// `end-of-session`, "and the settings dialog must say why rather than silently ignoring it".
//
// One function, called from three places — when the page is filled in, when the cadence combo
// changes, and when the checkbox itself is clicked — so that the answer the dialog shows and the
// answer Settings::normalise() computes cannot come apart. A dialog that let the user tick a box
// which the loader then quietly untick is worse than a disabled box: the user believes they
// configured something.
void refresh_editing_page(HWND dlg) {
    const bool eos = SendMessageW(GetDlgItem(dlg, kIdCadence), CB_GETCURSEL, 0, 0) ==
                     cadence_index(Cadence::EndOfSession);
    EnableWindow(GetDlgItem(dlg, kIdEditingEnabled), eos ? TRUE : FALSE);

    const bool on = eos && IsDlgButtonChecked(dlg, kIdEditingEnabled) == BST_CHECKED;
    EnableWindow(GetDlgItem(dlg, kIdEditingMouse), on ? TRUE : FALSE);
    EnableWindow(GetDlgItem(dlg, kIdEditingForeign), on ? TRUE : FALSE);

    SetWindowTextW(GetDlgItem(dlg, kIdEditingStatus),
                   eos ? L"The compose buffer is available with this cadence."
                       : L"Unavailable: the cadence on the Dictation page is not "
                         L"\"end-of-session\". With any other cadence the text is already in "
                         L"your document as you speak, so there is nothing left in the pill to "
                         L"edit or for Enter to commit.");
}

// The three colour fields belong to the "custom colours" scheme and to nothing else, so they are
// enabled by it and greyed the rest of the time. Same rule as refresh_editing_page above and for
// the same reason: a field the user can type into, whose value the program then ignores, is a
// setting they believe they changed.
//
// The values are left in the boxes rather than blanked. They are still what the file holds and
// still what choosing "custom colours" would apply, and a field that emptied itself when greyed
// would read as having thrown the user's colours away.
// Defined further down with the rest of the tab-strip drawing, and declared here because
// apply_dialog_theme is what installs it.
// ─── Push buttons, drawn by hand, and only in dark mode ─────────────────────────────────────
//
// The same hole as the tab strip, in a different shape. There is no dark theme class that takes
// for a BS_PUSHBUTTON on this build: naming one that does not exist un-themes the control
// instead, and an unthemed button draws its 3D frame in COLOR_3DHIGHLIGHT and its label in
// COLOR_BTNTEXT — both system colours, neither of which WM_CTLCOLORBTN can change. The result
// was the giveaway: a correctly dark button face inside a white box, with a black label on it.
//
// So the buttons are BS_OWNERDRAW while the dialog is dark, and this draws them: a flat fill, a
// one-pixel border a step lighter than the face, and the label in the dialog's text colour. The
// three states Windows reports are honoured, plus hover, which it does not report — see
// button_hover_subclass.

void draw_dialog_button(DialogState& st, const DRAWITEMSTRUCT* di) {
    if (di == nullptr || !st.dark) return;
    const theme::DialogPalette& p = theme::dialog_palette(true);

    const bool pressed = (di->itemState & ODS_SELECTED) != 0;
    const bool disabled = (di->itemState & ODS_DISABLED) != 0;
    const bool focused = (di->itemState & ODS_FOCUS) != 0;
    const bool hot = di->hwndItem == st.hot_button;
    // Which button Enter presses. Read from the id and not from BS_DEFPUSHBUTTON, because
    // BS_OWNERDRAW *is* the type bits — turning it on is what removed that style in the first
    // place, and it is why dialog_proc answers DM_GETDEFID by hand.
    const bool is_default = di->CtlID == kIdOk;

    uint32_t fill = p.button;
    if (is_default) fill = p.button_default;
    if (hot) fill = p.button_hot;
    if (pressed) fill = p.button_pressed;

    RECT rc = di->rcItem;
    if (HBRUSH brush = CreateSolidBrush(fill)) {
        FillRect(di->hDC, &rc, brush);
        DeleteObject(brush);
    }
    // The focus ring is the only place a bright line belongs: it is the answer to "where does
    // the keyboard go next", and it is drawn only while the button actually has the focus.
    if (HBRUSH edge = CreateSolidBrush(focused ? p.text : p.button_border)) {
        FrameRect(di->hDC, &rc, edge);
        DeleteObject(edge);
    }

    wchar_t text[128]{};
    GetWindowTextW(di->hwndItem, text, ARRAYSIZE(text));
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, disabled ? p.disabled_text : p.text);
    HGDIOBJ previous = SelectObject(di->hDC, st.font);
    // Pressed buttons move their label a pixel down and right, which is the other half of what
    // makes a flat button feel like it was clicked.
    if (pressed) OffsetRect(&rc, 1, 1);
    DrawTextW(di->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(di->hDC, previous);
}

// Hover, which DRAWITEMSTRUCT does not carry.
//
// A themed button gets its hot state from the theme engine; an owner-drawn one gets nothing, and
// a button that does not react to the pointer at all reads as disabled. TrackMouseEvent is what
// turns "the pointer is here" into a WM_MOUSELEAVE when it is not, without a timer.
LRESULT CALLBACK button_hover_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                                       DWORD_PTR ref) {
    auto* st = reinterpret_cast<DialogState*>(ref);
    if (st != nullptr && st->dark) {
        if (msg == WM_MOUSEMOVE && st->hot_button != hwnd) {
            st->hot_button = hwnd;
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            InvalidateRect(hwnd, nullptr, TRUE);
        } else if (msg == WM_MOUSELEAVE && st->hot_button == hwnd) {
            st->hot_button = nullptr;
            InvalidateRect(hwnd, nullptr, TRUE);
        }
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, button_hover_subclass, id);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK tab_strip_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                                    DWORD_PTR ref);

LRESULT CALLBACK button_hover_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                                       DWORD_PTR ref);

// Repaint the dialog itself in `dark` (or out of it). Called when the window is created, when
// the colour-scheme combo moves, and when Windows changes scheme while the dialog is open.
//
// Three separate mechanisms, none of which covers the others, and all three are needed for the
// window not to end up half-dark: theme.h's per-window and per-control theming (the title bar,
// the frames Windows draws), the two brushes below (everything this file paints — the face and
// the WM_CTLCOLOR* replies), and the tab control, which is the one common control with no dark
// theme class at all and is drawn by hand in WM_NOTIFY.
void apply_dialog_theme(HWND dlg, DialogState& st, bool dark) {
    st.dark = dark;

    if (st.face_brush) DeleteObject(st.face_brush);
    if (st.field_brush) DeleteObject(st.field_brush);
    st.face_brush = nullptr;
    st.field_brush = nullptr;
    if (dark) {
        const theme::DialogPalette& p = theme::dialog_palette(true);
        st.face_brush = CreateSolidBrush(p.face);
        st.field_brush = CreateSolidBrush(p.field);
    }

    theme::apply_window_dark_mode(reinterpret_cast<uintptr_t>(dlg), dark);

    // The tab control, explicitly and after the pass above, which has just given it a theme
    // class it has no dark variant of. Three things, in this order:
    //
    //   an empty theme class  "no visual style at all", so the frames it draws underneath are
    //                         classic 3D lines rather than a themed light strip.
    //   the subclass          which repaints the whole strip after the control has had its say.
    //                         Installed once and left in place: it is inert while `dark` is
    //                         false, and removing it on every scheme change would be one more
    //                         thing to get wrong.
    if (HWND tabs = GetDlgItem(dlg, kIdTabs)) {
        SetWindowTheme(tabs, dark ? L"" : L"Explorer", nullptr);
        SetWindowSubclass(tabs, tab_strip_subclass, 1, reinterpret_cast<DWORD_PTR>(&st));
        InvalidateRect(tabs, nullptr, TRUE);
    }

    // The three push buttons. BS_OWNERDRAW is what makes WM_DRAWITEM arrive for them, and it
    // occupies the same four style bits as BS_PUSHBUTTON and BS_DEFPUSHBUTTON — so the type has
    // to be put back explicitly on the way to light, and OK has to be put back as the *default*
    // button specifically, or Enter would stop being the accept key.
    const struct { int id; bool is_default; } push_buttons[] = {
        {kIdOk, true}, {kIdCancel, false}, {kIdDefaults, false}};
    for (const auto& b : push_buttons) {
        HWND h = GetDlgItem(dlg, b.id);
        if (h == nullptr) continue;
        LONG_PTR style = GetWindowLongPtrW(h, GWL_STYLE) & ~static_cast<LONG_PTR>(BS_TYPEMASK);
        style |= dark ? BS_OWNERDRAW : (b.is_default ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON);
        SetWindowLongPtrW(h, GWL_STYLE, style);
        // Inert while `dark` is false, like the tab strip's, and installed once for the same
        // reason: a subclass added and removed on every scheme change is one more thing to get
        // wrong for no gain.
        SetWindowSubclass(h, button_hover_subclass, 1, reinterpret_cast<DWORD_PTR>(&st));
        InvalidateRect(h, nullptr, TRUE);
    }
    st.hot_button = nullptr;

    // RDW_ALLCHILDREN and UPDATENOW for the same reason show_page() spells out below: the erase
    // and the children's repaint have to happen in this call, not at the next idle, or the
    // window is briefly two schemes at once.
    RedrawWindow(dlg, nullptr, nullptr,
                 RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

// Put the dialog in whatever scheme the combo currently names, resolved against Windows.
//
// The *combo*, not st.settings: the scheme is the one setting in this dialog whose effect the
// user can see while choosing it, so it follows the selection immediately rather than waiting
// for OK. Nothing is written by this — cancelling still leaves the file untouched, and the
// dialog was only ever borrowing the colours.
//
// The dialog has no colour setting of its own. Two schemes for one application is a question
// nobody wants to be asked, so it takes the pill's. ThemeMode::Custom paints it light: those
// colours are the *pill's*, and there is nothing in them to derive a dialog face from (see
// theme_paints_dark in overlay.h).
void refresh_dialog_theme(HWND dlg, DialogState& st) {
    const ThemeMode chosen = theme_at(SendMessageW(GetDlgItem(dlg, kIdTheme), CB_GETCURSEL, 0, 0));
    apply_dialog_theme(dlg, st, theme_paints_dark(chosen, theme::system_prefers_dark()));
}

void refresh_theme_fields(HWND dlg) {
    const BOOL custom =
        theme_at(SendMessageW(GetDlgItem(dlg, kIdTheme), CB_GETCURSEL, 0, 0)) == ThemeMode::Custom
            ? TRUE
            : FALSE;
    EnableWindow(GetDlgItem(dlg, kIdColorPending), custom);
    EnableWindow(GetDlgItem(dlg, kIdColorUnstable), custom);
    EnableWindow(GetDlgItem(dlg, kIdColorBackground), custom);
}

void load_controls(HWND dlg, DialogState& st) {
    const Settings& s = st.settings;

    // Activation.
    HWND hk = GetDlgItem(dlg, kIdHotkey);
    SendMessageW(hk, CB_RESETCONTENT, 0, 0);
    const std::vector<std::pair<std::string, HotkeySpec>> presets = hotkey_presets();
    const std::string current = s.hotkey.to_display_string();
    int hk_sel = -1;
    for (size_t i = 0; i < presets.size(); ++i) {
        combo_add(hk, wide(presets[i].first).c_str());
        if (presets[i].second.vk == s.hotkey.vk && presets[i].second.mods == s.hotkey.mods) {
            hk_sel = static_cast<int>(i);
        }
    }
    if (hk_sel < 0) {
        // A binding set by hand in the file that is not one of the offered presets is kept, not
        // silently replaced — the file is allowed to be ahead of the menu.
        combo_add(hk, wide(current).c_str());
        hk_sel = static_cast<int>(presets.size());
    }
    SendMessageW(hk, CB_SETCURSEL, static_cast<WPARAM>(hk_sel), 0);

    set_int(GetDlgItem(dlg, kIdEndpointMs), s.session.endpoint_silence_ms);
    set_int(GetDlgItem(dlg, kIdSessionMs), s.session.session_silence_ms);
    CheckDlgButton(dlg, kIdSessionNever, s.session.session_silence_never ? BST_CHECKED
                                                                        : BST_UNCHECKED);
    EnableWindow(GetDlgItem(dlg, kIdSessionMs), !s.session.session_silence_never);

    set_int(GetDlgItem(dlg, kIdStabN), s.stability.hypotheses_n);
    set_int(GetDlgItem(dlg, kIdStabM), s.stability.retract_clusters_m);
    set_int(GetDlgItem(dlg, kIdStabT), s.stability.hold_ms_t);

    HWND cad = GetDlgItem(dlg, kIdCadence);
    SendMessageW(cad, CB_RESETCONTENT, 0, 0);
    combo_add(cad, L"immediate — flush as soon as a prefix is eligible");
    combo_add(cad, L"periodic — delay the flush, never widen it");
    combo_add(cad, L"end-of-session — accumulate, then type it all once");
    SendMessageW(cad, CB_SETCURSEL, static_cast<WPARAM>(cadence_index(s.stability.cadence)), 0);
    set_int(GetDlgItem(dlg, kIdCadenceMs), s.stability.cadence_ms);
    EnableWindow(GetDlgItem(dlg, kIdCadenceMs), s.stability.cadence == Cadence::Periodic);

    // The compose buffer (docs/editing.md §12).
    CheckDlgButton(dlg, kIdEditingEnabled, s.editing.enabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, kIdEditingMouse, s.editing.mouse ? BST_CHECKED : BST_UNCHECKED);
    HWND fk = GetDlgItem(dlg, kIdEditingForeign);
    SendMessageW(fk, CB_RESETCONTENT, 0, 0);
    combo_add(fk, L"is ignored \x2014 the pill names it and nothing happens");
    combo_add(fk, L"finishes the session \x2014 the key reaches your document");
    SendMessageW(fk, CB_SETCURSEL,
                 s.editing.on_foreign_key == ForeignKeyAction::EndSession ? 1u : 0u, 0);
    // After the cadence combo above has been filled in and selected: this reads it.
    refresh_editing_page(dlg);

    // Injection.
    HWND im = GetDlgItem(dlg, kIdInjectMethod);
    SendMessageW(im, CB_RESETCONTENT, 0, 0);
    combo_add(im, L"SendInput (KEYEVENTF_UNICODE) — universal, slower");
    combo_add(im, L"Clipboard + paste — fast, clobbers the clipboard");
    SendMessageW(im, CB_SETCURSEL, s.inject.method == InjectMethod::Clipboard ? 1u : 0u, 0);
    set_int(GetDlgItem(dlg, kIdCharsBatch), s.inject.chars_per_batch);
    set_int(GetDlgItem(dlg, kIdBatchDelay), s.inject.inter_batch_delay_us);
    set_int(GetDlgItem(dlg, kIdPasteSettle), s.inject.paste_settle_ms);
    CheckDlgButton(dlg, kIdPasteChord, s.paste_ctrl_shift_v_default ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, kIdRefusePassword,
                   s.inject.refuse_password_fields ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, kIdRefuseElevated,
                   s.inject.refuse_elevated_targets ? BST_CHECKED : BST_UNCHECKED);
    SetWindowTextW(GetDlgItem(dlg, kIdAppPolicies), policies_to_text(s.app_policies).c_str());

    // Audio.
    HWND dev = GetDlgItem(dlg, kIdAudioDevice);
    SendMessageW(dev, CB_RESETCONTENT, 0, 0);
    combo_add(dev, L"(follow the default capture device)");
    int dev_sel = 0;
    for (size_t i = 0; i < st.devices.size(); ++i) {
        std::wstring label = wide(st.devices[i].friendly_name);
        if (st.devices[i].is_default) label += L"  [default]";
        combo_add(dev, label.c_str());
        if (st.devices[i].id == s.audio_device_id) dev_sel = static_cast<int>(i) + 1;
    }
    SendMessageW(dev, CB_SETCURSEL, static_cast<WPARAM>(dev_sel), 0);

    // Model.
    HWND model = GetDlgItem(dlg, kIdModel);
    SendMessageW(model, CB_RESETCONTENT, 0, 0);
    st.models.clear();
    int model_sel = -1;
    if (st.manifest) {
        for (const ModelEntry& e : st.manifest->models) {
            std::wstring label = wide(e.display_name);
            label += L"  [";
            label += wide(proto::to_string(e.streaming_mode));
            label += L"]";
            if (!e.enabled) label += L"  (disabled)";
            else if (!e.interpreter_available()) label += L"  (interpreter missing)";
            combo_add(model, label.c_str());
            if (e.id == s.model_id) model_sel = static_cast<int>(st.models.size());
            st.models.push_back(&e);
        }
    }
    if (model_sel < 0 && !st.models.empty()) model_sel = 0;
    SendMessageW(model, CB_SETCURSEL, static_cast<WPARAM>(model_sel), 0);
    update_model_status(dlg, st);

    // Overlay.
    HWND om = GetDlgItem(dlg, kIdOverlayMode);
    SendMessageW(om, CB_RESETCONTENT, 0, 0);
    for (const EnumName& e : kOverlayModes) combo_add(om, wide(e.text).c_str());
    for (size_t i = 0; i < std::size(kOverlayModes); ++i) {
        if (kOverlayModes[i].value == static_cast<int>(s.overlay_mode)) {
            SendMessageW(om, CB_SETCURSEL, i, 0);
        }
    }
    HWND fb = GetDlgItem(dlg, kIdFallback);
    SendMessageW(fb, CB_RESETCONTENT, 0, 0);
    combo_add(fb, L"floating pill near the target");
    combo_add(fb, L"no preview at all");
    SendMessageW(fb, CB_SETCURSEL,
                 s.anchor.below_threshold == anchor::Fallback::NoPreview ? 1u : 0u, 0);
    set_float(GetDlgItem(dlg, kIdMinConfidence), s.anchor.min_confidence);
    HWND th = GetDlgItem(dlg, kIdTheme);
    SendMessageW(th, CB_RESETCONTENT, 0, 0);
    // Prose, not the enum's spelling: "system" in a settings file is unambiguous, and "system"
    // in a combo box is a word the user has to guess the meaning of.
    combo_add(th, L"follow Windows (light or dark)");
    combo_add(th, L"always light");
    combo_add(th, L"always dark");
    combo_add(th, L"custom colours (the three below)");
    SendMessageW(th, CB_SETCURSEL, static_cast<WPARAM>(theme_index(s.overlay_style.theme)), 0);
    refresh_theme_fields(dlg);
    // Also on "Restore defaults", which is the other caller of load_controls and is entitled to
    // put the dialog back to following Windows in front of the user.
    refresh_dialog_theme(dlg, st);
    SetWindowTextW(GetDlgItem(dlg, kIdFontFamily), wide(s.overlay_style.font_family).c_str());
    set_float(GetDlgItem(dlg, kIdFontSize), s.overlay_style.font_size_dip);
    set_color(GetDlgItem(dlg, kIdColorPending), s.overlay_style.color_pending);
    set_color(GetDlgItem(dlg, kIdColorUnstable), s.overlay_style.color_unstable);
    set_color(GetDlgItem(dlg, kIdColorBackground), s.overlay_style.color_background);

    // Privacy.
    HWND lvl = GetDlgItem(dlg, kIdLogLevel);
    SendMessageW(lvl, CB_RESETCONTENT, 0, 0);
    static const log::Level kLevels[] = {log::Level::Trace, log::Level::Debug, log::Level::Info,
                                         log::Level::Warn,  log::Level::Error, log::Level::Off};
    for (size_t i = 0; i < std::size(kLevels); ++i) {
        combo_add(lvl, wide(log::to_string(kLevels[i])).c_str());
        if (kLevels[i] == s.log_level) SendMessageW(lvl, CB_SETCURSEL, i, 0);
    }
    CheckDlgButton(dlg, kIdDiagFile, s.diagnostics_file_enabled ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, kIdTranscript, s.transcript.enabled ? BST_CHECKED : BST_UNCHECKED);
    set_int(GetDlgItem(dlg, kIdTranscriptKb), s.transcript.max_bytes / 1024u);
    set_int(GetDlgItem(dlg, kIdTranscriptDays), s.transcript.max_age_days);
    EnableWindow(GetDlgItem(dlg, kIdTranscriptKb), s.transcript.enabled);
    EnableWindow(GetDlgItem(dlg, kIdTranscriptDays), s.transcript.enabled);
    CheckDlgButton(dlg, kIdStartup, s.start_with_windows ? BST_CHECKED : BST_UNCHECKED);

    // Context.
    CheckDlgButton(dlg, kIdSendContext, s.send_context ? BST_CHECKED : BST_UNCHECKED);
    SetWindowTextW(GetDlgItem(dlg, kIdContextTerms), terms_to_text(s.context_terms).c_str());
}

// Read every control back. Returns false with the offending control focused and a message box
// already shown; the dialog then stays open.
bool read_controls(HWND dlg, DialogState& st, Settings& out) {
    Settings s = st.settings;  // start from the working copy so unexposed fields survive
    uint64_t n = 0;

    const int hk_sel = static_cast<int>(SendMessageW(GetDlgItem(dlg, kIdHotkey), CB_GETCURSEL, 0, 0));
    const std::vector<std::pair<std::string, HotkeySpec>> presets = hotkey_presets();
    if (hk_sel >= 0 && static_cast<size_t>(hk_sel) < presets.size()) {
        s.hotkey = presets[static_cast<size_t>(hk_sel)].second;
    }  // else: the hand-set binding at the end of the list — leave s.hotkey as it was

    if (!read_uint(dlg, kIdEndpointMs, L"Endpoint silence", 100, 10000, n)) return false;
    s.session.endpoint_silence_ms = static_cast<uint32_t>(n);
    s.session.session_silence_never = IsDlgButtonChecked(dlg, kIdSessionNever) == BST_CHECKED;
    if (!s.session.session_silence_never) {
        if (!read_uint(dlg, kIdSessionMs, L"Session silence", 1000, 24u * 3600u * 1000u, n)) {
            return false;
        }
        s.session.session_silence_ms = static_cast<uint32_t>(n);
    }

    if (!read_uint(dlg, kIdStabN, L"Hypotheses (N)", 1, 8, n)) return false;
    s.stability.hypotheses_n = static_cast<uint32_t>(n);
    if (!read_uint(dlg, kIdStabM, L"Retraction (M)", 0, 64, n)) return false;
    s.stability.retract_clusters_m = static_cast<uint32_t>(n);
    if (!read_uint(dlg, kIdStabT, L"Hold time (T)", 0, 5000, n)) return false;
    s.stability.hold_ms_t = static_cast<uint32_t>(n);

    s.stability.cadence =
        cadence_at(SendMessageW(GetDlgItem(dlg, kIdCadence), CB_GETCURSEL, 0, 0));
    const bool periodic = s.stability.cadence == Cadence::Periodic;
    if (periodic) {
        if (!read_uint(dlg, kIdCadenceMs, L"Cadence interval", 50, 5000, n)) return false;
        s.stability.cadence_ms = static_cast<uint32_t>(n);
    }

    // The compose buffer. `enabled` is read as the user left it and then forced off by
    // normalise() if the cadence does not permit it — the dialog disables the box rather than
    // second-guessing here, so that there is one place that decides (docs/editing.md §6.1).
    s.editing.enabled = IsDlgButtonChecked(dlg, kIdEditingEnabled) == BST_CHECKED;
    s.editing.mouse = IsDlgButtonChecked(dlg, kIdEditingMouse) == BST_CHECKED;
    s.editing.on_foreign_key =
        SendMessageW(GetDlgItem(dlg, kIdEditingForeign), CB_GETCURSEL, 0, 0) == 1
            ? ForeignKeyAction::EndSession
            : ForeignKeyAction::Ignore;

    s.inject.method = SendMessageW(GetDlgItem(dlg, kIdInjectMethod), CB_GETCURSEL, 0, 0) == 1
                          ? InjectMethod::Clipboard
                          : InjectMethod::SendInput;
    if (!read_uint(dlg, kIdCharsBatch, L"Characters per batch", 1, 4096, n)) return false;
    s.inject.chars_per_batch = static_cast<uint32_t>(n);
    if (!read_uint(dlg, kIdBatchDelay, L"Inter-batch delay", 0, 100000, n)) return false;
    s.inject.inter_batch_delay_us = static_cast<uint32_t>(n);
    if (!read_uint(dlg, kIdPasteSettle, L"Paste settle time", 0, 5000, n)) return false;
    s.inject.paste_settle_ms = static_cast<uint32_t>(n);
    s.paste_ctrl_shift_v_default = IsDlgButtonChecked(dlg, kIdPasteChord) == BST_CHECKED;
    s.inject.refuse_password_fields = IsDlgButtonChecked(dlg, kIdRefusePassword) == BST_CHECKED;
    s.inject.refuse_elevated_targets = IsDlgButtonChecked(dlg, kIdRefuseElevated) == BST_CHECKED;

    {
        std::wstring err;
        if (!policies_from_text(get_text(GetDlgItem(dlg, kIdAppPolicies)), s.app_policies, err)) {
            MessageBoxW(dlg, err.c_str(), L"kvit-dictate settings", MB_OK | MB_ICONWARNING);
            SetFocus(GetDlgItem(dlg, kIdAppPolicies));
            return false;
        }
    }

    const int dev_sel =
        static_cast<int>(SendMessageW(GetDlgItem(dlg, kIdAudioDevice), CB_GETCURSEL, 0, 0));
    s.audio_device_id.clear();
    if (dev_sel > 0 && static_cast<size_t>(dev_sel - 1) < st.devices.size()) {
        s.audio_device_id = st.devices[static_cast<size_t>(dev_sel - 1)].id;
    }

    const int model_sel =
        static_cast<int>(SendMessageW(GetDlgItem(dlg, kIdModel), CB_GETCURSEL, 0, 0));
    if (model_sel >= 0 && static_cast<size_t>(model_sel) < st.models.size()) {
        s.model_id = st.models[static_cast<size_t>(model_sel)]->id;
    }

    {
        const int sel =
            static_cast<int>(SendMessageW(GetDlgItem(dlg, kIdOverlayMode), CB_GETCURSEL, 0, 0));
        if (sel >= 0 && static_cast<size_t>(sel) < std::size(kOverlayModes)) {
            s.overlay_mode = static_cast<OverlayMode>(kOverlayModes[sel].value);
        }
    }
    s.anchor.below_threshold =
        SendMessageW(GetDlgItem(dlg, kIdFallback), CB_GETCURSEL, 0, 0) == 1
            ? anchor::Fallback::NoPreview
            : anchor::Fallback::FloatingPill;
    if (!read_float(dlg, kIdMinConfidence, L"Minimum anchor confidence", 0.0f, 1.0f,
                    s.anchor.min_confidence)) {
        return false;
    }
    s.overlay_style.theme = theme_at(SendMessageW(GetDlgItem(dlg, kIdTheme), CB_GETCURSEL, 0, 0));
    s.overlay_style.font_family = narrow(get_text(GetDlgItem(dlg, kIdFontFamily)));
    if (!read_float(dlg, kIdFontSize, L"Overlay font size", 6.0f, 96.0f,
                    s.overlay_style.font_size_dip)) {
        return false;
    }
    if (!read_color(dlg, kIdColorPending, L"Pending-text colour", s.overlay_style.color_pending)) {
        return false;
    }
    if (!read_color(dlg, kIdColorUnstable, L"Unstable-text colour",
                    s.overlay_style.color_unstable)) {
        return false;
    }
    if (!read_color(dlg, kIdColorBackground, L"Overlay background colour",
                    s.overlay_style.color_background)) {
        return false;
    }

    {
        const int sel =
            static_cast<int>(SendMessageW(GetDlgItem(dlg, kIdLogLevel), CB_GETCURSEL, 0, 0));
        static const log::Level kLevels[] = {log::Level::Trace, log::Level::Debug,
                                             log::Level::Info,  log::Level::Warn,
                                             log::Level::Error, log::Level::Off};
        if (sel >= 0 && static_cast<size_t>(sel) < std::size(kLevels)) s.log_level = kLevels[sel];
    }
    s.diagnostics_file_enabled = IsDlgButtonChecked(dlg, kIdDiagFile) == BST_CHECKED;
    s.transcript.enabled = IsDlgButtonChecked(dlg, kIdTranscript) == BST_CHECKED;
    if (s.transcript.enabled) {
        if (!read_uint(dlg, kIdTranscriptKb, L"Transcript size limit", 1, 1024u * 1024u, n)) {
            return false;
        }
        s.transcript.max_bytes = n * 1024u;
        if (!read_uint(dlg, kIdTranscriptDays, L"Transcript age limit", 1, 3650, n)) return false;
        s.transcript.max_age_days = static_cast<uint32_t>(n);
    }
    s.start_with_windows = IsDlgButtonChecked(dlg, kIdStartup) == BST_CHECKED;

    s.send_context = IsDlgButtonChecked(dlg, kIdSendContext) == BST_CHECKED;
    s.context_terms = terms_from_text(get_text(GetDlgItem(dlg, kIdContextTerms)));

    s.normalise();
    Status valid = s.validate();
    if (!valid.is_ok()) {
        MessageBoxW(dlg, wide(valid.message()).c_str(), L"kvit-dictate settings",
                    MB_OK | MB_ICONWARNING);
        return false;
    }

    out = std::move(s);
    return true;
}

// ─── Construction ───────────────────────────────────────────────────────────────────────────

constexpr int kPageWidth = 700;   // logical px, two columns
constexpr int kPageHeight = 430;
constexpr int kMargin = 12;
constexpr int kTabStrip = 30;

void build_controls(HWND dlg, DialogState& st) {
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dlg, GWLP_HINSTANCE));

    const int col_w = px(st, (kPageWidth - kMargin * 3) / 2);
    const int col1_x = px(st, kMargin * 2);
    const int col2_x = col1_x + col_w + px(st, kMargin);
    const int top = px(st, kMargin + kTabStrip + kMargin);

    // ── Page 0: the compose buffer (docs/editing.md) ────────────────────────────────────────
    {
        Cursor c{dlg, inst, &st, 0, col1_x, top, col_w};
        c.section(L"The pill as a compose buffer");
        c.note(L"Nothing reaches your document until you say so. Speech accumulates in the pill; "
               L"Enter types it in and finishes, Esc throws it away, and the hotkey only ever "
               L"means \"dictate\" — it starts a session and resumes one, and never finishes "
               L"one.",
               5);
        c.check(L"Let me edit the pill before it is typed in", kIdEditingEnabled);
        c.add(L"STATIC", L"", 0, st.line_h * 5, kIdEditingStatus);
        c.gap(6);

        c.section(L"While you are editing");
        c.note(L"The microphone is CLOSED. Editing and speaking are separate states and you are "
               L"only ever in one of them; the pill and the tray both say which. Press the "
               L"hotkey, or the button in the pill, to start talking again.",
               4);
        c.note(L"The session-silence timer does not run while you are editing, so reading back a "
               L"long dictation cannot end the session under you.",
               3);
    }
    {
        Cursor c{dlg, inst, &st, 0, col2_x, top, col_w};
        c.section(L"The keyboard belongs to the pill");
        c.note(L"While a session is composing, editing keys — Backspace, Delete, the arrows, "
               L"Home/End, Ctrl+A/C/X/V/Z and ordinary characters — go to the pill instead of to "
               L"your document. Volume, media and Print Screen keys are never intercepted.",
               5);
        c.label_combo(L"Any other key (Ctrl+S, F5, Alt+Tab)", kIdEditingForeign);

        c.section(L"Mouse");
        c.note(L"Click in the pill to place the caret, drag to select, and scroll with the wheel "
               L"to read back what you have said. Clicking anywhere else still finishes the "
               L"session and types the text in.",
               4);
        c.check(L"Let me click and scroll inside the pill", kIdEditingMouse);
    }

    // ── Page 1: dictation ───────────────────────────────────────────────────────────────────
    {
        Cursor c{dlg, inst, &st, 1, col1_x, top, col_w};
        c.section(L"Activation");
        c.label_combo(L"Hotkey (toggle: press to start, press again to finalise)", kIdHotkey);

        c.section(L"Endpoint silence — ends an UTTERANCE");
        c.note(L"How long a pause finishes the current sentence: it is punctuated, finalised and "
               L"committed. The session keeps running and the microphone stays open.",
               3);
        c.label_edit(L"Endpoint silence (ms, default 800)", kIdEndpointMs, ES_NUMBER);

        c.section(L"Session silence — ends the SESSION");
        c.note(L"A different timer, and never the same one: how long the whole session survives "
               L"with no speech at all. The microphone closes when it fires. A click or a "
               L"keystroke already ends the session, so a generous value is safe.",
               4);
        c.label_edit(L"Session silence (ms, default 120000)", kIdSessionMs, ES_NUMBER);
        c.check(L"Never end the session on silence", kIdSessionNever);
    }
    {
        Cursor c{dlg, inst, &st, 1, col2_x, top, col_w};
        c.section(L"Stability — what is SAFE to commit");
        c.note(L"Eligibility is decided by stability alone. Committed text is never repaired, so "
               L"nothing below may ever be widened by the cadence setting.",
               3);
        c.label_edit(L"N — hypotheses compared (default 2)", kIdStabN, ES_NUMBER);
        c.label_edit(L"M — clusters retracted from the decode head (default 3)", kIdStabM,
                     ES_NUMBER);
        c.label_edit(L"T — milliseconds unchanged before committing (default 200)", kIdStabT,
                     ES_NUMBER);

        c.section(L"Cadence — WHEN an eligible prefix is flushed");
        c.note(L"Timing only. It can delay a commit; it can never promote unstable text.", 2);
        c.label_combo(L"Cadence", kIdCadence);
        c.label_edit(L"Interval (ms, used only by \"periodic\")", kIdCadenceMs, ES_NUMBER);
    }

    // ── Page 2: injection, audio, model ─────────────────────────────────────────────────────
    {
        Cursor c{dlg, inst, &st, 2, col1_x, top, col_w};
        c.section(L"Text injection");
        c.label_combo(L"Default method", kIdInjectMethod);
        c.label_edit(L"SendInput: characters per batch (0 = one batch)", kIdCharsBatch, ES_NUMBER);
        c.label_edit(L"SendInput: delay between batches (microseconds)", kIdBatchDelay, ES_NUMBER);
        c.label_edit(L"Clipboard: settle time before restoring (ms)", kIdPasteSettle, ES_NUMBER);
        c.check(L"Default paste chord is Ctrl+Shift+V (terminals)", kIdPasteChord);
        c.check(L"Refuse to type into password fields", kIdRefusePassword);
        c.check(L"Refuse elevated targets (UIPI cannot be worked around)", kIdRefuseElevated);

        c.section(L"Audio");
        c.label_combo(L"Capture device", kIdAudioDevice);
    }
    {
        Cursor c{dlg, inst, &st, 2, col2_x, top, col_w};
        c.section(L"Per-app injection policy");
        c.note(L"One line per application:   process.exe = sendinput | clipboard "
               L"[, ctrl-shift-v] [, unsupported]   # note",
               3);
        c.multiline(kIdAppPolicies, 110);

        c.section(L"Model and backend");
        c.note(L"Selecting a model restarts the sidecar under that model's own interpreter. "
               L"kvit-dictate never learns what the model is — only which executable to launch.",
               3);
        c.label_combo(L"Model", kIdModel);
        c.add(L"STATIC", L"", 0, 44, kIdModelStatus);
    }

    // ── Page 3: overlay, privacy, vocabulary ────────────────────────────────────────────────
    {
        Cursor c{dlg, inst, &st, 3, col1_x, top, col_w};
        c.section(L"Overlay");
        c.label_combo(L"Presentation", kIdOverlayMode);
        c.label_combo(L"When the anchor is not confident enough", kIdFallback);
        c.label_edit(L"Minimum anchor confidence (0.0 - 1.0)", kIdMinConfidence);
        // The scheme sits above the three colours it governs, and the colours name it in their
        // labels, because a greyed-out field says "not now" and never says "why". No explanatory
        // note under it: this column is 419 of kPageHeight's 430 logical px as it stands, and one
        // more line of prose here is a control pushed off the page rather than a page that
        // scrolls — see the top of this file on what this dialog deliberately does not have.
        c.label_combo(L"Colour scheme (this dialog follows it too)", kIdTheme);
        c.label_edit(L"Font family", kIdFontFamily);
        c.label_edit(L"Font size (DIP)", kIdFontSize);
        c.label_edit(L"Pending text, custom scheme only (#AARRGGBB)", kIdColorPending);
        c.label_edit(L"Unstable text, custom scheme only (#AARRGGBB)", kIdColorUnstable);
        c.label_edit(L"Background, custom scheme only (#AARRGGBB)", kIdColorBackground);
    }
    {
        Cursor c{dlg, inst, &st, 3, col2_x, top, col_w};
        c.section(L"Privacy and diagnostics");
        c.note(L"Shipping policy, not preference: nothing you dictate is written to disk, and "
               L"nothing here ever touches the network.",
               3);
        c.label_combo(L"Diagnostic log level", kIdLogLevel);
        c.check(L"Write diagnostics to a file (shapes and timings, never text)", kIdDiagFile);
        c.check(L"Log recognised text to a transcript file (off by default)", kIdTranscript);
        c.label_edit(L"Transcript retention: size (KB)", kIdTranscriptKb, ES_NUMBER);
        c.label_edit(L"Transcript retention: age (days)", kIdTranscriptDays, ES_NUMBER);
        c.check(L"Start kvit-dictate with Windows", kIdStartup);

        c.section(L"Vocabulary context");
        c.note(L"One term per line, sent with each session so the model can bias towards them.",
               2);
        c.check(L"Send vocabulary context to the model", kIdSendContext);
        c.multiline(kIdContextTerms, 90);
    }

    // ── Tabs and buttons ────────────────────────────────────────────────────────────────────
    HWND tabs = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                px(st, kMargin), px(st, kMargin), px(st, kPageWidth),
                                px(st, kTabStrip), dlg,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdTabs)), inst,
                                nullptr);
    if (tabs) {
        SendMessageW(tabs, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);
        const wchar_t* names[kPageCount] = {L"Compose buffer", L"Dictation", L"Injection && model",
                                            L"Overlay && privacy"};
        for (int i = 0; i < kPageCount; ++i) {
            TCITEMW item{};
            item.mask = TCIF_TEXT;
            item.pszText = const_cast<wchar_t*>(names[i]);
            SendMessageW(tabs, TCM_INSERTITEMW, static_cast<WPARAM>(i),
                         reinterpret_cast<LPARAM>(&item));
        }
    }

    const int button_w = px(st, 96);
    const int button_h = px(st, 26);
    const int button_y = px(st, kMargin + kTabStrip + kMargin + kPageHeight + kMargin);
    const int right = px(st, kMargin + kPageWidth);

    struct ButtonDef {
        const wchar_t* text;
        int id;
        int slot;
        DWORD style;
    };
    const ButtonDef buttons[] = {
        {L"OK", kIdOk, 0, BS_DEFPUSHBUTTON},
        {L"Cancel", kIdCancel, 1, BS_PUSHBUTTON},
        {L"Restore defaults", kIdDefaults, 2, BS_PUSHBUTTON},
    };
    for (const ButtonDef& b : buttons) {
        const int w = b.id == kIdDefaults ? px(st, 140) : button_w;
        int x = right - button_w * (b.slot + 1) - px(st, 8) * b.slot;
        if (b.id == kIdDefaults) x = px(st, kMargin);
        HWND h = CreateWindowExW(0, L"BUTTON", b.text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | b.style,
                                 x, button_y, w, button_h, dlg,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(b.id)), inst,
                                 nullptr);
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(st.font), TRUE);
    }
}

// ─── The tab strip, drawn by hand, and only in dark mode ────────────────────────────────────
//
// The tab control is the one common control Microsoft shipped no dark theme class for. Every
// other control here is opted in by name in theme.cpp; this one has to be painted, and getting
// there took three attempts that are worth recording, because each failed in a way that looked
// like progress:
//
//   NM_CUSTOMDRAW      never arrives. The tab control is not one of the controls that send it,
//                      so the handler was simply never called and the strip stayed entirely
//                      light while every other surface went dark.
//   TCS_OWNERDRAWFIXED + WM_DRAWITEM draws the tab *interiors*, and the control still draws its
//                      own 3D frame around each tab and the pane edge under the row — in
//                      COLOR_3DHIGHLIGHT, i.e. white. Dark tabs in white boxes.
//   painting after it  works, and is what this does: let the control paint whatever it likes,
//                      then draw the whole strip over the top from a subclass on WM_PAINT.
//
// Everything in the strip is therefore ours: the background, each tab's fill, and each label.
// The result is flat — no frames — and the selected tab is told apart by its lighter fill, which
// is what a modern tab strip does anyway.
void paint_tab_strip_dark(HWND tabs, DialogState& st) {
    const theme::DialogPalette& p = theme::dialog_palette(true);
    HDC dc = GetDC(tabs);
    if (dc == nullptr) return;

    RECT client{};
    GetClientRect(tabs, &client);
    FillRect(dc, &client, st.face_brush);

    const int selected = static_cast<int>(SendMessageW(tabs, TCM_GETCURSEL, 0, 0));
    const int count = static_cast<int>(SendMessageW(tabs, TCM_GETITEMCOUNT, 0, 0));
    HBRUSH face = CreateSolidBrush(p.tab_face);
    HBRUSH chosen = CreateSolidBrush(p.tab_selected);
    HGDIOBJ previous_font = SelectObject(dc, st.font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, p.text);

    for (int i = 0; i < count; ++i) {
        RECT rc{};
        if (!SendMessageW(tabs, TCM_GETITEMRECT, static_cast<WPARAM>(i),
                          reinterpret_cast<LPARAM>(&rc))) {
            continue;
        }
        RECT fill_rc = rc;
        // Two pixels proud on every side, which is where the frame the control just drew is.
        // The label is still laid out in the item's own rectangle, so it stays centred on the
        // tab rather than on the overspill.
        InflateRect(&fill_rc, 2, 2);
        FillRect(dc, &fill_rc, i == selected ? chosen : face);

        wchar_t text[64]{};
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = text;
        item.cchTextMax = ARRAYSIZE(text);
        SendMessageW(tabs, TCM_GETITEMW, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(&item));
        // Prefix processing left ON, deliberately: the tab names are written with "&&" and the
        // control's own drawing turns that into one ampersand. DT_NOPREFIX here would print
        // "Injection && model" on the dark dialog and "Injection & model" on the light one, which
        // is the kind of difference nobody attributes to the colour scheme.
        DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(dc, previous_font);
    if (face) DeleteObject(face);
    if (chosen) DeleteObject(chosen);
    ReleaseDC(tabs, dc);
}

LRESULT CALLBACK tab_strip_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                                    DWORD_PTR ref) {
    auto* st = reinterpret_cast<DialogState*>(ref);
    const bool dark = st != nullptr && st->dark && st->face_brush != nullptr;

    if (msg == WM_ERASEBKGND && dark) {
        // Not sufficient on its own — the control paints over this — but it stops the light
        // button face flashing up between the erase and the repaint below.
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wp), &rc, st->face_brush);
        return 1;
    }

    if (msg == WM_PAINT && dark) {
        const LRESULT painted = DefSubclassProc(hwnd, msg, wp, lp);
        paint_tab_strip_dark(hwnd, *st);
        return painted;
    }

    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, tab_strip_subclass, id);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// Every page's controls occupy the same rectangle, so the three pages are stacked overlapping
// siblings and only one is ever visible. That makes the *order* of the show/hide calls, and the
// repaint afterwards, load-bearing rather than incidental — see the two comments below. Both were
// wrong, and the symptom was a tab page that rendered completely blank while every one of its
// controls reported `IsWindowVisible() == true` and sat at the right coordinates.
void show_page(HWND dlg, DialogState& st, int page) {
    // Hide the other pages FIRST. Hiding a control invalidates the parent region underneath it,
    // and the parent's erase of that region paints over whatever is there — including controls
    // shown a moment earlier. Showing first and hiding second therefore wipes the page that was
    // just made visible, which is why returning to a page left it blank.
    for (int p = 0; p < kPageCount; ++p) {
        if (p == page) continue;
        for (HWND h : st.page_controls[p]) ShowWindow(h, SW_HIDE);
    }
    for (HWND h : st.page_controls[page]) ShowWindow(h, SW_SHOW);

    // ShowWindow only marks a control for painting; the erase still pending from the hides above
    // can be coalesced on top of it. One explicit, synchronous redraw of the dialog with
    // RDW_ALLCHILDREN is what makes the switch deterministic instead of dependent on the order
    // Windows happens to merge the invalid regions in. RDW_UPDATENOW rather than RDW_ERASENOW so
    // the children repaint in this call rather than at the next idle.
    RedrawWindow(dlg, nullptr, nullptr,
                 RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

LRESULT CALLBACK dialog_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DialogState* st = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;
        }
        case WM_CREATE:
            if (st) {
                build_controls(hwnd, *st);
                load_controls(hwnd, *st);
                show_page(hwnd, *st, 0);
            }
            return 0;

        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lp);
            if (st && hdr && hdr->idFrom == kIdTabs && hdr->code == TCN_SELCHANGE) {
                const int page =
                    static_cast<int>(SendMessageW(hdr->hwndFrom, TCM_GETCURSEL, 0, 0));
                if (page >= 0 && page < kPageCount) show_page(hwnd, *st, page);
            }
            return 0;
        }

        case WM_COMMAND: {
            if (!st) break;
            const int id = LOWORD(wp);
            const int code = HIWORD(wp);
            switch (id) {
                case kIdOk: {
                    Settings updated;
                    if (read_controls(hwnd, *st, updated)) {
                        st->settings = std::move(updated);
                        st->accepted = true;
                        st->running = false;
                        DestroyWindow(hwnd);
                    }
                    return 0;
                }
                case kIdCancel:
                    st->accepted = false;
                    st->running = false;
                    DestroyWindow(hwnd);
                    return 0;
                case kIdDefaults: {
                    // Defaults include the privacy defaults; that is the point of the button.
                    Settings fresh;
                    fresh.model_id = st->settings.model_id;  // not a preference — a manifest fact
                    fresh.normalise();
                    st->settings = std::move(fresh);
                    load_controls(hwnd, *st);
                    return 0;
                }
                case kIdSessionNever:
                    EnableWindow(GetDlgItem(hwnd, kIdSessionMs),
                                 IsDlgButtonChecked(hwnd, kIdSessionNever) != BST_CHECKED);
                    return 0;
                case kIdTranscript: {
                    const BOOL on = IsDlgButtonChecked(hwnd, kIdTranscript) == BST_CHECKED;
                    EnableWindow(GetDlgItem(hwnd, kIdTranscriptKb), on);
                    EnableWindow(GetDlgItem(hwnd, kIdTranscriptDays), on);
                    return 0;
                }
                case kIdCadence:
                    if (code == CBN_SELCHANGE) {
                        EnableWindow(GetDlgItem(hwnd, kIdCadenceMs),
                                     SendMessageW(GetDlgItem(hwnd, kIdCadence), CB_GETCURSEL, 0,
                                                  0) == 1);
                        // The compose buffer's availability follows the cadence, and it is on a
                        // different page — so the user changes the cadence here and finds the
                        // other page already telling them what it now permits, rather than
                        // discovering it on the next launch (docs/editing.md §12).
                        refresh_editing_page(hwnd);
                    }
                    return 0;
                case kIdEditingEnabled:
                    refresh_editing_page(hwnd);
                    return 0;
                case kIdModel:
                    if (code == CBN_SELCHANGE) update_model_status(hwnd, *st);
                    return 0;
                case kIdTheme:
                    if (code == CBN_SELCHANGE) {
                        refresh_theme_fields(hwnd);
                        refresh_dialog_theme(hwnd, *st);
                    }
                    return 0;
                default:
                    break;
            }
            break;
        }

        case DM_GETDEFID:
            // Which button Enter presses. Normally IsDialogMessageW finds this from the
            // BS_DEFPUSHBUTTON style — and in dark mode there is no such style on any of them,
            // because BS_OWNERDRAW replaced it. Answered here so the answer does not depend on
            // the colour scheme: Enter is OK in both.
            return MAKELRESULT(kIdOk, DC_HASDEFID);

        case WM_DRAWITEM:
            // The three push buttons, and only while the dialog is dark — nothing else in this
            // dialog is ever owner-drawn.
            if (st && st->dark) {
                draw_dialog_button(*st, reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
                return TRUE;
            }
            break;

        case WM_ERASEBKGND:
            // Light mode keeps the class background brush (COLOR_BTNFACE) and this message is
            // not ours. Dark mode has no system brush to name, so the face is painted here — and
            // it must be *this* message rather than WM_PAINT, because show_page() below relies
            // on the parent's erase to clear the region a hidden page's control vacated.
            if (st && st->dark && st->face_brush) {
                RECT rc{};
                GetClientRect(hwnd, &rc);
                FillRect(reinterpret_cast<HDC>(wp), &rc, st->face_brush);
                return 1;
            }
            break;

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            // Static text and check-box text over the dialog face; without this the labels get a
            // white block. TRANSPARENT keeps the face — painted above, or by the class brush —
            // showing through the glyphs' own background.
            SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
            if (st && st->dark && st->face_brush) {
                SetTextColor(reinterpret_cast<HDC>(wp), theme::dialog_palette(true).text);
                return reinterpret_cast<LRESULT>(st->face_brush);
            }
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            // The inside of an edit, of a combo's own field, and of its dropped-down list. A
            // *disabled* edit is not this message — it comes through WM_CTLCOLORSTATIC above and
            // so takes the face colour, which is exactly right: the three greyed colour fields
            // should read as part of the dialog rather than as wells you can type into.
            if (st && st->dark && st->field_brush) {
                const theme::DialogPalette& p = theme::dialog_palette(true);
                SetTextColor(reinterpret_cast<HDC>(wp), p.field_text);
                SetBkColor(reinterpret_cast<HDC>(wp), p.field);
                return reinterpret_cast<LRESULT>(st->field_brush);
            }
            break;

        case WM_SETTINGCHANGE:
            // Windows switched scheme under an open dialog. Only meaningful while the combo says
            // "follow Windows", and refresh_dialog_theme is what decides that — it re-reads both
            // halves of the answer, so a dialog set to "always dark" stays dark.
            if (st && theme::is_color_scheme_change(static_cast<intptr_t>(lp))) {
                refresh_dialog_theme(hwnd, *st);
            }
            break;

        case WM_CLOSE:
            if (st) {
                st->accepted = false;
                st->running = false;
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (st) st->running = false;
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool show_settings_dialog(uintptr_t instance, uintptr_t owner, Settings& s,
                          const ModelManifest& manifest) {
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(instance);
    HWND owner_hwnd = reinterpret_cast<HWND>(owner);

    // The tab control comes from comctl32 and its class is not registered until this is called.
    // Called here as well as in main.cpp because a tools/ binary may open the dialog without the
    // tray, and a missing tab control presents as "the dialog is blank", not as an error.
    INITCOMMONCONTROLSEX icc{sizeof(INITCOMMONCONTROLSEX), ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    // Before any of this dialog's windows exist: theme.h is explicit that a control created
    // before the process was opted in stays light. Called here as well as at startup for the
    // same reason as the line above — a tools/ binary may open this dialog without main.cpp.
    theme::init_app_dark_mode();

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = dialog_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kDialogClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            KD_LOG_ERROR("settings dialog class registration failed, err=%lu", GetLastError());
            return false;
        }
        registered = true;
    }

    DialogState st;
    st.settings = s;
    st.manifest = &manifest;

    // The device list is a snapshot taken when the dialog opens. It does not follow a device
    // arriving while the dialog is up; that is a known limit of the no-data-binding approach and
    // is why the "(follow the default capture device)" entry is first and is the default.
    if (Result<std::vector<audio::DeviceInfo>> devices = audio::enumerate_capture_devices()) {
        st.devices = std::move(*devices);
    } else {
        KD_LOG_WARN("settings: capture device enumeration failed (%s)",
                    devices.status().to_string().c_str());
    }

    // GetDpiForWindow is per-monitor: the dialog is laid out for the monitor the tray window is
    // on, which is where it will appear. The process is PER_MONITOR_AWARE_V2, so nothing scales
    // this for us.
    st.dpi = owner_hwnd ? static_cast<int>(GetDpiForWindow(owner_hwnd))
                        : static_cast<int>(GetDpiForSystem());
    if (st.dpi <= 0) st.dpi = 96;

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0,
                                    static_cast<UINT>(st.dpi))) {
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    }
    st.font = CreateFontIndirectW(&ncm.lfMessageFont);

    // Measure one line of that font and convert back to logical px, which is the unit `Cursor`
    // lays out in. Falls back to the previous constant if the DC cannot be had, so a measurement
    // failure costs the old clipping rather than a zero-height note.
    if (HDC dc = GetDC(nullptr)) {
        HGDIOBJ previous = SelectObject(dc, st.font);
        TEXTMETRICW tm{};
        if (GetTextMetricsW(dc, &tm)) {
            const int device_line = static_cast<int>(tm.tmHeight + tm.tmExternalLeading);
            const int logical_line = MulDiv(device_line, 96, st.dpi);
            if (logical_line > 0) st.line_h = logical_line;
        }
        SelectObject(dc, previous);
        ReleaseDC(nullptr, dc);
    }

    RECT client{0, 0, px(st, kMargin * 2 + kPageWidth),
                px(st, kMargin * 3 + kTabStrip + kPageHeight + 26 + kMargin)};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectExForDpi(&client, style, FALSE, 0, static_cast<UINT>(st.dpi));

    HWND dlg = CreateWindowExW(WS_EX_CONTROLPARENT | WS_EX_DLGMODALFRAME, kDialogClass,
                               L"kvit-dictate settings", style, CW_USEDEFAULT, CW_USEDEFAULT,
                               client.right - client.left, client.bottom - client.top, owner_hwnd,
                               nullptr, inst, &st);
    if (!dlg) {
        KD_LOG_ERROR("settings dialog creation failed, err=%lu", GetLastError());
        if (st.font) DeleteObject(st.font);
        return false;
    }

    // Modal by hand rather than DialogBoxParam, because there is no dialog template: disable the
    // owner, pump until the dialog is destroyed, then re-enable. Re-enabling BEFORE the window is
    // destroyed is what stops Windows activating another application's window in between.
    const BOOL owner_was_enabled = owner_hwnd ? IsWindowEnabled(owner_hwnd) : FALSE;
    if (owner_hwnd && owner_was_enabled) EnableWindow(owner_hwnd, FALSE);

    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);

    MSG msg;
    while (st.running) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0) {
            // WM_QUIT arrived while the dialog was up (session end, or the tray's Quit). Put it
            // back so the owner's loop sees it, and treat the dialog as cancelled.
            PostQuitMessage(static_cast<int>(msg.wParam));
            st.accepted = false;
            break;
        }
        if (got == -1) break;
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (owner_hwnd && owner_was_enabled) EnableWindow(owner_hwnd, TRUE);
    if (IsWindow(dlg)) DestroyWindow(dlg);
    if (owner_hwnd) SetActiveWindow(owner_hwnd);
    if (st.font) DeleteObject(st.font);
    // After the window is gone, so nothing can be asked to paint with a deleted brush.
    if (st.face_brush) DeleteObject(st.face_brush);
    if (st.field_brush) DeleteObject(st.field_brush);

    if (st.accepted) s = st.settings;
    return st.accepted;
}

}  // namespace kd
