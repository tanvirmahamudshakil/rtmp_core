# Fuzz harnesses (Phase 9 "fuzzing", docs/rtmp_promot.md). Every target here
# always builds as a plain executable (main() reads files from argv and
# replays them through LLVMFuzzerTestOneInput — a corpus-replay/regression
# tool usable with no fuzzing engine installed). With
# -DRTMP_SERVER_ENABLE_FUZZING=ON (Clang only — libFuzzer ships with LLVM's
# compiler-rt, not GCC's), the same sources are instead compiled with
# -fsanitize=fuzzer,address and linked against libFuzzer's own main(), for
# actual coverage-guided fuzzing.
option(RTMP_SERVER_ENABLE_FUZZING
    "Build fuzz harnesses with libFuzzer (-fsanitize=fuzzer,address); requires Clang" OFF)

# Phase 8 adds fuzz_handshake (the earliest unauthenticated parser) and
# fuzz_token_parser (playback token/query -> HMAC verification), completing the
# set docs/v2_promot.md Phase 8 task 4 requires: chunk decoder, AMF decoder,
# handshake parser, token parser.
set(RTMP_SERVER_FUZZ_TARGETS
    fuzz_amf0_decoder
    fuzz_chunk_decoder
    fuzz_flv_parser
    fuzz_handshake
    fuzz_token_parser
)

foreach(target IN LISTS RTMP_SERVER_FUZZ_TARGETS)
    add_executable(${target} ${target}.cpp)
    target_link_libraries(${target} PRIVATE
        rtmp_server_protocol
        rtmp_server_media
        rtmp_server_management
        rtmp_server_warnings
    )
    # fuzz_main.hpp (the standalone mutation driver used where libFuzzer is
    # unavailable) lives next to the harnesses.
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(${target} PUBLIC cxx_std_23)

    if(RTMP_SERVER_ENABLE_FUZZING)
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            message(FATAL_ERROR "RTMP_SERVER_ENABLE_FUZZING requires Clang (libFuzzer ships with LLVM's compiler-rt)")
        endif()
        target_compile_definitions(${target} PRIVATE RTMP_SERVER_FUZZING_ENGINE_LIBFUZZER)
        target_compile_options(${target} PRIVATE -fsanitize=fuzzer,address -g -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=fuzzer,address)
    endif()
endforeach()
