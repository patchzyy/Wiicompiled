#include "ppc_runtime.h"
// ppc_runtime.h re-exports the rest of the ISA package; the CR tier is pulled in
// by abi_bridge.h for generated code, so this TU asks for it directly.
#include "isa/ppc_isa_cr.h"
#include "memory.h"
#include "runtime_log.h"
#include "timebase_contract.h"

#include <array>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <stdexcept>
#include <limits>


extern "C" void PPC_TrapWord(uint32_t trapOptions, uint32_t lhs, uint32_t rhs)
{
    const bool trap =
        ((trapOptions & 0x10u) != 0u && static_cast<int32_t>(lhs) < static_cast<int32_t>(rhs)) ||
        ((trapOptions & 0x08u) != 0u && static_cast<int32_t>(lhs) > static_cast<int32_t>(rhs)) ||
        ((trapOptions & 0x04u) != 0u && lhs == rhs) ||
        ((trapOptions & 0x02u) != 0u && lhs < rhs) ||
        ((trapOptions & 0x01u) != 0u && lhs > rhs);
    if (trap)
    {
        throw std::runtime_error("PowerPC trap condition fired");
    }
}

namespace {

constexpr uint32_t kBroadwayPvr = 0x00087200u; // Known PVR value for Wii's Broadway CPU.

// One warning per (SPR, direction). A bitset rather than bool arrays: an
// unhandled SPR is a real translation gap worth reporting once per SPR, but the
// dedup state has no business costing 4 KiB of .bss.
std::bitset<2048> g_warnedSprRead{};
std::bitset<2048> g_warnedSprWrite{};
std::array<uint32_t, 2048> g_sprShadow{};
uint32_t g_reservationAddr = 0;
bool g_hasReservation = false;
const auto g_timeBaseStart = std::chrono::steady_clock::now();

// Days from 1970-01-01 to the given civil date (Howard Hinnant's algorithm).
constexpr int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// The Wii RTC holds local time; the OS counts ticks from 2000-01-01.
uint64_t InitialTimeBase()
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    if (localtime_s(&local, &now) != 0) {
        return 0;
    }
#else
    if (localtime_r(&now, &local) == nullptr) {
        return 0;
    }
#endif
    const int64_t days = DaysFromCivil(local.tm_year + 1900, static_cast<unsigned>(local.tm_mon + 1),
                                       static_cast<unsigned>(local.tm_mday)) -
                         DaysFromCivil(2000, 1, 1);
    const int64_t seconds = days * 86400 + local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    if (seconds <= 0) {
        return 0;
    }
    return static_cast<uint64_t>(seconds) * TimeBaseContract::kTicksPerSecond;
}

const uint64_t g_timeBaseOrigin = InitialTimeBase();

uint64_t GetTimeBase() {
    const auto elapsed = std::chrono::steady_clock::now() - g_timeBaseStart;
    const auto nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return g_timeBaseOrigin + TimeBaseContract::NanosecondsToTicks(static_cast<uint64_t>(nanoseconds));
}

constexpr uint32_t kFpscrFx = 1u << 31;
constexpr uint32_t kFpscrFex = 1u << 30;
constexpr uint32_t kFpscrVx = 1u << 29;
constexpr uint32_t kFpscrOx = 1u << 28;
constexpr uint32_t kFpscrUx = 1u << 27;
constexpr uint32_t kFpscrZx = 1u << 26;
constexpr uint32_t kFpscrXx = 1u << 25;
constexpr uint32_t kFpscrVxSnan = 1u << 24;
constexpr uint32_t kFpscrVxIsi = 1u << 23;
constexpr uint32_t kFpscrVxIdi = 1u << 22;
constexpr uint32_t kFpscrVxZdz = 1u << 21;
constexpr uint32_t kFpscrVxImz = 1u << 20;
constexpr uint32_t kFpscrVxVc = 1u << 19;
constexpr uint32_t kFpscrVxSoft = 1u << 10;
constexpr uint32_t kFpscrVxSqrt = 1u << 9;
constexpr uint32_t kFpscrVxCvi = 1u << 8;
constexpr uint32_t kFpscrVe = 1u << 7;
constexpr uint32_t kFpscrOe = 1u << 6;
constexpr uint32_t kFpscrUe = 1u << 5;
constexpr uint32_t kFpscrZe = 1u << 4;
constexpr uint32_t kFpscrXe = 1u << 3;
constexpr uint32_t kFpscrVxAny =
    kFpscrVxSnan | kFpscrVxIsi | kFpscrVxIdi | kFpscrVxZdz | kFpscrVxImz |
    kFpscrVxVc | kFpscrVxSoft | kFpscrVxSqrt | kFpscrVxCvi;
