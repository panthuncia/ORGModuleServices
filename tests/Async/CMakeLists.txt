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

add_executable(ORGAsyncFrameTests FrameTests.cpp)
target_link_libraries(ORGAsyncFrameTests PRIVATE ORGModuleServices::AsyncPrimitives)
target_compile_options(ORGAsyncFrameTests PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
add_test(NAME ORGAsyncFrameTests COMMAND ORGAsyncFrameTests)

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

# The thread-pool executor and its lanes.
include(../../cmake/AsyncExecutor.cmake)
add_executable(ORGAsyncExecutorTests ExecutorTests.cpp)
target_link_libraries(ORGAsyncExecutorTests PRIVATE ORGModuleServices::AsyncExecutor)
target_compile_options(ORGAsyncExecutorTests PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
add_test(NAME ORGAsyncExecutorTests COMMAND ORGAsyncExecutorTests)
set_tests_properties(ORGAsyncExecutorTests PROPERTIES TIMEOUT 60)

# The OpenRenderGraph task service over the executor. It needs only OpenRenderGraph's ITaskService.h header, so the test
# compiles the service's source itself when an OpenRenderGraph checkout sits beside this one.
set(ORG_ASYNC_TEST_ORG_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/../../../OpenRenderGraph/include" CACHE PATH
    "OpenRenderGraph's include folder, for the task service test")
if(EXISTS "${ORG_ASYNC_TEST_ORG_INCLUDE}/Render/Runtime/ITaskService.h")
    add_executable(ORGExecutorTaskServiceTests ExecutorTaskServiceTests.cpp ../../src/Async/ExecutorTaskService.cpp)
    target_include_directories(ORGExecutorTaskServiceTests PRIVATE "${ORG_ASYNC_TEST_ORG_INCLUDE}")
    target_link_libraries(ORGExecutorTaskServiceTests PRIVATE ORGModuleServices::AsyncExecutor)
    target_compile_options(ORGExecutorTaskServiceTests PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
    add_test(NAME ORGExecutorTaskServiceTests COMMAND ORGExecutorTaskServiceTests)
    set_tests_properties(ORGExecutorTaskServiceTests PROPERTIES TIMEOUT 60)
else()
    message(STATUS "ORGExecutorTaskServiceTests skipped: no OpenRenderGraph headers at ${ORG_ASYNC_TEST_ORG_INCLUDE}")
endif()

# A shared TBB goes beside the test executables.
if(WIN32)
    get_target_property(_org_tbb_type TBB::tbb TYPE)
    if(_org_tbb_type STREQUAL "SHARED_LIBRARY")
        foreach(_org_test IN ITEMS ORGAsyncExecutorTests ORGExecutorTaskServiceTests)
            if(TARGET ${_org_test})
                add_custom_command(TARGET ${_org_test} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:TBB::tbb> $<TARGET_FILE_DIR:${_org_test}>)
            endif()
        endforeach()
    endif()
endif()
