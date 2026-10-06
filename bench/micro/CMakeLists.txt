add_executable(bench_micro bench_main.cpp)
target_link_libraries(bench_micro PRIVATE dedup_warnings BLAKE3::blake3 benchmark::benchmark_main)
