get_filename_component(MKW_TEST_RUNTIME_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# Keep these independent from Aurora's BUILD_TESTING option: they validate the
# project's host-platform contracts, not Aurora's third-party test suite.
enable_testing()
if(MKW_BUILD_PSQ_TESTS)
    add_executable(mkw_psq_helpers_tests
        "${MKW_TEST_RUNTIME_DIR}/tests/psq_helpers_tests.cpp"
        "${MKW_TEST_RUNTIME_DIR}/src/ppc_quantized.cpp")
    target_include_directories(mkw_psq_helpers_tests PRIVATE
        "${MKW_TEST_RUNTIME_DIR}/tests/psq_memory"
        "${MKW_TEST_RUNTIME_DIR}/include/isa"
        "${MKW_TEST_RUNTIME_DIR}/include")
    target_compile_features(mkw_psq_helpers_tests PRIVATE cxx_std_17)
    target_compile_options(mkw_psq_helpers_tests PRIVATE
        -O2 -fno-fast-math -ffp-contract=off -fno-slp-vectorize)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
        target_compile_options(mkw_psq_helpers_tests PRIVATE
            -march=x86-64-${MKW_X86_CPU_PROFILE})
    endif()
    set_target_properties(mkw_psq_helpers_tests PROPERTIES UNITY_BUILD OFF)
    add_test(NAME mkw_psq_helpers_tests COMMAND mkw_psq_helpers_tests)
    add_test(NAME mkw_psq_reserved_tests COMMAND "${CMAKE_COMMAND}"
        "-DPSQ_TEST_EXECUTABLE=$<TARGET_FILE:mkw_psq_helpers_tests>"
        -P "${MKW_TEST_RUNTIME_DIR}/tests/psq_reserved_tests.cmake")
endif()

add_executable(mkw_platform_paths_tests "${MKW_TEST_RUNTIME_DIR}/tests/platform_paths_tests.cpp")
target_link_libraries(mkw_platform_paths_tests PRIVATE mkw_platform)
target_compile_features(mkw_platform_paths_tests PRIVATE cxx_std_17)
add_test(NAME mkw_platform_paths_tests COMMAND mkw_platform_paths_tests)

add_executable(mkw_runtime_config_tests "${MKW_TEST_RUNTIME_DIR}/tests/runtime_config_tests.cpp")
target_include_directories(mkw_runtime_config_tests PRIVATE
    "${MKW_TEST_RUNTIME_DIR}/include"
    "${MKW_TEST_RUNTIME_DIR}/third_party/toml11")
target_compile_features(mkw_runtime_config_tests PRIVATE cxx_std_20)
add_test(NAME mkw_runtime_config_tests COMMAND mkw_runtime_config_tests)

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
    # Probe at the base ISA, including OS support for AVX register state. Cross
    # builds execute target code only through a configured emulator.
    if(NOT CMAKE_CROSSCOMPILING OR CMAKE_CROSSCOMPILING_EMULATOR)
        include(CheckCXXSourceRuns)
        include(CMakePushCheckState)
        cmake_push_check_state(RESET)
        set(CMAKE_REQUIRED_FLAGS "-march=x86-64")
        check_cxx_source_runs([=[
            #include <cpuid.h>
            int main() {
                unsigned a, b, c, d;
                if (__get_cpuid_max(0, nullptr) < 7 ||
                    __get_cpuid_max(0x80000000u, nullptr) < 0x80000001u)
                    return 1;
                __cpuid_count(1, 0, a, b, c, d);
                // v2 plus FMA, MOVBE, XSAVE, OSXSAVE, AVX and F16C.
                const unsigned leaf1 = (1u << 0) | (1u << 9) | (1u << 12) |
                    (1u << 13) | (1u << 19) | (1u << 20) | (1u << 22) |
                    (1u << 23) | (1u << 26) | (1u << 27) | (1u << 28) | (1u << 29);
                if ((c & leaf1) != leaf1) return 1;
                __cpuid_count(7, 0, a, b, c, d);
                const unsigned leaf7 = (1u << 3) | (1u << 5) | (1u << 8);
                if ((b & leaf7) != leaf7) return 1;
                __cpuid_count(0x80000001u, 0, a, b, c, d);
                if ((c & 0x21u) != 0x21u) return 1; // LAHF-SAHF and LZCNT.
                __asm__ __volatile__("xgetbv" : "=a"(a), "=d"(d) : "c"(0));
                return (a & 0x6u) == 0x6u ? 0 : 1;
            }
        ]=] MKW_HOST_SUPPORTS_X86_V3)
        cmake_pop_check_state()
    endif()
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
        if((NOT CMAKE_CROSSCOMPILING OR CMAKE_CROSSCOMPILING_EMULATOR) AND
           (profile STREQUAL "v2" OR MKW_HOST_SUPPORTS_X86_V3))
            add_test(NAME mkw_ppc_pair_fma_${profile}_tests COMMAND mkw_ppc_pair_fma_${profile}_tests)
        endif()
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

    add_executable(mkw_macos_external_audio_tests
        "${MKW_TEST_RUNTIME_DIR}/tests/macos_external_audio_tests.cpp"
        "${MKW_TEST_RUNTIME_DIR}/src/external_audio_macos.cpp")
    target_include_directories(mkw_macos_external_audio_tests PRIVATE "${MKW_TEST_RUNTIME_DIR}/include")
    target_compile_features(mkw_macos_external_audio_tests PRIVATE cxx_std_17)
    add_test(NAME mkw_macos_external_audio_tests COMMAND mkw_macos_external_audio_tests)
endif()
