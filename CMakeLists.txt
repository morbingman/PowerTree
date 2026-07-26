# PowerTree — top-level CMake build
# Toolchain: CMake + vcpkg manifest mode, GCC 13.4 / MinGW-w64, C++23.
#
# Configure (Windows, with a vcpkg environment):
#   cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#   cmake --build build
# Run tests:
#   ctest --test-dir build --output-on-failure

cmake_minimum_required(VERSION 3.28)

project(PowerTree
    VERSION 0.1.0
    DESCRIPTION "PowerTree — a C++ CLI to-do application"
    LANGUAGES CXX)

# --- C++23, no extensions ---------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Warnings (treat warnings as errors in the app + tests) ----------------
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

# --- vcpkg dependencies -----------------------------------------------------
find_package(fmt CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Catch2 3 CONFIG REQUIRED)

# --- App target -------------------------------------------------------------
# Sources are added layer-by-layer as each is implemented. Today only main.cpp
# + header-only Domain/Core are wired in.
add_executable(powertree
    src/main.cpp
)

target_include_directories(powertree PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(powertree PRIVATE
    fmt::fmt
    CLI11::CLI11
    nlohmann_json::nlohmann_json
)

# --- Tests ------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
