// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// src/updater.cpp: the parts that decide what gets installed. The network and the dialogs are
// not here; what is here is every rule that could offer the wrong release, or run the wrong file.

#include "updater.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "kd_test.h"

namespace {

using kd::update::compare;
using kd::update::parse_version;
using kd::update::Version;

Version v(const char* text) {
    Version out;
    KD_REQUIRE(parse_version(text, out));
    return out;
}

// One release in GitHub's shape, with only the fields pick_update reads.
std::string release(const char* tag, bool prerelease, bool draft = false, bool installer = true,
                    bool sums = true) {
    std::string base(tag);
    if (!base.empty() && base[0] == 'v') base.erase(0, 1);
    if (const size_t dash = base.find('-'); dash != std::string::npos) base.resize(dash);
    std::string assets;
    auto add = [&](const std::string& name, int size) {
        if (!assets.empty()) assets += ",";
        assets += R"({"name":")" + name +
                  R"(","browser_download_url":"https://github.com/kvit-s/kvit-dictate/releases/download/)" +
                  tag + "/" + name + R"(","size":)" + std::to_string(size) + "}";
    };
    if (installer) add("kvit-dictate-" + base + "-setup.exe", 65030909);
    add("kvit-dictate-" + base + "-win-x64.zip", 96584390);
    if (sums) add("SHA256SUMS.txt", 192);
    return std::string(R"({"tag_name":")") + tag + R"(","draft":)" + (draft ? "true" : "false") +
           R"(,"prerelease":)" + (prerelease ? "true" : "false") +
           R"(,"html_url":"https://github.com/kvit-s/kvit-dictate/releases/tag/)" + tag +
           R"(","assets":[)" + assets + "]}";
}

std::string list(std::initializer_list<std::string> items) {
    std::string out = "[";
    for (const std::string& i : items) {
        if (out.size() > 1) out += ",";
        out += i;
    }
    return out + "]";
}

}  // namespace

KD_TEST(updater, parses_tags_and_versions) {
    Version x;
    KD_REQUIRE(parse_version("v1.2.3", x));
    KD_CHECK_EQ(x.major, 1u);
    KD_CHECK_EQ(x.minor, 2u);
    KD_CHECK_EQ(x.patch, 3u);
    KD_CHECK(x.pre.empty());

    KD_REQUIRE(parse_version("1.0.0-rc2", x));
    KD_CHECK_EQ(x.pre, std::string("rc2"));
    KD_CHECK(x.is_prerelease());
    KD_CHECK_EQ(kd::update::to_string(x), std::string("1.0.0-rc2"));

    KD_REQUIRE(parse_version("1.0.0+build.7", x));  // metadata is ignored
    KD_CHECK(x.pre.empty());

    for (const char* bad : {"", "v", "1.0", "1.0.0.0", "01.0.0", "1.0.0-", "1.0.0-rc..1",
                            "1.0.0-r c", "latest", "1.x.0"}) {
        KD_CHECK(!parse_version(bad, x));
    }
}

KD_TEST(updater, orders_versions_like_a_person_would) {
    KD_CHECK_LT(compare(v("1.0.0"), v("1.0.1")), 0);
    KD_CHECK_LT(compare(v("1.0.9"), v("1.1.0")), 0);
    KD_CHECK_LT(compare(v("1.9.0"), v("2.0.0")), 0);
    KD_CHECK_GT(compare(v("1.0.10"), v("1.0.9")), 0);
    KD_CHECK_EQ(compare(v("v1.0.0"), v("1.0.0")), 0);

    // A release candidate is older than its release, and the next one is newer than it.
    KD_CHECK_LT(compare(v("1.0.0-rc1"), v("1.0.0")), 0);
    KD_CHECK_LT(compare(v("1.0.0-rc1"), v("1.0.0-rc2")), 0);
    // Where SemVer's string order gets it backwards.
    KD_CHECK_LT(compare(v("1.0.0-rc9"), v("1.0.0-rc10")), 0);
    KD_CHECK_LT(compare(v("1.0.0-beta"), v("1.0.0-rc1")), 0);
    KD_CHECK_LT(compare(v("1.0.0-rc.1"), v("1.0.0-rc.1.1")), 0);
    KD_CHECK_LT(compare(v("1.0.0-1"), v("1.0.0-alpha")), 0);  // numeric before alphanumeric
    KD_CHECK_LT(compare(v("1.0.0"), v("1.0.1-rc1")), 0);
}

KD_TEST(updater, the_running_version_parses) {
    // Whatever CMake stamped, the updater has to be able to compare with it.
    Version x;
    KD_CHECK(parse_version(kd::update::running_version(), x));
}

