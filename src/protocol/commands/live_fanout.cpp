#include "rtmp_server/protocol/commands/live_fanout.hpp"

#include "rtmp_server/protocol/media/media_ingest.hpp"

namespace rtmp_server::protocol::commands {

using media::AacPacketType;
using media::AudioCodec;
using media::AvcPacketType;
using media::VideoCodec;
using media::VideoFrameType;

namespace {

bool classify_is_video_keyframe(const SharedMediaFrame& frame) {
    auto info = media::classify_video_tag(frame.payload.view());
    if (!info) return false;
    const bool key_type =
        info->frame_type == VideoFrameType::KeyFrame || info->frame_type == VideoFrameType::GeneratedKeyFrame;
    // AVC sequence headers conventionally carry frame-type=keyframe, but
    // they are decoder configuration, not a random-access media frame. They
    // must never release a viewer waiting for the next decodable GOP.
    return key_type && (info->codec != VideoCodec::Avc || info->avc_packet_type == AvcPacketType::Nalu);
}

bool classify_is_video_sequence_header(const SharedMediaFrame& frame) {
    auto info = media::classify_video_tag(frame.payload.view());
    return info && info->codec == VideoCodec::Avc && info->avc_packet_type == AvcPacketType::SequenceHeader;
}

bool classify_is_audio_sequence_header(const SharedMediaFrame& frame) {
    auto info = media::classify_audio_tag(frame.payload.view());
    return info && info->codec == AudioCodec::Aac && info->aac_packet_type == AacPacketType::SequenceHeader;
}

} // namespace

std::shared_ptr<LiveFanout::StreamState> LiveFanout::state_for(StreamId id) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto it = streams_.find(id.raw());
    if (it == streams_.end()) {
        it = streams_.emplace(id.raw(), std::make_shared<StreamState>(gop_limits_)).first;
    }
    return it->second;
}

std::vector<LiveFanout::PendingDelivery> LiveFanout::dispatch_locked(StreamState& state, const SharedMediaFrame& frame,
                                                                      bool is_video, bool is_audio, bool is_keyframe,
                                                                      StreamId stream_id) {
    std::vector<PendingDelivery> deliveries;
    std::vector<std::uint64_t> evict_ids;
    deliveries.reserve(state.subscribers.size());

    PendingDelivery::Kind kind = is_video   ? PendingDelivery::Kind::Video
                                  : is_audio ? PendingDelivery::Kind::Audio
                                             : PendingDelivery::Kind::Metadata;

    for (auto& [id, sub] : state.subscribers) {
        auto decision = sub.queue.offer(0, frame.payload.size(), is_video, is_keyframe);
        switch (decision) {
            case ViewerQueue::Decision::Deliver:
                deliveries.push_back(PendingDelivery{id, sub.sink, kind, std::nullopt, std::nullopt, &frame});
                break;
            case ViewerQueue::Decision::DeliverResumed: {
                // This viewer was stalled in WaitingForKeyframe and just got
                // the keyframe that lets it resume: a recovery, not a drop.
                if (metrics_ != nullptr) metrics_->increment(observability::MetricId::SlowViewerRecoveries);
                PendingDelivery delivery{id, sub.sink, kind, std::nullopt, std::nullopt, &frame};
                if (state.video_sequence_header) delivery.resend_video_seq_header = state.video_sequence_header;
                if (state.audio_sequence_header) delivery.resend_audio_seq_header = state.audio_sequence_header;
                deliveries.push_back(std::move(delivery));
                break;
            }
            case ViewerQueue::Decision::DropAndWait:
                // Counted per (frame, viewer) pair: 100 backed-up viewers
                // dropping the same frame is 100 dropped deliveries, which is
                // what an operator needs to see. These are plain atomic adds,
                // safe to do while state.mutex is held (no reentrancy, no
                // subscriber callback) — same justification as
                // subscription_hook_ below.
                // Metadata (is_video == is_audio == false) is deliberately
                // counted under neither: it is not a media frame and folding
                // it into dropped_audio_frames would mislead.
                if (metrics_ != nullptr && (is_video || is_audio)) {
                    metrics_->increment(is_video ? observability::MetricId::DroppedVideoFrames
                                                 : observability::MetricId::DroppedAudioFrames);
                }
                break;
            case ViewerQueue::Decision::Evict:
                evict_ids.push_back(id);
                break;
        }
    }

    for (auto id : evict_ids) {
        auto it = state.subscribers.find(id);
        if (it == state.subscribers.end()) continue;
        deliveries.push_back(PendingDelivery{id, it->second.sink, PendingDelivery::Kind::Evict, std::nullopt,
                                              std::nullopt, nullptr});
        state.subscribers.erase(it);
        if (metrics_ != nullptr) {
            metrics_->increment(observability::MetricId::SlowViewerEvictions);
            metrics_->increment(observability::MetricId::ViewerDisconnects);
            metrics_->add(observability::MetricId::ActiveViewers, -1);
        }
        if (subscription_hook_) subscription_hook_(stream_id, -1);
    }

    return deliveries;
}

