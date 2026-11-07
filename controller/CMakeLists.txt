add_executable(pgdemo-controller
    src/main.cpp
    src/Producer.cpp
)
target_link_libraries(pgdemo-controller PRIVATE pgdemo::db pgdemo::log)
target_compile_options(pgdemo-controller PRIVATE ${PGDEMO_WARNINGS})
