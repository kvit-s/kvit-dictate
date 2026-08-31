// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// A canonical 44-byte PCM WAV header, and a one-call writer for a finished buffer.
//
// Header-only because the two callers are on opposite sides of the link graph: `src/corpus.cpp`
// is in `kd_core`, and `src/asrd/session_server.cpp` is in the translator, which links `kd_wire`
// and not `kd_core`. Twenty lines of little-endian field packing is not worth a third library,
// and two copies of it drifting apart is exactly how a corpus ends up with files that one tool
// reads and another does not.
//
// PCM signed 16-bit only. Everything in this program that carries audio carries it in that form
// (docs/protocol.md §6.3), so a format parameter would be a knob with one setting.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace kd::wav {

inline constexpr size_t kHeaderBytes = 44;

// The two little-endian size fields a WAV carries: RIFF's at byte 4, `data`'s at byte 40. A
// streaming writer leaves both at zero and rewrites them as it grows — see `Corpus::flush_sizes`
// for why that matters when the process may not live to close the file.
inline constexpr long kRiffSizeOffset = 4;
inline constexpr long kDataSizeOffset = 40;

inline void put_u32_le(unsigned char* p, uint32_t v) {
    p[0] = static_cast<unsigned char>(v & 0xFF);
    p[1] = static_cast<unsigned char>((v >> 8) & 0xFF);
    p[2] = static_cast<unsigned char>((v >> 16) & 0xFF);
    p[3] = static_cast<unsigned char>((v >> 24) & 0xFF);
}

inline void put_u16_le(unsigned char* p, uint16_t v) {
    p[0] = static_cast<unsigned char>(v & 0xFF);
    p[1] = static_cast<unsigned char>((v >> 8) & 0xFF);
}

// `data_bytes` may be 0 for a stream whose length is not known yet; the file is a valid WAV from
// its first byte either way, and the two offsets above are where the truth is later written.
inline std::array<unsigned char, kHeaderBytes> header(uint32_t sample_rate, uint16_t channels,
                                                      uint32_t data_bytes) {
    std::array<unsigned char, kHeaderBytes> h{};
    constexpr uint16_t kBits = 16;
    const uint32_t byte_rate = sample_rate * channels * (kBits / 8);
    const uint16_t block_align = static_cast<uint16_t>(channels * (kBits / 8));
    std::memcpy(h.data() + 0, "RIFF", 4);
    put_u32_le(h.data() + 4, static_cast<uint32_t>(kHeaderBytes - 8 + data_bytes));
    std::memcpy(h.data() + 8, "WAVEfmt ", 8);
    put_u32_le(h.data() + 16, 16);  // PCM fmt chunk size
    put_u16_le(h.data() + 20, 1);   // WAVE_FORMAT_PCM
    put_u16_le(h.data() + 22, channels);
    put_u32_le(h.data() + 24, sample_rate);
    put_u32_le(h.data() + 28, byte_rate);
    put_u16_le(h.data() + 32, block_align);
    put_u16_le(h.data() + 34, kBits);
    std::memcpy(h.data() + 36, "data", 4);
    put_u32_le(h.data() + 40, data_bytes);
    return h;
}

// One finished buffer, header and all. False on any failure, with nothing left behind worth
// keeping — every caller of this is a diagnostic, and a diagnostic that cannot be written must
// never take down the thing it was diagnosing.
inline bool write_pcm16(const std::filesystem::path& path, const int16_t* samples, size_t count,
                        uint32_t sample_rate = 16000, uint16_t channels = 1) {
    if (samples == nullptr && count != 0) return false;
    const uint32_t data_bytes = static_cast<uint32_t>(count * sizeof(int16_t));
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, path.wstring().c_str(), L"wb") != 0 || f == nullptr) return false;
    const auto h = header(sample_rate, channels, data_bytes);
    bool ok = std::fwrite(h.data(), 1, h.size(), f) == h.size();
    if (ok && count != 0) {
        ok = std::fwrite(samples, sizeof(int16_t), count, f) == count;
    }
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    return ok;
}

}  // namespace kd::wav
