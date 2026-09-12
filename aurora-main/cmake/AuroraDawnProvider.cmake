include_guard(GLOBAL)

# Resolve Dawn/WebGPU dependency based on AURORA_DAWN_PROVIDER and AURORA_DAWN_LINKAGE.
#
# After this module runs:
#   - dawn::webgpu_dawn target is available
#   - dawn::dawncpp_headers target is available (for tests)
#   - DAWN_ENABLE_* backend variables are set for the target platform
#   - AURORA_DAWN_IS_SHARED is set to TRUE/FALSE

# When using a non-vendored Dawn, we don't get DAWN_ENABLE_* from its build.
# Infer from the target platform instead.
function(_aurora_dawn_set_platform_backends)
  if (WIN32)
    set(DAWN_ENABLE_D3D12 ON CACHE INTERNAL "")
    # Disable D3D11 because this Aurora fork does not support it.
    set(DAWN_ENABLE_D3D11 OFF CACHE INTERNAL "")
    set(DAWN_ENABLE_VULKAN ON CACHE INTERNAL "")
    set(DAWN_ENABLE_METAL OFF CACHE INTERNAL "")
    set(DAWN_ENABLE_DESKTOP_GL OFF CACHE INTERNAL "")
    set(DAWN_ENABLE_OPENGLES OFF CACHE INTERNAL "")
    set(DAWN_ENABLE_NULL ON CACHE INTERNAL "")
  elseif (APPLE)
    set(DAWN_ENABLE_D3D12 OFF CACHE INTERNAL "")
    set(DAWN_ENABLE_D3D11 OFF CACHE INTERNAL "")
    set(DAWN_ENABLE_VULKAN OFF CACHE INTERNAL "")
    set(DAWN_ENABLE_METAL ON CACHE INTERNAL "")
    set(DAWN_ENABLE_DESKTOP_GL OFF CACHE INTERNAL "")
    set(DAWN_ENABLE_OPENGLES OFF CACHE INTERNAL "")
    set(DAWN_ENABLE_NULL ON CACHE INTERNAL "")
  else () # Linux / other
    set(DAWN_ENABLE_D3D12 OFF CACHE INTERNAL "")
    set(DAWN_ENABLE_D3D11 OFF CACHE INTERNAL "")
    set(DAWN_ENABLE_VULKAN ON CACHE INTERNAL "")
    set(DAWN_ENABLE_METAL OFF CACHE INTERNAL "")
    set(DAWN_ENABLE_DESKTOP_GL OFF CACHE INTERNAL "")
    set(DAWN_ENABLE_OPENGLES OFF CACHE INTERNAL "")
    set(DAWN_ENABLE_NULL ON CACHE INTERNAL "")
  endif ()
endfunction()

# ── Auto: resolve provider based on platform availability ──
set(_aurora_dawn_provider "${AURORA_DAWN_PROVIDER}")
if (_aurora_dawn_provider STREQUAL "auto")
  # Prebuilt Dawn packages available for: windows-{amd64,arm64}, linux-{x86_64,aarch64}, darwin-{arm64,x86_64}
  set(_has_package FALSE)
  if (WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|ARM64|aarch64)$")
    set(_has_package TRUE)
  elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|aarch64)$")
    set(_has_package TRUE)
  elseif (APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|x86_64)$")
    set(_has_package TRUE)
  endif ()

  if (_has_package)
    set(_aurora_dawn_provider "package")
  else ()
    set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL ON)
    find_package(Dawn QUIET)
    set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL OFF)
    if (Dawn_FOUND)
      set(_aurora_dawn_provider "system")
    else ()
      set(_aurora_dawn_provider "vendor")
    endif ()
  endif ()
  set(AURORA_DAWN_PROVIDER "${_aurora_dawn_provider}" CACHE STRING "" FORCE)
  message(STATUS "aurora: Dawn auto-resolved provider: ${_aurora_dawn_provider}")
endif ()

