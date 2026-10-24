cmake_minimum_required(VERSION 3.16)

project(flexplanner
    VERSION 1.0.0
    DESCRIPTION "Header-only C++20 compile-time event-driven feature planner (Flex Model style)"
    LANGUAGES CXX)

option(FLEXPLANNER_BUILD_TESTS "Build the test suite" ON)
option(FLEXPLANNER_BUILD_EXAMPLES "Build the examples" ON)

# ---- library: header-only INTERFACE target --------------------------------
add_library(flexplanner INTERFACE)
add_library(flexplanner::flexplanner ALIAS flexplanner)

target_include_directories(flexplanner INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

# consteval, concepts, template lambdas
target_compile_features(flexplanner INTERFACE cxx_std_20)

# ---- tests & examples ------------------------------------------------------
if(FLEXPLANNER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(FLEXPLANNER_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ---- install & export ------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS flexplanner EXPORT flexplannerTargets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT flexplannerTargets
    FILE flexplannerTargets.cmake
    NAMESPACE flexplanner::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/flexplanner)

configure_package_config_file(
    cmake/flexplannerConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/flexplannerConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/flexplanner)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/flexplannerConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/flexplannerConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/flexplannerConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/flexplanner)
