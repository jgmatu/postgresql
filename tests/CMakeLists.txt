find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(auth_tests unit/auth_tests.cpp)
target_link_libraries(auth_tests PRIVATE pgdemo::auth GTest::gtest_main)
target_compile_options(auth_tests PRIVATE ${PGDEMO_WARNINGS})
gtest_discover_tests(auth_tests)

add_executable(dispatcher_tests unit/dispatcher_tests.cpp)
target_link_libraries(dispatcher_tests PRIVATE pgdemo::dispatcher GTest::gtest_main)
target_compile_options(dispatcher_tests PRIVATE ${PGDEMO_WARNINGS})
gtest_discover_tests(dispatcher_tests)

# Needs a database prepared by scripts/db-init.sh; see the header of db_tests.cpp.
add_executable(db_tests db/db_tests.cpp)
target_link_libraries(db_tests PRIVATE pgdemo::db pgdemo::dispatcher GTest::gtest_main)
target_compile_options(db_tests PRIVATE ${PGDEMO_WARNINGS})
gtest_discover_tests(db_tests PROPERTIES LABELS db)
