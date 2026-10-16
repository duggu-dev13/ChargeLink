cmake_minimum_required(VERSION 3.20)
project(chargelink VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost REQUIRED)
find_package(Threads REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)

function(chargelink_setup target)
  target_link_libraries(${target} PRIVATE
      Boost::headers
      Threads::Threads
      spdlog::spdlog
      nlohmann_json::nlohmann_json)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
    target_compile_definitions(${target} PRIVATE _WIN32_WINNT=0x0A00)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# ---- shared library: OCPP frames (and later all messages) ----
add_library(chargelink_common STATIC src/common/src/frame.cpp)
target_include_directories(chargelink_common PUBLIC src/common/include)
target_link_libraries(chargelink_common PUBLIC nlohmann_json::nlohmann_json)
chargelink_setup(chargelink_common)

# ---- executables ----
add_executable(csms src/csms/main.cpp)
target_link_libraries(csms PRIVATE chargelink_common)
chargelink_setup(csms)

add_executable(simulator src/simulator/main.cpp)
target_link_libraries(simulator PRIVATE chargelink_common)
chargelink_setup(simulator)

# ---- tests ----
enable_testing()
add_executable(chargelink_tests tests/test_frame.cpp)
target_link_libraries(chargelink_tests PRIVATE chargelink_common GTest::gtest GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(chargelink_tests)