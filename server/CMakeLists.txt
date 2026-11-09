# Correlation and fan-out primitives, without Oat++, so the tests can link them.
add_library(pgdemo_dispatcher STATIC
    src/dispatcher/Dispatcher.cpp
    src/events/EventHub.cpp
)
add_library(pgdemo::dispatcher ALIAS pgdemo_dispatcher)
target_include_directories(pgdemo_dispatcher PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pgdemo_dispatcher PUBLIC Threads::Threads PRIVATE pgdemo::log)
target_compile_options(pgdemo_dispatcher PRIVATE ${PGDEMO_WARNINGS})

add_executable(pgdemo-server src/main.cpp)
target_include_directories(pgdemo-server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pgdemo-server PRIVATE
    pgdemo::dispatcher
    pgdemo::db
    pgdemo::auth
    pgdemo::pki
    pgdemo::log
    oatpp
    oatpp-openssl
)
target_compile_options(pgdemo-server PRIVATE ${PGDEMO_WARNINGS})

# The process loads these by path: OPENSSL_CONF and OPENSSL_MODULES are the
# directory of the executable. The provider is a MODULE, not a linked library.
add_custom_command(TARGET pgdemo-server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:oqsprovider>"
            "$<TARGET_FILE_DIR:pgdemo-server>/oqsprovider.so"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/openssl-oqs.cnf"
            "$<TARGET_FILE_DIR:pgdemo-server>/openssl-oqs.cnf"
    VERBATIM)
