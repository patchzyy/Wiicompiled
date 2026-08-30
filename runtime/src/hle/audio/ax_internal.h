#pragma once

// AX/DSP HLE internals shared within this directory; ax_dsp.h is the public surface.
// Hot accessors stay header-inline because the shipped build links runtime shards without LTO.

#include "isa/big_endian.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace AxDspHle {

constexpr uint32_t kMailCmdList = 0xBABE0000u;
constexpr uint32_t kMailCmdListMask = 0xFFFF0000u;
constexpr uint32_t kTaskMailToCpu = 0xDCD10000u;
constexpr uint32_t kTaskMailToDsp = 0xCDD10000u;
constexpr uint32_t kDspInit = kTaskMailToCpu | 0x0000u;
constexpr uint32_t kDspResume = kTaskMailToCpu | 0x0001u;
constexpr uint32_t kDspYield = kTaskMailToCpu | 0x0002u;
constexpr uint32_t kDspDone = kTaskMailToCpu | 0x0003u;
constexpr uint32_t kDspSync = kTaskMailToCpu | 0x0004u;
constexpr uint32_t kMailResume = kTaskMailToDsp | 0x0000u;
constexpr uint32_t kMailNewUCode = kTaskMailToDsp | 0x0001u;
constexpr uint32_t kMailReset = kTaskMailToDsp | 0x0002u;
constexpr size_t kResamplingCoefficientCount = 0x800;
constexpr uint32_t kMailContinue = kTaskMailToDsp | 0x0003u;
constexpr uint32_t kAxSamplesPerFrame = 96u;
constexpr uint32_t kAxDspTaskAddr = MKW_GADDR(802F81A0);
constexpr uint32_t kDspInitializedAddr = MKW_GADDR(80386608);
constexpr uint32_t kDspAssertPendingAddr = MKW_GADDR(80386610);
constexpr uint32_t kDspAssertTaskAddr = MKW_GADDR(80386614);
constexpr uint32_t kDspCurrentTaskAddr = MKW_GADDR(8038661C);
constexpr uint32_t kDspFirstTaskAddr = MKW_GADDR(80386620);
constexpr uint32_t kDspRunningTaskAddr = MKW_GADDR(80386624);
constexpr uint32_t kAxIramMmemAddr = MKW_GADDR(8027F820);
constexpr uint32_t kAxDramMmemAddr = MKW_GADDR(802F8200);
constexpr uint32_t kAxDramLength = 64u;
constexpr uint32_t kAxDramDspAddr = 3282u;
constexpr uint32_t kAxInitCallback = MKW_GADDR(80126948);
constexpr uint32_t kAxResumeCallback = MKW_GADDR(80126954);
constexpr uint32_t kAxDoneCallback = MKW_GADDR(801269A8);
constexpr uint32_t kAxRequestCallback = MKW_GADDR(801269B8);

extern uint32_t g_axTaskPtr;

// The mix worker must resolve guest addresses only via the MEM1/MEM2 region bases below, never
// via MemoryInline::GetPointerFast or the flat guest view: GX deferred (EFB) reads rewrite the
// page table and flip protections from the guest thread behind its back, but the MEM1/MEM2 host
// alias is never reprotected.
extern std::atomic<uint8_t*> g_mixMem1;
extern std::atomic<uint8_t*> g_mixMem2;

// True only on the mix worker thread. Guest-thread callers keep the exact
// fallback behaviour they had before (Memory::Read*/Write*, which materializes
// deferred reads and reports access violations); the worker instead fails safe.
extern thread_local bool t_onMixWorker;

void ReportMixAddressOutOfRange(uint32_t addr, size_t bytes);

void RefreshMixMemoryMap();

