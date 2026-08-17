// The named-pipe transport. Specification: ipc_client.h, docs/protocol.md §2 and §9,
// plan.md §1.2.
//
// This is the only translation unit in the project that opens a pipe or spawns a process.
// Everything here is Windows; the framing and message codec it calls into (protocol.h) is pure.
//
// ─── Facts measured on this machine (Windows 11 26200, SDK 10.0.26100), not assumed ─────────
//
// The security and I/O decisions below were checked against a scratch probe before being
// written. Recorded here because each one is the kind of thing that otherwise gets "fixed" by
// someone who assumed the opposite:
//
//   * The logon SID is present in TokenGroups with SE_GROUP_LOGON_ID (S-1-5-5-X-Y).
//   * With the §2.3 DACL applied, GetSecurityInfo on the pipe reads back exactly one
//     ACCESS_ALLOWED ACE, mask 0x00100183, for that SID. Everyone/ANONYMOUS are absent.
//   * A client opening with GENERIC_READ | GENERIC_WRITE is **denied** (ERROR_ACCESS_DENIED):
//     FILE_GENERIC_READ is 0x00120089 and additionally wants FILE_READ_EA and READ_CONTROL.
//     The sidecar must ask for exactly kPipeAccessRights. See the note in ipc_client.h.
//   * A second CreateNamedPipeW with FILE_FLAG_FIRST_PIPE_INSTANCE fails ERROR_ACCESS_DENIED —
//     that is the squat detection, and it is why the failure is reported as `pipe_squatted`.
//   * Overlapped ReadFile on a message-mode pipe whose buffer is too small returns FALSE with
//     ERROR_MORE_DATA **and leaves lpNumberOfBytesRead at 0**; the real count comes from
//     GetOverlappedResult, which itself returns FALSE with ERROR_MORE_DATA. A reader that trusts
//     the inline count silently loses every byte of an oversized message.
//   * AssignProcessToJobObject succeeds even though this process is already inside a job
//     (nested jobs, Windows 8+), and closing the job handle does kill the child.
//   * FlushFileBuffers returns immediately when the peer is gone — but it is documented to block
//     until the client has read everything when the peer is alive and not reading, which is the
//     teardown deadlock stop() is written to avoid. See stop().

#define KD_LOG_MODULE "ipc"

#include "ipc_client.h"

#include <windows.h>
// The three below must follow windows.h.
#include <aclapi.h>
#include <bcrypt.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "util/clock.h"
#include "util/log.h"

namespace kd {
namespace {

// ─── The access mask, cross-checked against the SDK ──────────────────────────────────────────
//
// ipc_client.h spells kPipeAccessRights numerically because no header under src/ may include
// <windows.h>. These asserts are the join between the two spellings, so the header cannot drift
// away from what the ACE actually grants.
static_assert(kPipeAccessRights == (FILE_READ_DATA | FILE_WRITE_DATA | FILE_READ_ATTRIBUTES |
                                    FILE_WRITE_ATTRIBUTES | SYNCHRONIZE),
              "kPipeAccessRights must equal the five rights docs/protocol.md §2.3 names");
static_assert(kFileCreatePipeInstance == FILE_CREATE_PIPE_INSTANCE, "");
static_assert((kPipeAccessRights & FILE_CREATE_PIPE_INSTANCE) == 0,
              "granting FILE_CREATE_PIPE_INSTANCE would let a client create rival instances");
static_assert((FILE_GENERIC_WRITE & FILE_CREATE_PIPE_INSTANCE) != 0,
              "the reason FILE_GENERIC_WRITE is refused; if this ever stops holding, revisit §2.3");

// ─── Tunables that are not protocol constants ────────────────────────────────────────────────

// docs/protocol.md §2.2: nOutBufferSize / nInBufferSize = 4 × MAX_FRAME_ON_WIRE, rounded.
constexpr DWORD kPipeBufferBytes = 262144;

// One read must be able to take the largest legal frame in a single message-mode read, or the
// ERROR_MORE_DATA path runs on every big frame. 65541 rounded up.
constexpr size_t kReadBufferBytes = 66560;

// How long stop() waits for the I/O thread's orderly flush before killing the child to unwedge
// it. Not a protocol constant — a teardown policy. See stop().
constexpr DWORD kFlushGraceMs = 1000;

// How long stop() waits for a child to exit on its own after the pipe is disconnected, before
// the job object takes it out. Long enough for a Python interpreter to run its atexit handlers
// and free VRAM cleanly; short enough that quitting the app is never a visible wait.
constexpr DWORD kChildExitGraceMs = 3000;

// AUDIO_QUEUE_MAX_MS expressed in samples, computed once from the protocol constants so the two
// numbers cannot disagree.
constexpr size_t kMaxQueuedSamples =
    static_cast<size_t>(proto::kAudioQueueMaxMs) * proto::kSampleRate / 1000;

using Millis = kd::Millis;

Millis now_ms() noexcept { return SteadyClock::instance().now_ms(); }

// ─── RAII ────────────────────────────────────────────────────────────────────────────────────
//
// Every handle in this file lives in one of these. Not style: the paths below early-return on a
// dozen distinct Win32 failures, and a manual CloseHandle at the bottom of such a function is a
// leak waiting for the next early return to be added.
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) noexcept : h_(normalise(h)) {}
    ~Handle() { close(); }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : h_(o.release()) {}
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) {
            close();
            h_ = o.release();
        }
        return *this;
    }

    void reset(HANDLE h = nullptr) noexcept {
        close();
        h_ = normalise(h);
    }
    [[nodiscard]] HANDLE get() const noexcept { return h_; }
    [[nodiscard]] bool valid() const noexcept { return h_ != nullptr; }
    HANDLE release() noexcept {
        HANDLE h = h_;
        h_ = nullptr;
        return h;
    }
    void close() noexcept {
        if (h_ != nullptr) {
            CloseHandle(h_);
            h_ = nullptr;
        }
    }

private:
    // CreateFile-family failures are INVALID_HANDLE_VALUE; CreateEvent-family failures are NULL.
    // Collapsing both to NULL means valid() has one answer instead of two.
    static HANDLE normalise(HANDLE h) noexcept { return h == INVALID_HANDLE_VALUE ? nullptr : h; }
    HANDLE h_ = nullptr;
};

// LocalFree for the SDDL/GetSecurityInfo allocations, which use LocalAlloc, not the CRT.
template <typename T>
class LocalPtr {
public:
    LocalPtr() = default;
    ~LocalPtr() {
        if (p_ != nullptr) LocalFree(p_);
    }
    LocalPtr(const LocalPtr&) = delete;
    LocalPtr& operator=(const LocalPtr&) = delete;

    T** receive() noexcept { return &p_; }
    [[nodiscard]] T* get() const noexcept { return p_; }

private:
    T* p_ = nullptr;
};

Status os_failure(StatusCode code, const char* what, DWORD err) {
    char buf[256];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s failed (win32 %lu)", what, err);
    return Status{code, std::string(buf), static_cast<uint32_t>(err)};
}

// ─── Command line and environment ────────────────────────────────────────────────────────────

// CommandLineToArgvW's quoting rules, which are not the shell's. A backslash is literal except
// in a run immediately preceding a quote, where the run is doubled. Getting this wrong turns an
// interpreter path with a trailing backslash into a swallowed quote and a garbled argv.
void append_quoted_arg(std::wstring& out, const std::wstring& arg) {
    if (!out.empty()) out.push_back(L' ');
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        out += arg;
        return;
    }
    out.push_back(L'"');
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++backslashes;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        out.push_back(arg[i]);
    }
    out.push_back(L'"');
}

