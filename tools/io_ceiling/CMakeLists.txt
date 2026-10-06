add_executable(io_ceiling main.cpp)
target_link_libraries(io_ceiling PRIVATE dedup_warnings)
# Put the binary at build/<preset>/io_ceiling, where the roadmap checks expect it ($B/io_ceiling).
set_target_properties(io_ceiling PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
