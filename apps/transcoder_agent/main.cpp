#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#include "rtmp_server/control/http_server.hpp"
#include "rtmp_server/dispatch/transcoder_agent.hpp"
#include "rtmp_server/dispatch/transcoder_job_runner.hpp"

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void request_stop(int) { stop_requested = 1; }

std::string environment_or(const char* name, std::string fallback) {
    const char* value = std::getenv(name);
    return value == nullptr || *value == '\0' ? std::move(fallback) : std::string(value);
}

template <typename Integer>
Integer environment_number(const char* name, Integer fallback, Integer minimum, Integer maximum) {
    const char* text = std::getenv(name);
    if (text == nullptr || *text == '\0') return fallback;
    Integer value{};
    const std::string_view input(text);
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() || value < minimum ||
        value > maximum) {
        std::cerr << name << " must be between " << minimum << " and " << maximum << '\n';
        std::exit(2);
    }
    return value;
}

std::string json_escape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const char c : value) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) >= 0x20) result.push_back(c);
                break;
        }
    }
    return result;
}

std::string state_name(rtmp_server::dispatch::TranscoderJobRunnerState state) {
    using State = rtmp_server::dispatch::TranscoderJobRunnerState;
    switch (state) {
        case State::Connecting: return "connecting";
        case State::Running: return "running";
        case State::Error: return "error";
        case State::Stopped: return "stopped";
    }
    return "unknown";
}

std::string status_json(const rtmp_server::dispatch::TranscoderJobRunnerStatus& status) {
    std::ostringstream out;
    out << R"({"id":")" << json_escape(status.id) << R"(","state":")"
        << state_name(status.state) << R"(","detail":")" << json_escape(status.detail)
        << R"(","bytes_pushed":)" << status.bytes_pushed << '}';
    return out.str();
}

rtmp_server::control::HttpResponse error_response(int status, std::string_view code,
                                                  std::string_view message) {
    return rtmp_server::control::HttpResponse::json(
        status, R"({"error":")" + json_escape(code) + R"(","message":")" +
                    json_escape(message) + R"("})");
}

int error_status(rtmp_server::core::ErrorCode code) {
    using Code = rtmp_server::core::ErrorCode;
    switch (code) {
        case Code::NotFound: return 404;
        case Code::Conflict: return 409;
        case Code::ResourceExhausted: return 503;
        case Code::InvalidStateTransition: return 503;
        case Code::InvalidConfiguration:
        case Code::InvalidArgument: return 400;
        default: return 500;
    }
}

rtmp_server::control::HttpResponse handle_request(
    rtmp_server::dispatch::TranscoderAgent& agent,
    const rtmp_server::control::HttpRequest& request) {
    using rtmp_server::control::HttpResponse;
    if (request.method == "GET" &&
        (request.path == "/health/live" || request.path == "/health/ready")) {
        return HttpResponse::json(200, R"({"ok":true})");
    }
    if (request.path == "/jobs" && request.method == "GET") {
        std::ostringstream out;
        out << R"({"items":[)";
        const auto jobs = agent.list();
        for (std::size_t i = 0; i < jobs.size(); ++i) {
            if (i) out << ',';
            out << status_json(jobs[i]);
        }
        out << "]}";
        return HttpResponse::json(200, out.str());
    }
    if (request.path == "/jobs" && request.method == "POST") {
        auto assignment = rtmp_server::dispatch::parse_transcoder_job_assignment(request.body);
        if (!assignment) {
            return error_response(error_status(assignment.error().code()), "invalid_assignment",
                                  assignment.error().message());
        }
        auto started = agent.upsert(std::move(assignment).value());
        if (!started) {
            return error_response(error_status(started.error().code()), "job_start_failed",
                                  started.error().message());
        }
        return HttpResponse::json(200, status_json(started.value()));
    }
    constexpr std::string_view prefix = "/jobs/";
    if (request.path.starts_with(prefix) && request.method == "DELETE") {
        const std::string_view id(request.path.data() + prefix.size(),
                                  request.path.size() - prefix.size());
        auto removed = agent.remove(id);
        if (!removed) {
            return error_response(error_status(removed.error().code()), "job_remove_failed",
                                  removed.error().message());
        }
        return HttpResponse::json(200, R"({"deleted":true})");
    }
    if (request.path == "/jobs" || request.path.starts_with(prefix)) {
        return error_response(405, "method_not_allowed", "method not allowed for this route");
    }
    return error_response(404, "not_found", "route not found");
}

} // namespace

int main() {
    using namespace rtmp_server;
    // HttpServer uses ordinary send(2); a client closing between request and
    // response must fail that request, not terminate every transcode job.
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);

    const auto bind = environment_or("RTMP_TRANSCODER_BIND", "0.0.0.0");
    const auto port = environment_number<std::uint16_t>("RTMP_TRANSCODER_PORT", 9200, 1, 65535);
    const auto max_jobs = environment_number<std::size_t>(
        "RTMP_TRANSCODER_MAX_JOBS", 8, 1, std::numeric_limits<std::size_t>::max());

    dispatch::TranscoderAgent agent(
        [](dispatch::TranscoderJobAssignment assignment) -> std::unique_ptr<dispatch::TranscoderJob> {
            return std::make_unique<dispatch::TranscoderJobRunner>(std::move(assignment));
        },
        {.max_jobs = max_jobs});

    control::HttpServerOptions options;
    options.bind_address = bind;
    options.port = port;
    options.worker_threads = std::min<std::size_t>(max_jobs + 1, 16);
    options.max_pending_requests = 128;
    options.max_header_bytes = 32u * 1024u;
    options.max_body_bytes = 1u * 1024u * 1024u;
    control::HttpServer server(options);
    server.set_handler([&agent](const control::HttpRequest& request) {
        return handle_request(agent, request);
    });
    if (!server.start()) {
        std::cerr << "transcoder_agent could not listen on " << bind << ':' << port << '\n';
        return 1;
    }

    std::cout << "transcoder_agent listening on " << bind << ':' << server.bound_port()
              << " (max jobs " << max_jobs << ")\n";
    while (stop_requested == 0) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    server.stop();
    agent.stop_all();
    return 0;
}
