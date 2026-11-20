// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// updater.h says what this does and what it deliberately does not. Notes here are about how.

#define KD_LOG_MODULE "update"

#include "updater.h"

#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <winhttp.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "util/json.h"
#include "util/log.h"
#include "util/utf8.h"

#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (static_cast<NTSTATUS>(s) >= 0)
#endif

// Stamped by CMakeLists.txt onto this file only, so a new pre-release tag recompiles one file.
// Bare tokens stringified here, for the reason CMakeLists.txt gives at the .rc's defines.
// KVIT_VERSION_PRE is defined only for a pre-release build ("rc2"), never as empty.
#define KD_UPDATE_STR2(x) #x
#define KD_UPDATE_STR(x) KD_UPDATE_STR2(x)
#ifndef KVIT_VERSION_RAW
#define KVIT_VERSION_RAW 0.0.0
#endif

namespace kd::update {

namespace {

constexpr const wchar_t* kApiUrl =
    L"https://api.github.com/repos/kvit-s/kvit-dictate/releases?per_page=30";
constexpr const char* kSumsName = "SHA256SUMS.txt";
constexpr const char* kInstallerPrefix = "kvit-dictate-";
constexpr const char* kInstallerSuffix = "-setup.exe";

// kvit-dictate.iss's AppId. Inno Setup appends `_is1` to it for the uninstall key.
constexpr const wchar_t* kUninstallKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
    L"{4E1C9A67-2D3B-4F81-9A0E-7C6B5D48E210}_is1";

// The release list is a few kilobytes per release; this is room for thirty with long notes.
constexpr size_t kMaxApiBytes = 8u << 20;
constexpr size_t kMaxSumsBytes = 64u << 10;
// The installer is ~65 MB today. A bound, so a wrong URL cannot fill the disk.
constexpr uint64_t kMaxInstallerBytes = 1ull << 30;

constexpr const wchar_t* kDialogTitle = L"kvit-dictate update";

std::wstring wide(std::string_view s) { return utf8::to_utf16_or_empty(s); }
std::string narrow(std::wstring_view s) { return utf8::from_utf16_or_empty(std::wstring(s)); }

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alnum_or_hyphen(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}

bool parse_number(std::string_view s, uint32_t& out) {
    if (s.empty() || s.size() > 9) return false;
    if (s.size() > 1 && s[0] == '0') return false;  // SemVer: no leading zeros
    uint32_t v = 0;
    for (char c : s) {
        if (!is_digit(c)) return false;
        v = v * 10 + static_cast<uint32_t>(c - '0');
    }
    out = v;
    return true;
}

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    size_t start = 0;
    for (;;) {
        const size_t at = s.find(sep, start);
        if (at == std::string_view::npos) {
            out.push_back(s.substr(start));
            return out;
        }
        out.push_back(s.substr(start, at - start));
        start = at + 1;
    }
}

bool all_digits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!is_digit(c)) return false;
    }
    return true;
}

// Digit runs compare as numbers and everything else as ASCII, so "rc10" > "rc9".
int natural_compare(std::string_view a, std::string_view b) {
    size_t i = 0;
    size_t j = 0;
    while (i < a.size() && j < b.size()) {
        if (is_digit(a[i]) && is_digit(b[j])) {
            size_t ie = i;
            size_t je = j;
            while (ie < a.size() && is_digit(a[ie])) ++ie;
            while (je < b.size() && is_digit(b[je])) ++je;
            // Strip leading zeros, then the longer run is the larger number.
            size_t is = i;
            size_t js = j;
            while (is + 1 < ie && a[is] == '0') ++is;
            while (js + 1 < je && b[js] == '0') ++js;
            if (ie - is != je - js) return (ie - is) < (je - js) ? -1 : 1;
            const int c = a.substr(is, ie - is).compare(b.substr(js, je - js));
            if (c != 0) return c < 0 ? -1 : 1;
            i = ie;
            j = je;
        } else {
            if (a[i] != b[j]) return static_cast<unsigned char>(a[i]) < static_cast<unsigned char>(b[j]) ? -1 : 1;
            ++i;
            ++j;
        }
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

int compare_pre(std::string_view a, std::string_view b) {
    const std::vector<std::string_view> pa = split(a, '.');
    const std::vector<std::string_view> pb = split(b, '.');
    for (size_t k = 0; k < pa.size() && k < pb.size(); ++k) {
        const bool na = all_digits(pa[k]);
        const bool nb = all_digits(pb[k]);
        int c = 0;
        if (na && nb) {
            c = natural_compare(pa[k], pb[k]);
        } else if (na != nb) {
            c = na ? -1 : 1;  // SemVer: numeric identifiers sort before alphanumeric ones
        } else {
            c = natural_compare(pa[k], pb[k]);
        }
        if (c != 0) return c;
    }
    if (pa.size() == pb.size()) return 0;
    return pa.size() < pb.size() ? -1 : 1;
}

bool starts_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(0, p.size()) == p;
}
bool ends_with(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
}

bool is_installer_name(std::string_view name) {
    if (!starts_with(name, kInstallerPrefix) || !ends_with(name, kInstallerSuffix)) return false;
    // Nothing in the name that could step out of the download folder.
    for (char c : name) {
        if (!(is_alnum_or_hyphen(c) || c == '.' || c == '_')) return false;
    }
    return name.find("..") == std::string_view::npos;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool is_hex64(std::string_view s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        const bool hex = is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) return false;
    }
    return true;
}

