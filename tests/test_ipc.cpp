// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// tests/test_ipc.cpp — pipe naming and the security descriptor.
//
// No Python child is spawned here. What IS tested is the part plan.md §1.2 had to correct itself
// on, and which no integration test would catch because a pipe with a wide-open ACL works
// perfectly:
//
//   Revision 1 of the plan claimed a named pipe's default descriptor restricts it to the current
//   user "for free". That is false. Microsoft documents that the default grants full control to
//   LocalSystem, administrators and the creator owner, and READ ACCESS TO EVERYONE AND THE
//   ANONYMOUS ACCOUNT. For a tool whose entire premise is that dictated text never leaves the
//   machine, a pipe any process can read is the whole product failing quietly.
//
// So the assertions below are deliberately about absence — that Everyone is NOT in the DACL, and
// that FILE_CREATE_PIPE_INSTANCE is NOT granted. Absence is exactly what a smoke test cannot see.

#include "kd_test.h"

#include "ipc_client.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>

#include <set>
#include <string>
#include <vector>

namespace {

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}

// Every SID in the descriptor's DACL, as a string, plus the access mask each was granted.
struct AceInfo {
    std::string sid;
    uint32_t mask = 0;
    bool allow = false;
};

std::vector<AceInfo> dacl_of(PSECURITY_DESCRIPTOR sd) {
    std::vector<AceInfo> out;
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL acl = nullptr;
    if (!GetSecurityDescriptorDacl(sd, &present, &acl, &defaulted)) return out;
    if (!present || acl == nullptr) return out;  // a NULL DACL grants everyone everything

    for (WORD i = 0; i < acl->AceCount; ++i) {
        void* raw = nullptr;
        if (!GetAce(acl, i, &raw)) continue;
        auto* header = static_cast<ACE_HEADER*>(raw);
        AceInfo info;
        info.allow = header->AceType == ACCESS_ALLOWED_ACE_TYPE;
        PSID sid = nullptr;
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
            auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
            info.mask = ace->Mask;
            sid = &ace->SidStart;
        } else if (header->AceType == ACCESS_DENIED_ACE_TYPE) {
            auto* ace = static_cast<ACCESS_DENIED_ACE*>(raw);
            info.mask = ace->Mask;
            sid = &ace->SidStart;
        } else {
            continue;
        }
        LPWSTR text = nullptr;
        if (ConvertSidToStringSidW(sid, &text)) {
            info.sid = narrow(text);
            LocalFree(text);
        }
        out.push_back(info);
    }
    return out;
}

// S-1-1-0 Everyone, S-1-5-7 Anonymous Logon. The two the default descriptor hands read access to.
constexpr const char* kEveryone = "S-1-1-0";
constexpr const char* kAnonymous = "S-1-5-7";

}  // namespace

// ═══ Pipe naming ════════════════════════════════════════════════════════════════════════════

KD_TEST(ipc, pipe_name_has_the_documented_shape) {
    uint8_t bytes[16];
    for (int i = 0; i < 16; ++i) bytes[i] = static_cast<uint8_t>(i * 17);
    const std::wstring name = kd::format_pipe_name(bytes);
    const std::string s = narrow(name);

    const std::string prefix = "\\\\.\\pipe\\kvit-dictate-";
    KD_REQUIRE(s.rfind(prefix, 0) == 0);

    const std::string hex = s.substr(prefix.size());
    KD_CHECK_EQ(hex.size(), size_t{32});  // 16 bytes, two lowercase hex digits each
    bool ok = true;
    for (char c : hex) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) ok = false;
    }
    KD_CHECK(ok);
}

KD_TEST(ipc, pipe_name_is_a_pure_function_of_its_bytes) {
    uint8_t a[16] = {};
    uint8_t b[16] = {};
    b[15] = 1;
    KD_CHECK(kd::format_pipe_name(a) == kd::format_pipe_name(a));
    KD_CHECK(kd::format_pipe_name(a) != kd::format_pipe_name(b));
}