bool env_key_equals(const std::wstring& entry, const std::wstring& key) {
    const size_t eq = entry.find(L'=');
    if (eq == std::wstring::npos) return false;
    if (eq != key.size()) return false;
    // Environment variable names are case-insensitive on Windows; ordinal-insensitive compare is
    // the right one (a linguistic compare would make "STRASSE"/"straße" collide).
    return CompareStringOrdinal(entry.c_str(), static_cast<int>(eq), key.c_str(),
                                static_cast<int>(key.size()), TRUE) == CSTR_EQUAL;
}

// Inherited block plus overrides, double-NUL terminated. CreateProcessW documents that a Unicode
// environment block must be sorted, so it is sorted — ordinal, case-insensitive, with the
// "=C:=..." drive-current-directory entries left where they belong, at the front.
std::wstring build_environment(const std::vector<std::pair<std::wstring, std::wstring>>& overrides) {
    std::vector<std::wstring> entries;

    LPWCH block = GetEnvironmentStringsW();
    if (block != nullptr) {
        for (const wchar_t* p = block; *p != L'\0';) {
            std::wstring entry(p);
            p += entry.size() + 1;
            bool replaced = false;
            for (const auto& ov : overrides) {
                if (env_key_equals(entry, ov.first)) {
                    replaced = true;
                    break;
                }
            }
            if (!replaced) entries.push_back(std::move(entry));
        }
        FreeEnvironmentStringsW(block);
    }
    for (const auto& ov : overrides) {
        entries.push_back(ov.first + L'=' + ov.second);
    }

    std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        const bool ad = !a.empty() && a[0] == L'=';
        const bool bd = !b.empty() && b[0] == L'=';
        if (ad != bd) return ad;
        return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                    static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
    });

    std::wstring out;
    for (const auto& e : entries) {
        out.append(e);
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
}

}  // namespace

// ─── Pipe naming ─────────────────────────────────────────────────────────────────────────────

std::wstring format_pipe_name(const uint8_t random16[16]) {
    static const wchar_t kHex[] = L"0123456789abcdef";
    std::wstring name = L"\\\\.\\pipe\\kvit-dictate-";
    name.reserve(name.size() + 32);
    for (int i = 0; i < 16; ++i) {
        name.push_back(kHex[(random16[i] >> 4) & 0x0F]);
        name.push_back(kHex[random16[i] & 0x0F]);
    }
    return name;
}

Result<std::wstring> generate_pipe_name() {
    uint8_t bytes[16];
    // BCRYPT_USE_SYSTEM_PREFERRED_RNG: no algorithm handle to open, close, or leak, and the
    // system's preferred RNG is exactly what docs/protocol.md §2.2 specifies.
    const NTSTATUS st = BCryptGenRandom(nullptr, bytes, sizeof(bytes),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(st)) {
        // No fallback. rand() or GetTickCount would produce a name an attacker can predict and
        // therefore squat, and — worse — the degradation would be invisible.
        char buf[128];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "BCryptGenRandom failed (NTSTATUS 0x%08lX)",
                    static_cast<unsigned long>(st));
        return Status{StatusCode::OsError, std::string(buf), static_cast<uint32_t>(st)};
    }
    return format_pipe_name(bytes);
}

// ─── SecurityDescriptorBuilder ───────────────────────────────────────────────────────────────

struct SecurityDescriptorBuilder::Impl {
    std::vector<uint8_t> sid;   // a private copy: the token buffer it came from does not persist
    std::vector<uint8_t> acl;   // the SD points into this, so both live as long as the builder
    SECURITY_DESCRIPTOR sd{};
    SECURITY_ATTRIBUTES sa{};
    bool built = false;
    bool logon_sid = false;
};

SecurityDescriptorBuilder::SecurityDescriptorBuilder() : impl_(new Impl) {}
SecurityDescriptorBuilder::~SecurityDescriptorBuilder() { delete impl_; }

Status SecurityDescriptorBuilder::build() {
    Impl& im = *impl_;
    im.built = false;
    im.logon_sid = false;

    Handle token;
    {
        HANDLE raw = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) {
            return os_failure(StatusCode::OsError, "OpenProcessToken", GetLastError());
        }
        token.reset(raw);
    }

    // The logon SID lives in TokenGroups, flagged SE_GROUP_LOGON_ID. Microsoft's guidance
    // (quoted in docs/protocol.md §2.3): "To prevent remote users or users on a different
    // terminal services session from accessing a named pipe, use the logon SID on the DACL."
    DWORD len = 0;
    GetTokenInformation(token.get(), TokenGroups, nullptr, 0, &len);
    if (len == 0) {
        return os_failure(StatusCode::OsError, "GetTokenInformation(TokenGroups) sizing",
                          GetLastError());
    }
    std::vector<uint8_t> groups(len);
    if (!GetTokenInformation(token.get(), TokenGroups, groups.data(), len, &len)) {
        return os_failure(StatusCode::OsError, "GetTokenInformation(TokenGroups)", GetLastError());
    }

    PSID chosen = nullptr;
    const TOKEN_GROUPS* tg = reinterpret_cast<const TOKEN_GROUPS*>(groups.data());
    for (DWORD i = 0; i < tg->GroupCount; ++i) {
        if ((tg->Groups[i].Attributes & SE_GROUP_LOGON_ID) != 0) {
            chosen = tg->Groups[i].Sid;
            im.logon_sid = true;
            break;
        }
    }

    std::vector<uint8_t> user;  // must outlive `chosen` when the fallback is taken
    if (chosen == nullptr) {
        // Fallback: the user SID. **This is weaker and the difference is not cosmetic.** The user
        // SID is the same value in every concurrent logon session of this user, so a process in
        // another terminal-services session — an RDP session, a fast-user-switched desktop —
        // matches the ACE and can open the pipe. The logon SID is unique per logon session and is
        // the only one of the two that draws that boundary. We take the fallback rather than
        // refusing to start, because a token with no SE_GROUP_LOGON_ID group is a configuration
        // we have not seen and would rather degrade loudly than crash on; the warning below is
        // the loud part.
        DWORD ulen = 0;
        GetTokenInformation(token.get(), TokenUser, nullptr, 0, &ulen);
        if (ulen == 0) {
            return os_failure(StatusCode::OsError, "GetTokenInformation(TokenUser) sizing",
                              GetLastError());
        }
        user.resize(ulen);
        if (!GetTokenInformation(token.get(), TokenUser, user.data(), ulen, &ulen)) {
            return os_failure(StatusCode::OsError, "GetTokenInformation(TokenUser)", GetLastError());
        }
        chosen = reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid;
        KD_LOG_WARN(
            "no logon SID (SE_GROUP_LOGON_ID) in this token; falling back to the user SID. The "
            "pipe DACL no longer separates concurrent terminal-services sessions of this user.");
    }

    if (!IsValidSid(chosen)) {
        return Status{StatusCode::Internal, "token returned an invalid SID"};
    }
    const DWORD sid_len = GetLengthSid(chosen);
    im.sid.assign(sid_len, 0);
    if (!CopySid(sid_len, im.sid.data(), chosen)) {
        return os_failure(StatusCode::OsError, "CopySid", GetLastError());
    }

    // ACCESS_ALLOWED_ACE already contains the first DWORD of the SID (SidStart), hence the
    // subtraction — the classic ACL sizing formula, wrong by four bytes if copied carelessly.
    const DWORD acl_bytes =
        static_cast<DWORD>(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + sid_len);
    im.acl.assign(acl_bytes, 0);
    ACL* acl = reinterpret_cast<ACL*>(im.acl.data());
    if (!InitializeAcl(acl, acl_bytes, ACL_REVISION)) {
        return os_failure(StatusCode::OsError, "InitializeAcl", GetLastError());
    }
    if (!AddAccessAllowedAce(acl, ACL_REVISION, kPipeAccessRights,
                             reinterpret_cast<PSID>(im.sid.data()))) {
        return os_failure(StatusCode::OsError, "AddAccessAllowedAce", GetLastError());
    }

    if (!InitializeSecurityDescriptor(&im.sd, SECURITY_DESCRIPTOR_REVISION)) {
        return os_failure(StatusCode::OsError, "InitializeSecurityDescriptor", GetLastError());
    }
    // bDaclPresent = TRUE with a non-null ACL. TRUE with a null ACL would be a NULL DACL, which
    // grants everyone; FALSE would leave the default descriptor, which grants Everyone read.
    // Three spellings, one correct, and the wrong two fail silently — which is why
    // tests/test_ipc.cpp reads the result back off a real pipe object.
    if (!SetSecurityDescriptorDacl(&im.sd, TRUE, acl, FALSE)) {
        return os_failure(StatusCode::OsError, "SetSecurityDescriptorDacl", GetLastError());
    }

    im.sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    im.sa.lpSecurityDescriptor = &im.sd;
    // The child opens the pipe by name; it must never inherit the pipe-server handle (§2.2).
    im.sa.bInheritHandle = FALSE;
    im.built = true;
    return Status::ok();
}

