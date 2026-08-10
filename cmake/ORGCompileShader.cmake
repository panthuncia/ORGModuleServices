include_guard(GLOBAL)

function(org_compile_shader output_variable)
    cmake_parse_arguments(ARG "" "COMPILER;SOURCE;ENTRY;TARGET;OUTPUT" "INCLUDE_DIRECTORIES;DEFINES;OPTIONS;DEPENDS" ${ARGN})
    if(NOT ARG_COMPILER OR NOT ARG_SOURCE OR NOT ARG_ENTRY OR NOT ARG_TARGET OR NOT ARG_OUTPUT)
        message(FATAL_ERROR "org_compile_shader requires COMPILER, SOURCE, ENTRY, TARGET, and OUTPUT")
    endif()
    set(_arguments -T "${ARG_TARGET}" -E "${ARG_ENTRY}")
    foreach(_include IN LISTS ARG_INCLUDE_DIRECTORIES)
        list(APPEND _arguments -I "${_include}")
    endforeach()
    foreach(_define IN LISTS ARG_DEFINES)
        list(APPEND _arguments -D "${_define}")
    endforeach()
    list(APPEND _arguments ${ARG_OPTIONS} -Fo "${ARG_OUTPUT}" "${ARG_SOURCE}")
    get_filename_component(_output_directory "${ARG_OUTPUT}" DIRECTORY)
    add_custom_command(OUTPUT "${ARG_OUTPUT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_output_directory}"
        COMMAND "${ARG_COMPILER}" ${_arguments}
        DEPENDS "${ARG_SOURCE}" ${ARG_DEPENDS}
        VERBATIM COMMAND_EXPAND_LISTS)
    set(${output_variable} "${ARG_OUTPUT}" PARENT_SCOPE)
endfunction()