// ─── WinHTTP ────────────────────────────────────────────────────────────────────────────────

struct Internet {
    HINTERNET h = nullptr;
    explicit Internet(HINTERNET v = nullptr) : h(v) {}
    ~Internet() {
        if (h != nullptr) WinHttpCloseHandle(h);
    }
    Internet(const Internet&) = delete;
    Internet& operator=(const Internet&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

std::string network_hint(DWORD err) {
    switch (err) {
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
            return "github.com could not be found. Is this PC connected to the internet?";
        case ERROR_WINHTTP_TIMEOUT:
            return "GitHub did not answer in time.";
        case ERROR_WINHTTP_CANNOT_CONNECT:
        case ERROR_WINHTTP_CONNECTION_ERROR:
            return "the connection to GitHub failed.";
        case ERROR_WINHTTP_SECURE_FAILURE:
            return "GitHub's certificate was not trusted by this PC.";
        default:
            return "the request to GitHub failed (WinHTTP error " + std::to_string(err) + ").";
    }
}

Status net_failure(const char* what, DWORD err) {
    KD_LOG_WARN("%s failed: %lu", what, static_cast<unsigned long>(err));
    return Status{StatusCode::Unavailable, network_hint(err), err};
}

using Sink = std::function<Status(const char* data, size_t n, uint64_t total)>;

// One HTTPS GET, following GitHub's redirects to its download host (WinHTTP follows them by
// default and refuses an HTTPS-to-HTTP one). `api` selects GitHub's JSON headers.
Status http_get(const std::wstring& url, bool api, const Sink& sink,
                const std::atomic<bool>* cancel) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc)) {
        return Status{StatusCode::InvalidArgument, "not a URL: " + narrow(url)};
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) {
        return Status{StatusCode::InvalidArgument, "refusing a non-HTTPS URL: " + narrow(url)};
    }
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo != nullptr) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

    const std::wstring agent = L"kvit-dictate/" + wide(running_version());
    Internet session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return net_failure("WinHttpOpen", GetLastError());
    WinHttpSetTimeouts(session.h, 15000, 15000, 30000, 30000);

    Internet connect(WinHttpConnect(session.h, host.c_str(), uc.nPort, 0));
    if (!connect) return net_failure("WinHttpConnect", GetLastError());

    Internet req(WinHttpOpenRequest(connect.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!req) return net_failure("WinHttpOpenRequest", GetLastError());

    const wchar_t* headers = api ? L"Accept: application/vnd.github+json\r\n"
                                   L"X-GitHub-Api-Version: 2022-11-28\r\n"
                                 : L"Accept: application/octet-stream\r\n";
    if (!WinHttpSendRequest(req.h, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0,
                            0, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr)) {
        return net_failure("the GitHub request", GetLastError());
    }

    DWORD status = 0;
    DWORD len = sizeof(status);
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        KD_LOG_WARN("GitHub answered HTTP %lu for %s", static_cast<unsigned long>(status),
                    narrow(url).c_str());
        if (status == 403 || status == 429) {
            return Status{StatusCode::Unavailable,
                          "GitHub is refusing requests from this network for now (its limit is "
                          "60 an hour without signing in). Try again in an hour."};
        }
        return Status{StatusCode::Unavailable,
                      "GitHub answered HTTP " + std::to_string(status) + "."};
    }

    uint64_t total = 0;
    {
        wchar_t buf[32] = {};
        DWORD blen = sizeof(buf);
        if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                buf, &blen, WINHTTP_NO_HEADER_INDEX)) {
            total = _wcstoui64(buf, nullptr, 10);
        }
    }

    std::vector<char> chunk(64 * 1024);
    for (;;) {
        if (cancel != nullptr && cancel->load()) {
            return Status{StatusCode::Unavailable, "cancelled"};
        }
        DWORD got = 0;
        if (!WinHttpReadData(req.h, chunk.data(), static_cast<DWORD>(chunk.size()), &got)) {
            return net_failure("reading from GitHub", GetLastError());
        }
        if (got == 0) break;
        if (Status st = sink(chunk.data(), got, total); !st.ok()) return st;
    }
    return Status::success();
}

