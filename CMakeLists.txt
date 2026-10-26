cmake_minimum_required(VERSION 3.20)
project(matching_engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(ME_SANITIZE "build tests with address and undefined behaviour sanitizers" OFF)

add_library(me INTERFACE)
target_include_directories(me INTERFACE include)
target_compile_options(me INTERFACE -Wall -Wextra -Wpedantic)

enable_testing()

find_package(Threads REQUIRED)

add_executable(tests tests/test_main.cpp tests/test_book.cpp tests/test_order_types.cpp tests/test_differential.cpp tests/test_id_map.cpp tests/test_price_bitmap.cpp tests/test_spsc_ring.cpp tests/test_itch.cpp tests/test_backtest.cpp)
target_link_libraries(tests PRIVATE me Threads::Threads)
if(ME_SANITIZE)
  target_compile_options(tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_link_options(tests PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME tests COMMAND tests)

add_executable(bench bench/bench.cpp)
target_link_libraries(bench PRIVATE me)
target_compile_options(bench PRIVATE -O3)

add_executable(pipeline bench/pipeline.cpp)
target_link_libraries(pipeline PRIVATE me Threads::Threads)
target_compile_options(pipeline PRIVATE -O3)

# ITCH 5.0 replay: rebuilds every stock's book from a Nasdaq day file (raw or .gz)
find_package(ZLIB REQUIRED)
add_executable(itch_replay tools/itch_replay.cpp)
target_link_libraries(itch_replay PRIVATE me ZLIB::ZLIB)
target_compile_options(itch_replay PRIVATE -O3)

# passive market making backtest on the rebuilt order by order books
add_executable(mm_backtest tools/mm_backtest.cpp)
target_link_libraries(mm_backtest PRIVATE me ZLIB::ZLIB)
target_compile_options(mm_backtest PRIVATE -O3)
