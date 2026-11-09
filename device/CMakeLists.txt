add_executable(pgdemo-device src/main.cpp)
target_link_libraries(pgdemo-device PRIVATE pgdemo::pki pgdemo::wire pgdemo::log)
target_compile_options(pgdemo-device PRIVATE ${PGDEMO_WARNINGS})
