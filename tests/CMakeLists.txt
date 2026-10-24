# tests/CMakeLists.txt — runtime suite + compile-fail cases as ctest tests

add_executable(test_planner test_planner.cpp)
target_link_libraries(test_planner PRIVATE flexplanner::flexplanner)
add_test(NAME planner_suite COMMAND test_planner)

# ---------------------------------------------------------------------------
# Compile-fail tests: each case must FAIL to compile and (on Clang/GCC) the
# diagnostics must contain the expected text.  Implemented as `cmake -P`
# script tests that re-run the configured compiler on the single source.
# ---------------------------------------------------------------------------
function(flexplanner_add_compile_fail_test name expected_text)
    if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
        set(std_flag /std:c++20)
        set(compile_only_flag /c)
    else()
        set(std_flag -std=c++20)
        set(compile_only_flag -c)
    endif()

    add_test(NAME compile_fail_${name}
        COMMAND ${CMAKE_COMMAND}
            -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
            "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
            -DSTD_FLAG=${std_flag}
            -DCOMPILE_ONLY_FLAG=${compile_only_flag}
            -DTEST_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/${name}.cpp
            "-DEXPECTED_TEXT=${expected_text}"
            -DCOMPILER_ID=${CMAKE_CXX_COMPILER_ID}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/compile_fail_check.cmake)
endfunction()

flexplanner_add_compile_fail_test(cycle_handle "Cyclic handle dependency")
flexplanner_add_compile_fail_test(cycle_eval   "Cyclic eval dependency")
flexplanner_add_compile_fail_test(absent_dep   "Dependency absent from Planner")
