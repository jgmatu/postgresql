find_package(rclcpp REQUIRED)

add_executable(pgdemo-device
    src/main.cpp
    src/AppBootstrap.cpp
    src/Worker.cpp
    src/PufTable.cpp
)
target_link_libraries(pgdemo-device PRIVATE pgdemo::pki pgdemo::factory pgdemo::log pgdemo::msgs rclcpp::rclcpp)
target_compile_options(pgdemo-device PRIVATE ${PGDEMO_WARNINGS})