Status get_text(const std::wstring& url, bool api, size_t max_bytes, std::string& out) {
    out.clear();
    return http_get(
        url, api,
        [&](const char* d, size_t n, uint64_t) {
            if (out.size() + n > max_bytes) {
                return Status{StatusCode::Malformed, "GitHub's answer was unexpectedly large."};
            }
            out.append(d, n);
            return Status::success();
        },
        nullptr);
}

// ─── Files ──────────────────────────────────────────────────────────────────────────────────

struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~File() { close(); }
    void close() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
};

Result<std::string> sha256_file(const std::filesystem::path& p) {
    File f;
    f.h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                      FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f.h == INVALID_HANDLE_VALUE) {
        return Status{StatusCode::NotFound, "could not open " + p.string(), GetLastError()};
    }
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return Status{StatusCode::OsError, "BCryptOpenAlgorithmProvider(SHA256) failed"};
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!NT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return Status{StatusCode::OsError, "BCryptCreateHash failed"};
    }
    std::vector<unsigned char> buf(1 << 20);
    bool ok = true;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(f.h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
            ok = false;
            break;
        }
        if (got == 0) break;
        if (!NT_SUCCESS(BCryptHashData(hash, buf.data(), got, 0))) {
            ok = false;
            break;
        }
    }
    unsigned char digest[32] = {};
    if (ok) ok = NT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return Status{StatusCode::OsError, "hashing " + p.string() + " failed"};

    static const char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(64);
    for (unsigned char b : digest) {
        hex.push_back(kHex[b >> 4]);
        hex.push_back(kHex[b & 15]);
    }
    return hex;
}

std::filesystem::path download_dir() {
    wchar_t tmp[MAX_PATH + 1] = {};
    const DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
    std::filesystem::path base = n > 0 ? std::filesystem::path(tmp) : std::filesystem::path(L".");
    return base / L"kvit-dictate-update";
}

// ─── Dialogs ────────────────────────────────────────────────────────────────────────────────
//
// TaskDialogIndirect is looked up at run time rather than imported. It exists only in common
// controls 6, which kvit-dictate.exe gets from its manifest and kd_tests.exe — which links this
// file through kd_core — does not; an import would stop the test binary from loading at all.
// Without it every dialog falls back to a MessageBox, and the download runs without a bar.

using TaskDialogIndirectFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);

TaskDialogIndirectFn task_dialog() {
    static TaskDialogIndirectFn fn = [] {
        HMODULE m = GetModuleHandleW(L"comctl32.dll");
        if (m == nullptr) m = LoadLibraryW(L"comctl32.dll");
        return m != nullptr ? reinterpret_cast<TaskDialogIndirectFn>(
                                  GetProcAddress(m, "TaskDialogIndirect"))
                            : nullptr;
    }();
    return fn;
}

HRESULT CALLBACK common_callback(HWND hwnd, UINT msg, WPARAM, LPARAM lp, LONG_PTR) {
    if (msg == TDN_CREATED) {
        // A dialog from a tray click or a notification: this process was just given the right
        // to take the foreground, and without this the dialog can open behind the window the
        // user was looking at.
        SetForegroundWindow(hwnd);
    } else if (msg == TDN_HYPERLINK_CLICKED) {
        ShellExecuteW(nullptr, L"open", reinterpret_cast<const wchar_t*>(lp), nullptr, nullptr,
                      SW_SHOWNORMAL);
    }
    return S_OK;
}

void message(PCWSTR icon, UINT mb_icon, const std::wstring& main, const std::wstring& content) {
    if (TaskDialogIndirectFn td = task_dialog()) {
        TASKDIALOGCONFIG c{};
        c.cbSize = sizeof(c);
        c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_ENABLE_HYPERLINKS;
        c.dwCommonButtons = TDCBF_OK_BUTTON;
        c.pszWindowTitle = kDialogTitle;
        c.pszMainIcon = icon;
        c.pszMainInstruction = main.c_str();
        c.pszContent = content.c_str();
        c.pfCallback = common_callback;
        if (SUCCEEDED(td(&c, nullptr, nullptr, nullptr))) return;
    }
    MessageBoxW(nullptr, (main + L"\n\n" + content).c_str(), kDialogTitle,
                MB_OK | mb_icon | MB_SETFOREGROUND | MB_TOPMOST);
}

