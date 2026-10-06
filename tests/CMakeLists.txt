include(GoogleTest)

add_executable(smoke_test smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE dedup_warnings BLAKE3::blake3 libzstd_static GTest::gtest_main)
gtest_discover_tests(smoke_test TEST_PREFIX "smoke.")
