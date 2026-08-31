// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
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
// BP_CHECKBOX, BP_RADIOBUTTON and the CBS_/RBS_ state constants. uxtheme.h declares the drawing
// functions and vssym32.h names the parts they take; without it the check-box painter would have
// to spell the numbers out.
#include <vssym32.h>

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
#include <thread>
#include <vector>

#include "settings.h"

#include "audio_capture.h"
#include "ipc_client.h"
#include "translator.h"
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
//     "push_to_talk": false,                              // true: hold to talk, release types it in
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
//     "context":   { "send": true, "terms": [ "llama.cpp", "Qwen3" ] },
//     "normalizer":{ "enabled": false,                     // normalizer.h; off by default
//                    "url": "http://strixhalo.lan:10301/v1/text/normalize",
//                    "timeout_ms": 3000, "max_bytes": 4000,
//                    "styling": "semi-formal", "structure": "prose",
//                    "context": "general" }
//   }
//
// **Cadence is a sibling of stability, not a member of it**, even though StabilityParams carries
// both. The nesting is deliberate: plan.md §1.4 separates eligibility from cadence, and a file in
// which `cadence` sits inside `stability` invites exactly the merge the plan spent a revision
// undoing.

void Settings::normalise() {
    // `server.mode` and `server.url` must agree, and where they cannot, the URL wins.
    //
    // A `ws://` or `wss://` address in Local mode is never meaningful: `kvit-asrd` speaks
    // `/v1/audio/transcriptions` over HTTP and nothing else, so no configuration of it can talk
    // to a WebSocket. Reconciling here rather than letting validate() reject it keeps
    // serialize→parse a total function — a settings file this program wrote must always load —
    // and it is the same rule migrate_server_mode() applies to a file that has no mode at all.
    //
    // The reverse (an `http://` URL in Protocol mode) is deliberately left alone: docs §2.6.1
    // makes that a legal WebSocket synonym, and somebody who ticked "I run my own server" and
    // typed one meant it.
    if (server.mode == ServerMode::Local && !server.url.empty() &&
        migrate_server_mode(server.url) == ServerMode::Protocol) {
        server.mode = ServerMode::Protocol;
    }

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

    // The same rule, and for a harder reason than the compose buffer's. normalizer.h: a rewrite
    // is not prefix-stable, so under any cadence that has already typed the text, applying one
    // means retracting bytes from the user's document. Forced rather than checked at each use,
    // so the app, the settings dialog and validate() cannot hold different opinions — and forced
    // rather than merely rejected, because a user who switches cadence back to `immediate`
    // should get a working program, not a settings file that refuses to load.
    if (stability.cadence != Cadence::EndOfSession) normalizer.enabled = false;

    // Derived, exactly as `session.context` is: the terms the user maintains for the recogniser
    // are the terms the rewriter may not lose. One list to edit, two things protected.
    normalizer.protect_terms = context_terms;

    // The commit key is offered as "enter" and nothing else. A file that names some other key
    // still round-trips, but a file that names *no* key would silently disable the one gesture
    // that ends a composing session.
    if (editing.commit_vk == 0) editing.commit_vk = vk::kReturn;
}

// ─── ServerMode (openai-translator.md §4.6) ─────────────────────────────────────────────────

const char* to_string(ServerMode m) noexcept {
    switch (m) {
        case ServerMode::Local:    return "local";
        case ServerMode::Protocol: return "protocol";
    }
    return "local";
}

bool server_mode_from_string(std::string_view s, ServerMode& out) noexcept {
    if (s == "local") {
        out = ServerMode::Local;
        return true;
    }
    if (s == "protocol") {
        out = ServerMode::Protocol;
        return true;
    }
    return false;
}

std::string validate_speech_service_url(std::string_view url) {
    if (url.empty()) return {};  // the first-run state, not an error

    // Case-insensitive on the scheme only. A locale-aware fold is deliberately not used, for the
    // reason normalizer.cpp gives: the Turkish dotless i in a comparison whose job is to be
    // predictable.
    std::string lower;
    lower.reserve(url.size());
    for (const char c : url) {
        const auto u = static_cast<unsigned char>(c);
        lower.push_back(u < 0x80 ? static_cast<char>(std::tolower(u)) : c);
    }
    if (lower.rfind("ws://", 0) == 0 || lower.rfind("wss://", 0) == 0) {
        // The most likely mistake by far, so it gets a sentence rather than a syntax complaint.
        return "that is a kvit-dictate protocol server address. If you run your own server, "
               "choose \"I run my own kvit-dictate server\"; otherwise give the address of your "
               "speech service, which starts with http:// or https://";
    }
    if (lower.rfind("http://", 0) != 0 && lower.rfind("https://", 0) != 0) {
        return "a speech service address starts with http:// or https:// "
               "(for example http://strixhalo.lan:10301)";
    }

    // The characters that would have to be quoted onto `kvit-asrd --service`. Rejected here, in
    // the dialog, rather than at spawn time: the user is looking at the field now.
    for (const char c : url) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) return "the address contains a control character";
        if (c == ' ' || c == '\t') return "the address contains a space";
        if (c == '"' || c == '\'' || c == '`') return "the address contains a quote character";
    }

    // Structure, borrowed from the same WinHTTP parser the translator uses, so the dialog and
    // the child agree about what is well formed.
    const Result<ParsedServerUrl> parsed = parse_server_url(utf8::to_utf16_or_empty(std::string(url)));
    if (!parsed) return parsed.status().message();
    return {};
}

