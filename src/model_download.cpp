// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Model download from Microsoft's Foundry Local catalog — see model_download.h.

#define KD_LOG_MODULE "download"

#include "model_download.h"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <winhttp.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <vector>

#include "util/json.h"
#include "util/log.h"
#include "util/utf8.h"

namespace kd {
namespace {

namespace fs = std::filesystem;

constexpr const char* kRegistry =
    "https://eastus.api.azureml.ms/modelregistry/v1.0/registry/models/nonazureaccount?assetId=";

// Pinned 2026-11-23 from the catalog copy measured in docs/copilot-speech-model.md. `model.onnx`
// (a second copy of encoder.onnx), `model_config.json` and `inference_model.json` are Foundry's
// own bookkeeping and are not fetched. `silero_vad.onnx` is: genai_config.json names it, and the
// runtime may open it even though this program switches its VAD off (backend_nemotron.cpp).
constexpr CatalogFile kNemotronEnFiles[] = {
    {"LICENSE", 1082, "a6f5b307e3ff1acc20fa2fca4ff20fa19c2e2e605fa1f2993e434683803eb61f"},
    {"NOTICES", 372, "5ebcc4025ac107ec76fdfb9e17cd03ab29be75cb4977b3085a08446fed706b13"},
    {"audio_processor_config.json", 413,
     "ab28d41eb87ce3922006edeb9c3fad4d5ce451f9a56a12d84f470f02a5ec157b"},
    {"decoder.onnx", 4414, "3bbf05aa83ef9942a24d15fb7bc395b55fa369b2aea2883ca71f457b77df7a9b"},
    {"decoder.onnx.data", 28879360,
     "cdeaea6b84d05747168ebc6fae03a58a7b66ebd65e3959f70c8df5bfd18276c0"},
    {"encoder.onnx", 2738954, "89b7a08b5b1837bc1c8b3019ae0bc361cdce7250453b2daca6591473c8dd52b4"},
    {"encoder.onnx.data", 687187753,
     "1a7060f5bf919f3f76a48a0269b5134e2ce194e625206f238c4d4ed453093d0b"},
    {"genai_config.json", 2108, "05a7eb7cb0d9e1c0858c0624bf248beb3d1dfa1cef6d9a6effc01b479724423c"},
    {"joint.onnx", 1917, "4d78d3a0affc978c698e4bb04440e80f35f878e7ed957390c8c148315e31072d"},
    {"joint.onnx.data", 6893060, "a9c4504f7cce1935de59de7a8dfff87e7a710c37871286de6a978ff424f1f88f"},
    {"silero_vad.onnx", 2243022, "a4a068cd6cf1ea8355b84327595838ca748ec29a25bc91fc82e6c299ccdc5808"},
    {"tokenizer.json", 49581, "d8b96152ceaaa6f4df976cecc6f954b354842d09ac2698c62507283a22167d4c"},
    {"tokenizer_config.json", 183, "ea4b35353f468fea11f436f837d9621a29b4ba9d1c73c1ed0aa5743f5a53919e"},
    {"vocab.txt", 4939, "fbbb3ac65aebb9f0be6020f46f3e4d0ef326b85d852d7ab92db263798b27be34"},
};

constexpr CatalogModel kNemotronEn{
    "azureml://registries/azureml/models/nemotron-speech-streaming-en-0.6b-generic-cpu/versions/3",
    "v3",
    kNemotronEnFiles,
    sizeof kNemotronEnFiles / sizeof kNemotronEnFiles[0],
};

std::string to_hex(const uint8_t* p, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(digits[p[i] >> 4]);
        out.push_back(digits[p[i] & 0xF]);
    }
    return out;
}

std::string path_text(const fs::path& p) { return utf8::from_utf16_or_empty(p.wstring()); }

// ─── SHA-256, incrementally ─────────────────────────────────────────────────────────────────

class Sha256 {
public:
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
            alg_ = nullptr;
            return;
        }
        if (BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0) != 0) hash_ = nullptr;
    }
    ~Sha256() {
        if (hash_ != nullptr) BCryptDestroyHash(hash_);
        if (alg_ != nullptr) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    [[nodiscard]] bool valid() const noexcept { return hash_ != nullptr; }
    void update(const void* data, size_t n) {
        BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(n),
                       0);
    }
    std::string hex() {
        uint8_t digest[32] = {};
        BCryptFinishHash(hash_, digest, sizeof digest, 0);
        return to_hex(digest, sizeof digest);
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

// ─── One HTTPS GET, streamed ────────────────────────────────────────────────────────────────

struct InternetHandle {
    HINTERNET h = nullptr;
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET x) : h(x) {}
    ~InternetHandle() {
        if (h != nullptr) WinHttpCloseHandle(h);
    }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
};

