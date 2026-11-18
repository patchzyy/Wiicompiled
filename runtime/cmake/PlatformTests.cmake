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

    add_executable(mkw_psq_gqr_state_free_tests
        "${MKW_TEST_RUNTIME_DIR}/tests/psq_gqr_state_free_tests.cpp"
        "${MKW_TEST_RUNTIME_DIR}/src/ppc_quantized.cpp")
    target_include_directories(mkw_psq_gqr_state_free_tests PRIVATE
        "${MKW_TEST_RUNTIME_DIR}/tests/psq_memory"
        "${MKW_TEST_RUNTIME_DIR}/include/isa"
        "${MKW_TEST_RUNTIME_DIR}/include")
    target_compile_features(mkw_psq_gqr_state_free_tests PRIVATE cxx_std_17)
    target_compile_options(mkw_psq_gqr_state_free_tests PRIVATE
        -O2 -fno-fast-math -ffp-contract=off -fno-slp-vectorize)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
        target_compile_options(mkw_psq_gqr_state_free_tests PRIVATE
            -march=x86-64-${MKW_X86_CPU_PROFILE})
    endif()
    set_target_properties(mkw_psq_gqr_state_free_tests PROPERTIES UNITY_BUILD OFF)
    add_test(NAME mkw_psq_gqr_state_free_tests COMMAND mkw_psq_gqr_state_free_tests)
endif()

add_executable(mkw_ppc_cr_logical_tests
    "${MKW_TEST_RUNTIME_DIR}/tests/ppc_cr_logical_tests.cpp")
target_include_directories(mkw_ppc_cr_logical_tests PRIVATE "${MKW_TEST_RUNTIME_DIR}/include")
target_compile_features(mkw_ppc_cr_logical_tests PRIVATE cxx_std_17)
target_compile_options(mkw_ppc_cr_logical_tests PRIVATE
    -fno-fast-math -ffp-contract=off)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
    target_compile_options(mkw_ppc_cr_logical_tests PRIVATE -march=x86-64-${MKW_X86_CPU_PROFILE})
endif()
set_target_properties(mkw_ppc_cr_logical_tests PROPERTIES UNITY_BUILD OFF)
add_test(NAME mkw_ppc_cr_logical_tests COMMAND mkw_ppc_cr_logical_tests)

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

if(MKW_PLATFORM_MACOS)
    set(MKW_MEMORY_TEST_MAPPING_SOURCE "${MKW_TEST_RUNTIME_DIR}/src/guest_flat_memory_macos.cpp")
else()
    set(MKW_MEMORY_TEST_MAPPING_SOURCE "${MKW_TEST_RUNTIME_DIR}/src/guest_flat_memory.cpp")
endif()
add_executable(mkw_memory_access_tests
    "${MKW_TEST_RUNTIME_DIR}/tests/memory_access_tests.cpp"
    "${MKW_TEST_RUNTIME_DIR}/src/memory.cpp"
    "${MKW_MEMORY_TEST_MAPPING_SOURCE}")
target_include_directories(mkw_memory_access_tests PRIVATE "${MKW_TEST_RUNTIME_DIR}/include")
target_compile_features(mkw_memory_access_tests PRIVATE cxx_std_20)
target_compile_options(mkw_memory_access_tests PRIVATE -fno-fast-math -ffp-contract=off)
if(MKW_PLATFORM_WINDOWS)
    target_link_libraries(mkw_memory_access_tests PRIVATE dbghelp)
endif()
add_test(NAME mkw_memory_access_tests COMMAND mkw_memory_access_tests)
set_tests_properties(mkw_memory_access_tests PROPERTIES TIMEOUT 60)

add_executable(mkw_nand_save_tests "${MKW_TEST_RUNTIME_DIR}/tests/nand_save_tests.cpp")
target_include_directories(mkw_nand_save_tests PRIVATE "${MKW_TEST_RUNTIME_DIR}/include")
target_compile_features(mkw_nand_save_tests PRIVATE cxx_std_17)
add_test(NAME mkw_nand_save_tests COMMAND mkw_nand_save_tests)

