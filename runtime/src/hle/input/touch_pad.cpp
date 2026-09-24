#include "touch_pad.h"
#include "touch_art.h"
#include "ios_motion_input.h"
#include "ios_motion_steering.h"
#include "runtime_config.h"
#include "wii_remote_input.h"

#include "hle/controller_status_contract.h"
#include "settings_overlay.h"

#include <dolphin/pad.h>
#include <imgui.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_touch.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>

namespace TouchPad {
namespace {

bool s_autoAccelerateEnabled = true;
bool s_autoAccelerateLoaded = false;
bool s_gasLocked = false;
bool s_aPhysicallyPressedLast = false;
bool s_unlockingTap = false;
uint64_t s_aPressStartTimeMs = 0;

uint64_t CurrentTimeMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}

void EnsureAutoAccelerateConfigLoaded() {
    if (!s_autoAccelerateLoaded) {
        s_autoAccelerateEnabled = RuntimeConfigFile::TouchAutoAccelerate(true);
        s_autoAccelerateLoaded = true;
    }
}


struct Circle {
    float x, y, r;
    bool Contains(float px, float py, float aspect) const {
        const float dx = (px - x) * aspect;
        const float dy = py - y;
        return (dx * dx + dy * dy) <= (r * r);
    }
};

// Positions are held as a distance from one screen edge in units of screen
// height. A fraction of width would land somewhere different on a 1.45:1 iPad
// and a 2.17:1 iPhone.
struct Anchor {
    enum Edge { Left, Right, Centre };
    Edge edge;
    float dx;   // distance from that edge, in units of screen height
    float y;    // fraction of screen height
    float r;    // radius, in units of screen height

