// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// The dictation corpus — see corpus.h for what it is for and why it is session-scoped.

#define KD_LOG_MODULE "corpus"

#include "corpus.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <numeric>
#include <vector>

#include "util/log.h"

namespace kd {
namespace {

constexpr uint32_t kSampleRate = 16000;
constexpr uint16_t kChannels = 1;
constexpr uint16_t kBitsPerSample = 16;
constexpr size_t kWavHeaderBytes = 44;

// The two little-endian fields a WAV header carries twice over: RIFF's total size at byte 4 and
// `data`'s payload size at byte 40. Both are written as zero up front and rewritten in place.
constexpr long kRiffSizeOffset = 4;
constexpr long kDataSizeOffset = 40;

void put_u32(unsigned char* p, uint32_t v) {
    p[0] = static_cast<unsigned char>(v & 0xFF);
    p[1] = static_cast<unsigned char>((v >> 8) & 0xFF);
    p[2] = static_cast<unsigned char>((v >> 16) & 0xFF);
    p[3] = static_cast<unsigned char>((v >> 24) & 0xFF);
}

void put_u16(unsigned char* p, uint16_t v) {
    p[0] = static_cast<unsigned char>(v & 0xFF);
    p[1] = static_cast<unsigned char>((v >> 8) & 0xFF);
}

// A canonical 44-byte PCM header with both size fields left at zero. Nothing here depends on the
// eventual length, which is the whole point: the file is valid from its first byte and the sizes
// are corrected as it grows.
std::array<unsigned char, kWavHeaderBytes> wav_header() {
    std::array<unsigned char, kWavHeaderBytes> h{};
    const uint32_t byte_rate = kSampleRate * kChannels * (kBitsPerSample / 8);
    const uint16_t block_align = kChannels * (kBitsPerSample / 8);
    std::memcpy(h.data() + 0, "RIFF", 4);
    put_u32(h.data() + 4, 0);  // patched
    std::memcpy(h.data() + 8, "WAVEfmt ", 8);
    put_u32(h.data() + 16, 16);  // PCM fmt chunk size
    put_u16(h.data() + 20, 1);   // WAVE_FORMAT_PCM
    put_u16(h.data() + 22, kChannels);
    put_u32(h.data() + 24, kSampleRate);
    put_u32(h.data() + 28, byte_rate);
    put_u16(h.data() + 32, block_align);
    put_u16(h.data() + 34, kBitsPerSample);
    std::memcpy(h.data() + 36, "data", 4);
    put_u32(h.data() + 40, 0);  // patched
    return h;
}

std::string stamp_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

std::string iso_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

uint64_t dir_size(const std::filesystem::path& p) {
    uint64_t total = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(p, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec)) total += it->file_size(ec);
    }
    return total;
}

}  // namespace

// ─── Pruning ────────────────────────────────────────────────────────────────────────────────

std::vector<size_t> corpus_prune_set(const std::vector<uint64_t>& sizes,
                                     const std::vector<uint32_t>& ages, uint64_t max_bytes,
                                     uint32_t max_age_days) {
    std::vector<size_t> doomed;
    const size_t n = std::min(sizes.size(), ages.size());

    // Age first, because it is absolute: a session past the age bound goes whatever the store
    // weighs. Zero disables, rather than meaning "everything is too old" — the reading that
    // cannot wipe a corpus because of a zero in a config file.
    std::vector<bool> gone(n, false);
    if (max_age_days != 0) {
        for (size_t i = 0; i < n; ++i) {
            if (ages[i] > max_age_days) {
                gone[i] = true;
                doomed.push_back(i);
            }
        }
    }

    if (max_bytes == 0) return doomed;

    uint64_t total = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!gone[i]) total += sizes[i];
    }
    if (total <= max_bytes) return doomed;

    // Oldest-first, which is the order the caller hands them in — `ages` is only day-granular, so
    // the caller's ordering is what breaks ties, not this sort.
    std::vector<size_t> by_age(n);
    std::iota(by_age.begin(), by_age.end(), size_t{0});
    std::stable_sort(by_age.begin(), by_age.end(),
                     [&ages](size_t a, size_t b) { return ages[a] > ages[b]; });

    // Exactly one entry is protected: the newest. A corpus that deleted the session being
    // recorded to make room for it would lose the run in progress, and being briefly over the
    // bound is the better failure.
    //
    // **The newest, not everything sharing the newest age.** Ages are in whole days, so on a busy
    // day every session is age 0; a rule that spared them all would spare the entire store and
    // the byte bound would never bind again until midnight.
    const size_t protect = by_age.back();

    for (size_t idx : by_age) {
        if (total <= max_bytes) break;
        if (gone[idx] || idx == protect) continue;
        gone[idx] = true;
        doomed.push_back(idx);
        total -= sizes[idx];
    }
    return doomed;
}

