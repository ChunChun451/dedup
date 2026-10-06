# Third-party code, pinned to a release tarball + SHA256 (DECISIONS.md D12).
# To update a dependency: change URL and URL_HASH together, then run `ctest --preset release -R smoke`.
include(FetchContent)
cmake_policy(SET CMP0135 NEW)  # extracted files get the extraction time as timestamp

# BLAKE3 1.8.7 — official C implementation (CC0 / Apache-2.0). Target: BLAKE3::blake3
FetchContent_Declare(blake3
  URL https://github.com/BLAKE3-team/BLAKE3/archive/refs/tags/1.8.7.tar.gz
  URL_HASH SHA256=c6782a28842b1c0478524ac06a4f2ede784038ee298d6e2162c0b089c4306a3c
  SOURCE_SUBDIR c
  EXCLUDE_FROM_ALL SYSTEM)

# zstd 1.5.7 (BSD). Target: libzstd_static
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)  # we run our own threads, one context each (D7)
FetchContent_Declare(zstd
  URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
  URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
  SOURCE_SUBDIR build/cmake
  EXCLUDE_FROM_ALL SYSTEM)

# GoogleTest 1.18.0 (BSD-3). Targets: GTest::gtest_main
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.tar.gz
  URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
  EXCLUDE_FROM_ALL SYSTEM)

# google/benchmark 1.9.5 (Apache-2.0). Target: benchmark::benchmark
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
FetchContent_Declare(benchmark
  URL https://github.com/google/benchmark/archive/refs/tags/v1.9.5.tar.gz
  URL_HASH SHA256=9631341c82bac4a288bef951f8b26b41f69021794184ece969f8473977eaa340
  EXCLUDE_FROM_ALL SYSTEM)

FetchContent_MakeAvailable(blake3 zstd googletest benchmark)
