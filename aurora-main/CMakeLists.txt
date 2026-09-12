cmake_minimum_required(VERSION 3.25)
project(aurora LANGUAGES C CXX)
if (APPLE)
    enable_language(OBJC)
    enable_language(OBJCXX)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(AURORA_ENABLE_GX "Enable GX implementation and WebGPU renderer" ON)
option(AURORA_ENABLE_DVD "Enable DVD implementation backed by nod" OFF)  # TODO: need to fix tests
option(AURORA_ENABLE_CARD "Enable CARD implementation based on kabufuda" ON)
option(AURORA_CACHE_USE_ZSTD "Compress WebGPU cache entries with zstd" ON)

# Dependency versions
set(AURORA_DAWN_VERSION "v20260603.191052" CACHE STRING "Dawn version tag (https://github.com/encounter/dawn-build/releases)")
set(AURORA_SDL3_VERSION "3.4.4" CACHE STRING "SDL3 version tag (https://github.com/libsdl-org/SDL/releases)")
set(AURORA_NOD_VERSION "v2.0.0-alpha.8" CACHE STRING "nod version tag (https://github.com/encounter/nod/releases)")
set(AURORA_LIBUSB_VERSION "1.0.30" CACHE STRING "libusb version tag (https://github.com/libusb/libusb/releases)")

# Platform-specific defaults
if (CMAKE_CROSSCOMPILING)
  # Cross-compiling: vendor everything, don't use system packages
  set(_default_provider "vendor")
  set(_default_linkage "static")
else ()
  set(_default_provider "auto")
  if (WIN32)
    set(_default_linkage "shared")
  else ()
    set(_default_linkage "static")
  endif ()
endif ()

# Dawn/WebGPU (if AURORA_ENABLE_GX)
set(AURORA_DAWN_PROVIDER "${_default_provider}" CACHE STRING
  "How to provide Dawn: auto, vendor (build from source), system (find_package/imported), package (download prebuilt)")
set_property(CACHE AURORA_DAWN_PROVIDER PROPERTY STRINGS auto vendor system package)
set(AURORA_DAWN_LINKAGE "${_default_linkage}" CACHE STRING "Dawn linkage type preference")
set_property(CACHE AURORA_DAWN_LINKAGE PROPERTY STRINGS shared static)

# SDL3
set(AURORA_SDL3_PROVIDER "${_default_provider}" CACHE STRING
  "How to provide SDL3: auto, vendor (build from source), system (find_package/imported), or package (download prebuilt)")
set_property(CACHE AURORA_SDL3_PROVIDER PROPERTY STRINGS auto vendor system package)
set(AURORA_SDL3_LINKAGE "${_default_linkage}" CACHE STRING "SDL3 linkage type preference")
set_property(CACHE AURORA_SDL3_LINKAGE PROPERTY STRINGS shared static)
option(AURORA_SDL3_LIBUSB "Build the vendored SDL3 with libusb on Windows (official GameCube adapter support)" ON)

# nod (if AURORA_ENABLE_DVD)
set(AURORA_NOD_PROVIDER "${_default_provider}" CACHE STRING
  "How to provide nod: auto, vendor (build from source), system (find_package/imported), package (download prebuilt)")
set_property(CACHE AURORA_NOD_PROVIDER PROPERTY STRINGS auto vendor system package)
set(AURORA_NOD_LINKAGE "${_default_linkage}" CACHE STRING "nod linkage type preference")
set_property(CACHE AURORA_NOD_LINKAGE PROPERTY STRINGS shared static)

add_subdirectory(extern)

include(cmake/aurora_core.cmake)
include(cmake/aurora_os.cmake)
if (AURORA_ENABLE_GX)
  include(cmake/aurora_gx.cmake)
  include(cmake/aurora_gd.cmake)
endif ()
include(cmake/aurora_pad.cmake)
include(cmake/aurora_ms.cmake)
include(cmake/aurora_si.cmake)
include(cmake/aurora_main.cmake)
include(cmake/aurora_mtx.cmake)
include(cmake/aurora_vi.cmake)
if(AURORA_ENABLE_CARD)
  include(cmake/aurora_card.cmake)
endif()
if (AURORA_ENABLE_DVD)
  include(cmake/aurora_dvd.cmake)
endif ()

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  add_subdirectory(examples)
endif ()

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT CMAKE_CROSSCOMPILING)
  enable_testing()
  add_subdirectory(tests)
endif ()

option(AURORA_BUILD_METALFX_PRESENTATION_TEST "Build the macOS MetalFX presentation test" OFF)
if (AURORA_BUILD_METALFX_PRESENTATION_TEST AND APPLE AND AURORA_ENABLE_GX AND DAWN_ENABLE_METAL AND AURORA_METALFX_FRAMEWORK)
    add_executable(metalfx_presentation_test tests/metalfx_interop/presentation_test.cpp)
    target_include_directories(metalfx_presentation_test PRIVATE lib)
    target_link_libraries(metalfx_presentation_test PRIVATE aurora::core aurora::gx aurora::vi dawn::webgpu_dawn)
endif ()
