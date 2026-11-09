add_executable(pgdemo-controller
    src/main.cpp
    src/Fleet.cpp
    src/Spawn.cpp
)
target_link_libraries(pgdemo-controller PRIVATE pgdemo::db pgdemo::pki pgdemo::wire pgdemo::log)
target_compile_options(pgdemo-controller PRIVATE ${PGDEMO_WARNINGS})
# The default --device-bin is looked up next to the controller.
add_dependencies(pgdemo-controller pgdemo-device)