bool SecurityDescriptorBuilder::is_built() const noexcept { return impl_->built; }
bool SecurityDescriptorBuilder::used_logon_sid() const noexcept { return impl_->logon_sid; }

std::string SecurityDescriptorBuilder::sid_string() const {
    if (!impl_->built) return {};
    LocalPtr<wchar_t> str;
    if (!ConvertSidToStringSidW(reinterpret_cast<PSID>(impl_->sid.data()), str.receive())) {
        return {};
    }
    // A SID string is ASCII by construction, so this narrowing is exact; utf8::from_utf16 would
    // be the general answer but would drag the whole conversion in for "S-1-5-5-0-…".
    std::string out;
    for (const wchar_t* p = str.get(); *p != L'\0'; ++p) out.push_back(static_cast<char>(*p));
    return out;
}

uintptr_t SecurityDescriptorBuilder::security_attributes() const noexcept {
    return impl_->built ? reinterpret_cast<uintptr_t>(&impl_->sa) : 0;
}

// ─── Pipe creation ───────────────────────────────────────────────────────────────────────────

Result<uintptr_t> create_pipe_instance(const std::wstring& pipe_name,
                                       const SecurityDescriptorBuilder& sec) {
    if (sec.security_attributes() == 0) {
        return Status{StatusCode::InvalidArgument,
                      "create_pipe_instance called with an unbuilt security descriptor"};
    }
    SECURITY_ATTRIBUTES* sa = reinterpret_cast<SECURITY_ATTRIBUTES*>(sec.security_attributes());

    const HANDLE h = CreateNamedPipeW(
        pipe_name.c_str(),
        // FILE_FLAG_OVERLAPPED so a wedged peer can never block a thread past WRITE_DEADLINE_MS;
        // FILE_FLAG_FIRST_PIPE_INSTANCE so a squatter is ERROR_ACCESS_DENIED, not a silent second
        // instance we would then talk to.
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,  // nMaxInstances: exactly one sidecar per name, so a rival connector gets PIPE_BUSY
        kPipeBufferBytes, kPipeBufferBytes,
        0,  // nDefaultTimeOut: unused, every wait here is explicit
        sa);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            return Status{StatusCode::PermissionDenied,
                          "pipe_squatted: the pipe name already exists "
                          "(FILE_FLAG_FIRST_PIPE_INSTANCE refused to create a second instance)",
                          static_cast<uint32_t>(err)};
        }
        return os_failure(StatusCode::OsError, "CreateNamedPipeW", err);
    }
    return static_cast<uintptr_t>(reinterpret_cast<uintptr_t>(h));
}

// ─── IpcClient::Impl ─────────────────────────────────────────────────────────────────────────

namespace {

// One queue entry. Control and audio share a single FIFO on purpose: audio frames carry no
// session_id and are attributed positionally (docs §6.3), so a control frame that overtook queued
// audio would move the `stop` boundary and silently discard the tail of an utterance.
struct QueuedFrame {
    std::vector<uint8_t> bytes;
    bool audio = false;
    size_t samples = 0;  // audio only, for the AUDIO_QUEUE_MAX_MS bound
};

}  // namespace

struct IpcClient::Impl {
    Callbacks cb;
    std::wstring pipe_name;
    std::string model_id;

    Handle pipe;
    Handle child_process;
    Handle job;
    Handle child_out;  // read end of the child's stdout+stderr

    Handle ev_read, ev_write, ev_wake, ev_quit, ev_conn;

    std::thread io_thread;
    HANDLE io_native = nullptr;  // io_thread.native_handle(), cached for a *timed* join
    std::thread log_thread;

    mutable std::mutex mu;
    std::condition_variable cv;
    std::deque<QueuedFrame> queue;
    uint32_t control_depth = 0;
    size_t audio_items = 0;
    size_t audio_samples = 0;
    Stats stats;
    bool running = false;

    std::atomic<uint32_t> state{static_cast<uint32_t>(proto::ConnState::Dead)};
    std::atomic<uint32_t> pid{0};
    std::atomic<bool> overloaded{false};

    // ── I/O-thread-only state. No lock, because exactly one thread touches it. ──
    proto::FrameReader reader;
    std::vector<uint8_t> read_buf;
    OVERLAPPED rov{};
    OVERLAPPED wov{};
    bool read_pending = false;
    bool write_pending = false;
    QueuedFrame write_item;
    Millis write_started = 0;
    Millis state_deadline = 0;  // 0 = no deadline in this state
    bool io_done = false;       // stop looping
    bool reported = false;      // on_error already fired; never fire twice
    bool orderly = false;       // teardown may attempt the §2.5 flush

    Impl() { read_buf.resize(kReadBufferBytes); }

    std::atomic<bool> state_notify{false};

    // ── helpers ──

    // I/O thread only: the callback contract in ipc_client.h says every callback runs there.
    void set_state(proto::ConnState s) {
        const uint32_t v = static_cast<uint32_t>(s);
        if (state.exchange(v) == v) return;
        KD_LOG_DEBUG("state -> %s", proto::to_string(s));
        if (cb.on_state) cb.on_state(s);
    }

    // Callable from any thread. Updates the state immediately (so state() is never stale) but
    // hands the *notification* to the I/O thread, because on_state must not fire on the WASAPI
    // capture thread or the UI thread — session.h and the tray are single-threaded by design.
    void set_state_async(proto::ConnState s) {
        const uint32_t v = static_cast<uint32_t>(s);
        if (state.exchange(v) == v) return;
        state_notify.store(true);
        if (ev_wake.valid()) SetEvent(ev_wake.get());
    }
    [[nodiscard]] proto::ConnState get_state() const noexcept {
        return static_cast<proto::ConnState>(state.load());
    }

    void fail(proto::ErrorCode code, std::string msg, uint64_t at_offset = 0) {
        proto::ProtocolError e;
        e.code = code;
        e.fatal = true;
        e.msg = std::move(msg);
        e.at_offset = at_offset;
        fail_with(e);
    }

    void fail_with(const proto::ProtocolError& e) {
        io_done = true;
        orderly = false;
        set_state(proto::ConnState::Dead);
        if (reported) return;
        reported = true;
        KD_LOG_ERROR("%s", e.to_string().c_str());
        if (cb.on_error) cb.on_error(e);
    }