constexpr uint32_t kFpscrAnyX = kFpscrOx | kFpscrUx | kFpscrZx | kFpscrXx | kFpscrVxAny;
constexpr uint32_t kFpscrAnyE = kFpscrVe | kFpscrOe | kFpscrUe | kFpscrZe | kFpscrXe;

template <size_t N>
inline void WarnOnce(std::bitset<N>& warned, uint32_t spr, const char* op)
{
    if (spr < N)
    {
        if (warned.test(spr))
        {
            return;
        }
        warned.set(spr);
    }
    RT_LOG(RT_TAG_RUNTIME) << "PPC_" << op << " unhandled SPR " << spr << " (defaulting to 0)" << std::endl;
}



inline void UpdateFpscrSummary(CpuContext* cpu)
{
    if (!cpu)
    {
        return;
    }

    if ((cpu->fpscr & kFpscrVxAny) != 0)
    {
        cpu->fpscr |= kFpscrVx;
    }
    else
    {
        cpu->fpscr &= ~kFpscrVx;
    }

    if (((cpu->fpscr >> 22) & (cpu->fpscr & kFpscrAnyE)) != 0)
    {
        cpu->fpscr |= kFpscrFex;
    }
    else
    {
        cpu->fpscr &= ~kFpscrFex;
    }
}

} // namespace

extern "C" uint32_t PPC_UpdateCarryAdd(uint32_t lhs, uint32_t rhs, uint32_t carryIn)
{
    const uint64_t sum = static_cast<uint64_t>(lhs) + static_cast<uint64_t>(rhs) + static_cast<uint64_t>(carryIn & 1u);
    const uint32_t carryOut = static_cast<uint32_t>((sum >> 32) & 1u);
    CpuContext* cpu = TryGetCpuContext();
    if (cpu)
    {
        cpu->xer = (cpu->xer & ~0x20000000u) | (carryOut << 29);
        return cpu->xer;
    }

    return carryOut << 29;
}

extern "C" uint32_t PPC_UpdateCarrySub(uint32_t lhs, uint32_t rhs)
{
    // Carry flag is set when no borrow occurs (i.e., lhs >= rhs).
    const uint32_t carryOut = lhs >= rhs ? 1u : 0u;
    CpuContext* cpu = TryGetCpuContext();
    if (cpu)
    {
        cpu->xer = (cpu->xer & ~0x20000000u) | (carryOut << 29);
        return cpu->xer;
    }

    return carryOut << 29;
}

extern "C" uint32_t PPC_UpdateCarryShiftRight(uint32_t value, uint32_t shift)
{
    uint32_t carryOut = 0;

    // Per Broadway docs: CA is set if value is NEGATIVE and any 1 bits shifted out
    const bool isNegative = (value & 0x80000000u) != 0;

    if ((shift & 0x20u) != 0)
    {
        carryOut = isNegative ? 1u : 0u;
    }
    else
    {
        const uint32_t sh = shift & 31u;
        if (sh != 0 && isNegative)
        {
            const uint32_t mask = (1u << sh) - 1u;
            carryOut = (value & mask) ? 1u : 0u;
        }
    }

    CpuContext* cpu = TryGetCpuContext();
    if (cpu)
    {
        cpu->xer = (cpu->xer & ~0x20000000u) | (carryOut << 29);
        return cpu->xer;
    }

    return carryOut << 29;
}

extern "C" uint32_t PPC_GetCarry()
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu)
    {
        return 0;
    }
    return (cpu->xer >> 29) & 1u;
}

static void UpdateOverflow(uint32_t result, bool overflow)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu)
    {
        return;
    }

    if (overflow)
    {
        cpu->xer |= 0xC0000000u; // SO and OV.
    }
    else
    {
        cpu->xer &= ~0x40000000u; // Clear OV, preserve SO.
    }
    (void)result;
}

