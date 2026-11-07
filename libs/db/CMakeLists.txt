add_library(pgdemo_db STATIC
    src/DatabaseManager.cpp
    src/Errors.cpp
)
add_library(pgdemo::db ALIAS pgdemo_db)

target_include_directories(pgdemo_db PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(pgdemo_db
    PUBLIC
        libpqxx::pqxx
        Boost::json
        Threads::Threads
    PRIVATE
        pgdemo::log
)
target_compile_options(pgdemo_db PRIVATE ${PGDEMO_WARNINGS})
