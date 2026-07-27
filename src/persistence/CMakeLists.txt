# SQLite-backed persistence (Phase 9 "SQLite development persistence").
# find_package(SQLite3) is a CMake built-in module (3.14+); on macOS it
# resolves against the system SDK's libsqlite3, on Linux against
# libsqlite3-dev — a system library like OpenSSL, never vendored/fetched.
find_package(SQLite3 REQUIRED)

# FindSQLite3 used SQLite::SQLite3 through CMake 4.2 and switched to
# SQLite3::SQLite3 in CMake 4.3. Support both names because the production
# baseline (Ubuntu 24.04 / CMake 3.28) still provides the former. The variable
# fallback also covers distribution modules that report only the traditional
# SQLite3_INCLUDE_DIRS / SQLite3_LIBRARIES results.
if(TARGET SQLite3::SQLite3)
    set(RTMP_SERVER_SQLITE_TARGET SQLite3::SQLite3)
elseif(TARGET SQLite::SQLite3)
    set(RTMP_SERVER_SQLITE_TARGET SQLite::SQLite3)
else()
    add_library(rtmp_server_sqlite3 INTERFACE)
    target_include_directories(rtmp_server_sqlite3 INTERFACE
        ${SQLite3_INCLUDE_DIRS}
    )
    target_link_libraries(rtmp_server_sqlite3 INTERFACE
        ${SQLite3_LIBRARIES}
    )
    set(RTMP_SERVER_SQLITE_TARGET rtmp_server_sqlite3)
endif()

add_library(rtmp_server_persistence STATIC
    sqlite_store.cpp
)

target_include_directories(rtmp_server_persistence PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(rtmp_server_persistence
    PUBLIC
        rtmp_server_core
        rtmp_server_warnings
        rtmp_server_sanitizers
    PRIVATE
        ${RTMP_SERVER_SQLITE_TARGET}
)

target_compile_features(rtmp_server_persistence PUBLIC cxx_std_23)
