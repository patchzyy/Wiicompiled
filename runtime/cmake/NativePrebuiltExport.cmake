# Maintainer-only: describe the configured aurora + third-party build so it can
# be harvested into a redistributable precompiled package.
#
# Enabled with -DMKW_NATIVE_PREBUILT_EXPORT_DIR=<dir>. It never changes what is
# compiled: it adds two never-built probe targets and writes plain text files
# describing (a) which library targets aurora produced and where their build
# outputs land, and (b) the exact compile/link interface a consumer of
# aurora::gx/pad/si/vi/mtx receives.
#
# Everything is taken from the real, fully configured project rather than
# hand-written, so the package cannot describe a graph the build does not have.
# The compile/link interface is read back out of build.ninja by
# launcher/Prepare-NativePrebuilt.ps1: generator expressions cannot be evaluated
# for a specific language by file(GENERATE), and $<LINK_ONLY:...> collapses to
# nothing outside a link context, so the generated buildsystem is the only place
# where the fully resolved, correctly ordered values exist.

if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "MKW_NATIVE_PREBUILT_EXPORT_DIR requires the Ninja generator")
endif()

# Enumerates every target defined anywhere in the configured directory-scope
# tree rooted at _dir (aurora-main plus whatever its providers FetchContent'ed,
# whose sources live under the build tree rather than under aurora-main). The
# walk follows real SUBDIRECTORIES properties instead of parsing files, so it
# cannot disagree with the configure that just ran. The names are candidates
# only - each is re-checked below (TARGET existence, library type) and against
# the link closure mkw_np_probe actually pulled in before anything ships.
function(mkw_collect_buildsystem_targets _root_dir _out_var)
    set(_candidates "")
    set(_visited "")
    set(_pending "${_root_dir}")
    while(_pending)
        list(POP_FRONT _pending _dir)
        get_filename_component(_dir "${_dir}" ABSOLUTE)
        if(_dir IN_LIST _visited)
            continue()
        endif()
        list(APPEND _visited "${_dir}")
        get_property(_scope_targets DIRECTORY "${_dir}" PROPERTY BUILDSYSTEM_TARGETS)
        if(_scope_targets)
            list(APPEND _candidates ${_scope_targets})
        endif()
        get_property(_children DIRECTORY "${_dir}" PROPERTY SUBDIRECTORIES)
        foreach(_child IN LISTS _children)
            if(NOT IS_ABSOLUTE "${_child}")
                set(_child "${_dir}/${_child}")
            endif()
            list(APPEND _pending "${_child}")
        endforeach()
    endwhile()
    set(${_out_var} "${_candidates}" PARENT_SCOPE)
endfunction()

set(_mkw_np_probe_dir "${CMAKE_BINARY_DIR}/native_prebuilt_probe")
file(MAKE_DIRECTORY "${_mkw_np_probe_dir}")
file(WRITE "${_mkw_np_probe_dir}/probe.cpp"
    "// Records the compile and link interface aurora's public targets hand to a\n"
    "// consumer. Building mkw_np_probe compiles exactly the aurora + third-party\n"
    "// closure the game products need, and proves the resulting archives link.\n"
    "int main() { return 0; }\n")

# Control: identical target with no aurora dependency. Subtracting its command
# line from the probe's isolates exactly the aurora-derived contribution.
add_executable(mkw_np_probe_control EXCLUDE_FROM_ALL "${_mkw_np_probe_dir}/probe.cpp")
set_target_properties(mkw_np_probe_control PROPERTIES UNITY_BUILD OFF)
target_compile_features(mkw_np_probe_control PRIVATE cxx_std_20)

add_executable(mkw_np_probe EXCLUDE_FROM_ALL "${_mkw_np_probe_dir}/probe.cpp")
set_target_properties(mkw_np_probe PROPERTIES UNITY_BUILD OFF)
target_compile_features(mkw_np_probe PRIVATE cxx_std_20)
# Exactly what mkw_runtime_common and the products link (PublicProducts.cmake)
# with one deliberate exception: pugixml stays a locally compiled single
# translation unit, so it must not enter this closure. The runtime's vendored
# Crypto++ does join the harvest - ~150 translation units that are identical
# for every user under the pinned toolchain and flag set.
target_link_libraries(mkw_np_probe PRIVATE
    aurora::gx aurora::pad aurora::si aurora::vi aurora::mtx
    mkw::cryptopp)
if(NOT MKW_PLATFORM_WINDOWS)
    target_link_libraries(mkw_np_probe PRIVATE mkw::mbedtls)
endif()

