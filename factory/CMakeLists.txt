find_package(Threads REQUIRED)

add_executable(pgdemo-factory src/main.cpp src/AsioServer.cpp)
target_link_libraries(pgdemo-factory PRIVATE pgdemo::pki pgdemo::log Boost::headers Threads::Threads)
target_compile_options(pgdemo-factory PRIVATE ${PGDEMO_WARNINGS})
