// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// Downloading a model from Microsoft's Foundry Local catalog without the Foundry Local runtime.
//
// The catalog is where VS Code's dictation and Copilot CLI's `/voice` get their speech model
// (docs/copilot-speech-model.md). The runtime they use to fetch it, `foundry_local.dll`, carries
// Microsoft's 1DS telemetry client and uploads a `ProcessInfo` event even with telemetry
// switched off. That contradicts the README's "No telemetry", so this file does the two HTTPS
// requests the runtime would have done, and nothing else:
//
//   1. GET https://eastus.api.azureml.ms/modelregistry/v1.0/registry/models/nonazureaccount
//          ?assetId=azureml://registries/azureml/models/<name>/versions/<n>
//      Anonymous. The answer names a blob container and a read-only SAS for it (`blobSasUri`),
//      and the folder the files are in (`modelEntity.kvTags.directoryPath`).
//   2. GET <container>/<folder>/<file>?<sas>, once per file.
//
// **What is trusted is the list below, not the registry.** Every file's name, size and SHA-256
// is pinned here, from the copy measured on 2026-11-23 (whose MD5s matched the registry's own
// `Content-MD5` for every file). The registry only says *where* the bytes are. A file that does
// not hash to its pin is deleted and the download fails, so a changed upstream is a failed
// Test, never a different model running silently. A new model version is a code change.
//
// Files land in `<dir>.partial\` and the folder is renamed to `<dir>` only when every file has
// been checked, so a half-finished download is never mistaken for a model. A file already in
// the partial folder with the right size and hash is kept, which makes an interrupted download
// resumable at file granularity.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "util/result.h"

namespace kd {

struct CatalogFile {
    const char* name;
    uint64_t size;
    const char* sha256;  // lowercase hex
};

struct CatalogModel {
    const char* asset_id;   // azureml://registries/azureml/models/<name>/versions/<n>
    const char* directory;  // the blob folder the registry is expected to name, e.g. "v3"
    const CatalogFile* files;
    size_t file_count;
    [[nodiscard]] uint64_t total_bytes() const noexcept;
};

// nemotron-speech-streaming-en-0.6b-generic-cpu:3 — 14 files, 728 MB.
const CatalogModel& nemotron_en_catalog_model();

// Where that model lives: `%LOCALAPPDATA%\kvit-dictate\models\nemotron-speech-streaming-en-0.6b-v3`.
// Per user, beside settings.json and the logs, and nowhere the installer owns: an upgrade or an
// uninstall that replaced the program folder would otherwise throw away a 728 MB download. The
// Settings page downloads into it and kvit-asrd reads from it, so both ask this one function.
std::filesystem::path default_nemotron_model_dir();

// The registry's answer, reduced to the two things used from it.
struct RegistryLocation {
    std::string container_url;  // https://<account>.blob.core.windows.net/<container>
    std::string sas_query;      // without the leading '?'
    std::string directory;
};

// Pure, for the tests: pull `blobSasUri` and `directoryPath` out of the registry's JSON.
Result<RegistryLocation> parse_registry_answer(std::string_view json_text);

// Every pinned file present with its pinned size. With `hash`, also its SHA-256 (about two
// seconds for 728 MB). Fails naming the first file that is wrong.
Status verify_model_dir(const CatalogModel& model, const std::filesystem::path& dir, bool hash);

// SHA-256 of a file, lowercase hex. Exposed for the tests.
Result<std::string> sha256_file(const std::filesystem::path& file);

using DownloadProgress = std::function<void(uint64_t done_bytes, uint64_t total_bytes)>;

// Download `model` into `dir`. Returns at once if `dir` already verifies by size. `cancel` is
// polled between reads; a cancelled download keeps what it has in `<dir>.partial`.
Status download_catalog_model(const CatalogModel& model, const std::filesystem::path& dir,
                              const DownloadProgress& progress, const std::atomic<bool>& cancel);

}  // namespace kd