// Host pointer for a guest range that lies entirely inside MEM1 or MEM2 through
// any of their physical/cached/uncached aliases; nullptr otherwise.
inline uint8_t* MixResolveRange(uint32_t addr, size_t bytes) {
    if (bytes == 0 || bytes > Memory::kMem2Size) {
        return nullptr;
    }
    if (addr > UINT32_MAX - static_cast<uint32_t>(bytes - 1)) {
        return nullptr;
    }

    constexpr uint32_t kMem1Size = static_cast<uint32_t>(Memory::kMem1Size);
    constexpr uint32_t kMem2Size = static_cast<uint32_t>(Memory::kMem2Size);

    uint32_t offset = 0;
    bool inMem1 = false;
    if (addr < kMem1Size) {
        offset = addr;
        inMem1 = true;
    } else if (addr - Memory::kMem1CachedBase < kMem1Size) {
        offset = addr - Memory::kMem1CachedBase;
        inMem1 = true;
    } else if (addr - Memory::kMem1UncachedBase < kMem1Size) {
        offset = addr - Memory::kMem1UncachedBase;
        inMem1 = true;
    }
    if (inMem1) {
        if (bytes > kMem1Size - offset) {
            return nullptr;
        }
        uint8_t* base = g_mixMem1.load(std::memory_order_relaxed);
        return base != nullptr ? base + offset : nullptr;
    }

    bool inMem2 = false;
    if (addr - Memory::kMem2PhysicalBase < kMem2Size) {
        offset = addr - Memory::kMem2PhysicalBase;
        inMem2 = true;
    } else if (addr - Memory::kMem2CachedBase < kMem2Size) {
        offset = addr - Memory::kMem2CachedBase;
        inMem2 = true;
    } else if (addr - Memory::kMem2UncachedBase < kMem2Size) {
        offset = addr - Memory::kMem2UncachedBase;
        inMem2 = true;
    }
    if (inMem2) {
        if (bytes > kMem2Size - offset) {
            return nullptr;
        }
        uint8_t* base = g_mixMem2.load(std::memory_order_relaxed);
        return base != nullptr ? base + offset : nullptr;
    }
    return nullptr;
}

// One body for the two guest widths the mix reads, so the worker-safety fallback above can
// only ever be got right or wrong once; stays header-inline since the shipped build has no LTO.
template <typename T>
inline T MixReadBE(uint32_t addr) {
    static_assert(std::is_same<T, uint16_t>::value || std::is_same<T, uint32_t>::value,
                  "the AX mix only reads 16- and 32-bit guest words");
    if (const uint8_t* host = MixResolveRange(addr, sizeof(T))) {
        T value = 0;
        std::memcpy(&value, host, sizeof(value));
        if constexpr (std::is_same<T, uint16_t>::value) {
            return MemoryInline::ByteSwap16(value);
        } else {
            return MemoryInline::ByteSwap32(value);
        }
    }
    if (t_onMixWorker) {
        ReportMixAddressOutOfRange(addr, sizeof(T));
        return 0;
    }
    if constexpr (std::is_same<T, uint16_t>::value) {
        return Memory::Read16(addr);
    } else {
        return Memory::Read32(addr);
    }
}

inline uint16_t MixRead16(uint32_t addr) { return MixReadBE<uint16_t>(addr); }

inline uint32_t MixRead32(uint32_t addr) { return MixReadBE<uint32_t>(addr); }

inline void MixWrite16(uint32_t addr, uint16_t value) {
    if (uint8_t* host = MixResolveRange(addr, sizeof(uint16_t))) {
        BigEndian::Write16(host, value);
        return;
    }
    if (t_onMixWorker) {
        ReportMixAddressOutOfRange(addr, sizeof(uint16_t));
        return;
    }
    Memory::Write16(addr, value);
}

inline bool MixContains(uint32_t addr, size_t bytes) {
    if (MixResolveRange(addr, bytes) != nullptr) {
        return true;
    }
    return t_onMixWorker ? false : Memory::Contains(addr, bytes);
}

uint32_t ReadGuestU32OrZero(uint32_t addr);

void MarkDspInitialized();

void ResetDspTaskGlobals();

void LinkSingleDspTask(uint32_t taskPtr);

uint32_t HashEctorGuest(uint32_t addr, uint32_t length);

void InvokeAxTaskCallback(uint32_t callbackOffset, bool passTask);

