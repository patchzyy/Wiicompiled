#ifndef DOLPHIN_PAD_H
#define DOLPHIN_PAD_H

#include <dolphin/types.h>

#define PAD_SPEC_0 0
#define PAD_SPEC_1 1
#define PAD_SPEC_2 2
#define PAD_SPEC_3 3
#define PAD_SPEC_4 4
#define PAD_SPEC_5 5

#define PAD_CHAN0 0
#define PAD_CHAN1 1
#define PAD_CHAN2 2
#define PAD_CHAN3 3
#define PAD_CHANMAX 4

#define PAD_MOTOR_STOP 0
#define PAD_MOTOR_RUMBLE 1
#define PAD_MOTOR_STOP_HARD 2

#define PAD_ERR_NONE 0
#define PAD_ERR_NO_CONTROLLER -1
#define PAD_ERR_NOT_READY -2
#define PAD_ERR_TRANSFER -3

#define PAD_MAX_CONTROLLERS 4

#define PAD_BUTTON_LEFT 0x0001
#define PAD_BUTTON_RIGHT 0x0002
#define PAD_BUTTON_DOWN 0x0004
#define PAD_BUTTON_UP 0x0008
#define PAD_TRIGGER_Z 0x0010
#define PAD_TRIGGER_R 0x0020
#define PAD_TRIGGER_L 0x0040
#define PAD_BUTTON_A 0x0100
#define PAD_BUTTON_B 0x0200
#define PAD_BUTTON_X 0x0400
#define PAD_BUTTON_Y 0x0800
#define PAD_BUTTON_MENU 0x1000
#define PAD_BUTTON_START 0x1000
#ifdef TARGET_PC
#define PAD_BUTTON_BACK 0x0002000
#define PAD_BUTTON_GUIDE 0x0004000
#define PAD_BUTTON_MISC1 0x0008000
#define PAD_BUTTON_MISC2 0x0010000
#define PAD_BUTTON_MISC3 0x0020000
#define PAD_BUTTON_MISC4 0x0040000
#define PAD_BUTTON_MISC5 0x0080000
#define PAD_BUTTON_MISC6 0x0100000
#define PAD_BUTTON_RIGHT_PADDLE1 0x0200000
#define PAD_BUTTON_LEFT_PADDLE1 0x0400000
#define PAD_BUTTON_RIGHT_PADDLE2 0x0800000
#define PAD_BUTTON_LEFT_PADDLE2 0x1000000
#define PAD_BUTTON_RIGHT_STICK 0x2000000
#define PAD_BUTTON_LEFT_STICK 0x4000000
#define PAD_BUTTON_TOUCHPAD 0x8000000
#define PAD_EXT_BUTTON_COUNT 15
#endif

#define PAD_BUTTON_COUNT 12

// added by Aurora, not present in original SDK
#define PAD_AXIS_LEFT_X_POS 0
#define PAD_AXIS_LEFT_X_NEG 1
#define PAD_AXIS_LEFT_Y_POS 2
#define PAD_AXIS_LEFT_Y_NEG 3
#define PAD_AXIS_RIGHT_X_POS 4
#define PAD_AXIS_RIGHT_X_NEG 5
#define PAD_AXIS_RIGHT_Y_POS 6
#define PAD_AXIS_RIGHT_Y_NEG 7
#define PAD_AXIS_TRIGGER_L 8
#define PAD_AXIS_TRIGGER_R 9

#define PAD_AXIS_COUNT 10

#define PAD_CHAN0_BIT 0x80000000
#define PAD_CHAN1_BIT 0x40000000
#define PAD_CHAN2_BIT 0x20000000
#define PAD_CHAN3_BIT 0x10000000

#define PADButtonDown(buttonLast, button) (((buttonLast) ^ (button)) & (button))
#define PADButtonUp(buttonLast, button) (((buttonLast) ^ (button)) & (buttonLast))