// ─── The session ────────────────────────────────────────────────────────────────────────────

struct Corpus::Impl {
    std::FILE* wav = nullptr;
    std::ofstream events;
    std::filesystem::path dir;
    uint64_t samples = 0;
    bool closed = true;
};

Corpus::Corpus() : impl_(std::make_unique<Impl>()) {}

Corpus::~Corpus() {
    if (impl_ && !impl_->closed) close("destructor", json::Value::object());
}

bool Corpus::active() const noexcept { return impl_ && !impl_->closed && impl_->wav != nullptr; }

const std::filesystem::path& Corpus::session_dir() const noexcept { return impl_->dir; }

uint64_t Corpus::audio_ms() const noexcept {
    return impl_->samples * 1000u / kSampleRate;
}

namespace {

// Prune the store to `cfg`'s bounds. Every failure here is logged and swallowed: a corpus that
// could not be tidied must not stop the session it was about to record.
void prune_store(const std::filesystem::path& root, const CorpusConfig& cfg) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec)) return;

    struct Entry {
        std::filesystem::path path;
        uint64_t size;
        uint32_t age_days;
        std::filesystem::file_time_type written;
    };
    std::vector<Entry> entries;
    const auto now = std::filesystem::file_time_type::clock::now();
    for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        if (!e.is_directory(ec)) continue;
        const auto wt = std::filesystem::last_write_time(e.path(), ec);
        if (ec) continue;
        const auto age = std::chrono::duration_cast<std::chrono::hours>(now - wt).count() / 24;
        entries.push_back({e.path(), dir_size(e.path()),
                           static_cast<uint32_t>(age < 0 ? 0 : age), wt});
    }
    if (entries.empty()) return;

    // By the real timestamp, not by `age_days`: the bound is in days but the *ordering* has to be
    // finer than a day or a busy day's sessions are pruned in whatever order the directory
    // happened to be walked in.
    std::stable_sort(entries.begin(), entries.end(),
                     [](const Entry& a, const Entry& b) { return a.written < b.written; });

    std::vector<uint64_t> sizes;
    std::vector<uint32_t> ages;
    sizes.reserve(entries.size());
    ages.reserve(entries.size());
    for (const Entry& e : entries) {
        sizes.push_back(e.size);
        ages.push_back(e.age_days);
    }

    for (size_t idx : corpus_prune_set(sizes, ages, cfg.max_bytes, cfg.max_age_days)) {
        const uintmax_t removed = std::filesystem::remove_all(entries[idx].path, ec);
        if (ec) {
            KD_LOG_WARN("could not prune %s: %s", entries[idx].path.string().c_str(),
                        ec.message().c_str());
            continue;
        }
        KD_LOG_INFO("pruned %s (%llu bytes, %u days old, %llu files)",
                    entries[idx].path.filename().string().c_str(),
                    static_cast<unsigned long long>(entries[idx].size), entries[idx].age_days,
                    static_cast<unsigned long long>(removed));
    }
}

}  // namespace

