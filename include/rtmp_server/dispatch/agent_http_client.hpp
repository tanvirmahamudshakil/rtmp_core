#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "rtmp_server/core/result.hpp"

namespace rtmp_server::dispatch {

struct AgentHttpResponse {
    int status = 0;
    std::string body;
};

// A deliberately minimal, blocking HTTP/1.1 client: POST/DELETE/GET with a
// JSON body, no chunked transfer, no TLS, no redirects, no keep-alive across
// calls. This is what origin -> transcoder-agent job dispatch needs and
// nothing more -- the transcoder tier lives on the same private cluster
// network the heartbeat traffic already crosses in plain HTTP (see
// docs/high-availability.md's trust-boundary note), so the absence of TLS
// here matches the rest of that traffic, not an oversight.
//
// This exists instead of pulling in libcurl (native/http_client.cpp) because
// TranscoderDispatchManager runs on the origin, which must work without the
// codec libraries a transcoder node requires -- linking libcurl there only
// for this one small client would be a heavier dependency than writing it.
// At namespace scope, not nested, so the constructor below can default it:
// a nested type's default member initializers are not usable in a default
// argument of its own enclosing class.
struct AgentHttpClientOptions {
    std::chrono::seconds connect_timeout{5};
    std::chrono::seconds request_timeout{10};
};

class AgentHttpClient {
public:
    using Options = AgentHttpClientOptions;

    explicit AgentHttpClient(Options options = {});

    // `host` excludes scheme and port; `port` and `path` (starting with '/')
    // are separate so a caller never has to hand-assemble a URL.
    [[nodiscard]] core::Result<AgentHttpResponse> post(const std::string& host, std::uint16_t port,
                                                       const std::string& path,
                                                       const std::string& json_body) const;
    [[nodiscard]] core::Result<AgentHttpResponse> del(const std::string& host, std::uint16_t port,
                                                      const std::string& path) const;
    [[nodiscard]] core::Result<AgentHttpResponse> get(const std::string& host, std::uint16_t port,
                                                      const std::string& path) const;

private:
    [[nodiscard]] core::Result<AgentHttpResponse> request(const std::string& method,
                                                          const std::string& host,
                                                          std::uint16_t port, const std::string& path,
                                                          const std::string& body) const;
    Options options_;
};

} // namespace rtmp_server::dispatch
