add_library(dedup_gen STATIC gen.cpp)
target_include_directories(dedup_gen PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(dedup_gen PRIVATE dedup_warnings)

add_executable(dedup-gen main.cpp)
target_link_libraries(dedup-gen PRIVATE dedup_gen dedup_warnings)