// `sink` returns false to stop. 30 s for every step: a connection that delivers nothing for
// that long has stalled, and the download restarts from the file that stalled.
Status http_get(const std::string& url, const std::function<bool(const char*, size_t)>& sink,
                const std::atomic<bool>& cancel, uint32_t& status_out) {
    status_out = 0;
    const std::wstring wurl = utf8::to_utf16_or_empty(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        return Status{StatusCode::InvalidArgument, "not an https:// URL"};
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo != nullptr) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    // The product and nothing else: no version, no machine, no user.
    InternetHandle session(WinHttpOpen(L"kvit-dictate", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (session.h == nullptr) {
        return Status{StatusCode::Unavailable, "WinHttpOpen failed", GetLastError()};
    }
    WinHttpSetTimeouts(session.h, 30000, 30000, 30000, 30000);
    InternetHandle connect(WinHttpConnect(session.h, host.c_str(), parts.nPort, 0));
    if (connect.h == nullptr) {
        return Status{StatusCode::Unavailable, "cannot connect to " + utf8::from_utf16_or_empty(host),
                      GetLastError()};
    }
    InternetHandle request(WinHttpOpenRequest(connect.h, L"GET", path.c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              WINHTTP_FLAG_SECURE));
    if (request.h == nullptr) {
        return Status{StatusCode::Unavailable, "WinHttpOpenRequest failed", GetLastError()};
    }
    // The registry asks for a TLS client certificate and accepts none (measured 2026-11-23:
    // ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED on the first send). Saying "no certificate" and
    // sending again is WinHTTP's documented answer to that; curl and browsers do it silently.
    bool sent = false;
    for (int attempt = 0; attempt < 2 && !sent; ++attempt) {
        sent = WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
               WinHttpReceiveResponse(request.h, nullptr);
        if (!sent && GetLastError() == ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED) {
            WinHttpSetOption(request.h, WINHTTP_OPTION_CLIENT_CERT_CONTEXT,
                             WINHTTP_NO_CLIENT_CERT_CONTEXT, 0);
            continue;
        }
        break;
    }
    if (!sent) {
        return Status{StatusCode::Unavailable,
                      "no answer from " + utf8::from_utf16_or_empty(host), GetLastError()};
    }
    DWORD status = 0;
    DWORD size = sizeof status;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    status_out = status;
    if (status != 200) {
        return Status{StatusCode::Unavailable, utf8::from_utf16_or_empty(host) + " answered HTTP " +
                                                   std::to_string(status)};
    }
    std::vector<char> buf(1u << 20);
    for (;;) {
        if (cancel.load()) return Status{StatusCode::Unavailable, "the download was cancelled"};
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buf.data(), static_cast<DWORD>(buf.size()), &got)) {
            return Status{StatusCode::Unavailable,
                          "the download from " + utf8::from_utf16_or_empty(host) + " broke off",
                          GetLastError()};
        }
        if (got == 0) return Status::success();
        if (!sink(buf.data(), got)) return Status{StatusCode::Malformed, "rejected by the sink"};
    }
}