Status Corpus::open(const CorpusConfig& cfg, std::string_view session_id,
                    const json::Value& header) {
    if (!impl_->closed) close("superseded", json::Value::object());
    impl_->samples = 0;
    impl_->closed = true;

    if (!cfg.enabled) return Status::success();

    std::filesystem::path root = cfg.dir;
    if (root.empty()) return Status{StatusCode::InvalidArgument, "corpus directory is not set"};

    // Bounded *before* the write, so the bound is a bound rather than a high-water mark.
    prune_store(root, cfg);

    const std::filesystem::path dir = root / (stamp_now() + "-" + std::string(session_id));
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        return Status{StatusCode::Unavailable,
                      "could not create " + dir.string() + ": " + ec.message()};
    }

    const std::filesystem::path wav_path = dir / "audio.wav";
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, wav_path.wstring().c_str(), L"wb+") != 0 || f == nullptr) {
        return Status{StatusCode::Unavailable, "could not open " + wav_path.string()};
    }
    const auto h = wav_header();
    if (std::fwrite(h.data(), 1, h.size(), f) != h.size()) {
        std::fclose(f);
        return Status{StatusCode::Unavailable, "could not write the WAV header"};
    }
    std::fflush(f);

    impl_->events.open(dir / "events.jsonl", std::ios::binary | std::ios::trunc);
    if (!impl_->events) {
        std::fclose(f);
        return Status{StatusCode::Unavailable, "could not open events.jsonl"};
    }

    impl_->wav = f;
    impl_->dir = dir;
    impl_->closed = false;

    json::Value start = header;
    start.set("sample_rate", json::Value::number(static_cast<int64_t>(kSampleRate)));
    start.set("channels", json::Value::number(static_cast<int64_t>(kChannels)));
    event("start", std::move(start));

    KD_LOG_INFO("recording this session to %s", dir.string().c_str());
    return Status::success();
}

void Corpus::append_audio(const int16_t* samples, size_t count) {
    if (!active() || samples == nullptr || count == 0) return;
    const size_t wrote = std::fwrite(samples, sizeof(int16_t), count, impl_->wav);
    impl_->samples += wrote;
    if (wrote != count) {
        KD_LOG_WARN("the corpus recording is short by %zu samples; the disk may be full",
                    count - wrote);
    }
}

void Corpus::event(std::string_view type, json::Value body) {
    if (impl_->closed || !impl_->events) return;
    body.set("t", json::Value::string(std::string(type)));
    // Every event indexes the recording by the same clock the recording is on, which is what
    // makes the pair replayable: the audio offset, not the wall clock, because a wall clock
    // cannot be lined up with a WAV.
    body.set("ms", json::Value::number(static_cast<int64_t>(audio_ms())));
    body.set("wall", json::Value::string(iso_now()));
    impl_->events << json::serialize(body) << "\n";
    impl_->events.flush();
}

void Corpus::flush_sizes() {
    if (!active()) return;
    const uint64_t data_bytes = impl_->samples * sizeof(int16_t);
    unsigned char buf[4];
    std::fflush(impl_->wav);
    const long here = std::ftell(impl_->wav);

    put_u32(buf, static_cast<uint32_t>(kWavHeaderBytes - 8 + data_bytes));
    std::fseek(impl_->wav, kRiffSizeOffset, SEEK_SET);
    std::fwrite(buf, 1, 4, impl_->wav);

    put_u32(buf, static_cast<uint32_t>(data_bytes));
    std::fseek(impl_->wav, kDataSizeOffset, SEEK_SET);
    std::fwrite(buf, 1, 4, impl_->wav);

    std::fseek(impl_->wav, here, SEEK_SET);
    std::fflush(impl_->wav);
}

void Corpus::close(std::string_view reason, const json::Value& tail) {
    if (impl_->closed) return;

    json::Value end = tail;
    end.set("reason", json::Value::string(std::string(reason)));
    end.set("audio_ms", json::Value::number(static_cast<int64_t>(audio_ms())));
    event("end", std::move(end));

    flush_sizes();
    if (impl_->wav != nullptr) {
        std::fclose(impl_->wav);
        impl_->wav = nullptr;
    }
    impl_->events.close();
    impl_->closed = true;
    KD_LOG_INFO("recorded %llu ms of audio to %s",
                static_cast<unsigned long long>(audio_ms()), impl_->dir.string().c_str());
}

}  // namespace kd
