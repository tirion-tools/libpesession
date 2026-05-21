cmake_minimum_required(VERSION 3.16)
project(pesession
    VERSION 0.1.0
    DESCRIPTION "Clean-room reader for SentryOne / SolarWinds .pesession archives"
    LANGUAGES C CXX
)

option(PESESSION_BUILD_TESTS "Build libpesession tests" ON)
option(PESESSION_INSTALL     "Install libpesession targets" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ── Sibling libs ────────────────────────────────────────────────────
# Resolved as siblings under the parent directory by default. Override
# the paths with -D<NAME>_DIR=… if your tree shape differs.
set(SHOWPLAN_DIR ${CMAKE_SOURCE_DIR}/../libshowplan
    CACHE PATH "Path to libshowplan source tree")
set(NRBF_DIR ${CMAKE_SOURCE_DIR}/../libnrbf
    CACHE PATH "Path to libnrbf source tree")
set(OSESSION_DIR ${CMAKE_SOURCE_DIR}/../libosession
    CACHE PATH "Path to libosession source tree")

# Build the siblings but skip their tests + installs when we're
# building libpesession standalone.
if(NOT TARGET showplan)
    set(SHOWPLAN_BUILD_TESTS OFF CACHE INTERNAL "")
    set(SHOWPLAN_INSTALL OFF CACHE INTERNAL "")
    add_subdirectory(${SHOWPLAN_DIR} ${CMAKE_BINARY_DIR}/_deps/libshowplan-build
        EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET nrbf)
    set(NRBF_BUILD_TESTS OFF CACHE INTERNAL "")
    set(NRBF_INSTALL OFF CACHE INTERNAL "")
    add_subdirectory(${NRBF_DIR} ${CMAKE_BINARY_DIR}/_deps/libnrbf-build
        EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET osession)
    set(OSESSION_BUILD_TESTS OFF CACHE INTERNAL "")
    set(OSESSION_INSTALL OFF CACHE INTERNAL "")
    add_subdirectory(${OSESSION_DIR} ${CMAKE_BINARY_DIR}/_deps/libosession-build
        EXCLUDE_FROM_ALL)
endif()

# ── External deps ──────────────────────────────────────────────────
include(FetchContent)
if(NOT TARGET nlohmann_json::nlohmann_json)
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    set(JSON_Install    OFF CACHE INTERNAL "")
    FetchContent_Declare(json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(json)
endif()

# miniz comes from libosession's vendored copy. Reuse the existing
# target rather than defining a second.
if(NOT TARGET miniz)
    message(FATAL_ERROR
        "libpesession requires miniz. The standalone build expects "
        "to find it via the sibling libosession; if you're building "
        "libpesession out of context, add miniz to your tree.")
endif()

# ── Library ────────────────────────────────────────────────────────
add_library(pesession STATIC
    src/pesession.cpp
    src/pesession_to_osession.cpp
)
add_library(pesession::pesession ALIAS pesession)

target_include_directories(pesession
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(pesession
    PUBLIC
        showplan::showplan
        osession::osession
    PRIVATE
        nrbf::nrbf
        nlohmann_json::nlohmann_json
        miniz
)
target_compile_features(pesession PUBLIC cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pesession PRIVATE
        -Wall -Wextra -Wno-unused-parameter
    )
endif()

# ── Tests + CLI tools ──────────────────────────────────────────────
if(PESESSION_BUILD_TESTS)
    enable_testing()
    add_executable(pesession_smoke tests/test_smoke.cpp)
    target_link_libraries(pesession_smoke PRIVATE pesession)
    add_test(NAME pesession_smoke COMMAND pesession_smoke)
endif()

if(PESESSION_INSTALL)
    # No EXPORT set. Private deps (nrbf, nlohmann_json, miniz) live
    # in repos that don't ship export configs. The intended
    # integration path is add_subdirectory().
    include(GNUInstallDirs)
    install(TARGETS pesession
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
    install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
endif()