    Circle Resolve(float aspect) const {
        const float offset = dx / aspect;
        switch (edge) {
        case Left:   return Circle{offset, y, r};
        case Right:  return Circle{1.0f - offset, y, r};
        case Centre: return Circle{0.5f + offset, y, r};
        }
        return Circle{0.5f, y, r};
    }
};

constexpr Anchor kStickA{Anchor::Left, 0.215f, 0.400f, 0.130f};
constexpr Anchor kShoulderLA{Anchor::Left, 0.130f, 0.090f, 0.070f};
constexpr Anchor kShoulderRA{Anchor::Right, 0.130f, 0.090f, 0.070f};
constexpr Anchor kButtonAA{Anchor::Right, 0.210f, 0.640f, 0.105f};
constexpr Anchor kButtonBA{Anchor::Right, 0.430f, 0.790f, 0.072f};
constexpr Anchor kButtonItemA{Anchor::Right, 0.165f, 0.310f, 0.070f};
constexpr Anchor kButtonStartA{Anchor::Centre, 0.100f, 0.070f, 0.042f};
// There is no F10 on a touch device, so this is the only way into the settings
// bar. It is drawn whenever a touch screen exists - including when a pad has
// hidden the game controls - or connecting a pad would lock the user out.
constexpr Anchor kButtonMenuA{Anchor::Centre, -0.100f, 0.070f, 0.040f};

// r is the half-extent of the whole cross, not of one arm.
constexpr Anchor kDpadA{Anchor::Left, 0.215f, 0.760f, 0.125f};
constexpr float kDpadDeadzone = 0.30f;

struct Layout {
    Circle stick, shoulderL, shoulderR, a, b, item, start, menu;
    Circle dpad;  // the whole cross; direction is derived, not four zones
};

Layout LayoutFor(float aspect) {
    Layout l{};
    l.stick = kStickA.Resolve(aspect);
    l.shoulderL = kShoulderLA.Resolve(aspect);
    l.shoulderR = kShoulderRA.Resolve(aspect);
    l.a = kButtonAA.Resolve(aspect);
    l.b = kButtonBA.Resolve(aspect);
    l.item = kButtonItemA.Resolve(aspect);
    l.start = kButtonStartA.Resolve(aspect);
    l.menu = kButtonMenuA.Resolve(aspect);

    l.dpad = kDpadA.Resolve(aspect);
    return l;
}

struct Frame {
    bool stickHeld = false;
    float stickDx = 0.0f;  // -1..1 after deadzone and clamping
    float stickDy = 0.0f;
    bool a = false, b = false, item = false, start = false;
    bool l = false, r = false;
    bool up = false, down = false, left = false, right = false;
};

// A finger inside the stick circle steers; its offset from the circle centre is
// the analog deflection. Every other finger is hit-tested against the buttons,
// so any number of them can be held at once.
// Owning finger, or 0. Single-threaded: guest fibers and the event pump share
// the host main thread.
SDL_FingerID g_stickFinger = 0;

bool g_motionCentred = false;
double g_motionCentreAngle = 0.0;
bool g_shakeArmed = false;
double g_lastShakeTimestamp = -1.0;
uint8_t g_gestureFrames = 0;

Frame SampleFingers(float aspect) {
    const Layout L = LayoutFor(aspect);
    Frame frame;
    bool stickFingerSeen = false;
    int deviceCount = 0;
    SDL_TouchID* devices = SDL_GetTouchDevices(&deviceCount);
    if (devices == nullptr) {
        return frame;
    }

    for (int d = 0; d < deviceCount; ++d) {
        int fingerCount = 0;
        SDL_Finger** fingers = SDL_GetTouchFingers(devices[d], &fingerCount);
        if (fingers == nullptr) {
            continue;
        }
        for (int f = 0; f < fingerCount; ++f) {
            const float px = fingers[f]->x;
            const float py = fingers[f]->y;

            // Holding the grab lets the thumb slide past the gate without
            // dropping steering.
            const bool ownsStick = (g_stickFinger != 0 && fingers[f]->id == g_stickFinger);
            if (ownsStick || (!frame.stickHeld && g_stickFinger == 0 &&
                              L.stick.Contains(px, py, aspect))) {
                g_stickFinger = fingers[f]->id;
                stickFingerSeen = true;
                frame.stickHeld = true;
                const float dx = (px - L.stick.x) * aspect / L.stick.r;
                const float dy = (py - L.stick.y) / L.stick.r;
                const float len = std::sqrt(dx * dx + dy * dy);
                // Clamp to the circle so a finger dragged outside still reads as
                // full deflection rather than overshooting the guest's range.
                const float scale = (len > 1.0f) ? (1.0f / len) : 1.0f;
                frame.stickDx = dx * scale;
                frame.stickDy = dy * scale;
                continue;
            }
            if (L.a.Contains(px, py, aspect))     frame.a = true;
            if (L.b.Contains(px, py, aspect))     frame.b = true;
            if (L.item.Contains(px, py, aspect))  frame.item = true;
            if (L.start.Contains(px, py, aspect)) frame.start = true;
            if (L.shoulderL.Contains(px, py, aspect))   frame.l = true;
            if (L.shoulderR.Contains(px, py, aspect))   frame.r = true;
            {
                const float ddx = (px - L.dpad.x) * aspect / L.dpad.r;
                const float ddy = (py - L.dpad.y) / L.dpad.r;
                if (std::fabs(ddx) <= 1.0f && std::fabs(ddy) <= 1.0f &&
                    (ddx * ddx + ddy * ddy) > (kDpadDeadzone * kDpadDeadzone)) {
                    if (std::fabs(ddx) > std::fabs(ddy)) {
                        if (ddx < 0.0f) frame.left = true; else frame.right = true;
                    } else {
                        if (ddy < 0.0f) frame.up = true; else frame.down = true;
                    }
                }
            }
        }
        SDL_free(fingers);
    }
    SDL_free(devices);
    if (!stickFingerSeen) {
        g_stickFinger = 0;
    }

    return frame;
}

float CurrentAspect() {
    const ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.y <= 0.0f) {
        return 1.0f;
    }
    return io.DisplaySize.x / io.DisplaySize.y;
}

