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
    return info->frame_type == VideoFrameType::KeyFrame || info->frame_type == VideoFrameType::GeneratedKeyFrame;
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

LiveFanout::StreamState& LiveFanout::state_for(StreamId id) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto it = streams_.find(id.raw());
    if (it == streams_.end()) {
        it = streams_.emplace(id.raw(), std::make_unique<StreamState>(gop_limits_)).first;
    }
    return *it->second;
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
                deliveries.push_back(PendingDelivery{sub.sink, kind, std::nullopt, std::nullopt, frame});
                break;
            case ViewerQueue::Decision::DeliverResumed: {
                // This viewer was stalled in WaitingForKeyframe and just got
                // the keyframe that lets it resume: a recovery, not a drop.
                if (metrics_ != nullptr) metrics_->increment(observability::MetricId::SlowViewerRecoveries);
                PendingDelivery delivery{sub.sink, kind, std::nullopt, std::nullopt, frame};
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
        deliveries.push_back(PendingDelivery{it->second.sink, PendingDelivery::Kind::Evict, std::nullopt,
                                              std::nullopt, SharedMediaFrame{}});
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

void LiveFanout::run_deliveries(std::vector<PendingDelivery> deliveries) {
    // Bytes actually handed to viewer sinks. Accumulated locally and folded
    // into the counter once, rather than one atomic add per viewer per
    // frame — at 1,000 viewers that is the difference between 1 and 1,000
    // contended RMWs on the same cache line per media frame.
    std::uint64_t egress_bytes = 0;

    for (auto& delivery : deliveries) {
        if (delivery.resend_video_seq_header) {
            egress_bytes += delivery.resend_video_seq_header->payload.size();
            delivery.sink->on_video(*delivery.resend_video_seq_header);
        }
        if (delivery.resend_audio_seq_header) {
            egress_bytes += delivery.resend_audio_seq_header->payload.size();
            delivery.sink->on_audio(*delivery.resend_audio_seq_header);
        }
        if (delivery.kind != PendingDelivery::Kind::Evict) {
            egress_bytes += delivery.frame.payload.size();
        }
        switch (delivery.kind) {
            case PendingDelivery::Kind::Audio:
                delivery.sink->on_audio(delivery.frame);
                break;
            case PendingDelivery::Kind::Video:
                delivery.sink->on_video(delivery.frame);
                break;
            case PendingDelivery::Kind::Metadata:
                delivery.sink->on_metadata(delivery.frame);
                break;
            case PendingDelivery::Kind::Evict:
                delivery.sink->on_slow_client_evicted();
                break;
        }
    }

    if (metrics_ != nullptr && egress_bytes > 0) {
        metrics_->increment(observability::MetricId::EgressBytesTotal, egress_bytes);
    }
}

void LiveFanout::on_video(StreamId stream_id, const SharedMediaFrame& frame, bool is_replayed) {
    StreamState& state = state_for(stream_id);
    std::vector<PendingDelivery> deliveries;
    bool is_keyframe = false;
    bool is_sequence_header = false;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (classify_is_video_sequence_header(frame)) {
            state.video_sequence_header = frame;
            is_sequence_header = true;
        } else {
            is_keyframe = classify_is_video_keyframe(frame);
            if (is_keyframe) {
                state.gop_cache.begin_new_gop(frame);
            } else {
                state.gop_cache.push(frame); // no-op until the first keyframe arrives
            }
        }
        deliveries = dispatch_locked(state, frame, /*is_video=*/true, /*is_audio=*/false, is_keyframe, stream_id);
    }
    run_deliveries(std::move(deliveries));
    if (!is_replayed && forward_hook_)
        forward_hook_(stream_id, frame, /*is_video=*/true, /*is_audio=*/false, /*is_sticky=*/is_sequence_header);
}

void LiveFanout::on_audio(StreamId stream_id, const SharedMediaFrame& frame, bool is_replayed) {
    StreamState& state = state_for(stream_id);
    std::vector<PendingDelivery> deliveries;
    bool is_sequence_header = false;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        if (classify_is_audio_sequence_header(frame)) {
            state.audio_sequence_header = frame;
            is_sequence_header = true;
        } else {
            state.gop_cache.push(frame); // no-op until a video keyframe has started a GOP
        }
        deliveries = dispatch_locked(state, frame, /*is_video=*/false, /*is_audio=*/true, /*is_keyframe=*/false, stream_id);
    }
    run_deliveries(std::move(deliveries));
    if (!is_replayed && forward_hook_)
        forward_hook_(stream_id, frame, /*is_video=*/false, /*is_audio=*/true, /*is_sticky=*/is_sequence_header);
}