add_executable(mkw_nand_guest_path_tests "${MKW_TEST_RUNTIME_DIR}/tests/nand_guest_path_tests.cpp")
target_include_directories(mkw_nand_guest_path_tests PRIVATE "${MKW_TEST_RUNTIME_DIR}/include")
target_compile_features(mkw_nand_guest_path_tests PRIVATE cxx_std_17)
add_test(NAME mkw_nand_guest_path_tests COMMAND mkw_nand_guest_path_tests)

add_executable(mkw_nand_settings_tests "${MKW_TEST_RUNTIME_DIR}/tests/nand_settings_tests.cpp")
find_package(Threads REQUIRED)
target_link_libraries(mkw_memory_access_tests PRIVATE Threads::Threads)
target_link_libraries(mkw_runtime_config_tests PRIVATE Threads::Threads)

add_executable(mkw_host_file_tests "${MKW_TEST_RUNTIME_DIR}/tests/host_file_tests.cpp")
target_include_directories(mkw_host_file_tests PRIVATE "${MKW_TEST_RUNTIME_DIR}/include")
target_compile_features(mkw_host_file_tests PRIVATE cxx_std_17)
target_link_libraries(mkw_host_file_tests PRIVATE Threads::Threads)
add_test(NAME mkw_host_file_tests COMMAND mkw_host_file_tests)

add_executable(mkw_system_bridge_tests
    "${MKW_TEST_RUNTIME_DIR}/tests/system_bridge_tests.cpp"
    "${MKW_TEST_RUNTIME_DIR}/src/system_bridge.cpp")
target_include_directories(mkw_system_bridge_tests BEFORE PRIVATE
    "${MKW_TEST_RUNTIME_DIR}/tests/system_bridge_support"
    "${MKW_TEST_RUNTIME_DIR}/include"
    "${MKW_TEST_RUNTIME_DIR}/third_party/toml11")
target_compile_features(mkw_system_bridge_tests PRIVATE cxx_std_20)
set_target_properties(mkw_system_bridge_tests PROPERTIES UNITY_BUILD OFF)
add_test(NAME mkw_system_bridge_tests COMMAND mkw_system_bridge_tests)
if(MKW_PLATFORM_WINDOWS)
    add_executable(mkw_host_file_recovery_tests "${MKW_TEST_RUNTIME_DIR}/tests/host_file_recovery_tests.cpp")
    target_include_directories(mkw_host_file_recovery_tests PRIVATE "${MKW_TEST_RUNTIME_DIR}/include")
    target_compile_features(mkw_host_file_recovery_tests PRIVATE cxx_std_17)
    target_link_libraries(mkw_host_file_recovery_tests PRIVATE advapi32)
    add_test(NAME mkw_host_file_recovery_tests COMMAND mkw_host_file_recovery_tests)
endif()

add_executable(mkw_u8_archive_tests "${MKW_TEST_RUNTIME_DIR}/tests/u8_archive_tests.cpp")
target_include_directories(mkw_u8_archive_tests PRIVATE "${MKW_TEST_RUNTIME_DIR}/include")
target_compile_features(mkw_u8_archive_tests PRIVATE cxx_std_17)
add_test(NAME mkw_u8_archive_tests COMMAND mkw_u8_archive_tests)

add_executable(mkw_runtime_config_io_tests "${MKW_TEST_RUNTIME_DIR}/tests/runtime_config_io_tests.cpp")
target_include_directories(mkw_runtime_config_io_tests PRIVATE
    "${MKW_TEST_RUNTIME_DIR}/include" "${MKW_TEST_RUNTIME_DIR}/third_party/toml11")
target_compile_features(mkw_runtime_config_io_tests PRIVATE cxx_std_20)
target_link_libraries(mkw_runtime_config_io_tests PRIVATE Threads::Threads)
add_test(NAME mkw_runtime_config_io_tests COMMAND mkw_runtime_config_io_tests)
if(MKW_PLATFORM_WINDOWS)
    target_link_libraries(mkw_runtime_config_tests PRIVATE advapi32)
    target_link_libraries(mkw_host_file_tests PRIVATE advapi32)
    target_link_libraries(mkw_runtime_config_io_tests PRIVATE advapi32)
endif()