// GameCube's own button colours. White-on-white is unreadable over bright track
// surfaces, so every control also gets a dark halo and a dark label shadow,
// which keeps it legible on light and dark backgrounds alike.
constexpr ImU32 kColA     = IM_COL32(67, 176, 42, 255);   // green
constexpr ImU32 kColB     = IM_COL32(228, 0, 43, 255);    // red
constexpr ImU32 kColZ     = IM_COL32(104, 82, 214, 255);  // purple
constexpr ImU32 kColGrey  = IM_COL32(196, 198, 206, 255);
constexpr ImU32 kColStick = IM_COL32(174, 176, 184, 255);

ImU32 WithAlpha(ImU32 colour, int alpha) {
    return (colour & 0x00FFFFFFu) | (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT);
}

void DrawCircle(ImDrawList* list, const Circle& c, const ImVec2& size, const char* label,
                bool pressed, ImU32 colour, const char* art, bool locked = false) {
    const ImVec2 centre{c.x * size.x, c.y * size.y};
    const float radius = c.r * size.y;
    const ImVec2 lo{centre.x - radius, centre.y - radius};
    const ImVec2 hi{centre.x + radius, centre.y + radius};

    // The solid silhouette tinted black darkens the glyph's own shape; a plain
    // circle behind it would put a disc around the cross and the triggers.
    if (art != nullptr) {
        const ImTextureID fill = TouchArt::Get((std::string(art) + "_fill").c_str());
        if (fill != ImTextureID{}) {
            const ImU32 fillColor = locked
                ? IM_COL32(0, 195, 230, pressed ? 210 : 160)
                : IM_COL32(0, 0, 0, pressed ? 165 : 120);
            list->AddImage(fill, lo, hi, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), fillColor);
        }
        const ImTextureID line = TouchArt::Get(art);
        if (line != ImTextureID{}) {
            const ImU32 lineColor = locked
                ? IM_COL32(230, 255, 255, 255)
                : IM_COL32(255, 255, 255, pressed ? 250 : 175);
            list->AddImage(line, lo, hi, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), lineColor);
            if (locked) {
                // Outer cyan glow ring indicating latched acceleration
                list->AddCircle(centre, radius + 2.5f, IM_COL32(0, 240, 255, 240), 48, 3.0f);
            }
            return;
        }
    }

    // Vector fallback, used when the artwork is missing.
    const ImU32 baseColor = locked ? IM_COL32(0, 210, 240, 255) : colour;
    list->AddCircleFilled(centre, radius + 3.0f,
                          locked ? IM_COL32(0, 240, 255, 120) : IM_COL32(0, 0, 0, pressed ? 150 : 110), 48);
    list->AddCircleFilled(centre, radius, WithAlpha(baseColor, pressed ? 235 : 130), 48);
    list->AddCircle(centre, radius,
                    locked ? IM_COL32(255, 255, 255, 255) : WithAlpha(IM_COL32(255, 255, 255, 255), pressed ? 255 : 170),
                    48, locked ? 3.0f : 2.5f);
    if (locked) {
        list->AddCircle(centre, radius + 3.0f, IM_COL32(0, 240, 255, 230), 48, 2.5f);
    }
    if (label != nullptr && *label != '\0') {
        const ImVec2 text = ImGui::CalcTextSize(label);
        const ImVec2 at{centre.x - text.x * 0.5f, centre.y - text.y * 0.5f};
        list->AddText(ImVec2{at.x + 1.0f, at.y + 1.0f}, IM_COL32(0, 0, 0, 190), label);
        list->AddText(at, IM_COL32(255, 255, 255, 240), label);
    }
}

} // namespace

bool HasTouchScreen() {
    int deviceCount = 0;
    SDL_TouchID* devices = SDL_GetTouchDevices(&deviceCount);
    if (devices != nullptr) {
        SDL_free(devices);
    }
    return deviceCount > 0;
}

