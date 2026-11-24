cmake_minimum_required(VERSION 3.16)
project(deploy-rollback-cli VERSION 0.3.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# compile_commands.json lets clangd / VS Code IntelliSense / clang-tidy see the
# real compiler flags (e.g. -std=c++17), instead of guessing.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(DEPLOYGUARD_BUILD_TESTS "Build unit and integration tests" ON)
option(DEPLOYGUARD_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(DEPLOYGUARD_WERROR "Treat compiler warnings as errors" OFF)

if(DEPLOYGUARD_SANITIZE)
    # Applied globally (before FetchContent) so dependencies are instrumented too.
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

# ------------------------------------------------------------------------
# Dependencies
# ------------------------------------------------------------------------
include(FetchContent)

# We only need the yaml-cpp library itself, not its tests, tools or install rules.
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
set(YAML_CPP_FORMAT_SOURCE OFF CACHE BOOL "" FORCE)

# 0.9.0 is required: 0.8.0 does not #include <cstdint> and fails to compile on
# newer GCC releases (this is what broke CI on ubuntu-latest).
FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG        yaml-cpp-0.9.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(yaml-cpp)

# Treat third-party headers as system headers: our strict warning flags then
# apply only to our own code, not to code we do not control.
function(deployguard_mark_headers_as_system target)
    get_target_property(dirs ${target} INTERFACE_INCLUDE_DIRECTORIES)
    if(dirs)
        set_target_properties(${target} PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${dirs}")
    endif()
endfunction()
deployguard_mark_headers_as_system(yaml-cpp)

if(DEPLOYGUARD_BUILD_TESTS)
    set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(DOCTEST_NO_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG        v2.5.3
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(doctest)
    deployguard_mark_headers_as_system(doctest)
endif()

# ------------------------------------------------------------------------
# Warnings (applied to our own targets only, not to third-party code)
# ------------------------------------------------------------------------
function(deployguard_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
            -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual)
    endif()
    if(DEPLOYGUARD_WERROR)
        target_compile_options(${target} PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/WX,-Werror>)
    endif()
endfunction()

# ------------------------------------------------------------------------
# Core library: everything except main(). Shared by the CLI and the tests, so
# the test binary exercises exactly the code that ships.
# ------------------------------------------------------------------------
add_library(deployguard_core STATIC
    src/argument_parser.cpp
    src/config_parser.cpp
    src/deploy_controller.cpp
    src/file_lock.cpp
    src/logger.cpp
    src/process_manager.cpp
    src/signals.cpp
    src/state_store.cpp
)
target_include_directories(deployguard_core PUBLIC include)
target_link_libraries(deployguard_core PUBLIC yaml-cpp::yaml-cpp)
target_compile_definitions(deployguard_core PUBLIC DEPLOYGUARD_VERSION="${PROJECT_VERSION}")
deployguard_set_warnings(deployguard_core)

add_executable(deploy-cli src/main.cpp)
target_link_libraries(deploy-cli PRIVATE deployguard_core)
deployguard_set_warnings(deploy-cli)

# ------------------------------------------------------------------------
# Tests
#   - Unit tests (doctest): one CTest entry per TEST_CASE, so `ctest -j`
#     runs them in parallel. Each test works in its own temporary directory.
#   - Integration tests (bash, label "integration"): drive the real
#     deploy-cli binary end to end - SIGKILL mid-deployment, concurrent runs,
#     Ctrl+C, timeouts, and the bundled demo.
# ------------------------------------------------------------------------
if(DEPLOYGUARD_BUILD_TESTS)
    enable_testing()

    add_executable(deploy-cli-tests
        tests/test_main.cpp
        tests/test_argument_parser.cpp
        tests/test_config_parser.cpp
        tests/test_deploy_controller.cpp
        tests/test_file_lock.cpp
        tests/test_logger.cpp
        tests/test_process_manager.cpp
        tests/test_signals.cpp
        tests/test_state_store.cpp
    )
    target_link_libraries(deploy-cli-tests PRIVATE deployguard_core doctest::doctest)
    deployguard_set_warnings(deploy-cli-tests)

    include(${doctest_SOURCE_DIR}/scripts/cmake/doctest.cmake)
    doctest_discover_tests(deploy-cli-tests PROPERTIES LABELS unit)

    find_program(BASH_EXECUTABLE bash)
    if(BASH_EXECUTABLE)
        foreach(name cli_contract crash_recovery concurrent_runs cancellation timeout demo_end_to_end)
            add_test(NAME integration.${name}
                     COMMAND ${BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/test_${name}.sh
                             $<TARGET_FILE:deploy-cli> ${CMAKE_CURRENT_SOURCE_DIR})
            set_tests_properties(integration.${name} PROPERTIES LABELS integration TIMEOUT 180)
        endforeach()
    else()
        message(WARNING "bash not found: integration tests disabled")
    endif()
endif()
