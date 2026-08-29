cmake_minimum_required(VERSION 3.25)
project(opentm
    VERSION 0.0.1
    DESCRIPTION "OpenTM"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Every executable lands in one bin/ directory. The three processes find
# each other as siblings, so they have to be siblings in the build tree
# too - and it means the Qt runtime is deployed once instead of per target.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(cmake/BuildSpeed.cmake)

if(MSVC)
    # /MP is per-project multi-process compilation; it is what makes a
    # single target use more than one core. /Zc:inline lets the linker drop
    # unreferenced COMDATs instead of carrying them to the final image.
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /Zc:inline /MP)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Network Widgets)

# Release packaging does not need the test binary, and skipping it also
# skips the Catch2 lookup below - which is what keeps a packaging build from
# needing network access.
option(OPENTM_BUILD_TESTS "Build the unit tests" ON)

# Fedora and most other distros still ship Catch2 2.x, which the tests do
# not compile against. Prefer a system Catch2 3 when there is one and fetch
# it otherwise, so a plain `dnf install qt6-qtbase-devel` is enough to
# configure. Set OPENTM_FETCH_CATCH2=OFF to require the system package.
option(OPENTM_FETCH_CATCH2 "Download Catch2 3 when the system has no compatible version" ON)

if(OPENTM_BUILD_TESTS)
    find_package(Catch2 3 CONFIG QUIET)
    if(NOT Catch2_FOUND)
        if(NOT OPENTM_FETCH_CATCH2)
            message(FATAL_ERROR
                "Catch2 3 not found and OPENTM_FETCH_CATCH2 is OFF. Install Catch2 3, "
                "re-run with -DOPENTM_FETCH_CATCH2=ON, or build without tests "
                "(-DOPENTM_BUILD_TESTS=OFF).")
        endif()
        message(STATUS "Catch2 3 not found on the system - fetching it")
        include(FetchContent)
        FetchContent_Declare(Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG v3.9.1
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(Catch2)
        # Catch2's installed config adds this itself; a fetched copy does not,
        # and tests/ needs it for include(Catch).
        list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    endif()
    enable_testing()
endif()

# build_info.h carries the version, commit and toolchain shown in About.
# Regenerated every build so the hash tracks the working tree.
set(OPENTM_BUILD_INFO_DIR "${CMAKE_BINARY_DIR}/generated")
set(OPENTM_BUILD_INFO_H   "${OPENTM_BUILD_INFO_DIR}/build_info.h")
file(MAKE_DIRECTORY "${OPENTM_BUILD_INFO_DIR}")

add_custom_target(opentm_build_info_gen ALL
    COMMAND "${CMAKE_COMMAND}"
            "-DOPENTM_VERSION=${PROJECT_VERSION}"
            "-DOUT_FILE=${OPENTM_BUILD_INFO_H}"
            "-DSRC_DIR=${CMAKE_SOURCE_DIR}"
            "-DCXX_COMPILER=${CMAKE_CXX_COMPILER_ID}"
            "-DCXX_COMPILER_VERSION=${CMAKE_CXX_COMPILER_VERSION}"
            "-DBUILD_TYPE=$<CONFIG>"
            -P "${CMAKE_SOURCE_DIR}/cmake/GenerateBuildInfo.cmake"
    BYPRODUCTS "${OPENTM_BUILD_INFO_H}"
    COMMENT "Refreshing build_info.h"
    VERBATIM
)

add_library(opentm_build_info INTERFACE)
target_include_directories(opentm_build_info INTERFACE "${OPENTM_BUILD_INFO_DIR}")

# An INTERFACE library carries include dirs but not build ordering, so each
# consumer has to depend on the generator itself or it races the first build.
function(opentm_use_build_info target)
    target_link_libraries(${target} PRIVATE opentm_build_info)
    add_dependencies(${target} opentm_build_info_gen)
endfunction()

add_subdirectory(src/tm_core)
add_subdirectory(src/tm_session)
add_subdirectory(src/tm_launch)
add_subdirectory(src/tm_ui)
add_subdirectory(src/tm_dbg)
add_subdirectory(src/tm_tray)
add_subdirectory(src/tm_cli)
add_subdirectory(src/tm_server)

if(OPENTM_BUILD_TESTS)
    add_subdirectory(tests)
    add_subdirectory(tools/test)
endif()

include(cmake/Install.cmake)

opentm_report_build_speed()
