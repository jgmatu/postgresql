find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(auth_tests unit/auth_tests.cpp)
target_link_libraries(auth_tests PRIVATE pgdemo::auth GTest::gtest_main)
target_compile_options(auth_tests PRIVATE ${PGDEMO_WARNINGS})
gtest_discover_tests(auth_tests)

add_executable(pki_tests unit/pki_tests.cpp)
target_link_libraries(pki_tests PRIVATE pgdemo::pki GTest::gtest_main)
target_compile_options(pki_tests PRIVATE ${PGDEMO_WARNINGS})
gtest_discover_tests(pki_tests)

add_executable(factory_tests unit/factory_tests.cpp)
target_link_libraries(factory_tests PRIVATE pgdemo::factory GTest::gtest_main)
target_compile_options(factory_tests PRIVATE ${PGDEMO_WARNINGS})
gtest_discover_tests(factory_tests)

add_executable(device_puf_tests unit/device_puf_tests.cpp ${PROJECT_SOURCE_DIR}/device/src/PufTable.cpp)
target_include_directories(device_puf_tests PRIVATE ${PROJECT_SOURCE_DIR}/device/src)
target_link_libraries(device_puf_tests PRIVATE pgdemo::pki GTest::gtest_main)
target_compile_options(device_puf_tests PRIVATE ${PGDEMO_WARNINGS})
gtest_discover_tests(device_puf_tests)

add_executable(wire_tests unit/wire_tests.cpp)
target_include_directories(wire_tests PRIVATE ${PROJECT_SOURCE_DIR}/device/src)
target_link_libraries(wire_tests PRIVATE pgdemo::msgs pgdemo::log GTest::gtest_main)
target_compile_options(wire_tests PRIVATE ${PGDEMO_WARNINGS})
gtest_discover_tests(wire_tests)

add_executable(dispatcher_tests unit/dispatcher_tests.cpp)
target_link_libraries(dispatcher_tests PRIVATE pgdemo::dispatcher GTest::gtest_main)
target_compile_options(dispatcher_tests PRIVATE ${PGDEMO_WARNINGS})
gtest_discover_tests(dispatcher_tests)

add_executable(static_site_tests unit/static_site_tests.cpp)
target_include_directories(static_site_tests PRIVATE ${PROJECT_SOURCE_DIR}/server/src)
target_link_libraries(static_site_tests PRIVATE GTest::gtest_main)
target_compile_options(static_site_tests PRIVATE ${PGDEMO_WARNINGS})
gtest_discover_tests(static_site_tests)

add_executable(connection_pool_tests unit/connection_pool_tests.cpp)
target_link_libraries(connection_pool_tests PRIVATE pgdemo::db GTest::gtest_main GTest::gmock)
target_compile_options(connection_pool_tests PRIVATE ${PGDEMO_WARNINGS})
gtest_discover_tests(connection_pool_tests)

# Needs a database prepared by scripts/db-init.sh; see the header of db_tests.cpp.
add_executable(db_tests db/db_tests.cpp)
target_link_libraries(db_tests PRIVATE pgdemo::db pgdemo::dispatcher pgdemo::pki GTest::gtest_main)
target_compile_definitions(db_tests PRIVATE PGDEMO_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_compile_options(db_tests PRIVATE ${PGDEMO_WARNINGS})
gtest_discover_tests(db_tests PROPERTIES LABELS db)