KD_TEST(updater, picks_the_newest_installable_release) {
    const std::string json = list({release("v1.0.2", false), release("v1.1.0", false),
                                   release("v1.0.1", false), release("v1.0.0-rc1", true)});
    auto r = kd::update::pick_update(json, v("1.0.0"), false);
    KD_REQUIRE(r.is_ok());
    KD_REQUIRE(r->has_value());
    const kd::update::Release& got = **r;
    KD_CHECK_EQ(got.tag, std::string("v1.1.0"));
    KD_CHECK_EQ(got.installer_name, std::string("kvit-dictate-1.1.0-setup.exe"));
    KD_CHECK_EQ(got.installer_url,
                std::string("https://github.com/kvit-s/kvit-dictate/releases/download/v1.1.0/"
                            "kvit-dictate-1.1.0-setup.exe"));
    KD_CHECK_EQ(got.installer_size, 65030909ull);
    KD_CHECK(!got.sums_url.empty());
    KD_CHECK_EQ(got.page_url,
                std::string("https://github.com/kvit-s/kvit-dictate/releases/tag/v1.1.0"));
}

KD_TEST(updater, nothing_newer_is_no_value_not_an_error) {
    const std::string json = list({release("v1.0.0", false), release("v0.9.0", false)});
    auto r = kd::update::pick_update(json, v("1.0.0"), false);
    KD_REQUIRE(r.is_ok());
    KD_CHECK(!r->has_value());

    auto empty = kd::update::pick_update("[]", v("1.0.0"), false);
    KD_REQUIRE(empty.is_ok());
    KD_CHECK(!empty->has_value());
}

KD_TEST(updater, prereleases_only_for_a_prerelease_build) {
    const std::string json = list({release("v1.0.0-rc3", true), release("v1.0.0-rc2", true)});
    // A release build hears nothing about candidates...
    auto stable = kd::update::pick_update(json, v("0.9.0"), false);
    KD_REQUIRE(stable.is_ok());
    KD_CHECK(!stable->has_value());
    // ...a candidate hears about the next one.
    auto rc = kd::update::pick_update(json, v("1.0.0-rc2"), true);
    KD_REQUIRE(rc.is_ok());
    KD_REQUIRE(rc->has_value());
    KD_CHECK_EQ((*rc)->tag, std::string("v1.0.0-rc3"));
    KD_CHECK((*rc)->prerelease);

    // A tag with a suffix is a pre-release even if the release was not marked as one.
    const std::string unmarked = list({release("v1.0.0-rc3", false)});
    auto u = kd::update::pick_update(unmarked, v("0.9.0"), false);
    KD_REQUIRE(u.is_ok());
    KD_CHECK(!u->has_value());
}

KD_TEST(updater, the_release_after_a_candidate_is_offered_to_it) {
    const std::string json = list({release("v1.0.0", false), release("v1.0.0-rc2", true)});
    auto r = kd::update::pick_update(json, v("1.0.0-rc2"), true);
    KD_REQUIRE(r.is_ok());
    KD_REQUIRE(r->has_value());
    KD_CHECK_EQ((*r)->tag, std::string("v1.0.0"));
}

KD_TEST(updater, skips_drafts_and_releases_it_cannot_check) {
    const std::string json =
        list({release("v2.0.0", false, /*draft=*/true), release("v1.3.0", false, false, /*installer=*/false),
              release("v1.2.0", false, false, true, /*sums=*/false), release("v1.1.0", false),
              R"({"tag_name":"nightly","assets":[]})", R"("not an object")"});
    auto r = kd::update::pick_update(json, v("1.0.0"), false);
    KD_REQUIRE(r.is_ok());
    KD_REQUIRE(r->has_value());
    KD_CHECK_EQ((*r)->tag, std::string("v1.1.0"));
}

KD_TEST(updater, refuses_installer_names_that_could_leave_the_folder) {
    std::string json = list({release("v1.1.0", false)});
    const std::string good = "kvit-dictate-1.1.0-setup.exe\"";
    json.replace(json.find(good), good.size() - 1, "kvit-dictate-../../evil-setup.exe");
    auto r = kd::update::pick_update(json, v("1.0.0"), false);
    KD_REQUIRE(r.is_ok());
    KD_CHECK(!r->has_value());
}

KD_TEST(updater, an_error_answer_is_an_error) {
    auto r = kd::update::pick_update(R"({"message":"API rate limit exceeded"})", v("1.0.0"), false);
    KD_REQUIRE(!r.is_ok());
    KD_CHECK(r.status().message().find("API rate limit exceeded") != std::string::npos);
    KD_CHECK(!kd::update::pick_update("<html>", v("1.0.0"), false).is_ok());
}

