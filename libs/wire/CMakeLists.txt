add_library(pgdemo_wire STATIC src/Frame.cpp)
add_library(pgdemo::wire ALIAS pgdemo_wire)

target_include_directories(pgdemo_wire PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(pgdemo_wire PUBLIC Boost::json Boost::headers)
target_compile_options(pgdemo_wire PRIVATE ${PGDEMO_WARNINGS})
