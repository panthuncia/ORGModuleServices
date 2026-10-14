cmake_minimum_required(VERSION 3.21)
project(ORGGrowthTests LANGUAGES CXX)
enable_testing()

# Header-only: growth policy, extents, requirements and the contents rule. No renderer/RHI is configured.
add_executable(ORGGrowthTests GrowthTests.cpp)
target_compile_features(ORGGrowthTests PRIVATE cxx_std_20)
target_include_directories(ORGGrowthTests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../include")
target_compile_options(ORGGrowthTests PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
add_test(NAME ORGGrowthTests COMMAND ORGGrowthTests)