// Edge-triggered: the settings bar must toggle once per tap, not once per frame
// for as long as a finger rests on the button.
void PollMenuButton() {
    static bool wasDown = false;
    bool down = false;
    const float aspect = CurrentAspect();
    const Layout L = LayoutFor(aspect);
    int deviceCount = 0;
    SDL_TouchID* devices = SDL_GetTouchDevices(&deviceCount);
    if (devices != nullptr) {
        for (int d = 0; d < deviceCount && !down; ++d) {
            int fingerCount = 0;
            SDL_Finger** fingers = SDL_GetTouchFingers(devices[d], &fingerCount);
            if (fingers == nullptr) {
                continue;
            }
            for (int f = 0; f < fingerCount; ++f) {
                if (L.menu.Contains(fingers[f]->x, fingers[f]->y, aspect)) {
                    down = true;
                    break;
                }
            }
            SDL_free(fingers);
        }
        SDL_free(devices);
    }
    if (down && !wasDown) {
        settings_overlay::ToggleTopBar();
    }
    wasDown = down;
}

// Tapping the FPS readout opens the settings bar. Preferred over a dedicated
// button because it reuses something already on screen.
bool PollFpsTap(const ImVec2& size) {
    float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
    if (!settings_overlay::FpsOverlayBounds(&minX, &minY, &maxX, &maxY)) {
        return false;  // readout hidden - caller falls back to its own button
    }

    const bool controlsActive = IsActive();
    const float aspect = CurrentAspect();
    const Layout L = LayoutFor(aspect);

    static bool wasDown = false;
    bool down = false;
    int deviceCount = 0;
    SDL_TouchID* devices = SDL_GetTouchDevices(&deviceCount);
    if (devices != nullptr) {
        for (int d = 0; d < deviceCount && !down; ++d) {
            int fingerCount = 0;
            SDL_Finger** fingers = SDL_GetTouchFingers(devices[d], &fingerCount);
            if (fingers == nullptr) {
                continue;
            }
            for (int f = 0; f < fingerCount; ++f) {
                const float normX = fingers[f]->x;
                const float normY = fingers[f]->y;
                if (controlsActive) {
                    if (L.stick.Contains(normX, normY, aspect) ||
                        L.shoulderL.Contains(normX, normY, aspect) ||
                        L.shoulderR.Contains(normX, normY, aspect) ||
                        L.a.Contains(normX, normY, aspect) ||
                        L.b.Contains(normX, normY, aspect) ||
                        L.item.Contains(normX, normY, aspect) ||
                        L.start.Contains(normX, normY, aspect) ||
                        L.menu.Contains(normX, normY, aspect) ||
                        L.dpad.Contains(normX, normY, aspect)) {
                        continue;
                    }
                }
                const float px = normX * size.x;
                const float py = normY * size.y;
                if (px >= minX && px <= maxX && py >= minY && py <= maxY) {
                    down = true;
                    break;
                }
            }
            SDL_free(fingers);
        }
        SDL_free(devices);
    }
    if (down && !wasDown) {
        settings_overlay::ToggleTopBar();
    }
    wasDown = down;
    return true;
}

bool IsActive() {

    // A connected pad wins: PAD__Read_HLE already prefers it for input, and the
    // overlay should get out of the way visually too rather than sit on top of
    // the game doing nothing.
    int gamepadCount = 0;
    SDL_JoystickID* gamepads = SDL_GetGamepads(&gamepadCount);
    if (gamepads != nullptr) {
        SDL_free(gamepads);
    }
    if (gamepadCount > 0) {
        Reset();
        return false;
    }

    int deviceCount = 0;
    SDL_TouchID* devices = SDL_GetTouchDevices(&deviceCount);
    if (devices != nullptr) {
        SDL_free(devices);
    }
    return deviceCount > 0;
}

bool AutoAccelerateEnabled() {
    EnsureAutoAccelerateConfigLoaded();
    return s_autoAccelerateEnabled;
}

void SetAutoAccelerate(bool enabled) {
    EnsureAutoAccelerateConfigLoaded();
    s_autoAccelerateEnabled = enabled;
    RuntimeConfigFile::SetTouchAutoAccelerate(enabled);
    if (!enabled && s_gasLocked) {
        s_gasLocked = false;
        s_unlockingTap = false;
    }
}

