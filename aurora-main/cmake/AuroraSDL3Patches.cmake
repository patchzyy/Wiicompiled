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
  set(_hidapi "${sdl_source_dir}/src/joystick/hidapi/SDL_hidapijoystick.c")
  if (NOT EXISTS "${_hidapi}")
    message(FATAL_ERROR "aurora: SDL3 source tree at ${sdl_source_dir} has no SDL_hidapijoystick.c")
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

  # Without the lock, closing the handle must wait for in-flight rumble writes.
  _aurora_sdl3_replace("${_hidapi}" "HIDAPI device close waits for pending rumble"
[==[    device->driver->FreeDevice(device);
    device->driver = NULL;

    SDL_LockMutex(device->dev_lock);
]==]
[==[    device->driver->FreeDevice(device);
    device->driver = NULL;

    while (SDL_GetAtomicInt(&device->rumble_pending) > 0) {
        SDL_Delay(10);
    }

    SDL_LockMutex(device->dev_lock);
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

  # Classic Controller sticks: the stock path stretches each half-range by learned min/max;
  # this posts WPAD's linear value (raw - first-report centre) * unit, times 64.
  _aurora_sdl3_replace("${_wii}" "Wii Classic Controller linear sticks (helper)"
[==[static void HandleGamepadControllerButtonData(SDL_DriverWii_Context *ctx, SDL_Joystick *joystick, const WiiButtonData *data)
{
]==]
[==[/* WiiCompiled: axis = (raw - centre) * unit * 64, y inverted; see AuroraSDL3Patches.cmake */
static void PostClassicStickLinear(Uint64 timestamp, SDL_Joystick *joystick, StickCalibrationData *calibration, Uint8 axis, Uint16 data, int unit)
{
    int value;

    if (!calibration->center) {
        // Center on first read
        calibration->center = data;
        return;
    }
    value = ((int)data - (int)calibration->center) * unit * 64;
    if (axis == SDL_GAMEPAD_AXIS_LEFTY || axis == SDL_GAMEPAD_AXIS_RIGHTY) {
        value = -value;
    }
    SDL_SendJoystickAxis(timestamp, joystick, axis, (Sint16)SDL_clamp(value, SDL_JOYSTICK_AXIS_MIN, SDL_JOYSTICK_AXIS_MAX));
}

static void HandleGamepadControllerButtonData(SDL_DriverWii_Context *ctx, SDL_Joystick *joystick, const WiiButtonData *data)
{
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Classic Controller linear sticks (axes)"
[==[    PostStickCalibrated(ctx->timestamp, joystick, &ctx->m_StickCalibrationData[0], SDL_GAMEPAD_AXIS_LEFTX, lx);
    PostStickCalibrated(ctx->timestamp, joystick, &ctx->m_StickCalibrationData[1], SDL_GAMEPAD_AXIS_LEFTY, ly);
    PostStickCalibrated(ctx->timestamp, joystick, &ctx->m_StickCalibrationData[2], SDL_GAMEPAD_AXIS_RIGHTX, rx);
    PostStickCalibrated(ctx->timestamp, joystick, &ctx->m_StickCalibrationData[3], SDL_GAMEPAD_AXIS_RIGHTY, ry);
]==]
[==[    PostClassicStickLinear(ctx->timestamp, joystick, &ctx->m_StickCalibrationData[0], SDL_GAMEPAD_AXIS_LEFTX, lx, 16);
    PostClassicStickLinear(ctx->timestamp, joystick, &ctx->m_StickCalibrationData[1], SDL_GAMEPAD_AXIS_LEFTY, ly, 16);
    PostClassicStickLinear(ctx->timestamp, joystick, &ctx->m_StickCalibrationData[2], SDL_GAMEPAD_AXIS_RIGHTX, rx, 32);
    PostClassicStickLinear(ctx->timestamp, joystick, &ctx->m_StickCalibrationData[3], SDL_GAMEPAD_AXIS_RIGHTY, ry, 32);
]==])

  # Wii Remote: decode the accelerometer with the factory calibration from the EEPROM.
  _aurora_sdl3_replace("${_wii}" "Wii Remote accelerometer calibration (context fields)"
[==[    StickCalibrationData m_StickCalibrationData[6];
} SDL_DriverWii_Context;
]==]
[==[    StickCalibrationData m_StickCalibrationData[6];

    float m_fAccelZeroG[3];
    float m_fAccelPerG[3];
} SDL_DriverWii_Context;
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote accelerometer calibration (memory space parameter)"
[==[static bool ReadRegister(SDL_DriverWii_Context *ctx, Uint32 address, int size, bool sync)
{
    Uint8 readRequest[7];

    readRequest[0] = k_eWiiOutputReportIDs_ReadMemory;
    readRequest[1] = (Uint8)(0x04 | (Uint8)ctx->m_bRumbleActive);
]==]
[==[static bool ReadMemory(SDL_DriverWii_Context *ctx, Uint8 addressSpace, Uint32 address, int size, bool sync);

static bool ReadRegister(SDL_DriverWii_Context *ctx, Uint32 address, int size, bool sync)
{
    return ReadMemory(ctx, 0x04, address, size, sync);
}

static bool ReadMemory(SDL_DriverWii_Context *ctx, Uint8 addressSpace, Uint32 address, int size, bool sync)
{
    Uint8 readRequest[7];

    readRequest[0] = k_eWiiOutputReportIDs_ReadMemory;
    readRequest[1] = (Uint8)(addressSpace | (Uint8)ctx->m_bRumbleActive);
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote accelerometer calibration (EEPROM read)"
[==[static bool HIDAPI_DriverWii_InitDevice(SDL_HIDAPI_Device *device)
{
]==]
[==[// Nominal decode: zero-g at 0x200, 100 units per g
#define WII_ACCEL_NOMINAL_ZERO_G  512.0f
#define WII_ACCEL_NOMINAL_PER_G   100.0f

// EEPROM calibration at 0x16, copy at 0x20: zero-g XYZ, 1 g XYZ, LSBs of both, checksum
#define WII_ACCEL_CALIBRATION_SIZE 10
#define WII_ADDRESS_SPACE_EEPROM   0x00

static bool ReadAccelCalibrationBlock(SDL_DriverWii_Context *ctx, Uint8 address, Uint8 *block)
{
    if (!ReadMemory(ctx, WII_ADDRESS_SPACE_EEPROM, address, WII_ACCEL_CALIBRATION_SIZE, true)) {
        return false;
    }
    if (ctx->m_rgucReadBuffer[5] != address ||
        ctx->m_rgucReadBuffer[3] != ((WII_ACCEL_CALIBRATION_SIZE - 1) << 4)) {
        SDL_SetError("Unexpected EEPROM read response");
        return false;
    }
    SDL_memcpy(block, ctx->m_rgucReadBuffer + 6, WII_ACCEL_CALIBRATION_SIZE);
    return true;
}

static bool ParseAccelCalibration(SDL_DriverWii_Context *ctx, const Uint8 *block)
{
    int zero[3];
    int perG[3];
    Uint8 checksum = 0x55;
    int i;

    for (i = 0; i < WII_ACCEL_CALIBRATION_SIZE - 1; ++i) {
        checksum = (Uint8)(checksum + block[i]);
    }
    if (checksum != block[WII_ACCEL_CALIBRATION_SIZE - 1]) {
        SDL_SetError("Bad checksum");
        return false;
    }

    for (i = 0; i < 3; ++i) {
        int shift = 4 - 2 * i;
        int oneG = (block[4 + i] << 2) | ((block[7] >> shift) & 0x03);

        zero[i] = (block[i] << 2) | ((block[3] >> shift) & 0x03);
        perG[i] = oneG - zero[i];

        // Sanity limits around the nominal values
        if (zero[i] < 384 || zero[i] > 640 || perG[i] < 60 || perG[i] > 160) {
            SDL_SetError("Implausible values");
            return false;
        }
    }

    for (i = 0; i < 3; ++i) {
        ctx->m_fAccelZeroG[i] = (float)zero[i];
        ctx->m_fAccelPerG[i] = (float)perG[i];
    }
    return true;
}

static void ReadAccelCalibration(SDL_DriverWii_Context *ctx)
{
    static const Uint8 addresses[] = { 0x16, 0x20 };
    Uint8 block[WII_ACCEL_CALIBRATION_SIZE];
    int i;

    for (i = 0; i < SDL_arraysize(addresses); ++i) {
        if (ReadAccelCalibrationBlock(ctx, addresses[i], block) &&
            ParseAccelCalibration(ctx, block)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: Accelerometer calibration from EEPROM 0x%02X: zero %d/%d/%d, %d/%d/%d units per g",
                        addresses[i],
                        (int)ctx->m_fAccelZeroG[0], (int)ctx->m_fAccelZeroG[1], (int)ctx->m_fAccelZeroG[2],
                        (int)ctx->m_fAccelPerG[0], (int)ctx->m_fAccelPerG[1], (int)ctx->m_fAccelPerG[2]);
            return;
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: Accelerometer calibration at EEPROM 0x%02X unusable: %s", addresses[i], SDL_GetError());
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_INPUT, "HIDAPI Wii: Using nominal accelerometer calibration");
}

static bool HIDAPI_DriverWii_InitDevice(SDL_HIDAPI_Device *device)
{
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote accelerometer calibration (nominal defaults)"
[==[    ctx->device = device;
    device->context = ctx;
]==]
[==[    ctx->device = device;
    device->context = ctx;
    for (int i = 0; i < 3; ++i) {
        ctx->m_fAccelZeroG[i] = WII_ACCEL_NOMINAL_ZERO_G;
        ctx->m_fAccelPerG[i] = WII_ACCEL_NOMINAL_PER_G;
    }
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote accelerometer calibration (read at init)"
[==[        ctx->m_eExtensionControllerType = ReadExtensionControllerType(device);
]==]
[==[        ctx->m_eExtensionControllerType = ReadExtensionControllerType(device);
        ReadAccelCalibration(ctx);
]==])
  # Reports without accelerometer bytes (status, memory) decoded as garbage.
  _aurora_sdl3_replace("${_wii}" "Wii Remote accelerometer guard"
[==[    }

    x = ((data->rgucAccelerometer[0] << 2) | ((data->rgucBaseButtons[0] >> 5) & 0x03))]==]
[==[    }

    if (!data->hasAccelerometer) {
        return;
    }

    x = ((data->rgucAccelerometer[0] << 2) | ((data->rgucBaseButtons[0] >> 5) & 0x03))]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote accelerometer calibration (nominal constant)"
[==[const WiiButtonData *data)
{
    const float ACCEL_RES_PER_G = 100.0f;
    Sint16 x, y, z;
]==]
[==[const WiiButtonData *data)
{
    Sint16 x, y, z;
]==])
  _aurora_sdl3_replace("${_wii}" "Wii Remote accelerometer calibration (decode)"
[==[    x = ((data->rgucAccelerometer[0] << 2) | ((data->rgucBaseButtons[0] >> 5) & 0x03)) - 0x200;
    y = ((data->rgucAccelerometer[1] << 2) | ((data->rgucBaseButtons[1] >> 4) & 0x02)) - 0x200;
    z = ((data->rgucAccelerometer[2] << 2) | ((data->rgucBaseButtons[1] >> 5) & 0x02)) - 0x200;

    values[0] = -((float)x / ACCEL_RES_PER_G) * SDL_STANDARD_GRAVITY;
    values[1] = ((float)z / ACCEL_RES_PER_G) * SDL_STANDARD_GRAVITY;
    values[2] = ((float)y / ACCEL_RES_PER_G) * SDL_STANDARD_GRAVITY;
]==]
[==[    x = ((data->rgucAccelerometer[0] << 2) | ((data->rgucBaseButtons[0] >> 5) & 0x03));
    y = ((data->rgucAccelerometer[1] << 2) | ((data->rgucBaseButtons[1] >> 4) & 0x02));
    z = ((data->rgucAccelerometer[2] << 2) | ((data->rgucBaseButtons[1] >> 5) & 0x02));

    values[0] = -(((float)x - ctx->m_fAccelZeroG[0]) / ctx->m_fAccelPerG[0]) * SDL_STANDARD_GRAVITY;
    values[1] = (((float)z - ctx->m_fAccelZeroG[2]) / ctx->m_fAccelPerG[2]) * SDL_STANDARD_GRAVITY;
    values[2] = (((float)y - ctx->m_fAccelZeroG[1]) / ctx->m_fAccelPerG[1]) * SDL_STANDARD_GRAVITY;
]==])
endfunction()

# Script mode (FetchContent PATCH_COMMAND).
if (CMAKE_SCRIPT_MODE_FILE AND DEFINED SDL_SOURCE_DIR)
  aurora_sdl3_apply_patches("${SDL_SOURCE_DIR}")
endif ()
