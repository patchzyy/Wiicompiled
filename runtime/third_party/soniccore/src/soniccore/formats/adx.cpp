#include "adx.h"
#include <cmath>

namespace sonic {

bool decodeAdx(const u8* d, size_t len, PcmSound& out) {
    if (len < 0x20 || d[0] != 0x80 || d[1] != 0x00) return false;
    u16 copyOff = be16(d + 2);
    int encoding = d[4];
    int blockSize = d[5];
    int bits = d[6];
    int channels = d[7];
    u32 rate = be32(d + 8);
    u32 total = be32(d + 12);
    u16 highpass = be16(d + 16);
    int version = d[18];
    if (encoding != 3 || bits != 4 || channels < 1 || channels > 8 || blockSize < 3) return false;
    size_t dataStart = size_t(copyOff) + 4;
    if (dataStart > len) return false;

    out.sampleRate = int(rate);
    out.channels = channels;
    out.loop = false;
    if (version == 3 && copyOff >= 0x2C - 4) {
        if (be32(d + 0x18)) {
            out.loop = true;
            out.loopStart = be32(d + 0x1C);
            out.loopEnd = be32(d + 0x24);
        }
    } else if (version == 4 && copyOff >= 0x38 - 4) {
        if (be32(d + 0x24)) {
            out.loop = true;
            out.loopStart = be32(d + 0x28);
            out.loopEnd = be32(d + 0x30);
        }
    }

    double a = std::sqrt(2.0) - std::cos(2.0 * 3.14159265358979323846 * double(highpass) / double(rate));
    double b = std::sqrt(2.0) - 1.0;
    double c = (a - std::sqrt((a + b) * (a - b))) / b;
    int coef1 = int(std::floor(c * 8192.0));
    int coef2 = int(std::floor(c * c * -4096.0));

    int samplesPerBlock = (blockSize - 2) * 2;
    out.samples.assign(size_t(total) * channels, 0);
    std::vector<int> h1(channels, 0), h2(channels, 0);
    size_t pos = dataStart;
    size_t produced = 0;
    while (produced < total) {
        if (pos + size_t(blockSize) * channels > len) break;
        for (int ch = 0; ch < channels; ch++) {
            const u8* blk = d + pos + size_t(ch) * blockSize;
            int scale = int(be16(blk)) + 1;
            for (int s = 0; s < samplesPerBlock && produced + s < total; s++) {
                u8 byte = blk[2 + s / 2];
                int nib = (s & 1) ? (byte & 0xF) : (byte >> 4);
                if (nib & 8) nib -= 16;
                int pred = (coef1 * h1[ch] + coef2 * h2[ch]) >> 12;
                int v = nib * scale + pred;
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                h2[ch] = h1[ch];
                h1[ch] = v;
                out.samples[(produced + s) * channels + ch] = s16(v);
            }
        }
        // block with scale 0x8001 marks end-of-stream in some files
        produced += samplesPerBlock;
        pos += size_t(blockSize) * channels;
    }
    if (produced < total) out.samples.resize(produced * channels);
    if (out.loopEnd == 0 || out.loopEnd > out.frames()) out.loopEnd = out.frames();
    if (out.loopStart >= out.loopEnd) out.loop = false;
    return true;
}

bool parseSoundBank(const std::vector<u8>& data, std::vector<SoundBankEntry>& out) {
    out.clear();
    if (data.size() < 0x14 || memcmp(data.data(), "archive", 7) != 0) return false;
    u32 n = rd<u32>(&data[0x10]);
    for (u32 i = 0; i < n; i++) {
        size_t e = 0x14 + size_t(i) * 12;
        if (e + 12 > data.size()) return false;
        u32 no = rd<u32>(&data[e]), off = rd<u32>(&data[e + 4]), sz = rd<u32>(&data[e + 8]);
        SoundBankEntry se;
        for (size_t k = no; k < data.size() && data[k]; k++) se.name.push_back(char(data[k]));
        se.offset = off;
        se.size = sz;
        if (size_t(off) + sz <= data.size()) out.push_back(se);
    }
    return true;
}

}  // namespace sonic
