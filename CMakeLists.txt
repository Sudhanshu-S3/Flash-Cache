cmake_minimum_required(VERSION 3.10)
project(FlashCache VERSION 1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

include_directories(include)

file(GLOB SOURCES "src/*.cpp")

add_executable(flash_cache ${SOURCES})

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(flash_cache PRIVATE -O3 -march=native -Wall -Wextra)
endif()


# --- Testing Section ---
enable_testing()

# Download GoogleTest 
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/heads/main.zip
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(flash_test tests/test_main.cpp)

target_link_libraries(flash_test PRIVATE GTest::gtest_main)
target_include_directories(flash_test PRIVATE include)

include(GoogleTest)
gtest_discover_tests(flash_test)