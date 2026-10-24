cmake_minimum_required(VERSION 3.20)
project(matching_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(me INTERFACE)
target_include_directories(me INTERFACE include)
target_compile_options(me INTERFACE -Wall -Wextra -Wpedantic)

enable_testing()

add_executable(tests tests/test_main.cpp tests/test_book.cpp tests/test_order_types.cpp tests/test_differential.cpp tests/test_id_map.cpp tests/test_price_bitmap.cpp)
target_link_libraries(tests PRIVATE me)
add_test(NAME tests COMMAND tests)

add_executable(bench bench/bench.cpp)
target_link_libraries(bench PRIVATE me)
target_compile_options(bench PRIVATE -O3)
