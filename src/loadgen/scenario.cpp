#include "rtmp_server/loadgen/scenario.hpp"

#include <poll.h>

#include <algorithm>
#include <format>
#include <set>
#include <thread>

namespace rtmp_server::loadgen {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t percentile(std::vector<std::uint64_t>& values, double fraction) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    auto index = static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1));
    index = std::min(index, values.size() - 1);
    return values[index];
}

// One client plus the per-run scheduling state the driver needs.
struct Slot {
    std::unique_ptr<RtmpClient> client;
    Clock::time_point start_at{};
    bool started = false;
    bool is_publisher = false;
    bool abrupt_disconnect = false;
    bool disconnected = false;
    Clock::time_point last_reconnect{};
};

} // namespace

std::string ScenarioReport::to_text() const {
    std::string out;
    out += std::format("--- load scenario report ---\n");
    out += std::format("duration                 : {} ms\n", elapsed.count());
    out += std::format("publishers               : {} requested, {} reached publish\n", publishers_requested,
                       publishers_streaming);
    out += std::format("viewers                  : {} requested, {} reached play\n", viewers_requested,
                       viewers_streaming);
    out += std::format("clients failed           : {}\n", clients_failed);
    out += std::format("bytes sent (to server)   : {}\n", total_bytes_sent);
    out += std::format("bytes received (viewers) : {}\n", total_bytes_received);
    out += std::format("media msgs sent/received : {} / {}\n", media_messages_sent, media_messages_received);
    out += std::format("keyframes received       : {}\n", keyframes_received);
    out += std::format("payloads verified/corrupt: {} / {}\n", payloads_verified, payloads_corrupt);
    out += std::format("partial writes           : {}\n", partial_writes);
    out += std::format("publisher reconnects     : {}\n", publisher_reconnects);
    out += std::format("abrupt disconnects       : {}\n", abrupt_disconnects);
    out += std::format("ingress bitrate          : {:.0f} bps\n", ingress_bitrate_bps);
    out += std::format("egress bitrate (measured): {:.0f} bps\n", egress_bitrate_bps);
    out += std::format("tcp connect  p50/p99 (us): {} / {}\n", connect_latency_p50_us, connect_latency_p99_us);
    out += std::format("handshake    p50/p99 (us): {} / {}\n", handshake_latency_p50_us, handshake_latency_p99_us);
    out += std::format("play ack     p50/p99 (us): {} / {}\n", play_latency_p50_us, play_latency_p99_us);
    out += std::format("first media  p50/p99 (us): {} / {}\n", first_media_latency_p50_us,
                       first_media_latency_p99_us);
    if (!failure_reasons.empty()) {
        out += "failure reasons          :\n";
        for (const auto& reason : failure_reasons) out += std::format("  - {}\n", reason);
    }
    return out;
}

