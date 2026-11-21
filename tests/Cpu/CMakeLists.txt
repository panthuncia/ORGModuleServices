cmake_minimum_required(VERSION 3.21)
project(ORGSphereCullTests LANGUAGES CXX)
enable_testing()

# Standalone: the CPU sphere cull. No device and no renderer.
include(../../cmake/SphereCull.cmake)
add_executable(ORGSphereCullTests SphereCullTests.cpp)
target_compile_features(ORGSphereCullTests PRIVATE cxx_std_20)
target_link_libraries(ORGSphereCullTests PRIVATE ORGModuleServices::SphereCull)
target_compile_options(ORGSphereCullTests PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
add_test(NAME ORGSphereCullTests COMMAND ORGSphereCullTests)
