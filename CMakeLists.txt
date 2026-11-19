cmake_minimum_required(VERSION 3.20)
project(orderbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

option(OB_SANITIZE "Build with address and undefined-behaviour sanitizers" OFF)
if(OB_SANITIZE AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(orderbook src/order_book.cpp)
target_include_directories(orderbook PUBLIC include)

add_executable(ob_tests tests/test_order_book.cpp)
target_link_libraries(ob_tests PRIVATE orderbook)

add_executable(ob_bench bench/bench.cpp)
target_link_libraries(ob_bench PRIVATE orderbook)

enable_testing()
add_test(NAME ob_tests COMMAND ob_tests)
