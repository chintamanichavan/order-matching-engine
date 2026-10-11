include(FetchContent)

FetchContent_Declare(
  benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG        v1.9.0
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(benchmark)

add_executable(rtrade_bench
  lob_bench.cpp
)
target_link_libraries(rtrade_bench PRIVATE rtrade benchmark::benchmark)
target_compile_options(rtrade_bench PRIVATE ${RTRADE_WARNINGS})
