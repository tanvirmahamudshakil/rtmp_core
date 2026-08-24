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
    include(CheckCXXSourceCompiles)

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

    # SEND_ZC arrived after the baseline io_uring API. Keep the server
    # buildable against older distro liburing packages and enable the fast
    # path only when the installed headers expose the helper and flags.
    set(CMAKE_REQUIRED_INCLUDES "${LIBURING_INCLUDE_DIR}")
    check_cxx_source_compiles(
        "#include <liburing.h>
         int main() {
             io_uring_sqe sqe{};
             io_uring_prep_send_zc(&sqe, -1, nullptr, 0, 0,
                                   IORING_SEND_ZC_REPORT_USAGE);
             return (IORING_CQE_F_NOTIF | IORING_CQE_F_MORE) == 0;
         }"
        RTMP_LIBURING_HAS_SEND_ZC
    )
    unset(CMAKE_REQUIRED_INCLUDES)
endif()

# Optional native CPU H.264 -> HEVC transcoding pipeline (openh264 decode,
# libyuv scale, x265 encode) plus the H.264 source-transcode encode path
# (openh264 decode, libx264 encode). Off by default so a stock build needs
# none of these libraries; the pure geometry/parameter logic still compiles
# and is tested regardless. Enable with -DRTMP_ENABLE_NATIVE_TRANSCODE=ON
# after installing the dev packages (see docs/native-transcoding.md).
option(RTMP_ENABLE_NATIVE_TRANSCODE
    "Build the in-process native transcoding pipelines (needs x265, x264, openh264, libyuv)" OFF)

set(RTMP_NATIVE_TRANSCODE_AVAILABLE OFF)
if(RTMP_ENABLE_NATIVE_TRANSCODE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(X265 IMPORTED_TARGET x265)
    pkg_check_modules(X264 IMPORTED_TARGET x264)
    pkg_check_modules(OPENH264 IMPORTED_TARGET openh264)
    pkg_check_modules(FDKAAC IMPORTED_TARGET fdk-aac)
    pkg_check_modules(LIBCURL IMPORTED_TARGET libcurl)
    pkg_check_modules(LIBYUV IMPORTED_TARGET libyuv)
    # HEVC decode (source-transcode ingest of an HEVC source). libde265 is
    # LGPL-2.1: PkgConfig::LIBDE265 resolves to the system shared library via
    # pkg-config, so this stays a dynamic link -- do not switch this to a
    # static archive, that would violate the LGPL for a redistributed binary.
    pkg_check_modules(LIBDE265 IMPORTED_TARGET libde265)

    # libyuv frequently ships without a .pc file; fall back to a plain search.
    if(NOT LIBYUV_FOUND)
        find_library(LIBYUV_LIBRARY NAMES yuv libyuv)
        find_path(LIBYUV_INCLUDE_DIR NAMES libyuv.h)
        if(LIBYUV_LIBRARY AND LIBYUV_INCLUDE_DIR)
            add_library(rtmp_libyuv UNKNOWN IMPORTED)
            set_target_properties(rtmp_libyuv PROPERTIES
                IMPORTED_LOCATION "${LIBYUV_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${LIBYUV_INCLUDE_DIR}")
            set(RTMP_LIBYUV_TARGET rtmp_libyuv)
        endif()
    else()
        set(RTMP_LIBYUV_TARGET PkgConfig::LIBYUV)
    endif()

    if(X265_FOUND AND X264_FOUND AND OPENH264_FOUND AND FDKAAC_FOUND AND LIBCURL_FOUND AND RTMP_LIBYUV_TARGET AND LIBDE265_FOUND)
        set(RTMP_NATIVE_TRANSCODE_AVAILABLE ON)
    else()
        message(FATAL_ERROR
            "RTMP_ENABLE_NATIVE_TRANSCODE=ON but dependencies are missing "
            "(x265=${X265_FOUND} x264=${X264_FOUND} openh264=${OPENH264_FOUND} fdk-aac=${FDKAAC_FOUND} "
            "libcurl=${LIBCURL_FOUND} libyuv=${LIBYUV_FOUND} libde265=${LIBDE265_FOUND}). Install them, e.g. on Ubuntu: "
            "sudo apt-get install libx265-dev libx264-dev libopenh264-dev libfdk-aac-dev libcurl4-openssl-dev libyuv-dev libde265-dev")
    endif()
endif()
