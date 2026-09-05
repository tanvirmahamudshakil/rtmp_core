#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rtmp_server/dispatch/transcoder_dispatch_manager.hpp"

namespace rtmp_server::dispatch {

// The wire-level job sent by an origin to a transcoder agent.
struct TranscoderJobAssignment {
    std::string id; // "application/name"
    std::string source_url;
    std::uint32_t fps = 30;
    std::string target_application;
    std::string origin_rtmp_host;
    std::uint16_t origin_rtmp_port = 1935;
    std::vector<DispatchedRendition> renditions;
};

enum class TranscoderJobRunnerState { Connecting, Running, Error, Stopped };

struct TranscoderJobRunnerStatus {
    std::string id;
    TranscoderJobRunnerState state = TranscoderJobRunnerState::Connecting;
    std::string detail;
    std::uint64_t bytes_pushed = 0;
};

// Small interface so the agent's admission/lifecycle logic remains testable
// without loading codec libraries or opening RTMP sockets.
class TranscoderJob {
public:
    virtual ~TranscoderJob() = default;
    virtual void stop() = 0;
    [[nodiscard]] virtual TranscoderJobRunnerStatus status() const = 0;
};

[[nodiscard]] bool operator==(const TranscoderJobAssignment& lhs,
                              const TranscoderJobAssignment& rhs);

} // namespace rtmp_server::dispatch
