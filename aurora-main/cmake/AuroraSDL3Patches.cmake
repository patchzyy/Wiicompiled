# Source-level fixes applied to the vendored SDL3 tree before it is built.
#
# Each patch is an exact string replacement, checked and idempotent: a tree that
# already carries the fix is left alone, a tree where the anchor text is missing
# (a different SDL version) stops the configure with a clear message instead of
# silently building without the fix.
#
# Used two ways:
#   - included by AuroraSDL3Provider.cmake, which calls aurora_sdl3_apply_patches()
#     on a pre-provided source tree (FETCHCONTENT_SOURCE_DIR_SDL);
#   - run as `cmake -DSDL_SOURCE_DIR=<dir> -P AuroraSDL3Patches.cmake` from the
#     FetchContent PATCH_COMMAND for a freshly extracted tarball.

function(_aurora_sdl3_replace file description old new)
  file(READ "${file}" _content)
  string(FIND "${_content}" "${new}" _already)
  if (NOT _already EQUAL -1)
    return()
  endif ()
  string(FIND "${_content}" "${old}" _anchor)
  if (_anchor EQUAL -1)
    message(FATAL_ERROR "aurora: SDL3 patch '${description}' does not apply to ${file}; "
      "the vendored SDL version changed, review AuroraSDL3Patches.cmake")
  endif ()
  string(REPLACE "${old}" "${new}" _content "${_content}")
  file(WRITE "${file}" "${_content}")
  message(STATUS "aurora: SDL3 patch applied: ${description}")
endfunction()

function(aurora_sdl3_apply_patches sdl_source_dir)
  set(_wii "${sdl_source_dir}/src/joystick/hidapi/SDL_hidapi_wii.c")
  if (NOT EXISTS "${_wii}")
    message(FATAL_ERROR "aurora: SDL3 source tree at ${sdl_source_dir} has no SDL_hidapi_wii.c")
  endif ()
  set(_rumble "${sdl_source_dir}/src/joystick/hidapi/SDL_hidapi_rumble.c")
  if (NOT EXISTS "${_rumble}")
    message(FATAL_ERROR "aurora: SDL3 source tree at ${sdl_source_dir} has no SDL_hidapi_rumble.c")
  endif ()

  # Backport of SDL 71f4af732 (drop once SDL includes it): rumble writes must not hold dev_lock.
  _aurora_sdl3_replace("${_rumble}" "HIDAPI rumble writes no longer hold the device lock"
[==[        if (request) {
            SDL_LockMutex(request->device->dev_lock);
            if (request->device->dev) {
#ifdef DEBUG_RUMBLE
                HIDAPI_DumpPacket("Rumble packet: size = %d", request->data, request->size);
#endif
                SDL_hid_write(request->device->dev, request->data, request->size);
            }
            SDL_UnlockMutex(request->device->dev_lock);
]==]
[==[        if (request) {
            if (request->device->dev) {
#ifdef DEBUG_RUMBLE
                HIDAPI_DumpPacket("Rumble packet: size = %d", request->data, request->size);
#endif
                SDL_hid_write(request->device->dev, request->data, request->size);
            }
]==])

  # Wii Remote: rebuild the joystick in place after an extension change.
  #
  # When a Nunchuk or Classic Controller is plugged in or pulled out, the driver
  # flags the joystick as disconnected so it can come back with the new name and
  # capabilities. But the HID device stays open, and HIDAPI only removes a
  # joystick-less device once its handle is closed, so nothing ever re-creates
  # the joystick: the remote is gone for good until the application toggles the
  # driver hint (which closes and re-opens the Bluetooth HID handle, something
  # some Windows Bluetooth stacks answer by dropping the link). Instead, when the
  # device has no joystick, probe the extension again (two bounded attempts, at
  # most once a second) and connect a new joystick on the still-open handle.
  _aurora_sdl3_replace("${_wii}" "Wii Remote in-place reconnect (context field)"
[==[    Uint64 m_ulNextMotionPlusCheck;
    bool m_bDisconnected;
]==]
[==[    Uint64 m_ulNextMotionPlusCheck;
    bool m_bDisconnected;
    Uint64 m_ulNextReconnect; /* WiiCompiled: see HIDAPI_DriverWii_UpdateDevice */
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote in-place reconnect (bounded extension probe)"
[==[static EWiiExtensionControllerType ReadExtensionControllerType(SDL_HIDAPI_Device *device)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;
    EWiiExtensionControllerType eExtensionControllerType = k_eWiiExtensionControllerType_Unknown;
    const int MAX_ATTEMPTS = 20;
    int attempts = 0;
]==]
[==[static EWiiExtensionControllerType ReadExtensionControllerTypeAttempts(SDL_HIDAPI_Device *device, int MAX_ATTEMPTS)
{
    SDL_DriverWii_Context *ctx = (SDL_DriverWii_Context *)device->context;
    EWiiExtensionControllerType eExtensionControllerType = k_eWiiExtensionControllerType_Unknown;
    int attempts = 0;
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote in-place reconnect (probe wrapper)"
[==[static void UpdateDeviceIdentity(SDL_HIDAPI_Device *device)
{
]==]
[==[static EWiiExtensionControllerType ReadExtensionControllerType(SDL_HIDAPI_Device *device)
{
    return ReadExtensionControllerTypeAttempts(device, 20);
}

static void UpdateDeviceIdentity(SDL_HIDAPI_Device *device)
{
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote in-place reconnect (UpdateDevice)"
[==[    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    } else {
        return false;
    }

    now = SDL_GetTicks();
]==]
[==[    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    } else {
        /* WiiCompiled: the joystick was dropped (extension plugged or unplugged,
         * or a failed read) but the HID device is still open, and the device
         * list only removes a joystick-less device once its handle is closed.
         * Rebuild the joystick in place, so an extension change behaves like it
         * does on the console: the remote never goes away, it just changes
         * type. If the remote does not answer, keep the device and retry. */
        now = SDL_GetTicks();
        if (ctx->m_ulNextReconnect != 0 && now < ctx->m_ulNextReconnect) {
            return false;
        }
        ctx->m_ulNextReconnect = now + 1000;
        {
            EWiiExtensionControllerType type = ReadExtensionControllerTypeAttempts(device, 2);
            if (type == k_eWiiExtensionControllerType_Unknown) {
                return false;
            }
            ctx->m_bDisconnected = false;
            ctx->m_ulLastInput = now;
            ctx->m_ulNextReconnect = 0;
            ctx->m_eExtensionControllerType = type;
            UpdateDeviceIdentity(device);
            SDL_LogDebug(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: Reconnected joystick as %s", device->name);
            return HIDAPI_JoystickConnected(device, NULL);
        }
    }

    now = SDL_GetTicks();
]==])
endfunction()

# Script mode (FetchContent PATCH_COMMAND).
if (CMAKE_SCRIPT_MODE_FILE AND DEFINED SDL_SOURCE_DIR)
  aurora_sdl3_apply_patches("${SDL_SOURCE_DIR}")
endif ()
