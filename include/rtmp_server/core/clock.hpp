#pragma once

#include <chrono>
#include <cstdint>

namespace rtmp_server::core {

using MonotonicClock = std::chrono::steady_clock;
using WallClock = std::chrono::system_clock;

[[nodiscard]] inline MonotonicClock::time_point monotonic_now() noexcept {
    return MonotonicClock::now();
}

[[nodiscard]] inline WallClock::time_point wall_now() noexcept { return WallClock::now(); }

// RTMP timestamps are 32-bit milliseconds, deliberately not derived from
// wall-clock time here — see docs/timestamp-model.md for the full model
// (implemented in Phase 5, ahead of media routing).
[[nodiscard]] inline std::uint32_t to_millis(MonotonicClock::duration d) noexcept {
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
}

} // namespace rtmp_server::core
