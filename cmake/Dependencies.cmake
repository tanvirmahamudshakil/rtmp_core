include(FetchContent)

# GoogleTest, fetched only when tests are enabled.
if(RTMP_SERVER_BUILD_TESTS)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# liburing, only required on Linux for the io_uring transport target.
if(NOT RTMP_SERVER_CORE_ONLY AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_library(LIBURING_LIBRARY NAMES uring)
    find_path(LIBURING_INCLUDE_DIR NAMES liburing.h)

    if(NOT LIBURING_LIBRARY OR NOT LIBURING_INCLUDE_DIR)
        message(FATAL_ERROR
            "liburing not found. Install it, e.g. on Ubuntu: sudo apt-get install liburing-dev")
    endif()

    add_library(liburing::liburing UNKNOWN IMPORTED)
    set_target_properties(liburing::liburing PROPERTIES
        IMPORTED_LOCATION "${LIBURING_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBURING_INCLUDE_DIR}"
    )
endif()
