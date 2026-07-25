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

set(RTMP_SERVER_FUZZ_TARGETS fuzz_amf0_decoder fuzz_chunk_decoder fuzz_flv_parser)

foreach(target IN LISTS RTMP_SERVER_FUZZ_TARGETS)
    add_executable(${target} ${target}.cpp)
    target_link_libraries(${target} PRIVATE
        rtmp_server_protocol
        rtmp_server_media
        rtmp_server_warnings
    )
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
