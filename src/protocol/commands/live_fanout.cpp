#include "rtmp_server/protocol/commands/live_fanout.hpp"

namespace rtmp_server::protocol::commands {

using chunk::MessageTypeId;
using chunk::RtmpMessage;

namespace {

// FLV video tag byte 0: high nibble = frame type (1 = keyframe), low nibble
// = codec ID (7 = AVC). Byte 1, present only for codec ID 7, is the
// AVCPacketType (0 = sequence header / AVCDecoderConfigurationRecord).
bool is_video_keyframe(const RtmpMessage& m) { return !m.payload.empty() && (std::to_integer<int>(m.payload[0]) >> 4) == 1; }

bool is_avc(const RtmpMessage& m) { return !m.payload.empty() && (std::to_integer<int>(m.payload[0]) & 0x0F) == 7; }

bool is_video_sequence_header(const RtmpMessage& m) {
    return is_avc(m) && m.payload.size() > 1 && std::to_integer<int>(m.payload[1]) == 0;
}

// FLV audio tag byte 0: high nibble = sound format (10 = AAC). Byte 1,
// present only for AAC, is the AACPacketType (0 = sequence header /
// AudioSpecificConfig).
bool is_aac(const RtmpMessage& m) { return !m.payload.empty() && (std::to_integer<int>(m.payload[0]) >> 4) == 10; }

bool is_audio_sequence_header(const RtmpMessage& m) {
    return is_aac(m) && m.payload.size() > 1 && std::to_integer<int>(m.payload[1]) == 0;
}

} // namespace

std::vector<PlaybackSink*> LiveFanout::dispatch_locked(StreamState& state, const RtmpMessage& message,
                                                        bool (PlaybackSink::*method)(const RtmpMessage&)) {
    std::vector<std::uint64_t> evict_ids;
    for (auto& [id, sub] : state.subscribers) {
        bool delivered = (sub.sink->*method)(message);
        if (delivered) {
            sub.consecutive_drops = 0;
        } else if (++sub.consecutive_drops >= max_consecutive_drops_) {
            evict_ids.push_back(id);
        }
    }

    std::vector<PlaybackSink*> evicted_sinks;
    evicted_sinks.reserve(evict_ids.size());
    for (auto id : evict_ids) {
        auto it = state.subscribers.find(id);
        if (it == state.subscribers.end()) continue;
        evicted_sinks.push_back(it->second.sink);
        state.subscribers.erase(it);
    }
    return evicted_sinks;
}

void LiveFanout::on_video(const std::string& stream_key, const RtmpMessage& message) {
    std::vector<PlaybackSink*> evicted;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& state = streams_[stream_key];
        if (is_video_sequence_header(message)) {
            state.video_sequence_header = message;
        } else {
            if (is_video_keyframe(message)) state.gop_cache.clear();
            if (!state.gop_cache.empty() || is_video_keyframe(message)) state.gop_cache.push_back(message);
        }
        evicted = dispatch_locked(state, message, &PlaybackSink::on_video);
    }
    for (auto* sink : evicted) sink->on_slow_client_evicted();
}

void LiveFanout::on_audio(const std::string& stream_key, const RtmpMessage& message) {
    std::vector<PlaybackSink*> evicted;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& state = streams_[stream_key];
        if (is_audio_sequence_header(message)) {
            state.audio_sequence_header = message;
        } else if (!state.gop_cache.empty()) {
            state.gop_cache.push_back(message);
        }
        evicted = dispatch_locked(state, message, &PlaybackSink::on_audio);
    }
    for (auto* sink : evicted) sink->on_slow_client_evicted();
}

void LiveFanout::on_metadata(const std::string& stream_key, const RtmpMessage& message) {
    std::vector<PlaybackSink*> evicted;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& state = streams_[stream_key];
        state.metadata = message;
        evicted = dispatch_locked(state, message, &PlaybackSink::on_metadata);
    }
    for (auto* sink : evicted) sink->on_slow_client_evicted();
}

void LiveFanout::subscribe(const std::string& stream_key, std::uint64_t subscriber_id, PlaybackSink* sink) {
    std::vector<RtmpMessage> startup;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& state = streams_[stream_key];
        state.subscribers[subscriber_id] = Subscriber{sink, 0};

        if (state.metadata) startup.push_back(*state.metadata);
        if (state.video_sequence_header) startup.push_back(*state.video_sequence_header);
        if (state.audio_sequence_header) startup.push_back(*state.audio_sequence_header);
        for (const auto& m : state.gop_cache) startup.push_back(m);
    }

    for (const auto& m : startup) {
        switch (static_cast<MessageTypeId>(m.message_type_id)) {
            case MessageTypeId::Audio:
                sink->on_audio(m);
                break;
            case MessageTypeId::Video:
                sink->on_video(m);
                break;
            case MessageTypeId::Amf0Data:
                sink->on_metadata(m);
                break;
            default:
                break;
        }
    }
}

void LiveFanout::unsubscribe(const std::string& stream_key, std::uint64_t subscriber_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(stream_key);
    if (it == streams_.end()) return;
    it->second.subscribers.erase(subscriber_id);
}

void LiveFanout::publisher_stopped(const std::string& stream_key) {
    std::vector<PlaybackSink*> sinks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(stream_key);
        if (it == streams_.end()) return;
        sinks.reserve(it->second.subscribers.size());
        for (auto& [id, sub] : it->second.subscribers) sinks.push_back(sub.sink);
        streams_.erase(it);
    }
    for (auto* sink : sinks) sink->on_publisher_stopped();
}

std::size_t LiveFanout::subscriber_count(const std::string& stream_key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(stream_key);
    return it == streams_.end() ? 0 : it->second.subscribers.size();
}

} // namespace rtmp_server::protocol::commands
