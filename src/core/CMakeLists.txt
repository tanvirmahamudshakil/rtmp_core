find_package(OpenSSL REQUIRED COMPONENTS Crypto)

add_library(rtmp_server_core STATIC
    random.cpp
    config.cpp
    ${CMAKE_SOURCE_DIR}/src/observability/logger.cpp
)

target_include_directories(rtmp_server_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(rtmp_server_core
    PUBLIC
        rtmp_server_warnings
        rtmp_server_sanitizers
    PRIVATE
        OpenSSL::Crypto
)

target_compile_features(rtmp_server_core PUBLIC cxx_std_23)
