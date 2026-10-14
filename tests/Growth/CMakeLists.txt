cmake_minimum_required(VERSION 3.21)
project(ORGGrowthTests LANGUAGES CXX)
enable_testing()
find_package(Threads REQUIRED)

# Standalone: growth policy, extents, requirements, the contents rule, and growth's use of the backing pool (BackingPool built
# from source, its backings without an org::Buffer). No renderer/RHI is configured: OpenRenderGraph's include directory is used
# only for its header-only StreamingUploadTypes.h (VersionFill.h).
set(ORG_GROWTH_TESTS_ORG_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/../../../OpenRenderGraph/include" CACHE PATH "OpenRenderGraph's include directory")
set(_services_root "${CMAKE_CURRENT_LIST_DIR}/../..")
add_executable(ORGGrowthTests GrowthTests.cpp "${_services_root}/src/BackingPool.cpp" "${CMAKE_CURRENT_LIST_DIR}/../Buffers/NoBufferBinding.cpp")
target_compile_features(ORGGrowthTests PRIVATE cxx_std_20)
target_include_directories(ORGGrowthTests PRIVATE "${_services_root}/include" "${ORG_GROWTH_TESTS_ORG_INCLUDE}")
target_link_libraries(ORGGrowthTests PRIVATE Threads::Threads)
target_compile_definitions(ORGGrowthTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(ORGGrowthTests PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
add_test(NAME ORGGrowthTests COMMAND ORGGrowthTests)
