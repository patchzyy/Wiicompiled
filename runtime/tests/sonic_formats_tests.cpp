// Tests for runtime/src/sonic/sonic_formats.cpp on synthetic data (no game files).
#include "sonic/sonic_formats.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

using namespace sonic_mkw;

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static void TestYaz0() {
    std::mt19937 rng(7);
    for (int t = 0; t < 12; ++t) {
        Bytes data(size_t(rng() % 70000) + (t == 0 ? 0 : 1));
        for (size_t i = 0; i < data.size(); ++i) {
            data[i] = (t % 3 == 0) ? uint8_t(rng()) : uint8_t((i / 7) % 5 + (rng() % 9 == 0 ? rng() : 0));
        }
        const Bytes packed = Yaz0Encode(data.data(), data.size());
        Bytes unpacked;
        CHECK(IsYaz0(packed.data(), packed.size()));
        CHECK(Yaz0Decode(packed.data(), packed.size(), unpacked));
        CHECK(unpacked == data);
        if (t % 3 != 0 && data.size() > 1000) CHECK(packed.size() < data.size());
    }
}

static Bytes MakeBmg() {
    // INF1 (3 entries of 8 bytes), DAT1 (UTF-16BE pool), MID1 (ids 9000, 9007, 9008)
    auto section = [](const char* magic, const Bytes& body) {
        Bytes s(magic, magic + 4);
        BePush32(s, 0);
        s.insert(s.end(), body.begin(), body.end());
        while (s.size() % 32) s.push_back(0);
        BeWrite32(s.data() + 4, uint32_t(s.size()));
        return s;
    };
    Bytes pool = {0, 0};
    std::vector<uint32_t> offsets;
    for (const char* text : {"Mario", "Luigi", "Toad"}) {
        offsets.push_back(uint32_t(pool.size()));
        for (const char* c = text; *c; ++c) BePush16(pool, uint16_t(*c));
        BePush16(pool, 0);
    }
    Bytes inf;
    BePush16(inf, 3);
    BePush16(inf, 8);
    BePush32(inf, 0);
    for (uint32_t off : offsets) {
        BePush32(inf, off);
        BePush32(inf, 0x01000000);
    }
    Bytes mid;
    BePush16(mid, 3);
    mid.push_back(0x10);
    mid.push_back(0);
    BePush32(mid, 0);
    for (uint32_t id : {9000u, 9007u, 9008u}) BePush32(mid, id);

    Bytes bmg = {'M', 'E', 'S', 'G', 'b', 'm', 'g', '1'};
    bmg.resize(0x20, 0);
    BeWrite32(bmg.data() + 0x0C, 3);
    bmg[0x10] = 2;  // UTF-16
    for (const Bytes& s : {section("INF1", inf), section("DAT1", pool), section("MID1", mid)}) {
        bmg.insert(bmg.end(), s.begin(), s.end());
    }
    BeWrite32(bmg.data() + 8, uint32_t(bmg.size()));
    return bmg;
}

static void TestBmg() {
    Bytes bmg = MakeBmg();
    std::u16string text;
    CHECK(BmgGetMessage(bmg, 9007, text) && text == u"Luigi");
    CHECK(BmgReplaceMessage(bmg, 9007, u"Sonic"));
    CHECK(BmgGetMessage(bmg, 9007, text) && text == u"Sonic");
    CHECK(BmgGetMessage(bmg, 9000, text) && text == u"Mario");
    CHECK(BmgGetMessage(bmg, 9008, text) && text == u"Toad");
    CHECK(!BmgReplaceMessage(bmg, 1234, u"x"));
    CHECK(BeRead32(bmg.data() + 8) == bmg.size());
    CHECK(bmg.size() % 32 == 0);
}

static void TestU8AndTpl() {
    U8Archive archive;
    // root, ".", "./message", "./message/Common.bmg", "./timg", "./timg/tt_luigi_64x64.tpl"
    // Assemble the archive by hand, laid out the way Nintendo's packer does it.
    Bytes raw(0x20, 0);
    struct N { bool dir; const char* name; uint32_t a, b; Bytes data; };
    std::vector<N> nodes = {
        {true, "", 0, 6, {}}, {true, ".", 0, 6, {}}, {true, "message", 1, 4, {}},
        {false, "Common.bmg", 0, 0, MakeBmg()}, {true, "timg", 1, 6, {}},
        {false, "tt_luigi_64x64.tpl", 0, 0, Bytes(64, 0xAB)},
    };
    Bytes strings;
    std::vector<uint32_t> nameOffsets;
    for (auto& n : nodes) {
        nameOffsets.push_back(uint32_t(strings.size()));
        strings.insert(strings.end(), n.name, n.name + std::strlen(n.name) + 1);
    }
    const uint32_t dataStart = uint32_t((0x20 + nodes.size() * 12 + strings.size() + 31) & ~size_t(31));
    raw.resize(dataStart, 0);
    BeWrite32(raw.data(), 0x55AA382Du);
    BeWrite32(raw.data() + 4, 0x20);
    BeWrite32(raw.data() + 8, dataStart - 0x20);
    BeWrite32(raw.data() + 12, dataStart);
    std::memcpy(raw.data() + 0x20 + nodes.size() * 12, strings.data(), strings.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        uint8_t* p = raw.data() + 0x20 + i * 12;
        BeWrite32(p, (nodes[i].dir ? 0x01000000u : 0u) | nameOffsets[i]);
        if (nodes[i].dir) {
            BeWrite32(p + 4, nodes[i].a);
            BeWrite32(p + 8, nodes[i].b);
        } else {
            while (raw.size() % 32) raw.push_back(0);
            p = raw.data() + 0x20 + i * 12;
            BeWrite32(p + 4, uint32_t(raw.size()));
            BeWrite32(p + 8, uint32_t(nodes[i].data.size()));
            raw.insert(raw.end(), nodes[i].data.begin(), nodes[i].data.end());
        }
    }
    while (raw.size() % 32) raw.push_back(0);

    CHECK(archive.Parse(raw.data(), raw.size()));
    CHECK(archive.Build() == raw);
    CHECK(archive.PathOf(3) == "./message/Common.bmg");
    CHECK(archive.PathOf(5) == "./timg/tt_luigi_64x64.tpl");
    CHECK(archive.Files().size() == 2);

    std::vector<uint8_t> pixels(64 * 64 * 4);
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = uint8_t(i * 31);
    archive.At(5).data = TplMakeRgba8(pixels.data(), 64, 64);
    const Bytes rebuilt = archive.Build();
    U8Archive again;
    CHECK(again.Parse(rebuilt.data(), rebuilt.size()));
    TplInfo info;
    CHECK(TplReadInfo(again.At(5).data.data(), again.At(5).data.size(), info));
    CHECK(info.width == 64 && info.height == 64 && info.format == 6);
    // First 4x4 tile: AR pairs then GB pairs; pixel (1,0) is at index 1.
    const uint8_t* tile = again.At(5).data.data() + 0x40;
    CHECK(tile[2] == pixels[4 + 3] && tile[3] == pixels[4 + 0]);
    CHECK(tile[32 + 2] == pixels[4 + 1] && tile[32 + 3] == pixels[4 + 2]);
}

static void TestTokens() {
    const auto t = PathTokens("Race/Kart/sdf_kart-lg_2.szs");
    CHECK(t.size() == 7 && t[4] == "lg" && t[5] == "2");
    CHECK(ToLowerAscii("AbC") == "abc");
}

int main() {
    TestYaz0();
    TestBmg();
    TestU8AndTpl();
    TestTokens();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("sonic format tests passed\n");
    return 0;
}