    bool create_events();
    bool do_connect(DWORD expect_pid);
    void io_main(bool need_connect, DWORD expect_pid);
    bool issue_read();
    void complete_read();
    void drain_frames();
    void handle_frame(const proto::Frame& f);
    void on_control(const proto::Message& m);
    void try_issue_write();
    void on_write_complete(DWORD written);
    DWORD next_timeout() const;
    void check_deadlines();
    void enqueue_internal(std::vector<uint8_t> bytes);
    void log_main();
    void teardown_pipe();
};

bool IpcClient::Impl::create_events() {
    // Manual reset, initially unsignalled: an OVERLAPPED event must be manual-reset, because the
    // kernel signals it once and we may examine it after the fact.
    for (Handle* h : {&ev_read, &ev_write, &ev_wake, &ev_quit, &ev_conn}) {
        h->reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!h->valid()) return false;
    }
    return true;
}

void IpcClient::Impl::enqueue_internal(std::vector<uint8_t> bytes) {
    {
        std::lock_guard<std::mutex> lk(mu);
        queue.push_back(QueuedFrame{std::move(bytes), false, 0});
        ++control_depth;
    }
    SetEvent(ev_wake.get());
}

bool IpcClient::Impl::do_connect(DWORD expect_pid) {
    set_state(proto::ConnState::Spawning);
    ResetEvent(ev_conn.get());
    OVERLAPPED cov{};
    cov.hEvent = ev_conn.get();

    BOOL ok = ConnectNamedPipe(pipe.get(), &cov);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();

    if (!ok && err == ERROR_PIPE_CONNECTED) {
        // The client won the race between CreateNamedPipeW and ConnectNamedPipe. Documented as a
        // good connection despite the FALSE return; treating it as an error would make a fast
        // child look like a failure.
        ok = TRUE;
    } else if (!ok && err == ERROR_IO_PENDING) {
        HANDLE waits[3] = {ev_conn.get(), ev_quit.get(), child_process.get()};
        const DWORD count = child_process.valid() ? 3u : 2u;
        const DWORD w = WaitForMultipleObjects(count, waits, FALSE, proto::kConnectTimeoutMs);

        if (w != WAIT_OBJECT_0) {
            // Cancel, then *wait* for the cancelled operation: `cov` is on this stack frame and
            // the kernel may still be writing to it. CancelIoEx only requests cancellation.
            CancelIoEx(pipe.get(), &cov);
            DWORD n = 0;
            GetOverlappedResult(pipe.get(), &cov, &n, TRUE);
        }
        if (w == WAIT_TIMEOUT) {
            fail(proto::ErrorCode::spawn_timeout,
                 "no client connected within CONNECT_TIMEOUT_MS");
            return false;
        }
        if (w == WAIT_OBJECT_0 + 1) {
            io_done = true;  // stop() during connect: orderly, nothing to report
            return false;
        }
        if (w == WAIT_OBJECT_0 + 2) {
            fail(proto::ErrorCode::spawn_timeout, "child exited before connecting to the pipe");
            return false;
        }
        if (w != WAIT_OBJECT_0) {
            fail(proto::ErrorCode::internal_error, "WaitForMultipleObjects failed during connect");
            return false;
        }
        DWORD n = 0;
        if (!GetOverlappedResult(pipe.get(), &cov, &n, FALSE)) {
            fail(proto::ErrorCode::internal_error, "ConnectNamedPipe did not complete");
            return false;
        }
    } else if (!ok) {
        fail(proto::ErrorCode::internal_error, "ConnectNamedPipe failed");
        return false;
    }

    set_state(proto::ConnState::Validating);

    ULONG peer = 0;
    if (!GetNamedPipeClientProcessId(pipe.get(), &peer)) {
        fail(proto::ErrorCode::peer_mismatch, "GetNamedPipeClientProcessId failed");
        return false;
    }
    if (expect_pid != 0 && peer != expect_pid) {
        // A name guess is not enough. PID recycling is not a hole here: the child's process
        // handle is held open for its whole lifetime, and Windows will not recycle a PID while a
        // handle to that process exists.
        char buf[160];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "peer_mismatch: pipe client is pid %lu, expected the child pid %lu",
                    static_cast<unsigned long>(peer), static_cast<unsigned long>(expect_pid));
        if (child_process.valid()) TerminateProcess(child_process.get(), 1);
        DisconnectNamedPipe(pipe.get());
        fail(proto::ErrorCode::peer_mismatch, buf);
        return false;
    }
    return true;
}

bool IpcClient::Impl::issue_read() {
    if (read_pending) return true;
    ResetEvent(ev_read.get());
    std::memset(&rov, 0, sizeof(rov));
    rov.hEvent = ev_read.get();

    DWORD n = 0;
    const BOOL ok = ReadFile(pipe.get(), read_buf.data(), static_cast<DWORD>(read_buf.size()), &n,
                             &rov);
    if (ok) {
        read_pending = true;  // completed inline; complete_read() collects the count
        SetEvent(ev_read.get());
        return true;
    }
    const DWORD err = GetLastError();
    if (err == ERROR_IO_PENDING) {
        read_pending = true;
        return true;
    }
    if (err == ERROR_MORE_DATA) {
        read_pending = true;
        SetEvent(ev_read.get());
        return true;
    }
    if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_NO_DATA) {
        reader.notify_eof();
        drain_frames();
        io_done = true;
        return false;
    }
    fail(proto::ErrorCode::internal_error, "ReadFile on the pipe failed");
    return false;
}

void IpcClient::Impl::complete_read() {
    DWORD n = 0;
    BOOL ok = GetOverlappedResult(pipe.get(), &rov, &n, FALSE);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    read_pending = false;
    ResetEvent(ev_read.get());

    if (!ok && err == ERROR_MORE_DATA) {
        // The message is larger than our buffer. `n` from GetOverlappedResult is the real count
        // (the inline count from ReadFile is 0 in this case — measured). docs §2.2: continue
        // reading the remainder into the same frame stream. The FrameReader is a byte-stream
        // decoder, so nothing special is needed beyond not losing these bytes.
        ok = TRUE;
        err = ERROR_SUCCESS;
    }
    if (!ok) {
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_NO_DATA ||
            err == ERROR_OPERATION_ABORTED) {
            reader.notify_eof();
            drain_frames();
            io_done = true;
            return;
        }
        fail(proto::ErrorCode::internal_error, "GetOverlappedResult on the pipe read failed");
        return;
    }
    if (n > 0) {
        reader.push(read_buf.data(), static_cast<size_t>(n));
        drain_frames();
    }
}

void IpcClient::Impl::drain_frames() {
    // Drain until NeedMore. One frame per read fails fixture 002 (three frames in one delivery),
    // and on a message-mode pipe a coalesced delivery is exactly what ERROR_MORE_DATA reassembly
    // produces.
    for (;;) {
        proto::Frame f;
        switch (reader.next(f)) {
            case proto::FrameStatus::Frame:
                handle_frame(f);
                if (io_done) return;
                continue;
            case proto::FrameStatus::NeedMore:
                return;
            case proto::FrameStatus::EndOfStream:
                // Orderly shutdown at a frame boundary (fixture 010): not an error.
                KD_LOG_INFO("peer closed the connection at a frame boundary");
                io_done = true;
                orderly = true;
                set_state(proto::ConnState::Dead);
                return;
            case proto::FrameStatus::Error:
                fail_with(reader.error());
                return;
        }
    }
}

