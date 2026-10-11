cmake_minimum_required(VERSION 3.21)
project(ORGServicesCompletionTests LANGUAGES CXX)
enable_testing()
find_package(Threads REQUIRED)

# Standalone: builds the ShaderCompiler/PipelineService sources directly, without BasicRHI/OpenRenderGraph targets.
# ShaderCompiler needs dxcapi.h and BasicRHI's header-only rhi_shader_abi.h; without them only PipelineService is tested.
set(ORG_SERVICES_TEST_RHI_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/../../../BasicRHI" CACHE PATH "Directory containing rhi_shader_abi.h")
find_path(ORG_SERVICES_TEST_DXC_INCLUDE dxcapi.h
    HINTS
        "${CMAKE_CURRENT_LIST_DIR}/../../../FidelityFX-SDK/framework/cauldron/framework/libs/dxc/inc"
        "$ENV{VULKAN_SDK}/Include/dxc")

set(_services_root "${CMAKE_CURRENT_LIST_DIR}/../..")
add_executable(ORGServicesCompletionTests ServicesCompletionTests.cpp "${_services_root}/src/PipelineService.cpp")
target_compile_features(ORGServicesCompletionTests PRIVATE cxx_std_20)
target_include_directories(ORGServicesCompletionTests PRIVATE "${_services_root}/include" "${CMAKE_CURRENT_LIST_DIR}/telemetry_shim")
target_link_libraries(ORGServicesCompletionTests PRIVATE Threads::Threads)
target_compile_definitions(ORGServicesCompletionTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ORGServicesCompletionTests PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
if(ORG_SERVICES_TEST_DXC_INCLUDE AND EXISTS "${ORG_SERVICES_TEST_RHI_INCLUDE}/rhi_shader_abi.h")
    target_sources(ORGServicesCompletionTests PRIVATE "${_services_root}/src/ShaderCompiler.cpp")
    target_include_directories(ORGServicesCompletionTests PRIVATE "${ORG_SERVICES_TEST_DXC_INCLUDE}" "${ORG_SERVICES_TEST_RHI_INCLUDE}")
    target_compile_definitions(ORGServicesCompletionTests PRIVATE ORG_MODULE_SERVICES_HAS_DXC=1)
    if(NOT WIN32)
        target_link_libraries(ORGServicesCompletionTests PRIVATE ${CMAKE_DL_LIBS})
    endif()
else()
    message(STATUS "ORGServicesCompletionTests: dxcapi.h or rhi_shader_abi.h not found; ShaderCompiler tests are skipped")
endif()
add_test(NAME ORGServicesCompletionTests COMMAND ORGServicesCompletionTests)
set_tests_properties(ORGServicesCompletionTests PROPERTIES TIMEOUT 120)
