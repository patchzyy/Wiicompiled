cmake_minimum_required(VERSION 3.25)
project(metalfx_interop LANGUAGES CXX)

if(NOT APPLE)
  message(FATAL_ERROR "The MetalFX interoperability probe requires macOS")
endif()
enable_language(OBJCXX)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET 13.0)

# Use the same Dawn package as Aurora; no separate download or renderer build.
find_package(Threads REQUIRED)
find_package(Dawn CONFIG REQUIRED)
add_executable(metalfx_interop main.mm ../../lib/webgpu/metalfx.mm)
target_include_directories(metalfx_interop PRIVATE ../../lib)
target_compile_options(metalfx_interop PRIVATE -fobjc-arc -Wall -Wextra)
target_link_libraries(metalfx_interop PRIVATE dawn::webgpu_dawn
  "-framework MetalFX" "-framework Metal" "-framework IOSurface" "-framework Foundation")
enable_testing()
add_test(NAME metalfx_interop COMMAND metalfx_interop)
set_tests_properties(metalfx_interop PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

add_executable(metalfx_stub_test stub_test.cpp ../../lib/webgpu/metalfx_stub.cpp)
target_include_directories(metalfx_stub_test PRIVATE ../../lib)
target_compile_features(metalfx_stub_test PRIVATE cxx_std_20)
target_link_libraries(metalfx_stub_test PRIVATE dawn::webgpu_dawn)
add_test(NAME metalfx_stub COMMAND metalfx_stub_test)