// Voice sample fetch walks a contiguous guest range one byte at a time, so the
// enclosing guest page is resolved once per window and reused.
constexpr uint32_t kAramWindowShift = 12;
constexpr uint32_t kAramWindowSize = 1u << kAramWindowShift;

struct AramWindow {
    uint32_t begin = 0;
    uint32_t end = 0; // Exclusive. begin == end marks an empty window.
    uint32_t generation = 0;
    const uint8_t* host = nullptr;
};

// Bumped whenever the AX HLE (re)initializes so a cached host window can never
// outlive the guest memory mapping it was resolved against.
extern std::atomic<uint32_t> g_aramWindowGeneration;

uint8_t ReadAramByteSlow(uint32_t addr);

bool ResolveAramWindow(uint32_t addr, AramWindow& window);

inline uint8_t ReadAramByte(uint32_t addr) {
    static thread_local AramWindow window{};
    if (addr >= window.begin && addr < window.end &&
        window.generation == g_aramWindowGeneration.load(std::memory_order_relaxed)) {
        return window.host[addr - window.begin];
    }
    if (ResolveAramWindow(addr, window)) {
        return window.host[addr - window.begin];
    }
    return ReadAramByteSlow(addr);
}

enum class MailState {
    WaitingForCmdListSize,
    WaitingForCmdListAddress,
    WaitingForNextTask,
};

enum class AXCommandLayout {
    Auto,
    New,
    NewNoOutputVolume,
    Old,
};

enum AXMixControl : uint32_t {
    MIX_MAIN_L = 0x000001,
    MIX_MAIN_L_RAMP = 0x000002,
    MIX_MAIN_R = 0x000004,
    MIX_MAIN_R_RAMP = 0x000008,
    MIX_MAIN_S = 0x000010,
    MIX_MAIN_S_RAMP = 0x000020,
    MIX_AUXA_L = 0x000040,
    MIX_AUXA_L_RAMP = 0x000080,
    MIX_AUXA_R = 0x000100,
    MIX_AUXA_R_RAMP = 0x000200,
    MIX_AUXA_S = 0x000400,
    MIX_AUXA_S_RAMP = 0x000800,
    MIX_AUXB_L = 0x001000,
    MIX_AUXB_L_RAMP = 0x002000,
    MIX_AUXB_R = 0x004000,
    MIX_AUXB_R_RAMP = 0x008000,
    MIX_AUXB_S = 0x010000,
    MIX_AUXB_S_RAMP = 0x020000,
    MIX_AUXC_L = 0x040000,
    MIX_AUXC_L_RAMP = 0x080000,
    MIX_AUXC_R = 0x100000,
    MIX_AUXC_R_RAMP = 0x200000,
    MIX_AUXC_S = 0x400000,
    MIX_AUXC_S_RAMP = 0x800000,
};

struct VolumeData {
    uint16_t volume;
    uint16_t volume_delta;
};

struct PBMixerWii {
    VolumeData main_left, main_right;
    VolumeData auxA_left, auxA_right;
    VolumeData auxB_left, auxB_right;
    VolumeData auxC_left, auxC_right;
    VolumeData main_surround;
    VolumeData auxA_surround;
    VolumeData auxB_surround;
    VolumeData auxC_surround;
};

struct PBInitialTimeDelay {
    uint16_t on;
    uint16_t addrMemHigh;
    uint16_t addrMemLow;
    uint16_t offsetLeft;
    uint16_t offsetRight;
    uint16_t targetLeft;
    uint16_t targetRight;
};

struct PBDpopWii {
    int16_t main_left;
    int16_t auxA_left;
    int16_t auxB_left;
    int16_t auxC_left;
    int16_t main_right;
    int16_t auxA_right;
    int16_t auxB_right;
    int16_t auxC_right;
    int16_t main_surround;
    int16_t auxA_surround;
    int16_t auxB_surround;
    int16_t auxC_surround;
};

struct PBUpdatesWii {
    uint16_t num_updates[3];
    uint16_t data_hi;
    uint16_t data_lo;
};

