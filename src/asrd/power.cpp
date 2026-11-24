// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// See power.h.

#include "asrd/power.h"

#include <windows.h>

namespace kd::asrd {

bool opt_out_of_power_throttling(void* process) noexcept {
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    // Controlled, and off: "this process is not to be run for efficiency".
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0;
    const HANDLE h = process != nullptr ? static_cast<HANDLE>(process) : GetCurrentProcess();
    return SetProcessInformation(h, ProcessPowerThrottling, &state, sizeof state) != FALSE;
}

}  // namespace kd::asrd
