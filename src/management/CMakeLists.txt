# Management API domain logic (Phase 8): stream/application CRUD, key
# generation/rotation, signed tokens, disconnect controls, live-state
# snapshots. Depends on rtmp_server_protocol for StreamRegistry/LiveFanout
# (to cross-reference live publish/viewer state) and rtmp_server_core for
# Result/hmac/random — no HTTP server, no liburing, builds on every platform.
add_library(rtmp_server_management STATIC
    stream_manager.cpp
    token.cpp
    url_builder.cpp
    authorization_cache.cpp
)

target_include_directories(rtmp_server_management PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(rtmp_server_management
    PUBLIC
        rtmp_server_core
        rtmp_server_protocol
        rtmp_server_persistence
        rtmp_server_warnings
        rtmp_server_sanitizers
)

target_compile_features(rtmp_server_management PUBLIC cxx_std_23)
