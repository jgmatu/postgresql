# `docs` target: Doxygen HTML for every header of the project.
#
#   cmake --build build --target docs
#   build/docs/html/index.html
#
# Any undocumented class or public member fails the target.

find_package(Doxygen OPTIONAL_COMPONENTS dot)
if(NOT DOXYGEN_FOUND)
    message(STATUS "Doxygen not found: the docs target is not available")
    return()
endif()

set(DOXYGEN_PROJECT_NAME "pgdemo")
set(DOXYGEN_PROJECT_BRIEF "${PROJECT_DESCRIPTION}")
set(DOXYGEN_PROJECT_NUMBER "${PROJECT_VERSION}")
set(DOXYGEN_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/docs")
set(DOXYGEN_USE_MDFILE_AS_MAINPAGE "${CMAKE_SOURCE_DIR}/README.md")
set(DOXYGEN_FILE_PATTERNS *.hpp *.md *.dox)
# Section links in the Markdown files use GitHub anchors (`#1-processes-and-zones`).
set(DOXYGEN_MARKDOWN_ID_STYLE GITHUB)
set(DOXYGEN_EXTENSION_MAPPING hpp=C++)
set(DOXYGEN_FULL_PATH_NAMES YES)
set(DOXYGEN_STRIP_FROM_PATH "${CMAKE_SOURCE_DIR}")
set(DOXYGEN_STRIP_FROM_INC_PATH
    "${CMAKE_SOURCE_DIR}/libs/log/include"
    "${CMAKE_SOURCE_DIR}/libs/db/include"
    "${CMAKE_SOURCE_DIR}/libs/auth/include"
    "${CMAKE_SOURCE_DIR}/libs/pki/include"
    "${CMAKE_SOURCE_DIR}/libs/wire/include"
    "${CMAKE_SOURCE_DIR}/server/src"
    "${CMAKE_SOURCE_DIR}/controller/src"
    "${CMAKE_SOURCE_DIR}/device/src")

set(DOXYGEN_EXTRACT_ALL NO)
set(DOXYGEN_EXTRACT_PRIVATE NO)
set(DOXYGEN_EXTRACT_STATIC YES)
set(DOXYGEN_EXTRACT_ANON_NSPACES NO)
set(DOXYGEN_HIDE_UNDOC_RELATIONS NO)
set(DOXYGEN_WARN_IF_UNDOCUMENTED YES)
set(DOXYGEN_WARN_IF_DOC_ERROR YES)
set(DOXYGEN_WARN_AS_ERROR FAIL_ON_WARNINGS)
set(DOXYGEN_QUIET YES)

# Oat++ controllers declare routes with code-generation macros. Expand only
# these, so each route shows up as a member function named after the endpoint.
set(DOXYGEN_ENABLE_PREPROCESSING YES)
set(DOXYGEN_MACRO_EXPANSION YES)
set(DOXYGEN_EXPAND_ONLY_PREDEF YES)
set(DOXYGEN_SEARCH_INCLUDES NO)
set(DOXYGEN_PREDEFINED
    "ENDPOINT(method,path,name,...)=std::shared_ptr<OutgoingResponse> name()"
    "OATPP_CODEGEN_BEGIN(x)="
    "OATPP_CODEGEN_END(x)=")

set(DOXYGEN_GENERATE_HTML YES)
set(DOXYGEN_GENERATE_LATEX NO)
set(DOXYGEN_GENERATE_TREEVIEW YES)
set(DOXYGEN_SOURCE_BROWSER NO)

if(DOXYGEN_DOT_FOUND)
    set(DOXYGEN_HAVE_DOT YES)
    set(DOXYGEN_CLASS_GRAPH YES)
    set(DOXYGEN_COLLABORATION_GRAPH NO)
    set(DOXYGEN_INCLUDE_GRAPH NO)
    set(DOXYGEN_INCLUDED_BY_GRAPH NO)
    set(DOXYGEN_DOT_IMAGE_FORMAT svg)
else()
    set(DOXYGEN_HAVE_DOT NO)
endif()

doxygen_add_docs(docs
    "${CMAKE_SOURCE_DIR}/README.md"
    "${CMAKE_SOURCE_DIR}/docs/ARCHITECTURE.md"
    "${CMAKE_SOURCE_DIR}/docs/DEVICE_PKI.md"
    "${CMAKE_SOURCE_DIR}/patches/README.md"
    "${CMAKE_SOURCE_DIR}/sql/README.md"
    "${CMAKE_SOURCE_DIR}/tests/e2e/README.md"
    "${CMAKE_SOURCE_DIR}/docs/namespaces.dox"
    "${CMAKE_SOURCE_DIR}/libs/log/include"
    "${CMAKE_SOURCE_DIR}/libs/db/include"
    "${CMAKE_SOURCE_DIR}/libs/auth/include"
    "${CMAKE_SOURCE_DIR}/libs/pki/include"
    "${CMAKE_SOURCE_DIR}/libs/wire/include"
    "${CMAKE_SOURCE_DIR}/server/src"
    "${CMAKE_SOURCE_DIR}/controller/src"
    "${CMAKE_SOURCE_DIR}/device/src"
    COMMENT "Generating Doxygen HTML in ${CMAKE_BINARY_DIR}/docs/html")