if(TARGET SDL3::SDL3)
    add_executable(mkw_pad_trigger_tests "${MKW_TEST_RUNTIME_DIR}/tests/pad_trigger_tests.cpp")
    target_include_directories(mkw_pad_trigger_tests PRIVATE
        "${MKW_TEST_RUNTIME_DIR}/tests/pad_trigger_support"
        "${MKW_TEST_RUNTIME_DIR}/include" "${MKW_AURORA_DIR}/include")
    target_compile_definitions(mkw_pad_trigger_tests PRIVATE TARGET_PC=1 AURORA)
    target_compile_features(mkw_pad_trigger_tests PRIVATE cxx_std_20)
    target_link_libraries(mkw_pad_trigger_tests PRIVATE SDL3::SDL3)
    set_target_properties(mkw_pad_trigger_tests PROPERTIES UNITY_BUILD OFF)
    add_test(NAME mkw_pad_trigger_tests COMMAND mkw_pad_trigger_tests)
endif()

if(TARGET gx_fifo_tests)
    target_sources(gx_fifo_tests PRIVATE
        "${MKW_TEST_RUNTIME_DIR}/tests/gx_fifo_invalidation_tests.cpp"
        "${MKW_TEST_RUNTIME_DIR}/tests/gx_direct_draw_tests.cpp"
        "${MKW_TEST_RUNTIME_DIR}/src/hle/gx/gx_fifo.cpp")
    target_include_directories(gx_fifo_tests PRIVATE
        "${MKW_AURORA_DIR}/tests" "${MKW_TEST_RUNTIME_DIR}/include"
        "${MKW_TEST_RUNTIME_DIR}/third_party/toml11")
    target_compile_features(gx_fifo_tests PRIVATE cxx_std_20)
    set_target_properties(gx_fifo_tests PROPERTIES UNITY_BUILD OFF)
    if(MKW_PLATFORM_WINDOWS)
        target_link_libraries(gx_fifo_tests PRIVATE advapi32)
    endif()

    get_target_property(_mkw_gx_sources gx_fifo_tests SOURCES)
    get_target_property(_mkw_gx_source_dir gx_fifo_tests SOURCE_DIR)
    get_target_property(_mkw_gx_includes gx_fifo_tests INCLUDE_DIRECTORIES)
    get_target_property(_mkw_gx_links gx_fifo_tests LINK_LIBRARIES)
    set(_mkw_gx_encoder_sources)
    foreach(_mkw_gx_source IN LISTS _mkw_gx_sources)
        if(_mkw_gx_source MATCHES "(^|/)lib/")
            get_filename_component(_mkw_gx_absolute "${_mkw_gx_source}" ABSOLUTE
                BASE_DIR "${_mkw_gx_source_dir}")
            list(APPEND _mkw_gx_encoder_sources "${_mkw_gx_absolute}")
        endif()
    endforeach()

    add_executable(mkw_gx_texture_binding_tests
        "${MKW_TEST_RUNTIME_DIR}/tests/gx_texture_binding_tests.cpp"
        "${MKW_AURORA_DIR}/tests/gx_test_stubs.cpp"
        ${_mkw_gx_encoder_sources})
    add_executable(mkw_gx_sync_tests
        "${MKW_TEST_RUNTIME_DIR}/tests/gx_worker_tests.cpp"
        "${MKW_TEST_RUNTIME_DIR}/tests/gx_copy_tests.cpp"
        "${MKW_AURORA_DIR}/tests/gx_test_stubs.cpp"
        ${_mkw_gx_encoder_sources})
    target_compile_definitions(mkw_gx_sync_tests PRIVATE MKW_GX_COPY_TESTING=1)
    foreach(_mkw_gx_test IN ITEMS mkw_gx_texture_binding_tests mkw_gx_sync_tests)
        target_include_directories(${_mkw_gx_test} PRIVATE
            ${_mkw_gx_includes} "${MKW_TEST_RUNTIME_DIR}/tests")
        target_compile_definitions(${_mkw_gx_test} PRIVATE AURORA TARGET_PC=1)
        target_compile_features(${_mkw_gx_test} PRIVATE cxx_std_20)
        target_link_libraries(${_mkw_gx_test} PRIVATE ${_mkw_gx_links} Threads::Threads)
        set_target_properties(${_mkw_gx_test} PROPERTIES UNITY_BUILD OFF)
        aurora_copy_runtime_dlls(${_mkw_gx_test})
        add_test(NAME ${_mkw_gx_test} COMMAND ${_mkw_gx_test})
        set_tests_properties(${_mkw_gx_test} PROPERTIES TIMEOUT 120)
    endforeach()

    add_executable(mkw_gx_dl_cache_tests "${MKW_TEST_RUNTIME_DIR}/tests/gx_dl_cache_tests.cpp")
    target_include_directories(mkw_gx_dl_cache_tests PRIVATE
        "${MKW_TEST_RUNTIME_DIR}/include"
        "${MKW_TEST_RUNTIME_DIR}/third_party/toml11"
        "${MKW_AURORA_DIR}/include")
    target_compile_features(mkw_gx_dl_cache_tests PRIVATE cxx_std_20)
    target_compile_definitions(mkw_gx_dl_cache_tests PRIVATE AURORA TARGET_PC=1 NOMINMAX)
    target_link_libraries(mkw_gx_dl_cache_tests PRIVATE xxHash::xxhash ${AURORA_SDL3_TARGET})
    set_target_properties(mkw_gx_dl_cache_tests PROPERTIES UNITY_BUILD OFF)
    add_test(NAME mkw_gx_dl_cache_tests COMMAND mkw_gx_dl_cache_tests)
