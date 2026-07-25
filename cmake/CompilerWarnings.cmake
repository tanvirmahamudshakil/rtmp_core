# Applies the mandatory warning set from docs/rtmp_promot.md "Sanitizers and Static Analysis".
function(rtmp_server_set_warnings target_name)
    set(CLANG_GCC_WARNINGS
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wshadow
        -Wsign-conversion
        -Wformat=2
        -Wundef
        -Wnull-dereference
        -Wdouble-promotion
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target_name} INTERFACE ${CLANG_GCC_WARNINGS})
    else()
        message(WARNING "No known warning set for compiler '${CMAKE_CXX_COMPILER_ID}'")
    endif()
endfunction()
