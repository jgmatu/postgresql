# System packages (Ubuntu 26.04):
#   libpq-dev libbotan-3-dev libboost-json-dev libgtest-dev libgmock-dev
# Fetched and built from source:
#   libpqxx 8.0.0 with patches/libpqxx.patch.full (blocking wait_notification)
#   Oat++ 1.3.1

include(FetchContent)

find_package(Threads REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(Boost 1.83 CONFIG REQUIRED COMPONENTS json)
find_package(PkgConfig REQUIRED)
pkg_check_modules(BOTAN REQUIRED IMPORTED_TARGET botan-3)

set(PGDEMO_LIBPQXX_VERSION "8.0.0")
set(PGDEMO_OATPP_VERSION "1.3.1")

# --- libpqxx (patched) -------------------------------------------------------
set(SKIP_BUILD_TEST ON CACHE BOOL "" FORCE)
set(SKIP_BUILD_EXAMPLES ON CACHE BOOL "" FORCE)
set(BUILD_DOC OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(libpqxx
    URL https://github.com/jtv/libpqxx/archive/refs/tags/${PGDEMO_LIBPQXX_VERSION}.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PATCH_COMMAND patch -p1 --forward --input=${CMAKE_SOURCE_DIR}/patches/libpqxx.patch.full
)

# --- Oat++ ---------------------------------------------------------------------
set(OATPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(OATPP_INSTALL OFF CACHE BOOL "" FORCE)
set(OATPP_LINK_TEST_LIBRARY OFF CACHE BOOL "" FORCE)

FetchContent_Declare(oatpp
    GIT_REPOSITORY https://github.com/oatpp/oatpp.git
    GIT_TAG ${PGDEMO_OATPP_VERSION}
    GIT_SHALLOW TRUE
)

# Oat++ 1.3.1 declares cmake_minimum_required(VERSION 3.1), which CMake 4 rejects.
set(_pgdemo_saved_policy_min "${CMAKE_POLICY_VERSION_MINIMUM}")
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(libpqxx oatpp)
set(CMAKE_POLICY_VERSION_MINIMUM "${_pgdemo_saved_policy_min}")

# Third-party code is not held to the project warning set.
foreach(_tp pqxx oatpp)
    if(TARGET ${_tp})
        get_target_property(_tp_inc ${_tp} INTERFACE_INCLUDE_DIRECTORIES)
        set_target_properties(${_tp} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_tp_inc}")
    endif()
endforeach()