void LiveFanout::run_deliveries(StreamId stream_id, const std::shared_ptr<StreamState>& state,
                                std::vector<PendingDelivery> deliveries) {
    // Bytes actually handed to viewer sinks. Accumulated locally and folded
    // into the counter once, rather than one atomic add per viewer per
    // frame — at 1,000 viewers that is the difference between 1 and 1,000
    // contended RMWs on the same cache line per media frame.
    std::uint64_t egress_bytes = 0;
    struct DeliveryResult {
        std::uint64_t subscriber_id;
        PlaybackSink* sink;
        PendingDelivery::Kind kind;
        bool delivered;
    };
    std::vector<DeliveryResult> results;
    results.reserve(deliveries.size());

    for (auto& delivery : deliveries) {
        if (delivery.resend_video_seq_header) {
            if (delivery.sink->on_video(*delivery.resend_video_seq_header)) {
                egress_bytes += delivery.resend_video_seq_header->payload.size();
            }
        }
        if (delivery.resend_audio_seq_header) {
            if (delivery.sink->on_audio(*delivery.resend_audio_seq_header)) {
                egress_bytes += delivery.resend_audio_seq_header->payload.size();
            }
        }
        bool delivered = false;
        switch (delivery.kind) {
            case PendingDelivery::Kind::Audio:
                delivered = delivery.sink->on_audio(*delivery.frame);
                break;
            case PendingDelivery::Kind::Video:
                delivered = delivery.sink->on_video(*delivery.frame);
                break;
            case PendingDelivery::Kind::Metadata:
                delivered = delivery.sink->on_metadata(*delivery.frame);
                break;
            case PendingDelivery::Kind::Evict:
                delivery.sink->on_slow_client_evicted();
                break;
        }
        if (delivery.kind != PendingDelivery::Kind::Evict) {
            if (delivered) egress_bytes += delivery.frame->payload.size();
            results.push_back(DeliveryResult{delivery.subscriber_id, delivery.sink, delivery.kind, delivered});
        }
    }

    // PlaybackSink::false is the connection-local backpressure signal. Fold
    // it back into subscriber lifecycle after callbacks return, while
    // preserving the rule that user code never runs under a fanout mutex.
    std::vector<PlaybackSink*> transport_evictions;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        for (const auto& result : results) {
            auto it = state->subscribers.find(result.subscriber_id);
            // A callback may unsubscribe itself. Subscriber IDs are
            // generation-unique, and checking the pointer also prevents a
            // stale delivery result touching a replacement.
            if (it == state->subscribers.end() || it->second.sink != result.sink) continue;
            if (result.delivered) {
                it->second.consecutive_transport_drops = 0;
                continue;
            }

            ++it->second.consecutive_transport_drops;
            if (metrics_ != nullptr &&
                (result.kind == PendingDelivery::Kind::Video || result.kind == PendingDelivery::Kind::Audio)) {
                metrics_->increment(result.kind == PendingDelivery::Kind::Video
                                        ? observability::MetricId::DroppedVideoFrames
                                        : observability::MetricId::DroppedAudioFrames);
            }
            if (it->second.consecutive_transport_drops > max_frames_waiting_for_keyframe_) {
                transport_evictions.push_back(it->second.sink);
                state->subscribers.erase(it);
            }
        }
    }

    for (auto* sink : transport_evictions) {
        if (metrics_ != nullptr) {
            metrics_->increment(observability::MetricId::SlowViewerEvictions);
            metrics_->increment(observability::MetricId::ViewerDisconnects);
            metrics_->add(observability::MetricId::ActiveViewers, -1);
        }
        if (subscription_hook_) subscription_hook_(stream_id, -1);
        sink->on_slow_client_evicted();
    }

    if (metrics_ != nullptr && egress_bytes > 0) {
        metrics_->increment(observability::MetricId::EgressBytesTotal, egress_bytes);
    }
}