void IpcClient::Impl::handle_frame(const proto::Frame& f) {
    if (f.type == proto::FrameType::Audio) {
        // Type 0x02 is defined, but only Client -> Server (docs §6.3). A Server that sends one is
        // not speaking proto 1, and §11.3 puts "message illegal in the current state" under
        // state_violation rather than unknown_frame_type — the byte is known, the direction is not.
        fail(proto::ErrorCode::state_violation,
             "server sent an audio frame; audio is Client -> Server only",
             reader.stream_offset());
        return;
    }
    proto::Message m;
    proto::ProtocolError err;
    if (!proto::decode_control(f.payload, m, err)) {
        err.at_offset = reader.stream_offset();
        fail_with(err);
        return;
    }
    on_control(m);
}

void IpcClient::Impl::on_control(const proto::Message& m) {
    {
        std::lock_guard<std::mutex> lk(mu);
        ++stats.control_received;
    }

    // Connection-level bookkeeping first, so that a callback which reacts to the message sees the
    // state the message implies. The authoritative session state machine lives in session.h; what
    // is tracked here is only what the transport itself must know (what may be written next) and
    // what the tray shows.
    const proto::ConnState st = get_state();
    switch (m.type()) {
        case proto::MessageType::HelloAck: {
            if (st != proto::ConnState::Handshake) {
                fail(proto::ErrorCode::state_violation, "hello_ack outside the handshake");
                return;
            }
            const auto* ack = m.get_if<proto::HelloAckMsg>();
            if (ack == nullptr || ack->proto != proto::kProtoVersion) {
                // §5: version selection is the Client's. A Server that answers with a different
                // number has changed the contract unilaterally, and proceeding "best effort" is
                // exactly what the handshake exists to prevent.
                proto::ProtocolError e;
                e.code = proto::ErrorCode::proto_unsupported;
                e.fatal = true;
                e.msg = "hello_ack carried a proto version this build did not offer";
                fail_with(e);
                return;
            }
            set_state(proto::ConnState::Loading);
            state_deadline = now_ms() + proto::kReadyTimeoutMs;
            break;
        }
        case proto::MessageType::Loading:
            // §6.6: READY_TIMEOUT_MS restarts on each `loading`, because a cold model load can
            // legitimately take minutes and the heartbeat is how we tell it apart from a hang.
            state_deadline = now_ms() + proto::kReadyTimeoutMs;
            break;
        case proto::MessageType::Ready:
            set_state(proto::ConnState::Idle);
            state_deadline = 0;
            break;
        case proto::MessageType::Stopped:
            if (st == proto::ConnState::Running || st == proto::ConnState::Finalizing ||
                st == proto::ConnState::Cancelling) {
                set_state(proto::ConnState::Idle);
            }
            break;
        default:
            break;
    }

    if (cb.on_message) cb.on_message(m);

    // A fatal wire error ends the connection (§11.1). It is delivered as a message, not through
    // on_error: the peer told us in-band, and reporting it twice would make the tray show two
    // different sentences for one event.
    if (const auto* e = m.get_if<proto::ErrorMsg>()) {
        if (e->fatal) {
            KD_LOG_ERROR("peer reported a fatal error (%s); connection is finished",
                         e->code_raw.empty() ? proto::to_string(e->code) : e->code_raw.c_str());
            io_done = true;
            orderly = true;
            set_state(proto::ConnState::Dead);
        }
    }
}

void IpcClient::Impl::try_issue_write() {
    if (write_pending) return;
    {
        std::lock_guard<std::mutex> lk(mu);
        if (queue.empty()) return;
        write_item = std::move(queue.front());
        queue.pop_front();
        if (write_item.audio) {
            --audio_items;
            audio_samples -= write_item.samples;
        } else {
            --control_depth;
        }
    }
    cv.notify_all();  // flush_audio() may be waiting for the audio queue to empty

    ResetEvent(ev_write.get());
    std::memset(&wov, 0, sizeof(wov));
    wov.hEvent = ev_write.get();
    write_started = now_ms();

    DWORD n = 0;
    // Exactly one write per frame, never split and never coalesced (docs §2.2), so that message
    // boundaries and frame boundaries stay aligned for a conforming peer.
    const BOOL ok = WriteFile(pipe.get(), write_item.bytes.data(),
                              static_cast<DWORD>(write_item.bytes.size()), &n, &wov);
    if (ok) {
        on_write_complete(n);
        return;
    }
    const DWORD err = GetLastError();
    if (err == ERROR_IO_PENDING) {
        write_pending = true;
        return;
    }
    if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_NO_DATA) {
        reader.notify_eof();
        drain_frames();
        io_done = true;
        return;
    }
    fail(proto::ErrorCode::internal_error, "WriteFile on the pipe failed");
}

void IpcClient::Impl::on_write_complete(DWORD written) {
    write_pending = false;
    ResetEvent(ev_write.get());
    const Millis latency = now_ms() - write_started;

    std::lock_guard<std::mutex> lk(mu);
    if (write_item.audio) {
        ++stats.audio_frames_sent;
        stats.audio_bytes_sent += written;
    } else {
        ++stats.control_sent;
    }
    if (latency > stats.max_write_latency_ms) stats.max_write_latency_ms = latency;
}

DWORD IpcClient::Impl::next_timeout() const {
    Millis deadline = 0;
    if (write_pending) deadline = write_started + proto::kWriteDeadlineMs;
    if (state_deadline != 0 && (deadline == 0 || state_deadline < deadline)) {
        deadline = state_deadline;
    }
    if (deadline == 0) return INFINITE;
    const Millis t = now_ms();
    if (deadline <= t) return 0;
    return static_cast<DWORD>(deadline - t);
}

void IpcClient::Impl::check_deadlines() {
    const Millis t = now_ms();

    if (write_pending && t - write_started >= proto::kWriteDeadlineMs) {
        // §9: "A write that has not completed within WRITE_DEADLINE_MS is treated as overload of
        // the corresponding queue." The peer has stopped reading; the only honest answers are to
        // keep up or to fail loudly, and we cannot keep up.
        overloaded.store(true);
        CancelIoEx(pipe.get(), &wov);
        DWORD n = 0;
        GetOverlappedResult(pipe.get(), &wov, &n, TRUE);
        write_pending = false;
        fail(write_item.audio ? proto::ErrorCode::overload_audio
                              : proto::ErrorCode::overload_control,
             "a single write exceeded WRITE_DEADLINE_MS; the peer has stopped reading");
        return;
    }

    if (state_deadline != 0 && t >= state_deadline) {
        switch (get_state()) {
            case proto::ConnState::Handshake:
                fail(proto::ErrorCode::handshake_timeout,
                     "no hello_ack within HELLO_TIMEOUT_MS");
                return;
            case proto::ConnState::Loading:
                fail(proto::ErrorCode::model_load_timeout,
                     "no ready within READY_TIMEOUT_MS and no loading heartbeat to restart it");
                return;
            default:
                state_deadline = 0;
                return;
        }
    }
}