static uint32_t AddWithXer(uint32_t lhs, uint32_t rhs, uint32_t carryIn, bool updateCarry)
{
    const uint64_t wide = static_cast<uint64_t>(lhs) + static_cast<uint64_t>(rhs) + static_cast<uint64_t>(carryIn & 1u);
    const uint32_t result = static_cast<uint32_t>(wide);
    const bool overflow = ((~(lhs ^ rhs) & (lhs ^ result)) & 0x80000000u) != 0;
    if (updateCarry)
    {
        PPC_UpdateCarryAdd(lhs, rhs, carryIn);
    }
    UpdateOverflow(result, overflow);
    return result;
}

static uint32_t SubfWithXer(uint32_t subtrahend, uint32_t minuend, uint32_t carryIn, bool extended, bool updateCarry)
{
    const uint32_t rhs = ~subtrahend;
    const uint64_t wide = static_cast<uint64_t>(rhs) + static_cast<uint64_t>(minuend) + static_cast<uint64_t>(extended ? (carryIn & 1u) : 1u);
    const uint32_t result = static_cast<uint32_t>(wide);
    const bool overflow = (((minuend ^ subtrahend) & (minuend ^ result)) & 0x80000000u) != 0;
    if (updateCarry)
    {
        CpuContext* cpu = TryGetCpuContext();
        const uint32_t carryOut = static_cast<uint32_t>((wide >> 32) & 1u);
        if (cpu)
        {
            cpu->xer = (cpu->xer & ~0x20000000u) | (carryOut << 29);
        }
    }
    UpdateOverflow(result, overflow);
    return result;
}

extern "C" uint32_t PPC_Addo(uint32_t lhs, uint32_t rhs) { return AddWithXer(lhs, rhs, 0, false); }
extern "C" uint32_t PPC_Addco(uint32_t lhs, uint32_t rhs) { return AddWithXer(lhs, rhs, 0, true); }
extern "C" uint32_t PPC_Addeo(uint32_t lhs, uint32_t rhs) { return AddWithXer(lhs, rhs, PPC_GetCarry(), true); }
extern "C" uint32_t PPC_Addmeo(uint32_t value) { return AddWithXer(value, 0xFFFFFFFFu, PPC_GetCarry(), true); }
extern "C" uint32_t PPC_Addzeo(uint32_t value) { return AddWithXer(value, 0, PPC_GetCarry(), true); }
extern "C" uint32_t PPC_Subfo(uint32_t subtrahend, uint32_t minuend) { return SubfWithXer(subtrahend, minuend, 0, false, false); }
extern "C" uint32_t PPC_Subfco(uint32_t subtrahend, uint32_t minuend) { return SubfWithXer(subtrahend, minuend, 0, false, true); }
extern "C" uint32_t PPC_Subfeo(uint32_t subtrahend, uint32_t minuend) { return SubfWithXer(subtrahend, minuend, PPC_GetCarry(), true, true); }
extern "C" uint32_t PPC_Subfmeo(uint32_t value) { return SubfWithXer(value, 0xFFFFFFFFu, PPC_GetCarry(), true, true); }
extern "C" uint32_t PPC_Subfzeo(uint32_t value) { return SubfWithXer(value, 0, PPC_GetCarry(), true, true); }
extern "C" uint32_t PPC_Nego(uint32_t value) { return SubfWithXer(value, 0, 0, false, false); }

extern "C" uint32_t PPC_Mullwo(uint32_t lhs, uint32_t rhs)
{
    const int64_t product = static_cast<int64_t>(static_cast<int32_t>(lhs)) * static_cast<int64_t>(static_cast<int32_t>(rhs));
    const uint32_t result = static_cast<uint32_t>(product);
    UpdateOverflow(result, product < std::numeric_limits<int32_t>::min() || product > std::numeric_limits<int32_t>::max());
    return result;
}

extern "C" uint32_t PPC_Divwo(uint32_t lhs, uint32_t rhs)
{
    const int32_t dividend = static_cast<int32_t>(lhs);
    const int32_t divisor = static_cast<int32_t>(rhs);
    if (divisor == 0 || (dividend == std::numeric_limits<int32_t>::min() && divisor == -1))
    {
        const uint32_t result = dividend < 0 ? 0xFFFFFFFFu : 0u;
        UpdateOverflow(result, true);
        return result;
    }
    const uint32_t result = static_cast<uint32_t>(dividend / divisor);
    UpdateOverflow(result, false);
    return result;
}

