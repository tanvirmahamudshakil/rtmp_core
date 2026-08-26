#include "rtmp_server/core/cpu_partition.hpp"

#include <algorithm>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace rtmp_server::core {

CpuPartition compute_cpu_partition_for(unsigned total_cores, std::uint32_t transcode_percent) {
    CpuPartition partition;
    // A single-core box has no second core to isolate anything onto; leave
    // it unpinned rather than clamp into a degenerate 1-vs-0 split.
    if (total_cores < 2 || transcode_percent == 0) return partition;
    const std::uint32_t percent = std::min<std::uint32_t>(transcode_percent, 100);

    unsigned transcode_count =
        static_cast<unsigned>((static_cast<std::uint64_t>(total_cores) * percent) / 100);
    // A non-zero request must reserve at least one core, and a percentage
    // below 100 must leave at least one core for everything else -- a rounded
    // split that silently starves one side to zero cores would defeat the
    // isolation this exists to provide.
    transcode_count = std::clamp<unsigned>(transcode_count, 1, total_cores - (percent < 100 ? 1 : 0));

    partition.transcode_cores.reserve(transcode_count);
    for (unsigned core = 0; core < transcode_count; ++core) {
        partition.transcode_cores.push_back(core);
    }
    partition.other_cores.reserve(total_cores - transcode_count);
    for (unsigned core = transcode_count; core < total_cores; ++core) {
        partition.other_cores.push_back(core);
    }
    return partition;
}

CpuPartition compute_cpu_partition(std::uint32_t transcode_percent) {
    const unsigned hardware = std::thread::hardware_concurrency();
    return compute_cpu_partition_for(hardware == 0 ? 1 : hardware, transcode_percent);
}

void pin_current_thread_to_cores(const std::vector<unsigned>& cores) {
    if (cores.empty()) return;
#if defined(__linux__)
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    for (unsigned core : cores) CPU_SET(core, &cpu_set);
    [[maybe_unused]] int rc = ::pthread_setaffinity_np(::pthread_self(), sizeof(cpu_set_t), &cpu_set);
#else
    (void)cores;
#endif
}

} // namespace rtmp_server::core
