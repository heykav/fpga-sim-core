cmake_minimum_required(VERSION 3.20)
project(fpga-sim-core VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FPGA_SIM_WERROR "Treat compiler warnings as errors for this project's own targets" ON)

add_library(fpga-sim-core INTERFACE)
target_include_directories(fpga-sim-core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(fpga-sim-core INTERFACE -Wall -Wextra -Wpedantic)

# Stricter warnings for the executables built here (demo + tests). Kept off the
# INTERFACE library so that consumers of the headers keep their own settings.
add_library(fpga-sim-warnings INTERFACE)
target_compile_options(fpga-sim-warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast
  -Wcast-align)
if(FPGA_SIM_WERROR)
  target_compile_options(fpga-sim-warnings INTERFACE -Werror)
endif()

add_executable(fpga-sim-demo main.cpp)
target_link_libraries(fpga-sim-demo PRIVATE fpga-sim-core fpga-sim-warnings)

enable_testing()
add_test(NAME fpga-sim-demo COMMAND fpga-sim-demo)

foreach(t phy-pcs parser-itch50 itch-roundtrip discrete-engine order-book book-invariants
          vcd-logger hot-path-alloc ofi pipeline-golden)
  string(REPLACE "-" "_" src ${t})
  add_executable(test-${t} tests/test_${src}.cpp)
  target_include_directories(test-${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(test-${t} PRIVATE fpga-sim-core fpga-sim-warnings)
  add_test(NAME test-${t} COMMAND test-${t})
endforeach()

# Runs the demo twice in separate directories and requires byte-identical
# stdout and VCD, equal to the committed golden files (so every build type and
# platform that passes this test produces the same bytes).
add_test(NAME demo-determinism
  COMMAND ${CMAKE_COMMAND}
    -DDEMO=$<TARGET_FILE:fpga-sim-demo>
    -DWORK=${CMAKE_CURRENT_BINARY_DIR}/determinism
    -DGOLDEN_STDOUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/fpga-sim-demo.stdout
    -DGOLDEN_VCD_SHA256_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/fpga_sim_core.vcd.sha256
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/demo_determinism.cmake)
