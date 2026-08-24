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

# Native CPU H.264 <-> HEVC transcoding pipeline (openh264 decode, libyuv
# scale, x265 encode) plus the H.264/HEVC source-transcode path (openh264/
# libde265 decode, libx264 encode). On by default so the Source Transcode
# feature works out of the box; disable with -DRTMP_ENABLE_NATIVE_TRANSCODE=OFF
# for a stock build that needs none of these libraries (the pure geometry/
# parameter logic still compiles and is tested regardless either way; see
# docs/native-transcoding.md for the required dev packages).
option(RTMP_ENABLE_NATIVE_TRANSCODE
    "Build the in-process native transcoding pipelines (needs x265, x264, openh264, libde265, libyuv)" ON)

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

    # Auto-install on Debian/Ubuntu servers: if a dev package is missing and
    # this configure is running as root with apt-get available (the same
    # environment scripts/install-linux.sh targets), install the missing
    # packages once and re-probe, instead of just warning. Anything else
    # (non-apt distro, non-root configure, no network) silently skips this
    # and falls through to the graceful-degrade warning below.
    if(NOT (X265_FOUND AND X264_FOUND AND OPENH264_FOUND AND FDKAAC_FOUND AND LIBCURL_FOUND AND RTMP_LIBYUV_TARGET AND LIBDE265_FOUND))
        find_program(RTMP_APT_GET_BIN apt-get)
        if(RTMP_APT_GET_BIN AND CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
            execute_process(COMMAND id -u OUTPUT_VARIABLE RTMP_UID OUTPUT_STRIP_TRAILING_WHITESPACE)
            if(RTMP_UID STREQUAL "0")
                set(RTMP_MISSING_APT_PACKAGES "")
                if(NOT X265_FOUND) list(APPEND RTMP_MISSING_APT_PACKAGES libx265-dev) endif()
                if(NOT X264_FOUND) list(APPEND RTMP_MISSING_APT_PACKAGES libx264-dev) endif()
                if(NOT OPENH264_FOUND) list(APPEND RTMP_MISSING_APT_PACKAGES libopenh264-dev) endif()
                if(NOT FDKAAC_FOUND) list(APPEND RTMP_MISSING_APT_PACKAGES libfdk-aac-dev) endif()
                if(NOT LIBCURL_FOUND) list(APPEND RTMP_MISSING_APT_PACKAGES libcurl4-openssl-dev) endif()
                if(NOT RTMP_LIBYUV_TARGET) list(APPEND RTMP_MISSING_APT_PACKAGES libyuv-dev) endif()
                if(NOT LIBDE265_FOUND) list(APPEND RTMP_MISSING_APT_PACKAGES libde265-dev) endif()

                message(STATUS "Native transcoding dependencies missing (${RTMP_MISSING_APT_PACKAGES}); "
                                "attempting 'apt-get install' since this configure is running as root on Linux.")
                execute_process(COMMAND "${RTMP_APT_GET_BIN}" update RESULT_VARIABLE RTMP_APT_UPDATE_RC)
                execute_process(COMMAND "${RTMP_APT_GET_BIN}" install -y --no-install-recommends ${RTMP_MISSING_APT_PACKAGES}
                                RESULT_VARIABLE RTMP_APT_INSTALL_RC)
                if(RTMP_APT_INSTALL_RC EQUAL 0)
                    message(STATUS "apt-get install succeeded; re-checking native transcoding dependencies.")
                    pkg_check_modules(X265 IMPORTED_TARGET x265)
                    pkg_check_modules(X264 IMPORTED_TARGET x264)
                    pkg_check_modules(OPENH264 IMPORTED_TARGET openh264)
                    pkg_check_modules(FDKAAC IMPORTED_TARGET fdk-aac)
                    pkg_check_modules(LIBCURL IMPORTED_TARGET libcurl)
                    pkg_check_modules(LIBYUV IMPORTED_TARGET libyuv)
                    pkg_check_modules(LIBDE265 IMPORTED_TARGET libde265)
                    if(NOT LIBYUV_FOUND)
                        find_library(LIBYUV_LIBRARY NAMES yuv libyuv)
                        find_path(LIBYUV_INCLUDE_DIR NAMES libyuv.h)
                        if(LIBYUV_LIBRARY AND LIBYUV_INCLUDE_DIR AND NOT TARGET rtmp_libyuv)
                            add_library(rtmp_libyuv UNKNOWN IMPORTED)
                            set_target_properties(rtmp_libyuv PROPERTIES
                                IMPORTED_LOCATION "${LIBYUV_LIBRARY}"
                                INTERFACE_INCLUDE_DIRECTORIES "${LIBYUV_INCLUDE_DIR}")
                        endif()
                        if(TARGET rtmp_libyuv)
                            set(RTMP_LIBYUV_TARGET rtmp_libyuv)
                        endif()
                    else()
                        set(RTMP_LIBYUV_TARGET PkgConfig::LIBYUV)
                    endif()
                else()
                    message(WARNING "apt-get install for native transcoding dependencies failed (exit ${RTMP_APT_INSTALL_RC}); "
                                     "continuing without them.")
                endif()
            endif()
        endif()
    endif()

    if(X265_FOUND AND X264_FOUND AND OPENH264_FOUND AND FDKAAC_FOUND AND LIBCURL_FOUND AND RTMP_LIBYUV_TARGET AND LIBDE265_FOUND)
        set(RTMP_NATIVE_TRANSCODE_AVAILABLE ON)
        message(STATUS "Native transcoding pipeline: available (Source Transcode/HEVC will be built).")
    else()
        # RTMP_ENABLE_NATIVE_TRANSCODE defaults ON, so a plain `cmake -S . -B build`
        # with no preset and no dev packages installed must not hard-fail configure
        # -- that would break the first-build experience for anyone not building
        # the `core-only` preset (which explicitly sets this OFF) or the full
        # dependency set. Degrade gracefully to unavailable instead; the resulting
        # binary simply serves 503 transcoding_unavailable for Source Transcode,
        # same as explicitly setting -DRTMP_ENABLE_NATIVE_TRANSCODE=OFF.
        set(RTMP_NATIVE_TRANSCODE_AVAILABLE OFF)
        message(WARNING
            "RTMP_ENABLE_NATIVE_TRANSCODE=ON but dependencies are missing "
            "(x265=${X265_FOUND} x264=${X264_FOUND} openh264=${OPENH264_FOUND} fdk-aac=${FDKAAC_FOUND} "
            "libcurl=${LIBCURL_FOUND} libyuv=${LIBYUV_FOUND} libde265=${LIBDE265_FOUND}) -- building WITHOUT the "
            "native transcoding pipeline (Source Transcode/HEVC will be unavailable at runtime). Install them to "
            "enable it, e.g. on Ubuntu: "
            "sudo apt-get install libx265-dev libx264-dev libopenh264-dev libfdk-aac-dev libcurl4-openssl-dev libyuv-dev libde265-dev")
    endif()
endif()
