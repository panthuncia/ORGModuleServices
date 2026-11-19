cmake_minimum_required(VERSION 3.21)
project(ORGVariablesTests LANGUAGES CXX)
enable_testing()
find_package(Threads REQUIRED)

# Standalone: the header-only variable set. No device and no renderer.
set(_services_root "${CMAKE_CURRENT_LIST_DIR}/../..")
add_executable(ORGVariablesTests VariablesTests.cpp)
target_compile_features(ORGVariablesTests PRIVATE cxx_std_20)
target_include_directories(ORGVariablesTests PRIVATE "${_services_root}/include")
target_link_libraries(ORGVariablesTests PRIVATE Threads::Threads)
target_compile_options(ORGVariablesTests PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG /W4 /permissive-,-UNDEBUG>)
add_test(NAME ORGVariablesTests COMMAND ORGVariablesTests)
