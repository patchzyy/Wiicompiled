// SPDX-License-Identifier: GPL-3.0-only
#include "wiimote_native_protocol.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>

using Wiimote::Native::Protocol;
using Wiimote::Native::Report;
namespace {
int failures = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { \
    std::fprintf(stderr, "%d: %s\n", __LINE__, #__VA_ARGS__); ++failures; } } while (0)
bool Output(Protocol& p, std::initializer_list<uint8_t> bytes) {
    const Report data(bytes);
    return p.Output(data.data(), data.size());
}
Report Reply(Protocol& p) {
    Report report;
    CHECK(p.PopReply(report));
    return report;
}
void TestInput() {
    Protocol p;
    Report r;
    CHECK(p.Sample(r) && r == Report({0x30, 0, 0}));
    CHECK(!p.Sample(r));
    p.SetButtons(0xFFFF);
    CHECK(p.Sample(r) && r == Report({0x30, 0x1F, 0x9F}));
    p.SetButtons(0x0080); // HOME must survive wire masks and overlay handling
    CHECK(Output(p, {0x12, 2, 0x31}));
    CHECK(Reply(p) == Report({0x22, 0, 0x80, 0x12, 0}));
    p.SetAccel(3, 2, 106); // raw 515, 514, 618; Y/Z have one unavailable bit
    CHECK(p.Sample(r) && r == Report({0x31, 0x60, 0xE0, 128, 128, 154}));
    CHECK(!p.Sample(r));
    p.SetAccel(std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max(), -512);
    CHECK(p.Sample(r) && r[3] == 0 && r[4] == 255 && r[5] == 0);
    for (uint8_t mode = 0x30; mode <= 0x37; ++mode) {
        CHECK(Output(p, {0x12, 4, mode}));
        CHECK(p.Sample(r));
        const size_t sizes[] = {3, 6, 11, 18, 22, 22, 22, 22};
        CHECK(r.size() == sizes[mode - 0x30] && r[0] == mode);
        CHECK(p.Sample(r)); // continuous bit must repeat unchanged input
        if (mode == 0x33 || mode == 0x35 || mode == 0x37)
            CHECK(std::all_of(r.begin() + 6, r.end(), [](uint8_t b) { return b == 0xFF; }));
    }
    CHECK(!Output(p, {0x12, 2, 0x3E}));
    CHECK(Reply(p) == Report({0x22, 0, 0x80, 0x12, 3}));
    CHECK(p.Mode() == 0x37);
}
void TestCommandsAndMemory() {
    Protocol p;
    p.SetBattery(50);
    CHECK(Output(p, {0x11, 0xA3}));
    CHECK(p.Rumble() && p.Leds() == 0xA0);
    CHECK(Reply(p) == Report({0x22, 0, 0, 0x11, 0}));
    CHECK(Output(p, {0x15, 0}));
    CHECK(!p.Rumble());
    CHECK(Reply(p) == Report({0x20, 0, 0, 0xA0, 0, 0, 100}));
    CHECK(Output(p, {0x11, 0xF2}));
    CHECK(Reply(p) == Report({0x22, 0, 0, 0x11, 0}));
    CHECK(p.Leds() == 0xF0);
    CHECK(Output(p, {0x17, 0, 0, 0, 0x16, 0, 20}));
    auto r = Reply(p);
    CHECK(r.size() == 22 && r[3] == 0xF0 && r[5] == 0x16);
    CHECK(r[6] == 128 && r[10] == 154);
    unsigned checksum = 0x55;
    for (unsigned i = 6; i < 15; ++i) checksum += r[i];
    CHECK(r[15] == (checksum & 255));
    auto tail = Reply(p);
    CHECK(tail.size() == 22 && tail[3] == 0x30 && tail[5] == 0x26);
    CHECK(tail[6] == 154 && tail[9] == r[15]);
    // Volatile EEPROM writes and virtual speaker/IR config never touch hardware.
    CHECK(Output(p, {0x16, 0, 0, 1, 0, 2, 0xAA, 0x55}));
    CHECK(Reply(p)[4] == 0);
    CHECK(Output(p, {0x17, 0, 0, 1, 0, 0, 2}));
    r = Reply(p);
    CHECK(r[4] == 1 && r[5] == 0 && r[6] == 0xAA && r[7] == 0x55);
    CHECK(Output(p, {0x16, 4, 0xB0, 0, 0x30, 1, 8}));
    CHECK(Reply(p)[4] == 0);
    CHECK(Output(p, {0x17, 6, 0xB1, 0, 0x30, 0, 1}));
    CHECK(Reply(p)[6] == 8);
    CHECK(Output(p, {0x17, 8, 0xB0, 0, 0x30, 0, 1}));
    CHECK(Reply(p)[6] == 8); // alternate I2C address space
    for (uint8_t bus : {0xA4, 0xA6}) {
        CHECK(!Output(p, {0x16, 4, bus, 0, 0xF0, 1, 0x55}));
        CHECK(Reply(p)[4] == 7);
        CHECK(!Output(p, {0x17, 4, bus, 0, 0xFA, 0, 6}));
        r = Reply(p);
        CHECK(r[0] == 0x21 && r[3] == 0xF7 && r[5] == 0xFA);
    }
    CHECK(Output(p, {0x15, 0}));
    CHECK((Reply(p)[3] & 2) == 0); // never claim a physical extension is emulated
    CHECK(!Output(p, {0x17, 0, 0, 0x16, 0xFF, 0, 2}));
    CHECK(Reply(p)[3] == 0xF8);
    CHECK(!Output(p, {0x17, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF}));
    CHECK(Reply(p)[3] == 0xF8);
    CHECK(!Output(p, {0x16, 0, 0, 0, 0, 16, 0})); // truncated payload
    CHECK(!Output(p, {0x17, 0, 0, 0, 0, 0, 0}));
    CHECK(!Output(p, {0x12, 0}));
    CHECK(!p.Output(nullptr, 0));
}
void TestBounds() {
    Protocol p;
    for (unsigned i = 0; i < 513; ++i) Output(p, {0x15, 0});
    CHECK(!p.Healthy());
    Report r;
    unsigned replies = 0;
    while (p.PopReply(r)) ++replies;
    CHECK(replies == 512);
    // Deterministic malformed-input exercise, useful with ASan/UBSan.
    uint32_t seed = 0x9182;
    for (unsigned n = 0; n < 20000; ++n) {
        Protocol fresh;
        Report bytes(n % 24);
        for (auto& b : bytes) { seed = seed * 1664525 + 1013904223; b = seed >> 24; }
        fresh.Output(bytes.data(), bytes.size());
        while (fresh.PopReply(r)) CHECK(r.size() >= 5 && r.size() <= 22);
        fresh.Sample(r);
    }
}
}
int main() {
    TestInput();
    TestCommandsAndMemory();
    TestBounds();
    if (failures) return EXIT_FAILURE;
    std::puts("native Wii protocol tests passed");
}
