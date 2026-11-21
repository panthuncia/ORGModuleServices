cmake_minimum_required(VERSION 3.21)
project(ORGModuleServices LANGUAGES CXX)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/ORGCompileShader.cmake)

option(ORG_MODULE_SERVICES_ENABLE_DXC "Build DXC shader compilation support" ON)
option(ORG_MODULE_SERVICES_ENABLE_VULKAN "Enable SPIR-V shader targets" OFF)
option(ORG_MODULE_SERVICES_BUILD_TESTS "Build ORGModuleServices tests" OFF)
option(ORG_MODULE_SERVICES_ENABLE_ASYNC_STATE_GRAPH "Build the shared CPU dependency engine" OFF)

# Backend-independent coordination and immutable slot ownership. Kept separate
# from shader/RHI services so CPU-only consumers and tests need no device.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/AsyncRuntime.cmake)
# The thread-pool scheduler and its lanes, which the OpenRenderGraph task service runs on.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/AsyncExecutor.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/ShaderStore.cmake)
if(ORG_MODULE_SERVICES_ENABLE_ASYNC_STATE_GRAPH)
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/AsyncStateGraph.cmake)
endif()

add_library(ORGModuleServices STATIC
    src/Async/ExecutorTaskService.cpp
    src/BackingPool.cpp
    src/BackingPoolBinding.cpp
    src/ComposedVersions.cpp
    src/GraphJob.cpp
    src/GraphHostBackings.cpp
    src/GraphHostRetirement.cpp
    src/GrowableBufferSet.cpp
    src/Passes/GenerateMipsPass.cpp
    src/Passes/HzbPass.cpp
    src/Passes/PrefixSumPass.cpp
    src/Passes/ResolveMultisamplePass.cpp
    src/Passes/UavClearPass.cpp
    src/PipelineRegistry.cpp
    src/RecordingPools.cpp
    src/RetirementJoin.cpp
    src/SecondaryListPool.cpp
    src/TexelEncoding.cpp
    src/VersionedBufferBuild.cpp
    src/VersionedBufferGrowth.cpp
    src/VersionedTextureGrowth.cpp
    src/ViolationPolicy.cpp)
add_library(ORGModuleServices::ORGModuleServices ALIAS ORGModuleServices)
# The passes' shaders (Passes/GenerateMipsPass.h, Passes/ResolveMultisamplePass.h). A host compiles them with its own
# programs and ships this folder's files under "ORGModuleServices/" in its shader folder.
set(ORG_MODULE_SERVICES_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders" CACHE INTERNAL "ORGModuleServices pass shaders")
target_compile_features(ORGModuleServices PUBLIC cxx_std_20)
target_include_directories(ORGModuleServices PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(ORGModuleServices PUBLIC BasicRHI::BasicRHI OpenRenderGraph::OpenRenderGraph ORGModuleServices::ShaderStore
    ORGModuleServices::AsyncExecutor)
target_compile_definitions(ORGModuleServices PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

if(ORG_MODULE_SERVICES_ENABLE_DXC)
    find_path(ORG_MODULE_SERVICES_DXC_INCLUDE_DIR dxcapi.h
        HINTS
            "${CMAKE_CURRENT_LIST_DIR}/../FidelityFX-SDK/framework/cauldron/framework/libs/dxc/inc"
            "${CMAKE_CURRENT_LIST_DIR}/../../extern/FidelityFX-SDK/framework/cauldron/framework/libs/dxc/inc"
            "$ENV{VULKAN_SDK}/Include/dxc")
    if(ORG_MODULE_SERVICES_DXC_INCLUDE_DIR)
        target_sources(ORGModuleServices PRIVATE src/ShaderCompiler.cpp)
        target_include_directories(ORGModuleServices PRIVATE "${ORG_MODULE_SERVICES_DXC_INCLUDE_DIR}")
        target_compile_definitions(ORGModuleServices PUBLIC ORG_MODULE_SERVICES_HAS_DXC=1)
        if(ORG_MODULE_SERVICES_ENABLE_VULKAN)
            target_compile_definitions(ORGModuleServices PUBLIC ORG_MODULE_SERVICES_HAS_SPIRV=1)
        endif()
    else()
        message(WARNING "ORGModuleServices: dxcapi.h not found; shader compilation support is disabled")
    endif()
endif()

if(ORG_MODULE_SERVICES_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/Async)
    add_subdirectory(tests/Services)
    add_subdirectory(tests/Growth)
    add_subdirectory(tests/Buffers)
    add_subdirectory(tests/ShaderStore)
    add_subdirectory(tests/Variables)
    add_executable(ORGModuleServicesTests tests/ServicesTests.cpp)
    target_link_libraries(ORGModuleServicesTests PRIVATE ORGModuleServices::ORGModuleServices)
    add_test(NAME ORGModuleServicesTests COMMAND ORGModuleServicesTests)
    add_executable(ORGViolationPolicyTests tests/Policy/ViolationPolicyTests.cpp)
    target_link_libraries(ORGViolationPolicyTests PRIVATE ORGModuleServices::ORGModuleServices)
    add_test(NAME ORGViolationPolicyTests COMMAND ORGViolationPolicyTests)
    # Named texture versions and composed sets, against OpenRenderGraph on WARP (D3D12).
    if(WIN32)
        add_executable(ORGNamedVersionsTests tests/Versions/NamedVersionsTests.cpp)
        target_link_libraries(ORGNamedVersionsTests PRIVATE ORGModuleServices::ORGModuleServices d3d12 dxgi)
        target_include_directories(ORGNamedVersionsTests PRIVATE "${OpenRenderGraph_SOURCE_DIR}/tests")
        target_compile_definitions(ORGNamedVersionsTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        if(MSVC)
            target_compile_options(ORGNamedVersionsTests PRIVATE /utf-8)
        endif()
        add_test(NAME ORGNamedVersionsTests COMMAND ORGNamedVersionsTests)
        # Texture versions made, zero-filled on the uploader's copy queue and named, on WARP (D3D12).
        add_executable(ORGTextureVersionsTests tests/Versions/TextureVersionsTests.cpp)
        target_link_libraries(ORGTextureVersionsTests PRIVATE ORGModuleServices::ORGModuleServices d3d12 dxgi)
        target_include_directories(ORGTextureVersionsTests PRIVATE "${OpenRenderGraph_SOURCE_DIR}/tests")
        target_compile_definitions(ORGTextureVersionsTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        if(MSVC)
            target_compile_options(ORGTextureVersionsTests PRIVATE /utf-8)
        endif()
        add_test(NAME ORGTextureVersionsTests COMMAND ORGTextureVersionsTests)
        # Texel encodings for value births, against a DirectXMath reference. CPU only.
        add_executable(ORGTexelEncodingTests tests/Versions/TexelEncodingTests.cpp)
        target_link_libraries(ORGTexelEncodingTests PRIVATE ORGModuleServices::ORGModuleServices d3d12 dxgi)
        target_compile_definitions(ORGTexelEncodingTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        add_test(NAME ORGTexelEncodingTests COMMAND ORGTexelEncodingTests)
    endif()
endif()

install(TARGETS ORGModuleServices ORGShaderStore EXPORT ORGModuleServicesTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT ORGModuleServicesTargets NAMESPACE ORGModuleServices::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ORGModuleServices)
configure_package_config_file(cmake/ORGModuleServicesConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ORGModuleServicesConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ORGModuleServices)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/ORGModuleServicesConfigVersion.cmake
    VERSION 1.0.0 COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ORGModuleServicesConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ORGModuleServicesConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ORGModuleServices)
install(FILES cmake/ORGCompileShader.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ORGModuleServices)
