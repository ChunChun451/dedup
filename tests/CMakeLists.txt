include(GoogleTest)

add_executable(smoke_test smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE dedup_warnings BLAKE3::blake3 libzstd_static GTest::gtest_main)
gtest_discover_tests(smoke_test TEST_PREFIX "smoke.")

add_executable(gen_golden_test gen_golden_test.cpp)
target_link_libraries(gen_golden_test PRIVATE dedup_warnings dedup_gen BLAKE3::blake3 libzstd_static GTest::gtest_main)
gtest_discover_tests(gen_golden_test TEST_PREFIX "gen_golden.")
