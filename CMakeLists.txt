cmake_minimum_required(VERSION 3.20)
project(chargelink VERSION 0.1.0 LANGUAGES CXX)

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR
    "32-bit compiler detected. Run vcvars64.bat in this terminal and delete the build folder.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(Boost REQUIRED)
find_package(libpqxx CONFIG REQUIRED)

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
    target_compile_options(${target} PRIVATE /W4 /bigobj)
    target_compile_definitions(${target} PRIVATE _WIN32_WINNT=0x0A00)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# ---- shared library ----
add_library(chargelink_common STATIC
    src/common/src/frame.cpp
    src/common/src/messages.cpp
    src/common/src/time.cpp
    src/common/src/dispatcher.cpp
    src/common/src/call_tracker.cpp
    src/common/src/reliable_sender.cpp
    src/common/src/backoff.cpp)
target_include_directories(chargelink_common PUBLIC src/common/include)
target_link_libraries(chargelink_common PUBLIC nlohmann_json::nlohmann_json)
chargelink_setup(chargelink_common)

# ---- database layer (PostgreSQL via libpqxx) ----
add_library(chargelink_db STATIC src/db/src/repository.cpp)
target_include_directories(chargelink_db PUBLIC src/db/include)
target_link_libraries(chargelink_db PUBLIC libpqxx::pqxx)
chargelink_setup(chargelink_db)
if(MSVC)
  # libpqxx 8's DLL import lib re-exports std::string_view members that every
  # object file already defines inline (LNK2005). They are identical, so allow it.
  # PUBLIC: anything that links chargelink_db (tests, later csms.exe) gets the flag.
  target_link_options(chargelink_db PUBLIC /FORCE:MULTIPLE)
  # exec_prepared/exec_prepared0 are deprecated in libpqxx 8; silence until Step 22.
  target_compile_options(chargelink_db PRIVATE /wd4996)
endif()

# ---- simulator ----
add_library(chargelink_sim STATIC
    src/simulator/src/connector_fsm.cpp
    src/simulator/src/hardware.cpp
    src/simulator/src/sim_hardware.cpp
    src/simulator/src/charger_controller.cpp
    src/simulator/src/session_bridge.cpp
    src/simulator/src/ocpp_link.cpp
    src/simulator/src/console.cpp
    src/simulator/src/ocpp_client.cpp
    src/simulator/src/reconnecting_client.cpp
    src/simulator/src/station.cpp)
target_include_directories(chargelink_sim PUBLIC src/simulator/include)
target_link_libraries(chargelink_sim PUBLIC chargelink_common)
chargelink_setup(chargelink_sim)

add_executable(report src/report/main.cpp)
target_link_libraries(report PRIVATE chargelink_db)
chargelink_setup(report)


# ---- CSMS: testable core (no Boost networking) and the network layer ----
add_library(chargelink_csms_core STATIC
    src/csms/src/path.cpp
    src/csms/src/charger_registry.cpp
    src/csms/src/state.cpp
    src/csms/src/handlers.cpp
    src/csms/src/db_handlers.cpp)
target_include_directories(chargelink_csms_core PUBLIC src/csms/include)
target_link_libraries(chargelink_csms_core PUBLIC chargelink_common chargelink_db)
chargelink_setup(chargelink_csms_core)

add_library(chargelink_csms_net STATIC
    src/csms/src/session.cpp
    src/csms/src/server.cpp)
target_link_libraries(chargelink_csms_net PUBLIC chargelink_csms_core)
chargelink_setup(chargelink_csms_net)

# ---- executables ----
add_executable(csms src/csms/main.cpp)
target_link_libraries(csms PRIVATE chargelink_csms_net)
chargelink_setup(csms)

add_executable(simulator src/simulator/main.cpp)
target_link_libraries(simulator PRIVATE chargelink_sim chargelink_common)
chargelink_setup(simulator)

# ---- tests ----
enable_testing()
add_executable(chargelink_tests
    tests/test_frame.cpp
    tests/test_messages.cpp
    tests/test_dispatcher.cpp
    tests/test_connector_fsm.cpp
    tests/test_hardware.cpp
    tests/test_charger_controller.cpp
    tests/test_call_tracker.cpp
    tests/test_session_bridge.cpp
    tests/test_console.cpp
    tests/test_ocpp_link.cpp
    tests/test_backoff.cpp
    tests/test_reliable_sender.cpp
    tests/test_csms_path.cpp
    tests/test_charger_registry.cpp
    tests/test_csms_state.cpp
    tests/test_repository.cpp
    tests/test_db_handlers.cpp
    tests/test_queries.cpp
    tests/test_lifecycle.cpp
)

target_link_libraries(chargelink_tests PRIVATE
    chargelink_sim chargelink_csms_core chargelink_db chargelink_common
    GTest::gtest GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(chargelink_tests)