void IpcClient::Impl::io_main(bool need_connect, DWORD expect_pid) {
    if (need_connect && !do_connect(expect_pid)) {
        teardown_pipe();
        if (cb.on_disconnected) cb.on_disconnected();
        return;
    }

    // §5: `hello` is the first frame on the connection, before anything else may cross.
    proto::HelloMsg hello;
    hello.proto = proto::kProtoVersion;
    hello.client = "kvit-dictate/0.1";
    enqueue_internal(proto::encode_control_frame(proto::Message::of(hello)));
    set_state(proto::ConnState::Handshake);
    state_deadline = now_ms() + proto::kHelloTimeoutMs;

    while (!io_done) {
        if (!issue_read()) break;
        try_issue_write();
        if (io_done) break;

        HANDLE waits[5] = {ev_read.get(), ev_write.get(), ev_wake.get(), ev_quit.get(),
                           child_process.get()};
        const DWORD count = child_process.valid() ? 5u : 4u;
        const DWORD w = WaitForMultipleObjects(count, waits, FALSE, next_timeout());

        switch (w) {
            case WAIT_OBJECT_0 + 0:
                complete_read();
                break;
            case WAIT_OBJECT_0 + 1:
                if (write_pending) {
                    DWORD n = 0;
                    if (GetOverlappedResult(pipe.get(), &wov, &n, FALSE)) {
                        on_write_complete(n);
                    } else {
                        const DWORD err = GetLastError();
                        write_pending = false;
                        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED ||
                            err == ERROR_NO_DATA || err == ERROR_OPERATION_ABORTED) {
                            reader.notify_eof();
                            drain_frames();
                            io_done = true;
                        } else {
                            fail(proto::ErrorCode::internal_error, "pipe write did not complete");
                        }
                    }
                } else {
                    ResetEvent(ev_write.get());
                }
                break;
            case WAIT_OBJECT_0 + 2:
                ResetEvent(ev_wake.get());  // new queue entry; the loop head issues the write
                if (state_notify.exchange(false) && cb.on_state) cb.on_state(get_state());
                break;
            case WAIT_OBJECT_0 + 3:
                io_done = true;
                orderly = true;  // stop() asked; a flush is legitimate — teardown_pipe() decides
                break;
            case WAIT_OBJECT_0 + 4:
                // §10.3: child process exit is a dead connection. Drain first — the child may have
                // written a final `stopped` or `error` before dying, and that message is the only
                // explanation the user will ever get.
                complete_read();
                if (!io_done) {
                    reader.notify_eof();
                    drain_frames();
                }
                if (!io_done) {
                    fail(proto::ErrorCode::internal_error, "the sidecar process exited");
                }
                io_done = true;
                break;
            case WAIT_TIMEOUT:
                check_deadlines();
                break;
            default:
                fail(proto::ErrorCode::internal_error, "WaitForMultipleObjects failed");
                break;
        }
    }

    if (read_pending) {
        CancelIoEx(pipe.get(), &rov);
        DWORD n = 0;
        GetOverlappedResult(pipe.get(), &rov, &n, TRUE);
        read_pending = false;
    }
    if (write_pending) {
        // A write we had to cancel is a write the peer was not draining. That is the one state in
        // which the §2.5 flush must not be attempted at all — see teardown_pipe().
        orderly = false;
        CancelIoEx(pipe.get(), &wov);
        DWORD n = 0;
        GetOverlappedResult(pipe.get(), &wov, &n, TRUE);
        write_pending = false;
    }

    if (get_state() != proto::ConnState::Dead) set_state(proto::ConnState::Dead);
    teardown_pipe();

    {
        std::lock_guard<std::mutex> lk(mu);
        running = false;
    }
    cv.notify_all();  // unblock any flush_audio() waiter rather than let it burn its timeout

    if (cb.on_disconnected) cb.on_disconnected();
}

void IpcClient::Impl::teardown_pipe() {
    if (!pipe.valid()) return;
    bool queue_left = false;
    {
        std::lock_guard<std::mutex> lk(mu);
        queue_left = !queue.empty();
    }
    if (orderly && !write_pending && !queue_left) {
        // docs §2.5 wants FlushFileBuffers before DisconnectNamedPipe. On a pipe *server* that
        // call does not return until the client has read everything still buffered — so a child
        // that has stopped reading would wedge this thread forever. Three defences, in order:
        // it is skipped on every error path and whenever anything is still queued or a write had
        // to be cancelled (a peer that is not draining is exactly the deadlock case); stop()
        // bounds its wait for this thread; and when that bound expires stop() kills the child and
        // cancels this thread's synchronous I/O outright. A test-only loopback peer has no child
        // to kill, which is why the third defence is not optional.
        FlushFileBuffers(pipe.get());
    }
    DisconnectNamedPipe(pipe.get());
    pipe.close();
}

void IpcClient::Impl::log_main() {
    // The child's stdout and stderr, both redirected here. docs §2.1: the sidecar's runtime,
    // logging and C extensions all write to those streams unpredictably, which is precisely why
    // the protocol does not live on them — but the output is the only diagnostic we get when a
    // model fails to import, so it is captured rather than discarded.
    //
    // Privacy: the sidecar's contract is that it never prints recognised text (plan.md §1.9).
    // If a backend ever does, this is where it would reach the diagnostics log, and that is a
    // defect in the backend, to be fixed there.
    std::string acc;
    char buf[1024];
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(child_out.get(), buf, sizeof(buf), &n, nullptr) || n == 0) break;
        acc.append(buf, n);
        for (;;) {
            const size_t nl = acc.find('\n');
            if (nl == std::string::npos) break;
            std::string line = acc.substr(0, nl);
            acc.erase(0, nl + 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\0')) line.pop_back();
            if (line.size() > 500) line.resize(500);
            if (!line.empty()) KD_LOG_WARN("child: %s", line.c_str());
        }
        // A child that writes a megabyte without a newline must not grow this buffer without
        // bound; flush what we have and carry on.
        if (acc.size() > 4096) {
            std::string line = acc.substr(0, 500);
            acc.clear();
            KD_LOG_WARN("child: %s...", line.c_str());
        }
    }
    if (!acc.empty()) {
        if (acc.size() > 500) acc.resize(500);
        KD_LOG_WARN("child: %s", acc.c_str());
    }
}

// ─── IpcClient ───────────────────────────────────────────────────────────────────────────────

IpcClient::IpcClient() : impl_(new Impl) {}

IpcClient::~IpcClient() {
    stop();
    delete impl_;
}

