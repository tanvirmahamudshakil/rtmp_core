# Phase 7 real load generator (docs/v2_promot.md PHASE 7 "Real load
# generator"). A library rather than only an executable so the protocol
# correctness of its handshake/command/media bytes can be asserted directly
# by tests/load against the same codec infrastructure the server uses —
# see tests/load/loadgen_protocol_test.cpp.
#
# Pure POSIX sockets + the portable protocol layer: no io_uring, so this
# builds and runs on every platform including core-only/macOS, which is
# exactly why it can be validated on a host where the production transport
# cannot be built at all.
add_library(rtmp_server_loadgen STATIC
    media_source.cpp
    rtmp_client.cpp
    scenario.cpp
)

target_include_directories(rtmp_server_loadgen PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(rtmp_server_loadgen
    PUBLIC
        rtmp_server_core
        rtmp_server_protocol
        rtmp_server_warnings
        rtmp_server_sanitizers
)

target_compile_features(rtmp_server_loadgen PUBLIC cxx_std_23)