get_filename_component(_mkw_np_aurora_dir "${MKW_AURORA_DIR}" ABSOLUTE)
mkw_collect_buildsystem_targets("${_mkw_np_aurora_dir}" _mkw_np_all_targets)
# mkw_cryptopp is defined by this runtime project, not under aurora-main, so
# the scan above cannot see it; it is appended explicitly. The type and closure
# checks below still apply to it.
list(APPEND _mkw_np_all_targets "mkw_cryptopp")
if(NOT MKW_PLATFORM_WINDOWS)
    list(APPEND _mkw_np_all_targets mbedtls mbedx509 mbedcrypto)
    # Keep the TLS archives separate from aurora's aggregate link interface.
    string(CONCAT _mkw_np_mbedtls_lines
        "mbedtls|$<TARGET_LINKER_FILE:MbedTLS::mbedtls>\n"
        "mbedx509|$<TARGET_LINKER_FILE:MbedTLS::mbedx509>\n"
        "mbedcrypto|$<TARGET_LINKER_FILE:MbedTLS::mbedcrypto>\n")
    file(GENERATE OUTPUT "${MKW_NATIVE_PREBUILT_EXPORT_DIR}/mbedtls.txt"
        CONTENT "${_mkw_np_mbedtls_lines}")
endif()
if(NOT _mkw_np_all_targets)
    message(FATAL_ERROR "No buildsystem targets were found under ${_mkw_np_aurora_dir}")
endif()
list(REMOVE_DUPLICATES _mkw_np_all_targets)
list(SORT _mkw_np_all_targets)

set(_mkw_np_lib_targets "")
foreach(_t IN LISTS _mkw_np_all_targets)
    if(NOT TARGET ${_t})
        continue()
    endif()
    get_target_property(_imported ${_t} IMPORTED)
    if(_imported)
        continue()
    endif()
    get_target_property(_type ${_t} TYPE)
    if(_type STREQUAL "STATIC_LIBRARY" OR _type STREQUAL "SHARED_LIBRARY" OR
       _type STREQUAL "MODULE_LIBRARY")
        list(APPEND _mkw_np_lib_targets ${_t})
    endif()
endforeach()
if(NOT _mkw_np_lib_targets)
    message(FATAL_ERROR "aurora produced no library targets to export")
endif()

set(_mkw_np_target_lines "")
foreach(_t IN LISTS _mkw_np_lib_targets)
    string(APPEND _mkw_np_target_lines
        "${_t}|$<TARGET_PROPERTY:${_t},TYPE>|$<TARGET_FILE:${_t}>|$<TARGET_LINKER_FILE:${_t}>\n")
endforeach()
file(GENERATE OUTPUT "${MKW_NATIVE_PREBUILT_EXPORT_DIR}/targets.txt"
    CONTENT "${_mkw_np_target_lines}")

# Dawn stays a prebuilt package on the consumer side too; record how this build
# resolved it so the consumer can reproduce the same imported target.
set(_mkw_np_dawn_line "")
if(TARGET dawn::webgpu_dawn)
    set(_mkw_np_dawn_line
        "$<TARGET_PROPERTY:dawn::webgpu_dawn,TYPE>|$<TARGET_FILE:dawn::webgpu_dawn>|$<TARGET_LINKER_FILE:dawn::webgpu_dawn>\n")
endif()
file(GENERATE OUTPUT "${MKW_NATIVE_PREBUILT_EXPORT_DIR}/dawn.txt"
    CONTENT "${_mkw_np_dawn_line}")

set(_mkw_np_dawn_config_dir "")
if(Dawn_DIR AND EXISTS "${Dawn_DIR}/DawnConfig.cmake")
    set(_mkw_np_dawn_config_dir "${Dawn_DIR}")
endif()

string(REPLACE ";" "," _mkw_np_lib_targets_csv "${_mkw_np_lib_targets}")
file(WRITE "${MKW_NATIVE_PREBUILT_EXPORT_DIR}/meta.txt"
    "aurora_dir=${_mkw_np_aurora_dir}\n"
    "runtime_dir=${CMAKE_CURRENT_SOURCE_DIR}\n"
    "mbedtls_source_dir=${mkw_mbedtls_upstream_SOURCE_DIR}\n"
    "binary_dir=${CMAKE_BINARY_DIR}\n"
    "dawn_config_dir=${_mkw_np_dawn_config_dir}\n"
    "dawn_prebuilt_source_dir=${dawn_prebuilt_SOURCE_DIR}\n"
    "aurora_sdl3_target=${AURORA_SDL3_TARGET}\n"
    "aurora_dawn_version=${AURORA_DAWN_VERSION}\n"
    "aurora_dawn_linkage=${AURORA_DAWN_LINKAGE}\n"
    "cxx_compiler=${CMAKE_CXX_COMPILER}\n"
    "cxx_compiler_version=${CMAKE_CXX_COMPILER_VERSION}\n"
    "library_targets=${_mkw_np_lib_targets_csv}\n")

message(STATUS "Native prebuilt export: ${MKW_NATIVE_PREBUILT_EXPORT_DIR}")
