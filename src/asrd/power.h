// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
//
// Keeping Windows from treating a speech decoder as background work.
//
// Windows applies power throttling (EcoQoS) to a process that has no window and is not in the
// foreground, which describes kvit-asrd exactly. On a hybrid Intel CPU it moves the threads to the
// efficiency cores. Measured 2026-11-23 on an i7-14700K: the Nemotron model's
// 560 ms chunks took ~75 ms for the first couple of seconds of work and 240–400 ms for every
// chunk after that, in the same process (docs/copilot-speech-model.md §3). The user is waiting
// on every one of those decodes, so this process opts out, from `main`, before any backend runs.
// It is per process and not inherited.

namespace kd::asrd {

// `process` is a HANDLE with PROCESS_SET_INFORMATION; nullptr means this process. Returns false
// if Windows refused, which costs speed and nothing else, so callers log it and carry on.
bool opt_out_of_power_throttling(void* process = nullptr) noexcept;

}  // namespace kd::asrd
