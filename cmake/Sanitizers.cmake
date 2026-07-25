# Wires RTMP_SERVER_ENABLE_ASAN / RTMP_SERVER_ENABLE_TSAN onto an interface target
# that other targets link against. ASan and TSan are mutually exclusive.
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
endfunction()
