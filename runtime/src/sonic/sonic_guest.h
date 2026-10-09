// sonic_guest.h - small helpers for the Sonic integration's game-side code:
// calling guest functions from a native hook, reading and writing big-endian
// guest data, and a little guest memory of our own for strings and objects we
// hand to the game.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>

#include "soniccore/core/math.h"

struct CpuContext;

namespace sonic_mkw {
namespace guest {

// Calls the guest function at `address` with integer arguments in r3.. and float
// arguments in f1.. The caller's registers are restored afterwards, so this is
// safe in the middle of a native hook. Returns r3 (and f1 through `f1Out`).
// NB: calling an address that is itself hooked re-enters the hook.
uint32_t Call(CpuContext* ctx, uint32_t address, std::initializer_list<uint32_t> gprs = {},
              std::initializer_list<double> fprs = {}, double* f1Out = nullptr);

bool Valid(uint32_t address, uint32_t size = 4);
uint8_t U8(uint32_t address);
uint16_t U16(uint32_t address);
uint32_t U32(uint32_t address);
int16_t S16(uint32_t address);
float F32(uint32_t address);
// A pointer-sized word that must point into guest RAM, or 0.
uint32_t Ptr(uint32_t address);
sonic::Vec3 Vec(uint32_t address);
void SetU8(uint32_t address, uint8_t value);
void SetU16(uint32_t address, uint16_t value);
void SetU32(uint32_t address, uint32_t value);
void SetF32(uint32_t address, float value);
void SetVec(uint32_t address, const sonic::Vec3& value);
// Copies a NUL-terminated guest string (at most `maxLength` bytes).
std::string String(uint32_t address, size_t maxLength = 256);

// Guest memory reserved for the Sonic integration (system_bridge.cpp). Bump
// allocated and never freed; returns 0 when it is exhausted or not reserved.
uint32_t Alloc(uint32_t size, uint32_t align = 8);
// A guest copy of `text`, made once per distinct string.
uint32_t InternString(const std::string& text);

}  // namespace guest
}  // namespace sonic_mkw
