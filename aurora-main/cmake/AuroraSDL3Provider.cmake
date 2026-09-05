include_guard(GLOBAL)

# Resolve SDL3 dependency based on AURORA_SDL3_PROVIDER and AURORA_SDL3_LINKAGE.
#
# After this module runs, the variable AURORA_SDL3_TARGET is set to the
# appropriate target name for use in target_link_libraries() calls.

# Select the SDL3 target matching the requested linkage.
function(_aurora_sdl3_select_target)
  if (AURORA_SDL3_LINKAGE STREQUAL "static")
    if (TARGET SDL3::SDL3-static)
      set(AURORA_SDL3_TARGET SDL3::SDL3-static CACHE INTERNAL "")
      message(STATUS "aurora: Using static SDL3")
      return()
    elseif (TARGET SDL3::SDL3)
      message(WARNING "aurora: Static SDL3 requested but only shared is available")
      set(AURORA_SDL3_TARGET SDL3::SDL3 CACHE INTERNAL "")
      message(STATUS "aurora: Using shared SDL3")
      return()
    endif ()
  else ()
    if (TARGET SDL3::SDL3)
      set(AURORA_SDL3_TARGET SDL3::SDL3 CACHE INTERNAL "")
      message(STATUS "aurora: Using shared SDL3")
      return()
    elseif (TARGET SDL3::SDL3-static)
      message(WARNING "aurora: Shared SDL3 requested but only static is available")
      set(AURORA_SDL3_TARGET SDL3::SDL3-static CACHE INTERNAL "")
      message(STATUS "aurora: Using static SDL3")
      return()
    endif ()
  endif ()
  message(FATAL_ERROR "No SDL3 target found (looked for SDL3::SDL3 and SDL3::SDL3-static)")
endfunction()

# ── Auto: resolve provider based on platform availability ──
set(_aurora_sdl3_provider "${AURORA_SDL3_PROVIDER}")
if (_aurora_sdl3_provider STREQUAL "auto")
  # Prebuilt SDL3 packages are available for Windows (MSVC and MinGW)
  if (WIN32)
    set(_aurora_sdl3_provider "package")
  else ()
    set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL ON)
    # Aurora uses APIs from the SDL version pinned by AURORA_SDL3_VERSION.
    # Do not silently select an older system package and fail later while
    # compiling its headers.
    find_package(SDL3 ${AURORA_SDL3_VERSION} QUIET)
    set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL OFF)
    if (SDL3_FOUND)
      set(_aurora_sdl3_provider "system")
    else ()
      set(_aurora_sdl3_provider "vendor")
    endif ()
  endif ()
  set(AURORA_SDL3_PROVIDER "${_aurora_sdl3_provider}" CACHE STRING "" FORCE)
  message(STATUS "aurora: SDL3 auto-resolved provider: ${_aurora_sdl3_provider}")
endif ()

if (_aurora_sdl3_provider STREQUAL "system")
  # ── System: find_package(SDL3) ──
  message(STATUS "aurora: Using system SDL3 (provider=system)")
  if (NOT SDL3_FOUND)
    set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL ON)
    find_package(SDL3 ${AURORA_SDL3_VERSION} REQUIRED)
    set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL OFF)
  endif ()
  _aurora_sdl3_select_target()

