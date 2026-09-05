#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "rtmp_server/core/result.hpp"
#include "rtmp_server/dispatch/transcoder_job.hpp"

namespace rtmp_server::dispatch {

[[nodiscard]] core::Result<TranscoderJobAssignment> parse_transcoder_job_assignment(
    std::string_view json);

struct TranscoderAgentOptions {
    std::size_t max_jobs = 8;
};

// Owns the jobs accepted by one transcoder node. Mutation is serialized so
// two concurrent retries cannot create two runners for the same id. An exact
// duplicate assignment is idempotent; a changed assignment replaces the old
// runner after stopping it.
class TranscoderAgent {
public:
    using RunnerFactory =
        std::function<std::unique_ptr<TranscoderJob>(TranscoderJobAssignment assignment)>;

    explicit TranscoderAgent(RunnerFactory factory, TranscoderAgentOptions options = {});
    ~TranscoderAgent();
    TranscoderAgent(const TranscoderAgent&) = delete;
    TranscoderAgent& operator=(const TranscoderAgent&) = delete;

    [[nodiscard]] core::Result<TranscoderJobRunnerStatus> upsert(
        TranscoderJobAssignment assignment);
    [[nodiscard]] core::Result<void> remove(std::string_view id);
    [[nodiscard]] std::vector<TranscoderJobRunnerStatus> list() const;
    [[nodiscard]] std::size_t size() const;
    void stop_all();

private:
    struct Entry {
        TranscoderJobAssignment assignment;
        std::unique_ptr<TranscoderJob> runner;
    };

    RunnerFactory factory_;
    TranscoderAgentOptions options_;
    // Serializes replace/remove/stop so a runner is never resurrected by a
    // concurrent request while shutdown is in progress.
    mutable std::mutex mutation_mutex_;
    mutable std::mutex jobs_mutex_;
    std::unordered_map<std::string, Entry> jobs_;
    bool stopping_ = false;
};

} // namespace rtmp_server::dispatch
