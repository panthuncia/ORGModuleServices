include_guard(GLOBAL)
if(TARGET ORGModuleServices::ImGui)
    return()
endif()

# Dear ImGui's renderer backend on the render graph (ImGui/ImGuiBackend.h). Optional: ORGModuleServices builds without it.
# Include it after ORGModuleServices, or set ORG_MODULE_SERVICES_ENABLE_IMGUI.
#
# ImGui comes from the host. ORGModuleServices does not fetch or find it. Either name the host's ImGui target in
# ORG_MODULE_SERVICES_IMGUI_TARGET (imgui::imgui by default), which brings the include path and the configuration the host
# builds ImGui with, or give the folder holding imgui.h in ORG_MODULE_SERVICES_IMGUI_INCLUDE_DIR. With the folder alone,
# the host links ImGui itself and must build it with the same imconfig.
set(ORG_MODULE_SERVICES_IMGUI_TARGET "imgui::imgui" CACHE STRING "The host's Dear ImGui target, for ORGModuleServices::ImGui")
set(ORG_MODULE_SERVICES_IMGUI_INCLUDE_DIR "" CACHE PATH "The folder holding imgui.h, when the host has no ImGui target")

if(NOT TARGET ORGModuleServices::ORGModuleServices)
    message(FATAL_ERROR "ORGModuleServices::ImGui: include ImGuiBackend.cmake after ORGModuleServices")
endif()

set(_oms_imgui_dir ${CMAKE_CURRENT_LIST_DIR}/..)
add_library(ORGModuleServicesImGui STATIC
    ${_oms_imgui_dir}/src/ImGui/ImGuiBackend.cpp
    ${_oms_imgui_dir}/include/ORGModuleServices/ImGui/ImGuiBackend.h)
add_library(ORGModuleServices::ImGui ALIAS ORGModuleServicesImGui)
set_target_properties(ORGModuleServicesImGui PROPERTIES EXPORT_NAME ImGui)
target_compile_features(ORGModuleServicesImGui PUBLIC cxx_std_20)
target_link_libraries(ORGModuleServicesImGui PUBLIC ORGModuleServices::ORGModuleServices)
target_compile_definitions(ORGModuleServicesImGui PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(TARGET ${ORG_MODULE_SERVICES_IMGUI_TARGET})
    target_link_libraries(ORGModuleServicesImGui PUBLIC ${ORG_MODULE_SERVICES_IMGUI_TARGET})
elseif(ORG_MODULE_SERVICES_IMGUI_INCLUDE_DIR)
    target_include_directories(ORGModuleServicesImGui PUBLIC "${ORG_MODULE_SERVICES_IMGUI_INCLUDE_DIR}")
else()
    message(FATAL_ERROR "ORGModuleServices::ImGui needs ImGui from the host: define the target ${ORG_MODULE_SERVICES_IMGUI_TARGET} "
        "first, or set ORG_MODULE_SERVICES_IMGUI_TARGET or ORG_MODULE_SERVICES_IMGUI_INCLUDE_DIR")
endif()