bool IsGasLocked() {
    return s_gasLocked;
}

void Reset() {
    g_stickFinger = 0;
    s_gasLocked = false;
    s_unlockingTap = false;
    s_aPhysicallyPressedLast = false;
    s_aPressStartTimeMs = 0;
}

void UpdateAutoAccelerate(bool aPhysicalDown, uint64_t nowMs, bool autoAccelerateEnabled) {
    if (!autoAccelerateEnabled) {
        s_gasLocked = false;
        s_unlockingTap = false;
        s_aPhysicallyPressedLast = aPhysicalDown;
        return;
    }

    if (aPhysicalDown) {
        if (!s_aPhysicallyPressedLast) {
            // New touch down on button A
            if (s_gasLocked) {
                // Tapping while locked immediately unlocks
                s_gasLocked = false;
                s_unlockingTap = true;
            } else {
                s_unlockingTap = false;
                s_aPressStartTimeMs = nowMs;
            }
        } else {
            // Continuous hold on button A
            if (!s_gasLocked && !s_unlockingTap) {
                if (nowMs >= s_aPressStartTimeMs && (nowMs - s_aPressStartTimeMs) >= 1000) {
                    s_gasLocked = true;
                }
            }
        }
    } else {
        // Finger lifted
        s_unlockingTap = false;
    }

    s_aPhysicallyPressedLast = aPhysicalDown;
}

bool Read(std::array<PADStatus, 4>& statuses) {
    if (!IsActive()) {
        return false;
    }
    const Frame frame = SampleFingers(CurrentAspect());
    UpdateAutoAccelerate(frame.a, CurrentTimeMs(), AutoAccelerateEnabled());

    PADStatus& pad = statuses[0];
    pad = PADStatus{};
    pad.err = PAD_ERR_NONE;

    const bool aActive = frame.a || s_gasLocked;

    uint16_t buttons = 0;
    if (aActive)      buttons |= PAD_BUTTON_A;
    if (frame.b)      buttons |= PAD_BUTTON_B;
    if (frame.item)   buttons |= PAD_TRIGGER_Z;
    if (frame.start)  buttons |= PAD_BUTTON_START;
    if (frame.l)      buttons |= PAD_TRIGGER_L;
    if (frame.r)      buttons |= PAD_TRIGGER_R;
    if (frame.up)     buttons |= PAD_BUTTON_UP;
    if (frame.down)   buttons |= PAD_BUTTON_DOWN;
    if (frame.left)   buttons |= PAD_BUTTON_LEFT;
    if (frame.right)  buttons |= PAD_BUTTON_RIGHT;
    pad.button = buttons;

    // Screen y grows downward, the guest stick grows upward.
    pad.stickX = static_cast<int8_t>(std::clamp(frame.stickDx * 127.0f, -127.0f, 127.0f));
    pad.stickY = static_cast<int8_t>(std::clamp(-frame.stickDy * 127.0f, -127.0f, 127.0f));
    pad.triggerL = frame.l ? 255 : 0;
    pad.triggerR = frame.r ? 255 : 0;
#ifdef MKW_PLATFORM_IOS
    if (MotionGameCubeActive()) {
        IosMotionInput::Sample motion;
        if (IosMotionInput::Read(motion)) {
            float gravityX = 0.0f, gravityY = 0.0f;
            IosMotionSteering::RotateForLandscape(IosMotionInput::CurrentLandscapeOrientation(), motion.gravity[0],
                                                   motion.gravity[1], gravityX, gravityY);
            double angle = 0.0;
            if (IosMotionSteering::AngleFromGravity(gravityX, gravityY, angle)) {
                if (!g_motionCentred) {
                    g_motionCentreAngle = angle;
                    g_motionCentred = true;
                }
                if (!frame.stickHeld) {
                    const float steering = IosMotionSteering::SteeringValue(
                        angle, g_motionCentreAngle, RuntimeConfigFile::IosMotionSensitivity(),
                        RuntimeConfigFile::IosMotionInverted());
                    pad.stickX = static_cast<int8_t>(std::clamp(steering * 127.0f, -127.0f, 127.0f));
                }
            }
            const double magnitude = std::sqrt(motion.userAcceleration[0] * motion.userAcceleration[0] +
                                               motion.userAcceleration[1] * motion.userAcceleration[1] +
                                               motion.userAcceleration[2] * motion.userAcceleration[2]);
            const auto shake = IosMotionSteering::ShakeActionForSample(
                magnitude, motion.timestamp, g_shakeArmed, g_lastShakeTimestamp);
            if (shake == IosMotionSteering::ShakeAction::Rearm) {
                g_shakeArmed = true;
            } else if (shake == IosMotionSteering::ShakeAction::Trigger) {
                g_shakeArmed = false;
                g_lastShakeTimestamp = motion.timestamp;
                g_gestureFrames = 3;
            }
        }
        if (g_gestureFrames != 0) {
            pad.button |= PAD_BUTTON_UP;
            --g_gestureFrames;
        }
    }
#endif
    return true;
}