if (_aurora_dawn_provider STREQUAL "vendor")
  # ── Vendor: FetchContent build from source ──
  if (NOT TARGET webgpu_dawn)
    message(STATUS "aurora: Fetching Dawn (provider=vendor, linkage=${AURORA_DAWN_LINKAGE})")

    if (AURORA_DAWN_LINKAGE STREQUAL "shared")
      set(DAWN_BUILD_MONOLITHIC_LIBRARY SHARED CACHE INTERNAL
        "Bundle all dawn components into a single shared library.")
    else ()
      set(DAWN_BUILD_MONOLITHIC_LIBRARY STATIC CACHE INTERNAL
        "Bundle all dawn components into a single static library.")
    endif ()
    set(DAWN_BUILD_SAMPLES OFF CACHE INTERNAL "Disable Dawn sample applications")
    set(DAWN_BUILD_BENCHMARKS OFF CACHE INTERNAL "Disable Dawn benchmarks")
    set(DAWN_SUPPORTS_GLFW_FOR_WINDOWING OFF CACHE INTERNAL "Disable Dawn GLFW windowing")
    set(DAWN_ENABLE_DESKTOP_GL OFF CACHE INTERNAL "Enable compilation of the OpenGL backend")
    set(DAWN_ENABLE_OPENGLES OFF CACHE INTERNAL "Enable compilation of the OpenGL ES backend")
    set(DAWN_FETCH_DEPENDENCIES ON CACHE INTERNAL
      "Use fetch_dawn_dependencies.py as an alternative to using depot_tools")
    if (CMAKE_SYSTEM_NAME STREQUAL Linux)
      set(DAWN_USE_WAYLAND ON CACHE INTERNAL "Enable support for Wayland surface")
    endif ()
    set(TINT_BUILD_TESTS OFF CACHE INTERNAL "Build tests")
    set(TINT_BUILD_CMD_TOOLS OFF CACHE INTERNAL "Build the Tint command line tools")
    if (NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY OR CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL$")
      set(ABSL_MSVC_STATIC_RUNTIME OFF CACHE INTERNAL "Link static runtime libraries")
    else ()
      set(ABSL_MSVC_STATIC_RUNTIME ON CACHE INTERNAL "Link static runtime libraries")
    endif ()

    include(FetchContent)
    FetchContent_Declare(dawn
      URL "https://github.com/google/dawn/archive/refs/tags/${AURORA_DAWN_VERSION}.tar.gz"
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
      EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(dawn)
    if (NOT TARGET webgpu_dawn)
      message(FATAL_ERROR "Failed to make dawn available")
    endif ()
  else ()
    message(STATUS "aurora: Using existing Dawn")
  endif ()

  if (AURORA_DAWN_LINKAGE STREQUAL "shared")
    set(AURORA_DAWN_IS_SHARED TRUE PARENT_SCOPE)
  else ()
    set(AURORA_DAWN_IS_SHARED FALSE PARENT_SCOPE)
  endif ()

elseif (_aurora_dawn_provider STREQUAL "system")
  # ── System: find_package(Dawn) ──
  message(STATUS "aurora: Using system Dawn (provider=system)")
  if (NOT Dawn_FOUND)
    set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL ON)
    find_package(Dawn REQUIRED)
    set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL OFF)
  endif ()
  if (NOT TARGET dawn::webgpu_dawn)
    message(FATAL_ERROR "find_package(Dawn) succeeded but dawn::webgpu_dawn target not found")
  endif ()
  _aurora_dawn_set_platform_backends()

  get_target_property(_dawn_type dawn::webgpu_dawn TYPE)
  if (_dawn_type STREQUAL "SHARED_LIBRARY")
    set(AURORA_DAWN_IS_SHARED TRUE PARENT_SCOPE)
  else ()
    set(AURORA_DAWN_IS_SHARED FALSE PARENT_SCOPE)
  endif ()

