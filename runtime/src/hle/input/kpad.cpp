#include "hle_stubs.h"
#include "input_bindings.h"
#include "memory.h"
#include "wii_remote_input.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// KPAD HLE fed by a real Bluetooth Wii Remote. The game calls KPADRead once per
// frame with room for 16 KPADStatus entries and only looks at entry 0; with a
// Classic Controller it also calls KPADGetUnifiedWpadStatus for the raw
// WPADCLStatus (buttons, sticks and triggers of the extension).
namespace {

constexpr uint32_t kKpadStatusSize = 0x84;

// KPADStatus field offsets (RVL SDK).
constexpr uint32_t kHold = 0x00, kTrig = 0x04, kRelease = 0x08, kAcc = 0x0C, kAccValue = 0x18,
                   kAccSpeed = 0x1C, kPos = 0x20, kAccVertical = 0x54, kDevType = 0x5C, kWpadErr = 0x5D,
                   kDpdValidFg = 0x5E, kDataFormat = 0x5F, kFsStick = 0x60, kFsAcc = 0x68, kFsAccValue = 0x74,
                   kFsAccSpeed = 0x78;
// KPADStatus.ex_status.cl (KPADEXStatus, Classic Controller view).
constexpr uint32_t kClHold = 0x60, kClTrig = 0x64, kClRelease = 0x68, kClLStick = 0x6C, kClRStick = 0x74,
                   kClLTrigger = 0x7C, kClRTrigger = 0x80;

// KPADUnifiedWpadStatus: WPADStatus / WPADFSStatus / WPADCLStatus union, then fmt.
constexpr uint32_t kUnifiedSize = 0x38;
constexpr uint32_t kUButton = 0x00, kUAccX = 0x02, kUAccY = 0x04, kUAccZ = 0x06, kUObj = 0x08, kUDev = 0x28,
                   kUErr = 0x29, kUFsStickX = 0x2A, kUFsStickY = 0x2B, kUFsAccX = 0x2C, kUFsAccY = 0x2E,
                   kUFsAccZ = 0x30, kUClButton = 0x2A, kUClLStickX = 0x2C, kUClLStickY = 0x2E, kUClRStickX = 0x30,
                   kUClRStickY = 0x32, kUClTriggerL = 0x34, kUClTriggerR = 0x35, kUFmt = 0x36;

// WPAD device types (WPAD_DEV_*) and the data formats KPAD runs each of them
// in (WPAD_FMT_*_ACC_DPD): the values KPADStatus.dev_type / data_format and
// KPADUnifiedWpadStatus.dev / fmt carry on the console.
constexpr uint8_t kDevCore = 0;
constexpr uint8_t kDevFreestyle = 1;
constexpr uint8_t kDevClassic = 2;
constexpr uint8_t kFmtCoreAccDpd = 2;
constexpr uint8_t kFmtFreestyleAccDpd = 5;
constexpr uint8_t kFmtClassicAccDpd = 8;
constexpr int8_t kWpadErrNone = 0;
constexpr int8_t kWpadErrNoController = -1;

// Raw accelerometer as WPADStatus carries it: 10 bits, 0x200 at 0 g, 100 per g.
constexpr float kRawAccZero = 512.0f;
constexpr float kRawAccPerG = 100.0f;

struct ChannelState {
    uint32_t prevHold = 0;
    uint32_t prevClHold = 0;
    float prevAcc[3] = {0.0f, -1.0f, 0.0f};
    float prevFsAcc[3] = {0.0f, -1.0f, 0.0f};
};

std::array<ChannelState, 4> g_channels{};

// Euclidean length of a 3-vector.
float Length(const float* v) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// Euclidean distance between two 3-vectors.
float Distance(const float* a, const float* b) {
    const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return Length(d);
}

// Writes three big-endian floats to guest memory.
void WriteVec3(uint32_t addr, const float* v) {
    Memory::WriteFloat32(addr, v[0]);
    Memory::WriteFloat32(addr + 4, v[1]);
    Memory::WriteFloat32(addr + 8, v[2]);
}

// Zeroes `count` consecutive floats in guest memory.
void WriteZeroFloats(uint32_t addr, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        Memory::WriteFloat32(addr + i * 4, 0.0f);
    }
}

