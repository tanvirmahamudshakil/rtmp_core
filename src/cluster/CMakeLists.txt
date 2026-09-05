# Cluster membership and placement: which nodes exist, which are healthy, and
# which one a viewer should be sent to. Pure bookkeeping over the persistence
# layer — no sockets — so it builds and is tested on every platform.
add_library(rtmp_server_cluster STATIC
    node_registry.cpp
)

target_include_directories(rtmp_server_cluster PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(rtmp_server_cluster
    PUBLIC
        rtmp_server_core
        rtmp_server_persistence
        rtmp_server_warnings
        rtmp_server_sanitizers
)

target_compile_features(rtmp_server_cluster PUBLIC cxx_std_23)