bool ReadMotionRemote(WiiRemoteInput::KpadSample& sample) {
#ifdef MKW_PLATFORM_IOS
    if (!MotionRemoteActive()) {
        return false;
    }
    IosMotionInput::Start();
    IosMotionInput::Sample motion;
    if (!IosMotionInput::Read(motion)) {
        return false;
    }

    float gravityX = 0.0f, gravityY = 0.0f;
    IosMotionSteering::RotateForLandscape(IosMotionInput::CurrentLandscapeOrientation(), motion.gravity[0],
                                           motion.gravity[1], gravityX, gravityY);
    double angle = 0.0;
    const bool hasSteeringGravity = IosMotionSteering::AngleFromGravity(gravityX, gravityY, angle);
    float steering = 0.0f;
    if (hasSteeringGravity) {
        if (!g_motionCentred) {
            g_motionCentreAngle = angle;
            g_motionCentred = true;
        }
        steering = IosMotionSteering::SteeringValue(
            angle, g_motionCentreAngle, RuntimeConfigFile::IosMotionSensitivity(), RuntimeConfigFile::IosMotionInverted());
    }
    const double shakeMagnitude = std::sqrt(motion.userAcceleration[0] * motion.userAcceleration[0] +
                                            motion.userAcceleration[1] * motion.userAcceleration[1] +
                                            motion.userAcceleration[2] * motion.userAcceleration[2]);
    const IosMotionSteering::ShakeAction shake = IosMotionSteering::ShakeActionForSample(
        shakeMagnitude, motion.timestamp, g_shakeArmed, g_lastShakeTimestamp);
    if (shake == IosMotionSteering::ShakeAction::Rearm) {
        g_shakeArmed = true;
    } else if (shake == IosMotionSteering::ShakeAction::Trigger) {
        g_shakeArmed = false;
        g_lastShakeTimestamp = motion.timestamp;
        // KPAD can be polled more than once during a game frame. Keep the
        // gesture visible for several samples so a harmless status poll cannot
        // consume the one motion event before Mario Kart reads race input.
        g_gestureFrames = 3;
    }
    const Frame frame = SampleFingers(CurrentAspect());
    UpdateAutoAccelerate(frame.a, CurrentTimeMs(), AutoAccelerateEnabled());

    // A thumb on the on-screen stick is an optional fallback for one-handed
    // play. It deliberately overrides only steering; the virtual Wii Remote
    // still supplies its motion frame for tricks, POW dodges and wheelies.
    if (frame.stickHeld) {
        steering = frame.stickDx;
    }

    // The core Wii Remote is held sideways: a level wheel has +x gravity and
    // turning it moves that gravity into z, matching the KPAD Wii Wheel frame.
    // Keep its y axis neutral after a lift. Holding a synthetic raised pose
    // there made the game keep a bike wheelie alive and fight the next turn.
    const float remainingGravity = std::max(0.0f, 1.0f - steering * steering);
    // CMMotion gravity is deliberately filtered. A real Wii Remote reports
    // the unfiltered accelerometer too, and Mario Kart uses that brief pulse
    // when the player lifts a Wii Wheel for a bike wheelie. Restore the
    // screen-normal part of the phone's gravity-free acceleration here.
    const float liftImpulse = std::clamp(-motion.userAcceleration[2], -1.5f, 1.5f);
    sample = {};
    sample.acc[0] = std::sqrt(remainingGravity);
    sample.acc[1] = liftImpulse;
    // The KPAD sideways-Remote axis has the opposite sign to the phone's
    // landscape steering angle: leaning the phone left must steer left with
    // the default setting.
    sample.acc[2] = -steering;
    if (g_gestureFrames != 0) {
        sample.acc[1] += 1.5f;
        // Pair the accelerometer impulse with the game's logical
        // trick/wheelie action. The core Wii Remote has no extension D-pad,
        // so use its Up bit directly; this is separate from the rotated
        // on-screen menu D-pad below.
        sample.hold |= 0x0008;
        --g_gestureFrames;
    }
    const bool aActive = frame.a || s_gasLocked;
    if (aActive) sample.hold |= 0x0100;       // 2: accelerate
    if (frame.b) sample.hold |= 0x0200;       // 1: brake / reverse
    if (frame.item) sample.hold |= 0x0800;    // A: use item
    if (frame.r) sample.hold |= 0x0400;       // B: drift
    if (frame.start) sample.hold |= 0x0010;   // +: pause
    // Mario Kart rotates core-Wii-Remote menu directions for a sideways Wii
    // Wheel. Rotate the screen D-pad in the opposite direction so its arrows
    // remain literal: screen up opens/selects up, and so on.
    if (frame.up) sample.hold |= 0x0002;
    if (frame.down) sample.hold |= 0x0001;
    if (frame.left) sample.hold |= 0x0008;
    if (frame.right) sample.hold |= 0x0004;
    return true;
#else
    (void)sample;
    return false;
#endif
}

