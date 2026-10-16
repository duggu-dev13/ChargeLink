cmake_minimum_required(VERSION 3.20)
project(chargelink VERSION 0.1.0 LANGUAGES CXX)

# Fail early with a clear message if the terminal is 32-bit
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR
    "32-bit compiler detected. Run vcvars64.bat in this terminal and delete the build folder.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(Boost REQUIRED)

# pthreads is not used on Windows
if(NOT WIN32)
  find_package(Threads REQUIRED)
endif()

function(chargelink_setup target)
  target_link_libraries(${target} PRIVATE
      Boost::headers
      spdlog::spdlog
      nlohmann_json::nlohmann_json)
  if(NOT WIN32)
    target_link_libraries(${target} PRIVATE Threads::Threads)
  endif()
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
    target_compile_definitions(${target} PRIVATE _WIN32_WINNT=0x0A00)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# ---- shared library: OCPP frames, messages, dispatcher, utilities ----
add_library(chargelink_common STATIC
    src/common/src/frame.cpp
    src/common/src/messages.cpp
    src/common/src/time.cpp
    src/common/src/dispatcher.cpp)
target_include_directories(chargelink_common PUBLIC src/common/include)
target_link_libraries(chargelink_common PUBLIC nlohmann_json::nlohmann_json)
chargelink_setup(chargelink_common)

# ---- simulator logic (state machine now; more later) ----
add_library(chargelink_sim STATIC src/simulator/src/connector_fsm.cpp)
target_include_directories(chargelink_sim PUBLIC src/simulator/include)
target_link_libraries(chargelink_sim PUBLIC chargelink_common)
chargelink_setup(chargelink_sim)

# ---- executables ----
add_executable(csms src/csms/main.cpp)
target_link_libraries(csms PRIVATE chargelink_common)
chargelink_setup(csms)

add_executable(simulator src/simulator/main.cpp)
target_link_libraries(simulator PRIVATE chargelink_common)
chargelink_setup(simulator)

# ---- tests ----
enable_testing()
add_executable(chargelink_tests
    tests/test_frame.cpp
    tests/test_messages.cpp
    tests/test_dispatcher.cpp
    tests/test_connector_fsm.cpp)
target_link_libraries(chargelink_tests PRIVATE
    chargelink_sim chargelink_common GTest::gtest GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(chargelink_tests)