endif()

function(mkw_add_ax_mix_test target)
    add_executable(${target} "${MKW_TEST_RUNTIME_DIR}/tests/ax_mix_kernels_tests.cpp")
    target_include_directories(${target} PRIVATE
        "${MKW_TEST_RUNTIME_DIR}/include" "${MKW_TEST_RUNTIME_DIR}/src/hle/audio")
    target_compile_features(${target} PRIVATE cxx_std_17)
    set_target_properties(${target} PROPERTIES UNITY_BUILD OFF)
    if(NOT CMAKE_CROSSCOMPILING OR CMAKE_CROSSCOMPILING_EMULATOR)
        add_test(NAME ${target} COMMAND ${target})
    endif()
endfunction()

mkw_add_ax_mix_test(mkw_ax_mix_kernels_tests)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    target_compile_definitions(mkw_ax_mix_kernels_tests PRIVATE MKW_AX_MIX_REQUIRE_NEON=1)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|X86_64)$")
    target_compile_options(mkw_ax_mix_kernels_tests PRIVATE -march=x86-64-v2)
endif()

add_executable(mkw_fiber_manager_tests
  "${MKW_TEST_RUNTIME_DIR}/tests/fiber_manager_tests.cpp"
  "${MKW_TEST_RUNTIME_DIR}/src/fiber_manager.cpp"
  "${MKW_TEST_RUNTIME_DIR}/src/hle/os/os_scheduler.cpp"
  "${MKW_TEST_RUNTIME_DIR}/src/hle/storage/nand_callbacks.cpp"
  "${MKW_TEST_RUNTIME_DIR}/src/hle/os/os_thread.cpp")
target_include_directories(mkw_fiber_manager_tests PRIVATE
  "${MKW_TEST_RUNTIME_DIR}/tests/fiber_support"
  "${MKW_TEST_RUNTIME_DIR}/include" "${MKW_TEST_RUNTIME_DIR}/src"
  "${MKW_TEST_RUNTIME_DIR}/third_party"
  "${MKW_TEST_RUNTIME_DIR}/third_party/toml11")
target_compile_features(mkw_fiber_manager_tests PRIVATE cxx_std_20)
target_link_libraries(mkw_fiber_manager_tests PRIVATE Threads::Threads)
if(MKW_PLATFORM_MACOS_ARM64)
  enable_language(ASM)
  target_sources(mkw_fiber_manager_tests PRIVATE "${MKW_TEST_RUNTIME_DIR}/src/platform/macos/co_switch.S")
elseif(TARGET mkw::libco)
  target_link_libraries(mkw_fiber_manager_tests PRIVATE mkw::libco)
