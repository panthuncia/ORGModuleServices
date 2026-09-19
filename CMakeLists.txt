cmake_minimum_required(VERSION 3.21)
project(ORGModuleServices LANGUAGES CXX)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/ORGCompileShader.cmake)

option(ORG_MODULE_SERVICES_ENABLE_DXC "Build DXC shader compilation support" ON)
option(ORG_MODULE_SERVICES_ENABLE_VULKAN "Enable SPIR-V shader targets" OFF)
option(ORG_MODULE_SERVICES_BUILD_TESTS "Build ORGModuleServices tests" OFF)

add_library(ORGModuleServices STATIC
    src/PipelineService.cpp)
add_library(ORGModuleServices::ORGModuleServices ALIAS ORGModuleServices)
target_compile_features(ORGModuleServices PUBLIC cxx_std_20)
target_include_directories(ORGModuleServices PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(ORGModuleServices PUBLIC BasicRHI::BasicRHI OpenRenderGraph::OpenRenderGraph)
target_compile_definitions(ORGModuleServices PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

if(ORG_MODULE_SERVICES_ENABLE_DXC)
    find_path(ORG_MODULE_SERVICES_DXC_INCLUDE_DIR dxcapi.h
        HINTS
            "${CMAKE_CURRENT_LIST_DIR}/../FidelityFX-SDK/framework/cauldron/framework/libs/dxc/inc"
            "${CMAKE_CURRENT_LIST_DIR}/../../extern/FidelityFX-SDK/framework/cauldron/framework/libs/dxc/inc"
            "$ENV{VULKAN_SDK}/Include/dxc")
    if(ORG_MODULE_SERVICES_DXC_INCLUDE_DIR)
        target_sources(ORGModuleServices PRIVATE src/ShaderCompiler.cpp src/ShaderServiceAdapter.cpp)
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
    add_executable(ORGModuleServicesTests tests/ServicesTests.cpp)
    target_link_libraries(ORGModuleServicesTests PRIVATE ORGModuleServices::ORGModuleServices)
    add_test(NAME ORGModuleServicesTests COMMAND ORGModuleServicesTests)
endif()

install(TARGETS ORGModuleServices EXPORT ORGModuleServicesTargets
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
