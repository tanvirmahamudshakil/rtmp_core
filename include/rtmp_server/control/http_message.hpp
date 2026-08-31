#pragma once

#include <string>
#include <string_view>

#include "rtmp_server/control/http_server.hpp"

namespace rtmp_server::control {

// Pure HTTP/1.1 message helpers, shared by the blocking HttpServer and the
// event-driven AsyncHttpServer. Kept free of any socket I/O so both servers
// parse and serialise identically -- two hand-rolled parsers that drift
// apart is exactly how a request-smuggling difference gets introduced.

// Parses a complete request head: the request line plus header lines, with
// the terminating CRLFCRLF already stripped by the caller. Header names are
// lower-cased. Returns false on a malformed request line.
//
// Does not touch `out.body`: bodies are read differently by each server
// (blocking loop vs. readiness events), and how many body bytes to expect is
// the caller's decision via content_length_of().
[[nodiscard]] bool parse_request_head(std::string_view head, HttpRequest& out);

// Content-Length of a parsed request, or 0 when absent. `valid` is set false
// when the header is present but not a well-formed non-negative integer, so
// a caller can reject it rather than silently reading zero bytes and then
// treating the body bytes as the next pipelined request.
[[nodiscard]] std::size_t content_length_of(const HttpRequest& request, bool& valid);

// Reason phrases for the statuses these servers emit. HLS clients and CDNs
// key retry/caching behaviour off the status line, and a 206 or 416 labelled
// "OK"/"Error" is confusing to intermediaries even though the code is what
// is normative.
[[nodiscard]] const char* reason_phrase(int status);

// Serialises the status line and headers, including the trailing blank line.
// The body is never included: the async server writes it from the response's
// shared buffer without copying it into this string.
[[nodiscard]] std::string serialize_response_head(const HttpResponse& response, bool keep_alive);

} // namespace rtmp_server::control