Status file_matches(const CatalogFile& f, const fs::path& file, bool hash) {
    std::error_code ec;
    const uint64_t size = fs::file_size(file, ec);
    if (ec) return Status{StatusCode::NotFound, std::string(f.name) + " is missing"};
    if (size != f.size) {
        return Status{StatusCode::Malformed, std::string(f.name) + " is " + std::to_string(size) +
                                                 " bytes, expected " + std::to_string(f.size)};
    }
    if (!hash) return Status::success();
    Result<std::string> digest = sha256_file(file);
    if (!digest.is_ok()) return digest.status();
    if (*digest != f.sha256) {
        return Status{StatusCode::Malformed,
                      std::string(f.name) + " does not have the expected SHA-256"};
    }
    return Status::success();
}

}  // namespace

uint64_t CatalogModel::total_bytes() const noexcept {
    uint64_t n = 0;
    for (size_t i = 0; i < file_count; ++i) n += files[i].size;
    return n;
}

const CatalogModel& nemotron_en_catalog_model() { return kNemotronEn; }

fs::path default_nemotron_model_dir() {
    PWSTR base = nullptr;
    fs::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) dir = base;
    CoTaskMemFree(base);
    return dir / L"kvit-dictate" / L"models" / L"nemotron-speech-streaming-en-0.6b-v3";
}

Result<RegistryLocation> parse_registry_answer(std::string_view json_text) {
    json::Value root;
    json::Error err;
    if (!json::parse_object(json_text, root, err)) {
        return Status{StatusCode::Malformed, "the model registry's answer is not JSON"};
    }
    const json::Value* sas = root.find("blobSasUri");
    std::string_view uri;
    if (sas == nullptr || !sas->get_string(uri) || uri.empty()) {
        return Status{StatusCode::Malformed, "the model registry's answer has no blobSasUri"};
    }
    const size_t q = uri.find('?');
    if (q == std::string_view::npos || q + 1 >= uri.size() || uri.substr(0, 8) != "https://") {
        return Status{StatusCode::Malformed,
                      "the model registry's blobSasUri is not an https:// URL with a query"};
    }
    RegistryLocation loc;
    loc.container_url = std::string(uri.substr(0, q));
    while (!loc.container_url.empty() && loc.container_url.back() == '/') {
        loc.container_url.pop_back();
    }
    loc.sas_query = std::string(uri.substr(q + 1));

    // `modelEntity.kvTags.directoryPath`, falling back to `systemMetadata.directoryPath`.
    const json::Value* entity = root.find("modelEntity");
    for (const char* holder : {"kvTags", "systemMetadata"}) {
        if (entity == nullptr) break;
        const json::Value* h = entity->find(holder);
        const json::Value* d = h != nullptr ? h->find("directoryPath") : nullptr;
        std::string_view dir;
        if (d != nullptr && d->get_string(dir) && !dir.empty()) {
            loc.directory = std::string(dir);
            break;
        }
    }
    return loc;
}

Result<std::string> sha256_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return Status{StatusCode::NotFound, "cannot open " + path_text(file)};
    Sha256 h;
    if (!h.valid()) return Status{StatusCode::Internal, "BCrypt SHA-256 is unavailable"};
    std::vector<char> buf(1u << 20);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = in.gcount();
        if (n > 0) h.update(buf.data(), static_cast<size_t>(n));
    }
    return h.hex();
}

Status verify_model_dir(const CatalogModel& model, const fs::path& dir, bool hash) {
    for (size_t i = 0; i < model.file_count; ++i) {
        const Status st = file_matches(model.files[i], dir / model.files[i].name, hash);
        if (!st.ok()) return st;
    }
    return Status::success();
}

