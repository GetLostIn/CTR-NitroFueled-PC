cmake_minimum_required(VERSION 3.24)
project(native_psn_integration LANGUAGES C)
enable_testing()
find_package(Threads REQUIRED)
set(PSN_HOST_TESTS ON CACHE BOOL "Build the common PSN contract tests" FORCE)
add_subdirectory(../../platform/psn psn-host)

add_executable(native-psn-test test_runtime_psn.c probe_stubs.c ../../src/runtime.c ../../src/runtime_psn.c
    ../../src/runtime_services.c ../../src/runtime_net.c)
target_include_directories(native-psn-test PRIVATE ../.. ../../src)
target_compile_definitions(native-psn-test PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(native-psn-test PRIVATE -UNDEBUG -Wall -Wextra -Werror
    -Wno-unknown-warning-option -Wno-unused-but-set-global)
set_target_properties(native-psn-test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_link_libraries(native-psn-test PRIVATE psn-host)
if(WIN32)
    target_sources(native-psn-test PRIVATE ../../src/compat_win.c)
    target_link_libraries(native-psn-test PRIVATE ws2_32 bcrypt)
endif()
foreach(mode offline empty online selected selected-offline)
    add_test(NAME native-psn-${mode}
        COMMAND native-psn-test $<TARGET_FILE:psn-backend-fixture> ${mode})
    set_tests_properties(native-psn-${mode} PROPERTIES TIMEOUT 10)
endforeach()
add_test(NAME native-psn-selected-startup-failure COMMAND ${CMAKE_COMMAND}
    -DTEST_PROGRAM=$<TARGET_FILE:native-psn-test>
    -DFIXTURE=$<TARGET_FILE:psn-backend-fixture>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/check_start_failure.cmake)
set_tests_properties(native-psn-selected-startup-failure PROPERTIES TIMEOUT 10)

# Keep the existing private-server contracts under test on Windows as well.
add_executable(native-net-test test_net_portable.c probe_stubs.c)
target_include_directories(native-net-test PRIVATE ../.. ../../src)
target_compile_options(native-net-test PRIVATE -UNDEBUG -Wall -Wextra -Werror
    -Wno-unknown-warning-option -Wno-unused-but-set-global)
set_target_properties(native-net-test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_link_libraries(native-net-test PRIVATE Threads::Threads)
if(WIN32)
    target_sources(native-net-test PRIVATE ../../src/compat_win.c)
    target_link_libraries(native-net-test PRIVATE ws2_32 bcrypt)
endif()
add_test(NAME native-network-offline COMMAND native-net-test --offline)
add_test(NAME native-network-private-server COMMAND native-net-test)
set_tests_properties(native-network-offline native-network-private-server PROPERTIES TIMEOUT 20)