elseif (_aurora_sdl3_provider STREQUAL "package")
  # ── Package: download custom build or official SDL3 development package ──
  if (NOT AURORA_SDL3_PACKAGE_URL)
    if (MINGW)
      # WIN32 is also true for MinGW. Select the matching GNU-ABI package
      # before considering Visual C++ packages.
      set(AURORA_SDL3_PACKAGE_URL
        "https://github.com/libsdl-org/SDL/releases/download/release-${AURORA_SDL3_VERSION}/SDL3-devel-${AURORA_SDL3_VERSION}-mingw.tar.gz")
    elseif (WIN32)
      # We have custom builds of SDL3 for Win32 x86/AMD64 with libusb support included
      if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86)$")
        string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _sdl3_arch)
        set(AURORA_SDL3_PACKAGE_URL
          "https://github.com/encounter/sdl3-build/releases/download/v${AURORA_SDL3_VERSION}/SDL3-windows-${_sdl3_arch}.tar.gz")
      else ()
        set(AURORA_SDL3_PACKAGE_URL
          "https://github.com/libsdl-org/SDL/releases/download/release-${AURORA_SDL3_VERSION}/SDL3-devel-${AURORA_SDL3_VERSION}-VC.zip")
      endif ()
    else ()
      message(FATAL_ERROR
        "AURORA_SDL3_PROVIDER=package requires AURORA_SDL3_PACKAGE_URL on non-Windows platforms.\n"
        "Official SDL3 packages: https://github.com/libsdl-org/SDL/releases")
    endif ()
  endif ()
  message(STATUS "aurora: Fetching prebuilt SDL3 package from ${AURORA_SDL3_PACKAGE_URL}")

  include(FetchContent)
  FetchContent_Declare(sdl3_prebuilt
    URL "${AURORA_SDL3_PACKAGE_URL}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(sdl3_prebuilt)

  set(_sdl3_search_paths
    "${sdl3_prebuilt_SOURCE_DIR}/cmake"
    "${sdl3_prebuilt_SOURCE_DIR}/lib/cmake/SDL3"
    "${sdl3_prebuilt_SOURCE_DIR}"
  )
  file(GLOB _sdl3_subdirs "${sdl3_prebuilt_SOURCE_DIR}/SDL3-*")
  foreach (_sd IN LISTS _sdl3_subdirs)
    list(APPEND _sdl3_search_paths
      "${_sd}/cmake"
      "${_sd}/lib/cmake/SDL3"
      "${_sd}"
    )
  endforeach ()

  set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL ON)
  find_package(SDL3 REQUIRED CONFIG PATHS ${_sdl3_search_paths} NO_DEFAULT_PATH)
  set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL OFF)
  _aurora_sdl3_select_target()

elseif (_aurora_sdl3_provider STREQUAL "vendor")
  # ── Vendor: FetchContent build from source ──
  if (NOT TARGET SDL3-static AND NOT TARGET SDL3-shared)
    message(STATUS "aurora: Fetching SDL3 (provider=vendor)")

    if (AURORA_SDL3_LINKAGE STREQUAL "static")
      set(SDL_STATIC ON CACHE BOOL "Build SDL3 static library" FORCE)
      set(SDL_SHARED OFF CACHE BOOL "Build SDL3 shared library" FORCE)
    else ()
      set(SDL_STATIC OFF CACHE BOOL "Build SDL3 static library" FORCE)
      set(SDL_SHARED ON CACHE BOOL "Build SDL3 shared library" FORCE)
    endif ()
    if (WIN32)
      set(SDL_LIBC ON CACHE BOOL "Use the system C library" FORCE)
      if (AURORA_SDL3_LIBUSB)
        include("${CMAKE_CURRENT_LIST_DIR}/AuroraLibUSB.cmake")
      endif ()
    endif ()

    include(FetchContent)
    # Source fixes for the vendored SDL (see AuroraSDL3Patches.cmake). A freshly
    # downloaded tarball is patched by the PATCH_COMMAND; a tree handed in through
    # FETCHCONTENT_SOURCE_DIR_SDL skips the download steps, so patch it here.
    set(_aurora_sdl3_patches "${CMAKE_CURRENT_LIST_DIR}/AuroraSDL3Patches.cmake")
    include("${_aurora_sdl3_patches}")
    if (DEFINED FETCHCONTENT_SOURCE_DIR_SDL AND EXISTS "${FETCHCONTENT_SOURCE_DIR_SDL}")
      aurora_sdl3_apply_patches("${FETCHCONTENT_SOURCE_DIR_SDL}")
    endif ()
    FetchContent_Declare(SDL
      URL "https://github.com/libsdl-org/SDL/releases/download/release-${AURORA_SDL3_VERSION}/SDL3-${AURORA_SDL3_VERSION}.tar.gz"
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
      PATCH_COMMAND "${CMAKE_COMMAND}" -DSDL_SOURCE_DIR=<SOURCE_DIR> -P "${_aurora_sdl3_patches}"
      EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(SDL)
  else ()
    message(STATUS "aurora: Using existing SDL3")
  endif ()
  _aurora_sdl3_select_target()

else ()
  message(FATAL_ERROR "Invalid AURORA_SDL3_PROVIDER: ${AURORA_SDL3_PROVIDER} "
    "(must be auto, vendor, system, or package)")
endif ()
