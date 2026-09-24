get_filename_component(MKW_TEST_RUNTIME_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# Keep these independent from Aurora's BUILD_TESTING option: they validate the
# project's host-platform contracts, not Aurora's third-party test suite.
enable_testing()
add_executable(mkw_platform_paths_tests "${MKW_TEST_RUNTIME_DIR}/tests/platform_paths_tests.cpp")
target_link_libraries(mkw_platform_paths_tests PRIVATE mkw_platform)
target_compile_features(mkw_platform_paths_tests PRIVATE cxx_std_17)
add_test(NAME mkw_platform_paths_tests COMMAND mkw_platform_paths_tests)

add_executable(mkw_nand_save_tests "${MKW_TEST_RUNTIME_DIR}/tests/nand_save_tests.cpp")
target_include_directories(mkw_nand_save_tests PRIVATE "${MKW_TEST_RUNTIME_DIR}/include")
target_compile_features(mkw_nand_save_tests PRIVATE cxx_std_17)
add_test(NAME mkw_nand_save_tests COMMAND mkw_nand_save_tests)

add_executable(mkw_nand_settings_tests "${MKW_TEST_RUNTIME_DIR}/tests/nand_settings_tests.cpp")
find_package(Threads REQUIRED)
target_link_libraries(mkw_nand_settings_tests PRIVATE Threads::Threads)
target_include_directories(mkw_nand_settings_tests PRIVATE "${MKW_TEST_RUNTIME_DIR}/include")
target_compile_features(mkw_nand_settings_tests PRIVATE cxx_std_17)
add_test(NAME mkw_nand_settings_tests COMMAND mkw_nand_settings_tests)

add_executable(mkw_sc_serial_tests "${MKW_TEST_RUNTIME_DIR}/tests/sc_serial_tests.cpp")
target_include_directories(mkw_sc_serial_tests PRIVATE "${MKW_TEST_RUNTIME_DIR}/include")
target_compile_features(mkw_sc_serial_tests PRIVATE cxx_std_17)
add_test(NAME mkw_sc_serial_tests COMMAND mkw_sc_serial_tests)

# The input expression engine is self-contained, so it can be exercised without
# linking the runtime or SDL.
add_executable(mkw_input_expr_tests
    "${MKW_TEST_RUNTIME_DIR}/tests/test_expr.cpp"
    "${MKW_TEST_RUNTIME_DIR}/src/input_expr.cpp")
target_include_directories(mkw_input_expr_tests PRIVATE "${MKW_TEST_RUNTIME_DIR}/include")
target_compile_features(mkw_input_expr_tests PRIVATE cxx_std_17)
add_test(NAME mkw_input_expr_tests COMMAND mkw_input_expr_tests)

# HostContext deliberately keeps the platform-specific context primitive out
# of fiber_manager.cpp. Exercise the Linux libco handoff directly so future
# refactors cannot silently remove its headers, implementation, or link edge.
if(MKW_PLATFORM_LINUX)
    add_executable(mkw_linux_host_context_tests
        "${MKW_TEST_RUNTIME_DIR}/tests/host_context_tests.cpp"
        "${MKW_TEST_RUNTIME_DIR}/src/host_context.cpp")
    target_include_directories(mkw_linux_host_context_tests PRIVATE
        "${MKW_TEST_RUNTIME_DIR}/include"
        "${MKW_TEST_RUNTIME_DIR}/third_party/libco")
    target_compile_features(mkw_linux_host_context_tests PRIVATE cxx_std_17)
    target_link_libraries(mkw_linux_host_context_tests PRIVATE mkw::libco)
    add_test(NAME mkw_linux_host_context_tests COMMAND mkw_linux_host_context_tests)
endif()

if(MKW_PLATFORM_WINDOWS)
    add_executable(mkw_windows_host_context_tests
        "${MKW_TEST_RUNTIME_DIR}/tests/host_context_tests.cpp"
        "${MKW_TEST_RUNTIME_DIR}/src/host_context.cpp")
    target_include_directories(mkw_windows_host_context_tests PRIVATE "${MKW_TEST_RUNTIME_DIR}/include")
    target_compile_features(mkw_windows_host_context_tests PRIVATE cxx_std_17)
    add_test(NAME mkw_windows_host_context_tests COMMAND mkw_windows_host_context_tests)
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
    # Compile the same oracle cases for both paths. The v2 binary proves the
    # scalar fallback stays free of FMA; the v3 binary checks the existing
    # vector intrinsic path against the same strict result.
    foreach(profile IN ITEMS v2 v3)
        add_executable(mkw_ppc_pair_fma_${profile}_tests
            "${MKW_TEST_RUNTIME_DIR}/tests/ppc_pair_fma_tests.cpp")
        target_include_directories(mkw_ppc_pair_fma_${profile}_tests PRIVATE
            "${MKW_TEST_RUNTIME_DIR}/include")
        target_compile_features(mkw_ppc_pair_fma_${profile}_tests PRIVATE cxx_std_17)
        target_compile_options(mkw_ppc_pair_fma_${profile}_tests PRIVATE
            -march=x86-64-${profile} -fno-fast-math -ffp-contract=off)
        add_test(NAME mkw_ppc_pair_fma_${profile}_tests COMMAND mkw_ppc_pair_fma_${profile}_tests)
    endforeach()

    add_library(mkw_cpu_baseline_v2_compile OBJECT
        "${MKW_TEST_RUNTIME_DIR}/src/host_cpu_baseline.cpp")
    target_compile_features(mkw_cpu_baseline_v2_compile PRIVATE cxx_std_17)
    target_compile_definitions(mkw_cpu_baseline_v2_compile PRIVATE MKW_X86_CPU_PROFILE_V2=1)
    target_compile_options(mkw_cpu_baseline_v2_compile PRIVATE -march=x86-64-v2 -w)
endif()

if(MKW_PLATFORM_MACOS)
    # Exercise the public host-memory contracts separately from translated products.
    if(MKW_PLATFORM_MACOS_ARM64)
        # Apple Silicon's context ABI is implemented by the local assembly backend.
        enable_language(ASM)
        add_executable(mkw_macos_context_abi_tests
            "${MKW_TEST_RUNTIME_DIR}/tests/macos_context_abi_tests.cpp"
            "${MKW_TEST_RUNTIME_DIR}/src/platform/macos/co_switch.S")
        target_compile_features(mkw_macos_context_abi_tests PRIVATE cxx_std_17)
        add_test(NAME mkw_macos_context_abi_tests COMMAND mkw_macos_context_abi_tests)

        add_executable(mkw_macos_host_context_tests
            "${MKW_TEST_RUNTIME_DIR}/tests/host_context_tests.cpp"
            "${MKW_TEST_RUNTIME_DIR}/src/host_context.cpp"
            "${MKW_TEST_RUNTIME_DIR}/src/platform/macos/co_switch.S")
        target_include_directories(mkw_macos_host_context_tests PRIVATE "${MKW_TEST_RUNTIME_DIR}/include")
    else()
        # Intel macOS follows the same System V AMD64 libco path as Linux.
        add_executable(mkw_macos_host_context_tests
            "${MKW_TEST_RUNTIME_DIR}/tests/host_context_tests.cpp"
            "${MKW_TEST_RUNTIME_DIR}/src/host_context.cpp")
        target_include_directories(mkw_macos_host_context_tests PRIVATE
            "${MKW_TEST_RUNTIME_DIR}/include"
            "${MKW_TEST_RUNTIME_DIR}/third_party/libco")
        target_link_libraries(mkw_macos_host_context_tests PRIVATE mkw::libco)
    endif()
    target_compile_features(mkw_macos_host_context_tests PRIVATE cxx_std_17)
    add_test(NAME mkw_macos_host_context_tests COMMAND mkw_macos_host_context_tests)

    add_executable(mkw_macos_guest_flat_memory_tests
        "${MKW_TEST_RUNTIME_DIR}/tests/macos_guest_flat_memory_tests.cpp"
        "${MKW_TEST_RUNTIME_DIR}/src/guest_flat_memory_macos.cpp")
    target_include_directories(mkw_macos_guest_flat_memory_tests PRIVATE "${MKW_TEST_RUNTIME_DIR}/include")
    target_compile_features(mkw_macos_guest_flat_memory_tests PRIVATE cxx_std_17)
    add_test(NAME mkw_macos_guest_flat_memory_tests COMMAND mkw_macos_guest_flat_memory_tests)
endif()
