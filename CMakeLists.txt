cmake_minimum_required(VERSION 3.15)
project(deploy-rollback-cli VERSION 0.1.0 LANGUAGES CXX)

# Require C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Include directories
include_directories(include)

# Fetch yaml-cpp
include(FetchContent)
FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG 0.8.0
)
FetchContent_MakeAvailable(yaml-cpp)

# Fetch doctest for lightweight unit testing
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.11
)
FetchContent_MakeAvailable(doctest)

# Source files
file(GLOB_RECURSE SRC_FILES src/*.cpp)

# Executable target
add_executable(deploy-cli ${SRC_FILES})

# Link yaml-cpp library
target_link_libraries(deploy-cli PRIVATE yaml-cpp)

# Compiler warnings
if(MSVC)
    target_compile_options(deploy-cli PRIVATE /W4)
else()
    target_compile_options(deploy-cli PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ------------------------------------------------------------------------
# Unit tests (doctest). Excludes only main.cpp, since it provides its own
# main() that would collide with doctest's. process_manager.cpp and
# deploy_controller.cpp are included so the fork/exec process-handling logic
# (shell-injection safety, hang-on-backgrounded-grandchild behavior, exit
# code semantics) is actually exercised by tests instead of only compiled.
# ------------------------------------------------------------------------
enable_testing()

add_executable(deploy-cli-tests
    tests/test_main.cpp
    src/config_parser.cpp
    src/logger.cpp
    src/argument_parser.cpp
    src/process_manager.cpp
    src/deploy_controller.cpp
)
target_link_libraries(deploy-cli-tests PRIVATE yaml-cpp doctest::doctest)

include(CTest)
add_test(NAME deploy-cli-tests COMMAND deploy-cli-tests)