// Install, open the download page, or neither. 1 = the first, 2 = the second, 0 = neither.
int ask(const std::wstring& main, const std::wstring& content, const wchar_t* yes,
        const wchar_t* yes_note, const wchar_t* no) {
    if (TaskDialogIndirectFn td = task_dialog()) {
        const std::wstring yes_text = std::wstring(yes) + L"\n" + yes_note;
        TASKDIALOG_BUTTON buttons[] = {{1001, yes_text.c_str()}, {1002, no}};
        TASKDIALOGCONFIG c{};
        c.cbSize = sizeof(c);
        c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_ENABLE_HYPERLINKS | TDF_USE_COMMAND_LINKS;
        c.pszWindowTitle = kDialogTitle;
        c.pszMainIcon = TD_INFORMATION_ICON;
        c.pszMainInstruction = main.c_str();
        c.pszContent = content.c_str();
        c.cButtons = ARRAYSIZE(buttons);
        c.pButtons = buttons;
        c.nDefaultButton = 1001;
        c.pfCallback = common_callback;
        int pressed = 0;
        if (SUCCEEDED(td(&c, &pressed, nullptr, nullptr))) return pressed == 1001 ? 1 : 0;
    }
    const std::wstring text = main + L"\n\n" + content + L"\n\n" + yes + L"?";
    return MessageBoxW(nullptr, text.c_str(), kDialogTitle,
                       MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND | MB_TOPMOST) == IDYES
               ? 1
               : 0;
}

struct DownloadJob {
    std::atomic<uint64_t> done{0};
    std::atomic<uint64_t> total{0};
    std::atomic<bool> cancel{false};
    std::atomic<bool> finished{false};
    std::mutex mu;
    Status result;
    std::filesystem::path file;
    std::wstring version;
};

std::wstring megabytes(uint64_t bytes) {
    wchar_t buf[32];
    swprintf(buf, ARRAYSIZE(buf), L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

HRESULT CALLBACK progress_callback(HWND hwnd, UINT msg, WPARAM wp, LPARAM, LONG_PTR ref) {
    auto* job = reinterpret_cast<DownloadJob*>(ref);
    switch (msg) {
        case TDN_CREATED:
            SetForegroundWindow(hwnd);
            SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_RANGE, 0, MAKELPARAM(0, 1000));
            return S_OK;
        case TDN_TIMER: {
            if (job->finished.load()) {
                // The only button is Cancel; pressing it on the user's behalf is how a task
                // dialog is closed from code. `finished` tells the caller it was not the user.
                SendMessageW(hwnd, TDM_CLICK_BUTTON, IDCANCEL, 0);
                return S_OK;
            }
            const uint64_t done = job->done.load();
            const uint64_t total = job->total.load();
            if (total > 0) {
                SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_POS,
                             static_cast<WPARAM>(done * 1000 / total), 0);
            }
            const std::wstring text = megabytes(done) + (total > 0 ? L" of " + megabytes(total) : L"");
            // UPDATE, not SET: SET re-lays the dialog out, five times a second.
            SendMessageW(hwnd, TDM_UPDATE_ELEMENT_TEXT, TDE_CONTENT,
                         reinterpret_cast<LPARAM>(text.c_str()));
            return S_OK;
        }
        case TDN_BUTTON_CLICKED:
            if (static_cast<int>(wp) == IDCANCEL && !job->finished.load()) job->cancel.store(true);
            return S_OK;
        default:
            return S_OK;
    }
}

// Download on a thread of its own, with a progress dialog on this one. Returns the job's result;
// a cancel is reported as StatusCode::Unavailable with the message "cancelled".
Status download_with_progress(const Release& r, std::filesystem::path& out) {
    auto job = std::make_shared<DownloadJob>();
    job->total.store(r.installer_size);
    std::thread worker([job, r] {
        std::filesystem::path file;
        const Status st = download_installer(
            r, [&](uint64_t done, uint64_t total) {
                job->done.store(done);
                if (total > 0) job->total.store(total);
            },
            job->cancel, file);
        {
            std::lock_guard<std::mutex> lock(job->mu);
            job->result = st;
            job->file = file;
        }
        job->finished.store(true);
    });

    if (TaskDialogIndirectFn td = task_dialog()) {
        const std::wstring main = L"Downloading kvit-dictate " + wide(to_string(r.version)) + L"…";
        TASKDIALOGCONFIG c{};
        c.cbSize = sizeof(c);
        c.dwFlags = TDF_SHOW_PROGRESS_BAR | TDF_CALLBACK_TIMER | TDF_ALLOW_DIALOG_CANCELLATION;
        c.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        c.pszWindowTitle = kDialogTitle;
        c.pszMainInstruction = main.c_str();
        c.pszContent = L" ";
        c.pfCallback = progress_callback;
        c.lpCallbackData = reinterpret_cast<LONG_PTR>(job.get());
        if (FAILED(td(&c, nullptr, nullptr, nullptr))) {
            // No dialog means no Cancel either; the download simply runs to its end.
            KD_LOG_WARN("the progress dialog could not be shown; downloading without it");
        }
    }
    worker.join();
    std::lock_guard<std::mutex> lock(job->mu);
    out = job->file;
    return job->result;
}

}  // namespace