extern "C" uint32_t PPC_Divwuo(uint32_t lhs, uint32_t rhs)
{
    if (rhs == 0)
    {
        UpdateOverflow(0, true);
        return 0;
    }
    const uint32_t result = lhs / rhs;
    UpdateOverflow(result, false);
    return result;
}

static void LoadString(uint32_t rD, uint32_t addr, uint32_t byteCount)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu)
    {
        return;
    }

    uint32_t reg = rD & 31u;
    uint32_t current = 0;
    for (uint32_t i = 0; i < byteCount; ++i)
    {
        current = (current << 8) | Memory::Read8(addr + i);
        if ((i & 3u) == 3u)
        {
            cpu->gpr[reg] = current;
            reg = (reg + 1u) & 31u;
            current = 0;
        }
    }

    const uint32_t partial = byteCount & 3u;
    if (partial != 0)
    {
        cpu->gpr[reg] = current << ((4u - partial) * 8u);
    }
}

static void StoreString(uint32_t rS, uint32_t addr, uint32_t byteCount)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu)
    {
        return;
    }

    uint32_t reg = rS & 31u;
    for (uint32_t i = 0; i < byteCount; ++i)
    {
        const uint32_t shift = 24u - ((i & 3u) * 8u);
        Memory::Write8(addr + i, static_cast<uint8_t>(cpu->gpr[reg] >> shift));
        if ((i & 3u) == 3u)
        {
            reg = (reg + 1u) & 31u;
        }
    }
}

extern "C" void PPC_Lswi(uint32_t rD, uint32_t addr, uint32_t byteCount) { LoadString(rD, addr, byteCount == 0 ? 32u : byteCount); }
extern "C" void PPC_Lswx(uint32_t rD, uint32_t addr)
{
    CpuContext* cpu = TryGetCpuContext();
    LoadString(rD, addr, cpu ? (cpu->xer & 0x7Fu) : 0u);
}
extern "C" void PPC_Stswi(uint32_t rS, uint32_t addr, uint32_t byteCount) { StoreString(rS, addr, byteCount == 0 ? 32u : byteCount); }
extern "C" void PPC_Stswx(uint32_t rS, uint32_t addr)
{
    CpuContext* cpu = TryGetCpuContext();
    StoreString(rS, addr, cpu ? (cpu->xer & 0x7Fu) : 0u);
}

extern "C" uint32_t PPC_Lwarx(uint32_t addr)
{
    g_reservationAddr = addr;
    g_hasReservation = true;
    return Memory::Read32(addr);
}

extern "C" uint32_t PPC_Stwcx(uint32_t addr, uint32_t value)
{
    CpuContext* cpu = TryGetCpuContext();
    const bool success = g_hasReservation && g_reservationAddr == addr;
    g_hasReservation = false;
    if (success)
    {
        Memory::Write32(addr, value);
    }
    if (cpu)
    {
        const uint32_t so = (cpu->xer >> 31) & 1u;
        const uint32_t cr0 = (success ? 0x2u : 0u) | so;
        cpu->cr = (cpu->cr & 0x0FFFFFFFu) | (cr0 << 28);
    }
    return success ? 1u : 0u;
}

extern "C" uint32_t PPC_Mcrfs(uint32_t dstField, uint32_t srcField)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu)
    {
        return 0;
    }

    const uint32_t dst = dstField & 7u;
    const uint32_t src = srcField & 7u;
    const uint32_t value = (cpu->fpscr >> ((7u - src) * 4u)) & 0xFu;
    cpu->cr = (cpu->cr & ~(0xFu << ((7u - dst) * 4u))) | (value << ((7u - dst) * 4u));
    cpu->fpscr &= ~((0xFu << ((7u - src) * 4u)) & (kFpscrFx | kFpscrAnyX));
    UpdateFpscrSummary(cpu);
    return cpu->cr;
}

extern "C" uint32_t PPC_Eciwx(uint32_t addr)
{
    (void)addr;
    throw std::runtime_error("eciwx attempted external control/MMIO read");
}

extern "C" uint32_t PPC_Mftb()
{
    return static_cast<uint32_t>(GetTimeBase());
}

extern "C" uint32_t PPC_Mftbu()
{
    return static_cast<uint32_t>(GetTimeBase() >> 32);
}

extern "C" uint32_t OSSystemCall()
{
    return 0;
}