struct PBUpdate {
    uint16_t pb_offset;
    uint16_t new_value;
};

struct PBVolumeEnvelope {
    int16_t cur_volume;
    int16_t cur_volume_delta;
};

struct PBAudioAddr {
    uint16_t looping;
    uint16_t sample_format;
    uint16_t loop_addr_hi;
    uint16_t loop_addr_lo;
    uint16_t end_addr_hi;
    uint16_t end_addr_lo;
    uint16_t cur_addr_hi;
    uint16_t cur_addr_lo;
};

struct PBADPCMInfo {
    int16_t coefs[16];
    uint16_t gain;
    uint16_t pred_scale;
    int16_t yn1;
    int16_t yn2;
};

struct PBSampleRateConverter {
    uint16_t ratio_hi;
    uint16_t ratio_lo;
    uint16_t cur_addr_frac;
    int16_t last_samples[4];
};

struct PBSampleRateConverterWM {
    uint16_t cur_addr_frac;
    int16_t last_samples[4];
};

struct PBADPCMLoopInfo {
    uint16_t pred_scale;
    uint16_t yn1;
    uint16_t yn2;
};

struct PBLowPassFilter {
    uint16_t on;
    int16_t yn1;
    uint16_t a0;
    int16_t b0;
};

struct PBHighPassFilter {
    uint16_t on;
    uint16_t unk[3];
};

struct PBBiquadFilter {
    uint16_t on;
    int16_t xn1;
    int16_t xn2;
    int16_t yn1;
    int16_t yn2;
    int16_t b0;
    int16_t b1;
    int16_t b2;
    int16_t a1;
    int16_t a2;
};

struct PBMixerWM {
    VolumeData main0, aux0;
    VolumeData main1, aux1;
    VolumeData main2, aux2;
    VolumeData main3, aux3;
};

struct PBDpopWM {
    int16_t main0;
    int16_t main1;
    int16_t main2;
    int16_t main3;
    int16_t aux0;
    int16_t aux1;
    int16_t aux2;
    int16_t aux3;
};

union PBInfImpulseResponseWM {
    uint16_t on;
    PBLowPassFilter lpf;
    PBBiquadFilter biquad;
};

struct AXPBWii {
    uint16_t next_pb_hi;
    uint16_t next_pb_lo;
    uint16_t this_pb_hi;
    uint16_t this_pb_lo;
    uint16_t src_type;
    uint16_t coef_select;
    uint16_t mixer_control_hi;
    uint16_t mixer_control_lo;
    uint16_t running;
    uint16_t is_stream;
    PBMixerWii mixer;
    PBInitialTimeDelay initial_time_delay;
    PBDpopWii dpop;
    PBUpdatesWii updates;
    PBVolumeEnvelope vol_env;
    PBAudioAddr audio_addr;
    PBADPCMInfo adpcm;
    PBSampleRateConverter src;
    PBADPCMLoopInfo adpcm_loop_info;
    PBLowPassFilter lpf;
    union {
        PBHighPassFilter hpf;
        PBBiquadFilter biquad;
    };
    uint16_t remote;
    uint16_t remote_mixer_control;
    PBMixerWM remote_mixer;
    PBDpopWM remote_dpop;
    PBSampleRateConverterWM remote_src;
    PBInfImpulseResponseWM remote_iir;
    uint16_t pad[2];
};

// The AX mix buses in SETUP's block order. Load-bearing in three places (SETUP walks by
// index, ProcessVoice mixes by name, AdvanceBuffers steps by class), so declared once here.
enum AxBus : size_t {
    kBusMainL = 0,
    kBusMainR,
    kBusMainS,
    kBusAuxAL,
    kBusAuxAR,
    kBusAuxAS,
    kBusAuxBL,
    kBusAuxBR,
    kBusAuxBS,
    kBusAuxCL,
    kBusAuxCR,
    kBusAuxCS,
    kBusWm0Main,
    kBusWm0Aux,
    kBusWm1Main,
    kBusWm1Aux,
    kBusWm2Main,
    kBusWm2Aux,
    kBusWm3Main,
    kBusWm3Aux,
    kAxBusCount,
};

