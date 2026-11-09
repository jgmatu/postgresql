add_library(pgdemo_pki STATIC
    src/Certificate.cpp
    src/CertificateAuthority.cpp
    src/CertVerifier.cpp
    src/DeviceKey.cpp
)
add_library(pgdemo::pki ALIAS pgdemo_pki)

target_include_directories(pgdemo_pki PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(pgdemo_pki PUBLIC PkgConfig::BOTAN)
target_compile_options(pgdemo_pki PRIVATE ${PGDEMO_WARNINGS})
