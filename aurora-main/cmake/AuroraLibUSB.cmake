# libusb for SDL3's HIDAPI joystick drivers on Windows.
#
# The official GameCube adapter (WUP-028) is a vendor-specific USB device, not HID, so SDL3
# can only reach it through libusb - which the official SDL3 packages leave out. The vendored
# SDL3 build compiles libusb from the pinned upstream release and links it in statically.

include(FetchContent)
FetchContent_Declare(libusb
  URL "https://github.com/libusb/libusb/releases/download/v${AURORA_LIBUSB_VERSION}/libusb-${AURORA_LIBUSB_VERSION}.tar.bz2"
  DOWNLOAD_EXTRACT_TIMESTAMP FALSE
)
# Upstream ships no CMakeLists.txt, so this only populates the source tree.
FetchContent_MakeAvailable(libusb)

set(_libusb_root "${libusb_SOURCE_DIR}/libusb")
add_library(usb-1.0 STATIC
  "${_libusb_root}/core.c"
  "${_libusb_root}/descriptor.c"
  "${_libusb_root}/hotplug.c"
  "${_libusb_root}/io.c"
  "${_libusb_root}/strerror.c"
  "${_libusb_root}/sync.c"
  "${_libusb_root}/os/events_windows.c"
  "${_libusb_root}/os/threads_windows.c"
  "${_libusb_root}/os/windows_common.c"
  "${_libusb_root}/os/windows_usbdk.c"
  "${_libusb_root}/os/windows_winusb.c"
)
target_include_directories(usb-1.0
  PUBLIC "${_libusb_root}"
  PRIVATE "${CMAKE_CURRENT_LIST_DIR}/libusb" "${_libusb_root}/os"
)
set_target_properties(usb-1.0 PROPERTIES UNITY_BUILD OFF)
add_library(LibUSB::LibUSB ALIAS usb-1.0)

# SDL's FindLibUSB expects an installed copy. Satisfy its presence checks with this target
# instead: the alias pre-empts the imported target it would otherwise create, and the link
# probe is answered up front because the archive does not exist until build time.
set(LibUSB_INCLUDE_PATH "${_libusb_root}" CACHE PATH "" FORCE)
set(LibUSB_LIBRARY "usb-1.0" CACHE STRING "" FORCE)
set(HAVE_LIBUSB_H 1 CACHE INTERNAL "" FORCE)
set(SDL_HIDAPI_LIBUSB ON CACHE BOOL "" FORCE)
set(SDL_HIDAPI_LIBUSB_SHARED OFF CACHE BOOL "" FORCE)