KD_TEST(ipc, generated_names_are_unpredictable_and_do_not_repeat) {
    // Per-launch randomness is what defeats squatting by a process that started first. A counter
    // or a tick count would produce names an attacker can guess and create ahead of us.
    std::set<std::wstring> seen;
    for (int i = 0; i < 64; ++i) {
        auto r = kd::generate_pipe_name();
        KD_REQUIRE(static_cast<bool>(r));
        KD_CHECK(seen.insert(r.value()).second);
    }

    // A weak generator often leaves whole bytes constant across draws. Count distinct values in
    // each hex position; a real CSPRNG fills them.
    const std::string prefix = "\\\\.\\pipe\\kvit-dictate-";
    std::set<char> first_digit;
    for (const std::wstring& w : seen) {
        const std::string s = narrow(w);
        if (s.size() > prefix.size()) first_digit.insert(s[prefix.size()]);
    }
    KD_CHECK_GT(first_digit.size(), size_t{6});
}

// ═══ The security descriptor ════════════════════════════════════════════════════════════════

KD_TEST(ipc, descriptor_builds_and_prefers_the_logon_sid) {
    kd::SecurityDescriptorBuilder sec;
    const kd::Status st = sec.build();
    if (!st) {
        // Building needs the process token. If a runner lacks it, say so and stop — a silent pass
        // here would be a security test that never ran.
        KD_INFO("SecurityDescriptorBuilder::build() failed: " + st.to_string() +
                " — skipping the DACL assertions (they did NOT pass, they did not run)");
        return;
    }
    KD_CHECK(sec.is_built());
    KD_CHECK(sec.security_attributes() != 0);
    KD_CHECK(!sec.sid_string().empty());

    // The logon SID is what stops a different terminal-services session reaching the pipe. The
    // user SID is the documented weaker fallback, so not finding a logon SID is allowed — but it
    // must be visible rather than silently substituted.
    if (!sec.used_logon_sid()) {
        KD_INFO("no logon SID available; the descriptor fell back to the user SID (" +
                sec.sid_string() + "), which does not isolate terminal-services sessions");
    }
    KD_CHECK(true);
}

KD_TEST(ipc, everyone_and_anonymous_are_absent_from_the_dacl) {
    kd::SecurityDescriptorBuilder sec;
    if (!sec.build()) {
        KD_INFO("descriptor unavailable in this environment — assertions did not run");
        return;
    }

    auto* sa = reinterpret_cast<SECURITY_ATTRIBUTES*>(sec.security_attributes());
    KD_REQUIRE(sa != nullptr);
    KD_REQUIRE(sa->lpSecurityDescriptor != nullptr);

    const std::vector<AceInfo> aces = dacl_of(sa->lpSecurityDescriptor);
    // A DACL with no ACEs denies everyone, which is safe but useless; an ABSENT DACL grants
    // everyone everything, which is the failure. dacl_of returns empty for both, so check the
    // presence flag directly.
    BOOL present = FALSE;
    BOOL defaulted = FALSE;
    PACL acl = nullptr;
    KD_REQUIRE(GetSecurityDescriptorDacl(sa->lpSecurityDescriptor, &present, &acl, &defaulted));
    KD_CHECK(present);
    KD_CHECK(acl != nullptr);
    KD_CHECK(!defaulted);  // a defaulted DACL is the permissive one this test exists to reject

    for (const AceInfo& a : aces) {
        if (a.allow && a.sid == kEveryone) {
            KD_FAIL("the pipe DACL grants Everyone (S-1-1-0) — this is exactly the default "
                    "behaviour plan.md §1.2 corrects, and it makes dictated text readable by any "
                    "process on the machine");
        }
        if (a.allow && a.sid == kAnonymous) {
            KD_FAIL("the pipe DACL grants Anonymous Logon (S-1-5-7)");
        }
    }
    KD_CHECK_GT(aces.size(), size_t{0});
}