// ─── Versions ───────────────────────────────────────────────────────────────────────────────

bool parse_version(std::string_view text, Version& out) {
    if (!text.empty() && (text[0] == 'v' || text[0] == 'V')) text.remove_prefix(1);
    // Build metadata (`+sha.1234`) is ignored for ordering, by definition.
    if (const size_t plus = text.find('+'); plus != std::string_view::npos) text = text.substr(0, plus);
    std::string_view core = text;
    std::string_view pre;
    if (const size_t dash = text.find('-'); dash != std::string_view::npos) {
        core = text.substr(0, dash);
        pre = text.substr(dash + 1);
        if (pre.empty()) return false;
        for (std::string_view id : split(pre, '.')) {
            if (id.empty()) return false;
            for (char c : id) {
                if (!is_alnum_or_hyphen(c)) return false;
            }
        }
    }
    const std::vector<std::string_view> parts = split(core, '.');
    if (parts.size() != 3) return false;
    Version v;
    if (!parse_number(parts[0], v.major) || !parse_number(parts[1], v.minor) ||
        !parse_number(parts[2], v.patch)) {
        return false;
    }
    v.pre = std::string(pre);
    out = std::move(v);
    return true;
}

std::string to_string(const Version& v) {
    std::string s = std::to_string(v.major) + "." + std::to_string(v.minor) + "." +
                    std::to_string(v.patch);
    if (!v.pre.empty()) s += "-" + v.pre;
    return s;
}

int compare(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    if (a.pre.empty() != b.pre.empty()) return a.pre.empty() ? 1 : -1;
    return compare_pre(a.pre, b.pre);
}

std::string_view running_version() noexcept {
#ifdef KVIT_VERSION_PRE
    static constexpr char kVersion[] =
        KD_UPDATE_STR(KVIT_VERSION_RAW) "-" KD_UPDATE_STR(KVIT_VERSION_PRE);
#else
    static constexpr char kVersion[] = KD_UPDATE_STR(KVIT_VERSION_RAW);
#endif
    return kVersion;
}

// ─── The release list ───────────────────────────────────────────────────────────────────────

Result<std::optional<Release>> pick_update(std::string_view releases_json, const Version& current,
                                           bool include_prereleases) {
    json::Options opts;
    opts.max_bytes = kMaxApiBytes;
    json::Value root;
    json::Error err;
    if (!json::parse(releases_json, root, err, opts)) {
        return Status{StatusCode::Malformed, "GitHub's release list was not JSON (" +
                                                 std::string(json::to_string(err.code)) + ")"};
    }
    if (!root.is_array()) {
        std::string why = "GitHub's answer was not a release list";
        if (const json::Value* m = root.find("message"); m != nullptr && m->is_string()) {
            why += ": " + std::string(m->as_string());
        }
        return Status{StatusCode::Malformed, why};
    }

    std::optional<Release> best;
    for (size_t i = 0; i < root.size(); ++i) {
        const json::Value* rel = root.at(i);
        if (rel == nullptr || !rel->is_object()) continue;
        const json::Value* draft = rel->find("draft");
        if (draft != nullptr && draft->as_bool(false)) continue;

        const json::Value* tag = rel->find("tag_name");
        if (tag == nullptr || !tag->is_string()) continue;
        Release r;
        r.tag = std::string(tag->as_string());
        if (!parse_version(r.tag, r.version)) continue;
        const json::Value* pre = rel->find("prerelease");
        r.prerelease = (pre != nullptr && pre->as_bool(false)) || r.version.is_prerelease();
        if (r.prerelease && !include_prereleases) continue;
        if (compare(r.version, current) <= 0) continue;
        if (best.has_value() && compare(r.version, best->version) <= 0) continue;

        if (const json::Value* page = rel->find("html_url"); page != nullptr && page->is_string()) {
            r.page_url = std::string(page->as_string());
        }
        const json::Value* assets = rel->find("assets");
        if (assets == nullptr || !assets->is_array()) continue;
        for (size_t k = 0; k < assets->size(); ++k) {
            const json::Value* a = assets->at(k);
            if (a == nullptr || !a->is_object()) continue;
            const json::Value* name = a->find("name");
            const json::Value* url = a->find("browser_download_url");
            if (name == nullptr || !name->is_string() || url == nullptr || !url->is_string()) {
                continue;
            }
            const std::string_view n = name->as_string();
            if (n == kSumsName) {
                r.sums_url = std::string(url->as_string());
            } else if (is_installer_name(n)) {
                r.installer_name = std::string(n);
                r.installer_url = std::string(url->as_string());
                if (const json::Value* size = a->find("size"); size != nullptr) {
                    const int64_t s = size->as_int(0);
                    r.installer_size = s > 0 ? static_cast<uint64_t>(s) : 0;
                }
            }
        }
        if (r.installer_url.empty() || r.sums_url.empty()) continue;
        if (!starts_with(r.installer_url, "https://") || !starts_with(r.sums_url, "https://")) {
            continue;
        }
        if (r.page_url.empty()) r.page_url = kReleasesPage;
        best = std::move(r);
    }
    return best;
}

