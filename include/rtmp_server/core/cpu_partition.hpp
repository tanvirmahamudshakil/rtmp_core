#pragma once

#include <cstdint>
#include <vector>

namespace rtmp_server::core {

// Splits the machine's logical cores into a "transcode" set and an "other"
// set so encoder-heavy work and everything else (RTMP ingest, HTTP control
// API, admin/API threads) never compete for the same core under the OS
// scheduler. `transcode_percent` is clamped to [0, 100]; 0 disables the
// split entirely (both vectors come back empty, meaning "no reservation" to
// every caller — today's unpinned behaviour).
//
// Cores are assigned contiguously (transcode gets cores
// [0, floor(total * percent / 100)), other gets the rest) rather than
// interleaved, so a NUMA/cache-topology-aware operator can reason about the
// split from core index alone. At least one core is left in each non-empty
// set whenever both a reservation and headroom exist, so a reservation can
// never starve the other side down to zero cores on a small box.
struct CpuPartition {
    std::vector<unsigned> transcode_cores;
    std::vector<unsigned> other_cores;
};

// Detects hardware_concurrency() internally (falls back to 1 if the platform
// can't report it). Pure/deterministic given that count, so it is unit
// testable via compute_cpu_partition_for(total_cores, transcode_percent).
[[nodiscard]] CpuPartition compute_cpu_partition(std::uint32_t transcode_percent);
[[nodiscard]] CpuPartition compute_cpu_partition_for(unsigned total_cores,
                                                     std::uint32_t transcode_percent);

// Best-effort: restricts the calling thread to the given core set via
// pthread_setaffinity_np. A child thread created afterwards (including one a
// third-party library such as libx264/libx265 spawns internally for its own
// frame-parallelism) inherits the calling thread's affinity mask at clone()
// time, so pinning the thread that opens an encoder is sufficient to confine
// the encoder's own worker threads too — no cooperation from the library is
// needed. Empty `cores` is a no-op (nothing to restrict to). Failure (e.g. a
// cgroup-restricted cpuset on a container host, or a non-Linux platform) is
// swallowed: CPU isolation is a placement optimisation, not a correctness
// requirement, so it must never take the thread down with it.
void pin_current_thread_to_cores(const std::vector<unsigned>& cores);

} // namespace rtmp_server::core
