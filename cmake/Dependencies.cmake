# System packages (Ubuntu 26.04):
#   libpq-dev libbotan-3-dev libboost-json-dev libssl-dev libgtest-dev libgmock-dev
# Fetched and built from source:
#   libpqxx 8.0.0 with patches/libpqxx.patch.full (blocking wait_notification)
#   Oat++ 1.3.1
#   oatpp-openssl 1.3.0
#   liboqs 0.16.0 (static) and oqs-provider 0.12.0 (oqsprovider.so)

include(FetchContent)

find_package(Threads REQUIRED)
find_package(PostgreSQL REQUIRED)
find_package(Boost 1.83 CONFIG REQUIRED COMPONENTS json)
find_package(PkgConfig REQUIRED)
pkg_check_modules(BOTAN REQUIRED IMPORTED_TARGET botan-3)

set(PGDEMO_LIBPQXX_VERSION "8.0.0")
set(PGDEMO_OATPP_VERSION "1.3.1")
set(PGDEMO_OATPP_OPENSSL_VERSION "1.3.0")
set(PGDEMO_LIBOQS_VERSION "0.16.0")
set(PGDEMO_OQS_PROVIDER_VERSION "0.12.0")

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
# oatpp-openssl's in-tree link step always names the oatpp-test target.
set(OATPP_LINK_TEST_LIBRARY ON CACHE BOOL "" FORCE)

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

# --- oatpp-openssl, liboqs, oqs-provider -------------------------------------
# oatpp-openssl 1.3.0 also declares cmake_minimum_required(VERSION 3.1).
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(OATPP_MODULES_LOCATION "CUSTOM" CACHE STRING "" FORCE)
set(OATPP_DIR_SRC "${oatpp_SOURCE_DIR}/src" CACHE PATH "" FORCE)
set(OATPP_DIR_LIB "${oatpp_BINARY_DIR}/src" CACHE PATH "" FORCE)
set(OATPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(OATPP_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(oatpp-openssl
    GIT_REPOSITORY https://github.com/oatpp/oatpp-openssl.git
    GIT_TAG ${PGDEMO_OATPP_OPENSSL_VERSION}
    GIT_SHALLOW TRUE
)

# Static liboqs, no tests. The provider module links it in, so the runtime
# image does not need a separate liboqs shared library.
set(OQS_BUILD_ONLY_LIB ON CACHE BOOL "" FORCE)
set(OQS_DIST_BUILD OFF CACHE BOOL "" FORCE)
set(OQS_LIBJADE_BUILD OFF CACHE BOOL "" FORCE)

FetchContent_Declare(liboqs
    GIT_REPOSITORY https://github.com/open-quantum-safe/liboqs.git
    GIT_TAG ${PGDEMO_LIBOQS_VERSION}
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(oatpp-openssl liboqs)

if(TARGET oqs AND NOT TARGET OQS::oqs)
    add_library(OQS::oqs ALIAS oqs)
endif()
# oqs-provider's find_package(liboqs) wants an installed config. The target
# already exists from the fetch above; this file only has to say so.
file(WRITE "${CMAKE_BINARY_DIR}/liboqs-config/liboqsConfig.cmake"
"if(NOT TARGET OQS::oqs)\n  message(FATAL_ERROR \"OQS::oqs is missing\")\nendif()\n")
set(liboqs_DIR "${CMAKE_BINARY_DIR}/liboqs-config" CACHE PATH "" FORCE)

set(OQS_PROVIDER_BUILD_STATIC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(oqs-provider
    GIT_REPOSITORY https://github.com/open-quantum-safe/oqs-provider.git
    GIT_TAG ${PGDEMO_OQS_PROVIDER_VERSION}
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(oqs-provider)

set(CMAKE_POLICY_VERSION_MINIMUM "${_pgdemo_saved_policy_min}")

# Third-party code is not held to the project warning set.
foreach(_tp pqxx oatpp oatpp-openssl)
    if(TARGET ${_tp})
        get_target_property(_tp_inc ${_tp} INTERFACE_INCLUDE_DIRECTORIES)
        if(_tp_inc)
            set_target_properties(${_tp} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_tp_inc}")
        endif()
    endif()
endforeach()
