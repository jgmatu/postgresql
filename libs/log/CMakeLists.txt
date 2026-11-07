add_library(pgdemo_log INTERFACE)
add_library(pgdemo::log ALIAS pgdemo_log)
target_include_directories(pgdemo_log INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
