project(pgdemo_msgs LANGUAGES C CXX)

find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)
find_package(rosidl_default_runtime REQUIRED)

set(PGDEMO_IDL
    msg/Hello.idl
    msg/Heartbeat.idl
    msg/Error.idl
    msg/Bye.idl
    msg/Csr.idl
    msg/Certificate.idl
    msg/Rejected.idl
    msg/Challenge.idl
    msg/Proof.idl
    msg/Config.idl
    msg/ScalarSample.idl
    msg/GpsSample.idl
    msg/XySample.idl)

# SKIP_INSTALL: this tree is not an ament package. The binaries link the
# generated type support directly, and the image copies those libraries.
rosidl_generate_interfaces(${PROJECT_NAME} ${PGDEMO_IDL} SKIP_INSTALL)

rosidl_get_typesupport_target(pgdemo_msgs_cpp ${PROJECT_NAME} "rosidl_typesupport_cpp")
rosidl_get_typesupport_target(pgdemo_msgs_fastrtps ${PROJECT_NAME} "rosidl_typesupport_fastrtps_cpp")
rosidl_get_typesupport_target(pgdemo_msgs_introspection ${PROJECT_NAME} "rosidl_typesupport_introspection_cpp")

add_library(pgdemo_msgs_link INTERFACE)
add_library(pgdemo::msgs ALIAS pgdemo_msgs_link)
# The C++ type support dlopens the Fast DDS and introspection libraries by
# soname. --no-as-needed keeps them in NEEDED, so the executable RUNPATH loads
# them before the dlopen and a local ./build/device/pgdemo-device starts
# without extra paths.
target_link_libraries(pgdemo_msgs_link INTERFACE
    ${pgdemo_msgs_cpp}
    -Wl,--no-as-needed
    ${pgdemo_msgs_fastrtps}
    ${pgdemo_msgs_introspection}
    -Wl,--as-needed)
target_include_directories(pgdemo_msgs_link INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
