# compile_fail_check.cmake — ctest script for compile-fail cases.
# Run via `cmake -P` with:
#   CXX_COMPILER, CXX_FLAGS, STD_FLAG, COMPILE_ONLY_FLAG, TEST_SOURCE,
#   EXPECTED_TEXT, COMPILER_ID
# Passes when: compilation FAILS, and the diagnostics contain EXPECTED_TEXT
# (the text check is enforced on Clang/GCC; MSVC wording differs, so there
# only the failure itself is required).

separate_arguments(flags NATIVE_COMMAND "${CXX_FLAGS}")

execute_process(
    COMMAND ${CXX_COMPILER} ${flags} ${STD_FLAG} ${COMPILE_ONLY_FLAG} ${TEST_SOURCE}
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)

if(result EQUAL 0)
    message(FATAL_ERROR "compile-fail case compiled successfully but must fail: ${TEST_SOURCE}")
endif()

if(NOT COMPILER_ID MATCHES "MSVC")
    string(FIND "${stdout}${stderr}" "${EXPECTED_TEXT}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "compilation failed as required, but diagnostics do not contain '${EXPECTED_TEXT}':\n${stdout}${stderr}")
    endif()
endif()

message(STATUS "compile-fail case OK: ${TEST_SOURCE} (failed to compile as expected)")

# the probe compile drops an object file named after the source in this
# directory (no -o given); clean it up so the source tree stays pristine
get_filename_component(_stem ${TEST_SOURCE} NAME_WE)
file(REMOVE ${CMAKE_CURRENT_LIST_DIR}/${_stem}.obj ${CMAKE_CURRENT_LIST_DIR}/${_stem}.o)
