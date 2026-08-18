// tools/probe — the Phase 0B measurement instrument.
//
// NO ASR. FIXED TEXT ONLY. plan.md calls Phase 0B the critical gate: it decides whether an inline
// overlay anchored at the insertion point is the baseline interaction at all, or whether the
// product falls back to a floating pill. That decision needs measurements across nine
// applications and eighteen scenarios, so this tool exists to make each measurement cheap,
// uniform, and resumable — the operator will not finish in one sitting.
//
// What it deliberately does NOT do:
//
//   * It does not judge injection success from pixels. plan.md §1.5 is explicit that injection
//     success and anchor estimation are separate signals; an empty diff does not mean the text
//     failed to arrive, and a full one does not mean it did. Readback comes from the target's own
//     side channel (tools/testtarget logs what it received) or from the operator, and the record
//     says which.
//   * It does not silently invent a Windows.Graphics.Capture path. §8.3 of
//     docs/supported-targets.md asks whether WGC shows a user-visible capture indicator on the
//     desktop path; that is a question to be answered by running it, and `wgc_indicator_seen`
//     stays null until an operator answers it. A confident-looking implementation of an API
//     nobody has exercised is precisely the failure mode Phase 0A exists to prevent.
//
// Output goes to bench/results/phase0b/<app>/<scenario>/<method>/<stamp>/result.json in the
// schema docs/supported-targets.md §8.2 fixes. The summariser — not a human reading a screen —
// turns those records into the tables in §5.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <shellscalingapi.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <optional>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "anchor.h"
#include "injector.h"
#include "util/json.h"
#include "util/utf8.h"

namespace {

// ─── tools/testtarget's side channel (A09 only) ────────────────────────────────────────
//
// docs/supported-targets.md §3.4 calls `A09` *the only cell family with programmatic ground
// truth*, and tools/testtarget implements exactly that: a WM_COPYDATA channel reporting the true
// caret rectangle, the text actually in the control, the line height, the theme, the DPI and the
// code points it received. The probe never called it — `readback` was hard-coded to
// "unavailable" and `ground_truth` to null on every record, including A09's. So the one row whose
// numbers every other row is interpreted against would have needed the same manual operator pass
// as the eight applications we cannot instrument, which is the whole thing testtarget was built
// to avoid.
//
// Protocol, from tools/testtarget/main.cpp: send WM_COPYDATA with `dwData = kSideChannelRequest`
// and `wParam` = our own window; the target answers by *sending* WM_COPYDATA back to that window
// with `dwData = kSideChannelReply` and a JSON body. The reply therefore arrives re-entrantly
// while we are still blocked inside our own SendMessageTimeout — Windows dispatches inbound sent
// messages to a thread that is itself blocked in a send — so by the time the call returns the
// reply is already captured. PostMessage cannot be used in either direction: WM_COPYDATA's buffer
// is only valid for the duration of the send.
constexpr DWORD kSideChannelRequest = 0x4B440101u;
constexpr DWORD kSideChannelReply = 0x4B440102u;
constexpr wchar_t kTestTargetClass[] = L"KdTestTargetWindow";
constexpr wchar_t kProbeChannelClass[] = L"KdProbeSideChannel";

class SideChannel {
public:
    ~SideChannel() { close(); }

    bool open() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &SideChannel::proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kProbeChannelClass;
        RegisterClassExW(&wc);  // benign if already registered
        hwnd_ = CreateWindowExW(0, kProbeChannelClass, L"kd_probe", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                nullptr, wc.hInstance, nullptr);
        if (hwnd_ == nullptr) return false;
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        return true;
    }

    void close() {
        if (hwnd_ != nullptr) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    // True when a testtarget is running. Checked per call rather than cached: the operator may
    // start it after the probe.
    [[nodiscard]] static bool target_present() {
        return FindWindowW(kTestTargetClass, nullptr) != nullptr;
    }

    // Send a command, return the JSON state reply. `cmd` may be "state", "reset-log",
    // "focus:<control>", "set-text:<utf8>" or "select:<a>:<b>"; anything unrecognised by the
    // target is answered with a plain state report, which is why "state" needs no special case.
    std::optional<std::string> query(const std::string& cmd) {
        HWND t = FindWindowW(kTestTargetClass, nullptr);
        if (t == nullptr || hwnd_ == nullptr) return std::nullopt;
        reply_.clear();
        got_reply_ = false;
        COPYDATASTRUCT cds{};
        cds.dwData = kSideChannelRequest;
        cds.cbData = static_cast<DWORD>(cmd.size());
        cds.lpData = const_cast<char*>(cmd.data());
        DWORD_PTR unused = 0;
        if (SendMessageTimeoutW(t, WM_COPYDATA, reinterpret_cast<WPARAM>(hwnd_),
                                reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 3000,
                                &unused) == 0) {
            return std::nullopt;
        }
        if (!got_reply_) return std::nullopt;
        return reply_;
    }

private:
    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_COPYDATA) {
            auto* self = reinterpret_cast<SideChannel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
            const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lp);
            if (self != nullptr && cds != nullptr && cds->dwData == kSideChannelReply) {
                self->reply_.assign(static_cast<const char*>(cds->lpData), cds->cbData);
                self->got_reply_ = true;
                return 1;
            }
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    HWND hwnd_ = nullptr;
    std::string reply_;
    bool got_reply_ = false;
};

// Pull the four things a Phase 0B record needs out of a state reply. Returns false when the
// payload is not the schema this was written against, rather than filling in defaults — a
// ground-truth field that silently became 0,0 would read as a 40-line-height error later.
struct TargetState {
    bool caret_available = false;
    // True when the reported caret is one the TARGET drew and only the side channel can see.
    // It is ground truth and must never be fed back into the estimator as though it were an
    // anchor the product had: source A cannot see it, which is the entire point of scenario S10.
    bool caret_self_drawn = false;
    kd::anchor::Rect caret{};
    std::string text;
    int32_t line_height = 0;
    std::string theme;
    int32_t dpi = 0;
    bool is_password = false;
    int64_t events_total = 0;
};

bool parse_target_state(const std::string& json_text, TargetState& out);

// Wait until the target has finished consuming the injection, then report its state.
//
// `Injector::inject()` returns once the events have been *sent*, not once the target has
// processed them: SendInput queues into the target's input queue and a 207-byte REF-1 is several
// hundred messages. Reading the side channel immediately caught one run with 34 of 207 bytes
// present, and - worse than a short readback - it sampled the caret *mid-typing*, so the
// "post-commit insertion point" that every error_lh is scored against was wherever the target had
// got to. That is a silent, per-cell random bias, not a visible failure.
//
// Settling on the target's own counters rather than a fixed sleep: `events_total` and the text
// length both stop moving exactly when it is done, whatever the machine's load.
// The same job as settle_target, for a target that has no side channel: wait until the caret has
// stopped moving before believing it is the post-commit insertion point.
//
// This is not optional and it is easy to skip. `Injector::inject()` returns when the events have
// been *sent*, and the after-frame poll loop stops at the FIRST frame with any change at all, so
// both can complete while the target is still consuming keystrokes. Reading the caret there gets
// it mid-typing. Measured, rather than reasoned about: without this loop, A09/S01 scored 0.87 line
// heights against the side channel's 0.07 and S04 0.40 against 0.07 — the caret was about two
// characters short of where it ended up. That is small enough to look like ordinary estimator
// error and would have been baked silently into every real-application row.
//
// `baseline` is the caret before the injection. As in settle_target, waiting for movement to STOP
// is wrong on its own: two reads taken before the target has drained anything agree perfectly.
// Wait for it to start first.
bool settle_caret(kd::anchor::ICaretProvider& caret, uintptr_t window,
                  const kd::anchor::Rect& baseline, bool have_baseline, kd::anchor::Rect& out,
                  int timeout_ms) {
    kd::anchor::Rect prev{};
    bool have_prev = false;
    bool moved = !have_baseline;  // nothing to move away from; accept the first stable reading
    int stable = 0;
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
    for (;;) {
        kd::anchor::Rect now{};
        if (caret.caret_rect(window, now) && !now.empty()) {
            if (have_baseline && (now.left != baseline.left || now.top != baseline.top)) {
                moved = true;
            }
            if (moved && have_prev && now.left == prev.left && now.top == prev.top) {
                // Four agreeing reads, ~160 ms of stillness. Two was not enough: a target that
                // pauses briefly mid-injection - the paced SendInput leaves 1 ms between batches
                // and the target's message pump is not obliged to be prompt - produced two
                // identical reads while still typing, and A09/S02 came back 0.93 line heights
                // short of the side channel's answer while A09/S16 came back 0.87 short. Both are
                // about two characters, which is what a stall of one batch looks like.
                if (++stable >= 4) {
                    out = now;
                    return true;
                }
            } else {
                stable = 0;
            }
            prev = now;
            have_prev = true;
        }
        if (GetTickCount64() >= deadline) {
            if (have_prev) {
                out = prev;
                std::printf("  caret: %s at the settle deadline; this repetition's ground truth is "
                            "not trustworthy\n",
                            moved ? "still moving" : "never moved from its pre-injection position");
                return true;
            }
            return false;
        }
        Sleep(40);
    }
}

