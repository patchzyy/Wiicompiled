# Content identity of the game-independent runtime and the rule deciding whether
# a precompiled runtime archive may replace compiling it. Script mode prints
# exactly one stdout line, runtime_fingerprint=<sha256 hex>:
#   cmake -DMKW_RUNTIME_DIR=<abs runtime dir> [-DMKW_CPPWINRT_INCLUDE_DIR=<dir>]
#         [-DMKW_X86_CPU_PROFILE=v2|v3] [-DCMAKE_OSX_DEPLOYMENT_TARGET=<ver>]
#         -P runtime/cmake/RuntimeFingerprint.cmake
#
# Inputs: src/, include/, cmake/ and CMakeLists.txt (sorted relative paths and
# content hashes), winrt/base.h, the CPU profile and the macOS deployment target.
# third_party/ has its own package fingerprint; game/ is per-user. Dot-files
# differ between machines.

function(mkw_runtime_fingerprint_inputs out_var runtime_dir)
    get_filename_component(_root "${runtime_dir}" ABSOLUTE)
    file(GLOB_RECURSE _files LIST_DIRECTORIES false RELATIVE "${_root}"
        "${_root}/src/*" "${_root}/include/*" "${_root}/cmake/*")
    list(FILTER _files EXCLUDE REGEX "(^|/)\\.")
    list(APPEND _files "CMakeLists.txt")
    list(SORT _files COMPARE STRING CASE SENSITIVE)
    set(${out_var} "${_files}" PARENT_SCOPE)
endfunction()

function(mkw_runtime_fingerprint out_var runtime_dir cppwinrt_dir cpu_profile)
    get_filename_component(_root "${runtime_dir}" ABSOLUTE)
    mkw_runtime_fingerprint_inputs(_files "${_root}")
    set(_manifest "")
    foreach(_file IN LISTS _files)
        if(NOT EXISTS "${_root}/${_file}")
            message(FATAL_ERROR "Runtime fingerprint input is missing: ${_root}/${_file}")
        endif()
        file(SHA256 "${_root}/${_file}" _hash)
        string(APPEND _manifest "${_file} ${_hash}\n")
    endforeach()
    if(cppwinrt_dir)
        if(NOT EXISTS "${cppwinrt_dir}/winrt/base.h")
            message(FATAL_ERROR "MKW_CPPWINRT_INCLUDE_DIR does not contain winrt/base.h: ${cppwinrt_dir}")
        endif()
        file(SHA256 "${cppwinrt_dir}/winrt/base.h" _hash)
        string(APPEND _manifest "cppwinrt:winrt/base.h ${_hash}\n")
    endif()
    string(APPEND _manifest "x86_cpu_profile ${cpu_profile}\n")
    string(APPEND _manifest "osx_deployment_target ${CMAKE_OSX_DEPLOYMENT_TARGET}\n")
    string(SHA256 _fingerprint "${_manifest}")
    set(${out_var} "${_fingerprint}" PARENT_SCOPE)
endfunction()

# Sets out_use to TRUE/FALSE and out_reason to a one-line explanation. Pure, so
# it can be exercised in script mode without a configured project.
cmake_policy(PUSH)
if(POLICY CMP0174)
    cmake_policy(SET CMP0174 NEW)
endif()
function(mkw_runtime_prebuilt_decision out_use out_reason)
    cmake_parse_arguments(PARSE_ARGV 2 _d ""
        "ARCHIVE;ENABLED;X86_64;ARCHIVE_EXISTS;PACKAGE_PROFILE;PROFILE;PACKAGE_FINGERPRINT;FINGERPRINT" "")
    set(_use FALSE)
    if(NOT _d_ARCHIVE)
        set(_reason "the native prebuilt package has no runtime archive")
    elseif(NOT _d_ENABLED)
        set(_reason "MKW_USE_PREBUILT_RUNTIME is OFF")
    elseif(NOT _d_X86_64)
        set(_reason "the precompiled runtime is x86_64-only")
    elseif(NOT _d_ARCHIVE_EXISTS)
        set(_reason "the runtime archive is missing: ${_d_ARCHIVE}")
    elseif(NOT _d_PACKAGE_PROFILE STREQUAL _d_PROFILE)
        set(_reason "the package was built for x86-64-${_d_PACKAGE_PROFILE}, this build targets x86-64-${_d_PROFILE}")
    elseif(NOT _d_PACKAGE_FINGERPRINT)
        set(_reason "the package records no runtime fingerprint")
    elseif(NOT _d_FINGERPRINT OR NOT _d_PACKAGE_FINGERPRINT STREQUAL _d_FINGERPRINT)
        set(_reason "runtime sources differ from the package (package ${_d_PACKAGE_FINGERPRINT}, workspace ${_d_FINGERPRINT})")
    else()
        set(_use TRUE)
        set(_reason "fingerprint ${_d_FINGERPRINT} matches")
    endif()
    set(${out_use} ${_use} PARENT_SCOPE)
    set(${out_reason} "${_reason}" PARENT_SCOPE)
endfunction()
cmake_policy(POP)

if(CMAKE_SCRIPT_MODE_FILE)
    get_filename_component(_mkw_script "${CMAKE_SCRIPT_MODE_FILE}" REALPATH)
    get_filename_component(_mkw_self "${CMAKE_CURRENT_LIST_FILE}" REALPATH)
endif()
if(CMAKE_SCRIPT_MODE_FILE AND _mkw_script STREQUAL _mkw_self)
    if(NOT MKW_RUNTIME_DIR OR NOT IS_DIRECTORY "${MKW_RUNTIME_DIR}")
        message(FATAL_ERROR "Pass -DMKW_RUNTIME_DIR=<runtime directory>")
    endif()
    if(NOT MKW_X86_CPU_PROFILE)
        set(MKW_X86_CPU_PROFILE "v3")
    endif()
    # Same default runtime/CMakeLists.txt forces on every platform before project().
    if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0")
    endif()
    mkw_runtime_fingerprint(_mkw_fingerprint "${MKW_RUNTIME_DIR}" "${MKW_CPPWINRT_INCLUDE_DIR}" "${MKW_X86_CPU_PROFILE}")
    # message() writes to stderr; the echo child inherits stdout.
    execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "runtime_fingerprint=${_mkw_fingerprint}")
endif()
