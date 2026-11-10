cmake_minimum_required(VERSION 3.25)

project(pgdemo
    VERSION 1.0.0
    DESCRIPTION "PostgreSQL reactive architecture demo: persistent queue, LISTEN/NOTIFY, partitioned history, Argon2id + JWT"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(PGDEMO_BUILD_TESTS "Build the GoogleTest suites" ON)
option(PGDEMO_BUILD_DOCS "Add the docs target (Doxygen)" ON)

include(cmake/Dependencies.cmake)

set(PGDEMO_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)

add_subdirectory(libs/log)
add_subdirectory(libs/db)
add_subdirectory(libs/auth)
add_subdirectory(libs/pki)
add_subdirectory(msgs)
add_subdirectory(device)
add_subdirectory(controller)
add_subdirectory(server)

if(PGDEMO_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(PGDEMO_BUILD_DOCS)
    include(cmake/Docs.cmake)
endif()
