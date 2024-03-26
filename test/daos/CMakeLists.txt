# Using GTest to test basic DAOS libraries
# Ref: https://google.github.io/googletest/quickstart-cmake.html

# Process to build and test
# $ cmake -S . -B build
# $ cmake --build build
# $ cd build && ctest

cmake_minimum_required(VERSION 3.10)
project(daos-test)

# GoogleTest requires at least C++14
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/03597a01ee50ed33e9dfd640b249b4be3799d395.zip
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Locate DAOS
find_library(DAOS_LIBRARY
  NAMES libdaos.so.2
  PATHS /usr/lib64
)

# The DAOS header files require `yum install daos-devel` first
find_path(DAOS_INCLUDE_DIR
  NAMES daos.h
  PATHS /usr/include
)

if(DAOS_LIBRARY)
  set(DAOS_LIBRARIES ${DAOS_LIBRARY})
  set(DAOS_INCLUDE_DIRS  ${DAOS_INCLUDE_DIR})
  message(STATUS "DAOS_LIBRARIES = " ${DAOS_LIBRARIES})
else()
  message(FATAL_ERROR "libdaos is required!")
endif()

# Locate GTest
enable_testing()

set(TEST_NAME "daos_test")

add_executable(
  ${TEST_NAME}
  test_daos_version.cc
  test_daos_cont.cc
  test_daos_obj.cc
  test_daos_kv.cc
)

target_link_libraries(
  ${TEST_NAME}
  GTest::gtest_main
  ${DAOS_LIBRARIES}
)

target_include_directories(
  ${TEST_NAME}
  PUBLIC ${DAOS_INCLUDE_DIRS}
)

include(GoogleTest)
gtest_discover_tests(${TEST_NAME})
