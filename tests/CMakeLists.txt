add_executable(queue_test queue_test.cc)
target_link_libraries(queue_test PRIVATE project_settings jglockfree GTest::gtest_main)

gtest_discover_tests(queue_test)