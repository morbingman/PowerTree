# PowerTree — test target
add_executable(powertree_tests
    test_domain.cpp
)

target_include_directories(powertree_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(powertree_tests PRIVATE
    Catch2::Catch2WithMain
)

# Warnings-as-errors for tests too.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(powertree_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

catch_discover_tests(powertree_tests)