void LiveFanout::on_video(StreamId stream_id, const SharedMediaFrame& frame, bool is_replayed) {
    auto state = state_for(stream_id);
    std::vector<PendingDelivery> deliveries;
    bool is_keyframe = false;
    bool is_sequence_header = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (classify_is_video_sequence_header(frame)) {
            state->video_sequence_header = frame;
            is_sequence_header = true;
        } else {
            is_keyframe = classify_is_video_keyframe(frame);
            if (is_keyframe) {
                state->gop_cache.begin_new_gop(frame);
            } else {
                state->gop_cache.push(frame); // no-op until the first keyframe arrives
            }
        }
        deliveries = dispatch_locked(*state, frame, /*is_video=*/true, /*is_audio=*/false, is_keyframe, stream_id);
    }
    run_deliveries(stream_id, state, std::move(deliveries));
    if (!is_replayed && forward_hook_)
        forward_hook_(stream_id, frame, /*is_video=*/true, /*is_audio=*/false, /*is_sticky=*/is_sequence_header,
                      is_keyframe);
}

void LiveFanout::on_audio(StreamId stream_id, const SharedMediaFrame& frame, bool is_replayed) {
    auto state = state_for(stream_id);
    std::vector<PendingDelivery> deliveries;
    bool is_sequence_header = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (classify_is_audio_sequence_header(frame)) {
            state->audio_sequence_header = frame;
            is_sequence_header = true;
        } else {
            state->gop_cache.push(frame); // no-op until a video keyframe has started a GOP
        }
        deliveries = dispatch_locked(*state, frame, /*is_video=*/false, /*is_audio=*/true, /*is_keyframe=*/false,
                                     stream_id);
    }
    run_deliveries(stream_id, state, std::move(deliveries));
    if (!is_replayed && forward_hook_)
        forward_hook_(stream_id, frame, /*is_video=*/false, /*is_audio=*/true, /*is_sticky=*/is_sequence_header,
                      /*is_keyframe=*/false);
}

void LiveFanout::on_metadata(StreamId stream_id, const SharedMediaFrame& frame, bool is_replayed) {
    auto state = state_for(stream_id);
    std::vector<PendingDelivery> deliveries;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->metadata = frame;
        deliveries =
            dispatch_locked(*state, frame, /*is_video=*/false, /*is_audio=*/false, /*is_keyframe=*/false, stream_id);
    }
    run_deliveries(stream_id, state, std::move(deliveries));
    // Metadata (onMetadata) is always sticky init state a late subscriber needs.
    if (!is_replayed && forward_hook_)
        forward_hook_(stream_id, frame, /*is_video=*/false, /*is_audio=*/false, /*is_sticky=*/true,
                      /*is_keyframe=*/false);
}