bool find_checksum(std::string_view sums_text, std::string_view file_name, std::string& hex_out) {
    for (std::string_view line : split(sums_text, '\n')) {
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.size() < 66) continue;
        const std::string_view hash = line.substr(0, 64);
        if (!is_hex64(hash)) continue;
        std::string_view rest = line.substr(64);
        if (rest.empty() || (rest[0] != ' ' && rest[0] != '\t')) continue;
        while (!rest.empty() && (rest[0] == ' ' || rest[0] == '\t')) rest.remove_prefix(1);
        if (!rest.empty() && rest[0] == '*') rest.remove_prefix(1);  // sha256sum's binary mark
        if (rest == file_name) {
            hex_out = lower(hash);
            return true;
        }
    }
    return false;
}

// ─── Network ────────────────────────────────────────────────────────────────────────────────

Result<std::optional<Release>> check_github(const Version& current) {
    std::string body;
    if (Status st = get_text(kApiUrl, /*api=*/true, kMaxApiBytes, body); !st.ok()) return st;
    auto picked = pick_update(body, current, current.is_prerelease());
    if (picked.is_ok()) {
        if (picked->has_value()) {
            KD_LOG_INFO("update: %s is available (running %s)", (*picked)->tag.c_str(),
                        std::string(running_version()).c_str());
        } else {
            KD_LOG_INFO("update: %s is the newest release", std::string(running_version()).c_str());
        }
    }
    return picked;
}

Status download_installer(const Release& r, const std::function<void(uint64_t, uint64_t)>& progress,
                          const std::atomic<bool>& cancel, std::filesystem::path& out_file) {
    if (!is_installer_name(r.installer_name)) {
        return Status{StatusCode::InvalidArgument, "unexpected installer name " + r.installer_name};
    }

    std::string sums;
    if (Status st = get_text(wide(r.sums_url), false, kMaxSumsBytes, sums); !st.ok()) return st;
    std::string expected;
    if (!find_checksum(sums, r.installer_name, expected)) {
        return Status{StatusCode::Malformed,
                      "the release's SHA256SUMS.txt does not list " + r.installer_name +
                          ", so the download cannot be checked."};
    }

    const std::filesystem::path dir = download_dir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path target = dir / wide(r.installer_name);

    // Already here from an earlier attempt (the user said "not now" to installing, or a
    // dictation was in progress and the app quit first): use it if it is the right file.
    if (std::filesystem::exists(target, ec)) {
        auto have = sha256_file(target);
        if (have.is_ok() && *have == expected) {
            KD_LOG_INFO("update: %s already downloaded and verified", r.installer_name.c_str());
            out_file = target;
            return Status::success();
        }
    }
    // Anything else in the folder is a previous version's installer or a broken download.
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        std::error_code ignored;
        std::filesystem::remove(entry.path(), ignored);
    }

    const std::filesystem::path partial = dir / (wide(r.installer_name) + L".partial");
    File f;
    f.h = CreateFileW(partial.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f.h == INVALID_HANDLE_VALUE) {
        return Status{StatusCode::OsError, "could not write to " + dir.string(), GetLastError()};
    }
    uint64_t done = 0;
    Status st = http_get(
        wide(r.installer_url), false,
        [&](const char* d, size_t n, uint64_t total) {
            if (done + n > kMaxInstallerBytes) {
                return Status{StatusCode::Malformed, "the download is far larger than an installer"};
            }
            DWORD wrote = 0;
            if (!WriteFile(f.h, d, static_cast<DWORD>(n), &wrote, nullptr) || wrote != n) {
                return Status{StatusCode::OsError, "could not write the download to disk (is it full?)",
                              GetLastError()};
            }
            done += n;
            if (progress) progress(done, total != 0 ? total : r.installer_size);
            return Status::success();
        },
        &cancel);
    f.close();
    if (!st.ok()) {
        std::filesystem::remove(partial, ec);
        return st;
    }
    if (r.installer_size != 0 && done != r.installer_size) {
        std::filesystem::remove(partial, ec);
        return Status{StatusCode::Malformed, "the download ended early (" + std::to_string(done) +
                                                 " of " + std::to_string(r.installer_size) +
                                                 " bytes). Try again."};
    }
    auto got = sha256_file(partial);
    if (!got.is_ok()) return got.status();
    if (*got != expected) {
        std::filesystem::remove(partial, ec);
        KD_LOG_ERROR("update: %s has SHA-256 %s, the release lists %s; deleted",
                     r.installer_name.c_str(), got->c_str(), expected.c_str());
        return Status{StatusCode::Malformed,
                      "the downloaded installer does not match the checksum the release lists, so "
                      "it was deleted rather than run. Try again."};
    }
    if (!MoveFileExW(partial.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return Status{StatusCode::OsError, "could not rename the download", GetLastError()};
    }
    KD_LOG_INFO("update: downloaded %s (%llu bytes), SHA-256 verified", r.installer_name.c_str(),
                static_cast<unsigned long long>(done));
    out_file = target;
    return Status::success();
}

