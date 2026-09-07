cmake_minimum_required(VERSION 3.16)

# Share the same test registrations with the full runtime build, while allowing
# CI and developers to run them without translated game files or Aurora.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(WiiCompiledRuntimeTests LANGUAGES C CXX)
    enable_testing()
endif()

get_filename_component(runtime_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
find_package(Threads REQUIRED)

function(mkw_add_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE "${runtime_root}/include")
    target_compile_features(${name} PRIVATE cxx_std_17)
    target_link_libraries(${name} PRIVATE Threads::Threads)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

if(NOT TARGET mkw_platform)
    add_library(mkw_platform STATIC "${runtime_root}/src/platform/host_platform.cpp")
    target_include_directories(mkw_platform PUBLIC "${runtime_root}/include")
    target_compile_features(mkw_platform PUBLIC cxx_std_17)
endif()
if(WIN32)
    target_link_libraries(mkw_platform PUBLIC shell32 ole32)
endif()
mkw_add_test(mkw_platform_paths_tests platform_paths_tests.cpp)
target_link_libraries(mkw_platform_paths_tests PRIVATE mkw_platform)

mkw_add_test(mkw_nand_save_tests nand_save_tests.cpp)
mkw_add_test(mkw_nand_move_tests nand_move_tests.cpp
    "${runtime_root}/src/hle/storage/nand_file_ops.cpp")
target_include_directories(mkw_nand_move_tests PRIVATE "${runtime_root}/src/hle/storage")
mkw_add_test(mkw_nand_settings_tests nand_settings_tests.cpp)
mkw_add_test(mkw_sc_serial_tests sc_serial_tests.cpp)
mkw_add_test(mkw_input_expr_tests test_expr.cpp "${runtime_root}/src/input_expr.cpp")

if(WIN32)
    mkw_add_test(mkw_windows_host_context_tests host_context_tests.cpp
        "${runtime_root}/src/host_context.cpp")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(NOT TARGET mkw::libco)
        add_library(mkw_libco STATIC "${runtime_root}/third_party/libco/libco.c")
        add_library(mkw::libco ALIAS mkw_libco)
        target_include_directories(mkw_libco PUBLIC "${runtime_root}/third_party/libco")
    endif()
    mkw_add_test(mkw_linux_host_context_tests host_context_tests.cpp
        "${runtime_root}/src/host_context.cpp")
    target_link_libraries(mkw_linux_host_context_tests PRIVATE mkw::libco)
elseif(APPLE)
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|ARM64|aarch64)$")
        message(FATAL_ERROR "The macOS runtime tests require Apple Silicon")
    endif()
    enable_language(ASM)
    set(context_asm "${runtime_root}/src/platform/macos/co_switch.S")
    mkw_add_test(mkw_macos_context_abi_tests macos_context_abi_tests.cpp "${context_asm}")
    mkw_add_test(mkw_macos_host_context_tests host_context_tests.cpp
        "${runtime_root}/src/host_context.cpp" "${context_asm}")
    mkw_add_test(mkw_macos_guest_flat_memory_tests macos_guest_flat_memory_tests.cpp
        "${runtime_root}/src/guest_flat_memory_macos.cpp")
endif()