elseif (_aurora_dawn_provider STREQUAL "package")
  # ── Package: download prebuilt Dawn install tree ──
  if (NOT AURORA_DAWN_PACKAGE_URL)
    string(TOLOWER "${CMAKE_SYSTEM_NAME}" _dawn_system)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _dawn_arch)
    if (_dawn_system STREQUAL "windows")
      if (_dawn_arch STREQUAL "x86_64")
        set(_dawn_arch "amd64")
      elseif (_dawn_arch STREQUAL "aarch64")
        set(_dawn_arch "arm64")
      endif ()
    endif ()
    set(AURORA_DAWN_PACKAGE_URL
      "https://github.com/theofficialgman/dawn-build/releases/download/${AURORA_DAWN_VERSION}/dawn-${_dawn_system}-${_dawn_arch}.tar.gz")

    # A release asset is mutable: the same tag has already served two different windows-amd64 archives,
    # and a cached extraction is never re-verified. Pin the digest for the combinations we ship.
    if (NOT AURORA_DAWN_PACKAGE_URL_HASH AND AURORA_DAWN_VERSION STREQUAL "v20260603.191052")
      if (_dawn_system STREQUAL "windows" AND _dawn_arch STREQUAL "amd64")
        set(AURORA_DAWN_PACKAGE_URL_HASH
          "SHA256=13be9cff8b9b179c42dcd16aeabb6effcc8f0dfdcc14463eda2a5caeda225142")
      elseif (_dawn_system STREQUAL "windows" AND _dawn_arch STREQUAL "arm64")
        set(AURORA_DAWN_PACKAGE_URL_HASH
          "SHA256=bf2d921110f14a1d6553f673c5597988e66c02af5587e4a1fee167937d247734")
      elseif (_dawn_system STREQUAL "linux" AND _dawn_arch STREQUAL "x86_64")
        set(AURORA_DAWN_PACKAGE_URL_HASH
          "SHA256=7adcf241bb2a24ec0c576609f2d67203e0e65db9c5a286ca2bbb6281fa644b35")
      elseif (_dawn_system STREQUAL "linux" AND _dawn_arch STREQUAL "aarch64")
        set(AURORA_DAWN_PACKAGE_URL_HASH
          "SHA256=2415e253d46f91b2d72fc73bf6055fb31b98b67c773dc546e1991b1cf019732f")
      elseif (_dawn_system STREQUAL "darwin" AND _dawn_arch STREQUAL "arm64")
        set(AURORA_DAWN_PACKAGE_URL_HASH
          "SHA256=0a8ea8eb0159fc0ba1083c52155d9376fb173cffe690b400464a6ad8881bb461")
      elseif (_dawn_system STREQUAL "darwin" AND _dawn_arch STREQUAL "x86_64")
        set(AURORA_DAWN_PACKAGE_URL_HASH
          "SHA256=5fe2c7a2a8b4cb82acee4af16779a83ae333c7657b9dc1a5008f5fd1f5ad5f80")
      elseif (_dawn_system STREQUAL "ios" AND _dawn_arch STREQUAL "arm64")
        set(AURORA_DAWN_PACKAGE_URL_HASH
          "SHA256=f97701d26fd1f25bbcc260b4c31736ede134c730c12556029e2470fde967f424")
      elseif (_dawn_system STREQUAL "android" AND _dawn_arch STREQUAL "aarch64")
        set(AURORA_DAWN_PACKAGE_URL_HASH
          "SHA256=0e63e8cbf53551f703f582d1306f4257c0380353f66b53369d96952ce6d9f934")
      endif ()
    endif ()
  endif ()
  message(STATUS "aurora: Fetching prebuilt Dawn package from ${AURORA_DAWN_PACKAGE_URL}")

  set(_dawn_prebuilt_hash_argument "")
  if (AURORA_DAWN_PACKAGE_URL_HASH)
    set(_dawn_prebuilt_hash_argument URL_HASH "${AURORA_DAWN_PACKAGE_URL_HASH}")
  endif ()

  include(FetchContent)
  FetchContent_Declare(dawn_prebuilt
    URL "${AURORA_DAWN_PACKAGE_URL}"
    ${_dawn_prebuilt_hash_argument}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(dawn_prebuilt)

  # Detect package layout: may be nested in a subdirectory
  set(_dawn_pkg_dir "${dawn_prebuilt_SOURCE_DIR}")
  file(GLOB _dawn_subdirs
    "${dawn_prebuilt_SOURCE_DIR}/dawn-*"
    "${dawn_prebuilt_SOURCE_DIR}/Dawn-*"
  )
  foreach (_sd IN LISTS _dawn_subdirs)
    if (IS_DIRECTORY "${_sd}")
      set(_dawn_pkg_dir "${_sd}")
      break()
    endif ()
  endforeach ()
  if (IS_DIRECTORY "${dawn_prebuilt_SOURCE_DIR}/dawn-install")
    set(_dawn_pkg_dir "${dawn_prebuilt_SOURCE_DIR}/dawn-install")
  endif ()

  # Static Dawn packages may link Threads::Threads
  find_package(Threads QUIET)

  # Find DawnConfig.cmake in the package
  set(_dawn_cmake_found FALSE)
  foreach (_cmake_path
    "${_dawn_pkg_dir}/lib/cmake/Dawn"
    "${_dawn_pkg_dir}/lib64/cmake/Dawn"
    "${_dawn_pkg_dir}/cmake/Dawn"
    "${_dawn_pkg_dir}/cmake"
  )
    if (EXISTS "${_cmake_path}/DawnConfig.cmake")
      set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL ON)
      find_package(Dawn REQUIRED CONFIG PATHS "${_cmake_path}" NO_DEFAULT_PATH)
      set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL OFF)
      set(_dawn_cmake_found TRUE)
      break()
    endif ()
  endforeach ()

  if (NOT _dawn_cmake_found OR NOT TARGET dawn::webgpu_dawn)
    message(FATAL_ERROR
      "Dawn package does not contain DawnConfig.cmake.\n"
      "Searched: ${_dawn_pkg_dir}/{lib,lib64,cmake}/cmake/Dawn/\n"
      "The package must be a Dawn install tree built with DAWN_ENABLE_INSTALL=ON.")
  endif ()

  _aurora_dawn_set_platform_backends()

  get_target_property(_dawn_pkg_type dawn::webgpu_dawn TYPE)
  if (_dawn_pkg_type STREQUAL "SHARED_LIBRARY")
    set(AURORA_DAWN_IS_SHARED TRUE PARENT_SCOPE)

  else ()
    set(AURORA_DAWN_IS_SHARED FALSE PARENT_SCOPE)
  endif ()

else ()
  message(FATAL_ERROR "Invalid AURORA_DAWN_PROVIDER: ${AURORA_DAWN_PROVIDER} "
    "(must be auto, vendor, system, or package)")
endif ()

# Ensure dawn::dawncpp_headers target exists (needed by tests).
# When Dawn is vendored, this target is created by Dawn's CMake.
# For package/system providers, create it from the same include dirs.
if (NOT TARGET dawn::dawncpp_headers)
  add_library(dawn_dawncpp_headers INTERFACE IMPORTED GLOBAL)
  get_target_property(_dawn_inc dawn::webgpu_dawn INTERFACE_INCLUDE_DIRECTORIES)
  if (NOT _dawn_inc)
    if (TARGET dawn::dawn_public_config)
      get_target_property(_dawn_inc dawn::dawn_public_config INTERFACE_INCLUDE_DIRECTORIES)
    endif ()
  endif ()
  if (_dawn_inc)
    target_include_directories(dawn_dawncpp_headers INTERFACE ${_dawn_inc})
  else ()
    target_link_libraries(dawn_dawncpp_headers INTERFACE dawn::webgpu_dawn)
  endif ()
  add_library(dawn::dawncpp_headers ALIAS dawn_dawncpp_headers)
endif ()
