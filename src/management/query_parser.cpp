#include "rtmp_server/management/query_parser.hpp"

#include <charconv>
#include <system_error>

namespace rtmp_server::management {

SplitStreamName split_stream_name(std::string_view stream_arg) noexcept {
    const auto q = stream_arg.find('?');
    if (q == std::string_view::npos) return SplitStreamName{stream_arg, {}};
    return SplitStreamName{stream_arg.substr(0, q), stream_arg.substr(q + 1)};
}

PlaybackQuery parse_playback_query(std::string_view query) {
    PlaybackQuery out;

    if (query.size() > kMaxQueryLength) {
        out.truncated = true;
        query = query.substr(0, kMaxQueryLength);
    }

    std::size_t pos = 0;
    std::size_t pairs = 0;
    while (pos < query.size()) {
        if (pairs >= kMaxQueryPairs) {
            out.truncated = true;
            break;
        }
        ++pairs;

        const auto amp = query.find('&', pos);
        const std::string_view pair =
            query.substr(pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);

        if (const auto eq = pair.find('='); eq != std::string_view::npos) {
            const std::string_view key = pair.substr(0, eq);
            const std::string_view value = pair.substr(eq + 1);

            // Only the two fields the token scheme defines are materialised.
            // Everything else is skipped without allocating, which is what
            // keeps a query full of junk parameters cheap. First occurrence
            // wins, so a duplicate "token=" appended by an attacker cannot
            // override the one the operator signed into the URL.
            if (key == "token" && !out.token.has_value()) {
                if (!value.empty() && value.size() <= kMaxFieldValueLength) {
                    out.token.emplace(value);
                }
            } else if (key == "expires" && !out.expires_at_unix.has_value()) {
                if (!value.empty() && value.size() <= kMaxFieldValueLength) {
                    std::int64_t parsed = 0;
                    const auto* begin = value.data();
                    const auto* end = value.data() + value.size();
                    const auto result = std::from_chars(begin, end, parsed);
                    // Require the *whole* value to be consumed: "123abc" is
                    // not a timestamp, and accepting its 123 prefix would let
                    // a crafted URL present a different expiry to the parser
                    // than a human reviewing the link would read.
                    if (result.ec == std::errc{} && result.ptr == end) {
                        out.expires_at_unix = parsed;
                    }
                }
            }
        }

        if (amp == std::string_view::npos) break;
        pos = amp + 1;
    }

    return out;
}

} // namespace rtmp_server::management
