# Build-tree deployment only. No installation or changes outside the target directory.
cmake_minimum_required(VERSION 3.24)
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM windows+pe)
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL objdump)
set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${OBJDUMP}")
file(GET_RUNTIME_DEPENDENCIES LIBRARIES "${MODULE}" DIRECTORIES "${SEARCH}"
    RESOLVED_DEPENDENCIES_VAR resolved UNRESOLVED_DEPENDENCIES_VAR missing
    PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*"
    POST_EXCLUDE_REGEXES ".*[Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\].*")
if(missing)
    message(FATAL_ERROR "Backend runtime dependencies were not found: ${missing}")
endif()
get_filename_component(destination "${MODULE}" DIRECTORY)
foreach(dependency IN LISTS resolved)
    file(COPY "${dependency}" DESTINATION "${destination}")
endforeach()
