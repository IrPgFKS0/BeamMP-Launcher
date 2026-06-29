cmake_minimum_required(VERSION 3.10)

# Single combined binary: ONE build, THREE runtime modes (default launcher / --server-only /
# --combined) chosen by a commandline flag. The server is ALWAYS embedded (user chose one binary
# over a lean standalone), so always pull its vcpkg deps. MUST be before project() so the vcpkg
# toolchain sees the manifest feature.
list(APPEND VCPKG_MANIFEST_FEATURES "embed-server")

project(Launcher)

if (WIN32)
    message(STATUS "MSVC -> forcing use of statically-linked runtime.")
    STRING(REPLACE "/MD" "/MT" CMAKE_CXX_FLAGS_RELEASE ${CMAKE_CXX_FLAGS_RELEASE})
    STRING(REPLACE "/MDd" "/MTd" CMAKE_CXX_FLAGS_DEBUG ${CMAKE_CXX_FLAGS_DEBUG})
endif(WIN32)

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG")

add_compile_definitions(CPPHTTPLIB_OPENSSL_SUPPORT)

file(GLOB source_files "src/*.cpp" "src/*/*.cpp" "src/*/*.hpp" "include/*.h"  "include/*/*.h" "include/*/*/*.h" "include/*.hpp"  "include/*/*.hpp" "include/*/*/*.hpp")
find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(CURL REQUIRED)

# --- Embedded BeamMP server (ALWAYS: one binary, three runtime modes) ---
# The whole server is linked in; the runtime mode (default launcher / --server-only / --combined)
# is chosen by a commandline flag in main.cpp. We pull in the server project so its static lib
# (beammp_server_lib) + transitive deps come along; the toolchain is already set by the launcher
# cmake invocation so the server's Vcpkg.cmake no-ops; EXCLUDE_FROM_ALL so only the library (our
# link dependency) builds -- not the server's own exe/doctest tests. BEAMMP_EMBED_SERVER stays
# defined for the main.cpp #ifdefs (the --server-only / --combined branches).
set(BeamMP-Server_ENABLE_UNIT_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(../BeamMP-Server ${CMAKE_BINARY_DIR}/beammp-server-build EXCLUDE_FROM_ALL)
add_compile_definitions(BEAMMP_EMBED_SERVER)
set(EMBED_SERVER_LIB beammp_server_lib)

add_executable(${PROJECT_NAME} ${source_files})
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "BeamMP-Launcher")

if (WIN32)
    find_package(ZLIB REQUIRED)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE
            ${EMBED_SERVER_LIB}
            ZLIB::ZLIB OpenSSL::SSL OpenSSL::Crypto ws2_32 httplib::httplib nlohmann_json::nlohmann_json CURL::libcurl)
elseif (UNIX)
    find_package(ZLIB REQUIRED)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE
            ${EMBED_SERVER_LIB}
            ZLIB::ZLIB OpenSSL::SSL OpenSSL::Crypto CURL::libcurl)
else(WIN32) #MINGW
    add_definitions("-D_WIN32_WINNT=0x0600")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Os -s --static")
    target_link_libraries(${PROJECT_NAME} ssl crypto ws2_32 ssp crypt32 z CURL::libcurl)
endif(WIN32)
target_include_directories(${PROJECT_NAME} PRIVATE "include")
