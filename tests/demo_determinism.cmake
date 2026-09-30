# Determinism check for fpga-sim-demo, run by ctest as `cmake -P`.
#
# Runs the demo twice, each in its own fresh working directory, and requires
#   1. the two stdout captures to be byte-identical,
#   2. the two VCD files to be byte-identical,
#   3. stdout to equal the committed golden file byte for byte, and
#   4. the VCD's SHA-256 to equal the committed golden hash.
# Because (3) and (4) compare against files in the repository, every build
# type, compiler and platform that passes this test produced the same bytes.
foreach(var DEMO WORK GOLDEN_STDOUT GOLDEN_VCD_SHA256_FILE)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "demo_determinism.cmake: -D${var}=... is required")
  endif()
endforeach()

set(hashes "")
foreach(run 1 2)
  set(dir "${WORK}/run${run}")
  file(REMOVE_RECURSE "${dir}")
  file(MAKE_DIRECTORY "${dir}")
  execute_process(COMMAND "${DEMO}" WORKING_DIRECTORY "${dir}"
                  OUTPUT_FILE "${dir}/stdout.txt" RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "run ${run}: fpga-sim-demo exited with ${rc}")
  endif()
  if(NOT EXISTS "${dir}/fpga_sim_core.vcd")
    message(FATAL_ERROR "run ${run}: no VCD written")
  endif()
endforeach()

foreach(f stdout.txt fpga_sim_core.vcd)
  execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${WORK}/run1/${f}" "${WORK}/run2/${f}"
                  RESULT_VARIABLE diff)
  if(NOT diff EQUAL 0)
    message(FATAL_ERROR "${f} differs between two runs of the same binary")
  endif()
endforeach()

execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files "${WORK}/run1/stdout.txt" "${GOLDEN_STDOUT}"
                RESULT_VARIABLE diff)
if(NOT diff EQUAL 0)
  file(READ "${WORK}/run1/stdout.txt" got)
  message(FATAL_ERROR "stdout differs from ${GOLDEN_STDOUT}; this build printed:\n${got}")
endif()

file(SHA256 "${WORK}/run1/fpga_sim_core.vcd" vcd_hash)
file(READ "${GOLDEN_VCD_SHA256_FILE}" golden_hash)
string(STRIP "${golden_hash}" golden_hash)
if(NOT vcd_hash STREQUAL golden_hash)
  message(FATAL_ERROR "VCD SHA-256 ${vcd_hash} differs from golden ${golden_hash}")
endif()

file(REMOVE_RECURSE "${WORK}")
message(STATUS "demo stdout and VCD byte-identical across runs and equal to golden (VCD sha256 ${vcd_hash})")
