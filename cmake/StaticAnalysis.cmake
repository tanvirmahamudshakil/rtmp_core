# Optionally wires clang-tidy into the build via CMAKE_CXX_CLANG_TIDY.
# Enable with -DRTMP_SERVER_ENABLE_CLANG_TIDY=ON.
option(RTMP_SERVER_ENABLE_CLANG_TIDY "Run clang-tidy during the build" OFF)

if(RTMP_SERVER_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy)
    if(CLANG_TIDY_EXE)
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
    else()
        message(WARNING "RTMP_SERVER_ENABLE_CLANG_TIDY is ON but clang-tidy was not found")
    endif()
endif()