// Fills one KPADStatus at `addr` and returns the number of valid entries (1),
// or writes an "unplugged" status and returns 0.
int32_t WriteStatus(uint32_t chan, uint32_t addr, const WiiRemoteInput::KpadSample* sample) {
    ChannelState& state = g_channels[chan];
    if (sample == nullptr) {
        state = {};
        Memory::Write32(addr + kHold, 0);
        Memory::Write32(addr + kTrig, 0);
        Memory::Write32(addr + kRelease, 0);
        Memory::Write8(addr + kDevType, kDevCore);
        Memory::Write8(addr + kWpadErr, static_cast<uint8_t>(kWpadErrNoController));
        Memory::Write8(addr + kDpdValidFg, 0);
        return 0;
    }

    const uint32_t hold = sample->hold;
    Memory::Write32(addr + kHold, hold);
    Memory::Write32(addr + kTrig, hold & ~state.prevHold);
    Memory::Write32(addr + kRelease, state.prevHold & ~hold);
    state.prevHold = hold;

    WriteVec3(addr + kAcc, sample->acc);
    Memory::WriteFloat32(addr + kAccValue, Length(sample->acc));
    Memory::WriteFloat32(addr + kAccSpeed, Distance(sample->acc, state.prevAcc));
    for (int i = 0; i < 3; ++i) state.prevAcc[i] = sample->acc[i];

    // No IR pointer: pos .. acc_vertical zeroed and dpd_valid_fg clear, which
    // the game treats as "pointing away from the screen".
    WriteZeroFloats(addr + kPos, (kAccVertical + 8 - kPos) / 4);
    Memory::Write8(addr + kDpdValidFg, 0);

    const uint8_t devType = sample->hasClassic ? kDevClassic : sample->hasNunchuk ? kDevFreestyle : kDevCore;
    const uint8_t dataFormat =
        sample->hasClassic ? kFmtClassicAccDpd : sample->hasNunchuk ? kFmtFreestyleAccDpd : kFmtCoreAccDpd;
    Memory::Write8(addr + kDevType, devType);
    Memory::Write8(addr + kWpadErr, static_cast<uint8_t>(kWpadErrNone));
    Memory::Write8(addr + kDataFormat, dataFormat);

    if (sample->hasClassic) {
        const uint32_t clHold = sample->clHold;
        Memory::Write32(addr + kClHold, clHold);
        Memory::Write32(addr + kClTrig, clHold & ~state.prevClHold);
        Memory::Write32(addr + kClRelease, state.prevClHold & ~clHold);
        state.prevClHold = clHold;
        Memory::WriteFloat32(addr + kClLStick, sample->clLStick[0]);
        Memory::WriteFloat32(addr + kClLStick + 4, sample->clLStick[1]);
        Memory::WriteFloat32(addr + kClRStick, sample->clRStick[0]);
        Memory::WriteFloat32(addr + kClRStick + 4, sample->clRStick[1]);
        Memory::WriteFloat32(addr + kClLTrigger, sample->clTriggerL / 255.0f);
        Memory::WriteFloat32(addr + kClRTrigger, sample->clTriggerR / 255.0f);
    } else if (sample->hasNunchuk) {
        state.prevClHold = 0;
        Memory::WriteFloat32(addr + kFsStick, sample->stick[0]);
        Memory::WriteFloat32(addr + kFsStick + 4, sample->stick[1]);
        WriteVec3(addr + kFsAcc, sample->nunchukAcc);
        Memory::WriteFloat32(addr + kFsAccValue, Length(sample->nunchukAcc));
        Memory::WriteFloat32(addr + kFsAccSpeed, Distance(sample->nunchukAcc, state.prevFsAcc));
        for (int i = 0; i < 3; ++i) state.prevFsAcc[i] = sample->nunchukAcc[i];
    } else {
        state.prevClHold = 0;
        WriteZeroFloats(addr + kFsStick, (kKpadStatusSize - kFsStick) / 4);
    }
    return 1;
}

// One accelerometer axis of KPAD's g vector back to the 10-bit raw WPAD value.
uint16_t RawAcc(float g) {
    const float raw = kRawAccZero + g * kRawAccPerG;
    return static_cast<uint16_t>(std::clamp(raw, 0.0f, 1023.0f));
}

