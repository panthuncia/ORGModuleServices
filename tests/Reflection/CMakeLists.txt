cmake_minimum_required(VERSION 3.21)
project(ORGReflectionTests LANGUAGES CXX)
enable_testing()
find_package(Threads REQUIRED)

# Standalone, no device: a hand-written SPIR-V module always. With dxcapi.h and BasicRHI's rhi_shader_abi.h on Windows,
# also stages compiled by ShaderCompiler::CompileHere and DXIL reflection (skipped at run time when DXC cannot compile).
set(ORG_REFLECTION_TEST_RHI_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/../../../BasicRHI" CACHE PATH "Directory containing rhi_shader_abi.h")
find_path(ORG_REFLECTION_TEST_DXC_INCLUDE dxcapi.h
    HINTS
        "${CMAKE_CURRENT_LIST_DIR}/../../../FidelityFX-SDK/framework/cauldron/framework/libs/dxc/inc"
        "$ENV{VULKAN_SDK}/Include/dxc")

set(_services_root "${CMAKE_CURRENT_LIST_DIR}/../..")
add_executable(ORGReflectionTests ReflectionTests.cpp "${_services_root}/src/ShaderReflection.cpp")
target_compile_features(ORGReflectionTests PRIVATE cxx_std_20)
target_include_directories(ORGReflectionTests PRIVATE "${_services_root}/include")
target_link_libraries(ORGReflectionTests PRIVATE Threads::Threads)
target_compile_definitions(ORGReflectionTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ORGReflectionTests PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
if(WIN32 AND ORG_REFLECTION_TEST_DXC_INCLUDE AND EXISTS "${ORG_REFLECTION_TEST_RHI_INCLUDE}/rhi_shader_abi.h")
    if(NOT TARGET ORGModuleServices::ShaderStore)
        include(${_services_root}/cmake/ShaderStore.cmake)
    endif()
    target_link_libraries(ORGReflectionTests PRIVATE ORGModuleServices::ShaderStore)
    target_sources(ORGReflectionTests PRIVATE "${_services_root}/src/ShaderCompiler.cpp" "${_services_root}/src/DxilReflection.cpp")
    target_include_directories(ORGReflectionTests PRIVATE "${ORG_REFLECTION_TEST_DXC_INCLUDE}" "${ORG_REFLECTION_TEST_RHI_INCLUDE}"
        "${CMAKE_CURRENT_LIST_DIR}/../Services/telemetry_shim")
    target_compile_definitions(ORGReflectionTests PRIVATE ORG_MODULE_SERVICES_HAS_DXC=1 ORG_MODULE_SERVICES_HAS_SPIRV=1
        ORG_MODULE_SERVICES_DXIL_REFLECTION=1)
else()
    message(STATUS "ORGReflectionTests: not Windows, or dxcapi.h or rhi_shader_abi.h not found; compiled-stage tests are skipped")
endif()
add_test(NAME ORGReflectionTests COMMAND ORGReflectionTests)
set_tests_properties(ORGReflectionTests PROPERTIES TIMEOUT 120)
