include(FetchContent)

# GoogleTest pinned. Single source of truth for the version.
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.2
)
set(BUILD_GMOCK     OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(rtrade_tests
  lob_test.cpp
  simulator_test.cpp
)
target_link_libraries(rtrade_tests PRIVATE rtrade GTest::gtest_main)
target_compile_options(rtrade_tests PRIVATE ${RTRADE_WARNINGS})

include(GoogleTest)
gtest_discover_tests(rtrade_tests)