Status IpcClient::start(const ChildSpec& child, Callbacks cb) {
    Impl& im = *impl_;
    {
        std::lock_guard<std::mutex> lk(im.mu);
        if (im.running) {
            return Status{StatusCode::Internal, "IpcClient::start() while already running"};
        }
    }
    if (im.io_thread.joinable()) stop();  // a previous connection that has ended but not been reaped

    // §10.3: recovery is a new child and a new pipe. Nothing from the previous connection is
    // reused — not the handle, not the name, not the frame reader's buffer.
    im.reader.reset();
    im.io_done = false;
    im.reported = false;
    im.orderly = false;
    im.state_deadline = 0;
    im.overloaded.store(false);
    im.cb = std::move(cb);
    im.model_id = child.model_id;
    im.pid.store(0);
    {
        std::lock_guard<std::mutex> lk(im.mu);
        im.queue.clear();
        im.control_depth = 0;
        im.audio_items = 0;
        im.audio_samples = 0;
        im.stats = Stats{};
    }

    Result<std::wstring> name = generate_pipe_name();
    if (!name) return name.status();
    im.pipe_name = *name;

    // The descriptor must outlive CreateNamedPipeW only — the kernel object keeps its own copy of
    // the security information, so nothing here needs to hold the ACL after the pipe exists.
    SecurityDescriptorBuilder sec;
    if (Status st = sec.build(); !st) return st;
    if (!sec.used_logon_sid()) {
        KD_LOG_WARN("pipe DACL is using the user SID, not the logon SID (see the warning above)");
    }

    // §2.4 step 1: the pipe exists before the child is spawned, so the child never races an
    // uncreated pipe and never has to retry.
    Result<uintptr_t> h = create_pipe_instance(im.pipe_name, sec);
    if (!h) return h.status();
    im.pipe.reset(reinterpret_cast<HANDLE>(*h));

    if (!im.create_events()) {
        im.pipe.close();
        return os_failure(StatusCode::OsError, "CreateEventW", GetLastError());
    }

    // Job object first, so that the child can be assigned before it runs a single instruction.
    im.job.reset(CreateJobObjectW(nullptr, nullptr));
    if (!im.job.valid()) {
        im.pipe.close();
        return os_failure(StatusCode::OsError, "CreateJobObjectW", GetLastError());
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    // plan.md Phase 3: a crashed or killed parent must never orphan a Python process holding
    // VRAM. KILL_ON_JOB_CLOSE gives that for free even when we are terminated without running
    // any cleanup at all, which no amount of careful shutdown code can.
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(im.job.get(), JobObjectExtendedLimitInformation, &limits,
                                 sizeof(limits))) {
        const DWORD err = GetLastError();
        im.pipe.close();
        im.job.close();
        return os_failure(StatusCode::OsError, "SetInformationJobObject", err);
    }

    // ── the child's stdout/stderr, and a null stdin ──
    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;

    Handle out_read, out_write, null_in;
    {
        HANDLE r = nullptr, w = nullptr;
        if (!CreatePipe(&r, &w, &inherit, 0)) {
            const DWORD err = GetLastError();
            im.pipe.close();
            im.job.close();
            return os_failure(StatusCode::OsError, "CreatePipe (child stderr)", err);
        }
        out_read.reset(r);
        out_write.reset(w);
        // Our end must not be inheritable, or the child holds a copy and we never see EOF.
        SetHandleInformation(out_read.get(), HANDLE_FLAG_INHERIT, 0);
    }
    null_in.reset(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                              OPEN_EXISTING, 0, nullptr));

    std::wstring cmdline;
    append_quoted_arg(cmdline, child.interpreter);
    for (const std::wstring& a : child.args) append_quoted_arg(cmdline, a);
    // §2.4 step 2. Appended here, by us, so that no caller can forget them or spell them
    // differently — the pipe name in particular is the only way the child can find us.
    append_quoted_arg(cmdline, L"--pipe");
    append_quoted_arg(cmdline, im.pipe_name);
    append_quoted_arg(cmdline, L"--proto");
    append_quoted_arg(cmdline, L"1");
    // §2.4 step 5's mirror check. GetNamedPipeServerProcessId is confirmed present in Windows SDK
    // 10.0.26100 (winbase.h, desktop partition), so the child-side check is implementable and the
    // argument it needs is passed. See docs/protocol.md §14 item 1.
    append_quoted_arg(cmdline, L"--parent-pid");
    append_quoted_arg(cmdline, std::to_wstring(GetCurrentProcessId()));

    std::wstring env = build_environment(child.environment);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = null_in.get();
    si.StartupInfo.hStdOutput = out_write.get();
    si.StartupInfo.hStdError = out_write.get();

    // An explicit handle list rather than bare bInheritHandles=TRUE: without it the child would
    // inherit *every* inheritable handle in this process, including ones another thread happens
    // to be creating at this instant. That is a real race, not a theoretical one, and the leaked
    // handle would keep a pipe or a file alive for the child's whole lifetime.
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<uint8_t> attr_buf(attr_size);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    HANDLE inherited[2] = {out_write.get(), null_in.get()};
    const DWORD inherited_count = null_in.valid() ? 2u : 1u;
    bool attrs_ok =
        InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size) &&
        UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  inherited, inherited_count * sizeof(HANDLE), nullptr, nullptr);
    if (!attrs_ok) {
        const DWORD err = GetLastError();
        im.pipe.close();
        im.job.close();
        return os_failure(StatusCode::OsError, "InitializeProcThreadAttributeList", err);
    }

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutable_cmdline(cmdline.begin(), cmdline.end());
    mutable_cmdline.push_back(L'\0');  // CreateProcessW may write into lpCommandLine

    const BOOL created = CreateProcessW(
        child.interpreter.empty() ? nullptr : child.interpreter.c_str(), mutable_cmdline.data(),
        nullptr, nullptr, TRUE,
        // CREATE_SUSPENDED so the process is in the job before it executes anything; without it a
        // fast child could fork a grandchild that escapes the job's kill-on-close.
        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
            EXTENDED_STARTUPINFO_PRESENT,
        env.data(), child.working_directory.empty() ? nullptr : child.working_directory.c_str(),
        &si.StartupInfo, &pi);
    const DWORD create_err = GetLastError();
    DeleteProcThreadAttributeList(si.lpAttributeList);

    if (!created) {
        im.pipe.close();
        im.job.close();
        return os_failure(StatusCode::NotFound, "CreateProcessW (sidecar interpreter)", create_err);
    }

    Handle proc(pi.hProcess);
    Handle thread(pi.hThread);
    if (!AssignProcessToJobObject(im.job.get(), proc.get())) {
        const DWORD err = GetLastError();
        TerminateProcess(proc.get(), 1);
        im.pipe.close();
        im.job.close();
        return os_failure(StatusCode::OsError, "AssignProcessToJobObject", err);
    }
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        const DWORD err = GetLastError();
        TerminateProcess(proc.get(), 1);
        im.pipe.close();
        im.job.close();
        return os_failure(StatusCode::OsError, "ResumeThread", err);
    }

    im.child_process = std::move(proc);
    im.child_out = std::move(out_read);
    im.pid.store(pi.dwProcessId);
    // Drop our copy of the write end, or the reader below never sees end of stream.
    out_write.close();
    null_in.close();

    KD_LOG_INFO("spawned sidecar pid %lu for model '%s'", static_cast<unsigned long>(pi.dwProcessId),
                im.model_id.c_str());

    im.set_state(proto::ConnState::Spawning);
    {
        std::lock_guard<std::mutex> lk(im.mu);
        im.running = true;
    }
    im.log_thread = std::thread([&im] { im.log_main(); });
    im.io_thread = std::thread([&im, dwpid = pi.dwProcessId] { im.io_main(true, dwpid); });
    im.io_native = im.io_thread.native_handle();
    return Status::ok();
}

Status IpcClient::attach_connected(uintptr_t connected_pipe_handle, Callbacks cb) {
    Impl& im = *impl_;
    {
        std::lock_guard<std::mutex> lk(im.mu);
        if (im.running) {
            return Status{StatusCode::Internal, "IpcClient::attach_connected() while running"};
        }
    }
    if (im.io_thread.joinable()) stop();
    if (connected_pipe_handle == 0) {
        return Status{StatusCode::InvalidArgument, "attach_connected() given a null handle"};
    }

    im.reader.reset();
    im.io_done = false;
    im.reported = false;
    im.orderly = false;
    im.state_deadline = 0;
    im.overloaded.store(false);
    im.cb = std::move(cb);
    im.pipe.reset(reinterpret_cast<HANDLE>(connected_pipe_handle));
    {
        std::lock_guard<std::mutex> lk(im.mu);
        im.queue.clear();
        im.control_depth = 0;
        im.audio_items = 0;
        im.audio_samples = 0;
        im.stats = Stats{};
        im.running = true;
    }
    if (!im.create_events()) {
        std::lock_guard<std::mutex> lk(im.mu);
        im.running = false;
        return os_failure(StatusCode::OsError, "CreateEventW", GetLastError());
    }
    im.set_state(proto::ConnState::Validating);
    im.io_thread = std::thread([&im] { im.io_main(false, 0); });
    im.io_native = im.io_thread.native_handle();
    return Status::ok();
}

