include_guard(GLOBAL)
if(TARGET ORGModuleServices::SphereCull)
    return()
endif()

# The CPU sphere cull (Cpu/SphereCull.h). It needs no device, renderer or RHI, so a CPU-only host can build it alone.
# Each wide kernel file is built for its own instruction set and the rest for the baseline. Supported() picks one at
# run time. A host that wants the kernels built with its own floating-point and optimisation options adds them to
# ORGSphereCull; they must not allow FMA contraction, or the kernels' verdicts would differ.
set(_oms_sphere_cull_dir ${CMAKE_CURRENT_LIST_DIR}/../src/Cpu)
add_library(ORGSphereCull STATIC
    ${_oms_sphere_cull_dir}/SphereCull.cpp
    ${_oms_sphere_cull_dir}/SphereCullAvx2.cpp
    ${_oms_sphere_cull_dir}/SphereCullAvx512.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../include/ORGModuleServices/Cpu/SphereCull.h)
add_library(ORGModuleServices::SphereCull ALIAS ORGSphereCull)
set_target_properties(ORGSphereCull PROPERTIES EXPORT_NAME SphereCull POSITION_INDEPENDENT_CODE ON)
target_compile_features(ORGSphereCull PUBLIC cxx_std_20)
target_include_directories(ORGSphereCull PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/../include>
    $<INSTALL_INTERFACE:include>)
if(MSVC)
    set(_oms_sphere_cull_avx2 /arch:AVX2)
    set(_oms_sphere_cull_avx512 /arch:AVX512)
else()
    set(_oms_sphere_cull_avx2 -mavx2)
    set(_oms_sphere_cull_avx512 -mavx512f -mavx512bw -mavx512vl)
endif()
set_source_files_properties(${_oms_sphere_cull_dir}/SphereCullAvx2.cpp PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON SKIP_UNITY_BUILD_INCLUSION ON COMPILE_OPTIONS "${_oms_sphere_cull_avx2}")
set_source_files_properties(${_oms_sphere_cull_dir}/SphereCullAvx512.cpp PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON SKIP_UNITY_BUILD_INCLUSION ON COMPILE_OPTIONS "${_oms_sphere_cull_avx512}")
