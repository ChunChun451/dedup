cmake_minimum_required(VERSION 3.28)
project(dedup VERSION 0.0.1 LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

# Warnings for our own code only (third-party code is built without -Werror).
add_library(dedup_warnings INTERFACE)
target_compile_options(dedup_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)

include(cmake/Dependencies.cmake)

configure_file(src/version.hpp.in ${CMAKE_BINARY_DIR}/generated/version.hpp @ONLY)

add_executable(dedup src/main.cpp)
target_include_directories(dedup PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(dedup PRIVATE dedup_warnings)

enable_testing()
add_subdirectory(tests)
add_subdirectory(bench/micro)
add_subdirectory(bench/hw)