void IpcClient::stop() {
    Impl& im = *impl_;
    if (!im.io_thread.joinable() && !im.log_thread.joinable() && !im.child_process.valid() &&
        !im.job.valid() && !im.pipe.valid()) {
        return;  // never started, or already stopped: idempotent, like `stop`/`cancel` on the wire
    }

    if (im.ev_quit.valid()) SetEvent(im.ev_quit.get());

    if (im.io_thread.joinable()) {
        // A *timed* wait, then join. The I/O thread's last act may be FlushFileBuffers, which
        // blocks until the peer has read everything; if the child has stopped reading, the only
        // way out is to kill it. Closing the job does that, the pipe breaks, the flush returns,
        // and the join completes. Without this bound, quitting the app with a wedged sidecar
        // would hang forever — the exact failure this module is required not to have.
        if (im.io_native != nullptr &&
            WaitForSingleObject(im.io_native, kFlushGraceMs) == WAIT_TIMEOUT) {
            KD_LOG_WARN("sidecar did not drain within %lu ms; closing the job to unwedge teardown",
                        static_cast<unsigned long>(kFlushGraceMs));
            im.job.close();  // KILL_ON_JOB_CLOSE
        }
        im.io_thread.join();
        im.io_native = nullptr;
    }

    im.pipe.close();

    if (im.child_process.valid()) {
        const DWORD w = WaitForSingleObject(im.child_process.get(), kChildExitGraceMs);
        if (w != WAIT_OBJECT_0) {
            KD_LOG_WARN("sidecar pid %lu still alive after teardown; the job object will end it",
                        static_cast<unsigned long>(im.pid.load()));
        }
    }
    // Closing the job kills whatever is left of the child (and any grandchild it forked), which
    // is what keeps a model out of VRAM after we are gone.
    im.job.close();

    if (im.log_thread.joinable()) {
        // Safe only after the child is gone: the reader blocks in ReadFile until the write end
        // closes, and the write end lives in the child.
        im.log_thread.join();
    }
    im.child_out.close();
    im.child_process.close();

    {
        std::lock_guard<std::mutex> lk(im.mu);
        im.running = false;
    }
    im.cv.notify_all();
    im.set_state(proto::ConnState::Dead);
    im.pid.store(0);
}

proto::ConnState IpcClient::state() const noexcept { return impl_->get_state(); }

bool IpcClient::is_connected() const noexcept {
    const proto::ConnState s = impl_->get_state();
    return s == proto::ConnState::Handshake || s == proto::ConnState::Loading ||
           s == proto::ConnState::Idle || s == proto::ConnState::Running ||
           s == proto::ConnState::Finalizing || s == proto::ConnState::Cancelling;
}

Status IpcClient::send_control(const proto::Message& m) {
    Impl& im = *impl_;
    std::vector<uint8_t> bytes = proto::encode_control_frame(m);
    if (bytes.empty()) {
        return Status{StatusCode::Internal, "encode_control_frame produced no bytes"};
    }
    {
        std::lock_guard<std::mutex> lk(im.mu);
        if (!im.running) {
            return Status{StatusCode::Unavailable, "the sidecar connection is not running"};
        }
        if (im.control_depth >= proto::kControlQueueMax) {
            // §9: control backlog is fatal rather than absorbed. Nothing is dropped here; the
            // caller is told, and the caller's answer is to end the connection.
            return Status{StatusCode::Overloaded,
                          "control queue is at CONTROL_QUEUE_MAX (256); connection is overloaded"};
        }
        im.queue.push_back(QueuedFrame{std::move(bytes), false, 0});
        ++im.control_depth;
    }

    // Mirror the session-affecting transitions so state() means something to the tray. The
    // authority is session.h; this is a shadow, and it never gates a send.
    switch (m.type()) {
        case proto::MessageType::Start:
            im.set_state_async(proto::ConnState::Running);
            break;
        case proto::MessageType::Stop:
            im.set_state_async(proto::ConnState::Finalizing);
            break;
        case proto::MessageType::Cancel:
            im.set_state_async(proto::ConnState::Cancelling);
            break;
        default:
            break;
    }

    SetEvent(im.ev_wake.get());
    return Status::ok();
}

Status IpcClient::send_audio(const int16_t* samples, size_t count) {
    Impl& im = *impl_;
    if (samples == nullptr && count != 0) {
        return Status{StatusCode::InvalidArgument, "send_audio(nullptr, count>0)"};
    }

    // §10.2: audio may not be written before `ready`. Refusing here keeps a local ordering bug
    // from becoming a `state_violation` that tears down the connection at the far end.
    const proto::ConnState st = im.get_state();
    if (st != proto::ConnState::Idle && st != proto::ConnState::Running &&
        st != proto::ConnState::Finalizing && st != proto::ConnState::Cancelling) {
        return Status{StatusCode::Unavailable, "audio before the sidecar reported ready"};
    }

    if (im.overloaded.load()) {
        // Sticky until the next start()/attach_connected(). §9 makes overload_audio fatal:true —
        // the connection is finished and recovery is a new child — so quietly accepting audio
        // again after the queue happened to drain would resume a session the caller was already
        // told to end.
        return Status{StatusCode::Overloaded,
                      "connection already overloaded; it must be torn down"};
    }

    std::vector<uint8_t> bytes;
    if (!proto::encode_audio_frame(samples, count, bytes)) {
        return Status{StatusCode::InvalidArgument,
                      "audio payload exceeds MAX_PAYLOAD; split it before sending"};
    }
    {
        std::lock_guard<std::mutex> lk(im.mu);
        if (!im.running) {
            return Status{StatusCode::Unavailable, "the sidecar connection is not running"};
        }
        if (im.audio_samples + count > kMaxQueuedSamples) {
            // **Nothing is dropped and nothing is delayed.** §9 and plan.md §1.2: silently
            // dropping or delaying audio produces mysteriously bad transcription. The queue is
            // left exactly as it was and the caller must end the session with `overload_audio`.
            im.overloaded.store(true);
            return Status{StatusCode::Overloaded,
                          "audio queue would exceed AUDIO_QUEUE_MAX_MS (3000 ms); "
                          "end the session with overload_audio — do not retry and do not drop"};
        }
        im.queue.push_back(QueuedFrame{std::move(bytes), true, count});
        ++im.audio_items;
        im.audio_samples += count;
    }
    SetEvent(im.ev_wake.get());
    return Status::ok();
}

Status IpcClient::flush_audio(uint32_t timeout_ms) {
    Impl& im = *impl_;
    std::unique_lock<std::mutex> lk(im.mu);
    if (!im.running) return Status::ok();  // nothing can be in flight on a dead connection
    const bool drained = im.cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                        [&im] { return im.audio_items == 0 || !im.running; });
    if (!drained) {
        return Status{StatusCode::Timeout, "queued audio did not drain before the timeout"};
    }
    return Status::ok();
}

void IpcClient::discard_queued_audio() {
    Impl& im = *impl_;
    size_t dropped = 0;
    {
        std::lock_guard<std::mutex> lk(im.mu);
        // The one place audio is legitimately discarded, and it is not backpressure: `cancel`
        // means the audio is about to become meaningless, so writing it would only delay the
        // teardown. Control frames keep their order relative to each other.
        for (auto it = im.queue.begin(); it != im.queue.end();) {
            if (it->audio) {
                ++dropped;
                it = im.queue.erase(it);
            } else {
                ++it;
            }
        }
        im.audio_items = 0;
        im.audio_samples = 0;
    }
    im.cv.notify_all();
    if (dropped != 0) KD_LOG_DEBUG("discarded %zu queued audio frames for a cancel", dropped);
}

IpcClient::Stats IpcClient::stats() const noexcept {
    Impl& im = *impl_;
    std::lock_guard<std::mutex> lk(im.mu);
    Stats s = im.stats;
    s.control_queue_depth = im.control_depth;
    s.audio_queue_ms =
        static_cast<uint32_t>(im.audio_samples * 1000 / proto::kSampleRate);
    return s;
}

std::wstring IpcClient::pipe_name() const { return impl_->pipe_name; }

uint32_t IpcClient::child_pid() const noexcept { return impl_->pid.load(); }

}  // namespace kd
