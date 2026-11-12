add_executable(pgdemo-factory src/main.cpp)
target_link_libraries(pgdemo-factory PRIVATE pgdemo::pki pgdemo::log)
target_compile_options(pgdemo-factory PRIVATE ${PGDEMO_WARNINGS})
