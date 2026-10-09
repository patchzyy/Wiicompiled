// sonic_guest.cpp - see sonic_guest.h.
#include "sonic/sonic_guest.h"

#include "abi_bridge.h"
#include "memory.h"
#include "ppc_runtime.h"

#include <cstring>
#include <map>
#include <mutex>

extern "C" uint32_t g_sonicScratchBase;
extern "C" uint32_t g_sonicScratchSize;

namespace sonic_mkw {
namespace guest {

namespace guest_detail {
std::mutex g_allocMutex;
uint32_t g_allocUsed = 0;
std::map<std::string, uint32_t> g_strings;
}  // namespace guest_detail

uint32_t Call(CpuContext* ctx, uint32_t address, std::initializer_list<uint32_t> gprs,
              std::initializer_list<double> fprs, double* f1Out) {
    const CpuContext saved = *ctx;
    int r = 3;
    for (uint32_t v : gprs) {
        if (r > 10) break;
        ctx->gpr[r++] = v;
    }
    int f = 1;
    for (double v : fprs) {
        if (f > 8) break;
        ctx->fpr[f++].d = v;
    }
    InvokeIndirectCpu(address, ctx);
    const uint32_t result = ctx->gpr[3];
    if (f1Out) *f1Out = ctx->fpr[1].d;
    *ctx = saved;
    return result;
}

bool Valid(uint32_t address, uint32_t size) { return address != 0 && Memory::Contains(address, size); }

uint8_t U8(uint32_t address) { return Valid(address, 1) ? Memory::Read8(address) : 0; }
uint16_t U16(uint32_t address) { return Valid(address, 2) ? Memory::Read16(address) : 0; }
uint32_t U32(uint32_t address) { return Valid(address, 4) ? Memory::Read32(address) : 0; }
int16_t S16(uint32_t address) { return int16_t(U16(address)); }

float F32(uint32_t address) {
    const uint32_t bits = U32(address);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

uint32_t Ptr(uint32_t address) {
    const uint32_t value = U32(address);
    return Valid(value, 4) ? value : 0;
}

sonic::Vec3 Vec(uint32_t address) { return sonic::Vec3(F32(address), F32(address + 4), F32(address + 8)); }

void SetU8(uint32_t address, uint8_t value) {
    if (Valid(address, 1)) Memory::Write8(address, value);
}
void SetU16(uint32_t address, uint16_t value) {
    if (Valid(address, 2)) Memory::Write16(address, value);
}
void SetU32(uint32_t address, uint32_t value) {
    if (Valid(address, 4)) Memory::Write32(address, value);
}
void SetF32(uint32_t address, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    SetU32(address, bits);
}
void SetVec(uint32_t address, const sonic::Vec3& value) {
    SetF32(address, value.x);
    SetF32(address + 4, value.y);
    SetF32(address + 8, value.z);
}

std::string String(uint32_t address, size_t maxLength) {
    std::string out;
    for (size_t i = 0; i < maxLength && Valid(uint32_t(address + i), 1); ++i) {
        const char c = char(Memory::Read8(uint32_t(address + i)));
        if (!c) break;
        out.push_back(c);
    }
    return out;
}

uint32_t Alloc(uint32_t size, uint32_t align) {
    using namespace guest_detail;
    std::lock_guard<std::mutex> lock(g_allocMutex);
    if (!g_sonicScratchBase || !align) return 0;
    const uint32_t start = (g_allocUsed + align - 1) / align * align;
    if (start + size > g_sonicScratchSize) return 0;
    g_allocUsed = start + size;
    const uint32_t address = g_sonicScratchBase + start;
    for (uint32_t i = 0; i < size; ++i) Memory::Write8(address + i, 0);
    return address;
}

uint32_t InternString(const std::string& text) {
    using namespace guest_detail;
    {
        std::lock_guard<std::mutex> lock(g_allocMutex);
        auto it = g_strings.find(text);
        if (it != g_strings.end()) return it->second;
    }
    const uint32_t address = Alloc(uint32_t(text.size() + 1), 4);
    if (!address) return 0;
    for (size_t i = 0; i < text.size(); ++i) Memory::Write8(uint32_t(address + i), uint8_t(text[i]));
    std::lock_guard<std::mutex> lock(g_allocMutex);
    g_strings[text] = address;
    return address;
}

}  // namespace guest
}  // namespace sonic_mkw
