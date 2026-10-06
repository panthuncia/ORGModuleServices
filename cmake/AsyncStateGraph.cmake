include_guard(GLOBAL)
if(TARGET ORGModuleServices::AsyncStateGraph)
    return()
endif()
include(${CMAKE_CURRENT_LIST_DIR}/AsyncRuntime.cmake)
if(NOT TARGET TBB::tbb)
    find_package(TBB CONFIG REQUIRED)
endif()
# A host that links the compiled spdlog (SPDLOG_COMPILED_LIB) sets this to spdlog::spdlog: header-only copies collide with it.
set(ORG_ASYNC_STATE_GRAPH_SPDLOG_TARGET spdlog::spdlog_header_only CACHE STRING "The spdlog target the state graph links")
if(NOT TARGET ${ORG_ASYNC_STATE_GRAPH_SPDLOG_TARGET})
    find_package(spdlog CONFIG REQUIRED)
endif()
if(NOT TARGET BasicTelemetry::Core)
    find_package(BasicTelemetry CONFIG REQUIRED)
endif()

add_library(ORGAsyncStateGraph STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../src/Async/StateGraph.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/ORGModuleServices/Async/StateGraph.h
    ${CMAKE_CURRENT_LIST_DIR}/../include/ORGModuleServices/Async/StateGraphTypes.h
    ${CMAKE_CURRENT_LIST_DIR}/../include/ORGModuleServices/Async/Detail/StateGraphImplementation.h)
add_library(ORGModuleServices::AsyncStateGraph ALIAS ORGAsyncStateGraph)
set_target_properties(ORGAsyncStateGraph PROPERTIES EXPORT_NAME AsyncStateGraph POSITION_INDEPENDENT_CODE ON)
target_compile_features(ORGAsyncStateGraph PUBLIC cxx_std_23)
target_compile_definitions(ORGAsyncStateGraph PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ORGAsyncStateGraph PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)
target_link_libraries(ORGAsyncStateGraph PUBLIC ORGModuleServices::AsyncRuntime
    PRIVATE TBB::tbb ${ORG_ASYNC_STATE_GRAPH_SPDLOG_TARGET} BasicTelemetry::Core)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS ORGAsyncStateGraph EXPORT ORGAsyncStateGraphTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT ORGAsyncStateGraphTargets NAMESPACE ORGModuleServices::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ORGAsyncStateGraph)
configure_package_config_file(${CMAKE_CURRENT_LIST_DIR}/ORGAsyncStateGraphConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ORGAsyncStateGraphConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ORGAsyncStateGraph)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ORGAsyncStateGraphConfig.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ORGAsyncStateGraph)
