include_guard(GLOBAL)
if(TARGET ORGModuleServices::AsyncRuntime)
    return()
endif()
include(${CMAKE_CURRENT_LIST_DIR}/AsyncPrimitives.cmake)

# The compiled coordination services have no renderer/RHI/shader dependency.
add_library(ORGAsyncRuntime STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../src/Async/SuspensionIdentity.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/ORGModuleServices/Async/SuspensionIdentity.h)
add_library(ORGModuleServices::AsyncRuntime ALIAS ORGAsyncRuntime)
set_target_properties(ORGAsyncRuntime PROPERTIES EXPORT_NAME AsyncRuntime POSITION_INDEPENDENT_CODE ON)
target_link_libraries(ORGAsyncRuntime PUBLIC ORGModuleServices::AsyncPrimitives)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS ORGAsyncRuntime ORGAsyncPrimitives EXPORT ORGAsyncRuntimeTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/../include/ORGModuleServices/Async
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ORGModuleServices)
install(EXPORT ORGAsyncRuntimeTargets NAMESPACE ORGModuleServices::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ORGAsyncRuntime)
configure_package_config_file(${CMAKE_CURRENT_LIST_DIR}/ORGAsyncRuntimeConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ORGAsyncRuntimeConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ORGAsyncRuntime)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ORGAsyncRuntimeConfig.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ORGAsyncRuntime)
