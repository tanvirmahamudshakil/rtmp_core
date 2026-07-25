# Small CLI that reads a .flv file back and prints its header + tag list, for
# verifying recorded output byte-for-byte when no real media player is
# available on the host (docs/phase6-checklist.md "Acceptance criteria
# evidence"). Pure file reader — no io_uring — so it builds on every platform.
add_executable(flv_inspector main.cpp)

target_link_libraries(flv_inspector PRIVATE
    rtmp_server_media
    rtmp_server_warnings
    rtmp_server_sanitizers
)

target_compile_features(flv_inspector PUBLIC cxx_std_23)