endif()
set_target_properties(mkw_fiber_manager_tests PROPERTIES UNITY_BUILD OFF)
add_test(NAME mkw_fiber_manager_tests COMMAND mkw_fiber_manager_tests)
add_executable(mkw_audio_backend_ownership_tests "${MKW_TEST_RUNTIME_DIR}/tests/audio_backend_ownership_tests.cpp" "${MKW_TEST_RUNTIME_DIR}/src/audio_backend.cpp")
target_include_directories(mkw_audio_backend_ownership_tests BEFORE PRIVATE "${MKW_TEST_RUNTIME_DIR}/tests/host_io" "${MKW_TEST_RUNTIME_DIR}/include" "${MKW_TEST_RUNTIME_DIR}/src")
target_compile_features(mkw_audio_backend_ownership_tests PRIVATE cxx_std_20)
add_test(NAME mkw_audio_backend_ownership_tests COMMAND mkw_audio_backend_ownership_tests)
add_executable(mkw_network_receive_tests "${MKW_TEST_RUNTIME_DIR}/tests/network_receive_tests.cpp")
target_include_directories(mkw_network_receive_tests PRIVATE "${MKW_TEST_RUNTIME_DIR}/include" "${MKW_TEST_RUNTIME_DIR}/src")
target_compile_features(mkw_network_receive_tests PRIVATE cxx_std_20)
target_link_libraries(mkw_network_receive_tests PRIVATE Threads::Threads)
if(MKW_PLATFORM_WINDOWS)
  target_link_libraries(mkw_network_receive_tests PRIVATE ws2_32)
endif()
set_target_properties(mkw_audio_backend_ownership_tests mkw_network_receive_tests PROPERTIES UNITY_BUILD OFF)
add_test(NAME mkw_network_receive_tests COMMAND mkw_network_receive_tests)
add_test(NAME mkw_windows_seh_order_tests COMMAND "${CMAKE_COMMAND}" "-DSEH_SOURCE=${MKW_TEST_RUNTIME_DIR}/src/main.cpp" -P "${MKW_TEST_RUNTIME_DIR}/tests/windows_seh_order_tests.cmake")
set_tests_properties(mkw_fiber_manager_tests mkw_network_receive_tests PROPERTIES TIMEOUT 60)

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

add_executable(mkw_motion_trick_tests
    "${MKW_TEST_RUNTIME_DIR}/tests/motion_trick_tests.cpp"
    "${MKW_TEST_RUNTIME_DIR}/src/motion_trick_detector.cpp")
target_include_directories(mkw_motion_trick_tests PRIVATE "${MKW_TEST_RUNTIME_DIR}/include")
target_compile_features(mkw_motion_trick_tests PRIVATE cxx_std_17)
# Match the runtime's float model so the bad-data guard is tested as shipped.
target_compile_options(mkw_motion_trick_tests PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffast-math>)
add_test(NAME mkw_motion_trick_tests COMMAND mkw_motion_trick_tests)

# The emulated Bluetooth controller is pure host code.
add_executable(mkw_bluetooth_controller_tests
    "${MKW_TEST_RUNTIME_DIR}/tests/bluetooth_controller_tests.cpp"
    "${MKW_TEST_RUNTIME_DIR}/src/hle/bluetooth/bt_controller.cpp"
    "${MKW_TEST_RUNTIME_DIR}/src/hle/bluetooth/bt_remote_link.cpp"
    "${MKW_TEST_RUNTIME_DIR}/src/hle/bluetooth/bt_sdp.cpp")
target_include_directories(mkw_bluetooth_controller_tests PRIVATE "${MKW_TEST_RUNTIME_DIR}/include")
target_compile_features(mkw_bluetooth_controller_tests PRIVATE cxx_std_20)
add_test(NAME mkw_bluetooth_controller_tests COMMAND mkw_bluetooth_controller_tests)

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
    if((NOT CMAKE_CROSSCOMPILING OR CMAKE_CROSSCOMPILING_EMULATOR) AND MKW_HOST_SUPPORTS_X86_V3)
        mkw_add_ax_mix_test(mkw_ax_mix_kernels_v3_tests)
        target_compile_options(mkw_ax_mix_kernels_v3_tests PRIVATE -march=x86-64-v3)
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
