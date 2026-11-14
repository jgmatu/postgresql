find_package(Threads REQUIRED)

add_library(pgdemo_factory STATIC
    src/FactoryChannel.cpp
    src/SecurityPolicy.cpp
)
add_library(pgdemo::factory ALIAS pgdemo_factory)

target_include_directories(pgdemo_factory PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(pgdemo_factory PUBLIC pgdemo::pki)
target_compile_options(pgdemo_factory PRIVATE ${PGDEMO_WARNINGS})

add_executable(pgdemo-factory src/main.cpp src/AsioServer.cpp)
target_link_libraries(pgdemo-factory PRIVATE pgdemo::factory pgdemo::log Boost::headers Threads::Threads)
target_compile_options(pgdemo-factory PRIVATE ${PGDEMO_WARNINGS})
