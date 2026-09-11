cmake_minimum_required(VERSION 3.20)
project(fpga-sim-core VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(fpga-sim-core INTERFACE)
target_include_directories(fpga-sim-core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(fpga-sim-core INTERFACE -Wall -Wextra -Wpedantic)

add_executable(fpga-sim-demo main.cpp)
target_link_libraries(fpga-sim-demo PRIVATE fpga-sim-core)

enable_testing()
add_test(NAME fpga-sim-demo COMMAND fpga-sim-demo)
