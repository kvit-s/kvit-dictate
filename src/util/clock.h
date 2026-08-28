// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// kd::IClock — an injectable monotonic millisecond clock.
//
// Why a whole interface for reading the time: three of this project's load-bearing rules are
// *timers*, and every one of them has to be unit-tested deterministically —
//   * the stability hold time T (plan.md §1.4 step 5, default 200 ms),
//   * the endpoint-silence and session-silence timers (plan.md §1.6, 0.8 s and 120 s),
//   * every protocol deadline in docs/protocol.md §1.1 (HELLO_TIMEOUT_MS, READY_TIMEOUT_MS,
//     WRITE_DEADLINE_MS, …).
// A test that has to sleep 120 real seconds to check the session timeout will not be written,
// and a rule that is not tested is a rule that does not hold. So the pure-logic modules
// (session.h, commit_engine.h) take an IClock and the tests hand them a ManualClock.
//
// Monotonic, not wall clock. All of the above are elapsed-time questions; a wall clock jumps
// on NTP sync, DST, and manual changes, and a session that ends because the user's clock
// stepped backwards is a bug nobody will ever reproduce.
//
// This header is Windows-free by design (it uses std::chrono::steady_clock, which on MSVC is
// backed by QueryPerformanceCounter). Modules that need QPC timestamps correlated with WASAPI
// device positions do *not* use this — see audio_capture.h, which carries raw QPC values.
//
// Threading: SteadyClock::now_ms() is thread-safe and lock-free. ManualClock is NOT — it is a
// test fixture, driven from the test thread only.
//
// Implements: infrastructure for plan.md §1.4 and §1.6.

#include <atomic>
#include <chrono>
#include <cstdint>

namespace kd {

// Milliseconds since an unspecified monotonic epoch. Only differences are meaningful.
using Millis = uint64_t;

class IClock {
public:
    virtual ~IClock() = default;
    virtual Millis now_ms() const noexcept = 0;
};

// Production clock. Cheap enough to call per audio buffer.
class SteadyClock final : public IClock {
public:
    Millis now_ms() const noexcept override {
        const auto d = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<Millis>(std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
    }

    // Convenience for code that wants a process-wide clock without threading one through.
    // Prefer injection; this exists for logging and one-off diagnostics.
    static SteadyClock& instance() noexcept {
        static SteadyClock c;
        return c;
    }
};

// Test clock. Time moves only when the test moves it.
class ManualClock final : public IClock {
public:
    explicit ManualClock(Millis start = 0) noexcept : now_(start) {}
    Millis now_ms() const noexcept override { return now_.load(std::memory_order_relaxed); }
    void advance(Millis delta) noexcept { now_.fetch_add(delta, std::memory_order_relaxed); }
    void set(Millis t) noexcept { now_.store(t, std::memory_order_relaxed); }

private:
    std::atomic<Millis> now_;
};

}  // namespace kd