// The buses before kBusWm0Main run at 32 samples per millisecond; the wiimote
// buses that follow run at 6.
constexpr size_t kAxRegularBusCount = kBusWm0Main;
constexpr size_t kAxWiimoteBusCount = kAxBusCount - kAxRegularBusCount;
constexpr uint32_t kAxWiimoteSamplesPerFrame = 18u;

using AXBuffers = std::array<int*, kAxBusCount>;

inline uint32_t Hilo(uint16_t hi, uint16_t lo) {
    return (static_cast<uint32_t>(hi) << 16) | lo;
}

inline int16_t ClampS16(int64_t sample) {
    return static_cast<int16_t>(std::clamp<int64_t>(sample, -0x8000, 0x7fff));
}

inline int16_t ClampAdpcmS16(int64_t sample) {
    return static_cast<int16_t>(std::clamp<int64_t>(sample, -0x7fff, 0x7fff));
}

enum class PBLayout {
    Unknown,
    SkipBiquadGapOnly,
    SkipUpdatesAndBiquadGap,
    SkipUpdatesOnly,
};

inline PBLayout PBLayoutForUCode(uint32_t crc) {
    switch (crc) {
    case 0x7699af32u:
    case 0xfa450138u:
        return PBLayout::SkipBiquadGapOnly;
    case 0xd9c4bf34u:
    case 0xadbc06bdu:
        return PBLayout::SkipUpdatesAndBiquadGap;
    case 0x347112bau:
    case 0x4cc52064u:
        return PBLayout::SkipUpdatesOnly;
    default:
        return PBLayout::Unknown;
    }
}

inline bool UCodeUsesOldAxWiiCommands(uint32_t crc) {
    return crc == 0xfa450138u || crc == 0x7699af32u;
}

inline bool UCodeUsesNewFilter(uint32_t crc) {
    return crc == 0x347112bau || crc == 0x4cc52064u;
}

inline AXCommandLayout CommandLayoutForUCode(uint32_t crc) {
    if (UCodeUsesOldAxWiiCommands(crc)) {
        return AXCommandLayout::Old;
    }
    if (crc == 0xd9c4bf34u) {
        return AXCommandLayout::NewNoOutputVolume;
    }
    return AXCommandLayout::New;
}

void WriteGuestS32Buffer(uint32_t addr, const int* src, size_t count);

void ReadGuestS32Buffer(uint32_t addr, int* dst, size_t count);

class Accelerator {
public:
    void Setup(AXPBWii* pb) {
        m_pb = pb;
        m_start = Hilo(pb->audio_addr.loop_addr_hi, pb->audio_addr.loop_addr_lo) & 0x3fffffffu;
        m_end = Hilo(pb->audio_addr.end_addr_hi, pb->audio_addr.end_addr_lo) & 0x3fffffffu;
        m_current = Hilo(pb->audio_addr.cur_addr_hi, pb->audio_addr.cur_addr_lo) & 0xbfffffffu;
        m_format = pb->audio_addr.sample_format;
        m_gain = static_cast<int16_t>(pb->adpcm.gain);
        m_pred_scale = pb->adpcm.pred_scale & 0x7f;
        m_yn1 = pb->adpcm.yn1;
        m_yn2 = pb->adpcm.yn2;
        m_reads_stopped = false;
    }

