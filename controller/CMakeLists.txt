add_executable(pgdemo-controller
    src/main.cpp
    src/Manager.cpp
    src/Fleet.cpp
    src/ProcessTable.cpp
    src/Session.cpp
    src/TelemetryWriter.cpp
    src/Spawn.cpp
    src/Origin.cpp
)
find_package(rclcpp REQUIRED)

target_link_libraries(pgdemo-controller PRIVATE pgdemo::db pgdemo::pki pgdemo::log pgdemo::msgs
                      rclcpp::rclcpp)
target_compile_options(pgdemo-controller PRIVATE ${PGDEMO_WARNINGS})
# The default --device-bin is looked up next to the controller.
add_dependencies(pgdemo-controller pgdemo-device)
