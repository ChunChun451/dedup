add_executable(membw membw.cpp)
target_link_libraries(membw PRIVATE dedup_warnings)

add_executable(calib calib.cpp)
target_link_libraries(calib PRIVATE dedup_warnings)