void remove_stale_downloads(const Version& running) {
    const std::filesystem::path dir = download_dir();
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = narrow(entry.path().filename().wstring());
        bool stale = true;
        if (is_installer_name(name)) {
            // kvit-dictate-1.0.1-setup.exe -> 1.0.1. The file name has no pre-release part, so
            // the comparison drops the running one's too: on 1.0.1-rc2 the 1.0.1 installer that
            // just installed it goes. At worst that deletes a downloaded rc3 the user said "not
            // now" to, and the next Install downloads it again.
            const std::string_view middle = std::string_view(name).substr(
                std::strlen(kInstallerPrefix),
                name.size() - std::strlen(kInstallerPrefix) - std::strlen(kInstallerSuffix));
            Version v;
            Version base = running;
            base.pre.clear();
            stale = parse_version(middle, v) && compare(v, base) <= 0;
        }
        if (!stale) continue;
        std::error_code ignored;
        if (std::filesystem::remove(entry.path(), ignored)) {
            KD_LOG_INFO("update: removed %s from an earlier update", name.c_str());
        }
    }
    std::filesystem::remove(dir, ec);  // only succeeds once it is empty
}

// ─── Installing ─────────────────────────────────────────────────────────────────────────────

bool same_directory(std::wstring_view a, std::wstring_view b) {
    auto trim = [](std::wstring s) {
        for (wchar_t& c : s) {
            if (c == L'/') c = L'\\';
        }
        while (!s.empty() && s.back() == L'\\') s.pop_back();
        return s;
    };
    const std::wstring x = trim(std::wstring(a));
    const std::wstring y = trim(std::wstring(b));
    if (x.empty() || y.empty()) return false;
    return CompareStringOrdinal(x.c_str(), static_cast<int>(x.size()), y.c_str(),
                                static_cast<int>(y.size()), TRUE) == CSTR_EQUAL;
}

InstallKind installed_here(const std::filesystem::path& exe_dir) {
    const struct {
        HKEY root;
        InstallKind kind;
    } places[] = {{HKEY_CURRENT_USER, InstallKind::PerUser},
                  {HKEY_LOCAL_MACHINE, InstallKind::AllUsers}};
    for (const auto& p : places) {
        wchar_t buf[MAX_PATH * 2] = {};
        DWORD size = sizeof(buf);
        if (RegGetValueW(p.root, kUninstallKey, L"InstallLocation", RRF_RT_REG_SZ, nullptr, buf,
                         &size) == ERROR_SUCCESS &&
            same_directory(buf, exe_dir.wstring())) {
            return p.kind;
        }
    }
    return InstallKind::NotInstalled;
}