#ifdef __cplusplus
extern "C" {
#endif

#define PAD_NATIVE_BUTTON_INVALID 0xFFFFFFFF

typedef struct PADStatus {
  u16 button;
  s8 stickX;
  s8 stickY;
  s8 substickX;
  s8 substickY;
  union {
    u8 triggerLeft;
    u8 triggerL;
  };
  union {
    u8 triggerRight;
    u8 triggerR;
  };
  u8 analogA;
  u8 analogB;
  s8 err;
#ifdef TARGET_PC
  u32 extButton;
#endif
} PADStatus;

typedef enum PADAxisSign {
  AXIS_SIGN_NEGATIVE = -1,
  AXIS_SIGN_POSITIVE = 1,
} PADAxisSign;

typedef struct PADSignedNativeAxis {
  int32_t nativeAxis;
  PADAxisSign sign;
} PADSignedNativeAxis;

struct SDL_Gamepad;

BOOL PADInit();
u32 PADRead(PADStatus* status);
BOOL PADReset(u32 mask);
BOOL PADRecalibrate(u32 mask);
void PADClamp(PADStatus* status);
void PADClampCircle(PADStatus* status);
void PADControlMotor(u32 chan, u32 cmd);
void PADSetSpec(u32 spec);
void PADControlAllMotors(const u32* cmdArr);
void PADSetAnalogMode(u32 mode);

#ifdef TARGET_PC
#define PAD_KEY_INVALID (-1)
#define PAD_KEY_MOUSE_LEFT (-2)
#define PAD_KEY_MOUSE_MIDDLE (-3)
#define PAD_KEY_MOUSE_RIGHT (-4)
#define PAD_KEY_MOUSE_X1 (-5)
#define PAD_KEY_MOUSE_X2 (-6)
typedef u16 PADButton;
typedef u32 PADExtButton;
typedef u16 PADAxis;

typedef struct PADKeyButtonBinding {
  s32 scancode;
  PADButton padButton;
} PADKeyButtonBinding;

typedef struct PADKeyAxisBinding {
  s32 scancode;
  PADAxis padAxis;
  s16 influence; // normalized percentage between 0 and 1
} PADKeyAxisBinding;

/* New API to facilitate controller interactions */
typedef struct PADDeadZones {
  bool emulateTriggers;
  bool useDeadzones;
  u16 stickDeadZone;
  u16 substickDeadZone;
  u16 leftTriggerActivationZone;
  u16 rightTriggerActivationZone;
} PADDeadZones;

typedef struct PADButtonMapping {
  u32 nativeButton;
  PADButton padButton;
} PADButtonMapping;

typedef struct PADAxisMapping {
  PADSignedNativeAxis nativeAxis;
  s32 nativeButton;
  PADAxis padAxis;
} PADAxisMapping;

typedef struct PADDefaultMapping {
  PADButtonMapping buttons[PAD_BUTTON_COUNT];
  PADAxisMapping axes[PAD_AXIS_COUNT];
} PADDefaultMapping;

/* Returns the total number of controllers */
u32 PADCount();
/* Returns the controller name for the given index into the controller map */
const char* PADGetNameForControllerIndex(u32 idx);
void PADSetPortForIndex(u32 index, u32 port);
s32 PADGetIndexForPort(u32 port);
void PADGetVidPid(u32 port, u32* vid, u32* pid);
void PADClearPort(u32 port);
const char* PADGetName(u32 port);
void PADSetButtonMapping(u32 port, PADButtonMapping mapping);
void PADSetAllButtonMappings(u32 port, PADButtonMapping buttons[PAD_BUTTON_COUNT]);
PADButtonMapping* PADGetButtonMappings(u32 port, u32* buttonCount);
void PADSetAltButtonMapping(u32 port, PADButtonMapping mapping);
PADButtonMapping* PADGetAltButtonMappings(u32 port, u32* buttonCount);
void PADSetAxisMapping(u32 port, PADAxisMapping mapping);
void PADSetAllAxisMappings(u32 port, PADAxisMapping axes[PAD_AXIS_COUNT]);
PADAxisMapping* PADGetAxisMappings(u32 port, u32* axisCount);
void PADSerializeMappings();

BOOL PADSetKeyButtonBinding(u32 port, PADKeyButtonBinding binding);
BOOL PADSetKeyButtonBindings(u32 port, PADKeyButtonBinding bindings[PAD_BUTTON_COUNT]);
PADKeyButtonBinding* PADGetKeyButtonBindings(u32 port, u32* buttonCount);
BOOL PADSetKeyAxisBinding(u32 port, PADKeyAxisBinding binding);
BOOL PADSetKeyAxisBindings(u32 port, PADKeyAxisBinding bindings[PAD_BUTTON_COUNT]);
PADKeyAxisBinding* PADGetKeyAxisBindings(u32 port, u32* axisCount);
void PADClearKeyBindings(u32 port);
void PADSetKeyboardActive(u32 port, BOOL active);

PADDeadZones* PADGetDeadZones(u32 port);
const char* PADGetButtonName(PADButton);
const char* PADGetNativeButtonName(u32 button);
const char* PADGetAxisName(PADAxis);
const char* PADGetAxisDirectionLabel(PADAxis);
const char* PADGetNativeAxisName(PADSignedNativeAxis axis);

BOOL PADIsGCAdapter(u32 port);

/**
 * Returns the SDL gamepad for the index into the controller map.
 */
struct SDL_Gamepad* PADGetSDLGamepadForIndex(u32 index);
/* Returns the first native button which is currently pressed */
s32 PADGetNativeButtonPressed(u32 port);
/* Returns the first native axis which is currently pulled halfway or more */
PADSignedNativeAxis PADGetNativeAxisPulled(u32 port);
void PADRestoreDefaultMapping(u32 port);
void PADBlockInput(bool block);

/**
 * Set the default controller mapping used.
 *
 * Must be called before PADInit.
 */

typedef enum {
  PAD_TYPE_UNKNOWN = 0,
  PAD_TYPE_STANDARD,
  PAD_TYPE_XBOX360,
  PAD_TYPE_XBOXONE,
  PAD_TYPE_PS3,
  PAD_TYPE_PS4,
  PAD_TYPE_PS5,
  PAD_TYPE_SWITCH_PROCON,
  PAD_TYPE_JOYCON_LEFT,
  PAD_TYPE_JOYCON_RIGHT,
  PAD_TYPE_JOYCON_PAIR,
  PAD_TYPE_GAMECUBE,
  PAD_TYPE_NSO_GAMECUBE,
} PADControllerType;

void PADSetDefaultMapping(const PADDefaultMapping* mapping, PADControllerType type);

BOOL PADSetColor(u32 port, u8 red, u8 green, u8 blue);
BOOL PADGetColor(u32 port, u8* red, u8* green, u8* blue);

typedef enum {
  PAD_SENSOR_INVALID = -1,
  PAD_SENSOR_UNKNOWN,
  PAD_SENSOR_ACCEL,
  PAD_SENSOR_GYRO,
  PAD_SENSOR_ACCEL_LEFT,
  PAD_SENSOR_GYRO_LEFT,
  PAD_SENSOR_ACCEL_RIGHT,
  PAD_SENSOR_GYRO_RIGHT,
} PADSensorType;
BOOL PADSetSensorEnabled(u32 port, PADSensorType sensor, BOOL enabled);

BOOL PADHasSensor(u32 port, PADSensorType sensor);

BOOL PADGetSensorData(u32 port, PADSensorType sensor, f32* data, int nValues);

BOOL PADSetRumbleIntensity(u32 port, u16 low, u16 high);
BOOL PADGetRumbleIntensity(u32 port, u16* low, u16* high);
BOOL PADSupportsRumbleIntensity(u32 port);

typedef enum PADBatteryState {
  PAD_BATTERYSTATE_ERROR = -1,
  PAD_BATTERYSTATE_UNKNOWN,
  PAD_BATTERYSTATE_ON_BATTERY,
  PAD_BATTERYSTATE_NO_BATTERY,
  PAD_BATTERYSTATE_CHARGING,
  PAD_BATTERYSTATE_CHARGED,
} PADBatteryState;

PADBatteryState PADGetBatteryState(u32 port, f32* perc);

PADControllerType PADGetControllerType(u32 port);
PADControllerType PADGetControllerTypeForIndex(u32 index);

#endif

#ifdef __cplusplus
}
#endif

#endif
