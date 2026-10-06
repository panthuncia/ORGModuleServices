include_guard(GLOBAL)
if(TARGET ORGModuleServices::AsyncPrimitives)
    return()
endif()
# May be included directly by CPU-only hosts without configuring RHI services.
add_library(ORGAsyncPrimitives INTERFACE)
add_library(ORGModuleServices::AsyncPrimitives ALIAS ORGAsyncPrimitives)
set_target_properties(ORGAsyncPrimitives PROPERTIES EXPORT_NAME AsyncPrimitives)
target_compile_features(ORGAsyncPrimitives INTERFACE cxx_std_20)
target_include_directories(ORGAsyncPrimitives INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../include>
    $<INSTALL_INTERFACE:include>)
target_sources(ORGAsyncPrimitives INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../include/ORGModuleServices/Async/GraphDiagnostics.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../include/ORGModuleServices/Async/GraphTrace.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../include/ORGModuleServices/Async/ArtifactBuild.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../include/ORGModuleServices/Async/GraphSchedulingLayout.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../include/ORGModuleServices/Async/ArtifactSnapshot.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../include/ORGModuleServices/Async/ArtifactIdentity.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../include/ORGModuleServices/Async/ArtifactResources.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../include/ORGModuleServices/Async/ArtifactKindPolicy.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../include/ORGModuleServices/Async/GraphScheduler.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../include/ORGModuleServices/Async/SerializedTaskPump.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../include/ORGModuleServices/Async/LeasedArraySlots.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../include/ORGModuleServices/Async/PublicationExchange.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../include/ORGModuleServices/Async/RevisionAssembly.h>)
