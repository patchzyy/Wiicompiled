# Cross-compile a thin x86_64 macOS build from an Apple Silicon Mac.
# Pass this file on the first configure with:
#   -DCMAKE_TOOLCHAIN_FILE=/absolute/path/to/macos-x86_64-toolchain.cmake
#
# CMAKE_SYSTEM_PROCESSOR is deliberately declared here rather than inferred
# from CMAKE_OSX_ARCHITECTURES, so target-aware CMake dependencies select their
# x86_64 artifacts.
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_OSX_ARCHITECTURES x86_64 CACHE STRING
    "Target macOS architectures" FORCE)

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT DEFINED CMAKE_CROSSCOMPILING_EMULATOR
   AND NOT DEFINED ENV{CMAKE_CROSSCOMPILING_EMULATOR})
    execute_process(COMMAND /usr/bin/arch -x86_64 /usr/bin/true
        RESULT_VARIABLE _mkw_can_run_x86_64 OUTPUT_QUIET ERROR_QUIET)
    if(_mkw_can_run_x86_64 EQUAL 0)
        set(CMAKE_CROSSCOMPILING_EMULATOR /usr/bin/arch -x86_64)
    endif()
endif()
