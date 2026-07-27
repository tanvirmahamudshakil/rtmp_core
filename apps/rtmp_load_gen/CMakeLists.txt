# Phase 7 CLI front-end for rtmp_server_loadgen. All the logic lives in the
# library so tests can assert the generator's protocol correctness directly;
# this target is only argument parsing and reporting.
add_executable(rtmp_load_gen main.cpp)

target_link_libraries(rtmp_load_gen PRIVATE
    rtmp_server_loadgen
    rtmp_server_warnings
    rtmp_server_sanitizers
)

target_compile_features(rtmp_load_gen PUBLIC cxx_std_23)
