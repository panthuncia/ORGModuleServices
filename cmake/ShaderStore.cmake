include_guard(GLOBAL)
if(TARGET ORGModuleServices::ShaderStore)
    return()
endif()

# The persistent shader cache. It needs no renderer, RHI or compiler, so a host without
# the rest of this module can still build it. xxhash.h is private and fully inlined.
add_library(ORGShaderStore STATIC
    ${CMAKE_CURRENT_LIST_DIR}/../src/ShaderStore.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/ORGModuleServices/ShaderStore.h)
add_library(ORGModuleServices::ShaderStore ALIAS ORGShaderStore)
set_target_properties(ORGShaderStore PROPERTIES EXPORT_NAME ShaderStore POSITION_INDEPENDENT_CODE ON)
target_compile_features(ORGShaderStore PUBLIC cxx_std_20)
target_include_directories(ORGShaderStore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../third_party/xxhash)
target_compile_definitions(ORGShaderStore PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