bool MotionRemoteActive() {
#ifdef MKW_PLATFORM_IOS
    if (!RuntimeConfigFile::IosMotionControlsEnabled(false) || !IsActive()) {
        IosMotionInput::Stop();
        g_motionCentred = false;
        g_shakeArmed = false;
        g_lastShakeTimestamp = -1.0;
        g_gestureFrames = 0;
        return false;
    }
    // GameCube Motion owns the same Core Motion stream through Read(). Do not
    // stop it merely because this Wii Remote routing probe is expected to fail.
    if (RuntimeConfigFile::IosMotionGameCubeEnabled(false)) {
        return false;
    }
    IosMotionInput::Start();
    return IosMotionInput::IsAvailable();
#else
    return false;
#endif
}

bool MotionGameCubeActive() {
#ifdef MKW_PLATFORM_IOS
    if (!RuntimeConfigFile::IosMotionControlsEnabled(false) || !RuntimeConfigFile::IosMotionGameCubeEnabled(false) || !IsActive()) {
        return false;
    }
    IosMotionInput::Start();
    return IosMotionInput::IsAvailable();
#else
    return false;
#endif
}

void RecenterMotionRemote() {
#ifdef MKW_PLATFORM_IOS
    IosMotionInput::Sample motion;
    double angle = 0.0;
    float gravityX = 0.0f, gravityY = 0.0f;
    if (IosMotionInput::Read(motion)) {
        IosMotionSteering::RotateForLandscape(IosMotionInput::CurrentLandscapeOrientation(), motion.gravity[0],
                                               motion.gravity[1], gravityX, gravityY);
    }
    if (IosMotionSteering::AngleFromGravity(gravityX, gravityY, angle)) {
        g_motionCentreAngle = angle;
        g_motionCentred = true;
    } else {
        g_motionCentred = false;
    }
#endif
}

void ResetMotionRemote() {
#ifdef MKW_PLATFORM_IOS
    IosMotionInput::Stop();
    g_motionCentred = false;
    g_shakeArmed = false;
    g_lastShakeTimestamp = -1.0;
    g_gestureFrames = 0;
#endif
}

