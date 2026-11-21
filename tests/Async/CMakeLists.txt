cmake_minimum_required(VERSION 3.21)
project(ORGAsyncPrimitivesTests LANGUAGES CXX)
enable_testing()
find_package(Threads REQUIRED)
include(../../cmake/AsyncRuntime.cmake)
add_executable(ORGAsyncPrimitivesTests AsyncPrimitivesTests.cpp)
target_compile_features(ORGAsyncPrimitivesTests PRIVATE cxx_std_20)
target_link_libraries(ORGAsyncPrimitivesTests PRIVATE Threads::Threads ORGModuleServices::AsyncPrimitives)
target_compile_options(ORGAsyncPrimitivesTests PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
add_test(NAME ORGAsyncPrimitivesTests COMMAND ORGAsyncPrimitivesTests)

add_executable(ORGAsyncRuntimeTests AsyncRuntimeTests.cpp SuspensionIdentityTestPeer.cpp)
target_link_libraries(ORGAsyncRuntimeTests PRIVATE Threads::Threads ORGModuleServices::AsyncRuntime)
target_compile_options(ORGAsyncRuntimeTests PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
add_test(NAME ORGAsyncRuntimeTests COMMAND ORGAsyncRuntimeTests)

# Kept arrays, change journals and slot tables (header-only, CPU only).
add_executable(ORGKeptStateTests KeptStateTests.cpp)
target_link_libraries(ORGKeptStateTests PRIVATE Threads::Threads ORGModuleServices::AsyncPrimitives)  # the concurrent writes after PrepareWrites
target_compile_options(ORGKeptStateTests PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
add_test(NAME ORGKeptStateTests COMMAND ORGKeptStateTests)

# The retirement chain and Retired. Header-only, but Retired queues through OpenRenderGraph's EventQueue header.
set(ORG_ASYNC_TEST_ORG_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/../../../OpenRenderGraph/include" CACHE PATH "OpenRenderGraph's include directory")
if(EXISTS "${ORG_ASYNC_TEST_ORG_INCLUDE}/Render/Runtime/EventQueue.h")
    add_executable(ORGRetirementChainTests RetirementChainTests.cpp)
    target_compile_features(ORGRetirementChainTests PRIVATE cxx_std_20)
    target_include_directories(ORGRetirementChainTests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../include" "${ORG_ASYNC_TEST_ORG_INCLUDE}")
    target_link_libraries(ORGRetirementChainTests PRIVATE Threads::Threads)
    target_compile_options(ORGRetirementChainTests PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
    add_test(NAME ORGRetirementChainTests COMMAND ORGRetirementChainTests)
else()
    message(STATUS "ORGRetirementChainTests: OpenRenderGraph's headers not found (ORG_ASYNC_TEST_ORG_INCLUDE); skipped")
endif()