bool settle_target(SideChannel& sc, TargetState& out, int timeout_ms, int64_t baseline_events) {
    TargetState prev;
    bool have_prev = false;
    bool progressed = false;
    int stable = 0;
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms);
    for (;;) {
        const std::optional<std::string> reply = sc.query("state");
        TargetState now;
        if (reply.has_value() && parse_target_state(*reply, now)) {
            // Wait for the target to START before waiting for it to STOP. Two reads taken before
            // the input queue has been drained at all agree perfectly, which the first version of
            // this loop accepted as "settled" - so a 25-byte REF-2 read back as 0 bytes on two of
            // three repetitions. Progress past the pre-injection event count is what distinguishes
            // "finished" from "not started".
            if (now.events_total > baseline_events) progressed = true;
            if (progressed && have_prev && now.events_total == prev.events_total &&
                now.text.size() == prev.text.size()) {
                if (++stable >= 2) {  // two agreeing reads, ~80 ms apart
                    out = now;
                    return true;
                }
            } else {
                stable = 0;
            }
            prev = now;
            have_prev = true;
        }
        if (GetTickCount64() >= deadline) {
            if (have_prev) {
                out = prev;
                std::printf("  side channel: %s at the settle deadline; the readback and ground "
                            "truth for this repetition are not trustworthy\n",
                            progressed ? "target still busy"
                                       : "target never consumed the injection");
                return true;
            }
            return false;
        }
        Sleep(40);
    }
}

bool parse_target_state(const std::string& json_text, TargetState& out) {
    kd::json::Value root;
    kd::json::Error err;
    if (!kd::json::parse_object(json_text, root, err)) return false;
    const kd::json::Value* schema = root.find("schema");
    if (schema == nullptr || schema->as_string() != "kd-testtarget-state/1") return false;

    if (const kd::json::Value* caret = root.find("caret")) {
        const kd::json::Value* avail = caret->find("available");
        out.caret_available = avail != nullptr && avail->as_bool();
        const kd::json::Value* self_drawn = caret->find("self_drawn");
        out.caret_self_drawn = self_drawn != nullptr && self_drawn->as_bool();
        if (const kd::json::Value* r = caret->find("screen_rect")) {
            if (r->is_array() && r->size() == 4) {
                out.caret.left = static_cast<int32_t>(r->at(0)->as_int());
                out.caret.top = static_cast<int32_t>(r->at(1)->as_int());
                out.caret.right = static_cast<int32_t>(r->at(2)->as_int());
                out.caret.bottom = static_cast<int32_t>(r->at(3)->as_int());
            }
        }
    }
    if (const kd::json::Value* t = root.find("text")) out.text = std::string(t->as_string());
    if (const kd::json::Value* f = root.find("focus")) {
        if (const kd::json::Value* lh = f->find("line_height_px")) {
            out.line_height = static_cast<int32_t>(lh->as_int());
        }
        if (const kd::json::Value* pw = f->find("is_password")) out.is_password = pw->as_bool();
    }
    if (const kd::json::Value* th = root.find("theme")) out.theme = std::string(th->as_string());
    if (const kd::json::Value* d = root.find("dpi")) out.dpi = static_cast<int32_t>(d->as_int());
    if (const kd::json::Value* e = root.find("events_total")) out.events_total = e->as_int();
    return true;
}


// ─── REF-1, defined once (docs/supported-targets.md §4.2) ───────────────────────────────────
//
// Eight segments, space separated, each probing a different way Unicode injection breaks. It is
// spelled as explicit UTF-8 bytes rather than source literals so that the file's own encoding,
// an editor's normalisation pass, or a compiler's charset switch cannot quietly change what gets
// measured. Segment 5 in particular is decomposed-vs-precomposed and MUST NOT be normalised.

struct Segment {
    const char* id;
    const char* utf8;
};