void LiveFanout::on_metadata(StreamId stream_id, const SharedMediaFrame& frame, bool is_replayed) {
    StreamState& state = state_for(stream_id);
    std::vector<PendingDelivery> deliveries;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.metadata = frame;
        deliveries = dispatch_locked(state, frame, /*is_video=*/false, /*is_audio=*/false, /*is_keyframe=*/false, stream_id);
    }
    run_deliveries(std::move(deliveries));
    // Metadata (onMetadata) is always sticky init state a late subscriber needs.
    if (!is_replayed && forward_hook_)
        forward_hook_(stream_id, frame, /*is_video=*/false, /*is_audio=*/false, /*is_sticky=*/true);
}

void LiveFanout::subscribe(StreamId stream_id, SubscriberId subscriber_id, PlaybackSink* sink) {
    StreamState& state = state_for(stream_id);
    std::vector<SharedMediaFrame> startup_metadata_and_headers;
    std::vector<SharedMediaFrame> startup_gop;
    bool have_video_header = false;
    bool have_audio_header = false;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        state.subscribers.emplace(subscriber_id.raw(), Subscriber{sink, ViewerQueue(queue_limits_, max_frames_waiting_for_keyframe_)});

        if (state.metadata) startup_metadata_and_headers.push_back(*state.metadata);
        if (state.video_sequence_header) {
            startup_metadata_and_headers.push_back(*state.video_sequence_header);
            have_video_header = true;
        }
        if (state.audio_sequence_header) {
            startup_metadata_and_headers.push_back(*state.audio_sequence_header);
            have_audio_header = true;
        }
        for (const auto& frame : state.gop_cache.frames()) startup_gop.push_back(frame);
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
    std::unique_lock<std::mutex> structural_lock(streams_mutex_);
    auto it = streams_.find(stream_id.raw());
    if (it == streams_.end()) return;
    StreamState& state = *it->second;
    structural_lock.unlock();

    std::size_t erased = 0;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        erased = state.subscribers.erase(subscriber_id.raw()); // no-op on a missing key: idempotent
    }
    if (erased > 0) {
        if (metrics_ != nullptr) {
            metrics_->add(observability::MetricId::ActiveViewers, -1);
            metrics_->increment(observability::MetricId::ViewerDisconnects);
        }
        if (subscription_hook_) subscription_hook_(stream_id, -1);
    }
}

void LiveFanout::publisher_stopped(StreamId stream_id) {
    std::unique_ptr<StreamState> removed;
    {
        std::lock_guard<std::mutex> lock(streams_mutex_);
        auto it = streams_.find(stream_id.raw());
        if (it == streams_.end()) return;
        removed = std::move(it->second);
        streams_.erase(it);
    }

    // `removed` is no longer reachable from streams_, so no other thread can
    // observe or lock it concurrently — safe to read/iterate its subscribers
    // without removed->mutex, and safe to call back into sinks here (outside
    // any LiveFanout-owned lock).
    std::vector<PlaybackSink*> sinks;
    sinks.reserve(removed->subscribers.size());
    for (auto& [id, sub] : removed->subscribers) sinks.push_back(sub.sink);
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

    if (stream_end_hook_) stream_end_hook_(stream_id);
}

void LiveFanout::sample_gauges() {
    if (metrics_ == nullptr) return;

    // Snapshot the node pointers under the structural lock only, then release
    // it before touching any per-stream mutex — same lock-ordering discipline
    // the media path uses (streams_mutex_ is never held across state.mutex).
    // StreamState nodes are held by unique_ptr and are address-stable, but a
    // concurrent publisher_stopped() could destroy one after we drop
    // streams_mutex_, so copy the owning shared state instead: here we take
    // the simpler, provably-safe route of doing the whole walk under
    // streams_mutex_, which blocks only structural changes (publish/stop),
    // not the media hot path.
    std::uint64_t gop_bytes = 0;
    std::uint64_t gop_packets = 0;
    std::uint64_t queue_bytes = 0;
    std::uint64_t queue_packets = 0;
    std::vector<std::int64_t> viewers_per_stream;

    {
        std::lock_guard<std::mutex> structural_lock(streams_mutex_);
        viewers_per_stream.reserve(streams_.size());
        for (const auto& [raw_id, node] : streams_) {
            std::lock_guard<std::mutex> lock(node->mutex);
            gop_bytes += node->gop_cache.total_bytes();
            gop_packets += node->gop_cache.packet_count();
            for (const auto& [sub_id, sub] : node->subscribers) {
                queue_bytes += sub.queue.bytes();
                queue_packets += sub.queue.packet_count();
            }
            viewers_per_stream.push_back(static_cast<std::int64_t>(node->subscribers.size()));
        }
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
    std::lock_guard<std::mutex> lock(streams_mutex_);
    auto it = streams_.find(stream_id.raw());
    if (it == streams_.end()) return 0;
    // Reading subscribers.size() without the per-stream mutex here would
    // race with a concurrent subscribe()/unsubscribe(); acquire it too.
    // streams_mutex_ is already held, but that only protects the map
    // structure, not StreamState's own fields.
    std::lock_guard<std::mutex> stream_lock(it->second->mutex);
    return it->second->subscribers.size();
}

} // namespace rtmp_server::protocol::commands
