find_package(CURL 7.85 REQUIRED)
find_package(nlohmann_json 3.11 QUIET)
if(NOT TARGET nlohmann_json::nlohmann_json)
    find_path(PSN_JSON_INCLUDE_DIR nlohmann/json.hpp REQUIRED)
    add_library(psn-json INTERFACE)
    target_include_directories(psn-json INTERFACE ${PSN_JSON_INCLUDE_DIR})
    add_library(nlohmann_json::nlohmann_json ALIAS psn-json)
endif()
add_library(psn-bridge-backend MODULE bridge_backend.cpp)
set_target_properties(psn-bridge-backend PROPERTIES PREFIX "" CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_link_libraries(psn-bridge-backend PRIVATE CURL::libcurl nlohmann_json::nlohmann_json Threads::Threads)
target_compile_options(psn-bridge-backend PRIVATE -Wall -Wextra -Werror)
if(WIN32)
    get_filename_component(PSN_COMPILER_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(PSN_OBJDUMP NAMES llvm-objdump objdump REQUIRED)
    add_custom_command(TARGET psn-bridge-backend POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DMODULE=$<TARGET_FILE:psn-bridge-backend>"
            "-DSEARCH=${PSN_COMPILER_BIN}"
            "-DOBJDUMP=${PSN_OBJDUMP}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/copy_runtime.cmake"
        VERBATIM)
endif()
if(PSN_HOST_TESTS)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_executable(psn-bridge-test ../tests/test_psn_bridge.cpp)
    set_target_properties(psn-bridge-test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
    target_link_libraries(psn-bridge-test PRIVATE psn-host nlohmann_json::nlohmann_json)
    target_compile_options(psn-bridge-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
    add_test(NAME psn-bridge-http COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/../tests/bridge_http_fixture.py
        $<TARGET_FILE:psn-bridge-test> $<TARGET_FILE:psn-bridge-backend>)
    set_tests_properties(psn-bridge-http PROPERTIES TIMEOUT 40)
endif()
