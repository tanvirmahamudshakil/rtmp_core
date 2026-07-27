#include "rtmp_server/recording/recording_path.hpp"

#include <string>

namespace rtmp_server::recording {

namespace {

using core::Error;
using core::ErrorCategory;
using core::ErrorCode;

Error invalid(std::string_view what) {
    return Error(ErrorCode::InvalidArgument, ErrorCategory::Internal, std::string(what));
}

bool is_directory_alias(std::string_view component) noexcept {
    return component == "." || component == "..";
}

} // namespace

bool is_safe_path_char(char c) noexcept {
    // Explicit ranges rather than std::isalnum: the <cctype> functions are
    // locale-dependent and take an int that must be representable as unsigned
    // char, both of which are easy to get subtly wrong on attacker input.
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
           c == '-';
}

bool is_safe_path_component(std::string_view component) noexcept {
    if (component.empty()) return false;
    if (component.size() > kMaxComponentLength) return false;
    if (is_directory_alias(component)) return false;
    // A leading '-' makes the filename look like an option to every command
    // line an operator or retention script will ever run over the recording
    // directory ("rm -rf-named-file"); a leading '.' makes it a hidden file
    // that a `ls`/glob-based sweeper silently skips. Both bytes stay legal
    // elsewhere in the component.
    if (component.front() == '-' || component.front() == '.') return false;
    for (const char c : component) {
        if (!is_safe_path_char(c)) return false;
    }
    return true;
}

core::Result<std::string> sanitize_path_component(std::string_view component) {
    std::string out;
    out.reserve(component.size() < kMaxComponentLength ? component.size() : kMaxComponentLength);
    for (const char c : component) {
        if (out.size() >= kMaxComponentLength) break;
        out.push_back(is_safe_path_char(c) ? c : '_');
    }
    if (out.empty()) return invalid("path component is empty after sanitisation");
    // A component of only dots ("..", "...") sanitises to itself because '.'
    // is in the allow-list, so the alias check must happen after rewriting,
    // not before.
    if (is_directory_alias(out)) return invalid("path component sanitises to a directory alias");
    // Same reasoning for the leading-byte rule: '-' and '.' survive byte-level
    // rewriting, so neutralise them here rather than emitting a component
    // that is_safe_path_component() would then reject.
    if (out.front() == '-' || out.front() == '.') out.front() = '_';
    return out;
}

core::Result<std::string> build_recording_path(std::string_view directory, std::string_view application,
                                                std::string_view stream_name, std::int64_t started_at_unix) {
    if (directory.empty()) return invalid("recording directory must not be empty");
    if (!is_safe_path_component(application)) {
        return invalid("application name is not safe to use as a filesystem path component");
    }
    if (!is_safe_path_component(stream_name)) {
        return invalid("stream name is not safe to use as a filesystem path component");
    }

    std::string path(directory);
    if (path.back() != '/') path.push_back('/');
    path.append(application);
    path.push_back('/');
    path.append(stream_name);
    path.push_back('-');
    path.append(std::to_string(started_at_unix));
    path.append(".flv");
    return path;
}

} // namespace rtmp_server::recording
