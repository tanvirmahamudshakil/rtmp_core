# Wires RTMP_SERVER_ENABLE_ASAN / _TSAN / _UBSAN onto an interface target that
# other targets link against.
#
# Compatibility: ASan and TSan cannot coexist (separate, conflicting shadow-
# memory runtimes). UBSan composes with either, so the asan option turns on
# address+undefined together; RTMP_SERVER_ENABLE_UBSAN exists for running
# UBSan alone, which is cheap enough for every-commit CI and keeps UBSan
# findings separable from ASan's.
function(rtmp_server_set_sanitizers target_name)
    if(RTMP_SERVER_ENABLE_ASAN AND RTMP_SERVER_ENABLE_TSAN)
        message(FATAL_ERROR "RTMP_SERVER_ENABLE_ASAN and RTMP_SERVER_ENABLE_TSAN are mutually exclusive")
    endif()

    if(RTMP_SERVER_ENABLE_ASAN)
        target_compile_options(${target_name} INTERFACE
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all
        )
        target_link_options(${target_name} INTERFACE
            -fsanitize=address,undefined
        )
    endif()

    if(RTMP_SERVER_ENABLE_TSAN)
        target_compile_options(${target_name} INTERFACE
            -fsanitize=thread
            -fno-omit-frame-pointer
        )
        target_link_options(${target_name} INTERFACE
            -fsanitize=thread
        )
    endif()

    # UBSan on its own. Skipped when ASan is on, which already includes it.
    if(RTMP_SERVER_ENABLE_UBSAN AND NOT RTMP_SERVER_ENABLE_ASAN)
        target_compile_options(${target_name} INTERFACE
            -fsanitize=undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all
        )
        target_link_options(${target_name} INTERFACE
            -fsanitize=undefined
        )
    endif()
endfunction()

# Exploit-mitigation flags for deployable builds (RTMP_SERVER_ENABLE_HARDENING,
# set by the `production` CMake preset). Deliberately separate from the
# sanitizers: sanitizers are development instrumentation and must never ship,
# whereas these are cheap, always-on runtime defences.
#
# Each flag and its cost/benefit is documented in docs/deployment.md
# "Build hardening"; in brief:
#
#   _FORTIFY_SOURCE=3   compile-time-sized checks on memcpy/strcpy/sprintf and
#                       friends, aborting instead of overflowing. Needs an
#                       optimised build to have size information, which is why
#                       the production preset is RelWithDebInfo and not Debug.
#   -fstack-protector-strong
#                       stack canary on any function with an array or an
#                       address-taken local. Turns a stack-buffer overflow into
#                       a controlled abort rather than a return-address
#                       overwrite. "strong" rather than "all" is the standard
#                       distribution trade-off (nearly all the coverage, a
#                       fraction of the cost).
#   -D_GLIBCXX_ASSERTIONS
#                       bounds checks on libstdc++ container operator[] and
#                       iterator arithmetic. No effect with libc++; harmless.
#   -fno-delete-null-pointer-checks
#                       stops the optimiser deleting a null check it has
#                       "proved" redundant via UB, which is a recurring source
#                       of silently removed security checks.
#   -fno-strict-aliasing
#                       this codebase reinterprets byte buffers as protocol
#                       structures; conservative aliasing keeps that sound.
#   -Wl,-z,relro -Wl,-z,now
#                       full RELRO: resolve the PLT/GOT at load time and make
#                       it read-only, removing GOT overwrite as a technique.
#   -Wl,-z,noexecstack  non-executable stack.
#
# The -Wl,-z options are ELF/GNU-ld specific and are applied only on Linux,
# which is the only supported deployment target anyway.
function(rtmp_server_set_hardening target_name)
    if(NOT RTMP_SERVER_ENABLE_HARDENING)
        return()
    endif()

    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        message(WARNING "RTMP_SERVER_ENABLE_HARDENING has no flag set for '${CMAKE_CXX_COMPILER_ID}'")
        return()
    endif()

    target_compile_options(${target_name} INTERFACE
        -fstack-protector-strong
        -fno-delete-null-pointer-checks
        -fno-strict-aliasing
    )
    target_compile_definitions(${target_name} INTERFACE
        _FORTIFY_SOURCE=3
        _GLIBCXX_ASSERTIONS
    )

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(${target_name} INTERFACE
            -Wl,-z,relro
            -Wl,-z,now
            -Wl,-z,noexecstack
        )
    endif()
endfunction()