    int16_t ReadSample() {
        if (!m_pb || m_reads_stopped) {
            return 0;
        }

        uint16_t raw = CurrentRawSample();
        uint16_t decode = (m_format >> 2) & 3u;
        int16_t value = 0;
        uint8_t step = 2;

        if (decode == 0) {
            raw &= 0xf;
            int32_t nibble = raw >= 8 ? static_cast<int32_t>(raw) - 16 : static_cast<int32_t>(raw);
            const int coef_idx = (m_pred_scale >> 4) & 0x7;
            const int32_t coef1 = m_pb->adpcm.coefs[coef_idx * 2 + 0];
            const int32_t coef2 = m_pb->adpcm.coefs[coef_idx * 2 + 1];
            const int32_t scale = 1 << (m_pred_scale & 0xf);
            const int32_t decoded = scale * nibble + ((0x400 + coef1 * m_yn1 + coef2 * m_yn2) >> 11);
            value = ClampAdpcmS16(decoded);
            m_yn2 = m_yn1;
            m_yn1 = value;
            ++m_current;
            if ((m_end & 0xf) == 0 && m_current == m_end) {
                m_current = m_start + 1;
            } else if ((m_end & 0xf) == 1 && m_current == m_end - 1) {
                m_current = m_start;
            } else if ((m_current & 15) == 0) {
                m_pred_scale = ReadAram8((m_current & ~15u) >> 1) & 0x7f;
                m_current += 2;
                step += 2;
            }
        } else {
            int16_t pcm = static_cast<int16_t>(raw);
            const uint16_t gain_scale = (m_format >> 4) & 3u;
            uint8_t gain_shift = gain_scale == 1 ? 0 : (gain_scale == 2 ? 16 : 11);
            const int coef_idx = (m_pred_scale >> 4) & 0x7;
            const int32_t coef1 = m_pb->adpcm.coefs[coef_idx * 2 + 0];
            const int32_t coef2 = m_pb->adpcm.coefs[coef_idx * 2 + 1];
            value = static_cast<int16_t>(((static_cast<int32_t>(m_gain) * pcm) >> gain_shift) +
                                         ((coef1 * m_yn1) >> gain_shift) +
                                         ((coef2 * m_yn2) >> gain_shift));
            m_yn2 = m_yn1;
            m_yn1 = value;
            if (decode != 1) {
                ++m_current;
            }
        }

        if (m_current == m_end + step - 1) {
            m_current = m_start;
            m_reads_stopped = true;
            if (m_pb->audio_addr.looping) {
                m_pred_scale = m_pb->adpcm_loop_info.pred_scale & 0x7f;
                if (m_pb->is_stream != 1) {
                    m_yn1 = static_cast<int16_t>(m_pb->adpcm_loop_info.yn1);
                    SetYn2(static_cast<int16_t>(m_pb->adpcm_loop_info.yn2));
                } else {
                    SetYn2(m_yn2);
                }
            } else {
                m_pb->running = 0;
            }
        }

        // Mirrors DSPAccelerator::SetCurrentAddress's CURRENT_ADDRESS_MASK: keep only the
        // 30-bit ARAM address plus the 0x80000000 write flag.
        m_current &= 0xbfffffffu;

        m_pb->audio_addr.cur_addr_hi = static_cast<uint16_t>(m_current >> 16);
        m_pb->audio_addr.cur_addr_lo = static_cast<uint16_t>(m_current);
        m_pb->adpcm.yn1 = m_yn1;
        m_pb->adpcm.yn2 = m_yn2;
        m_pb->adpcm.pred_scale = m_pred_scale;
        return value;
    }

    static uint8_t ReadAram8(uint32_t addr) {
        return ReadAramByte(addr);
    }

private:
    void SetYn2(int16_t yn2) {
        m_yn2 = yn2;
        m_reads_stopped = false;
    }

    uint16_t CurrentRawSample() const {
        const uint16_t size = m_format & 3u;
        if (size == 0) {
            uint8_t byte = ReadAram8(m_current >> 1);
            return (m_current & 1) ? (byte & 0xf) : (byte >> 4);
        }
        if (size == 1) {
            return ReadAram8(m_current);
        }
        if (size == 2) {
            return static_cast<uint16_t>((ReadAram8(m_current * 2) << 8) | ReadAram8(m_current * 2 + 1));
        }
        return 0;
    }

    AXPBWii* m_pb = nullptr;
    uint32_t m_start = 0;
    uint32_t m_end = 0;
    uint32_t m_current = 0;
    uint16_t m_format = 0;
    int16_t m_gain = 0;
    int16_t m_yn1 = 0;
    int16_t m_yn2 = 0;
    uint16_t m_pred_scale = 0;
    bool m_reads_stopped = false;
};

}
