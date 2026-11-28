# Uses the real production manager, platform services, SDL HIDAPI, and native
# adapter. Only the unrelated game/overlay callbacks are supplied by the probe.
if(NOT MKW_PLATFORM_LINUX)
    message(FATAL_ERROR "The production Wii Remote probe currently requires Linux")
endif()
function(mkw_add_wiimote_runtime_probe target native)
    add_executable(${target}
        "${CMAKE_CURRENT_LIST_DIR}/../tests/wiimote_runtime_probe.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../src/wiimote/wiimote_host.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../src/wiimote/wiimote_platform_unix.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../src/wiimote/wiimote_extension.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../src/wiimote/wiimote_ext_crypto.cpp")
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../include")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_link_libraries(${target} PRIVATE
        aurora::pad mkw::toml11 mkw_platform ${CMAKE_DL_LIBS})
    if(native)
        target_sources(${target} PRIVATE
            "${CMAKE_CURRENT_LIST_DIR}/../src/wiimote/wiimote_native_protocol.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/../src/wiimote/wiimote_xwiimote.cpp")
        target_link_libraries(${target} PRIVATE mkw::xwiimote)
        add_test(NAME ${target}_configuration COMMAND ${target} --expect-xwiimote)
    else()
        add_test(NAME ${target}_configuration COMMAND ${target} --expect-sdl)
    endif()
endfunction()
mkw_add_wiimote_runtime_probe(mkw_wiimote_runtime_probe ${MKW_ENABLE_XWIIMOTE})
if(MKW_ENABLE_XWIIMOTE)
    # Compile and link the old path too, without the native interface target or
    # any libxwiimote symbols, even while auditing a native-enabled build.
    mkw_add_wiimote_runtime_probe(mkw_wiimote_runtime_probe_sdl FALSE)
endif()

if(MKW_ENABLE_XWIIMOTE)
    # Actual manager with per-device xwiimote ABI fakes and an inert platform.
    # Never initializes SDL HIDAPI or links the Bluetooth pairing implementation.
    add_executable(mkw_wiimote_native_manager_tests
        "${CMAKE_CURRENT_LIST_DIR}/../tests/wiimote_native_manager_tests.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../src/wiimote/wiimote_native_protocol.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../src/wiimote/wiimote_xwiimote.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../src/wiimote/wiimote_extension.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../src/wiimote/wiimote_ext_crypto.cpp")
    target_include_directories(mkw_wiimote_native_manager_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../include")
    target_compile_definitions(mkw_wiimote_native_manager_tests PRIVATE MKW_WIIMOTE_MANAGER_TEST=1)
    target_compile_features(mkw_wiimote_native_manager_tests PRIVATE cxx_std_20)
    target_compile_options(mkw_wiimote_native_manager_tests PRIVATE -UNDEBUG)
    target_link_libraries(mkw_wiimote_native_manager_tests PRIVATE aurora::pad mkw_platform mkw::toml11 mkw::xwiimote ${CMAKE_DL_LIBS})
    add_test(NAME mkw_wiimote_native_manager_tests COMMAND mkw_wiimote_native_manager_tests)
    set_tests_properties(mkw_wiimote_native_manager_tests PROPERTIES TIMEOUT 15)
endif()