extern "C" void PPC_Ecowx(uint32_t addr, uint32_t value)
{
    (void)addr;
    (void)value;
    throw std::runtime_error("ecowx attempted external control/MMIO write");
}

extern "C" uint32_t PPC_Mcrxr(uint32_t crField)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu)
    {
        return 0;
    }

    const uint32_t field = crField & 7u;
    const uint32_t value = (cpu->xer >> 28) & 0xFu;
    const uint32_t shift = (7u - field) * 4u;
    cpu->cr = (cpu->cr & ~(0xFu << shift)) | (value << shift);
    cpu->xer &= ~0xF0000000u;
    return cpu->cr;
}

extern "C" uint32_t PPC_ReadSpr(uint32_t spr)
{
    CpuContext* cpu = TryGetCpuContext();

    // Preserve previously written values for unknown SPRs so that callers see what they set.
    auto shadowValue = [&]() -> uint32_t
    {
        if (spr < g_sprShadow.size())
        {
            return g_sprShadow[spr];
        }
        return 0u;
    };

    if (spr >= 912u && spr <= 919u)
    {
        return cpu ? cpu->gqr[spr - 912u] : 0u;
    }

    switch (spr)
    {
        case 8: return cpu ? cpu->lr : 0u;
        case 9: return cpu ? cpu->ctr : 0u;
        case 1: return cpu ? cpu->xer : 0u;
        case 22: return shadowValue(); // DEC - emulate as latched value
        case 26: return cpu ? cpu->srr0 : 0u;
        case 27: return cpu ? cpu->srr1 : 0u;
        case 268: return PPC_Mftb();
        case 269: return PPC_Mftbu();
        case 287: return kBroadwayPvr; // PVR
        case 920: return cpu ? cpu->hid2 : 0u;
        case 1008: return cpu ? cpu->hid0 : 0u;
        case 1009: return cpu ? cpu->hid1 : 0u;
        case 1011: return shadowValue(); // spr3f3 in TRK save block
        case 1017: return shadowValue(); // DMA/L2 ancillary SPRs (preserve writes)
        case 952:  // PM registers - preserve writes
        case 953:
        case 954:
        case 956:
        case 957:
        case 958:
            return shadowValue();
        default:
            WarnOnce(g_warnedSprRead, spr, "ReadSpr");
            return shadowValue();
    }
}

extern "C" void PPC_WriteSpr(uint32_t spr, uint32_t value)
{
    CpuContext* cpu = TryGetCpuContext();

    if (spr >= 912u && spr <= 919u)
    {
        if (cpu)
        {
            cpu->gqr[spr - 912u] = value;
        }
        return;
    }

    switch (spr)
    {
        case 8: if (cpu) cpu->lr = value; return;
        case 9: if (cpu) cpu->ctr = value; return;
        case 1: if (cpu) cpu->xer = value; return;
        case 22: break; // DEC - fallthrough to shadow
        case 26: if (cpu) cpu->srr0 = value; return;
        case 27: if (cpu) cpu->srr1 = value; return;
        case 920: if (cpu) cpu->hid2 = value; return;
        case 1008: if (cpu) cpu->hid0 = value; return;
        case 1009: if (cpu) cpu->hid1 = value; return;
        case 1011: break;
        case 1017: break;
        case 952:  // PM registers - preserve values
        case 953:
        case 954:
        case 956:
        case 957:
        case 958:
            break;
        default:
            WarnOnce(g_warnedSprWrite, spr, "WriteSpr");
            break;
    }

    if (spr < g_sprShadow.size())
    {
        g_sprShadow[spr] = value;
    }
}

extern "C" uint32_t PPC_Cntlzw(uint32_t value)
{
    // cntlzw returns 32 when the input is zero; __builtin_clz is undefined in that case.
    if (value == 0)
    {
        return 32u;
    }
    return static_cast<uint32_t>(__builtin_clz(value));
}

// The CR semantics themselves live in isa/ppc_isa_cr.h, which is what generated
// code calls (Ppc*Resident on a register-resident CR). These out-of-line forms
// exist only for the !hasDest lifter path and must not re-implement them.
extern "C" uint32_t PPC_CrSetBit(uint32_t bitIndex, uint32_t value)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu)
    {
        return 0;
    }

    cpu->cr = PpcCrSetBitResident(cpu->cr, bitIndex, value);
    return cpu->cr;
}

