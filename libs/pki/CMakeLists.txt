find_package(OpenSSL REQUIRED)

add_library(pgdemo_pki STATIC
    src/Certificate.cpp
    src/CertificateAuthority.cpp
    src/CertVerifier.cpp
    src/DeviceKey.cpp
    src/PufTable.cpp
    src/Handshake.cpp
    src/FactoryChannel.cpp
    src/SecurityPolicy.cpp
)
add_library(pgdemo::pki ALIAS pgdemo_pki)

target_include_directories(pgdemo_pki PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(pgdemo_pki PUBLIC PkgConfig::BOTAN OpenSSL::SSL OpenSSL::Crypto)
target_compile_options(pgdemo_pki PRIVATE ${PGDEMO_WARNINGS})
