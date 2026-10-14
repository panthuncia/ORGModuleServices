cmake_minimum_required(VERSION 3.21)
project(ORGBuffersTests LANGUAGES CXX)
enable_testing()
find_package(Threads REQUIRED)

set(_services_root "${CMAKE_CURRENT_LIST_DIR}/../..")

# Standalone, MS headers only: the paged image, the journal, the retirement clock and the backing pool (no org::Buffer: the
# allocate hook makes none, and NoBufferBinding.cpp stands in for the pin-factory install), plus the public headers' self-containment.
add_executable(ORGBufferPoolTests
    BackingPoolTests.cpp
    NoBufferBinding.cpp
    PublicHeaders/BufferImage.cpp
    PublicHeaders/BufferJournal.cpp
    PublicHeaders/BackingPool.cpp
    PublicHeaders/VersionedBufferBuild.cpp
    PublicHeaders/JournaledBufferSet.cpp
    "${_services_root}/src/BackingPool.cpp"
    "${_services_root}/src/BufferImage.cpp"
    "${_services_root}/src/BufferJournal.cpp")
target_compile_features(ORGBufferPoolTests PRIVATE cxx_std_20)
target_include_directories(ORGBufferPoolTests PRIVATE "${_services_root}/include")
target_link_libraries(ORGBufferPoolTests PRIVATE Threads::Threads)
target_compile_definitions(ORGBufferPoolTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ORGBufferPoolTests PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG /W4 /WX,-UNDEBUG>)
add_test(NAME ORGBufferPoolTests COMMAND ORGBufferPoolTests)
set_tests_properties(ORGBufferPoolTests PROPERTIES TIMEOUT 120)

# Header-compiled against OpenRenderGraph/BasicRHI (nothing of them is linked): BuildVersionedBuffer, ReplayVersionedBuffer,
# JournaledBufferSet and VersionedBufferFamily through a recording upload service. Needs ORG's third-party headers (flecs, spdlog,
# boost, fmt), e.g. CommunityShaders' vcpkg tree; skipped without them.
set(ORG_BUFFERS_TEST_EXTERN "${_services_root}/.." CACHE PATH "Directory holding OpenRenderGraph, BasicRHI, volk and BasicTelemetry")
find_path(ORG_BUFFERS_TEST_DEPS_INCLUDE flecs.h
    HINTS
        "${_services_root}/../../build/ALL/vcpkg_installed/x64-windows-static-md-release/include"
        "${_services_root}/../../build/vcpkg_installed/x64-windows/include")
find_path(ORG_BUFFERS_TEST_VULKAN_INCLUDE vulkan/vulkan.h HINTS "$ENV{VULKAN_SDK}/Include" "$ENV{VULKAN_SDK}/include")
if(ORG_BUFFERS_TEST_DEPS_INCLUDE AND ORG_BUFFERS_TEST_VULKAN_INCLUDE AND EXISTS "${ORG_BUFFERS_TEST_EXTERN}/OpenRenderGraph/include")
    add_executable(ORGBufferBuildTests
        VersionedBufferBuildTests.cpp
        NoBufferBinding.cpp
        "${_services_root}/src/BackingPool.cpp"
        "${_services_root}/src/BufferImage.cpp"
        "${_services_root}/src/BufferJournal.cpp"
        "${_services_root}/src/JournaledBufferSet.cpp"
        "${_services_root}/src/VersionedBufferBuild.cpp")
    target_compile_features(ORGBufferBuildTests PRIVATE cxx_std_20)
    target_include_directories(ORGBufferBuildTests PRIVATE
        "${_services_root}/include"
        "${ORG_BUFFERS_TEST_EXTERN}/OpenRenderGraph/include"
        "${ORG_BUFFERS_TEST_EXTERN}/BasicRHI"
        "${ORG_BUFFERS_TEST_EXTERN}/volk"
        "${ORG_BUFFERS_TEST_EXTERN}/BasicTelemetry/include")
    target_include_directories(ORGBufferBuildTests SYSTEM PRIVATE "${ORG_BUFFERS_TEST_VULKAN_INCLUDE}" "${ORG_BUFFERS_TEST_DEPS_INCLUDE}")
    # flecs' header defines globals that reference its library (Resource includes flecs.h).
    find_library(ORG_BUFFERS_TEST_FLECS NAMES flecs_static flecs HINTS "${ORG_BUFFERS_TEST_DEPS_INCLUDE}/../lib")
    target_link_libraries(ORGBufferBuildTests PRIVATE Threads::Threads ${ORG_BUFFERS_TEST_FLECS})
    if(WIN32)
        target_link_libraries(ORGBufferBuildTests PRIVATE ws2_32 dbghelp)
    endif()
    target_compile_definitions(ORGBufferBuildTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _UNICODE UNICODE VK_USE_PLATFORM_WIN32_KHR VOLK_NAMESPACE
        BASICRHI_ENABLE_STREAMLINE=0 BASICRHI_ENABLE_PIX=0 BASICRHI_ENABLE_RESHAPE=0 BASICRHI_HAS_DXR2_HEADERS=0
        BASICRHI_ENABLE_TRACY_GPU_PROFILING=0 BASICRHI_ENABLE_TRACY_CPU_PROFILING=0 BASICRHI_ENABLE_D3D12=0 BASICRHI_ENABLE_VULKAN=1
        BUILD_TYPE_DEBUG=0 BUILD_TYPE_RELEASE=1 BUILD_TYPE_RELEASE_DEBUG=2 BUILD_TYPE=BUILD_TYPE_RELEASE
        flecs_STATIC SPDLOG_COMPILED_LIB SPDLOG_FMT_EXTERNAL)
    target_compile_options(ORGBufferBuildTests PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG /utf-8,-UNDEBUG>)
    add_test(NAME ORGBufferBuildTests COMMAND ORGBufferBuildTests)
    set_tests_properties(ORGBufferBuildTests PROPERTIES TIMEOUT 120)
else()
    message(STATUS "ORGBufferBuildTests: OpenRenderGraph's third-party headers or the Vulkan SDK not found; skipped")
endif()