extern "C" uint32_t PPC_CrLogical(uint32_t op, uint32_t bt, uint32_t ba, uint32_t bb)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu)
    {
        return 0;
    }

    cpu->cr = PpcCrLogicalResident(cpu->cr, op, bt, ba, bb);
    return cpu->cr;
}

extern "C" uint32_t PPC_Mcrf(uint32_t dstField, uint32_t srcField)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu)
    {
        return 0;
    }

    cpu->cr = PpcMcrfResident(cpu->cr, dstField, srcField);
    return cpu->cr;
}

extern "C" double PPC_PsAdd(double lhs, double rhs)
{
    return PPC_PsAddInline(lhs, rhs);
}

extern "C" double PPC_PsSub(double lhs, double rhs)
{
    return PPC_PsSubInline(lhs, rhs);
}

extern "C" double PPC_PsDiv(double lhs, double rhs)
{
    return PPC_PsDivInline(lhs, rhs);
}

extern "C" double PPC_PsNeg(double value)
{
    return PPC_PsNegInline(value);
}

extern "C" double PPC_PsMul(double lhs, double rhs)
{
    return PPC_PsMulInline(lhs, rhs);
}

extern "C" double PPC_PsMsub(double multiplicand, double multiplier, double subtractor)
{
    return PPC_PsMsubInline(multiplicand, multiplier, subtractor);
}

extern "C" double PPC_PsMadd(double multiplicand, double multiplier, double addend)
{
    return PPC_PsMaddInline(multiplicand, multiplier, addend);
}

// ps_madds0: frD[ps0] = frA[ps0] * frC[ps0] + frB[ps0], frD[ps1] = frA[ps1] * frC[ps0] + frB[ps1]
// Uses frC[ps0] as a scalar multiplier for both lanes
extern "C" double PPC_PsMadds0(double multiplicand, double multiplier, double addend)
{
    return PPC_PsMadds0Inline(multiplicand, multiplier, addend);
}

// ps_madds1: frD[ps0] = frA[ps0] * frC[ps1] + frB[ps0], frD[ps1] = frA[ps1] * frC[ps1] + frB[ps1]
// Uses frC[ps1] as a scalar multiplier for both lanes
extern "C" double PPC_PsMadds1(double multiplicand, double multiplier, double addend)
{
    return PPC_PsMadds1Inline(multiplicand, multiplier, addend);
}

extern "C" double PPC_PsNmsub(double multiplicand, double multiplier, double subtractor)
{
    return PPC_PsNmsubInline(multiplicand, multiplier, subtractor);
}

extern "C" double PPC_PsNmadd(double multiplicand, double multiplier, double addend)
{
    return PPC_PsNmaddInline(multiplicand, multiplier, addend);
}

extern "C" double PPC_PsSel(double lhs, double control, double rhs)
{
    // Dolphin models fsel/ps_sel as "fra >= -0.0 ? frC : frB".
    // That comparison deliberately sends unordered/NaN controls to frB.
    return PPC_PsSelInline(lhs, control, rhs);
}

// PPC_PsRes is defined extern "C" inline in ppc_runtime.h (hot in THP dequant).

extern "C" double PPC_PsRsqrte(double value)
{
    return PpcPackPairedInline(
        static_cast<float>(PpcApproximateReciprocalSquareRootInline(
            static_cast<double>(PpcGetPs0Inline(value)))),
        static_cast<float>(PpcApproximateReciprocalSquareRootInline(
            static_cast<double>(PpcGetPs1Inline(value)))));
}

extern "C" double PPC_PsFromScalar(double value)
{
    return PPC_PsFromScalarInline(value);
}

extern "C" double PPC_PsToScalar(double value)
{
    return PPC_PsToScalarInline(value);
}

extern "C" double PPC_PsMerge00(double a, double b)
{
    return PPC_PsMerge00Inline(a, b);
}

extern "C" double PPC_PsMerge01(double a, double b)
{
    return PPC_PsMerge01Inline(a, b);
}

extern "C" double PPC_PsMerge10(double a, double b)
{
    return PPC_PsMerge10Inline(a, b);
}

extern "C" double PPC_PsMerge11(double a, double b)
{
    return PPC_PsMerge11Inline(a, b);
}

extern "C" double PPC_PsSum0(double a, double b, double c)
{
    return PPC_PsSum0Inline(a, b, c);
}

