cmake_minimum_required(VERSION 3.15)
project(deploy-rollback-cli VERSION 0.1.0 LANGUAGES CXX)

# Require C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Include directories
include_directories(include)

# Source files
file(GLOB_RECURSE SRC_FILES src/*.cpp)

# Executable target
add_executable(deploy-cli ${SRC_FILES})

# Compiler warnings
if(MSVC)
    target_compile_options(deploy-cli PRIVATE /W4)
else()
    target_compile_options(deploy-cli PRIVATE -Wall -Wextra -Wpedantic)
endif()
