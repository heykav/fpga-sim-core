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

add_executable(test-phy-pcs tests/test_phy_pcs.cpp)
target_link_libraries(test-phy-pcs PRIVATE fpga-sim-core)
add_test(NAME test-phy-pcs COMMAND test-phy-pcs)

add_executable(test-parser-itch50 tests/test_parser_itch50.cpp)
target_link_libraries(test-parser-itch50 PRIVATE fpga-sim-core)
add_test(NAME test-parser-itch50 COMMAND test-parser-itch50)
