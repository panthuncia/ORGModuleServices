cmake_minimum_required(VERSION 3.21)
project(ORGUploadsTests LANGUAGES CXX)
enable_testing()

# Standalone: owned rows (header only) and content owners. No device and no renderer.
set(_services_root "${CMAKE_CURRENT_LIST_DIR}/../..")
add_executable(ORGUploadsTests UploadsTests.cpp "${_services_root}/src/ContentOwners.cpp")
target_compile_features(ORGUploadsTests PRIVATE cxx_std_20)
target_include_directories(ORGUploadsTests PRIVATE "${_services_root}/include")
target_compile_options(ORGUploadsTests PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG /W4 /permissive-,-UNDEBUG>)
add_test(NAME ORGUploadsTests COMMAND ORGUploadsTests)