extern "C" double PPC_PsSum1(double a, double b, double c)
{
    return PPC_PsSum1Inline(a, b, c);
}

extern "C" double PPC_PsMuls0(double a, double c)
{
    return PPC_PsMuls0Inline(a, c);
}

extern "C" double PPC_PsMuls1(double a, double c)
{
    return PPC_PsMuls1Inline(a, c);
}

extern "C" double PPC_PsAbs(double value)
{
    return PPC_PsAbsInline(value);
}

extern "C" double PPC_Fsqrt(double value)
{
    return std::sqrt(value);
}

// PPC_Fres, PPC_Frsqrte and PPC_Fsel are now defined inline in ppc_runtime.h:
// they are emitted at ~1,100 translated sites between them and an out-of-line
// definition made each one a full caller-saved register barrier (there is no
// LTO in this build). The bodies moved verbatim; ApproximateReciprocalSquareRoot
// below is retained because PPC_PsRsqrte still uses it.

extern "C" double PPC_Fmadd(double multiplicand, double multiplier, double addend)
{
    return PpcFmaddInline(multiplicand, multiplier, addend);
}

extern "C" double PPC_Fmsub(double multiplicand, double multiplier, double subtractor)
{
    return PpcFmsubInline(multiplicand, multiplier, subtractor);
}

extern "C" double PPC_Fnmadd(double multiplicand, double multiplier, double addend)
{
    return PpcFnmaddInline(multiplicand, multiplier, addend);
}

extern "C" double PPC_Fnmsub(double multiplicand, double multiplier, double subtractor)
{
    return PpcFnmsubInline(multiplicand, multiplier, subtractor);
}

// Single-precision scalar helpers.
// PowerPC "single" ops perform the arithmetic in full precision and then round
// the result to single-precision. Converting operands to float first can lose
// critical low bits (e.g., integer->float via 0x4330 trick), yielding zeros.
extern "C" double PPC_Fadds(double a, double b)
{
    const float res = PpcForceSingleValueInline(a + b);
    return static_cast<double>(res);
}

extern "C" double PPC_Fsubs(double a, double b)
{
    const float res = PpcForceSingleValueInline(a - b);
    return static_cast<double>(res);
}

extern "C" double PPC_Fmuls(double a, double b)
{
    return PpcFmulsInline(a, b);
}

extern "C" double PPC_Fdivs(double a, double b)
{
    const float res = PpcForceSingleValueInline(a / b);
    return static_cast<double>(res);
}

// PPC_Fmadds / PPC_Fmsubs / PPC_Fnmadds / PPC_Fnmsubs are defined inline in
// ppc_runtime.h for the same reason as PPC_Fsel above: the out-of-line wrapper
// around the already-inline body was itself the register barrier.

extern "C" uint32_t PPC_LoadWordByteReverse(uint32_t addr)
{
    // lwbrx: Load Word Byte-Reverse Indexed
    // Loads a 32-bit word from memory and byte-swaps it
    uint32_t value = Memory::Read32(addr);
    
    // Byte-swap: ABCD -> DCBA
    return ((value >> 24) & 0x000000FF) |
           ((value >> 8)  & 0x0000FF00) |
           ((value << 8)  & 0x00FF0000) |
           ((value << 24) & 0xFF000000);
}

extern "C" void PPC_StoreWordByteReverse(uint32_t addr, uint32_t value)
{
    // stwbrx: Store Word Byte-Reverse Indexed
    // Byte-swaps a 32-bit value and stores it to memory
    // Byte-swap: ABCD -> DCBA
    uint32_t swapped = ((value >> 24) & 0x000000FF) |
                       ((value >> 8)  & 0x0000FF00) |
                       ((value << 8)  & 0x00FF0000) |
                       ((value << 24) & 0xFF000000);
    
    Memory::Write32(addr, swapped);
}

extern "C" uint32_t PPC_LoadHalfwordByteReverse(uint32_t addr)
{
    // lhbrx: Load Halfword Byte-Reverse Indexed
    // Loads a 16-bit halfword from memory and byte-swaps it, zero-extending to 32 bits
    uint16_t value = Memory::Read16(addr);
    
    // Byte-swap: AB -> BA
    uint16_t swapped = ((value >> 8) & 0x00FF) |
                       ((value << 8) & 0xFF00);
    return static_cast<uint32_t>(swapped);
}