const Segment kRef1[] = {
    {"ascii", "The quick brown fox"},
    {"identifiers", "llama.cpp --n-gpu-layers=999 C:\\apps\\foo_bar.exe"},
    {"punctuation", "\"curly\" 'quotes' \xE2\x80\x94 em-dash \xE2\x80\xA6 ellipsis"},
    {"latin-accent", "na\xC3\xAFve caf\xC3\xA9 \xC3\x85ngstr\xC3\xB6m"},
    {"combining", "e\xCC\x81 vs \xC3\xA9"},
    {"cjk", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE3\x83\x86\xE3\x82\xAD\xE3\x82\xB9\xE3\x83\x88"},
    {"emoji-zwj", "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB"},
    {"rtl", "\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9 \xD7\xA2\xD7\x91\xD7\xA8\xD7\x99\xD7\xAA"},
};

// ─── REF-2, the anchor probe (docs/supported-targets.md §5.9.1) ────────────────────────
//
// REF-1 measures Unicode fidelity (§3.2) and is the wrong instrument for anchor error (§3.4),
// for two reasons that are properties of the string rather than of any target:
//
//   * At 159 characters (~1272 px in a 10 pt fixed-pitch face) it does not fit the capture
//     region, which is 900 px wide with the insertion point at 65 % of it — about 315 px, or ~39
//     characters, of room to the right. The diff is clipped at the region edge, so the rightmost
//     changed pixel that `anchor_from_diff` keys on is the *boundary*, not the end of the text.
//   * It ends with an RTL run, after which the caret sits at the LEFT of that run. That is the
//     failure S16 exists to isolate; with REF-1 as the universal injection every row measures it.
//
// REF-2 is deliberately dull: short, ASCII, left-to-right, no combining marks, no bidi, and well
// inside the room to the right of the insertion point. It is also closer to what the product
// actually commits — plan.md §1.4 flushes a stability-eligible prefix, a few words at a time, not
// a paragraph — so the anchor figures describe the case that happens rather than a torture case
// that never does.
//
// Keep it ASCII. The moment REF-2 acquires an accent it starts measuring §3.2 again.
const char kRef2Text[] = "the quick brown fox jumps";

std::string ref2() { return std::string(kRef2Text); }

// ─── REF-3, the RTL anchor probe (S16) ──────────────────────────────────────────────────────
//
// REF-2's rules with one deliberate change: right-to-left. `S16` says "insertion point is at the
// **left** edge of the changed region. Bidi runs make 'rightmost changed pixel' actively wrong",
// and until this existed nothing could test that, because REF-1 is unusable for §3.4 (§5.9.1) and
// REF-2 is LTR by construction. `S16` measured with REF-2 injects an LTR run into an RTL
// *control*, which is a different thing and was labelled as such in §5.9.2.
//
// This is the string that makes docs/supported-targets.md §9 question 8 answerable: whether
// `diff_insertion_point`'s leftward-growth heuristic should relocate the reported point or only
// cost confidence. On the measured LTR cells the relocation only ever hurt (`S08` 39.00 line
// heights, `S16` 19.13) — but no measurement has ever put genuinely RTL text in front of it, so
// "it has never helped" is not yet "it never helps".
//
// Hebrew rather than Arabic on purpose: Arabic is cursive and its glyphs shape contextually, so
// the advance width of a run depends on its neighbours and the changed band's extent stops being
// a clean function of the text. Hebrew has no shaping. No niqqud, for the same reason REF-2 has
// no accents — combining marks would drag §3.2 back into a §3.4 measurement.
const char kRef3Text[] = "\xD7\xA2\xD7\x91\xD7\xA8\xD7\x99\xD7\xAA \xD7\x9E\xD7\x94\xD7\x99\xD7\xA8\xD7\x94 \xD7\x9B\xD7\x90\xD7\x9F";

// ─── REF-4 and REF-5, the invisible commits (S05, S06) ──────────────────────────────────────
//
// `S05` is a newline-only commit and `S06` a whitespace-only one. Both exist to test the same
// rule, from plan.md §1.5 and restated in §2 of the supported-targets doc: **an empty diff must
// not be read as injection failure.** A space can be accepted with no visible change at all, and
// a newline changes layout below the insertion point rather than at it.
//
// So for these two cells the expected `error_lh` is *null* and the expected `looks_like_scroll`
// for S05 is plausibly true — and neither of those is a failure. What would be a failure is the
// record claiming the injection did not arrive: readback comes from the side channel, never from
// pixels, and that separation is the thing under test.
const char kRef4Text[] = "\n";
const char kRef5Text[] = "   ";

std::string ref1() {
    std::string s;
    for (size_t i = 0; i < std::size(kRef1); ++i) {
        if (i != 0) s += ' ';
        s += kRef1[i].utf8;
    }
    return s;
}

// ─── The scenario matrix (plan.md Phase 0B, IDs from docs/supported-targets.md §3) ──────────

struct Named {
    const char* id;
    const char* label;
};

const Named kApps[] = {
    {"A01", "VS Code"},      {"A02", "Chrome"},        {"A03", "Windows Terminal"},
    {"A04", "WSLg window"},  {"A05", "Slack"},         {"A06", "Explorer search box"},
    {"A07", "Office"},       {"A08", "kvit-notes"},    {"A09", "tools/testtarget"},
};

const Named kScenarios[] = {
    {"S01", "empty field"},        {"S02", "mid-line insertion"}, {"S03", "selection replacement"},
    {"S04", "line wrap"},          {"S05", "newline-only"},       {"S06", "whitespace-only"},
    {"S07", "vertical scroll"},    {"S08", "horizontal scroll"},  {"S09", "autocomplete open"},
    {"S10", "steady cursor"},      {"S11", "dark theme"},         {"S12", "light theme"},
    {"S13", "mixed DPI"},          {"S14", "multi-monitor"},      {"S15", "HDR"},
    {"S16", "RTL text"},           {"S17", "password field"},     {"S18", "elevated target"},
};

std::string utc_stamp() {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::string file_stamp() {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::string hex_ptr(uintptr_t v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%08llX", static_cast<unsigned long long>(v));
    return buf;
}

std::string narrow(const std::wstring& w) {
    std::string s;
    kd::utf8::from_utf16(w, s, nullptr);
    return s;
}

const char* integrity_name(kd::IntegrityLevel l) {
    switch (l) {
        case kd::IntegrityLevel::Untrusted: return "untrusted";
        case kd::IntegrityLevel::Low: return "low";
        case kd::IntegrityLevel::Medium: return "medium";
        case kd::IntegrityLevel::High: return "high";
        case kd::IntegrityLevel::System: return "system";
        default: return "unknown";
    }
}

bool is_remote_session() { return GetSystemMetrics(SM_REMOTESESSION) != 0; }

// ─── Reporting ──────────────────────────────────────────────────────────────────────────────

std::filesystem::path repo_results_dir() {
    // The probe is built into build-*/bin/<config>/, so the repository root is four levels up.
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::filesystem::path p(exe);
    p = p.parent_path().parent_path().parent_path().parent_path();
    return p / "bench" / "results" / "phase0b";
}

void print_matrix_status() {
    const std::filesystem::path root = repo_results_dir();
    std::printf("Phase 0B coverage — %ls\n\n", root.c_str());
    std::printf("      ");
    for (const Named& s : kScenarios) std::printf("%4s", s.id);
    std::printf("\n");

    size_t done = 0;
    size_t total = 0;
    for (const Named& a : kApps) {
        std::printf("%-6s", a.id);
        for (const Named& s : kScenarios) {
            ++total;
            bool any = false;
            for (const char* m : {"sendinput", "clipboard"}) {
                std::error_code ec;
                const std::filesystem::path d = root / a.id / s.id / m;
                if (std::filesystem::exists(d, ec) &&
                    !std::filesystem::is_empty(d, ec)) {
                    any = true;
                }
            }
            if (any) ++done;
            std::printf("%4s", any ? "x" : ".");
        }
        std::printf("   %s\n", a.label);
    }
    // A coverage figure the operator can act on, and which the gate arithmetic later needs.
    std::printf("\n%zu of %zu cells measured (%.0f%%).\n", done, total,
                total != 0 ? 100.0 * static_cast<double>(done) / static_cast<double>(total) : 0.0);
    std::printf("Cells with no record are NOT passes — they are unmeasured, and the §7 gate is "
                "computed only over measured cells with the exclusions named.\n");
}

}  // namespace

int main(int argc, char** argv) {
    // Physical pixels, per-monitor. The whole measurement is in screen coordinates and a
    // virtualised process would report positions that are wrong by the scale factor — which
    // would look exactly like an anchor error and is not one.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::string app = "A09";
    std::string scenario = "S01";
    // Which reference string to inject. REF-1 is the default because it is what every existing
    // record used; a sweep measuring §3.4 must pass --text ref2.
    std::string text_id = "ref1";
    // Refuse to inject unless the foreground process is this one. See the block that uses it.
    std::string require_exe;
    std::string method = "sendinput";
    int repeat = 1;
    int countdown = 5;
    int settle_ms = 0;
    bool status_only = false;
    // Ask tools/testtarget what state it is in and print the answer. Injects nothing, captures
    // nothing and never touches the foreground, so it is the one probe mode that is safe to run
    // while the machine is in use — and it is how a scenario arrangement gets checked without
    // typing REF-2 into whatever is focused to find out.
    // Pretend tools/testtarget has no side channel, so an A09 run takes the same path a real
    // application does. This is how the system-caret ground truth was validated without typing
    // into anybody's document: same target, same scenario, two independent oracles, compare.
    bool no_side_channel = false;
    bool dump_state = false;
    std::string side_channel_cmd;
    std::string notes;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (a == "--app") app = next();
        else if (a == "--scenario") scenario = next();
        else if (a == "--text") text_id = next();
        else if (a == "--require-exe") require_exe = next();
        else if (a == "--inject") method = next();
        else if (a == "--repeat") repeat = std::atoi(next().c_str());
        else if (a == "--countdown") countdown = std::atoi(next().c_str());
        else if (a == "--settle-ms") settle_ms = std::atoi(next().c_str());
        else if (a == "--notes") notes = next();
        else if (a == "--status") status_only = true;
        else if (a == "--dump-state") dump_state = true;
        else if (a == "--no-side-channel") no_side_channel = true;
        else if (a == "--send") { side_channel_cmd = next(); dump_state = true; }
        else if (a == "--help" || a == "-h") {
            std::printf(
                "kd_probe — Phase 0B measurement harness (fixed text, no ASR)\n\n"
                "  --status                 print the resumable coverage matrix and exit\n"
                "  --dump-state             print tools/testtarget's state JSON and exit.\n"
                "                           Injects nothing and does not touch the foreground.\n"
                "  --send \"<cmd>\"           send one side-channel command (focus:<control>,\n"
                "                           select:<a>:<b>, set-text:<utf8>, reset-log,\n"
                "                           log-visual:on|off) and print the state that follows.\n"
                "                           Implies --dump-state. Also injects nothing.\n"
                "  --app A01..A09           application under test\n"
                "  --scenario S01..S18      scenario under test\n"
                "  --text ref1|..|ref5      ref1 = Unicode fidelity (§3.2, the default)\n"
                "                           ref2 = short LTR anchor probe (§3.4)\n"
                "                           ref3 = short RTL anchor probe (§3.4, S16)\n"
                "                           ref4 = newline only (S05)\n"
                "                           ref5 = whitespace only (S06)\n"
                "                           §5.9.1: ref1 cannot measure anchor error, and\n"
                "                           ref4/ref5 produce no error figure by design.\n"
                "  --inject sendinput|clipboard\n"
                "  --repeat N               repetitions (default 1)\n"
                "  --no-side-channel        ignore tools/testtarget's side channel, so an A09\n"
                "                           run takes the same path a real application does and\n"
                "                           ground truth comes from the system caret. A\n"
                "                           validation aid, not a measurement mode.\n"
                "  --settle-ms N            wait N ms after injecting before the first\n"
                "                           after-frame capture (default 0). A target that\n"
                "                           repaints in stages is otherwise diffed mid-repaint.\n"
                "  --countdown N            seconds to focus the target (default 5)\n"
                "  --require-exe NAME.exe   refuse to inject unless the foreground process is\n"
                "                           NAME.exe. STRONGLY recommended for unattended runs:\n"
                "                           without it the probe types REF-1 into whatever the\n"
                "                           operator happened to focus during the countdown.\n"
                "  --notes \"...\"            free text stored in the record\n\n"
                "Focus the target window during the countdown. The probe injects REF-1, captures\n"
                "before/after frames, scores the diff, and writes one JSON record per repetition.\n");
            return 0;
        }
    }

    if (status_only) {
        print_matrix_status();
        return 0;
    }

    if (dump_state) {
        SideChannel sc;
        if (!sc.open()) {
            std::printf("could not create the side-channel window\n");
            return 2;
        }
        if (!SideChannel::target_present()) {
            std::printf("no kd_testtarget window; start build-*/bin/*/kd_testtarget.exe first\n");
            return 2;
        }
        // "state" is not a real command — anything the target does not recognise falls through
        // to a plain state report, which is exactly what an empty --send should produce.
        const std::optional<std::string> reply =
            sc.query(side_channel_cmd.empty() ? std::string("state") : side_channel_cmd);
        if (!reply.has_value()) {
            std::printf("no reply from kd_testtarget\n");
            return 2;
        }
        std::printf("%s\n", reply->c_str());
        return 0;
    }

    for (char& c : text_id) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string text;
    std::string text_label;
    // Which reference strings produce a figure that may be aggregated into §3.4. REF-1 may not
    // (§5.9.1: it exceeds the capture region and ends in a bidi run). REF-4 and REF-5 may not
    // either, but for the opposite reason — they are *designed* to produce little or no visible
    // change, so an error figure from them would be measuring noise, and their result is the
    // readback and the refusal-to-misread, not a distance.
    bool measuring_anchor = false;
    const char* measures = "";
    if (text_id == "ref1") {
        text = ref1();
        text_label = "REF-1";
        measures = "Unicode fidelity (\xC2\xA7""3.2)";
    } else if (text_id == "ref2") {
        text = ref2();
        text_label = "REF-2";
        measuring_anchor = true;
        measures = "LTR anchor error (\xC2\xA7""3.4)";
    } else if (text_id == "ref3") {
        text = kRef3Text;
        text_label = "REF-3";
        measuring_anchor = true;
        measures = "RTL anchor error (\xC2\xA7""3.4, S16)";
    } else if (text_id == "ref4") {
        text = kRef4Text;
        text_label = "REF-4";
        measures = "newline-only commit (S05) - an empty diff is NOT a failure";
    } else if (text_id == "ref5") {
        text = kRef5Text;
        text_label = "REF-5";
        measures = "whitespace-only commit (S06) - an empty diff is NOT a failure";
    } else {
        std::printf("--text must be ref1|ref2|ref3|ref4|ref5, got '%s'\n", text_id.c_str());
        return 2;
    }
    std::printf("injection: %s (%zu bytes) - measures %s\n", text_label.c_str(), text.size(),
                measures);

    // The side channel is A09's alone: it is tools/testtarget's own protocol, and asking any
    // other application for it would find no window and quietly do nothing. Opened once for the
    // whole run rather than per repetition, so `--repeat 20` does not create and destroy twenty
    // message-only windows.
    SideChannel side_channel;
    bool use_side_channel = false;
    if (app == "A09" && no_side_channel) {
        std::printf("side channel: SUPPRESSED by --no-side-channel; ground truth will come from "
                    "the system caret, as it does for a real application\n");
    } else if (app == "A09") {
        if (!SideChannel::target_present()) {
            std::printf("A09 selected but no tools/testtarget window was found. Ground truth and "
                        "readback will be null and the operator has to supply them by hand, which "
                        "is the thing A09 exists to avoid. Start kd_testtarget.exe first.\n");
        } else if (!side_channel.open()) {
            std::printf("A09 selected but the probe could not create its side-channel window "
                        "(err=%lu); falling back to operator-supplied ground truth.\n",
                        GetLastError());
        } else {
            use_side_channel = true;
            std::printf("side channel: connected to tools/testtarget\n");
            // Stop the target's log pane repainting for the duration of the run. It takes an
            // LB_ADDSTRING per received keystroke, so any capture region overlapping it measures
            // scrolling log text instead of the injected characters - A09/S01 reported 14-17
            // changed regions and ~40 line heights of apparent error with it live. Events are
            // still recorded; only the drawing stops, so `readback` and `events_total` are
            // unaffected. Restored at the end of the run.
            side_channel.query("log-visual:off");
        }
    }

    for (int rep = 0; rep < repeat; ++rep) {
        std::printf("\n[%s/%s/%s] repetition %d of %d\n", app.c_str(), scenario.c_str(),
                    method.c_str(), rep + 1, repeat);
        std::printf("Focus the target window. Injecting in ");
        for (int s = countdown; s > 0; --s) {
            std::printf("%d... ", s);
            std::fflush(stdout);
            Sleep(1000);
        }
        std::printf("go\n");

        auto captured = kd::capture_foreground_target();
        if (!captured) {
            std::printf("  no usable foreground target: %s\n",
                        captured.status().to_string().c_str());
            continue;
        }
        const kd::TargetIdentity target = captured.value();

        // The probe types into whatever holds the foreground when the countdown expires. A
        // caller that checks focus *before* launching the probe has not checked anything: the
        // countdown is a whole second during which a human clicks back into their own work, and
        // then REF-1 lands in it. That happened - an automated sweep typed into the operator's
        // editor - and no amount of care in the launching script can close the gap, because the
        // gap is inside this process.
        //
        // So the check belongs here, against the target this repetition is actually about to use,
        // and it is a refusal rather than a warning.
        if (!require_exe.empty()) {
            std::string got = target.process_name;
            std::string want = require_exe;
            for (char& c : got) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (char& c : want) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (got != want) {
                std::printf("  REFUSING to inject: foreground is '%s', --require-exe is '%s'. "
                            "Nothing was typed.\n",
                            target.process_name.c_str(), require_exe.c_str());
                continue;
            }
        }

        kd::json::Value record = kd::json::Value::object();
        record.set("probe_build", kd::json::Value::string(std::string("dev")));
        record.set("utc", kd::json::Value::string(utc_stamp()));
        record.set("app", kd::json::Value::string(app));
        record.set("scenario", kd::json::Value::string(scenario));
        record.set("repeat_index", kd::json::Value::number(static_cast<int64_t>(rep)));

        kd::json::Value tgt = kd::json::Value::object();
        tgt.set("hwnd", kd::json::Value::string(hex_ptr(target.top_level)));
        tgt.set("exe", kd::json::Value::string(target.process_name));
        tgt.set("integrity", kd::json::Value::string(std::string(integrity_name(target.integrity))));
        tgt.set("is_password_field", kd::json::Value::boolean(target.is_password_field));
        tgt.set("is_console", kd::json::Value::boolean(target.is_console));
        tgt.set("session", kd::json::Value::string(std::string(is_remote_session() ? "rdp" : "console")));
        {
            RECT wr{};
            GetWindowRect(reinterpret_cast<HWND>(target.top_level), &wr);
            const UINT dpi = GetDpiForWindow(reinterpret_cast<HWND>(target.top_level));
            tgt.set("dpi", kd::json::Value::number(static_cast<int64_t>(dpi)));
            kd::json::Value rect = kd::json::Value::array();
            rect.push_back(kd::json::Value::number(static_cast<int64_t>(wr.left)));
            rect.push_back(kd::json::Value::number(static_cast<int64_t>(wr.top)));
            rect.push_back(kd::json::Value::number(static_cast<int64_t>(wr.right)));
            rect.push_back(kd::json::Value::number(static_cast<int64_t>(wr.bottom)));
            tgt.set("window_rect", std::move(rect));
        }
        record.set("target", std::move(tgt));

        // ── Refusals are decided up front and recorded, never discovered afterwards ──────────
        kd::InjectConfig cfg;
        cfg.method = (method == "clipboard") ? kd::InjectMethod::Clipboard
                                             : kd::InjectMethod::SendInput;
        kd::Injector injector(cfg);
        const kd::InjectStatus supported = injector.check_supported(target);
        if (supported != kd::InjectStatus::Ok) {
            kd::json::Value refusal = kd::json::Value::object();
            refusal.set("reason", kd::json::Value::string(std::string(kd::to_string(supported))));
            refusal.set("shown", kd::json::Value::string(std::string("probe refused before injecting")));
            record.set("refusal", std::move(refusal));
            std::printf("  REFUSED: %s\n", kd::to_string(supported));
            // S17/S18 are detection tests: a refusal here is the PASS, not a failure.
            if (scenario == "S17" || scenario == "S18") {
                std::printf("  (this is the expected result for %s — refusing is the behaviour "
                            "under test)\n", scenario.c_str());
            }
        } else {
            record.set("refusal", kd::json::Value::null());  // explicit null: absence is a finding
        }

        // ── Capture the before frame ────────────────────────────────────────────────────────
        std::unique_ptr<kd::anchor::IScreenCapture> capture = kd::anchor::make_bitblt_capture(true);
        std::unique_ptr<kd::anchor::ICaretProvider> caret =
            kd::anchor::make_gui_thread_caret_provider();

        RECT wr{};
        GetWindowRect(reinterpret_cast<HWND>(target.top_level), &wr);
        // The capture region must be the one the PRODUCT uses, not the whole window.
        // src/main.cpp calls `capture_region_for()` on the current insertion point; a probe that
        // captured the entire window would score a diff the product never computes, and would
        // report the estimator as far worse than it is. The first A09 run measured 70.6 line
        // heights of error with a window-sized region against tools/testtarget, whose own event
        // log repaints on every keystroke and dominated the diff - a number that says nothing
        // about the estimator and would have falsely indicted it, since A09 is the calibration
        // row (§5.9: "if A09 cannot hit one line height, the estimator is broken, not the
        // target").
        //
        // The caret read a few lines below is what the product would anchor on at session start,
        // so it is the honest basis. Without one there is nothing better than the window, and the
        // record says which was used so the two are never averaged together.
        kd::anchor::Rect caret_for_region;
        const bool have_caret_for_region =
            caret->caret_rect(target.focus != 0 ? target.focus : target.top_level,
                              caret_for_region);
        kd::anchor::AnchorConfig region_cfg;
        const bool region_from_caret = have_caret_for_region && !caret_for_region.empty();
        const kd::anchor::Point caret_pt{caret_for_region.left, caret_for_region.top};
        const kd::anchor::Rect region =
            region_from_caret
                ? kd::anchor::capture_region_for(caret_pt, region_cfg,
                                                 kd::anchor::monitor_geometry_for(caret_pt))
                : kd::anchor::Rect{wr.left, wr.top, wr.right, wr.bottom};
        std::printf("  capture region: %s [%d,%d %dx%d]\n",
                    region_from_caret ? "around the caret (as the product does)"
                                      : "whole window (no caret available)",
                    region.left, region.top, region.right - region.left,
                    region.bottom - region.top);

        std::vector<uint8_t> before_pixels;
        kd::anchor::BitmapView before_view;
        bool before_black = false;
        if (auto b = capture->capture(region); b) {
            before_view = b.value();
            before_black = capture->last_capture_was_black();
            before_pixels.assign(before_view.pixels,
                                 before_view.pixels +
                                     static_cast<size_t>(before_view.stride) * before_view.height);
            before_view.pixels = before_pixels.data();
        }

        // Source A, before injecting: rcCaret is only meaningful while the target still has the
        // caret it is going to insert at.
        kd::anchor::Rect caret_rect;
        const bool caret_ok = caret->caret_rect(target.focus != 0 ? target.focus : target.top_level,
                                                caret_rect);

        // Ground truth, for A09 only. Taken *before* the injection for the caret - that is the
        // insertion point the anchor is trying to predict, and after the text lands it has moved
        // to the end of what was typed. The log is cleared in the same call so `events_total`
        // afterwards counts this repetition's keystrokes and not the previous one's.
        // Focus continuity across the whole measurement, not just at the moment of injection.
        //
        // `--require-exe` stops the text going somewhere it should not, which is the safety
        // question. This is the *validity* question, and it is different: the capture is of a
        // screen RECTANGLE, so if another window overlaps the target region during the before or
        // after frame - a notification, an alt-tab, a focus flicker that returns - the diff
        // contains someone else's pixels and the record looks perfectly normal. A measurement
        // taken under those conditions is not wrong-looking, it is quietly wrong, which is worse.
        //
        // So each stage that can corrupt the result records whether the target still held the
        // foreground, and the record carries the verdict rather than leaving a reader to assume.
        const HWND target_hwnd = reinterpret_cast<HWND>(target.top_level);
        bool fg_at_before_frame = (GetForegroundWindow() == target_hwnd);
        bool fg_at_after_frame = true;

        TargetState before_state;
        bool have_before_state = false;
        // The caret AFTER the text lands. This, not the pre-injection caret, is what source C is
        // trying to predict: the estimator's whole job is to say where the *next* text goes, and
        // the overlay draws the pending preview there. Scoring against the pre-injection caret
        // measures the width of what was just typed and calls it estimator error - a systematic
        // bias that would have been baked into all 162 cells and read as "the estimator is poor".
        TargetState after_state;
        bool have_after_state = false;
        if (use_side_channel) {
            if (const std::optional<std::string> reply = side_channel.query("reset-log")) {
                have_before_state = parse_target_state(*reply, before_state);
                if (!have_before_state) {
                    std::printf("  side channel: reply was not kd-testtarget-state/1; treating "
                                "ground truth as unavailable\n");
                }
            } else {
                std::printf("  side channel: no reply from tools/testtarget\n");
            }
        }

        // ── Inject ──────────────────────────────────────────────────────────────────────────
        // Second check, immediately before the events go out. Between capturing the target and
        // this line the probe captured a screen frame and read the caret, which is milliseconds
        // but not zero, and a focus change in that window would send the text somewhere else.
        if (!require_exe.empty() && GetForegroundWindow() != reinterpret_cast<HWND>(target.top_level)) {
            std::printf("  REFUSING to inject: the foreground window changed after the target was "
                        "captured. Nothing was typed.\n");
            continue;
        }

        kd::InjectResult inject_result;
        if (supported == kd::InjectStatus::Ok) {
            const ULONGLONG t0 = GetTickCount64();
            inject_result = injector.inject(target, text);
            const ULONGLONG t1 = GetTickCount64();

            kd::json::Value inj = kd::json::Value::object();
            inj.set("method", kd::json::Value::string(method));
            inj.set("text_id", kd::json::Value::string(text_label));
            // Which column this repetition is evidence for. Recorded rather than left to the
            // reader, because the two are not interchangeable and averaging them would produce a
            // number that describes neither (§5.9.1).
            inj.set("measures", kd::json::Value::string(std::string(
                                    measuring_anchor ? "anchor-error" : "unicode-fidelity")));
            inj.set("bytes_sent", kd::json::Value::number(static_cast<int64_t>(text.size())));
            inj.set("elapsed_ms", kd::json::Value::number(static_cast<int64_t>(t1 - t0)));
            inj.set("status", kd::json::Value::string(std::string(kd::to_string(inject_result.status))));
            // Readback is NOT inferred here. tools/testtarget logs the code points it actually
            // received; every other application needs the operator to compare. Either way the
            // record says which, so a later reader cannot mistake a guess for a measurement.
            // Readback is measured where it can be and declared unavailable where it cannot;
            // the record always says which, so a later reader cannot mistake a guess for a
            // measurement.
            std::string readback = "unavailable";
            std::string readback_source = "operator";
            if (use_side_channel) {
                {
                    const int64_t baseline =
                        have_before_state ? before_state.events_total : 0;
                    if (settle_target(side_channel, after_state, 4000, baseline)) {
                        have_after_state = true;
                        readback = after_state.text;
                        readback_source = "side-channel";
                        const bool exact = readback.find(text) != std::string::npos;
                        std::printf("  readback: %s %s (%zu bytes, %lld events)\n",
                                    text_label.c_str(),
                                    exact ? "present verbatim" : "NOT present verbatim",
                                    readback.size(),
                                    static_cast<long long>(after_state.events_total));
                    }
                }
            }
            inj.set("readback", kd::json::Value::string(readback));
            inj.set("readback_source", kd::json::Value::string(readback_source));
            inj.set("lossy_segments", kd::json::Value::array());
            record.set("inject", std::move(inj));
            std::printf("  injected %zu bytes in %llu ms (%s)\n", text.size(),
                        static_cast<unsigned long long>(t1 - t0),
                        kd::to_string(inject_result.status));
        }

        // Wait for the target to finish consuming the injection BEFORE the after-frame is taken.
        //
        // With a side channel that already happened above (settle_target, on events_total). Without
        // one it has to happen here, and skipping it is not a small error: `inject()` returns when
        // the events have been *sent*, the poll loop below stops at the first frame with any change
        // at all, and the two together capture a frame with the text half-painted. The diff then
        // describes a partial repaint and the estimate moves run to run. Measured: with the caret
        // settled only afterwards, A09/S01 scored 0.07, 0.87 and 1.94 line heights on three
        // identical repetitions against a side-channel figure of 0.07 that reproduces to the pixel.
        //
        // The caret is the available proxy for "the target has caught up" when nothing else is.
        kd::anchor::Rect caret_settled{};
        bool caret_settled_ok = false;
        if (!use_side_channel) {
            caret_settled_ok =
                settle_caret(*caret, target.focus != 0 ? target.focus : target.top_level,
                             caret_rect, caret_ok, caret_settled, 4000);
        }

        // ── Capture the after frame, polled ─────────────────────────────────────────────────
        //
        // Electron and WSLg render asynchronously: a single capture taken immediately after
        // SendInput returns often shows the frame BEFORE the text appeared, which reads as "the
        // injection produced no visible change" and is simply a race.
        // The loop below breaks on the FIRST frame with any change at all, which is right for
        // "did anything appear" and wrong for a target that repaints in stages: a control that
        // scrolls may paint the caret's own line first and the rest of the scroll a frame later,
        // and the diff then describes a partial repaint. --settle-ms N waits N ms before the
        // first capture, so that reading can be tested rather than assumed. Default 0: it does
        // not change what any previous record measured.
        if (settle_ms > 0) Sleep(static_cast<DWORD>(settle_ms));

        kd::anchor::DiffResult diff;
        int polls = 0;
        std::vector<uint8_t> after_pixels;
        for (; polls < 20; ++polls) {
            Sleep(25);
            auto a = capture->capture(region);
            if (!a) break;
            kd::anchor::BitmapView after_view = a.value();
            diff = kd::anchor::diff_bitmaps(before_view, after_view);
            if (diff.changed_pixels > 0) {
                after_pixels.assign(after_view.pixels,
                                    after_view.pixels + static_cast<size_t>(after_view.stride) *
                                                            after_view.height);
                break;
            }
        }

        fg_at_after_frame = (GetForegroundWindow() == target_hwnd);

        // The incumbent anchor the product would be scoring against, which until now the probe
        // did not supply at all.
        //
        // `score_diff`'s proximity term — the one term specifically meant to catch "the estimate
        // jumped" — is inert without a `previous`, and so is `diff_insertion_point`'s
        // leftward-growth RTL check. Passing std::nullopt therefore measured `score_diff` minus
        // its main discriminator, and the first eleven cells' confidences are all missing it.
        //
        // The faithful stand-in is the caret as it was *before* the injection: that is where the
        // product's published anchor sits at the moment a commit's diff is scored, whether it
        // came from source A at session start or from the previous commit's own diff. When the
        // target draws its own caret there is no such thing, and std::nullopt is then correct
        // rather than a gap — it is exactly the state the product's first commit is in.
        std::optional<kd::anchor::Anchor> previous;
        // `caret_self_drawn` excluded deliberately: tools/testtarget reports the pane's caret
        // because it drew it, and handing that back would give the estimator an oracle the
        // product does not have. S10 exists to measure source C with NO anchor to start from.
        if (have_before_state && before_state.caret_available && !before_state.caret_self_drawn) {
            kd::anchor::Anchor p;
            p.insertion_point = kd::anchor::Point{before_state.caret.left, before_state.caret.top};
            p.line_height = before_state.line_height > 0 ? before_state.line_height
                                                         : before_state.caret.height();
            p.confidence = kd::anchor::kCaretConfidence;
            p.source = kd::anchor::Source::Caret;
            p.timestamp_ms = 0;
            previous = p;
        } else if (!have_before_state && caret_ok) {
            // No side channel — a real application. The caret read before the injection is the
            // same thing the product would be holding, so it is the right incumbent here too.
            kd::anchor::Anchor p;
            p.insertion_point = kd::anchor::Point{caret_rect.left, caret_rect.top};
            p.line_height = caret_rect.height();
            p.confidence = kd::anchor::kCaretConfidence;
            p.source = kd::anchor::Source::Caret;
            p.timestamp_ms = 0;
            previous = p;
        }

        // ── Ground truth without a side channel, for any target with a system caret ──────────
        //
        // §3.4 says third-party applications have no programmatic ground truth and that an
        // operator must click the caret centre in the captured after-frame. That is true only of
        // targets which draw their own caret. For anything backed by a real system caret,
        // `GetGUIThreadInfo` read from *outside* the process returns exactly what the target
        // reports about itself: checked against tools/testtarget across 11 cells, the probe's own
        // read and the side channel's agreed **to the pixel every time** (`source_a.pt` ==
        // `caret_before_injection` in every record carrying both).
        //
        // So the post-injection caret is ground truth for those targets, self-measured, and the
        // operator-annotation step is needed only for the self-drawn ones (VS Code, Slack,
        // terminals). That is what makes A02/A06/A07-class rows measurable unattended.
        //
        // The limitation this carries, and it is not small: **on such a cell source A is the
        // oracle**, so nothing in it can assess source A's own accuracy, and §3.3's "which anchor
        // source won" is unanswerable — the answer would be assumed rather than measured. Only
        // source C's error against the caret is available. `ground_truth_source` records which
        // kind of truth a record holds so the two are never aggregated as though they were alike.
        // Already settled above, before the after-frame was captured. Re-reading here would only
        // risk catching a blink or a later repaint.
        const kd::anchor::Rect caret_after = caret_settled;
        const bool caret_after_ok = caret_settled_ok && !caret_after.empty();

        const float confidence = kd::anchor::score_diff(diff, region, previous);

        kd::json::Value anchor_obj = kd::json::Value::object();
        {
            kd::json::Value a = kd::json::Value::object();
            a.set("available", kd::json::Value::boolean(caret_ok));
            if (caret_ok) {
                kd::json::Value pt = kd::json::Value::array();
                pt.push_back(kd::json::Value::number(static_cast<int64_t>(caret_rect.left)));
                pt.push_back(kd::json::Value::number(static_cast<int64_t>(caret_rect.top)));
                a.set("pt", std::move(pt));
            }
            anchor_obj.set("source_a", std::move(a));
        }
        {
            // Source B is the hotkey-time click cache, which the probe does not install hooks
            // for. Recorded as unavailable rather than omitted, so the field's absence never has
            // to be interpreted.
            kd::json::Value b = kd::json::Value::object();
            b.set("available", kd::json::Value::boolean(false));
            b.set("note", kd::json::Value::string(std::string(
                              "probe does not install WH_MOUSE_LL; source B is measured in the "
                              "application, not here")));
            anchor_obj.set("source_b", std::move(b));
        }
        {
            kd::json::Value c = kd::json::Value::object();
            c.set("available", kd::json::Value::boolean(diff.changed_pixels > 0));
            c.set("confidence", kd::json::Value::number(static_cast<double>(confidence)));
            kd::json::Value r = kd::json::Value::array();
            r.push_back(kd::json::Value::number(static_cast<int64_t>(diff.changed_bounds.left)));
            r.push_back(kd::json::Value::number(static_cast<int64_t>(diff.changed_bounds.top)));
            r.push_back(kd::json::Value::number(static_cast<int64_t>(diff.changed_bounds.right)));
            r.push_back(kd::json::Value::number(static_cast<int64_t>(diff.changed_bounds.bottom)));
            c.set("diff_rect", std::move(r));
            c.set("diff_rows", kd::json::Value::number(static_cast<int64_t>(diff.changed_rows)));
            c.set("changed_regions", kd::json::Value::number(static_cast<int64_t>(diff.changed_regions)));
            c.set("looks_like_scroll", kd::json::Value::boolean(diff.looks_like_scroll));
            // The line-wrap correction: the insertion point follows the LAST changed text row,
            // not the bounding box, whose right edge belongs to the previous line.
            kd::json::Value last = kd::json::Value::array();
            last.push_back(kd::json::Value::number(static_cast<int64_t>(diff.last_changed_row_extent.left)));
            last.push_back(kd::json::Value::number(static_cast<int64_t>(diff.last_changed_row_extent.top)));
            last.push_back(kd::json::Value::number(static_cast<int64_t>(diff.last_changed_row_extent.right)));
            last.push_back(
                kd::json::Value::number(static_cast<int64_t>(diff.last_changed_row_extent.bottom)));
            c.set("last_changed_row", std::move(last));
            anchor_obj.set("source_c", std::move(c));
        }
        // Which incumbent the score and the position were computed against. Records written
        // before 2026-08-18 have no such field and were all scored with none, so they are not
        // comparable with these and must not be aggregated together.
        if (previous.has_value()) {
            kd::json::Value prev = kd::json::Value::array();
            prev.push_back(kd::json::Value::number(static_cast<int64_t>(previous->insertion_point.x)));
            prev.push_back(kd::json::Value::number(static_cast<int64_t>(previous->insertion_point.y)));
            anchor_obj.set("scored_against_previous", std::move(prev));
        } else {
            anchor_obj.set("scored_against_previous", kd::json::Value::null());
        }
        anchor_obj.set("winner", kd::json::Value::string(std::string(
                                     diff.changed_pixels > 0 ? "C" : (caret_ok ? "A" : "none"))));
        anchor_obj.set("confidence", kd::json::Value::number(static_cast<double>(confidence)));
        // Ground truth, in falling order of authority: A09's side channel, then the target's own
        // system caret read after the injection (see the block above), then nobody — the
        // operator's job (§8.1 step 6). It stays null when it cannot be measured; a default of
        // "0,0" would silently become a 40-line-height error in the aggregate, which is worse
        // than a gap.
        const bool gt_from_side_channel = have_after_state && after_state.caret_available;
        const bool gt_from_system_caret = !gt_from_side_channel && caret_after_ok;
        const bool gt_ok = gt_from_side_channel || gt_from_system_caret;
        if (gt_ok) {
            const kd::anchor::Rect& truth =
                gt_from_side_channel ? after_state.caret : caret_after;
            kd::json::Value gt = kd::json::Value::array();
            gt.push_back(kd::json::Value::number(static_cast<int64_t>(truth.left)));
            gt.push_back(kd::json::Value::number(static_cast<int64_t>(truth.top)));
            anchor_obj.set("ground_truth", std::move(gt));
            // Kept so the two are never confused by a later reader, and so the distance between
            // them - the width of the committed text - is recoverable from the record.
            const bool have_pre = gt_from_side_channel
                                      ? (have_before_state && before_state.caret_available)
                                      : caret_ok;
            if (have_pre) {
                const kd::anchor::Rect& pre_rect =
                    gt_from_side_channel ? before_state.caret : caret_rect;
                kd::json::Value pre = kd::json::Value::array();
                pre.push_back(kd::json::Value::number(static_cast<int64_t>(pre_rect.left)));
                pre.push_back(kd::json::Value::number(static_cast<int64_t>(pre_rect.top)));
                anchor_obj.set("caret_before_injection", std::move(pre));
            }
            anchor_obj.set(
                "ground_truth_source",
                kd::json::Value::string(std::string(gt_from_side_channel
                                                        ? "testtarget-side-channel"
                                                        : "system-caret-after-injection")));
            // On a system-caret cell source A IS the oracle, so the record must not be read as
            // saying anything about source A's own accuracy, and §3.3 cannot be answered from it.
            if (gt_from_system_caret) {
                anchor_obj.set(
                    "ground_truth_caveat",
                    kd::json::Value::string(std::string(
                        "Ground truth is the target's own system caret, read with GetGUIThreadInfo "
                        "after the injection settled. Validated against tools/testtarget: the "
                        "probe's external read matched the side channel to the pixel on 11/11 "
                        "cells. Source A is therefore the oracle here - this record measures "
                        "source C's error against the caret and says NOTHING about source A's own "
                        "accuracy, so it cannot answer §3.3 (which anchor source won).")));
            }

            // §3.4's figure: the estimator's error in LINE HEIGHTS, not pixels, so a 4K target
            // and a 96-dpi one are comparable. Scored through `anchor_from_diff` - the same
            // function the product uses - rather than a reimplementation here, because a probe
            // that measured its own arithmetic would report a number the product never produces.
            // Same `previous` the confidence above was scored with. It changes the *position*
            // too, not only the score: diff_insertion_point uses it to decide whether the change
            // grew leftward, and an RTL verdict moves the reported insertion point to the other
            // end of the changed band.
            const std::optional<kd::anchor::Anchor> est =
                kd::anchor::anchor_from_diff(diff, region, previous, 0);
            // Line height: the side channel's when there is one, otherwise the caret's own
            // height, which is what the caret rectangle is for.
            int32_t lh = after_state.line_height > 0 ? after_state.line_height
                                                     : before_state.line_height;
            if (lh <= 0) lh = caret_after_ok ? caret_after.height() : caret_rect.height();
            if (est.has_value() && lh > 0) {
                const double dx = static_cast<double>(est->insertion_point.x - truth.left);
                const double dy = static_cast<double>(est->insertion_point.y - truth.top);
                const double dist = std::sqrt(dx * dx + dy * dy);
                anchor_obj.set("error_lh", kd::json::Value::number(dist / static_cast<double>(lh)));
                // §5.9.1: an error_lh computed from REF-1 measures region clipping plus S16's
                // bidi failure, not the estimator. It is still recorded - throwing away a
                // measurement is worse than labelling it - but it must never be aggregated into
                // the §3.4 column, and the record says so rather than relying on the reader
                // remembering which string was injected.
                anchor_obj.set("error_lh_valid_for_3_4",
                               kd::json::Value::boolean(measuring_anchor));
                if (!measuring_anchor) {
                    // Which reason, so a reader does not have to know the reference strings by
                    // heart to know why the figure beside them is disqualified.
                    const char* why =
                        text_id == "ref1"
                            ? "REF-1 exceeds the capture region and ends with an RTL run; this "
                              "figure is not an estimator measurement (see §5.9.1). Re-run with "
                              "--text ref2 (LTR) or ref3 (RTL)."
                            : "REF-4/REF-5 are designed to produce little or no visible change, so "
                              "a distance computed from their diff measures noise. The result for "
                              "S05/S06 is the readback plus the fact that an empty diff was not "
                              "reported as an injection failure - not an error figure.";
                    anchor_obj.set("error_lh_caveat", kd::json::Value::string(std::string(why)));
                }
                std::printf("  ground truth: post-commit caret (%d,%d) lh=%d -> estimator "
                            "error %.2f line heights\n",
                            truth.left, truth.top, lh, dist / static_cast<double>(lh));
            } else {
                // No source-C anchor to score, or no line height. Null rather than 0.
                anchor_obj.set("error_lh", kd::json::Value::null());
            }
            kd::json::Value env = kd::json::Value::object();
            env.set("theme", kd::json::Value::string(after_state.theme.empty()
                                                         ? before_state.theme
                                                         : after_state.theme));
            env.set("dpi", kd::json::Value::number(static_cast<int64_t>(before_state.dpi)));
            env.set("line_height_px",
                    kd::json::Value::number(static_cast<int64_t>(before_state.line_height)));
            env.set("is_password", kd::json::Value::boolean(before_state.is_password));
            record.set("target_state", std::move(env));
        } else {
            anchor_obj.set("ground_truth", kd::json::Value::null());
            anchor_obj.set("ground_truth_source", kd::json::Value::null());
            anchor_obj.set("error_lh", kd::json::Value::null());
        }
        anchor_obj.set("fallback_mode",
                       kd::json::Value::string(std::string(confidence >= 0.5f ? "inline" : "pill")));
        record.set("anchor", std::move(anchor_obj));

        kd::json::Value cap = kd::json::Value::object();
        {
            kd::json::Value rr = kd::json::Value::array();
            rr.push_back(kd::json::Value::number(static_cast<int64_t>(region.left)));
            rr.push_back(kd::json::Value::number(static_cast<int64_t>(region.top)));
            rr.push_back(kd::json::Value::number(static_cast<int64_t>(region.right)));
            rr.push_back(kd::json::Value::number(static_cast<int64_t>(region.bottom)));
            cap.set("region", std::move(rr));
            // Never averaged across bases: a window-sized region measures a different thing.
            cap.set("region_basis",
                    kd::json::Value::string(std::string(region_from_caret ? "caret" : "window")));
        }
        cap.set("path", kd::json::Value::string(std::string(capture->name())));
        cap.set("all_black", kd::json::Value::boolean(before_black || capture->last_capture_was_black()));
        // Null, not false: nobody has run the WGC desktop path yet, and recording "false" would
        // assert an answer to the open question in docs/supported-targets.md §9.
        cap.set("wgc_indicator_seen", kd::json::Value::null());
        cap.set("after_polls", kd::json::Value::number(static_cast<int64_t>(polls)));
        // Recorded so a settled after-frame is distinguishable from one caught mid-repaint
        // without having to know which command line produced the record.
        cap.set("settle_ms", kd::json::Value::number(static_cast<int64_t>(settle_ms)));
        record.set("capture", std::move(cap));

        {
            const bool stable = fg_at_before_frame && fg_at_after_frame;
            kd::json::Value v = kd::json::Value::object();
            v.set("foreground_at_before_frame", kd::json::Value::boolean(fg_at_before_frame));
            v.set("foreground_at_after_frame", kd::json::Value::boolean(fg_at_after_frame));
            v.set("suspect", kd::json::Value::boolean(!stable));
            if (!stable) {
                v.set("suspect_reason",
                      kd::json::Value::string(std::string(
                          "the target did not hold the foreground for the whole measurement; the "
                          "captured rectangle may contain another window's pixels, so the diff "
                          "and every figure derived from it are unreliable. Exclude from "
                          "aggregates and re-run.")));
                std::printf("  SUSPECT: the target lost the foreground during the measurement "
                            "(before=%s after=%s); this record is marked unreliable\n",
                            fg_at_before_frame ? "ok" : "lost", fg_at_after_frame ? "ok" : "lost");
            }
            record.set("validity", std::move(v));
        }
        record.set("notes", kd::json::Value::string(notes));

        const std::filesystem::path dir =
            repo_results_dir() / app / scenario / method / file_stamp();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path out = dir / "result.json";
        std::ofstream f(out, std::ios::binary);
        f << kd::json::serialize_pretty(record);
        f.close();

        std::printf("  anchor: winner=%s confidence=%.2f rows=%d regions=%d scroll=%s\n",
                    diff.changed_pixels > 0 ? "C" : (caret_ok ? "A" : "none"),
                    static_cast<double>(confidence), diff.changed_rows, diff.changed_regions,
                    diff.looks_like_scroll ? "yes" : "no");
        if (before_black || capture->last_capture_was_black()) {
            std::printf("  NOTE: capture came back all black — this is the HDR / multi-plane "
                        "overlay case. The WGC fallback is unimplemented and its desktop path is "
                        "still an open question (docs/supported-targets.md §9).\n");
        }
        std::printf("  wrote %ls\n", out.c_str());
        if (!have_before_state) {
            std::printf("  TODO for the operator: compare what arrived against REF-1, and click "
                        "the true caret position, then fill readback/ground_truth in the "
                        "record.\n");
        }
    }

    if (use_side_channel) side_channel.query("log-visual:on");
    return 0;
}