Status download_catalog_model(const CatalogModel& model, const fs::path& dir,
                              const DownloadProgress& progress, const std::atomic<bool>& cancel) {
    if (verify_model_dir(model, dir, false).ok()) return Status::success();

    const uint64_t total = model.total_bytes();
    KD_LOG_INFO("downloading %s (%llu MB) into %s", model.asset_id,
                static_cast<unsigned long long>(total / 1000000), path_text(dir).c_str());

    // 1. Where the files are.
    std::string answer;
    uint32_t http_status = 0;
    Status st = http_get(
        std::string(kRegistry) + model.asset_id,
        [&answer](const char* p, size_t n) {
            answer.append(p, n);
            return answer.size() < (4u << 20);
        },
        cancel, http_status);
    if (!st.ok()) {
        return Status{StatusCode::Unavailable,
                      "Microsoft's model registry did not answer: " + st.message(), st.os_error()};
    }
    Result<RegistryLocation> loc = parse_registry_answer(answer);
    if (!loc.is_ok()) return loc.status();
    if (!loc->directory.empty() && loc->directory != model.directory) {
        // The pinned files are version-specific; a registry that has moved them has a different
        // version behind the same name, and the hashes below would reject every file anyway.
        return Status{StatusCode::Malformed, "the model registry now keeps this model in \"" +
                                                 loc->directory + "\", not \"" + model.directory +
                                                 "\"; this build cannot use that version"};
    }

    // 2. The files, into a partial folder.
    fs::path partial = dir;
    partial += L".partial";
    std::error_code ec;
    fs::create_directories(partial, ec);
    if (ec) {
        return Status{StatusCode::OsError, "cannot create " + path_text(partial) + ": " + ec.message()};
    }

    uint64_t done = 0;
    for (size_t i = 0; i < model.file_count; ++i) {
        const CatalogFile& f = model.files[i];
        const fs::path target = partial / f.name;
        if (file_matches(f, target, true).ok()) {
            done += f.size;
            if (progress) progress(done, total);
            continue;
        }
        fs::path part = target;
        part += L".part";
        {
            std::ofstream out(part, std::ios::binary | std::ios::trunc);
            if (!out) return Status{StatusCode::OsError, "cannot write " + path_text(part)};
            Sha256 h;
            if (!h.valid()) return Status{StatusCode::Internal, "BCrypt SHA-256 is unavailable"};
            uint64_t got = 0;
            uint64_t last_report = 0;
            const std::string url = loc->container_url + "/" + model.directory + "/" + f.name +
                                    "?" + loc->sas_query;
            st = http_get(
                url,
                [&](const char* p, size_t n) {
                    got += n;
                    if (got > f.size) return false;  // longer than pinned: not this file
                    h.update(p, n);
                    out.write(p, static_cast<std::streamsize>(n));
                    if (progress && got - last_report >= (8u << 20)) {
                        last_report = got;
                        progress(done + got, total);
                    }
                    return static_cast<bool>(out);
                },
                cancel, http_status);
            out.close();
            if (st.ok() && got != f.size) {
                st = Status{StatusCode::Malformed, std::string(f.name) + " arrived as " +
                                                       std::to_string(got) + " bytes, expected " +
                                                       std::to_string(f.size)};
            }
            if (st.ok() && h.hex() != f.sha256) {
                st = Status{StatusCode::Malformed,
                            std::string(f.name) +
                                " did not match its pinned SHA-256; it was deleted. The catalog "
                                "copy may have changed, or the download was tampered with"};
            }
            if (!st.ok()) {
                fs::remove(part, ec);
                return Status{st.code(), "downloading " + std::string(f.name) + ": " + st.message(),
                              st.os_error()};
            }
        }
        fs::rename(part, target, ec);
        if (ec) return Status{StatusCode::OsError, "cannot rename " + path_text(part)};
        done += f.size;
        if (progress) progress(done, total);
    }

    // 3. Only a complete, checked folder takes the real name.
    if (fs::exists(dir, ec)) fs::remove_all(dir, ec);
    fs::rename(partial, dir, ec);
    if (ec) {
        return Status{StatusCode::OsError,
                      "cannot rename " + path_text(partial) + " to " + path_text(dir) + ": " +
                          ec.message()};
    }
    KD_LOG_INFO("the model is downloaded and every file matches its pinned SHA-256");
    return Status::success();
}

}  // namespace kd