extern "C" void PPC_StoreHalfwordByteReverse(uint32_t addr, uint32_t value)
{
    // sthbrx: Store Halfword Byte-Reverse Indexed
    // Byte-swaps the lower 16 bits of the value and stores it to memory
    uint16_t halfword = static_cast<uint16_t>(value & 0xFFFF);
    
    // Byte-swap: AB -> BA
    uint16_t swapped = ((halfword >> 8) & 0x00FF) |
                       ((halfword << 8) & 0xFF00);
    Memory::Write16(addr, swapped);
}

// psq_l / psq_st with a runtime GQR index. The lifter only reaches these when
// w/i are non-constant, which never happens in this DOL - every emitted site is
// rewritten to the isa templates in ppc_isa_quantized.h. The bodies below defer
// to the same isa runtime-GQR dispatch so the two can never drift.
extern "C" double PPC_PsqL(uint32_t addr, uint32_t w, uint32_t i)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu) {
        std::abort();  // CpuContext must never be null in PPC_PsqL
    }

    const uint32_t gqr = cpu->gqr[i & 7];
    return w == 0 ? PPC_PsqLStateFallback<0u, false>(gqr, addr)
                  : PPC_PsqLStateFallback<1u, false>(gqr, addr);
}

extern "C" void PPC_PsqSt(uint32_t addr, double value, uint32_t w, uint32_t i)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu)
    {
        std::abort();
    }

    const uint32_t gqr = cpu->gqr[i & 7];
    if (w == 0)
    {
        PPC_PsqStStateFallback<0u, false>(gqr, addr, value);
    }
    else
    {
        PPC_PsqStStateFallback<1u, false>(gqr, addr, value);
    }
}

extern "C" int32_t memset_zero_32(int32_t address)
{
    // dcbz zeros a 32-byte block.
    // The translator aligns the address to 32 bytes before calling this.
    try {
        // Try fast path: get direct pointer to memory
        uint8_t* ptr = Memory::GetPointer(static_cast<uint32_t>(address), 32);
        std::memset(ptr, 0, 32);
    } catch (...) {
        // Fallback or MMIO (rare for dcbz): write using scalar helpers
        uint32_t addr = static_cast<uint32_t>(address);
        for (int i = 0; i < 8; ++i) {
             Memory::Write32(addr + i * 4, 0);
        }
    }
    return 0;
}

// Floating-point comparison (fcmpu/fcmpo)
// Sets the specified CR field based on comparing two double values.
extern "C" void PPC_Fcmp(uint32_t crField, double a, double b)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu) {
        return;
    }
    SetCRFloat(cpu, crField & 7, a, b);
}

// ps_cmpo and ps_cmpu differ only in which FPSCR invalid-operation bit a NaN
// operand raises (VXVC vs VXSNAN). This runtime never models FPSCR exception
// bits from a compare - SetCRFloat writes the CR field and nothing else - so the
// ordered and unordered forms are deliberately the same body. If FPSCR exception
// state ever becomes observable, these four need to diverge.
extern "C" void PPC_PsCmpo0(uint32_t crField, double a, double b)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu) {
        return;
    }
    SetCRFloat(cpu, crField & 7, PpcGetPs0Inline(a), PpcGetPs0Inline(b));
}

extern "C" void PPC_PsCmpu0(uint32_t crField, double a, double b)
{
    PPC_PsCmpo0(crField, a, b);
}

extern "C" void PPC_PsCmpo1(uint32_t crField, double a, double b)
{
    CpuContext* cpu = TryGetCpuContext();
    if (!cpu) {
        return;
    }
    SetCRFloat(cpu, crField & 7, PpcGetPs1Inline(a), PpcGetPs1Inline(b));
}

extern "C" void PPC_PsCmpu1(uint32_t crField, double a, double b)
{
    PPC_PsCmpo1(crField, a, b);
}

extern "C" double PPC_PsNabs(double value)
{
    // No isa twin: ps_nabs is never emitted by this DOL, so it stays out-of-line
    // here, but built from the same lane accessors as its neighbours.
    return PpcPackPairedInline(-std::abs(PpcGetPs0Inline(value)),
                               -std::abs(PpcGetPs1Inline(value)));
}

extern "C" double PPC_PsMr(double value)
{
    return value;
}
