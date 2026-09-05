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
