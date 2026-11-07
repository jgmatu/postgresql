add_library(pgdemo_auth STATIC
    src/PasswordHasher.cpp
    src/JwtManager.cpp
)
add_library(pgdemo::auth ALIAS pgdemo_auth)

target_include_directories(pgdemo_auth PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(pgdemo_auth
    PUBLIC
        PkgConfig::BOTAN
    PRIVATE
        Boost::json
)
target_compile_options(pgdemo_auth PRIVATE ${PGDEMO_WARNINGS})
