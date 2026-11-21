include_guard(GLOBAL)
if(TARGET ORGModuleServices::AsyncExecutor)
    return()
endif()
# AsyncRuntime brings AsyncPrimitives and installs it, so this target's export can name it.
include(${CMAKE_CURRENT_LIST_DIR}/AsyncRuntime.cmake)
find_package(Threads REQUIRED)
if(NOT TARGET TBB::tbb)
    find_package(TBB CONFIG REQUIRED)
endif()

# The thread-pool GraphScheduler (Async/Executor.h) and its lanes (Async/Lanes.h). CPU only: no renderer, RHI or
# shader dependency, so CPU-only hosts and tests link it alone. The OpenRenderGraph task service over it
# (Async/ExecutorTaskService.h) is in ORGModuleServices, which links OpenRenderGraph.
add_library(ORGAsyncExecutor STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../src/Async/Executor.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../src/Async/Lanes.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/ORGModuleServices/Async/Executor.h
    ${CMAKE_CURRENT_LIST_DIR}/../include/ORGModuleServices/Async/Lanes.h)
add_library(ORGModuleServices::AsyncExecutor ALIAS ORGAsyncExecutor)
set_target_properties(ORGAsyncExecutor PROPERTIES EXPORT_NAME AsyncExecutor POSITION_INDEPENDENT_CODE ON)
target_compile_features(ORGAsyncExecutor PUBLIC cxx_std_20)
target_compile_definitions(ORGAsyncExecutor PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(ORGAsyncExecutor PUBLIC ORGModuleServices::AsyncPrimitives Threads::Threads PRIVATE TBB::tbb)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS ORGAsyncExecutor EXPORT ORGAsyncExecutorTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT ORGAsyncExecutorTargets NAMESPACE ORGModuleServices::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ORGAsyncExecutor)
configure_package_config_file(${CMAKE_CURRENT_LIST_DIR}/ORGAsyncExecutorConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ORGAsyncExecutorConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ORGAsyncExecutor)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ORGAsyncExecutorConfig.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ORGAsyncExecutor)