void Draw() {
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 size = io.DisplaySize;
    if (size.x <= 0.0f || size.y <= 0.0f || !HasTouchScreen()) {
        return;
    }
    const Layout L = LayoutFor(size.x / size.y);

    // The way into settings exists whenever there is a screen to tap, even when
    // a pad has hidden everything else.
    ImDrawList* menuList = ImGui::GetBackgroundDrawList();
    if (!PollFpsTap(size)) {
        // No FPS readout to tap, so fall back to an explicit button - otherwise a
        // touch host with show_fps off has no way into the settings bar at all.
        PollMenuButton();
        DrawCircle(menuList, L.menu, size, settings_overlay::TopBarVisible() ? "X" : "=",
                   settings_overlay::TopBarVisible(), kColGrey, nullptr);
    }

    if (!IsActive()) {
        return;
    }
    const Frame frame = SampleFingers(size.x / size.y);
    ImDrawList* list = ImGui::GetBackgroundDrawList();

    const bool motionWheel = RuntimeConfigFile::IosMotionControlsEnabled(false) &&
                             !RuntimeConfigFile::IosMotionGameCubeEnabled(false);
    const bool aPressed = frame.a || s_gasLocked;
    if (!motionWheel) {
        // Stick: outer ring plus a thumb dot showing current deflection.
        // The glyph is the knob; nothing is drawn behind it.
        const ImVec2 stickCentre{L.stick.x * size.x, L.stick.y * size.y};
        const float stickRadius = L.stick.r * size.y;
        const float knobRadius = stickRadius * 0.80f;
        const float travel = stickRadius * 0.38f;
        const Circle knob{(stickCentre.x + frame.stickDx * travel) / size.x,
                          (stickCentre.y + frame.stickDy * travel) / size.y,
                          knobRadius / size.y};
        DrawCircle(list, knob, size, "", frame.stickHeld, kColStick, "control_stick");
        DrawCircle(list, L.a, size, "A", aPressed, kColA, "a", s_gasLocked);
        DrawCircle(list, L.b, size, "B", frame.b, kColB, "b");
        DrawCircle(list, L.item, size, "Z", frame.item, kColZ, "right_bumper");
        DrawCircle(list, L.start, size, "START", frame.start, kColGrey, "start_pause");
        DrawCircle(list, L.shoulderL, size, "L", frame.l, kColGrey, "l_analog");
        DrawCircle(list, L.shoulderR, size, "R", frame.r, kColGrey, "r_analog");
    } else {
        // Wii Wheel mode sends a sideways core Remote through KPAD. Use plain
        // glyphs instead of the GameCube artwork so the displayed controls
        // exactly match the virtual buttons.
        DrawCircle(list, L.a, size, "2", aPressed, kColA, nullptr, s_gasLocked);
        DrawCircle(list, L.b, size, "1", frame.b, kColB, nullptr);
        DrawCircle(list, L.item, size, "A", frame.item, kColZ, nullptr);
        DrawCircle(list, L.start, size, "+", frame.start, kColGrey, nullptr);
        DrawCircle(list, L.shoulderR, size, "B", frame.r, kColGrey, nullptr);

        // Keep the familiar touch stick available above the D-pad. It is a
        // temporary steering fallback, useful for menus and one-handed play.
        const ImVec2 stickCentre{L.stick.x * size.x, L.stick.y * size.y};
        const float stickRadius = L.stick.r * size.y;
        const float knobRadius = stickRadius * 0.80f;
        const float travel = stickRadius * 0.38f;
        const Circle knob{(stickCentre.x + frame.stickDx * travel) / size.x,
                          (stickCentre.y + frame.stickDy * travel) / size.y,
                          knobRadius / size.y};
        DrawCircle(list, knob, size, "", frame.stickHeld, kColStick, "control_stick");
    }
    const bool dpadHeld = frame.up || frame.down || frame.left || frame.right;
    DrawCircle(list, L.dpad, size, "", dpadHeld, kColGrey, "d-pad");
}

} // namespace TouchPad