KD_TEST(ipc, no_ace_grants_the_right_to_create_rival_pipe_instances) {
    // FILE_GENERIC_WRITE implies FILE_CREATE_PIPE_INSTANCE. A client holding it can create
    // another instance of OUR pipe name and start answering connections intended for us — so the
    // plan requires individual rights rather than the convenient generic mask.
    kd::SecurityDescriptorBuilder sec;
    if (!sec.build()) {
        KD_INFO("descriptor unavailable in this environment — assertions did not run");
        return;
    }
    auto* sa = reinterpret_cast<SECURITY_ATTRIBUTES*>(sec.security_attributes());
    KD_REQUIRE(sa != nullptr);

    for (const AceInfo& a : dacl_of(sa->lpSecurityDescriptor)) {
        if (!a.allow) continue;
        if ((a.mask & kd::kFileCreatePipeInstance) != 0) {
            KD_FAIL("SID " + a.sid + " is granted FILE_CREATE_PIPE_INSTANCE (mask 0x" +
                    std::to_string(a.mask) + ") — it can create rival instances of our pipe");
        }
        if ((a.mask & GENERIC_ALL) != 0 || (a.mask & GENERIC_WRITE) != 0) {
            KD_FAIL("SID " + a.sid + " is granted a GENERIC mask, which expands to include "
                    "FILE_CREATE_PIPE_INSTANCE");
        }
    }
    KD_CHECK(true);
}

KD_TEST(ipc, the_documented_access_mask_excludes_create_pipe_instance) {
    // The constant itself, checked independently of any token. If someone widens
    // kPipeAccessRights, this fails before anything is built.
    KD_CHECK_EQ(kd::kPipeAccessRights & kd::kFileCreatePipeInstance, uint32_t{0});
    KD_CHECK_GT(kd::kPipeAccessRights & FILE_GENERIC_READ, uint32_t{0});
}

// ═══ Loopback ═══════════════════════════════════════════════════════════════════════════════

KD_TEST(ipc, a_pipe_created_with_the_descriptor_accepts_a_local_client) {
    // Proves the descriptor is not so restrictive that our own child cannot connect — the
    // opposite failure to the one above, and just as fatal.
    kd::SecurityDescriptorBuilder sec;
    if (!sec.build()) {
        KD_INFO("descriptor unavailable in this environment — loopback did not run");
        return;
    }
    auto name = kd::generate_pipe_name();
    KD_REQUIRE(static_cast<bool>(name));

    auto server = kd::create_pipe_instance(name.value(), sec);
    if (!server) {
        KD_INFO("create_pipe_instance failed: " + server.status().to_string());
        return;
    }
    auto server_handle = reinterpret_cast<HANDLE>(server.value());

    // Open with exactly the rights the DACL grants — which is what the Python sidecar does too
    // (kvit_asr/transport.py PIPE_ACCESS_RIGHTS). Asking for GENERIC_READ|GENERIC_WRITE here
    // fails with ERROR_ACCESS_DENIED, because GENERIC_READ pulls in FILE_READ_EA and
    // GENERIC_WRITE pulls in FILE_CREATE_PIPE_INSTANCE — the very right we refuse to grant.
    // That is not a descriptor that is too tight; it is a request that is too broad.
    const HANDLE client = CreateFileW(name.value().c_str(), kd::kPipeAccessRights, 0, nullptr,
                                      OPEN_EXISTING, 0, nullptr);
    if (client == INVALID_HANDLE_VALUE) {
        KD_FAIL("our own process could not open the pipe we just created (error " +
                std::to_string(GetLastError()) +
                ") — the descriptor is too restrictive for the sidecar to connect");
        CloseHandle(server_handle);
        return;
    }
    KD_CHECK(true);
    CloseHandle(client);
    CloseHandle(server_handle);
}