ScenarioReport run_scenario(const ScenarioConfig& config) {
    ScenarioReport report;
    report.publishers_requested = config.publishers;
    report.viewers_requested = config.publishers * config.viewers_per_publisher;
    if ((!config.publish_key.empty() || !config.playback_name.empty()) &&
        (config.publish_key.empty() || config.playback_name.empty() ||
         config.publishers != 1)) {
        report.clients_failed = config.publishers + report.viewers_requested;
        report.failure_reasons.push_back(
            "publish name and playback name must both be set and require publishers=1");
        return report;
    }

    const auto run_started = Clock::now();
    const auto deadline = run_started + config.duration;
    const auto halfway = run_started + config.duration / 2;

    std::vector<Slot> slots;
    slots.reserve(static_cast<std::size_t>(config.publishers) * (1 + config.viewers_per_publisher));

    const auto total_clients =
        static_cast<std::size_t>(config.publishers) * (1u + config.viewers_per_publisher);
    std::size_t created = 0;

    const auto ramp_offset = [&](std::size_t index) {
        if (config.ramp_up.count() <= 0 || total_clients <= 1) return std::chrono::milliseconds{0};
        // Spread evenly across the ramp window.
        const auto span = static_cast<double>(config.ramp_up.count());
        const auto position = static_cast<double>(index) / static_cast<double>(total_clients - 1);
        return std::chrono::milliseconds{static_cast<std::int64_t>(span * position)};
    };

    std::uint32_t slow_viewers_assigned = 0;
    std::uint32_t abrupt_assigned = 0;
    const auto slow_target =
        static_cast<std::uint32_t>(static_cast<double>(report.viewers_requested) * config.slow_viewer_fraction);
    const auto abrupt_target =
        static_cast<std::uint32_t>(static_cast<double>(report.viewers_requested) * config.abrupt_disconnect_fraction);

    for (std::uint32_t p = 0; p < config.publishers; ++p) {
        const std::string key = config.publish_key.empty()
                                    ? config.stream_key_prefix + std::to_string(p)
                                    : config.publish_key;
        const std::string viewer_name =
            config.playback_name.empty() ? key : config.playback_name;

        // Publisher first: viewers ramp in behind it so there is a stream to
        // subscribe to, matching how a real audience arrives.
        {
            RtmpClient::Config cc;
            cc.host = config.host;
            cc.port = config.port;
            cc.application = config.application;
            cc.stream_key = key;
            cc.role = RtmpClient::Role::Publisher;
            cc.media = config.media;

            Slot slot;
            slot.client = std::make_unique<RtmpClient>(std::move(cc));
            slot.start_at = run_started + ramp_offset(created);
            slot.is_publisher = true;
            slots.push_back(std::move(slot));
            ++created;
        }

        for (std::uint32_t v = 0; v < config.viewers_per_publisher; ++v) {
            RtmpClient::Config cc;
            cc.host = config.host;
            cc.port = config.port;
            cc.application = config.application;
            cc.stream_key = viewer_name;
            cc.role = RtmpClient::Role::Viewer;
            if (slow_viewers_assigned < slow_target) {
                cc.read_budget_per_tick = config.slow_viewer_read_budget;
                ++slow_viewers_assigned;
            }

            Slot slot;
            slot.client = std::make_unique<RtmpClient>(std::move(cc));
            slot.start_at = run_started + ramp_offset(created);
            if (abrupt_assigned < abrupt_target) {
                slot.abrupt_disconnect = true;
                ++abrupt_assigned;
            }
            slots.push_back(std::move(slot));
            ++created;
        }
    }

    std::vector<pollfd> poll_set;
    poll_set.reserve(slots.size());

    while (Clock::now() < deadline) {
        const auto now = Clock::now();

        // --- ramp-up: start whatever is due ---------------------------------
        for (auto& slot : slots) {
            if (slot.started || now < slot.start_at) continue;
            slot.started = true;
            if (!slot.client->start().ok()) ++report.clients_failed;
        }

        // --- abrupt disconnect at the halfway mark ---------------------------
        if (now >= halfway) {
            for (auto& slot : slots) {
                if (!slot.abrupt_disconnect || slot.disconnected || !slot.started) continue;
                if (slot.client->finished()) continue;
                slot.client->abort_connection();
                slot.disconnected = true;
                ++report.abrupt_disconnects;
            }
        }

        // --- publisher reconnect ---------------------------------------------
        if (config.publisher_reconnect_interval.count() > 0) {
            for (auto& slot : slots) {
                if (!slot.is_publisher || !slot.started) continue;
                const auto since = slot.last_reconnect == Clock::time_point{} ? slot.start_at : slot.last_reconnect;
                if (now - since < config.publisher_reconnect_interval) continue;
                slot.last_reconnect = now;
                if (slot.client->reconnect().ok()) ++report.publisher_reconnects;
            }
        }

        // --- build the poll set ------------------------------------------------
        poll_set.clear();
        std::vector<std::size_t> poll_owner;
        poll_owner.reserve(slots.size());
        for (std::size_t i = 0; i < slots.size(); ++i) {
            auto& slot = slots[i];
            if (!slot.started || slot.client->finished() || slot.client->fd() < 0) continue;
            pollfd pfd{};
            pfd.fd = slot.client->fd();
            pfd.events = POLLIN;
            if (slot.client->wants_write()) pfd.events |= POLLOUT;
            poll_set.push_back(pfd);
            poll_owner.push_back(i);
        }

        if (!poll_set.empty()) {
            const int rc = ::poll(poll_set.data(), static_cast<nfds_t>(poll_set.size()),
                                  static_cast<int>(config.tick.count()));
            if (rc > 0) {
                for (std::size_t i = 0; i < poll_set.size(); ++i) {
                    auto& slot = slots[poll_owner[i]];
                    const short revents = poll_set[i].revents;
                    if (revents == 0) continue;
                    // Writability first: it may complete the TCP connect, and
                    // draining before reading keeps the publisher's pipeline
                    // moving.
                    if ((revents & (POLLOUT | POLLERR | POLLHUP)) != 0) slot.client->on_writable();
                    if (!slot.client->finished() && (revents & (POLLIN | POLLHUP)) != 0) slot.client->on_readable();
                }
            }
        } else {
            // Nothing to poll yet (still ramping): sleep one tick rather than
            // spin. This is a load generator, not an event-loop worker, so a
            // bounded sleep here is correct, not a rule violation.
            std::this_thread::sleep_for(config.tick);
        }

        // --- media generation ---------------------------------------------------
        const auto stream_time =
            static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - run_started)
                                           .count());
        for (auto& slot : slots) {
            if (!slot.is_publisher || !slot.started || slot.client->finished()) continue;
            slot.client->pump_media(stream_time);
        }
    }

    // --- collect ----------------------------------------------------------------
    report.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - run_started);

    std::vector<std::uint64_t> connect_us;
    std::vector<std::uint64_t> handshake_us;
    std::vector<std::uint64_t> play_us;
    std::vector<std::uint64_t> first_media_us;
    std::set<std::string> reasons;

    for (auto& slot : slots) {
        if (!slot.started) continue;
        const auto& s = slot.client->stats();

        report.total_bytes_sent += s.bytes_sent;
        report.total_bytes_received += s.bytes_received;
        report.media_messages_sent += s.media_messages_sent;
        report.media_messages_received += s.media_messages_received;
        report.keyframes_received += s.keyframes_received;
        report.payloads_verified += s.payloads_verified;
        report.payloads_corrupt += s.payloads_corrupt;
        report.partial_writes += s.partial_writes;

        if (s.reached_streaming) {
            if (slot.is_publisher) {
                ++report.publishers_streaming;
            } else {
                ++report.viewers_streaming;
            }
        }
        if (!s.failure_reason.empty()) {
            ++report.clients_failed;
            if (reasons.size() < 10) reasons.insert(s.failure_reason);
        }

        if (s.tcp_connect_latency.count() > 0) {
            connect_us.push_back(static_cast<std::uint64_t>(s.tcp_connect_latency.count()));
        }
        if (s.handshake_latency.count() > 0) {
            handshake_us.push_back(static_cast<std::uint64_t>(s.handshake_latency.count()));
        }
        if (!slot.is_publisher && s.publish_or_play_latency.count() > 0) {
            play_us.push_back(static_cast<std::uint64_t>(s.publish_or_play_latency.count()));
        }
        if (!slot.is_publisher && s.first_media_latency.count() > 0) {
            first_media_us.push_back(static_cast<std::uint64_t>(s.first_media_latency.count()));
        }
    }

    report.failure_reasons.assign(reasons.begin(), reasons.end());

    report.connect_latency_p50_us = percentile(connect_us, 0.50);
    report.connect_latency_p99_us = percentile(connect_us, 0.99);
    report.handshake_latency_p50_us = percentile(handshake_us, 0.50);
    report.handshake_latency_p99_us = percentile(handshake_us, 0.99);
    report.play_latency_p50_us = percentile(play_us, 0.50);
    report.play_latency_p99_us = percentile(play_us, 0.99);
    report.first_media_latency_p50_us = percentile(first_media_us, 0.50);
    report.first_media_latency_p99_us = percentile(first_media_us, 0.99);

    const double seconds = static_cast<double>(report.elapsed.count()) / 1000.0;
    if (seconds > 0) {
        report.ingress_bitrate_bps = static_cast<double>(report.total_bytes_sent) * 8.0 / seconds;
        report.egress_bitrate_bps = static_cast<double>(report.total_bytes_received) * 8.0 / seconds;
    }

    // Close everything deterministically before returning, so the server sees
    // a clean teardown for the clients we did not deliberately abort.
    for (auto& slot : slots) {
        if (slot.started && !slot.client->finished()) slot.client->close_gracefully();
    }

    return report;
}

} // namespace rtmp_server::loadgen