void LiveFanout::subscribe(StreamId stream_id, SubscriberId subscriber_id, PlaybackSink* sink,
                            std::string_view client_ip) {
    auto state = state_for(stream_id);
    std::vector<SharedMediaFrame> startup_metadata_and_headers;
    std::vector<SharedMediaFrame> startup_gop;
    bool have_video_header = false;
    bool have_audio_header = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->subscribers.emplace(
            subscriber_id.raw(),
            Subscriber{sink, ViewerQueue(queue_limits_, max_frames_waiting_for_keyframe_), 0,
                       std::string(client_ip)});

        if (state->metadata) startup_metadata_and_headers.push_back(*state->metadata);
        if (state->video_sequence_header) {
            startup_metadata_and_headers.push_back(*state->video_sequence_header);
            have_video_header = true;
        }
        if (state->audio_sequence_header) {
            startup_metadata_and_headers.push_back(*state->audio_sequence_header);
            have_audio_header = true;
        }
        for (const auto& frame : state->gop_cache.frames()) startup_gop.push_back(frame);
    }
    (void)have_video_header;
    (void)have_audio_header;

    // Deliver outside the lock: metadata + sequence headers first, then the
    // cached GOP (which always starts at a keyframe by GopCache's
    // invariant), in the order a fresh decoder needs them.
    for (const auto& frame : startup_metadata_and_headers) {
        if (classify_is_video_sequence_header(frame)) {
            sink->on_video(frame);
        } else if (classify_is_audio_sequence_header(frame)) {
            sink->on_audio(frame);
        } else {
            sink->on_metadata(frame);
        }
    }
    for (const auto& frame : startup_gop) {
        if (frame.message_type_id == static_cast<std::uint8_t>(chunk::MessageTypeId::Video)) {
            sink->on_video(frame);
        } else {
            sink->on_audio(frame);
        }
    }

    if (metrics_ != nullptr) metrics_->add(observability::MetricId::ActiveViewers, +1);
    if (subscription_hook_) subscription_hook_(stream_id, +1);
}

void LiveFanout::unsubscribe(StreamId stream_id, SubscriberId subscriber_id) {
    std::shared_ptr<StreamState> state;
    {
        std::lock_guard<std::mutex> structural_lock(streams_mutex_);
        auto it = streams_.find(stream_id.raw());
        if (it == streams_.end()) return;
        state = it->second;
    }

    std::size_t erased = 0;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        erased = state->subscribers.erase(subscriber_id.raw()); // no-op on a missing key: idempotent
    }
    if (erased > 0) {
        if (metrics_ != nullptr) {
            metrics_->add(observability::MetricId::ActiveViewers, -1);
            metrics_->increment(observability::MetricId::ViewerDisconnects);
        }
        if (subscription_hook_) subscription_hook_(stream_id, -1);
    }
}

void LiveFanout::publisher_stopped(StreamId stream_id, bool is_replayed) {
    std::shared_ptr<StreamState> removed;
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        auto it = streams_.find(stream_id.raw());
        if (it == streams_.end()) return;
        removed = std::move(it->second);
        streams_.erase(it);
    }

    // Media delivery may already hold a shared snapshot of this state. Clear
    // the subscriber table under its lock, then notify outside all locks.
    std::vector<PlaybackSink*> sinks;
    {
        std::lock_guard<std::mutex> lock(removed->mutex);
        sinks.reserve(removed->subscribers.size());
        for (auto& [id, sub] : removed->subscribers) sinks.push_back(sub.sink);
        removed->subscribers.clear();
    }
    for (auto* sink : sinks) sink->on_publisher_stopped();

    if (metrics_ != nullptr) {
        metrics_->increment(observability::MetricId::PublisherDisconnects);
        // Every subscriber of this stream is gone too; they were counted in
        // active_viewers at subscribe() time, so unwind them all here rather
        // than leaving the gauge permanently inflated after a publisher ends.
        const auto viewers = static_cast<std::int64_t>(sinks.size());
        if (viewers > 0) {
            metrics_->add(observability::MetricId::ActiveViewers, -viewers);
            metrics_->increment(observability::MetricId::ViewerDisconnects, static_cast<std::uint64_t>(viewers));
        }
    }

    if (!is_replayed && stream_end_hook_) stream_end_hook_(stream_id);
}