// Fills one KPADUnifiedWpadStatus at `addr` from the sample: the WPADStatus core
// (remote buttons, raw accelerometer, no IR objects), then the Nunchuk or Classic
// Controller tail, then the data format.
void WriteUnifiedStatus(uint32_t addr, const WiiRemoteInput::KpadSample* sample) {
    for (uint32_t offset = 0; offset < kUnifiedSize; offset += 4) {
        Memory::Write32(addr + offset, 0);
    }
    if (sample == nullptr) {
        Memory::Write8(addr + kUDev, kDevCore);
        Memory::Write8(addr + kUErr, static_cast<uint8_t>(kWpadErrNoController));
        Memory::Write8(addr + kUFmt, kFmtCoreAccDpd);
        return;
    }
    Memory::Write16(addr + kUButton, static_cast<uint16_t>(sample->hold & 0xFFFF));
    // KPAD's acc is (-wiiX, -wiiZ, wiiY); WPADStatus keeps the remote's own axes.
    Memory::Write16(addr + kUAccX, RawAcc(-sample->acc[0]));
    Memory::Write16(addr + kUAccY, RawAcc(sample->acc[2]));
    Memory::Write16(addr + kUAccZ, RawAcc(-sample->acc[1]));
    // No IR: every DPDObject invalid (x/y at the sensor's out-of-range value).
    for (uint32_t i = 0; i < 4; ++i) {
        Memory::Write16(addr + kUObj + i * 8, 0x3FF);
        Memory::Write16(addr + kUObj + i * 8 + 2, 0x3FF);
    }
    Memory::Write8(addr + kUErr, static_cast<uint8_t>(kWpadErrNone));
    if (sample->hasClassic) {
        Memory::Write8(addr + kUDev, kDevClassic);
        Memory::Write16(addr + kUClButton, static_cast<uint16_t>(sample->clHold & 0xFFFF));
        Memory::Write16(addr + kUClLStickX, static_cast<uint16_t>(sample->clLStickRaw[0]));
        Memory::Write16(addr + kUClLStickY, static_cast<uint16_t>(sample->clLStickRaw[1]));
        Memory::Write16(addr + kUClRStickX, static_cast<uint16_t>(sample->clRStickRaw[0]));
        Memory::Write16(addr + kUClRStickY, static_cast<uint16_t>(sample->clRStickRaw[1]));
        Memory::Write8(addr + kUClTriggerL, sample->clTriggerL);
        Memory::Write8(addr + kUClTriggerR, sample->clTriggerR);
        Memory::Write8(addr + kUFmt, kFmtClassicAccDpd);
    } else if (sample->hasNunchuk) {
        Memory::Write8(addr + kUDev, kDevFreestyle);
        // WPADFSStatus: 8-bit stick (centre 128) and 10-bit Nunchuk accelerometer.
        Memory::Write8(addr + kUFsStickX,
                       static_cast<uint8_t>(std::clamp(128.0f + sample->stick[0] * 100.0f, 0.0f, 255.0f)));
        Memory::Write8(addr + kUFsStickY,
                       static_cast<uint8_t>(std::clamp(128.0f + sample->stick[1] * 100.0f, 0.0f, 255.0f)));
        Memory::Write16(addr + kUFsAccX, RawAcc(-sample->nunchukAcc[0]));
        Memory::Write16(addr + kUFsAccY, RawAcc(sample->nunchukAcc[2]));
        Memory::Write16(addr + kUFsAccZ, RawAcc(-sample->nunchukAcc[1]));
        Memory::Write8(addr + kUFmt, kFmtFreestyleAccDpd);
    } else {
        Memory::Write8(addr + kUDev, kDevCore);
        Memory::Write8(addr + kUFmt, kFmtCoreAccDpd);
    }
}

// Neutral sticks and no buttons while an overlay owns input; the remote stays connected.
bool ReadSample(uint32_t chan, WiiRemoteInput::KpadSample& sample) {
    if (!WiiRemoteInput::ReadKpadSample(chan, sample)) {
        return false;
    }
    if (InputBindings::InputBlocked()) {
        sample.hold = 0;
        sample.clHold = 0;
        sample.stick[0] = sample.stick[1] = 0.0f;
        sample.clLStick[0] = sample.clLStick[1] = 0.0f;
        sample.clRStick[0] = sample.clRStick[1] = 0.0f;
        sample.clLStickRaw[0] = sample.clLStickRaw[1] = 0;
        sample.clRStickRaw[0] = sample.clRStickRaw[1] = 0;
        sample.clTriggerL = sample.clTriggerR = 0;
    }
    return true;
}

} // namespace

// KPADRead: fills KPADStatus[0] for `chan` from the Bluetooth remote, returns the entry count.
extern "C" int32_t KPAD__Read_HLE(uint32_t chan, uint32_t statusPtr, uint32_t count)
{
    if (chan >= g_channels.size() || statusPtr == 0 || count == 0) {
        return 0;
    }
    WiiRemoteInput::KpadSample sample;
    const bool have = ReadSample(chan, sample);
    try {
        return WriteStatus(chan, statusPtr, have ? &sample : nullptr);
    } catch (const Memory::AccessViolation&) {
        return 0;
    }
}
PPC_NATIVE_OVERRIDE(80197380, KPAD__Read_HLE, int32_t, (uint32_t chan, uint32_t statusPtr, uint32_t count),
         (chan, statusPtr, count));

// KPADGetUnifiedWpadStatus: the raw WPAD status behind KPADStatus. The game
// reads the Classic Controller's buttons, sticks and triggers from here. The
// SDK fills `count` entries with the channel's recent samples (the game asks for
// as many as it asked KPADRead for and looks at entry 0); with one sample per
// frame here, every entry gets the current one.
extern "C" int32_t KPAD__GetUnifiedWpadStatus_HLE(uint32_t chan, uint32_t statusPtr, uint32_t count)
{
    constexpr uint32_t kMaxEntries = 16; // KPAD_MAX_READ_BUFS
    if (chan >= g_channels.size() || statusPtr == 0 || count == 0) {
        return 0;
    }
    WiiRemoteInput::KpadSample sample;
    const bool have = ReadSample(chan, sample);
    try {
        const uint32_t entries = std::min(count, kMaxEntries);
        for (uint32_t i = 0; i < entries; ++i) {
            WriteUnifiedStatus(statusPtr + i * kUnifiedSize, have ? &sample : nullptr);
        }
    } catch (const Memory::AccessViolation&) {
        return 0;
    }
    return have ? 1 : 0;
}
PPC_NATIVE_OVERRIDE(8019812C, KPAD__GetUnifiedWpadStatus_HLE, int32_t,
         (uint32_t chan, uint32_t statusPtr, uint32_t count), (chan, statusPtr, count));
