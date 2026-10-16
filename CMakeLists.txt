cmake_minimum_required(VERSION 3.20)
project(chargelink VERSION 0.1.0 LANGUAGES CXX)
find_package(Boost REQUIRED)
find_package(Threads REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(chargelink src/main.cpp)

target_link_libraries(chargelink PRIVATE
    Boost::headers
    Threads::Threads
    spdlog::spdlog
    nlohmann_json::nlohmann_json)

if(MSVC)
  target_compile_options(chargelink PRIVATE /W4)
else()
  target_compile_options(chargelink PRIVATE -Wall -Wextra -Wpedantic)
endif()