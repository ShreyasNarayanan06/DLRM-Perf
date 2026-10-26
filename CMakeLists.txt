cmake_minimum_required(VERSION 3.12)
project(radix_coherence LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)

add_library(coherence
    src/region.cpp
    src/predictor.cpp
    src/workload.cpp
    src/simulator.cpp)
target_include_directories(coherence PUBLIC include)
target_compile_options(coherence PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

if(ENABLE_SANITIZERS AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(coherence PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    # target_link_options requires CMake 3.13; keep compatibility with 3.12.
    target_link_libraries(coherence PUBLIC -fsanitize=address,undefined)
endif()

add_executable(radix_coherence src/main.cpp)
target_link_libraries(radix_coherence PRIVATE coherence)
target_compile_options(radix_coherence PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

enable_testing()
add_executable(coherence_tests tests/tests.cpp)
target_link_libraries(coherence_tests PRIVATE coherence)
target_compile_options(coherence_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
add_test(NAME coherence_unit_and_integration COMMAND coherence_tests)
add_test(NAME nominal_cli COMMAND radix_coherence)
add_test(NAME stress_cli COMMAND radix_coherence --stress)
add_test(NAME one_accelerator COMMAND radix_coherence --accelerators 1 --stress)
add_test(NAME no_review_phase COMMAND radix_coherence --review-reads 0 --stress)
add_test(NAME small_regions COMMAND radix_coherence --region-bytes 128 --leaf-regions 2 --stress)
add_test(NAME invalid_accelerators COMMAND radix_coherence --accelerators 0)
set_tests_properties(invalid_accelerators PROPERTIES WILL_FAIL TRUE)

# Performance frontend using baseline traffic profiles, without an optimized policy.
add_subdirectory(perf)
