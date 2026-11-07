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
    pgdemo::log
    oatpp
)
target_compile_options(pgdemo-server PRIVATE ${PGDEMO_WARNINGS})