std::wstring installer_command_line(const std::filesystem::path& setup, InstallKind kind,
                                    uint32_t wait_pid) {
    std::wstring cmd = L"\"" + setup.wstring() + L"\"";
    // /SILENT, not /VERYSILENT: the user just asked for this and should see it happening.
    cmd += L" /SILENT /SUPPRESSMSGBOXES /NORESTART";
    // kvit-dictate.iss: wait for this process to exit, then start the new version afterwards.
    cmd += L" /WAITPID=" + std::to_wstring(wait_pid) + L" /RELAUNCH=1";
    // The same mode it was installed in, so the upgrade lands on top of the existing copy and
    // asks no question. All-users asks for UAC, which is what installing there means.
    cmd += kind == InstallKind::AllUsers ? L" /ALLUSERS" : L" /CURRENTUSER";
    cmd += L" /LOG=\"" + (setup.parent_path() / L"install.log").wstring() + L"\"";
    return cmd;
}

Status launch_installer(const std::filesystem::path& setup, InstallKind kind, uint32_t wait_pid) {
    std::wstring cmd = installer_command_line(setup, kind, wait_pid);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // Out of any job this process is in. One with KILL_ON_JOB_CLOSE (some launchers and
    // terminals start programs in one) would take the installer down with this process, which is
    // exactly the moment the installer starts working.
    BOOL ok = CreateProcessW(setup.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                             CREATE_BREAKAWAY_FROM_JOB, nullptr, setup.parent_path().c_str(), &si, &pi);
    DWORD err = ok ? 0 : GetLastError();
    if (!ok && err == ERROR_ACCESS_DENIED) {
        // A job that forbids breakaway. Try anyway; most such jobs do not kill on close.
        ok = CreateProcessW(setup.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                            setup.parent_path().c_str(), &si, &pi);
        err = ok ? 0 : GetLastError();
    }
    if (!ok) {
        KD_LOG_ERROR("update: could not start %s: %lu", setup.string().c_str(),
                     static_cast<unsigned long>(err));
        return Status{StatusCode::OsError, "the installer could not be started", err};
    }
    KD_LOG_INFO("update: started the installer, pid %lu", static_cast<unsigned long>(pi.dwProcessId));
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return Status::success();
}

// ─── The dialogs ────────────────────────────────────────────────────────────────────────────

void show_error(std::string_view headline, std::string_view detail) {
    message(TD_ERROR_ICON, MB_ICONERROR, wide(headline), wide(detail));
}

void run_interactive(const UiContext& ctx, std::optional<Release> known) {
    const std::string current_text(running_version());
    Version current;
    if (!parse_version(current_text, current)) {
        show_error("This build's version could not be read.", current_text);
        return;
    }

    Release rel;
    if (known.has_value()) {
        rel = std::move(*known);
    } else {
        auto found = check_github(current);
        if (!found.is_ok()) {
            show_error("Could not check for updates.", found.status().message());
            return;
        }
        if (!found->has_value()) {
            message(TD_INFORMATION_ICON, MB_ICONINFORMATION, L"kvit-dictate is up to date.",
                    L"You have version " + wide(current_text) +
                        L", the newest on <a href=\"" + wide(kReleasesPage) + L"\">GitHub</a>.");
            return;
        }
        rel = std::move(**found);
    }

    const std::wstring newer = wide(to_string(rel.version));
    const std::wstring notes =
        L"<a href=\"" + wide(rel.page_url) + L"\">What's new in " + newer + L"</a>";
    const InstallKind kind = installed_here(ctx.exe_dir);
    if (kind == InstallKind::NotInstalled) {
        // A portable copy or a build: the installer would put a second copy somewhere else.
        const int choice = ask(L"kvit-dictate " + newer + L" is available.",
                               L"You have " + wide(current_text) +
                                   L". This copy was not installed by the installer (it is the "
                                   L"portable ZIP or a build), so it cannot update itself.\n\n" +
                                   notes,
                               L"Open the download page", L"Download it from GitHub yourself",
                               L"Not now");
        if (choice == 1) {
            ShellExecuteW(nullptr, L"open", wide(rel.page_url).c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
        }
        return;
    }

    const int choice = ask(L"kvit-dictate " + newer + L" is available.",
                           L"You have " + wide(current_text) + L".\n\n" + notes,
                           L"Install now",
                           L"Downloads it, closes kvit-dictate, installs, and starts it again.",
                           L"Not now");
    if (choice != 1) return;

    std::filesystem::path setup;
    const Status st = download_with_progress(rel, setup);
    if (!st.ok()) {
        if (st.message() != "cancelled") show_error("The update could not be downloaded.", st.message());
        return;
    }
    if (ctx.on_ready) ctx.on_ready(setup, kind);
}

}  // namespace kd::update
