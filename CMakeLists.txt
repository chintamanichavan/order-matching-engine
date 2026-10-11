cmake_minimum_required(VERSION 3.25)
project(rtrade
  VERSION 0.1.0
  DESCRIPTION "Order Matching Engine: single-instrument matching engine in C++23"
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(RTRADE_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wnon-virtual-dtor -Wold-style-cast)

option(RTRADE_BUILD_TESTS "Build unit tests" ON)
option(RTRADE_BUILD_BENCH "Build benchmarks"   OFF)
option(RTRADE_WERROR      "Treat warnings as errors" ON)
set(RTRADE_SANITIZE "" CACHE STRING "Sanitizer: address | undefined | thread (empty = off)")

if(RTRADE_WERROR)
  list(APPEND RTRADE_WARNINGS -Werror)
endif()

if(RTRADE_SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address)
elseif(RTRADE_SANITIZE STREQUAL "undefined")
  add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=undefined)
elseif(RTRADE_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

add_library(rtrade STATIC
  src/lob.cpp
  src/simulator.cpp
)
target_include_directories(rtrade PUBLIC include)
target_compile_options(rtrade PRIVATE ${RTRADE_WARNINGS})

if(RTRADE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(RTRADE_BUILD_BENCH)
  add_subdirectory(bench)
endif()