ServerMode migrate_server_mode(std::string_view url) noexcept {
    std::string lower;
    lower.reserve(url.size());
    for (const char c : url) {
        const auto u = static_cast<unsigned char>(c);
        lower.push_back(u < 0x80 ? static_cast<char>(std::tolower(u)) : c);
    }
    if (lower.rfind("ws://", 0) == 0 || lower.rfind("wss://", 0) == 0) {
        return ServerMode::Protocol;
    }
    // Empty, or anything else. Empty is the first-run Server page; "anything else" is an
    // `http://` URL that an older build would have treated as a WebSocket synonym, and which is
    // far more likely to be a speech service now that there is somewhere to put one. It is
    // re-validated on the next save rather than silently connected to.
    return ServerMode::Local;
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

    // normalizer.h owns its own rules; this is the one call site. The cadence is passed in
    // rather than read there, so that normalizer.h stays free of commit_engine.h and the rule
    // reads where it is enforced. normalise() has already forced `enabled` off under any other
    // cadence, so the cadence check below can only fire for a Settings built programmatically —
    // which is exactly the case that should not be able to skip it.
    if (const Status st =
            normalizer.validate(stability.cadence == Cadence::EndOfSession);
        !st.ok()) {
        return st;
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

    // An empty URL is legal and is the first-run state (docs/protocol.md §10.6). A URL that is
    // *present and malformed* is not: it would fail at connect time, in the tray, minutes after
    // the dialog that accepted it closed.
    //
    // Which parser applies depends on `mode`, and that is the whole point of the field: in Local
    // mode the URL is a speech service and `parse_server_url` would happily accept it as a
    // WebSocket synonym, which is the mistake openai-translator.md exists to end.
    if (!server.url.empty()) {
        if (server.mode == ServerMode::Local) {
            if (const std::string why = validate_speech_service_url(server.url); !why.empty()) {
                return Status{StatusCode::InvalidArgument, "server.url: " + why};
            }
        } else if (const Result<ParsedServerUrl> u =
                       parse_server_url(utf8::to_utf16_or_empty(server.url));
                   !u) {
            return Status{StatusCode::InvalidArgument, "server.url: " + u.status().message()};
        }
    }
    if (server.reconnect_max_ms < server.reconnect_initial_ms) {
        return Status{StatusCode::InvalidArgument,
                      "server.reconnect_max_ms must not be below reconnect_initial_ms"};
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
    // A sibling key rather than part of the spelling: `hotkey` names the keys and has to survive
    // HotkeySpec::parse() unchanged, which is only true while it says nothing about the gesture.
    put_bool(root, "push_to_talk", s.hotkey.push_to_talk);

    {
        // Always written, even when empty. A key that is present and blank says "there is no
        // server yet"; an absent key would be indistinguishable from a settings file written by
        // an older build, and the first-run path needs to tell those apart.
        json::Value o = json::Value::object();
        // `mode` is new and `kSettingsSchema` is deliberately NOT bumped for it: every settings
        // file written without it can be interpreted correctly from the URL it already holds
        // (see migrate_server_mode), and a bump would make those files unreadable for a field
        // that needs no migration dialog.
        put_str(o, "mode", to_string(s.server.mode));
        put_str(o, "url", s.server.url);
        put_str(o, "model", s.server.model);
        put_bool(o, "live_preview", s.server.live_preview);
        put_u64(o, "connect_timeout_ms", s.server.connect_timeout_ms);
        put_u64(o, "reconnect_initial_ms", s.server.reconnect_initial_ms);
        put_u64(o, "reconnect_max_ms", s.server.reconnect_max_ms);
        root.set("server", std::move(o));
    }

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
    {
        json::Value o = json::Value::object();
        put_bool(o, "enabled", s.normalizer.enabled);
        put_str(o, "url", s.normalizer.url);
        put_u64(o, "timeout_ms", s.normalizer.timeout_ms);
        put_u64(o, "max_bytes", static_cast<uint64_t>(s.normalizer.max_bytes));
        put_str(o, "styling", s.normalizer.styling);
        put_str(o, "structure", s.normalizer.structure);
        put_str(o, "context", s.normalizer.context);
        // `protect_terms` is deliberately NOT written: normalise() derives it from
        // `context_terms`, and persisting a derived field is how the two drift apart and the
        // file grows a second, stale answer to the same question.
        root.set("normalizer", std::move(o));
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

    if (const json::Value* v = root.find("server")) {
        Obj o(*v, "settings.server");
        o.str_opt("url", s.server.url);
        o.str_opt("model", s.server.model);
        // Absent `mode` means a settings file written before the translator existed. Infer it
        // from the URL rather than defaulting: defaulting to Local would hand a `ws://` address
        // to the speech-service validator, and defaulting to Protocol would hand an `http://`
        // one to `IpcClient::connect`. Both are wrong for somebody.
        std::string mode_text;
        o.str_opt("mode", mode_text);
        if (!mode_text.empty()) {
            if (!server_mode_from_string(mode_text, s.server.mode)) {
                return Status{StatusCode::Malformed,
                              "settings.server.mode: expected \"local\" or \"protocol\", got \"" +
                                  mode_text + "\""};
            }
        } else {
            s.server.mode = migrate_server_mode(s.server.url);
        }
        o.bool_opt("live_preview", s.server.live_preview);
        o.u32_opt("connect_timeout_ms", s.server.connect_timeout_ms, 1000, 120000);
        o.u32_opt("reconnect_initial_ms", s.server.reconnect_initial_ms, 100, 60000);
        o.u32_opt("reconnect_max_ms", s.server.reconnect_max_ms, 1000, 600000);
        if (o.failed()) return Status{StatusCode::Malformed, o.error()};
    }
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
        // AFTER the assignment, never before: `parsed` is a fresh spec carrying the field's
        // default, so reading the mode first and assigning second would silently discard it.
        top.bool_opt("push_to_talk", s.hotkey.push_to_talk);
        if (top.failed()) return Status{StatusCode::Malformed, top.error()};
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

    if (const json::Value* v = root.find("normalizer")) {
        Obj o(*v, "settings.normalizer");
        o.bool_opt("enabled", s.normalizer.enabled);
        o.str_opt("url", s.normalizer.url);
        o.u32_opt("timeout_ms", s.normalizer.timeout_ms, 100, 30000);
        uint64_t max_bytes = static_cast<uint64_t>(s.normalizer.max_bytes);
        o.u64_opt("max_bytes", max_bytes, 64, 65536);
        s.normalizer.max_bytes = static_cast<size_t>(max_bytes);
        o.str_opt("styling", s.normalizer.styling);
        o.str_opt("structure", s.normalizer.structure);
        o.str_opt("context", s.normalizer.context);
        if (o.failed()) return Status{StatusCode::Malformed, o.error()};
        // The control values are checked by NormalizerConfig::validate(), reached through
        // Settings::validate() below, so that the endpoint's vocabulary is enforced in exactly
        // one place rather than once here and once there.
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

bool settings_file_exists(const std::filesystem::path& file) {
    if (file.empty()) return false;
    std::error_code ec;
    return std::filesystem::is_regular_file(file, ec);
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
    kIdPushToTalk,
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
    kIdServerUrl,
    kIdServerModel,
    kIdServerTest,
    kIdServerStatus,
    kIdServerWarning,
    // openai-translator.md §4.6. Two radios rather than a scheme sniffer, because `http://` is a
    // legal WebSocket synonym (docs §2.6.1) and there is no way to tell the two meanings apart
    // by looking at the URL — which is the whole reason `ServerMode` exists.
    kIdServerModeLocal,
    kIdServerModeProtocol,
    kIdServerLivePreview,
    // The rewriter (normalizer.h). On the compose-buffer page rather than the server one, even
    // though it is an address the user types: it rewrites **the pill** and nothing else, and its
    // one hard precondition — the end-of-session cadence — is the same one that page already
    // spends a paragraph explaining. A user who has just read why the pill exists is the user
    // who can decide whether they want it rewritten.
    //
    // **One check box and one line of status, on the Server page.** That is the whole surface,
    // and it is a decision rather than an omission.
    //
    // This started as its own tab with the address, `styling`, `structure`, `context` and
    // `timeout_ms` on it, which is what the settings block contains — and every one of those
    // controls was another paragraph standing between the user and the only question they
    // actually have, which is *clean it up or not*. The address in particular was asking them to
    // go and look up something they had already typed into the box directly above.
    //
    // So: a tick box, and one address that fills itself in. `styling`, `structure`, `context`,
    // `timeout_ms` and `max_bytes` keep their defaults, stay in `settings.json` for the
    // deployment that needs them, and round-trip untouched through the working copy this dialog
    // edits. docs/setup.md §3.1 says where they are. A dialog that has to be read is a dialog
    // that does not get used.
    //
    // The address **follows the speech service** — `normalizer_url_from_server`, the same host
    // and port with `/v1/text/normalize` on the end, which is how the reference deployment runs
    // — and keeps following it while the user leaves it alone, so moving the service moves this
    // with it and there is nothing to notice. Typing in the box stops it following, permanently
    // and for that address only: the text model on a different machine from the recogniser is a
    // real deployment, it is just not the common one, and it should cost one edit rather than a
    // page. `DialogState::normalizer_url_follows` is where "left it alone" is remembered.
    kIdNormalizerEnabled,
    kIdNormalizerUrl,
    kIdNormalizerTest,
    kIdNormalizerStatus,
};

constexpr int kPageCount = 5;

// Posted by the Test worker when it has an answer. wParam is the generation the request was
// issued under, so a reply for a URL the user has since edited is discarded instead of
// overwriting the newer one.
constexpr UINT kMsgProbeDone = WM_APP + 41;

// The same shape for the rewriter's own Test button, on its own generation counter so the two
// cannot answer into each other's status line.
constexpr UINT kMsgNormalizeTestDone = WM_APP + 42;

struct DialogState {
    Settings settings;                       // the working copy; only copied out on OK
    const ModelManifest* manifest = nullptr;
    std::vector<audio::DeviceInfo> devices;  // parallel to the audio combo's items, minus item 0
    std::vector<HWND> page_controls[kPageCount];
    HFONT font = nullptr;
    int dpi = 96;

    // ── The Test button (release-plan.md §2.2) ──────────────────────────────────────────────
    //
    // probe_server() blocks for as long as a cold model load takes, which can be minutes, so it
    // runs on a worker. The worker owns a copy of everything it needs and posts a string back;
    // it never touches this struct, because the dialog may have been destroyed by then.
    uint32_t probe_generation = 0;
    bool probe_running = false;
    uint32_t normalize_test_generation = 0;
    bool normalize_test_running = false;

    //: One line of `font`, in *logical* px, measured rather than assumed.
    //:
    //: The explanatory notes under each section are plain STATIC controls given a fixed height of
    //: `lines` × this. The height used to be the literal 13, which is smaller than Segoe UI 9pt
    //: actually occupies at 96 DPI — so the last line of every multi-line note was cut in half,
    //: and the dialog read as broken text rather than as a layout that had run out of room. It is
    //: measured because the whole point of laying labels out above their controls (see `Cursor`)
    //: is to survive a font or DPI this file did not anticipate.
    int line_h = 15;

    // The tallest column, in logical px, filled in by `Cursor::mark()` while the controls are
    // built. The dialog is then sized from it (`fit_page_height`) instead of from a constant.
    int content_h = 0;
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
    int top_y = 0;  // where this column started, so `mark()` measures content and not position
    int w = 0;

    // The tallest any column has reached, in logical px below `top`. Recorded rather than
    // assumed, because the page height used to be a constant tuned against one font size and
    // every column's real height is a function of `st->line_h`, which is *measured* — so on a
    // machine whose message font is a point taller, prose that fitted here silently ran into the
    // button row. See `fit_page_height` for what is done with it.
    void mark() {
        if (st != nullptr && y - top_y > st->content_h) st->content_h = y - top_y;
    }

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
        mark();
        return c;
    }

    void gap(int logical = 8) {
        y += px(*st, logical);
        mark();
    }

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

    // BS_AUTORADIOBUTTON groups by tab order: the first of a run carries WS_GROUP, and Windows
    // then unchecks its siblings for us. Getting the group wrong is how two radios end up both
    // checked, so the caller says which one starts the run.
    HWND radio(const wchar_t* label, int id, bool first_in_group) {
        HWND h = add(L"BUTTON", label,
                     WS_TABSTOP | BS_AUTORADIOBUTTON | (first_in_group ? WS_GROUP : 0u), 19, id);
        gap(3);
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

// The grey example text an empty EDIT shows. This is where "for example
// http://strixhalo.lan:10301" belongs: in the box it is an example *of*, shown exactly while the
// box is empty and gone the moment it is not — rather than as a paragraph underneath that is
// still there, still being read, long after the user has filled the box in.
//
// TRUE, so it stays visible when the box has focus: the user clicks into an empty box and the
// example is the thing they most want to still be able to see.
void set_cue(HWND edit, const wchar_t* text) {
    if (edit != nullptr) SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(text));
}

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

// ─── The Server page (release-plan.md §2.2) ─────────────────────────────────────────────────

// docs/protocol.md §2.8: a sentence, never a block. The user's network is not this program's to
// adjudicate, and an app that refused to talk to the endpoint its owner named would simply be
// replaced by one that did. What it is for is the realistic case, not an attack: somebody types
// a public hostname into a URL box, and dictates a password into it within a week.
// Which of the two things the URL box currently means. Read from the controls rather than from
// the working copy, because the user may have just clicked the other radio.
ServerMode mode_of(HWND dlg) {
    return IsDlgButtonChecked(dlg, kIdServerModeProtocol) == BST_CHECKED ? ServerMode::Protocol
                                                                        : ServerMode::Local;
}

// The label and the example change with the mode, because "Server address" over a box that
// wants `http://strixhalo.lan:10301` is how the two get confused in the first place.
void update_server_mode_ui(HWND dlg) {
    const bool local = mode_of(dlg) == ServerMode::Local;
    set_cue(GetDlgItem(dlg, kIdServerUrl),
            local ? L"http://strixhalo.lan:10301" : L"ws://127.0.0.1:10402/");
    // Live preview is a translator setting, so it means nothing when the translator is not the
    // thing being talked to. Disabled rather than hidden: a control that vanishes is a control
    // the user cannot find again.
    EnableWindow(GetDlgItem(dlg, kIdServerLivePreview), local ? TRUE : FALSE);
}

std::wstring server_warning_for(HWND dlg, const std::wstring& url) {
    if (url.empty()) return {};
    if (mode_of(dlg) == ServerMode::Local) {
        if (const std::string why = validate_speech_service_url(narrow(url)); !why.empty()) {
            return L"⚠  " + wide(why);
        }
    }
    const Result<ParsedServerUrl> u = parse_server_url(url);
    if (!u) return L"⚠  " + wide(u.status().message());
    if (!u->secure && !host_is_private(u->host)) {
        return L"⚠  Not a private address, and unencrypted: your audio crosses the network "
               L"in the clear. Use a network you control.";
    }
    return {};
}

void update_server_warning(HWND dlg) {
    const std::wstring warning = server_warning_for(dlg, get_text(GetDlgItem(dlg, kIdServerUrl)));
    SetWindowTextW(GetDlgItem(dlg, kIdServerWarning), warning.c_str());
}

// One line per fact, because this text is what a user will paste into a support conversation.
std::wstring describe_probe(const ServerProbe& p) {
    std::wstring out = L"Connected.\r\n";
    out += L"Server: ";
    out += p.server_build.empty() ? L"(build id not reported)" : wide(p.server_build);
    out += L"\r\nModel: " + wide(p.model) + L"  (" + wide(proto::to_string(p.mode)) + L")";
    if (p.chunk_ms != 0) out += L", " + std::to_wstring(p.chunk_ms) + L" ms chunks";
    out += p.context_supported ? L"\r\nVocabulary hints: supported"
                               : L"\r\nVocabulary hints: not supported by this model";
    out += L"\r\nConnected in " + std::to_wstring(p.connect_ms) + L" ms, ready in " +
           std::to_wstring(p.ready_ms) + L" ms.";
    if (p.models.empty()) {
        out += L"\r\nThis server does not advertise a model list, so the model box is free "
               L"text. Leave it empty to take whatever it serves.";
    } else {
        out += L"\r\nModels offered:";
        for (const proto::ModelInfo& mi : p.models) {
            out += L"\r\n    " + wide(mi.id);
            if (mi.mode.has_value()) {
                out += L"  (";
                out += wide(proto::to_string(*mi.mode));
                out += L")";
            }
        }
    }
    return out;
}

void start_server_test(HWND dlg, DialogState& st) {
    if (st.probe_running) return;

    const ServerMode mode = mode_of(dlg);
    const std::wstring url = get_text(GetDlgItem(dlg, kIdServerUrl));
    const std::string model = narrow(get_text(GetDlgItem(dlg, kIdServerModel)));
    const bool live_preview = IsDlgButtonChecked(dlg, kIdServerLivePreview) == BST_CHECKED;

    if (url.empty()) {
        SetWindowTextW(GetDlgItem(dlg, kIdServerStatus),
                       mode == ServerMode::Local
                           ? L"Type your speech service's address first, for example "
                             L"http://strixhalo.lan:10301"
                           : L"Type a server address first, for example ws://127.0.0.1:10402/");
        return;
    }
    if (mode == ServerMode::Local) {
        if (const std::string why = validate_speech_service_url(narrow(url)); !why.empty()) {
            SetWindowTextW(GetDlgItem(dlg, kIdServerStatus), wide(why).c_str());
            return;
        }
    } else if (const Result<ParsedServerUrl> u = parse_server_url(url); !u) {
        SetWindowTextW(GetDlgItem(dlg, kIdServerStatus), wide(u.status().message()).c_str());
        return;
    }

    st.probe_running = true;
    const uint32_t generation = ++st.probe_generation;
    SetWindowTextW(GetDlgItem(dlg, kIdServerStatus),
                   L"Connecting… (a service loading a model for the first time can take a "
                   L"while; this waits up to a minute)");
    EnableWindow(GetDlgItem(dlg, kIdServerTest), FALSE);

    // ── What Test actually does, which is the point of it ───────────────────────────────────
    //
    // **It always speaks the protocol to whatever this process will really connect to.** In
    // Local mode that means launching a *private* `kvit-asrd.exe` on its own port, completing
    // the handshake with it, and waiting for `ready` — not POSTing to
    // /v1/audio/transcriptions directly, which would pass while the translator was broken.
    //
    // And because asrd does not emit `ready` unless its own TCP connect to the speech host
    // succeeded (openai-translator.md §4.2), a green Test means the whole path answered: this
    // client, the translator, and the speech service. That is the property the button is worth
    // having for.
    //
    // The worker owns everything it touches and hands back one heap-allocated string. It must
    // not read DialogState: by the time it finishes, the dialog may be gone. PostMessage to a
    // destroyed window fails harmlessly, and the string is freed by whoever wins.
    std::thread([dlg, generation, mode, url, model, live_preview]() {
        std::wstring text;
        // Declared here so the child outlives the probe and dies with this scope: its destructor
        // closes the job, which kills it. A Test that left a translator running would collide
        // with the real one the next time dictation started.
        Translator probe_child;

        ServerEndpoint ep;
        ep.model = model;
        bool launched = true;
        if (mode == ServerMode::Local) {
            TranslatorSpec spec;
            spec.service_url = narrow(url);
            spec.model = model;
            spec.live_preview = live_preview;
            if (const Result<std::string> started = probe_child.start(spec); started) {
                ep.url = wide(*started);
                text = L"Started the translator on " + ep.url + L"\r\n";
            } else {
                launched = false;
                text = L"Could not start the translator.\r\n" + wide(started.status().message());
            }
        } else {
            ep.url = url;
        }

        if (launched) {
            if (const Result<ServerProbe> r = probe_server(ep, 60000); r) {
                text += describe_probe(*r);
                if (mode == ServerMode::Local) {
                    text += L"\r\nThe translator and your speech service both answered. "
                            L"kvit-dictate starts this for you when you dictate; there is "
                            L"nothing else to run.";
                }
            } else {
                text += L"Could not use this endpoint.\r\n" + wide(r.status().message());
                if (mode == ServerMode::Local) {
                    text += L"\r\nThe translator started but did not become ready, which "
                            L"usually means it could not reach the speech service at that "
                            L"address. Its own diagnostics are in this app's log, prefixed "
                            L"\"asrd:\".";
                }
            }
        }

        auto* payload = new std::wstring(std::move(text));
        if (!PostMessageW(dlg, kMsgProbeDone, static_cast<WPARAM>(generation),
                          reinterpret_cast<LPARAM>(payload))) {
            delete payload;
        }
    }).detach();
}

// ─── Populating the controls from a Settings ────────────────────────────────────────────────

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
                   eos ? L""
                       : L"Needs the \"end-of-session\" cadence, on the Dictation tab: with any "
                         L"other one the text is already in your document as you speak.");
}

// ─── The rewriter (normalizer.h), in two controls ───────────────────────────────────────────

// The normalize route on whatever machine the user already named as their speech service.
//
// Asking a second time for an address the user typed into the box directly above is asking them
// to go and look it up, and the answer is the same host and port with a different route on the
// end — that is how the reference deployment runs (docs/setup.md §2.2: one box serving
// `/v1/audio/transcriptions` and `/v1/text/normalize`). So there is no address field: this
// derives it, and a deployment that really does split the two models across machines writes
// `normalizer.url` in the file, which is honoured ahead of this.
//
// Empty when the speech service is not an HTTP address: in Protocol mode `url` is a `ws://`
// kvit-dictate server (openai-translator.md §4.6), which has no such route and never will.
std::wstring normalizer_url_from_server(HWND dlg) {
    if (mode_of(dlg) != ServerMode::Local) return {};
    std::wstring url = get_text(GetDlgItem(dlg, kIdServerUrl));
    while (!url.empty() && (url.front() == L' ' || url.front() == L'	')) url.erase(0, 1);
    while (!url.empty() && (url.back() == L' ' || url.back() == L'	' || url.back() == L'/')) {
        url.pop_back();
    }
    if (url.rfind(L"http://", 0) != 0 && url.rfind(L"https://", 0) != 0) return {};
    return url + L"/v1/text/normalize";
}

// True when `normalizer.url` in the file is something the user chose rather than something a
// previous OK derived from the speech service. The test is that it is not what the *stored*
// speech-service address derives to; anything else is theirs and is never overwritten.
bool stored_normalizer_url_is_custom(const DialogState& st) {
    if (st.settings.normalizer.url.empty()) return false;
    std::string server = st.settings.server.url;
    while (!server.empty() && server.back() == '/') server.pop_back();
    if (server.empty()) return true;
    return st.settings.normalizer.url != server + "/v1/text/normalize";
}

// The same rule as refresh_editing_page above, for the same reason and with the same shape: a
// box the user can tick, whose value the loader then quietly reverts, is worse than a box that
// says why it is greyed.
//
// `normalizer.enabled` has one hard precondition — Cadence::EndOfSession — enforced in three
// places that must agree: NormalizerConfig::validate() refuses the file, Settings::normalise()
// forces the flag off, and App::pump_normalize() refuses to send. It has one soft one: there
// has to be an address, which here means an HTTP speech service to derive one from, or a
// `normalizer.url` already in the file.
void refresh_normalizer_controls(HWND dlg) {
    const bool eos = SendMessageW(GetDlgItem(dlg, kIdCadence), CB_GETCURSEL, 0, 0) ==
                     cadence_index(Cadence::EndOfSession);
    const std::wstring derived = normalizer_url_from_server(dlg);
    // Which address this would actually use: the one in the file when the user put it there by
    // hand, otherwise the one derived from whatever is in the server box *now*. Same test as
    // read_controls, so the line the user reads and the value that gets written agree.
    const bool have_url =
        !derived.empty() || !get_text(GetDlgItem(dlg, kIdNormalizerUrl)).empty();
    const bool available = eos && have_url;
    EnableWindow(GetDlgItem(dlg, kIdNormalizerEnabled), available ? TRUE : FALSE);

    const bool on = available && IsDlgButtonChecked(dlg, kIdNormalizerEnabled) == BST_CHECKED;
    HWND url_box = GetDlgItem(dlg, kIdNormalizerUrl);
    EnableWindow(url_box, on ? TRUE : FALSE);
    EnableWindow(GetDlgItem(dlg, kIdNormalizerTest), on ? TRUE : FALSE);
    // An empty box means "the speech service above", and the cue banner is what says so — it
    // shows the address that resolves to, and follows that address as it is edited. No flag to
    // keep, nothing written into the box behind the user, and clearing the box is how you go
    // back to following.
    set_cue(url_box, derived.c_str());

    // Only ever the reason it is greyed. When it is available there is nothing to say that the
    // box and the address above it do not already say.
    std::wstring status;
    if (!eos) {
        status = L"Needs the \"end-of-session\" cadence, on the Dictation tab.";
    } else if (!have_url) {
        status = L"Needs a speech service above.";
    }
    SetWindowTextW(GetDlgItem(dlg, kIdNormalizerStatus), status.c_str());
}

// The rewriter's Test button.
//
// It sends one short sentence with a filler and a spoken date in it and shows what comes back,
// because that answers the question the user actually has — *is this thing going to do what I
// want to my text* — in a way that "connected OK" does not. `normalize_once` is the worker's own
// POST, so a green answer here is the same call dictation will make.
//
// Same worker-thread shape as start_server_test: everything it touches is copied in, it reads no
// DialogState, and it hands back one heap-allocated string that whoever wins frees.
void start_normalize_test(HWND dlg, DialogState& st) {
    if (st.normalize_test_running) return;

    NormalizerConfig cfg = st.settings.normalizer;
    const std::wstring typed = get_text(GetDlgItem(dlg, kIdNormalizerUrl));
    cfg.url = narrow(typed.empty() ? normalizer_url_from_server(dlg) : typed);
    cfg.enabled = true;
    if (cfg.url.rfind("http://", 0) != 0 && cfg.url.rfind("https://", 0) != 0) {
        SetWindowTextW(GetDlgItem(dlg, kIdNormalizerStatus),
                       cfg.url.empty()
                           ? L"No address: give the speech service above an http:// address, or "
                             L"type one here."
                           : L"The address must begin with http:// or https://.");
        return;
    }

    st.normalize_test_running = true;
    const uint32_t generation = ++st.normalize_test_generation;
    SetWindowTextW(GetDlgItem(dlg, kIdNormalizerStatus), L"Sending a test sentence…");
    EnableWindow(GetDlgItem(dlg, kIdNormalizerTest), FALSE);

    std::thread([dlg, generation, cfg]() {
        static constexpr char kProbeText[] =
            "um so i think we should uh ship it on friday the third";
        const uint64_t began = SteadyClock::instance().now_ms();
        const Result<std::string> answer = normalize_once(cfg, kProbeText);
        const uint64_t elapsed = SteadyClock::instance().now_ms() - began;

        std::wstring text;
        if (answer) {
            text = L"Answered in " + std::to_wstring(elapsed) + L" ms.\r\n\r\n\x201c" +
                   wide(kProbeText) + L"\x201d\r\nbecomes\r\n\x201c" + wide(*answer) +
                   L"\x201d";
        } else {
            text = L"No answer from " + wide(cfg.url) + L"\r\n" +
                   wide(answer.status().message());
        }

        auto* payload = new std::wstring(std::move(text));
        if (!PostMessageW(dlg, kMsgNormalizeTestDone, static_cast<WPARAM>(generation),
                          reinterpret_cast<LPARAM>(payload))) {
            delete payload;
        }
    }).detach();
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

// ─── Check boxes and radio buttons, drawn by hand, and only in dark mode ────────────────────
//
// **The same hole as the push buttons, found the same way and fixed differently.** A BUTTON in
// the check-box or radio family draws its own label, and once it is themed it does so with
// DrawThemeText and the *theme's* colour — so the parent's WM_CTLCOLORSTATIC reply, which is
// what makes every STATIC on this dialog light, is ignored. `DarkMode_Explorer` darkens the
// glyph and leaves the text at the light theme's near-black, which on our dark face is
// unreadable. Reported from use as "text is black on dark background"; it was true of every
// check box on every page, and the two new radios on the Server page merely made it obvious.
//
// Why a subclass and not BS_OWNERDRAW, which is what the push buttons use: BS_OWNERDRAW occupies
// the same style bits as BS_AUTOCHECKBOX and BS_AUTORADIOBUTTON, so turning it on takes the
// *auto* with it — the control stops toggling itself, stops answering BM_GETCHECK usefully, and
// every IsDlgButtonChecked call in this file would have to be replaced by state kept by hand.
// That is a large change to make for a colour. Painting inside a subclass leaves the style, the
// toggle, the keyboard handling and the group behaviour exactly as they were, and changes only
// the pixels.
//
// The glyph still comes from the theme, opened through the "DarkMode_Explorer::Button" class
// list so the dark check and the dark radio are Windows' own rather than something drawn here
// that will not match the next release. DrawFrameControl is the fallback when that class does
// not exist, which is a build old enough to have no dark mode at all.

void paint_check_or_radio(HWND hwnd, DialogState& st) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc{};
    GetClientRect(hwnd, &rc);
    if (st.face_brush != nullptr) FillRect(dc, &rc, st.face_brush);

    const theme::DialogPalette& p = theme::dialog_palette(true);
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR type = style & BS_TYPEMASK;
    const bool radio = type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
    const bool checked = SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool disabled = IsWindowEnabled(hwnd) == FALSE;

    // 13 logical px is the size Windows has drawn these at since the beginning; taking it from
    // the theme would be better and GetThemePartSize needs a theme we may not have.
    const int box = px(st, 13);
    const int top = rc.top + ((rc.bottom - rc.top) - box) / 2;
    RECT glyph{rc.left, top, rc.left + box, top + box};

    HTHEME theme = OpenThemeData(hwnd, L"DarkMode_Explorer::Button");
    if (theme != nullptr) {
        const int part = radio ? BP_RADIOBUTTON : BP_CHECKBOX;
        int state;
        if (radio) {
            state = checked ? (disabled ? RBS_CHECKEDDISABLED : RBS_CHECKEDNORMAL)
                            : (disabled ? RBS_UNCHECKEDDISABLED : RBS_UNCHECKEDNORMAL);
        } else {
            state = checked ? (disabled ? CBS_CHECKEDDISABLED : CBS_CHECKEDNORMAL)
                            : (disabled ? CBS_UNCHECKEDDISABLED : CBS_UNCHECKEDNORMAL);
        }
        DrawThemeBackground(theme, dc, part, state, &glyph, nullptr);
        CloseThemeData(theme);
    } else {
        UINT flags = radio ? DFCS_BUTTONRADIO : DFCS_BUTTONCHECK;
        if (checked) flags |= DFCS_CHECKED;
        if (disabled) flags |= DFCS_INACTIVE;
        DrawFrameControl(dc, &glyph, DFC_BUTTON, flags);
    }

    // The label, in the colour the rest of the dialog uses — which is the whole point of this
    // function. Disabled takes the palette's own grey rather than COLOR_GRAYTEXT, which is tuned
    // for a light face and disappears on a dark one.
    RECT text{glyph.right + px(st, 6), rc.top, rc.right, rc.bottom};
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? p.disabled_text : p.text);
    HGDIOBJ previous_font = SelectObject(dc, st.font);
    wchar_t label[256]{};
    GetWindowTextW(hwnd, label, ARRAYSIZE(label));
    DrawTextW(dc, label, -1, &text,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    // Keyboard focus. Windows draws this for us on an unowned button and cannot here, and a
    // dialog you can tab through without seeing where you are is worse than one that is light.
    if ((SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) == 0 &&
        GetFocus() == hwnd) {
        RECT focus = text;
        DrawTextW(dc, label, -1, &focus, DT_LEFT | DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
        focus.top = rc.top;
        focus.bottom = rc.bottom;
        InflateRect(&focus, px(st, 1), 0);
        SetTextColor(dc, p.text);
        DrawFocusRect(dc, &focus);
    }

    SelectObject(dc, previous_font);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK check_radio_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                                      DWORD_PTR ref) {
    auto* st = reinterpret_cast<DialogState*>(ref);
    switch (msg) {
        case WM_PAINT:
            // Inert in light mode, like the tab strip's subclass and the button hover one: the
            // system's own painting is right there and a hand-drawn imitation of it could only
            // differ.
            if (st != nullptr && st->dark) {
                paint_check_or_radio(hwnd, *st);
                return 0;
            }
            break;

        case WM_ERASEBKGND:
            // The paint above fills the whole client rect. Letting the default erase run first
            // is a visible flash on every toggle.
            if (st != nullptr && st->dark) return 1;
            break;

        case BM_SETCHECK:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_ENABLE:
            // A state the paint reads but Windows does not know we are reading. Repainted after
            // the default handler has actually changed it.
            if (st != nullptr && st->dark) {
                const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
                InvalidateRect(hwnd, nullptr, FALSE);
                return r;
            }
            break;

        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, check_radio_subclass, id);
            break;

        default:
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

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

    // Every check box and radio button on every page. Enumerated rather than listed by id: this
    // dialog has fourteen of them across five pages, a list would go stale the first time one was
    // added, and "the one nobody updated the list for" is precisely the control that would still
    // be drawing black text on a dark face.
    //
    // Installed once and left in place, like the two subclasses above: it is inert while `dark`
    // is false, and adding and removing it on every scheme change is one more thing to get wrong
    // for no gain.
    EnumChildWindows(
        dlg,
        [](HWND child, LPARAM param) -> BOOL {
            wchar_t cls[32]{};
            if (GetClassNameW(child, cls, ARRAYSIZE(cls)) == 0) return TRUE;
            if (_wcsicmp(cls, L"Button") != 0) return TRUE;
            const LONG_PTR type = GetWindowLongPtrW(child, GWL_STYLE) & BS_TYPEMASK;
            if (type == BS_AUTOCHECKBOX || type == BS_CHECKBOX || type == BS_AUTORADIOBUTTON ||
                type == BS_RADIOBUTTON) {
                SetWindowSubclass(child, check_radio_subclass, 1, static_cast<DWORD_PTR>(param));
                InvalidateRect(child, nullptr, TRUE);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&st));

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
    CheckDlgButton(dlg, kIdPushToTalk, s.hotkey.push_to_talk ? BST_CHECKED : BST_UNCHECKED);

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

    // The rewriter (docs/normalizer.md). Also after the cadence combo, and for the same reason.
    CheckDlgButton(dlg, kIdNormalizerEnabled, s.normalizer.enabled ? BST_CHECKED : BST_UNCHECKED);
    SetWindowTextW(GetDlgItem(dlg, kIdNormalizerUrl),
                   stored_normalizer_url_is_custom(st) ? wide(s.normalizer.url).c_str() : L"");
    refresh_normalizer_controls(dlg);

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

    // Server.
    CheckDlgButton(dlg, kIdServerModeLocal,
                   s.server.mode == ServerMode::Local ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, kIdServerModeProtocol,
                   s.server.mode == ServerMode::Protocol ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, kIdServerLivePreview, s.server.live_preview ? BST_CHECKED : BST_UNCHECKED);
    SetWindowTextW(GetDlgItem(dlg, kIdServerUrl), wide(s.server.url).c_str());
    SetWindowTextW(GetDlgItem(dlg, kIdServerModel), wide(s.server.model).c_str());
    update_server_mode_ui(dlg);
    SetWindowTextW(GetDlgItem(dlg, kIdServerStatus),
                   s.server.configured()
                       ? L"Press Test to check that this address answers."
                       : L"No server is configured yet, so dictation is switched off. Type an "
                         L"address above and press Test.");
    update_server_warning(dlg);

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

    // The server, first, because it is the field most likely to be wrong and the one whose
    // rejection has to name the mistake rather than the field. An empty box is accepted: it is
    // how a user switches dictation off, and it is the first-run state (docs/protocol.md §10.6).
    {
        std::wstring url = get_text(GetDlgItem(dlg, kIdServerUrl));
        while (!url.empty() && (url.front() == L' ' || url.front() == L'	')) url.erase(0, 1);
        while (!url.empty() && (url.back() == L' ' || url.back() == L'	')) url.pop_back();
        const ServerMode mode = mode_of(dlg);
        if (!url.empty()) {
            // Which parser applies is the mode's whole job. In Local mode `parse_server_url`
            // would accept `http://` as a WebSocket synonym and the dialog would cheerfully
            // save an address the client then tries to upgrade.
            std::string why;
            if (mode == ServerMode::Local) {
                why = validate_speech_service_url(narrow(url));
            } else if (const Result<ParsedServerUrl> u = parse_server_url(url); !u) {
                why = u.status().message();
            }
            if (!why.empty()) {
                MessageBoxW(dlg,
                            ((mode == ServerMode::Local ? L"Speech service: " : L"Server address: ") +
                             wide(why))
                                .c_str(),
                            L"kvit-dictate settings", MB_OK | MB_ICONWARNING);
                SetFocus(GetDlgItem(dlg, kIdServerUrl));
                return false;
            }
        }
        s.server.mode = mode;
        s.server.url = narrow(url);
        s.server.model = narrow(get_text(GetDlgItem(dlg, kIdServerModel)));
        s.server.live_preview = IsDlgButtonChecked(dlg, kIdServerLivePreview) == BST_CHECKED;
    }

    const int hk_sel = static_cast<int>(SendMessageW(GetDlgItem(dlg, kIdHotkey), CB_GETCURSEL, 0, 0));
    const std::vector<std::pair<std::string, HotkeySpec>> presets = hotkey_presets();
    if (hk_sel >= 0 && static_cast<size_t>(hk_sel) < presets.size()) {
        s.hotkey = presets[static_cast<size_t>(hk_sel)].second;
    }  // else: the hand-set binding at the end of the list — leave s.hotkey as it was
    // After the preset assignment, which overwrites the whole spec: the presets are keys, and
    // the mode is the checkbox's to decide whichever branch above ran.
    s.hotkey.push_to_talk = IsDlgButtonChecked(dlg, kIdPushToTalk) == BST_CHECKED;

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

    // The rewriter. `enabled` as the user left it, forced off by normalise() if the cadence
    // does not permit it — the same division of labour as the compose buffer above.
    //
    // The address is derived rather than asked for, and re-derived here so that moving the
    // speech service moves the rewriter with it. It is only overwritten when it is empty or
    // when it is still exactly what the *old* speech-service address derived to: an address the
    // user put in the file by hand is theirs, and following the server box would silently throw
    // it away. `styling`, `structure`, `context`, `timeout_ms` and `max_bytes` have no controls
    // and survive untouched, because this function starts from the working copy.
    {
        s.normalizer.enabled = IsDlgButtonChecked(dlg, kIdNormalizerEnabled) == BST_CHECKED;
        std::wstring typed = get_text(GetDlgItem(dlg, kIdNormalizerUrl));
        while (!typed.empty() && (typed.front() == L' ' || typed.front() == L'	')) {
            typed.erase(0, 1);
        }
        while (!typed.empty() && (typed.back() == L' ' || typed.back() == L'	')) typed.pop_back();
        // Empty means the speech service above, which is what the cue banner has been showing.
        s.normalizer.url = narrow(typed.empty() ? normalizer_url_from_server(dlg) : typed);

        if (s.normalizer.enabled && s.stability.cadence == Cadence::EndOfSession) {
            const bool http = s.normalizer.url.rfind("http://", 0) == 0 ||
                              s.normalizer.url.rfind("https://", 0) == 0;
            if (!http) {
                MessageBoxW(dlg,
                            s.normalizer.url.empty()
                                ? L"Cleaning the text up needs a service to do it. Either give "
                                  L"the speech service above an http:// address, or type one for "
                                  L"the text service."
                                : L"The text service address must begin with http:// or https://.",
                            L"kvit-dictate settings", MB_OK | MB_ICONWARNING);
                SetFocus(GetDlgItem(dlg, kIdNormalizerUrl));
                return false;
            }
        }
    }

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

// The *minimum* page height, and no longer the actual one.
//
// It used to be exactly the height, and that was a latent bug in every note on every page: the
// content's real height is a function of `DialogState::line_h`, which is **measured** from the
// system message font at run time, while this was a constant tuned against one machine's
// measurement. A font a point taller — a different DPI, a different Windows language, a user who
// changed the message font — pushed the bottom of a column into the button row, silently. It was
// visible on the Overlay page (whose own comment records it as "419 of 430" and which measures
// 451 at line_h = 17) long before the Server page made it obvious.
//
// `fit_page_height` now grows the dialog to whatever the tallest column actually needs.
constexpr int kPageHeightMin = 430;
constexpr int kMargin = 12;
constexpr int kTabStrip = 30;

// The page height this dialog will actually use: the tallest column, never less than the
// historical minimum so that a sparse page does not produce a stubby window.
//
// Deliberately not clamped to the screen here. A column taller than the work area is a content
// problem, and silently clipping it is exactly the failure this replaces; `create_dialog` warns
// and lets the window be tall, which at least leaves every control reachable by moving it.
int page_height(const DialogState& st) {
    return st.content_h > kPageHeightMin ? st.content_h : kPageHeightMin;
}

void build_controls(HWND dlg, DialogState& st) {
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dlg, GWLP_HINSTANCE));

    st.content_h = 0;
    const int col_w = px(st, (kPageWidth - kMargin * 3) / 2);
    const int col1_x = px(st, kMargin * 2);
    const int col2_x = col1_x + col_w + px(st, kMargin);
    const int top = px(st, kMargin + kTabStrip + kMargin);

    // ── Page 0: the server (release-plan.md §2.2) ───────────────────────────────────────────
    //
    // First, because it is the only page a new install cannot work without. Everything else in
    // this dialog tunes behaviour; this one decides whether there is any.
    {
        Cursor c{dlg, inst, &st, 0, col1_x, top, top, col_w};
        c.section(L"Speech service");
        // The address box comes FIRST and the radios after it, because for most people the only
        // thing to do on this page is type one address and press Test. The second radio is the
        // escape hatch for somebody who already runs a protocol server, not a choice everyone
        // has to make.
        c.add(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 21, kIdServerUrl,
              WS_EX_CLIENTEDGE);
        c.gap(8);

        c.radio(L"OpenAI-compatible speech service", kIdServerModeLocal, true);
        c.radio(L"kvit-dictate service", kIdServerModeProtocol, false);
        c.gap(6);

        set_cue(c.label_edit(L"Model (optional)", kIdServerModel),
                L"whatever the service offers");
        c.add(L"BUTTON", L"Test", WS_TABSTOP | BS_PUSHBUTTON, 26, kIdServerTest);
        c.gap(6);
        // Seven lines, not nine. `describe_probe` writes six on a good answer and the box scrolls
        // nothing, so the last two were dead space paid for on *every* page — the dialog is one
        // size for all five, so the tallest column anywhere sets the height everywhere.
        c.add(L"STATIC", L"", 0, st.line_h * 7, kIdServerStatus);
    }
    {
        Cursor c{dlg, inst, &st, 0, col2_x, top, top, col_w};
        c.section(L"Live preview");
        c.check(L"Show words as they are recognised", kIdServerLivePreview);
        c.gap(6);

        // ── The rewriter (docs/normalizer.md) ───────────────────────────────────────────────
        //
        // One check box, next to the other thing that service does for you, because that is the
        // whole decision. Everything else about it — the route, the styling, the timeout — is
        // derived or defaulted (see kIdNormalizerEnabled).
        c.section(L"Clean-up");
        c.check(L"Remove filler words and add punctuation", kIdNormalizerEnabled);
        // Empty means "use the speech service above", and the cue banner shows the address that
        // resolves to, so the common case is a box nobody has to touch and the split-machine
        // deployment costs one edit.
        c.label_edit(L"Text service (optional)", kIdNormalizerUrl);
        c.add(L"BUTTON", L"Test", WS_TABSTOP | BS_PUSHBUTTON, 26, kIdNormalizerTest);
        c.gap(6);
        c.add(L"STATIC", L"", 0, st.line_h * 4, kIdNormalizerStatus);
        c.gap(6);

        // No heading and no prose: it is empty unless the address is one worth warning about,
        // and a permanent paragraph about the network is not a setting.
        c.add(L"STATIC", L"", 0, st.line_h * 4, kIdServerWarning);
    }

    // ── Page 1: the compose buffer (docs/editing.md) ────────────────────────────────────────
    {
        Cursor c{dlg, inst, &st, 1, col1_x, top, top, col_w};
        c.section(L"Compose buffer");
        c.check(L"Let me edit the pill before it is typed in", kIdEditingEnabled);
        c.note(L"Enter types it in, Esc throws it away.", 1);
        c.add(L"STATIC", L"", 0, st.line_h * 3, kIdEditingStatus);
    }
    {
        Cursor c{dlg, inst, &st, 1, col2_x, top, top, col_w};
        c.section(L"Keyboard");
        c.label_combo(L"Any other key (Ctrl+S, F5, Alt+Tab)", kIdEditingForeign);

        c.section(L"Mouse");
        c.check(L"Let me click and scroll inside the pill", kIdEditingMouse);
    }

    // ── Page 1: dictation ───────────────────────────────────────────────────────────────────
    {
        Cursor c{dlg, inst, &st, 2, col1_x, top, top, col_w};
        c.section(L"Activation");
        c.label_combo(L"Hotkey", kIdHotkey);
        c.check(L"Push to talk — hold the hotkey, release to type it in", kIdPushToTalk);

        c.section(L"Silence");
        c.label_edit(L"Ends a sentence after (ms, default 800)", kIdEndpointMs, ES_NUMBER);
        c.label_edit(L"Ends the session after (ms, default 120000)", kIdSessionMs, ES_NUMBER);
        c.check(L"Never end the session on silence", kIdSessionNever);
    }
    {
        Cursor c{dlg, inst, &st, 2, col2_x, top, top, col_w};
        c.section(L"Stability");
        c.label_edit(L"N — hypotheses compared (default 2)", kIdStabN, ES_NUMBER);
        c.label_edit(L"M — clusters retracted from the decode head (default 3)", kIdStabM,
                     ES_NUMBER);
        c.label_edit(L"T — milliseconds unchanged before committing (default 200)", kIdStabT,
                     ES_NUMBER);

        c.section(L"Cadence");
        c.label_combo(L"Mode", kIdCadence);
        c.label_edit(L"Interval (ms, used only by \"periodic\")", kIdCadenceMs, ES_NUMBER);
    }

    // ── Page 2: injection, audio, model ─────────────────────────────────────────────────────
    {
        Cursor c{dlg, inst, &st, 3, col1_x, top, top, col_w};
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
        Cursor c{dlg, inst, &st, 3, col2_x, top, top, col_w};
        c.section(L"Per-app injection policy");
        c.note(L"process.exe = sendinput | clipboard [, ctrl-shift-v] [, unsupported]", 2);
        c.multiline(kIdAppPolicies, 110);

    }

    // ── Page 3: overlay, privacy, vocabulary ────────────────────────────────────────────────
    {
        Cursor c{dlg, inst, &st, 4, col1_x, top, top, col_w};
        c.section(L"Overlay");
        c.label_combo(L"Presentation", kIdOverlayMode);
        c.label_combo(L"When the anchor is not confident enough", kIdFallback);
        c.label_edit(L"Minimum anchor confidence (0.0 - 1.0)", kIdMinConfidence);
        // The scheme sits above the three colours it governs, and the colours name it in their
        // labels, because a greyed-out field says "not now" and never says "why".
        //
        // There used to be a warning here that this column was "419 of kPageHeight's 430" and
        // that one more line of prose would push a control off the page. It was already false
        // when it was written — 430 was a constant and this column's height depends on the
        // measured `line_h`, so at line_h = 17 it is 451 and the last control was already under
        // the buttons. `fit_page_height` grows the dialog instead, so prose here costs height
        // rather than correctness.
        c.label_combo(L"Colour scheme (this dialog follows it too)", kIdTheme);
        c.label_edit(L"Font family", kIdFontFamily);
        c.label_edit(L"Font size (DIP)", kIdFontSize);
        c.label_edit(L"Pending text, custom scheme only (#AARRGGBB)", kIdColorPending);
        c.label_edit(L"Unstable text, custom scheme only (#AARRGGBB)", kIdColorUnstable);
        c.label_edit(L"Background, custom scheme only (#AARRGGBB)", kIdColorBackground);
    }
    {
        Cursor c{dlg, inst, &st, 4, col2_x, top, top, col_w};
        c.section(L"Privacy and diagnostics");
        c.label_combo(L"Diagnostic log level", kIdLogLevel);
        c.check(L"Write diagnostics to a file (shapes and timings, never text)", kIdDiagFile);
        c.check(L"Log recognised text to a transcript file (off by default)", kIdTranscript);
        c.label_edit(L"Transcript retention: size (KB)", kIdTranscriptKb, ES_NUMBER);
        c.label_edit(L"Transcript retention: age (days)", kIdTranscriptDays, ES_NUMBER);
        c.check(L"Start kvit-dictate with Windows", kIdStartup);

        c.section(L"Vocabulary context");
        c.check(L"Send vocabulary context to the model", kIdSendContext);
        c.note(L"One term per line.", 1);
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
        const wchar_t* names[kPageCount] = {L"Server", L"Compose buffer", L"Dictation",
                                            L"Injection", L"Overlay && privacy"};
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
    // From the content, not from a constant: `page_height` is whatever the tallest column
    // actually needed. Both the buttons and the window below are placed from the same number, so
    // they cannot disagree.
    const int button_y = px(st, kMargin + kTabStrip + kMargin) + px(st, kMargin) +
                         px(st, page_height(st));
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

        case kMsgProbeDone: {
            auto* text = reinterpret_cast<std::wstring*>(lp);
            if (st) {
                st->probe_running = false;
                EnableWindow(GetDlgItem(hwnd, kIdServerTest), TRUE);
                // A reply for a URL the user has since edited and re-tested is stale; showing it
                // would answer a question they have stopped asking.
                if (static_cast<uint32_t>(wp) == st->probe_generation && text != nullptr) {
                    SetWindowTextW(GetDlgItem(hwnd, kIdServerStatus), text->c_str());
                }
            }
            delete text;
            return 0;
        }

        case kMsgNormalizeTestDone: {
            auto* text = reinterpret_cast<std::wstring*>(lp);
            if (st) {
                st->normalize_test_running = false;
                // Through refresh, not EnableWindow: by the time an answer lands the user may
                // have unticked the feature, and the button follows the feature.
                refresh_normalizer_controls(hwnd);
                if (static_cast<uint32_t>(wp) == st->normalize_test_generation &&
                    text != nullptr) {
                    SetWindowTextW(GetDlgItem(hwnd, kIdNormalizerStatus), text->c_str());
                }
            }
            delete text;
            return 0;
        }

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
                    // Nor is the server address. "Restore defaults" is about behaviour; a user
                    // who lost the address of their own server to it would have to go and find
                    // it again, which is not what they asked for.
                    fresh.server = st->settings.server;
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
                        // The rewriter's precondition is the same one, so it follows the same
                        // combo. Both pages are told at once rather than each finding out when
                        // it is next shown.
                        refresh_normalizer_controls(hwnd);
                    }
                    return 0;
                case kIdEditingEnabled:
                    refresh_editing_page(hwnd);
                    return 0;
                case kIdNormalizerEnabled:
                    refresh_normalizer_controls(hwnd);
                    return 0;
                case kIdNormalizerUrl:
                    // Whether there is an address at all is what decides if the check box above
                    // is available, and that can change with every keystroke here.
                    if (code == EN_CHANGE) refresh_normalizer_controls(hwnd);
                    return 0;
                case kIdServerTest:
                    start_server_test(hwnd, *st);
                    return 0;
                case kIdNormalizerTest:
                    start_normalize_test(hwnd, *st);
                    return 0;
                case kIdServerUrl:
                    if (code == EN_CHANGE) {
                        // Live, because the warning is about the address being typed and a
                        // warning that only appears after OK is a warning nobody reads.
                        update_server_warning(hwnd);
                        // The rewriter's address is derived from this box, and whether there is
                        // one at all decides if its check box is available at all.
                        refresh_normalizer_controls(hwnd);
                    }
                    return 0;
                case kIdServerModeLocal:
                case kIdServerModeProtocol:
                    // The label, the example and the warning all change meaning with the mode,
                    // and the warning in particular is about a *different* validation — so it is
                    // recomputed here rather than only on the next keystroke.
                    update_server_mode_ui(hwnd);
                    update_server_warning(hwnd);
                    refresh_normalizer_controls(hwnd);
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

// The one open dialog, published for open_settings_dialog_window(). Not atomic and not
// guarded: the dialog is created, pumped and destroyed on one thread, and the accessor is
// documented as belonging to that thread too.
HWND g_open_dialog = nullptr;

}  // namespace

uintptr_t open_settings_dialog_window() noexcept {
    return reinterpret_cast<uintptr_t>(g_open_dialog);
}

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
                px(st, kMargin * 3 + kTabStrip + 26 + kMargin) + px(st, page_height(st))};
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

    // ── Grow to what the content turned out to need ──────────────────────────────────────────
    //
    // The window has to exist before its controls can be parented to it, and the controls are
    // what reveal how tall the tallest column is — so the height above is a first guess from the
    // minimum and this is the correction. WM_CREATE has already run `build_controls`, which
    // filled `st.content_h` and placed the buttons from the same number, so only the frame is
    // out of date by the time we get here.
    if (page_height(st) > kPageHeightMin) {
        RECT grown{0, 0, px(st, kMargin * 2 + kPageWidth),
                   px(st, kMargin * 3 + kTabStrip + 26 + kMargin) + px(st, page_height(st))};
        AdjustWindowRectExForDpi(&grown, style, FALSE, 0, static_cast<UINT>(st.dpi));
        SetWindowPos(dlg, nullptr, 0, 0, grown.right - grown.left, grown.bottom - grown.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        KD_LOG_DEBUG("settings dialog grown to %d logical px of page (line_h=%d)",
                     page_height(st), st.line_h);

        // A page taller than the screen is a content problem, not a layout one, and clipping it
        // silently is the failure this whole mechanism replaces. Say so; the window is still
        // usable because it can be moved.
        RECT work{};
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) &&
            (grown.bottom - grown.top) > (work.bottom - work.top)) {
            KD_LOG_WARN(
                "the settings dialog needs %d px and the work area is %d px tall; a page has more "
                "content than fits and some of it will be off screen",
                grown.bottom - grown.top, work.bottom - work.top);
        }
    }

    g_open_dialog = dlg;

    // ── Centre it ────────────────────────────────────────────────────────────────────────────
    //
    // CW_USEDEFAULT above lets Windows place the window, and Windows cascades from the *owner* —
    // which here is a message-only window with no position at all, so every launch landed in the
    // top-left corner of the primary monitor.
    //
    // The monitor under the pointer, not the primary one and not the owner's: the tray menu the
    // user just clicked is where the pointer is, and on a multi-monitor desktop that is the
    // screen they are looking at. Clamped to the work area rather than centred blindly, so a
    // dialog taller than the screen starts at the top with its tabs reachable instead of
    // hanging equally off both ends.
    {
        RECT frame{};
        POINT cursor{};
        if (GetWindowRect(dlg, &frame) && GetCursorPos(&cursor)) {
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi)) {
                const int w = frame.right - frame.left;
                const int h = frame.bottom - frame.top;
                int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
                int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
                if (x < mi.rcWork.left) x = mi.rcWork.left;
                if (y < mi.rcWork.top) y = mi.rcWork.top;
                SetWindowPos(dlg, nullptr, x, y, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
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

    g_open_dialog = nullptr;
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
