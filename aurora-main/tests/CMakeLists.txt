include(FetchContent)
include(GoogleTest)

if (NOT TARGET gtest)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(googletest)
endif ()

if (AURORA_ENABLE_GX)
  # GX FIFO round-trip tests
  # Compile GX sources directly to avoid pulling in the full renderer/WebGPU runtime
  add_executable(gx_fifo_tests
    gx_fifo_test.cpp
    gx_test_stubs.cpp
    texture_bind_group_cache_key_test.cpp
    renderer_regression_test.cpp
    ../lib/gfx/efb_ram_encoder.cpp
    # GX API implementations (encoders)
    ../lib/dolphin/gx/GXBump.cpp
    ../lib/dolphin/gx/GXCull.cpp
    ../lib/dolphin/gx/GXCpu2Efb.cpp
    ../lib/dolphin/gx/GXDispList.cpp
    ../lib/dolphin/gx/GXDraw.cpp
    ../lib/dolphin/gx/GXExtra.cpp
    ../lib/dolphin/gx/GXFifo.cpp
    ../lib/dolphin/gx/GXFrameBuffer.cpp
    ../lib/dolphin/gx/GXGeometry.cpp
    ../lib/dolphin/gx/GXGet.cpp
    ../lib/dolphin/gx/GXLighting.cpp
    ../lib/dolphin/gx/GXManage.cpp
    ../lib/dolphin/gx/GXAurora.cpp
    ../lib/dolphin/gx/GXPixel.cpp
    ../lib/dolphin/gx/GXTev.cpp
    ../lib/dolphin/gx/GXTexture.cpp
    ../lib/dolphin/gx/GXTransform.cpp
    ../lib/dolphin/gx/GXVert.cpp
    # FIFO/command processor (encoder + decoder)
    ../lib/gx/fifo.cpp
    ../lib/gx/command_processor.cpp
    ../lib/gx/shader_info.cpp
    ../lib/gx/frame_interpolation.cpp
  )

  target_include_directories(gx_fifo_tests PRIVATE
    ../include
    ../lib
    ../lib/dolphin/gx
  )

  target_compile_definitions(gx_fifo_tests PRIVATE AURORA TARGET_PC)

  target_link_libraries(gx_fifo_tests PRIVATE
    gtest
    gtest_main
    fmt::fmt
    xxhash
    absl::flat_hash_map
    absl::btree
    dawn::dawncpp_headers
    TracyClient
    ${AURORA_SDL3_TARGET}
  )

  gtest_discover_tests(gx_fifo_tests)

  option(AURORA_BUILD_GPU_TESTS "Build renderer pixel tests requiring a graphics device" OFF)
  if (AURORA_BUILD_GPU_TESTS)
    add_executable(gx_readback_tests gpu_readback_test.cpp)
    target_compile_features(gx_readback_tests PRIVATE cxx_std_20)
    target_include_directories(gx_readback_tests PRIVATE ../lib)
    target_link_libraries(gx_readback_tests PRIVATE
      aurora::core aurora::gx aurora::pad aurora::vi aurora::mtx aurora::si
      dawn::dawncpp_headers absl::flat_hash_map absl::btree TracyClient)
    add_test(NAME gx_readback_tests COMMAND gx_readback_tests "${CMAKE_CURRENT_BINARY_DIR}/readback-cache")
    set_tests_properties(gx_readback_tests PROPERTIES TIMEOUT 90)
  endif ()
endif () # AURORA_ENABLE_GX

# DVD API tests
if (TARGET aurora::dvd)
  add_executable(dvd_tests test_dvd.cpp)
  target_link_libraries(dvd_tests PRIVATE aurora::dvd gtest gtest_main)

  # Pass disc image path as a compile definition if provided
  if (DEFINED DVD_TEST_IMAGE AND EXISTS "${DVD_TEST_IMAGE}")
    target_compile_definitions(dvd_tests PRIVATE DVD_TEST_IMAGE="${DVD_TEST_IMAGE}")
  endif ()

  gtest_discover_tests(dvd_tests)
endif ()

add_executable(os_alloc_tests
  os_alloc_test.cpp
  os_test_globals.cpp
  ../lib/dolphin/os/OSAlloc.cpp
  ../lib/logging.cpp
)
target_include_directories(os_alloc_tests PRIVATE
  ../include
)
target_compile_definitions(os_alloc_tests PRIVATE AURORA TARGET_PC)
target_link_libraries(os_alloc_tests PRIVATE gtest gtest_main fmt::fmt)
gtest_discover_tests(os_alloc_tests)
