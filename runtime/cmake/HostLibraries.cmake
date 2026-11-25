# Shared by the product build and the dependency-free platform test configuration.
get_filename_component(MKW_HOST_RUNTIME_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# POSIX guest-fiber scheduling (runtime/src/host_context.cpp) needs a symmetric
# stackful-coroutine primitive to stand in for Win32 Fibers. libco's co_switch() transfers
# directly to any other created coroutine, matching SwitchToFiber's semantics exactly (unlike
# asymmetric resume/yield coroutine libraries, which would need every call site restructured).
# Vendored from upstream (higan-emu/libco @ e18e09d, 2019-10-16, ISC license; valgrind.h is
# separately BSD-style licensed, see third_party/libco/LICENSE). Windows keeps native Fibers;
# Apple Silicon uses the project's x18-safe AArch64 assembly backend, while Intel macOS uses
# libco's existing System V AMD64 backend.
if(MKW_PLATFORM_LINUX OR MKW_PLATFORM_MACOS_X86_64)
    add_library(mkw_libco STATIC "${MKW_HOST_RUNTIME_DIR}/third_party/libco/libco.c")
    add_library(mkw::libco ALIAS mkw_libco)
    target_include_directories(mkw_libco PUBLIC "${MKW_HOST_RUNTIME_DIR}/third_party/libco")
    set_target_properties(mkw_libco PROPERTIES UNITY_BUILD OFF)
endif()

# This deliberately small library contains host services that are safe to
# validate before guest memory and fiber work makes a full runtime build viable.
add_library(mkw_platform STATIC "${MKW_HOST_RUNTIME_DIR}/src/platform/host_platform.cpp")
target_include_directories(mkw_platform PUBLIC "${MKW_HOST_RUNTIME_DIR}/include")
target_compile_features(mkw_platform PUBLIC cxx_std_17)
set_target_properties(mkw_platform PROPERTIES UNITY_BUILD OFF)

if(MKW_PLATFORM_WINDOWS)
    target_compile_definitions(mkw_platform PRIVATE NOMINMAX)
    target_link_libraries(mkw_platform PUBLIC shell32 ole32 uuid)
endif()