KD_TEST(updater, finds_the_installer_line_in_sha256sums) {
    // Exactly the shape .github/workflows/release.yml writes: lower-case hex, two spaces, LF.
    const std::string sums =
        "4f2c7a0d2e2f5e0c8b1a7f1f6f2b0e7a2c8d7b6a5f4e3d2c1b0a9f8e7d6c5b4a  kvit-dictate-1.0.1-setup.exe\n"
        "0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef *kvit-dictate-1.0.1-win-x64.zip\r\n";
    std::string hex;
    KD_REQUIRE(kd::update::find_checksum(sums, "kvit-dictate-1.0.1-setup.exe", hex));
    KD_CHECK_EQ(hex, std::string("4f2c7a0d2e2f5e0c8b1a7f1f6f2b0e7a2c8d7b6a5f4e3d2c1b0a9f8e7d6c5b4a"));
    KD_REQUIRE(kd::update::find_checksum(sums, "kvit-dictate-1.0.1-win-x64.zip", hex));
    KD_CHECK_EQ(hex, std::string("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));

    KD_CHECK(!kd::update::find_checksum(sums, "kvit-dictate-1.0.1", hex));  // whole name only
    KD_CHECK(!kd::update::find_checksum(sums, "kvit-dictate-1.0.2-setup.exe", hex));
    KD_CHECK(!kd::update::find_checksum("abc  kvit-dictate-1.0.1-setup.exe\n",
                                        "kvit-dictate-1.0.1-setup.exe", hex));
}

KD_TEST(updater, compares_install_directories) {
    using kd::update::same_directory;
    KD_CHECK(same_directory(L"C:\\Users\\a\\AppData\\Local\\Programs\\kvit-dictate\\",
                            L"c:\\users\\A\\appdata\\local\\programs\\KVIT-DICTATE"));
    KD_CHECK(same_directory(L"C:/Programs/kvit-dictate", L"C:\\Programs\\kvit-dictate\\"));
    KD_CHECK(!same_directory(L"C:\\Programs\\kvit-dictate", L"C:\\Programs\\kvit-dictate-old"));
    KD_CHECK(!same_directory(L"", L""));
}

KD_TEST(updater, installer_command_line) {
    const std::filesystem::path setup = L"C:\\Temp\\kvit-dictate-update\\kvit-dictate-1.0.1-setup.exe";
    const std::wstring user =
        kd::update::installer_command_line(setup, kd::update::InstallKind::PerUser, 4242);
    KD_CHECK(user.rfind(L"\"C:\\Temp\\kvit-dictate-update\\kvit-dictate-1.0.1-setup.exe\"", 0) == 0);
    for (const wchar_t* flag : {L" /SILENT", L" /SUPPRESSMSGBOXES", L" /NORESTART", L" /WAITPID=4242",
                                L" /RELAUNCH=1", L" /CURRENTUSER",
                                L" /LOG=\"C:\\Temp\\kvit-dictate-update\\install.log\""}) {
        KD_CHECK(user.find(flag) != std::wstring::npos);
    }
    KD_CHECK(user.find(L"/ALLUSERS") == std::wstring::npos);

    const std::wstring all =
        kd::update::installer_command_line(setup, kd::update::InstallKind::AllUsers, 1);
    KD_CHECK(all.find(L" /ALLUSERS") != std::wstring::npos);
    KD_CHECK(all.find(L"/CURRENTUSER") == std::wstring::npos);
}

// The real thing, against github.com: list, pick, download, verify. Off unless KD_TEST_NETWORK=1,
// because a unit test that needs the internet fails on every machine that has none — and this
// one downloads the whole installer (~65 MB) into %TEMP%\kvit-dictate-update.
KD_TEST(updater, live_github_when_KD_TEST_NETWORK_is_set) {
    char flag[8] = {};
    if (GetEnvironmentVariableA("KD_TEST_NETWORK", flag, sizeof(flag)) == 0 || flag[0] != '1') {
        return;
    }
    // Older than every release there is, and a pre-release, so candidates count too.
    auto r = kd::update::check_github(v("0.0.1-a"));
    KD_REQUIRE(r.is_ok());
    KD_REQUIRE(r->has_value());
    const kd::update::Release rel = **r;
    std::printf("  newest: %s, %s (%llu bytes)\n", rel.tag.c_str(), rel.installer_name.c_str(),
                static_cast<unsigned long long>(rel.installer_size));

    std::atomic<bool> cancel{false};
    uint64_t last = 0;
    std::filesystem::path file;
    const kd::Status st = kd::update::download_installer(
        rel, [&](uint64_t done, uint64_t) { last = done; }, cancel, file);
    KD_REQUIRE(st.ok());
    KD_CHECK(std::filesystem::exists(file));
    KD_CHECK_EQ(std::filesystem::file_size(file), rel.installer_size);
    std::printf("  verified: %s\n", file.string().c_str());

    // A second time is the cached path: no download, same file.
    last = 0;
    std::filesystem::path again;
    KD_REQUIRE(kd::update::download_installer(rel, [&](uint64_t done, uint64_t) { last = done; },
                                              cancel, again)
                   .ok());
    KD_CHECK_EQ(again, file);
    KD_CHECK_EQ(last, 0ull);
}
