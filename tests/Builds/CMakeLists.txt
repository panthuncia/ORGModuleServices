cmake_minimum_required(VERSION 3.21)
project(ORGBuildsTests LANGUAGES CXX)
enable_testing()
find_package(Threads REQUIRED)

# Standalone: the build context (what a build's extensions defer) and the build inputs (serials, built and attached).
# Both are std only, so no renderer or RHI is configured. StagedBuilds itself needs a graph host and is not covered here.
set(_services_root "${CMAKE_CURRENT_LIST_DIR}/../..")
add_executable(ORGBuildsTests BuildsTests.cpp "${_services_root}/src/BuildContext.cpp")
target_compile_features(ORGBuildsTests PRIVATE cxx_std_20)
target_include_directories(ORGBuildsTests PRIVATE "${_services_root}/include")
target_link_libraries(ORGBuildsTests PRIVATE Threads::Threads)
target_compile_options(ORGBuildsTests PRIVATE $<IF:$<CXX_COMPILER_ID:MSVC>,/UNDEBUG,-UNDEBUG>)
add_test(NAME ORGBuildsTests COMMAND ORGBuildsTests)