void LiveFanout::sample_gauges() {
    if (metrics_ == nullptr) return;

    // Snapshot shared ownership under the structural lock, then sample each
    // stream independently. Publishing/stopping another stream cannot stall
    // for the duration of this O(streams + viewers) metrics walk.
    std::uint64_t gop_bytes = 0;
    std::uint64_t gop_packets = 0;
    std::uint64_t queue_bytes = 0;
    std::uint64_t queue_packets = 0;
    std::vector<std::int64_t> viewers_per_stream;

    std::vector<std::shared_ptr<StreamState>> states;
    {
        std::lock_guard<std::mutex> structural_lock(streams_mutex_);
        states.reserve(streams_.size());
        for (const auto& [raw_id, node] : streams_) states.push_back(node);
    }
    viewers_per_stream.reserve(states.size());
    for (const auto& node : states) {
        std::lock_guard<std::mutex> lock(node->mutex);
        gop_bytes += node->gop_cache.total_bytes();
        gop_packets += node->gop_cache.packet_count();
        for (const auto& [sub_id, sub] : node->subscribers) {
            queue_bytes += sub.queue.bytes();
            queue_packets += sub.queue.packet_count();
        }
        viewers_per_stream.push_back(static_cast<std::int64_t>(node->subscribers.size()));
    }

    metrics_->set(observability::MetricId::GopCacheBytes, static_cast<std::int64_t>(gop_bytes));
    metrics_->set(observability::MetricId::GopCachePackets, static_cast<std::int64_t>(gop_packets));
    metrics_->set(observability::MetricId::OutboundQueueBytes, static_cast<std::int64_t>(queue_bytes));
    metrics_->set(observability::MetricId::OutboundQueuePackets, static_cast<std::int64_t>(queue_packets));

    // Aggregate, never per-stream-labelled: stream count is unbounded.
    for (const std::int64_t viewers : viewers_per_stream) metrics_->observe_viewers_per_stream(viewers);
    metrics_->commit_viewers_per_stream();
}

std::size_t LiveFanout::subscriber_count(StreamId stream_id) const {
    std::shared_ptr<StreamState> state;
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        auto it = streams_.find(stream_id.raw());
        if (it == streams_.end()) return 0;
        state = it->second;
    }
    // Reading subscribers.size() without the per-stream mutex here would
    // race with a concurrent subscribe()/unsubscribe(); acquire it too.
    // streams_mutex_ is already held, but that only protects the map
    // structure, not StreamState's own fields.
    std::lock_guard<std::mutex> stream_lock(state->mutex);
    return state->subscribers.size();
}

std::size_t LiveFanout::unique_viewer_count(StreamId stream_id) const {
    std::shared_ptr<StreamState> state;
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        auto it = streams_.find(stream_id.raw());
        if (it == streams_.end()) return 0;
        state = it->second;
    }
    std::lock_guard<std::mutex> stream_lock(state->mutex);
    std::unordered_set<std::string> seen_ips;
    std::size_t count = 0;
    for (const auto& [id, subscriber] : state->subscribers) {
        if (subscriber.client_ip.empty()) {
            ++count; // no IP recorded — can't dedup, count it on its own
        } else if (seen_ips.insert(subscriber.client_ip).second) {
            ++count; // first time this IP has been seen for this stream
        }
    }
    return count;
}

} // namespace rtmp_server::protocol::commands
